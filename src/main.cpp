#ifdef CONFIG_BTDM_CONTROLLER_BLE_MAX_CONN
#undef CONFIG_BTDM_CONTROLLER_BLE_MAX_CONN
#endif
#define CONFIG_BTDM_CONTROLLER_BLE_MAX_CONN 9

#include <Arduino.h>
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <esp_task_wdt.h>
#include <Preferences.h> // use the Preferences.h library to store data

#define WDT_TIMEOUT 3

//  ---- FIRMWARE UPDATES ====  //
//  1.0.0 - Initial Design
//  1.0.1 - Add functionality with Terry's PIC board && Change BT name to Beefbooks
//  1.0.2 - On BLE Disconnect reset esp so that it will reestablish connection
//  1.0.3 - Add ESP reset if fail to establish serial connection, reset only once on BT disconnect
//  1.0.4 - Add delay before establishing seral connection in setup, Enable 3 second watchdog timeout.
//  1.1.0 - Add ability to change blutooth device name from BT service.
//  1.2.0 - Major Refactor of code and fix bug with device renaming crashing on long names.

struct Version
{
  const int major = 1;
  const int minor = 2;
  const int patch = 0;
  const bool beta = true;

  String toString() const
  {
    return String(major) + "." + String(minor) + "." + String(patch) + (beta ? "-beta" : "");
  }
};

Version VERSION;

Preferences preferences; // initiate an instance of the Preferences library

// RS232 Config
const int RXD2 = 21;
const int TXD2 = 19;
constexpr int RX_BUFFER_SIZE = 25;
char rx2_buffer[RX_BUFFER_SIZE];
static int rx2_pointer;
bool process_buffer_flag = false;
bool lock_flag = false;

// LED Config
const int lockLedRed = 13;
const int lockLedGreen = 14;
const int lockLedBlue = 12;

// Weight Data
constexpr int WEIGHT_STR_SIZE = 19;
char weightStr[WEIGHT_STR_SIZE];
char savedWeight[WEIGHT_STR_SIZE];

// BLE Config
BLECharacteristic *characteristicTX = nullptr;
BLECharacteristic *pCharacteristic = nullptr;
bool deviceConnected = false;
bool wasConnectedFlag = false;

constexpr char DEVICE_LOCAL_NAME[] = "Agri-Tronix"; // MAX 20 Char
constexpr char SERVICE_UUID[] = "569a1101-b87f-490c-92cb-11ba5ea5167c";
constexpr char CHARACTERISTIC_UUID_RX[] = "569a2001-b87f-490c-92cb-11ba5ea5167c";
constexpr char CHARACTERISTIC_UUID_TX[] = "569a2000-b87f-490c-92cb-11ba5ea5167c";
constexpr char CHARACTERISTIC_UUID_RX2[] = "569a2003-b87f-490c-92cb-11ba5ea5167c";
constexpr char CHARACTERISTIC_UUID_TX2[] = "569a2002-b87f-490c-92cb-11ba5ea5167c";
constexpr char DEVICE_NAME[] = "00002a00-0000-1000-8000-00805f9b34fb";

/**
 * @brief Callback class to direct buttons pressed from app
 */
class CharacteristicCallbacks : public BLECharacteristicCallbacks
{
  void onWrite(BLECharacteristic *characteristic)
  {
    std::string rxValue = characteristic->getValue();
    if (rxValue.empty())
      return;

    // Convert to lower case for case-insensitive comparison
    std::transform(rxValue.begin(), rxValue.end(), rxValue.begin(), ::tolower);

    if (rxValue.find('z') != std::string::npos)
    {
      Serial.println("Zero Pressed");
      Serial2.write("\x05z\x03");
    }
    else if (rxValue.find('t') != std::string::npos)
    {
      Serial.println("Tare Pressed");
      Serial2.write("\x05t\x03");
    }
    else if (rxValue.find('c') != std::string::npos)
    {
      Serial.println("Units Pressed");
      Serial2.write("\x05c\x03");
    }
    else if (rxValue.find('n') != std::string::npos)
    {
      Serial.println("Net Pressed");
      Serial2.write("\x05n\x03");
    }
    else if (rxValue.find('g') != std::string::npos)
    {
      Serial.println("Gross Pressed");
      Serial2.write("\x05g\x03");
    }
  }
};
/**
 * @brief Callback class to change device name
 */
class CharacteristicCallbacksNameChange : public BLECharacteristicCallbacks
{
  void onWrite(BLECharacteristic *characteristic)
  {
    std::string rxValueName = characteristic->getValue();

    if (rxValueName.empty())
      return;

    if (rxValueName.length() > 20)
    {
      Serial.println("Name too long");
      // Optionally: Write a status back to the BLE characteristic
      return;
    }

    // Check for any special characters here if needed

    Serial.print("Received New Name: ");
    Serial.println(rxValueName.c_str());

    preferences.begin("my-app", false);
    preferences.putString("device_name", rxValueName.c_str());
    preferences.end();

    Serial.println("Restarting ESP");
    ESP.restart();
  }
};

/**
 * @brief Clears scale buffer
 *
 */
void clear_buffer()
{
  int x = 20;
  while (x != 0)
  {
    rx2_buffer[x] = 0x00;
    x = x - 1;
  }
}
/**
 * @brief Initialize LED pins. Call this function in setup().
 */
void initLEDs()
{
  pinMode(lockLedRed, OUTPUT);
  pinMode(lockLedGreen, OUTPUT);
  pinMode(lockLedBlue, OUTPUT);
}

/**
 * @brief Turn LED ON
 */
void ledOn(int ledNum)
{
  digitalWrite(ledNum, LOW);
}

/**
 * @brief Turn LED OFF
 */
void ledOff(int ledNum)
{
  digitalWrite(ledNum, HIGH);
}

/**
 * @brief Set the status of RGB LEDs
 */
void ledRGBStatus(bool red, bool green, bool blue)
{
  red ? ledOn(lockLedRed) : ledOff(lockLedRed);
  green ? ledOn(lockLedGreen) : ledOff(lockLedGreen);
  blue ? ledOn(lockLedBlue) : ledOff(lockLedBlue);
}
/**
 * @brief Callback class that determines if Bluetooth is connected.
 *
 */
class ServerCallbacks : public BLEServerCallbacks
{
  void onConnect(BLEServer *server)
  {
    deviceConnected = true;
    Serial.println("Device connected");
    ledOn(lockLedBlue);
  };

  void onDisconnect(BLEServer *server)
  {
    deviceConnected = false;
    Serial.println("Device disconnected");
    ledOff(lockLedBlue);
  }
};

void dotDotDotDelay(int seconds)
{
  for (int i = 0; i < seconds; i++)
  {
    Serial.print(".");
    delay(1);
  }
  Serial.println("");
}

// Function to truncate the device name if it exceeds the maximum length
String truncateDeviceName(const String &deviceName, const String &version, int maxLength = 30)
{
  // Calculate the length of each part
  int nameLength = deviceName.length();
  int macPartLength = 3; // '[' + last MAC address char + ']'
  int versionLength = String(" v" + version).length();

  // Calculate total length
  int totalLength = nameLength + macPartLength + versionLength;

  // Check if it exceeds the limit
  if (totalLength > maxLength)
  {
    // Truncate deviceName or handle it some other way
    int excess = totalLength - maxLength;
    return deviceName.substring(0, deviceName.length() - excess);
  }
  return deviceName;
}

void initBluetooth(String deviceName)
{
  Serial.print("Starting BLE device");
  dotDotDotDelay(5);
  // Truncate if necessary
  deviceName = truncateDeviceName(deviceName, VERSION.toString());
  // Build the comboName
  String comboName = deviceName + '[' + WiFi.macAddress()[13] + "] v" + VERSION.toString();
  Serial.println(comboName);
  dotDotDotDelay(5);
  Serial.println(DEVICE_LOCAL_NAME);
  // Initialize Bluetooth
  //  BLEDevice::init(DEVICE_LOCAL_NAME);
  BLEDevice::init(comboName.c_str());
  // Set Bluetooth power transmistion to 9 (Max Power)
  esp_err_t errRc = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN, ESP_PWR_LVL_P9);
  Serial.println(errRc);

  // Create a bluetooth server
  Serial.print("Creating BLE server");
  dotDotDotDelay(5);
  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks()); // set a callback to server

  BLEService *pService = pServer->createService(SERVICE_UUID);

  // Create a BLE Characteristic for transmitting data
  characteristicTX = pService->createCharacteristic(
      CHARACTERISTIC_UUID_TX,
      BLECharacteristic::PROPERTY_NOTIFY);
  characteristicTX->addDescriptor(new BLE2902());

  // initialize second service,  I'm not sure what this is for but the original Big Gain unit had this UUID that seeming did nothing
  pCharacteristic = pService->createCharacteristic(
      CHARACTERISTIC_UUID_TX2,
      BLECharacteristic::PROPERTY_NOTIFY);
  pCharacteristic->addDescriptor(new BLE2902());

  // In this stage of the Setup, we create the characteristics (this time) for the reception of data.
  // We set the callback to receive the information and initialize the service.
  // We propagate the signal from the ESP32, and initialize the scale,
  // Create a BLE Characteristic for recieving data
  Serial.print("Create a BLE Characteristic for recieving data");
  dotDotDotDelay(5);
  BLECharacteristic *characteristic = pService->createCharacteristic(
      CHARACTERISTIC_UUID_RX,
      BLECharacteristic::PROPERTY_WRITE |
          BLECharacteristic::PROPERTY_WRITE_NR);
  BLECharacteristic *characteristic2 = pService->createCharacteristic(
      CHARACTERISTIC_UUID_RX2,
      BLECharacteristic::PROPERTY_WRITE |
          BLECharacteristic::PROPERTY_WRITE_NR);
  BLECharacteristic *devicename = pService->createCharacteristic(
      DEVICE_NAME,
      BLECharacteristic::PROPERTY_WRITE |
          BLECharacteristic::PROPERTY_WRITE_NR);

  characteristic->setCallbacks(new CharacteristicCallbacks());
  characteristic2->setCallbacks(new CharacteristicCallbacks());
  devicename->setCallbacks(new CharacteristicCallbacksNameChange());

  // Start the service
  Serial.print("Start the service");
  dotDotDotDelay(5);
  pService->start();

  // Start advertising
  Serial.print("Start advertising");
  dotDotDotDelay(5);
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06); // functions that help with iPhone connections issue
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
}

void initScale()
{
  Serial.print("Initializing the scale...");
  dotDotDotDelay(5);
  try
  {
    delay(1000);
    Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);
  }
  catch (...)
  {
    // Handle exceptions that occur during Serial2.begin
    Serial.print("Could not establish serial connection. RESTARTING...");
    ESP.restart(); // Alternatively, ESP.reset();
  }
}

void initPreferences()
{
  // The begin() method opens a “storage space” with a defined namespace.
  // The false argument means that we’ll use it in read/write mode.
  // Use true to open or create the namespace in read-only mode.
  preferences.begin("my-app", false);
  // // Clear all keys in this namespace
  // preferences.clear();
  String deviceName = preferences.getString("device_name", String(DEVICE_LOCAL_NAME));
  preferences.end();
}

void setup()
{
  initPreferences();

  Serial.begin(115200);
  Serial.print("Booting Up");
  Serial.println("Configuring WDT...");
  esp_task_wdt_init(WDT_TIMEOUT, true);
  esp_task_wdt_add(NULL);

  dotDotDotDelay(5);

  Serial.print("Software Version: ");
  Serial.println(VERSION.toString());

  String deviceName = truncateDeviceName(preferences.getString("device_name", String(DEVICE_LOCAL_NAME)), VERSION.toString());

  initBluetooth(deviceName);
  initLEDs();
  initScale();

  Serial.print("End of setup()");
  dotDotDotDelay(5);
}

void loop()
{
  // In the Loop, we find that there is some device connected, and we try to read the sensor.
  // If the action is carried out, we collect scale weight.
  // We convert the value to a char array, set this value, and send it to the smartphone.
  // if (deviceConnected) {

  // Serial.println("Commence Reading Scale");

  static char units[2];
  static char ng[2];

  // check uart for input from scale
  if (Serial2.available())
  {
    // Serial.println("Scale is available");
    rx2_buffer[rx2_pointer] = Serial2.read(); // read character and place in buffer
    // Serial.printf("%hhX ", rx2_buffer[rx2_pointer]);
    switch (rx2_buffer[rx2_pointer])
    {
    case 0x02:              // if beginning of string character reset the pointer
      rx2_buffer[0] = 0x02; // set first character to 0x02
      rx2_pointer = 0;      // reset the pointer
      // ledOn(lockLedRed);
      break;
    case 0x2E:
      break;
    case 'H':        // if there is an 'H' in the string then set flag to turn on lock led
      lock_flag = 1; // set flag so lock light will come on when processing string
      break;
    case 'L':
      units[0] = 'L';
      units[1] = 'B';
      break;
    case 'K':
      units[0] = 'K';
      units[1] = 'G';
      break;
    case 'G':
      ng[0] = 'G';
      ng[1] = 'R';
      break;
    case 'N':
      ng[0] = 'N';
      ng[1] = 'T';
      break;
    case 0x20:
      break;
    case 'M':
      Serial.print("Found an M at position: ");
      Serial.println(rx2_pointer);
      break;
    case 'O':
      Serial.print("Found an O at position: ");
      Serial.println(rx2_pointer);
      break;
    case 0x0D:
      // Serial.println("END OF STRING");
      process_buffer_flag = 1; // set flag so code will process buffer
      // ledOff(lockLedRed);

      break;
    default:
      break;
    }

    if (++rx2_pointer >= 24)
    { // increment pointer and check for overflow
      Serial.println("Buffer Overflow");
      Serial.println(rx2_pointer + " = " + rx2_buffer[rx2_pointer]);
      rx2_pointer = 0; // reset pointer on buffer overflow
    }
  } // END if (Serial2.available())
  // else {
  //   byte weight[] = {0x20, 0x4e, 0x6f, 0x20, 0x53, 0x69, 0x67, 0x20,
  //     0x20,
  //     units[0],
  //     units[1],
  //     0x20,
  //     ng[0],
  //     ng[1],
  //     0x0d,
  //     0x0a,
  //     0x00
  //   };
  //   byte testValue[] = {
  //     0x02,
  //     0x00
  //   };
  // }

  if (process_buffer_flag)
  {
    // Serial.println(weightStr);
    memset(weightStr, 0, sizeof(weightStr));
    strncpy(weightStr, rx2_buffer + 1, 8);
    rx2_buffer[rx2_pointer] = 0x00; // add null zero to string

    // Reset back to initial state
    process_buffer_flag = 0; // reset flag

    clear_buffer(); // clear the rs232 buffer
    rx2_pointer = 0;
    byte weight[] = {
        weightStr[0],
        weightStr[1],
        weightStr[2],
        weightStr[3],
        weightStr[4],
        weightStr[5],
        weightStr[6],
        weightStr[7],
        0x20,
        units[0],
        units[1],
        0x20,
        ng[0],
        ng[1],
        0x0d,
        0x0a,
        0x00};
    byte testValue[] = {
        0x02,
        0x00};
    // if (atoi(weightStr) != atoi(savedWeight)) {
    if (true)
    {
      // Serial.print(weightStr);
      // Serial.print(" != ");
      // Serial.println(savedWeight);

      if (!deviceConnected && wasConnectedFlag)
      {
        Serial.println("BLE Disconnected");
        delay(100);
        Serial.println("Resetting ESP");
        ESP.restart(); // ESP.reset();
      }
      if (deviceConnected)
      {
        wasConnectedFlag = true;
        // Serial.println("BLE Connected");
        characteristicTX->setValue((char *)testValue); // Set a value for notification
        characteristicTX->notify();                    // Notify Smartphone
        characteristicTX->setValue((char *)weight);    // Set a value for notification
        characteristicTX->notify();                    // Notify Smartphone
        // Serial.println();
        // Serial.println((char * ) weight);
        strncpy(savedWeight, weightStr, 19);
      }
    }
  } // if (process_buffer_flag)
  //} //if (deviceConnected)
  delay(5);
  // Serial.println("Resetting WDT...");
  esp_task_wdt_reset();
} // void loop()

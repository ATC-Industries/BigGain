# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.2.0]

### Refactor

- Major refactoring to make code more readable.
- Introduced truncateDeviceName function to handle device name length issues.
- Split Bluetooth initialization into initBluetooth function for better modularity.
- Modularized scale initialization into initScale function.

### Fixed

- Fixed bug where device name would exceed BLE advertisement length limits.
- Implemented reconnection mechanism to handle loss of signal.
- Resolved issue where changing the device name from the client did not update as expected.

### Added

- Added logic to restart BLE advertising after device name change.

## [1.1.0-beta]

### Added

- Add ability to change blutooth device name from BT service.

## [1.0.4]

### Added
 - Add delay before establishing seral connection in setup, Enable 3 second watchdog timeout.

## [1.0.3]

### Added
 - Add ESP reset if fail to establish serial connection, reset only once on BT disconnect

## [1.0.2]

### Added
 - On BLE Disconnect reset esp so that it will reestablish connection

## [1.0.1]

### Added
 - Add functionality with Terry's PIC board && Change BT name to Beefbooks

## [1.0.0]

### Added
 - Initial Design

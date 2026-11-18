# Flight Tracker Display

Firmware for an ESP32-S3-based flight information display using a Waveshare RGB LED Matrix Panel (64×32, HUB75).

The display retrieves flight information from the Flight Tracker API and displays it on the LED matrix.

## Hardware

The project uses the following hardware:

- **LED Matrix:** Waveshare RGB LED Matrix Panel, 64×32 pixels, HUB75 interface
- **Microcontroller:** diymore ESP32-S3 DevKitC-1 N16R8


The ESP32-S3 connects to the LED matrix via the HUB75 interface and retrieves flight information over Wi-Fi.

## Project Overview

This repository contains the firmware for the LED display.

The actual flight data is provided by a separate project:

**Flight Tracker (Go backend)**

The Flight Tracker runs on a server, for example a Raspberry Pi, and provides an HTTP API with the current flight information.

The ESP32-S3 periodically requests this information and renders it on the LED matrix.

Architecture:

```text
┌──────────────────────────┐
│      Flight Tracker      │
│       Go Backend         │
│                          │
│       HTTP API           │
└────────────┬─────────────┘
             │
             │ HTTP / JSON
             ▼
┌──────────────────────────┐
│        ESP32-S3          │
│                          │
│    Wi-Fi HTTP Client     │
│    Flight Data Parser    │
│    Display Controller   │
└────────────┬─────────────┘
             │
             │ HUB75
             ▼
┌──────────────────────────┐
│   Waveshare RGB Matrix   │
│        64 × 32           │
│                          │
│    Flight Information    │
└──────────────────────────┘
```

The Flight Tracker repository can be used as the data source. Its API URL must be configured in this project.

## Development Environment

The firmware is developed using **ESP-IDF**.

### 1. Install ESP-IDF

Follow the official ESP-IDF installation instructions for macOS:

https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/

Clone ESP-IDF if it is not already installed:

```bash
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32s3
```

### 2. Activate the Python Virtual Environment

ESP-IDF uses a Python virtual environment for its build tools and dependencies.

Activate the environment by running the ESP-IDF export script:

```bash
. ~/esp/esp-idf/export.sh
```

Adjust the path if ESP-IDF is installed somewhere else.

This activates the ESP-IDF environment and makes the required tools available in the current terminal session.

**Important:** Run this command again whenever you open a new terminal session.

### 3. Build the Project

Navigate to the project directory:

```bash
cd flight-tracker-display
```

Set the target to ESP32-S3:

```bash
idf.py set-target esp32s3
```

Configure the project:

```bash
idf.py menuconfig
```

In the configuration menu, make sure to set the **font size to 12**.

This setting is important for the correct layout and rendering of the flight information on the 64×32 LED matrix.

### 4. Configure the API

The display needs the URL of the Flight Tracker API.

Configure the API URL in the project configuration (`menuconfig` or the corresponding project configuration setting).

Example:

```text
http://192.168.1.100:8080/api/flight
```

Replace the example address with the address of your Flight Tracker server.

Make sure that:

- The ESP32-S3 is connected to the same network as the server.
- The API server is reachable from the ESP32.
- The configured URL points to the correct API endpoint.

The Flight Tracker backend is maintained in a separate repository and can be used as the data source for this display.

### 5. Build and Flash

Connect the ESP32-S3 via USB.

Build the firmware:

```bash
idf.py build
```

Flash the firmware to the ESP32-S3:

```bash
idf.py flash
```

To build, flash and start the serial monitor in one step:

```bash
idf.py build flash monitor
```

To exit the serial monitor, press `Ctrl + ]`.

## Updating the ESP32

Whenever the firmware is changed, rebuild and flash the device:

```bash
. ~/esp/esp-idf/export.sh
cd flight-tracker-display
idf.py build flash monitor
```

The ESP32-S3 will restart with the newly flashed firmware.

If the project configuration has changed, open `menuconfig` again and verify the settings before flashing:

```bash
idf.py menuconfig
```

## Useful Commands

| Command | Description |
|---|---|
| `idf.py build` | Build the firmware |
| `idf.py flash` | Flash firmware to the ESP32 |
| `idf.py monitor` | Open the serial monitor |
| `idf.py build flash monitor` | Build, flash and monitor |
| `idf.py menuconfig` | Open project configuration |
| `idf.py fullclean` | Remove previous build files |


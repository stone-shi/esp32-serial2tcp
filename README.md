# ESP32-S3 Serial to TCP Bridge

## Overview

This project implements a transparent Serial-to-TCP bridge using the **ESP32-S3** microcontroller. It enables bidirectional communication between a hardware Serial (UART) interface and a TCP socket over WiFi. This is useful for exposing legacy serial devices, sensors, or debug ports to a network.

## Features

*   **ESP32-S3 Optimized**: Leverages the S3's dual-core architecture and WiFi capabilities.
*   **Transparent Bridging**: Raw data is passed between the TCP client and the UART interface without modification.
*   **Bidirectional**: Full-duplex communication.
*   **WiFi Modes**: Supports Station (STA) mode to connect to an existing router or Access Point (AP) mode to act as a standalone network.
*   **Single-Client**: Currently configured for one active TCP connection at a time (rejects concurrent attempts).
*   **Configurable**: Baud rate, TCP port, and WiFi credentials can be easily customized.
*   **Status Monitor**: Dedicated TCP port (9090) provides system stats (RSSI, IP, Bytes transferred).

## Hardware Requirements

*   **ESP32-S3 Development Board** (e.g., ESP32-S3-DevKitC-1).
*   Target Serial Device (e.g., GPS module, Industrial Controller, 3D Printer).
*   Jumper wires.

## Pin Configuration

By default, the project uses the following pins for the secondary UART connection. These can be changed in the configuration.

| ESP32-S3 Pin | UART Function | Connection |
| :--- | :--- | :--- |
| **GPIO 17** (Default TX) | TX | Connect to Target Device **RX** |
| **GPIO 18** (Default RX) | RX | Connect to Target Device **TX** |
| **GPIO 48** (Onboard LED) | LED | Status Indicator (Addressable RGB) |
| **GND** | Ground | Connect to Target Device **GND** |

> **Note**: Ensure the voltage levels are compatible (ESP32-S3 is 3.3V logic). If your target device is 5V, use a logic level shifter.

## Getting Started

### Prerequisites

*   [PlatformIO](https://platformio.org/) (Recommended) or Arduino IDE.
*   Drivers for the ESP32-S3 USB/UART bridge (if not already installed).

### Configuration

Before flashing, configure your network and serial settings.

1.  Open the source code (`main/serial_tcp.c`).
2.  Update the following constants:
    *   `EXAMPLE_ESP_WIFI_SSID`: Your WiFi Network Name (or set via Kconfig).
    *   `EXAMPLE_ESP_WIFI_PASS`: Your WiFi Password (or set via Kconfig).
    *   `TCP_PORT`: The port to listen on (Default: `8989`).
    *   `UART_BAUD`: The baud rate for the serial connection (e.g., `115200`, `9600`).

### Building and Flashing (PlatformIO)

1.  Clone this repository.
2.  Open the project folder in VS Code.
3.  Connect your ESP32-S3 board via USB.
4.  Click the **PlatformIO: Upload** button (right arrow icon) in the bottom status bar.
5.  Monitor the output to ensure the upload is successful.

## Usage

1.  **Power On**: Reset or power cycle the ESP32-S3.
2.  **Check Connection**: Open the Serial Monitor (USB) to see the debug logs. The ESP32 will print its assigned IP address once connected to WiFi.
    ```text
    Connecting to WiFi...
    Connected! IP Address: 192.168.1.105
    Server listening on port 8989
    ```
3.  **Connect via TCP**:
    *   Use a tool like `netcat`, `telnet`, or PuTTY.
    *   **Command**: `nc 192.168.1.105 8989`
4.  **Transfer Data**:
    *   Type into your TCP client; characters will appear on the ESP32-S3 UART TX pin.
    *   Send data to the ESP32-S3 UART RX pin; characters will appear in your TCP client.
5.  **Check Status**:
    *   Connect to the status port to view signal strength and transfer stats.
    *   **Command**: `nc 192.168.1.105 9090`
    *   **Output**:
        ```text
        --- Status Monitor ---
        WiFi Signal: -55 dBm
        IP Address: 192.168.1.105
        Client Connected: Yes
        Total Bytes Bridged: 1024
        Bytes (Last 1 min): 120
        ----------------------
        ```

## Troubleshooting

*   **No Data Received**: Check your wiring (TX goes to RX, RX goes to TX). Verify the baud rate matches the target device.
*   **Cannot Connect to WiFi**: Ensure 2.4GHz WiFi is available (ESP32-S3 supports 2.4GHz). Check SSID/Password.
*   **Connection Refused**: Ensure the ESP32 has obtained an IP and the firewall on your computer isn't blocking the connection.

## License

This project is licensed under the MIT License - see the LICENSE file for details.
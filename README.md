# ESP32-S3 HMI remote with ESP-NOW

This project pairs an ESP32-S3 transmitter with an ESP32 receiver. The transmitter targets the **Elecrow CrowPanel 7.0-inch HMI (DIS08070H / ESP32-S3-WROOM-1-N4R8)**, with an 800×480 LCD driven by EK9716BD3/EK73002ACGB and capacitive touch. It uses LVGL 9.6 for a live dashboard and a 4×4 on-screen track pad with four banks (tracks 1–16, 17–32, 33–48, and 49–64). Tapping a track sends its single numeric track number to the receiver, which plays that MP3 Trigger track. Four analog controls come from an ADS1115 and six physical buttons use a PCF8574. The receiver sends control positions, motor commands, and battery telemetry back over ESP-NOW.

The board assumption is based on Elecrow's DIS08070H 7-inch HMI. Its LCD uses a 16-bit parallel RGB interface, not SPI; the board's SPI interface is for its TF-card slot. The display-panel library initializes the RGB panel, backlight, touch controller, and the board's touch-reset expander profile. Other ESP32-S3 display boards may use different RGB timings, touch controllers, and pin mappings; select their matching `ESP32_Display_Panel` profile before use.

## Hardware

- Transmitter: Elecrow CrowPanel 7.0-inch HMI DIS08070H, ESP32-S3-WROOM-1-N4R8 (4 MB flash, 8 MB OPI PSRAM)
- Receiver: ESP32 development board
- Six momentary buttons and a PCF8574 at 0x21
- Four potentiometers/analog controls and ADS1115 at 0x48
- SparkFun original serial MP3 Trigger with a FAT16/FAT32 card and numbered files
- Dimension Engineering Sabertooth dual motor driver and SyRen motor driver
- Battery divider parts: 100 kΩ + 100 kΩ for transmitter sense; 100 kΩ + 15 kΩ for receiver sense

## Transmitter wiring

Use the CrowPanel's external I²C connector (SDA GPIO19, SCL GPIO20) for the ADS1115 and button PCF8574. The display profile initializes the touch controller and I²C master bus; the transmitter adds its sensor devices to that same `driver_ng` bus. It does not initialize a second host or use Arduino `Wire`. The LVGL pointer driver reads touch coordinates and activates the on-screen buttons. The 4×4 track pad shows buttons 1–16 in bank 1, 17–32 in bank 2, 33–48 in bank 3, and 49–64 in bank 4. **BACK** and **NEXT** beside the grid change banks, wrapping between bank 1 and bank 4. Those bank controls stay local to the transmitter. Tapping a numbered button sends one `EVENT_TRACK` packet whose value is the selected track number; it does not send key presses, ASCII digits, or a separate play command.

### Analog controls via ADS1115

Connect the four control wipers to ADS1115 A0–A3, with control ends at 3.3 V and GND. Keep analog inputs within the ADS1115 supply range (3.3 V). ADS1115 must be powered at 3.3 V and use address `0x48` (ADDR to GND). Readings are converted to 0–4095 and sent as analog channels 1–4. The receiver maps channels 1–2 to Sabertooth throttle/steering, channel 3 to SyRen, and channel 4 to telemetry.

### Six buttons via PCF8574 (0x21)

Connect one side of each momentary switch to P0–P5 and the other side to GND. P6–P7 remain unused. The firmware drives the expander pins high and scans active-low button presses. Button events are sent to the receiver; current receiver firmware reports them but does not assign actions.

### Transmitter battery sense

Connect the battery measurement divider midpoint to **GPIO10 (ADC1 channel 9)**: battery positive → 100 kΩ → GPIO10 → 100 kΩ → GND. This reports the sensed source as transmitter battery voltage. Keep GPIO10 at or below 3.3 V and share ground. With the 2:1 divider, do not measure above about 6.6 V. Do not connect battery positive directly to an ESP32 pin. The selected CrowPanel display profile uses GPIO9 for RGB data and leaves GPIO10 unused.

## Receiver wiring and operation

The receiver uses UART2 GPIO17 TX/GPIO16 RX for the SparkFun Trigger at 38,400 baud. Connect ESP32 TX to Trigger RX, RX to TX, and GND to GND. The Trigger sends audio to its own speaker/amplifier output.

The Sabertooth uses Simplified Serial at 9600 baud from receiver GPIO4 to S1, with common ground. Analog inputs 1 and 2 become mixed throttle and steering. The SyRen uses Simplified Serial at 9600 baud from receiver GPIO22 to S1, with common ground; analog input 3 commands reverse/stop/forward. Configure each driver's serial mode according to its model manual. If the remote stops transmitting, the receiver stops both drivers after 750 ms. Test with wheels lifted before running under load.

Receiver battery sensing uses a divider from battery positive through 100 kΩ to receiver GPIO32 and 15 kΩ from GPIO32 to ground (maximum about 25 V). It reports only the battery connected to this divider. Join signal grounds. Never connect battery positive directly to an ESP32 GPIO.

The screen reports ESP-NOW link age, last track number sent, current track bank, all four local analog controls, receiver control positions, Sabertooth/SyRen commanded power, and transmitter/receiver battery voltage. Reported “positions” are ADC control positions, not motor shaft positions; encoders are needed for actual shaft feedback. The six separate physical buttons remain available through the PCF8574.

## ESP-IDF transmitter build

The transmitter is a native ESP-IDF application in `main/`; it does not use Arduino-ESP32. Install ESP-IDF 6.1 and the ESP-IDF VS Code extension, then open this repository as the ESP-IDF project. The Component Manager manifest pins ESP32_Display_Panel 1.0.4 and LVGL 9.6.0, including the display panel's managed dependencies. The local component copies in `components/` provide ESP-IDF 6.1 compatibility for the pinned display-panel 1.0.4 and I/O expander 1.1.1: they migrate the I²C hosts and transfers to `driver_ng`, update RGB LCD and LEDC integration, add explicit GPIO enum conversions, and preserve the upstream licenses. The supported CrowPanel profile and LVGL 9 configuration are in `main/`.

The project targets ESP32-S3 with 4 MB flash and OPI PSRAM. `sdkconfig.defaults` selects the target and `partitions.csv` reserves a 3 MB factory app partition. To configure and build from an ESP-IDF terminal:

```powershell
idf.py set-target esp32s3
idf.py build
```

Use `idf.py menuconfig` to adjust project settings. The ADS1115, PCF8574, touch controller, and board expander share the ESP-IDF 6 I²C master bus initialized by the CrowPanel profile. The original Arduino sketches remain in `ESP-TX/remote` and `ESP-RX/receiver` as separate firmware sources.

The receiver STA MAC still needs to be copied into `receiverMac` in `main/main.cpp`. GPIO10 is used for transmitter battery ADC because GPIO38 is not ADC-capable on the ESP32-S3 and GPIO9 is used by the display.

## Arduino builds

The original transmitter and receiver sketches remain buildable with the VS Code **ESP32: Build transmitter** and **ESP32: Build receiver** tasks. Their Arduino-ESP32 3.1.1 setup is separate from the native ESP-IDF transmitter project.

## Protocol and notes

- ESP-NOW packets are unencrypted and target the receiver's MAC address. Receiver telemetry is broadcast every 500 ms.
- Track button packets carry the selected track number in `InputPacket.value` (valid range 1–64); the receiver immediately sends the SparkFun Trigger binary `t` command and the track byte.
- Change `BUTTON_PCF8574_ADDRESS` or `ADS1115_ADDRESS` in `remote.ino` if your address straps differ.
- The receiver firmware targets the original serial SparkFun MP3 Trigger, not the Qwiic MP3 Trigger.

## References

- [Elecrow CrowPanel 7.0-inch HMI specs and pinout](https://www.elecrow.com/wiki/esp32-display-702727-intelligent-touch-screen-wi-fi26ble-800480-hmi-display.html)
- [Elecrow note on the Arduino ESP32 3.2.0 I²C driver conflict](https://forum.elecrow.com/discussion/1190/beware-of-the-3-2-0-esp-board-package)
- [Espressif ESP32_Display_Panel supported boards and CrowPanel 7.0 profile](https://github.com/esp-arduino-libs/ESP32_Display_Panel)
- [Espressif ESP32_Display_Panel library and Arduino setup](https://github.com/esp-arduino-libs/ESP32_Display_Panel)
- [LVGL documentation (current 9.6 docs)](https://docs.lvgl.io/master/)
- [LVGL 9.6 changelog](https://github.com/lvgl/lvgl/blob/master/docs/src/changelog/CHANGELOG.mdx)
- [TI PCF8574 datasheet](https://www.ti.com/lit/ds/symlink/pcf8574.pdf)
- [SparkFun MP3 Trigger serial protocol](https://learn.sparkfun.com/tutorials/mp3-trigger-hookup-guide-v24/serial-control-protocol-)
- [Dimension Engineering Sabertooth setup](https://www.dimensionengineering.com/software/SabertoothSimplifiedArduinoLibrary/html/)
- [Dimension Engineering SyRen setup](https://www.dimensionengineering.com/software/SyRenSimplifiedArduinoLibrary/html/)
- [Espressif ESP-NOW documentation](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/espnow.html)

# P4dcsi

First end-to-end hardware test for:

- Wireless-Tag **WT9932P4-TINY** (ESP32-P4 v1.x)
- **OV9281** MIPI-CSI monochrome camera
- **iPistBit 4.3" 800x480** Raspberry-Pi-style MIPI-DSI display

The firmware uses a single shared I2C bus on GPIO7/8, matching the WT9932P4-TINY CSI/DSI wiring. GPIO0 is driven high to enable the camera rail.

## Current test path

```
OV9281 640x400 RAW8 @ 100 fps
        |
        v
ESP32-P4 MIPI CSI / esp_video
        |
        v
display every fourth frame (~25 fps target)
RAW8 -> grayscale RGB565
        |
        v
LVGL 640x400 canvas centered on 800x480
        |
        v
ESP32-P4 MIPI DSI
        |
        v
Pi-style ATTINY88 (0x45) + TC358762 bridge
```

The display driver is based on the working ESP32-P4 Raspberry-Pi-display implementation at
`oguzhanbaser/esp32-p4-waveshare-5inch-dsi-lcd`. It uses one DSI data lane at 600 Mbps and the established TC358762 Generic Long Write initialization sequence.

This first build deliberately does not initialize touch, USB, recording, SD, or image processing beyond RAW8-to-grayscale conversion.

## Expected I2C diagnostics

The firmware probes:

- `0x45` - Pi-style panel control MCU
- `0x38` - common FT5x06 touch address (diagnostic only)
- `0x60` - OV9281 with SID low
- `0x30` - alternate camera address diagnostic

## Build

ESP-IDF 6.0.2 is used by CI.

```sh
idf.py set-target esp32p4
idf.py build
```

Both CSI and DSI FFCs on this WT9932P4-TINY setup use same-side-contact ribbons.

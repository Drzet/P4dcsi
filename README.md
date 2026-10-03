# P4dcsi

End-to-end hardware test for:

- Wireless-Tag **WT9932P4-TINY** (ESP32-P4 v1.x)
- **OV9281** MIPI-CSI monochrome camera
- **iPistBit 4.3" 800x480** Raspberry-Pi-style MIPI-DSI display

The firmware uses the WT9932P4-TINY shared I2C wiring on GPIO7/8. The CSI side follows Wireless-Tag's WT9932P4-TINY BSP implementation; camera reset and PWDN are both unused (`-1`).

## Test path

```
OV9281 640x400 RAW8 @ 100 fps
        |
        v
ESP32-P4 MIPI CSI / esp_video
        |
        v
every captured frame
RAW8 -> RGB888 grayscale
        |
        v
800x480 native DPI framebuffer
(640x400 image centred, 80 px left/right, 40 px top/bottom)
        |
        v
ESP32-P4 MIPI DSI
        |
        v
Pi-style ATTINY88 (0x45) + TC358762 bridge
```

The camera capture flow is taken from Wireless-Tag's current WT9932P4-TINY BSP `wt_bsp_csi.c`: `esp_video_init`, `VIDIOC_S_FMT`, MMAP buffers, `VIDIOC_QBUF`, `VIDIOC_STREAMON`, then `VIDIOC_DQBUF` / callback / `VIDIOC_QBUF`. Only the OV9281-specific width, height and RAW8 pixel format are substituted.

The display driver is based on the working ESP32-P4 Raspberry-Pi-display implementation at
`oguzhanbaser/esp32-p4-waveshare-5inch-dsi-lcd`. It uses one DSI data lane at 600 Mbps and the established TC358762 Generic Long Write initialization sequence.

There is no firmware frame limiter. Each OV9281 frame is converted into the display framebuffer as it arrives.

The first test deliberately omits touch, LVGL, USB, recording, SD, and other image processing.

## Expected I2C diagnostics

The firmware probes:

- `0x45` - Pi-style panel control MCU
- `0x38` - common FT5x06 touch address (diagnostic only)
- `0x60` - OV9281 with SID low
- `0x30` - alternate camera-address diagnostic

If `0x45` is absent, this iPistBit revision is not exposing the expected Pi-style control interface and display initialization stops with a clear serial error.

## Build

The project requires ESP-IDF `>=6.0.2`. CI currently builds with ESP-IDF 6.0.2.

```sh
idf.py set-target esp32p4
idf.py build
```

Both CSI and DSI FFCs on this WT9932P4-TINY setup use same-side-contact ribbons.

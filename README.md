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

The camera capture flow follows Wireless-Tag's WT9932P4-TINY BSP `wt_bsp_csi.c`: `esp_video_init`, sensor-format setup, `VIDIOC_S_FMT`, MMAP buffers, `VIDIOC_QBUF`, `VIDIOC_STREAMON`, then `VIDIOC_DQBUF` / callback / `VIDIOC_QBUF`.

## Black-screen investigation

The pinned `esp_cam_sensor` 2.0.1 OV9281 driver describes its register tables as
**800 Mbps per lane**, but supplies `mipi_info.mipi_clk = 400000000`.
`esp_video` 2.0.1 divides that value by 1,000,000 and passes **400 Mbps** to
the CSI receiver. The [Arducam OV9281 driver](https://github.com/ArduCAM/ov9281_driver/blob/08382e6fd9fdb0a8a722a56b40e3b7b7b3dac4fc/ov9281.c)
also documents 800 Mbps per lane for its 24 MHz input / 400 MHz DDR link.

This project copies the active sensor format into persistent application state,
sets its lane-rate metadata to 800,000,000, and applies it with
`VIDIOC_S_SENSOR_FMT` before configuring capture buffers. The sensor register
table, exposure, gain, and lane count are preserved. This is a candidate fix for
the missing frames; successful compilation does not establish hardware success.

On boot:

1. The CPU writes colour bars directly to the display framebuffer and holds them
   for three seconds **before any camera initialization**. They remain until a
   camera frame overwrites the central 640x400 area. The coloured border remains
   visible even if the camera image itself is black.
2. The log prints the actual sensor mode and the corrected CSI lane rate. An
   incompatible saved `sdkconfig` is rejected instead of being silently assumed
   to match `sdkconfig.defaults`.
3. `first CSI frame` confirms a completed capture buffer; `first image` reports
   its minimum, maximum and mean brightness. Recurring `live` lines confirm
   that frames reach the display framebuffer.
4. A two-second `VIDIOC_S_DQBUF_TIMEOUT` prevents a missing frame from blocking
   silently. Repeated waits are logged about every ten seconds. In this pinned
   esp_video version an expired dequeue maps to `EPERM`; the code checks that
   the wait actually elapsed before treating that error as a timeout.

| Observation | What it establishes |
| --- | --- |
| Bars visible, no completed CSI frames | The display path works; capture is failing. |
| No bars, but completed CSI frames and `live` logs | The display path needs investigation independently of capture. |
| Bars visible, dark centre, near-zero image statistics | Frames arrive; inspect sensor exposure and image data. |
| No bars and no completed CSI frames | Neither data path is verified; successful I2C probes are insufficient. |

`display configured` and `stream requested` are initialization status only.

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

The project requires ESP-IDF `>=6.0.3`. CI builds with ESP-IDF 6.0.3.

```sh
idf.py set-target esp32p4
idf.py build
```

Both CSI and DSI FFCs on this WT9932P4-TINY setup use same-side-contact ribbons.

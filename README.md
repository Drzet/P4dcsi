# P4dcsi

End-to-end hardware test for:

- Wireless-Tag **WT9932P4-TINY** (ESP32-P4 v1.x)
- **OV9281** MIPI-CSI monochrome camera
- **iPistBit 4.3" 800x480** Raspberry-Pi-style MIPI-DSI display

The firmware uses the WT9932P4-TINY shared I2C wiring on GPIO7/8. The CSI side follows Wireless-Tag's WT9932P4-TINY BSP implementation; camera reset and PWDN are both unused (`-1`).

## Test path

```
OV9281 1280x720 RAW8 @ 50 fps
        |
        v
ESP32-P4 MIPI CSI / esp_video
        |
        v
every captured frame
RAW8 -> scaled RGB888 grayscale
        |
        v
800x480 native DPI framebuffer
(800x450 image centred, 15 px top/bottom)
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

Hardware testing of `db33fb6` confirmed visible colour bars but zero completed
CSI frames, even with the 800 Mbps receiver setting. The display path is working;
the cause of the capture failure is still unconfirmed.

The next capture test addresses startup order: the OV9281 mode table ends by
enabling streaming, before `VIDIOC_STREAMON` creates the receiver. The application
now puts the sensor into standby through the existing esp_video control interface
and verifies register `0x0100` before receiver setup. `VIDIOC_STREAMON` subsequently
starts CSI, ISP, then the sensor.

Hardware testing of `b5bfab3` then confirmed sustained capture: the first buffer
contained 256,000 bytes and the log reached 2,250 captures with matching framebuffer
updates. The remaining reported fault was the CPU0 idle-task watchdog while
`csi_stream` performed the grayscale conversion. The stream loop now blocks for
one scheduler tick after requeueing a buffer once 100 ms of processing have elapsed,
allowing lower-priority tasks to run even when dequeue never waits. Watchdog
checks remain enabled. The scheduling change needs a new hardware run.

On boot:

1. The CPU writes colour bars directly to the display framebuffer and holds them
   for three seconds **before any camera initialization**. They remain until a
   camera frame overwrites the central 800x450 area. The coloured border remains
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
5. The first timeout also prints a `csi_diag` snapshot: sensor streaming, mode
   and PLL registers; sampled CSI PHY state; and CSI host, bridge and ISP status.
   Include that entire snapshot when reporting the result. High-speed clock
   samples are observations, not a decoded packet or frame count.

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

The current test uses the driver's 1280x720 RAW8 / 50 fps mode. Each capture buffer
contains 921,600 bytes. Preview uses nearest-neighbour scaling to 800x450, retaining
the whole frame and its aspect ratio; the panel cannot display all captured pixels
at once. The standby sequencing and periodic idle-task break are retained.

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

When updating an existing 640x400 checkout, use a fresh configuration file so the
saved low-resolution selection does not override the new defaults:

```sh
idf.py -D SDKCONFIG=sdkconfig.hd build flash monitor
```

The boot log must show `MIPI_2lane_24Minput_RAW8_1280x720_50fps`.

Both CSI and DSI FFCs on this WT9932P4-TINY setup use same-side-contact ribbons.

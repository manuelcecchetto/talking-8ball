# Firmware

Runs on the Waveshare ESP32-S3-Touch-AMOLED-1.75 inside the ball: the screen, shake detection, the
microphones, Wi-Fi to the [server](../server/README.md), and an offline answer bank for when the
server can't be reached. ESP-IDF 5.5 with Waveshare's board support package.

| Listening | Answer |
|---|---|
| ![](../shots/fw-listening.png) | ![](../shots/fw-answer.png) |

(Screenshots taken from the ball over USB with `ballctl.py snap`.)

## Files

| File | What |
|---|---|
| `main/main.c` | Start-up and the ask cycle: listen, think, ask the server (or the offline bank), reveal |
| `main/scene.c` | The screen: its own compositor (see below); LVGL only renders the answer triangle |
| `main/motion.c` | QMI8658 accelerometer: shake trigger, tilt, stirring the liquid |
| `main/voice.c` | ES7210 microphones at 16 kHz, voice-activity detection, WAV for the server |
| `main/net.c` | Wi-Fi, settings in NVS, finds the server by mDNS (`_8ball._tcp`), `POST /ask` |
| `main/console.c` | Line commands over the USB cable |
| `main/answers.c` | Offline answers, Italian and English |
| `tools/ballctl.py` | Logs, console commands and screenshots from the PC |

## Build and flash

From an ESP-IDF 5.5 shell (`export.ps1` on Windows; ESP-IDF refuses Git Bash/MSYS):

```bash
idf.py build
idf.py -p COM5 -b 921600 flash
```

After changing `sdkconfig.defaults`, delete `sdkconfig` so it is regenerated.

## Setting it up over USB

```bash
python tools/ballctl.py COM5 cmd "set ssid MyWifi"
python tools/ballctl.py COM5 cmd "set pass secret"
python tools/ballctl.py COM5 cmd "set token <token from server/config.json>"
python tools/ballctl.py COM5 cmd "reboot"
```

Commands: `status`, `set ssid|pass|server|token|lang <value>`, `reboot`, `ask`, `say <text>` (reveal a
text without asking), `snap` (screenshot), `fps on|off`, `imu on|off`, `mic on|off`.
`ballctl.py COM5 log 10` prints the log, `reset 8` reboots and prints the boot, `snap out.png` saves
what the screen shows.

## The screen

The panel is 466×466 on a 40 MHz QSPI link: a full frame is 434 KB and takes 22 ms just to send, so
smoothness depends on sending few pixels. The first version drew everything with LVGL and ran at
**4.5 fps**: 34 moving specks made more invalidated areas than LVGL keeps (32), so it redrew the whole
screen every frame, in PSRAM buffers that the SPI driver then copied again.

`scene.c` now composes the scene itself:

- Every frame each item (blobs, specks, glow, rings, triangle, bubbles) decides whether it changed
  enough to redraw, and marks its old and new boxes dirty. Rings mark short arcs, not their box.
- The dirty rectangles are merged where that wastes little, then composed in strips into two DMA
  buffers in internal RAM, sent while the next is composed. Core 0 composes the bottom half of each
  strip while core 1 does the top.
- Items are always composed from the state they were last committed with, so a rectangle that
  touches an item which hasn't been redrawn never shows it half moved.
- The triangle and its text are rendered once per answer by LVGL off screen, packed, and flattened
  into a single layer for when it rests.

Measured with `fps on` (frames actually sent per second; 60 is the cap):

| Mode | fps |
|---|---|
| Idle | 60 |
| Listening | 28–30 |
| Answer rising | 30–41 |
| Answer resting | 58–60 |

Learned the hard way:

- **CO5300 windows must start on an even and end on an odd coordinate**, or the edges leave trails.
- **PSRAM reads are the cost**, not arithmetic: the round glows are computed from distance² through a
  table in internal RAM rather than read from images. The triangle's layers (8 bytes/px in PSRAM)
  are the most expensive thing on screen, hence the flattened copy for when it rests.
- The panel wants RGB565 byte-swapped; `compose` works natively and swaps just before sending.
- `esp_lcd_panel_draw_bitmap` waits for the previous colour transfer before sending, which is what
  makes two alternating DMA buffers safe.
- Brightness goes over the same SPI link, so only the render task sets it.

## Memory

Internal RAM is tight (Wi-Fi, DMA buffers, I2S). `net_init` runs first because Wi-Fi fails without
enough internal RAM; the IRAM speed-ups of Wi-Fi/lwIP/FreeRTOS are off and ordinary allocations go
to PSRAM (`sdkconfig.defaults`). The boot log prints free memory after each stage.

## Board quirks

- If the screen stays black and the COM port disappears, the AXP2101 power chip switched off: press
  the PWR side button.
- The accelerometer's first readings jump; `motion.c` ignores shakes for the first 1.5 s.
- Back up the factory image before flashing (`esptool read-flash 0 0x1000000 backup.bin`); restore it
  with `esptool write-flash 0 backup.bin`.

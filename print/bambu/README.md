# Bambu Lab: A1 mini and P2S

Sliced in Bambu Studio 2.08.02.61 with the printers' own profiles and **Bambu PLA Basic**. Each file is
a Bambu project with the plate already sliced: open it in Bambu Studio (or Bambu Handy) and print, or
re-slice it there for another filament. The support painting and all settings are inside.

Use the folder for the printer: `a1mini/` or `p2s/`. They are not interchangeable.

| Job | Colour | A1 mini | P2S | Filament |
|---|---|---|---|---|
| `1_test_rings` | black | 28 min | 28 min | 20 g |
| `2_front_half` | black | 1 h 16 | 1 h 12 | 48 g |
| `3_back_half` | black | 1 h 20 | 1 h 17 | 56 g |
| `4_chassis_stand` | black (the chassis is hidden: any colour) | 29 min | 27 min | 17 g |
| `5_eight_disc` | **white** | 10 min | 9 min | 2 g |
| **Total** | | **3.7 h** | **3.5 h** | **143 g** |

Times are Bambu Studio's estimates for the models alone (the printer adds a few minutes of
calibration per plate).

**Print `1_test_rings` first** and check the twist-lock (below) before the halves.

## Settings in the files

- 0.20 mm Standard (0.16 mm for the disc), 3 walls, 15% infill, seam aligned.
- **Elephant's foot compensation 0.2 mm.** The seam and the twist-lock are sized for it; keep it.
- **Supports: front half only, "normal (manual)", snug, on build plate only.** Only the painted
  areas get support: the 4 screw bosses, the USB-C duct and the 2 pin guides, all inside the dome.
  Don't switch to automatic support: it fills the ring behind the window, where the display glass
  sits, and grows into the twist-lock notches at the rim.
- 5 mm brim on the test rings and both halves, 3 mm on the disc.
- Any PLA or PETG works (matte black looks best). **Not** carbon-fibre or metal-filled: the Wi-Fi
  antenna is inside. With another filament, pick it in Studio and re-slice: the painting stays.

## Fit check with the test rings

Push the back ring's 3 lugs into the 3 notches of the front ring and twist about 28°. It should turn
with light resistance and **click** at the end; twisting back opens it. If it's too tight, too loose
or doesn't click, tell Manuel: the fit is adjusted in `cad/ball.py` and the files regenerated.

## The "8"

Single colour: print `5_eight_disc` in white and fill the recessed "8" with a black paint marker.

With an AMS (P2S): in Bambu Studio, load `out/eight_disc_white.stl` and `out/eight_ink_black.stl`
together and answer **Yes** to "load as a single object with multiple parts", then give the ink part
black and the disc white.

Regenerate these files: `cad/slice_bambu.py` (needs Bambu Studio; `BAMBU_STUDIO` = path to
`bambu-studio.exe`).

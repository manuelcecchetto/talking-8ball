# Print job sheet: talking 8-ball

**Bambu Lab A1 mini or P2S? Use [bambu/](bambu/README.md): sliced, ready-to-print projects for those
printers.** The rest of this page is the PrusaSlicer version (and the general rules).

Give this folder to whoever prints it (a print service, a friend, a maker space).
Everything here was sliced and checked with PrusaSlicer 2.9.6 on a Prusa MK4S profile (bed 250 × 210).
Any FDM printer with a bed of at least 120 × 120 mm and 60 mm height can print it; on a bed under
~210 mm wide, print the two parts of jobs 1 and 4 one at a time.

## Jobs

| Job | File | Colour | Layer | Supports | Brim | Time* | Filament* |
|---|---|---|---|---|---|---|---|
| 1 | `1_test_rings.3mf` (2 rings) | black | 0.20 | none | 5 mm | 1 h 00 | 20 g |
| 2 | `2_front_half.3mf` | black | 0.20 | **painted only** (see below) | 5 mm | 2 h 20 | 50 g |
| 3 | `3_back_half.3mf` | black | 0.20 | none | 5 mm | 2 h 35 | 58 g |
| 4 | `4_chassis_stand.3mf` (2 parts) | black (chassis is hidden: any colour) | 0.20 | none | none | 0 h 52 | 18 g |
| 5 | `5_eight_disc.3mf` | **white** | 0.15 | none | 3 mm | 0 h 09 | 2 g |
| | | | | | | **≈ 7 h** | **≈ 148 g** |

\* PrusaSlicer's estimate for a Prusa MK4S with Generic PLA. Other printers differ.

**Print job 1 first** and check the fit (below) before printing jobs 2 and 3.

## Settings (all jobs)

- **Material:** PLA (or PETG). Matte black looks best. Not carbon-fibre or metal-filled filament:
  the Wi-Fi antenna is inside.
- 3 perimeters, 15% infill, seam "aligned".
- **Elephant's foot compensation 0.2 mm.** The seam and the twist-lock are sized for it.
- Parts are already oriented. Don't rotate or flip them.

## Supports (front half only)

The 3MF has **support enforcers painted** on the only surfaces that need them, and automatic support
turned off:
- the undersides of the 4 screw bosses;
- the USB-C duct;
- the 2 small pin guides.

These are all inside the dome. Snug (straight-down) supports from the build plate.

If your slicer drops the painting (other slicers may), set it up by hand on `out/front_half.stl`:
**support on build plate only, snug/normal style, and enforcers on just those features (or block
everywhere else).** Do **not** use automatic supports everywhere. They fill the ring behind the
window, where the display glass sits, and can grow into the twist-lock notches at the rim.

Nothing on the outside of the ball needs support.

## Fit check with the test rings (job 1)

1. Push the black back ring's 3 lugs into the 3 notches of the front ring and twist about 28°.
2. It should turn with light resistance and **click** at the end. Twisting back opens it.
3. If it's **too tight** or **too loose** (or doesn't click), change `SOCK_R` / `SLOT_Z` / the bump in
   `cad/ball.py`, rerun `cad/ball.py` and `cad/slice.py`, and print the rings again.

## Files

- `*.3mf`: model + settings (+ painted supports). Opens in PrusaSlicer; Bambu Studio and OrcaSlicer
  import the geometry, and possibly the settings and painting (not checked).
- `gcode-mk4s/*.gcode`: ready to print **only on a Prusa MK4S** with a 0.4 mm nozzle and PLA.
- STLs in `../out/` if someone wants plain meshes.

"""Talking 8-ball: printable shell and internals for the Waveshare ESP32-S3-Touch-AMOLED-1.75.

Coordinates (mm): ball centre at the origin, window on +Z, the "8" on -Z, seam at Z = 0.
Run:  .venv/Scripts/python.exe cad/ball.py [--check]
Writes print-oriented STLs and an assembly STEP to out/.
"""
import math
import sys
from pathlib import Path

import trimesh

from build123d import (Align, Box, Color, Compound, Cylinder, Cone, Pos, Polyline, Rot, Sphere, Text,
                       export_step, export_stl, extrude, import_step, make_face)

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "out"
BOARD_STEP = ROOT / "vendor/ws175/extracted/ESP32-S3-Touch-AMOLED-1_75.stp"
FONT = "C:/Windows/Fonts/ariblk.ttf"

SPEAKER = False                  # True: pocket + grille for the Waveshare 2030 cavity speaker (20x30x5.5)

# --- ball ---------------------------------------------------------------
R_O, WALL = 50.0, 3.0
R_I = R_O - WALL

# --- window (board facts from Waveshare's drawing and STEP) ---------------
GLASS_R = 48.96 / 2              # cover glass / touch panel outer radius
Z_FLAT = 42.5                    # flat window face
LIP = 1.2                        # lip in front of the glass, hides its black border
LIP_R = 45.5 / 2                 # window opening (visible area is 44.16)
CB_R = GLASS_R + 0.22            # counterbore that locates the glass
FOAM = 0.6                       # gap for 0.8-1 mm foam tape between lip and glass
Z_GLASS = Z_FLAT - LIP - FOAM    # glass front face
STEP_GLASS_Y = 2.77              # glass front in the STEP's frame (board normal = +Y)
DZ = Z_GLASS - STEP_GLASS_Y      # STEP y -> ball z offset

def bz(step_y):                  # STEP y -> ball z
    return step_y + DZ

Z_STANDOFF = bz(-7.63)           # face of the board's M2 brass standoffs
STANDOFFS = [(13.75, -14.70), (-13.75, -14.70), (0.0, 20.50)]   # ball x, y
USB_Z = bz((-7.38 + -3.22) / 2)  # USB-C centre height, port faces -X
USB_FACE_R = 23.79
MICS = [(-17.35, -11.74), (-17.54, 10.96)]                       # MEMS mics, sound enters from the side
MIC_Z = bz(-4.73)
BUTTONS = [(19.54, 11.31), (19.54, -11.31)]                      # PWR, BOOT: side buttons pressed along -X
BUTTON_Z = bz(-4.83)
GUIDE_R = 2.0

# --- chassis ----------------------------------------------------------------
PLATE_T = 3.0
Z_PLATE_TOP = Z_STANDOFF
Z_PLATE_BOT = Z_PLATE_TOP - PLATE_T
PLATE_R = 23.6
EAR_R, EAR_PAD = 31.5, 4.0       # chassis-to-shell screws: M3 self-tapping into the bosses
EAR_ANGLES = (45, 135, 225, 315)
BAT = (34.0, 50.0, 10.0)         # 103450 LiPo: width (X), length (Y), thickness (Z)
BAT_CLEAR = 0.6

# --- seam: twist-lock (male spigot on the back half) ------------------------
SPIG_IN, SPIG_OUT, SPIG_H = 45.0, 47.0, 7.4
SOCK_R, SOCK_H = 47.25, 7.6
CHAMF_OUT = 48.0                 # back half: cone 48.0 -> 47.0 above the seam (printable ledge)
LUG_OUT, LUG_W, LUG_Z = 48.25, 12.0, (2.45, 5.05)
SLOT_OUT, SLOT_Z = 48.5, (2.2, 5.3)
ENTRY_ANGLES = (90, 210, 330)
TWIST = 28                       # degrees from entry to locked

# --- "8" ------------------------------------------------------------------
EIGHT_R, EIGHT_T, EIGHT_INK = 20.0, 1.15, 0.6


# --- helpers ----------------------------------------------------------------
def cyl(r, z0, z1, x=0.0, y=0.0):
    return Pos(x, y, z0) * Cylinder(r, z1 - z0, align=(Align.CENTER, Align.CENTER, Align.MIN))

def box(x0, x1, y0, y1, z0, z1):
    return Pos(x0, y0, z0) * Box(x1 - x0, y1 - y0, z1 - z0, align=(Align.MIN, Align.MIN, Align.MIN))

def ring(r0, r1, z0, z1):
    return cyl(r1, z0, z1) - cyl(r0, z0 - 1, z1 + 1)

def sector(a0, a1, r, z0, z1, steps=32):
    pts = [(0.0, 0.0)] + [(r * math.cos(math.radians(a0 + (a1 - a0) * i / steps)),
                           r * math.sin(math.radians(a0 + (a1 - a0) * i / steps))) for i in range(steps + 1)]
    return Pos(0, 0, z0) * extrude(make_face(Polyline(*pts, close=True)), amount=z1 - z0)

def slab(angle, width, r0, r1, z0, z1):
    """Radial bar of constant width pointing at `angle`."""
    return Rot(0, 0, angle) * box(r0, r1, -width / 2, width / 2, z0, z1)

def along_x(r, x0, x1, y, z):
    return Pos(x0, y, z) * Rot(0, 90, 0) * Cylinder(r, x1 - x0, align=(Align.CENTER, Align.CENTER, Align.MIN))

def radial_hole(r, angle, z, r0=20.0, r1=60.0):
    return Rot(0, 0, angle) * along_x(r, r0, r1, 0.0, z)

def shell(z0, z1):
    return (Sphere(R_O) - Sphere(R_I)) & box(-60, 60, -60, 60, z0, z1)


def export_mesh(shape, path):
    """STL export. OpenCascade's tessellation leaves a pinhole (2 open edges) where a sphere's pole
    lies on a kept face; close it here and refuse to write anything that is still open."""
    export_stl(shape, str(path), tolerance=0.01, angular_tolerance=0.04)
    m = trimesh.load(str(path))
    if not m.is_watertight:
        m.merge_vertices(digits_vertex=5)
        m.update_faces(m.nondegenerate_faces(height=1e-9))
        trimesh.repair.fill_holes(m)
        trimesh.repair.fix_normals(m)
        if not m.is_watertight:
            raise RuntimeError(f"{path.name}: mesh still has open edges")
        m.export(str(path))


# --- board ------------------------------------------------------------------
def load_board():
    return Pos(0, 0, DZ) * Rot(90, 0, 0) * import_step(str(BOARD_STEP))


def low_parts(board):
    """Board parts that reach below the standoff face: the plate must clear them."""
    cuts = []
    for s in board.solids():
        b = s.bounding_box()
        if b.min.Z < Z_PLATE_TOP - 0.05 and b.max.X - b.min.X < 30:
            cuts.append((b.min.X, b.max.X, b.min.Y, b.max.Y))
    return cuts


# --- front half (window) --------------------------------------------------------
def front_half():
    body = shell(0, Z_FLAT)

    # Bosses for the chassis screws (support from the build plate when printing).
    for a in EAR_ANGLES:
        x, y = EAR_R * math.cos(math.radians(a)), EAR_R * math.sin(math.radians(a))
        body += cyl(EAR_PAD, Z_PLATE_TOP, R_O, x, y) & Sphere(R_O - 0.5)

    # USB-C duct: guides the plug to the receptacle.
    duct = box(-40, -25.0, -7.8, 7.8, USB_Z - 4.9, USB_Z + 4.9) & Sphere(R_O - 0.5)
    body += duct

    # Pin guides for the PWR and BOOT side buttons; they start outside the glass's path into the shell.
    for (bx, by) in BUTTONS:
        x0 = math.sqrt(GLASS_R ** 2 - (abs(by) - GUIDE_R) ** 2) + 0.4
        body += along_x(GUIDE_R, x0, 45, by, BUTTON_Z) & Sphere(R_O - 0.5)

    if SPEAKER:
        body += speaker_seat()

    # Window: opening, front chamfer, counterbore for the glass.
    body -= cyl(LIP_R, Z_FLAT - LIP - 1, Z_FLAT + 1)
    body -= Pos(0, 0, Z_FLAT - 0.6) * Cone(LIP_R, LIP_R + 0.6, 0.61,
                                           align=(Align.CENTER, Align.CENTER, Align.MIN))
    body -= cyl(CB_R, 30, Z_FLAT - LIP)

    # Screw pilots (M3 self-tapping).
    for a in EAR_ANGLES:
        x, y = EAR_R * math.cos(math.radians(a)), EAR_R * math.sin(math.radians(a))
        body -= cyl(1.25, Z_PLATE_TOP - 1, Z_PLATE_TOP + 7, x, y)

    # USB-C channel through duct and wall, with a lead-in.
    body -= box(-60, -24.0, -6.6, 6.6, USB_Z - 3.7, USB_Z + 3.7)
    body -= box(-60, -36.6, -7.4, 7.4, USB_Z - 4.5, USB_Z + 4.5) & cyl(60, -1, 60) - Sphere(R_O - 1.0)

    # Mic holes and button pin holes.
    for (mx, my) in MICS:
        body -= radial_hole(0.65, math.degrees(math.atan2(my, mx)), MIC_Z)
    for (bx, by) in BUTTONS:
        body -= along_x(1.1, bx + 0.5, 60, by, BUTTON_Z)

    if SPEAKER:
        body -= speaker_cuts()

    # Seam: outer edge chamfer (sits on the bed: hides elephant's foot at the seam), socket, mouth
    # chamfer, twist-lock slots with a detent bump.
    body -= cyl(R_O + 1, -1, 0.5) - Pos(0, 0, -0.1) * Cone(R_O - 0.6, R_O, 0.6,
                                                           align=(Align.CENTER, Align.CENTER, Align.MIN))
    # Socket, topped with a 45-degree chamfer instead of a flat ledge (prints without support).
    body -= cyl(SOCK_R, -1, SOCK_H)
    body -= Pos(0, 0, SOCK_H - 0.01) * Cone(SOCK_R, SOCK_R - 1.6, 1.6, align=(Align.CENTER, Align.CENTER, Align.MIN))
    body -= Pos(0, 0, -0.01) * Cone(CHAMF_OUT + 0.2, SOCK_R, CHAMF_OUT + 0.2 - SOCK_R,
                                    align=(Align.CENTER, Align.CENTER, Align.MIN))
    lug_half = math.degrees(LUG_W / 2 / SPIG_OUT)
    # Slot ceilings rise 45 degrees toward the inside, so they print without support. The lug only
    # bears on the slot floor, the ceiling is clearance.
    roof_top = SLOT_Z[1] + SLOT_OUT - SOCK_R
    roof = Cone(SLOT_OUT + SLOT_Z[1], SOCK_R - 0.01, roof_top + 0.01 - 0,
                align=(Align.CENTER, Align.CENTER, Align.MIN))
    for a in ENTRY_ANGLES:
        # Entry and twist slot merged first, then trimmed once: trimming them separately leaves two
        # coincident faces at SLOT_OUT, and OpenCascade's boolean then silently drops material.
        slot = slab(a, LUG_W + 1.0, 40, SLOT_OUT + 2, -1, roof_top) + \
            sector(a, a + TWIST + lug_half + 1.5, SLOT_OUT + 2, SLOT_Z[0], roof_top)
        body -= slot & roof & ring(SOCK_R - 0.01, SLOT_OUT, -2, 10)
        bump_a = math.radians(a + TWIST - lug_half - 1.6)
        body += (Pos(0, 0, SLOT_Z[0]) * Rot(0, 0, math.degrees(bump_a)) *
                 along_x(0.45, SOCK_R + 0.05, SLOT_OUT + 0.05, 0, 0)) & cyl(SLOT_OUT + 0.1, SLOT_Z[0] - 1, SLOT_Z[0] + 1)
    return body


# --- speaker (optional) -----------------------------------------------------------
SPK_E, SPK_SEAT = 28.0, 43.0     # elevation of the speaker axis, seat distance from the centre
SPK = (5.5, 30.0, 20.0)          # depth, width (tangential), height (meridional)

def _spk(shape):
    return Rot(0, -SPK_E, 0) * shape

def speaker_seat():
    d, w, h = SPK
    frame = box(SPK_SEAT - 4.5, R_O + 2, -w / 2 - 1.6, w / 2 + 1.6, -h / 2 - 1.6, h / 2 + 1.6)
    return _spk(frame) & Sphere(R_O - 0.01)

def speaker_cuts():
    d, w, h = SPK
    cut = _spk(box(SPK_SEAT - 10, SPK_SEAT, -w / 2 - 0.3, w / 2 + 0.3, -h / 2 - 0.3, h / 2 + 0.3))
    pitch = 2.6
    for i in range(-5, 6):
        for j in range(-4, 5):
            t = i * pitch + (j % 2) * pitch / 2
            v = j * pitch * math.sqrt(3) / 2
            if (t / 11) ** 2 + (v / 7) ** 2 <= 1:
                cut += _spk(Pos(SPK_SEAT - 1, t, v) * Rot(0, 90, 0) *
                            Cylinder(0.8, 12, align=(Align.CENTER, Align.CENTER, Align.MIN)))
    return cut


# --- back half (the 8) -----------------------------------------------------------
def back_half():
    body = shell(-R_O - 1, 0)
    body += ring(SPIG_IN, SPIG_OUT, -0.5, SPIG_H)
    # Lead-in on the spigot tip, which is also the first layer on the bed (elephant's foot).
    body -= cyl(SPIG_OUT + 1, SPIG_H - 0.5, SPIG_H + 1) - Pos(0, 0, SPIG_H - 0.5) * Cone(
        SPIG_OUT, SPIG_OUT - 0.5, 0.5, align=(Align.CENTER, Align.CENTER, Align.MIN))
    body += Cone(CHAMF_OUT, SPIG_OUT, CHAMF_OUT - SPIG_OUT, align=(Align.CENTER, Align.CENTER, Align.MIN)) \
        - cyl(SPIG_IN, -1, 5)
    lug = ring(SPIG_OUT - 0.5, LUG_OUT, *LUG_Z)
    for a in ENTRY_ANGLES:
        body += lug & slab(a + TWIST, LUG_W, 0, 60, -5, 10)
    # Recess for the white "8" disc.
    body -= (Sphere(R_O + 1) - Sphere(R_O - EIGHT_T - 0.05)) & cyl(EIGHT_R + 0.3, -R_O - 2, -30)
    return body


def eight_disc():
    disc = (Sphere(R_O) - Sphere(R_O - EIGHT_T)) & cyl(EIGHT_R, -R_O - 2, -30)
    glyph = Pos(0, 0, -R_O - 2) * extrude(Rot(180, 0, 0) * Text("8", font_size=34, font_path=FONT,
                                                                align=(Align.CENTER, Align.CENTER)), amount=-10)
    ink = glyph & (Sphere(R_O) - Sphere(R_O - EIGHT_INK))
    return disc - ink, ink


# --- chassis -------------------------------------------------------------------
def chassis(cuts):
    z0, z1 = Z_PLATE_BOT, Z_PLATE_TOP
    plate = cyl(PLATE_R, z0, z1)
    for a in EAR_ANGLES:
        x, y = EAR_R * math.cos(math.radians(a)), EAR_R * math.sin(math.radians(a))
        plate += slab(a, 2 * EAR_PAD, 15, EAR_R, z0, z1) + cyl(EAR_PAD, z0, z1, x, y)
    # Battery bay: rails with lips, closed at -Y, slide the cell in from +Y.
    bw, bl, bt = BAT[0] / 2 + BAT_CLEAR / 2, BAT[1] / 2 + BAT_CLEAR / 2, BAT[2] + BAT_CLEAR
    zb = z0 - bt                              # cell bottom
    y_end = bl + 3.2
    for s in (1, -1):
        plate += box(*sorted((s * bw, s * (bw + 1.6))), -bl - 1.6, y_end, zb - 1.5, z0)
        plate += box(*sorted((s * (bw - 1.5), s * bw)), -bl, y_end, zb - 1.5, zb)
    plate += box(-bw - 1.6, bw + 1.6, -bl - 1.6, -bl, zb - 1.5, z0)
    # Holes: chassis screws (M3), board screws (M2) with head counterbores, zip-tie slots, board parts.
    for a in EAR_ANGLES:
        x, y = EAR_R * math.cos(math.radians(a)), EAR_R * math.sin(math.radians(a))
        plate -= cyl(1.7, z0 - 1, z1 + 1, x, y)
    for (x, y) in STANDOFFS:
        plate -= cyl(1.15, z0 - 1, z1 + 1, x, y)
        plate -= cyl(2.1, z0 - 1, z0 + 1.5, x, y)
    plate -= box(-30, 30, bl + 0.4, bl + 2.4, zb + 3, zb + 7)
    for (x0, x1, y0, y1) in cuts:
        plate -= box(x0 - 0.5, x1 + 0.5, y0 - 0.5, y1 + 0.5, z0 + 0.6, z1 + 1)
    return plate


def stand():
    zc = 12 + math.sqrt((R_O + 0.4) ** 2 - 20 ** 2)
    return ring(13, 24, 0, 12) - Pos(0, 0, zc) * Sphere(R_O + 0.4)


# --- checks ---------------------------------------------------------------------
def overlap(a, b):
    try:
        return (a & b).volume
    except Exception:
        return float("nan")


def board_overlap(board, part, name, solids=None, quiet=False):
    pb = part.bounding_box()
    hits = []
    for s in solids or board.solids():
        b = s.bounding_box()
        if b.max.X < pb.min.X or b.min.X > pb.max.X or b.max.Y < pb.min.Y or b.min.Y > pb.max.Y \
                or b.max.Z < pb.min.Z or b.min.Z > pb.max.Z:
            continue
        v = overlap(s, part)
        if v > 1e-3:
            hits.append((v, b))
    if quiet:
        return sum(h[0] for h in hits)
    print(f"  board vs {name}: {len(hits)} touching solids, {sum(h[0] for h in hits):.3f} mm3")
    for v, b in sorted(hits, key=lambda h: -h[0])[:8]:
        print(f"    {v:8.3f} mm3 at x {b.min.X:.1f}..{b.max.X:.1f} y {b.min.Y:.1f}..{b.max.Y:.1f} z {b.min.Z:.1f}..{b.max.Z:.1f}")


def main():
    check = "--check" in sys.argv
    OUT.mkdir(exist_ok=True)
    print("loading board STEP ...")
    board = load_board()
    bb = board.bounding_box()
    print(f"  board z {bb.min.Z:.2f}..{bb.max.Z:.2f}, plate {Z_PLATE_BOT:.2f}..{Z_PLATE_TOP:.2f}, USB z {USB_Z:.2f}")

    front, back, ch = front_half(), back_half(), chassis(low_parts(board))
    disc, ink = eight_disc()
    st = stand()
    bw, bl = BAT[0] / 2, BAT[1] / 2
    battery = box(-bw, bw, -bl, bl, Z_PLATE_BOT - BAT[2] - 0.3, Z_PLATE_BOT - 0.3)

    parts = {"front_half": front, "back_half": back, "chassis": ch, "eight_disc_white": disc,
             "eight_ink_black": ink, "stand": st}
    for n, p in parts.items():
        b = p.bounding_box()
        print(f"{n:18s} {p.volume / 1000:6.1f} cm3  ({b.size.X:.1f} x {b.size.Y:.1f} x {b.size.Z:.1f})  "
              f"solids={len(p.solids())}")

    if check:
        print("interference (should be ~0 except the detent bumps, ~0.1 mm3 each):")
        print(f"  front vs back   {overlap(front, back):.3f} mm3 (locked)")
        open_back = Rot(0, 0, -TWIST) * back
        print(f"  front vs back   {overlap(front, open_back):.3f} mm3 (lugs at the entries)")
        for dz in (2.0, 4.0, 6.0):
            print(f"    sliding in, {dz} mm apart: {overlap(front, Pos(0, 0, -dz) * open_back):.3f} mm3")
        print(f"    half-twisted: {overlap(front, Rot(0, 0, -TWIST / 2) * back):.3f} mm3 (detent bump)")
        print(f"  front vs chassis {overlap(front, ch):.3f} mm3")
        print(f"  back vs chassis  {overlap(back, ch):.3f} mm3")
        print(f"  battery vs chassis {overlap(battery, ch):.3f}  vs front {overlap(battery, front):.3f}  vs back {overlap(battery, back):.3f}")
        print(f"  disc vs back    {overlap(disc + ink, back):.3f} mm3")
        sweep = [overlap(front, Pos(0, 0, -dz) * ch) +
                 board_overlap(None, front, "", [Pos(0, 0, -dz) * s for s in board.solids()], quiet=True)
                 for dz in (40, 30, 20, 12, 8, 5, 3, 1.5, 0.5)]
        print("  inserting board+chassis, 40..0.5 mm before seated:", " ".join(f"{v:.2f}" for v in sweep))
        plug = box(-80, -(USB_FACE_R + 0.1), -6.2, 6.2, USB_Z - 3.25, USB_Z + 3.25)  # 12.4 x 6.5 overmold
        print(f"  USB-C plug overmold seated: vs front {overlap(plug, front):.3f}  vs chassis {overlap(plug, ch):.3f}")
        board_overlap(board, plug, "plug overmold")
        board_overlap(board, front, "front_half")
        board_overlap(board, ch, "chassis")
        board_overlap(board, back, "back_half")

    # Print orientation: flat on the bed, z >= 0.
    def on_bed(shape, flip=False):
        s = Rot(180, 0, 0) * shape if flip else shape
        return Pos(0, 0, -s.bounding_box().min.Z) * s

    printed = {
        "front_half": on_bed(front),               # rim down
        "back_half": on_bed(back, flip=True),      # spigot down, the 8 up
        "chassis": on_bed(ch, flip=True),          # plate down
        "eight_disc_white": on_bed(disc, flip=True),
        "eight_ink_black": Pos(0, 0, -(Rot(180, 0, 0) * disc).bounding_box().min.Z) * (Rot(180, 0, 0) * ink),
        "stand": on_bed(st),
        # ~10 g test rings: print these first to check the twist-lock fit.
        "test_seam_front": on_bed(front & cyl(60, -1, 10)),
        "test_seam_back": on_bed(back & cyl(60, -4, 10), flip=True),
    }
    for n, p in printed.items():
        export_mesh(p, OUT / f"{n}.stl")

    asm = []   # the board is left out: its vendor STEP alone is 18 MB
    for shape, label, color in [(front, "front_half", "black"), (back, "back_half", "black"),
                                (ch, "chassis", "orange"), (disc, "eight_disc", "white"),
                                (ink, "eight_ink", "black"), (battery, "battery_103450", "silver")]:
        shape.label, shape.color = label, Color(color)
        asm.append(shape)
    export_step(Compound(children=asm), str(OUT / "assembly.step"))
    for shape, label in [(front, "front_half"), (back, "back_half"), (ch, "chassis"), (disc, "eight_disc"),
                         (ink, "eight_ink"), (battery, "battery")]:
        export_stl(shape, str(OUT / f"asm_{label}.stl"), tolerance=0.01, angular_tolerance=0.04)  # viewer only
    export_stl(board, str(OUT / "asm_board.stl"), tolerance=0.05, angular_tolerance=0.3)
    print("written to", OUT)


if __name__ == "__main__":
    main()

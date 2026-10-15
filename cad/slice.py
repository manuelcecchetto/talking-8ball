"""Package and verify the print jobs.

For each job: resolve the settings with PrusaSlicer (reference printer Prusa MK4S, Generic PLA), write
print/<job>.3mf (parts arranged on the bed, settings embedded, support enforcers painted on the front
half), slice that same 3MF and check the G-code: time, grams, slicer warnings, and that support only
grows where it was painted.
Run: .venv/Scripts/python.exe cad/slice.py   (after cad/ball.py)
"""
import math
import os
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

import numpy as np
import trimesh

sys.path.insert(0, str(Path(__file__).parent))
import ball  # noqa: E402  (single source of the dimensions)

ROOT = Path(__file__).resolve().parent.parent
OUT, PRINT = ROOT / "out", ROOT / "print"
# PrusaSlicer 2.9.6 CLI (prusa-slicer-console.exe on Windows) and a datadir with the PrusaResearch vendor
# profiles and the MK4S enabled (see README).
SLICER = Path(os.environ.get("PRUSA_SLICER", "prusa-slicer-console.exe"))
DATADIR = Path(os.environ.get("PRUSA_DATADIR", Path.home() / "tools" / "ps-datadir"))
PRINTER = "Original Prusa MK4S 0.4 nozzle"
FILAMENT = "Generic PLA @MK4S"

COMMON = ["--perimeters", "3", "--fill-density", "15%", "--seam-position", "aligned",
          "--elefant-foot-compensation", "0.2"]
# Support only where painted (support_material_auto off): the bosses, the USB duct and the pin guides.
PAINTED = ["--support-material", "--no-support-material-auto", "--support-material-buildplate-only",
           "--support-material-style", "snug"]

JOBS = {
    # job: (parts, print profile, extra options, filament colour)
    "1_test_rings":    (["test_seam_front", "test_seam_back"], "0.20mm STRUCTURAL @MK4S 0.4", ["--brim-width", "5"], "black"),
    "2_front_half":    (["front_half"], "0.20mm STRUCTURAL @MK4S 0.4", ["--brim-width", "5"] + PAINTED, "black"),
    "3_back_half":     (["back_half"], "0.20mm STRUCTURAL @MK4S 0.4", ["--brim-width", "5"], "black"),
    "4_chassis_stand": (["chassis", "stand"], "0.20mm STRUCTURAL @MK4S 0.4", [], "any (hidden), black for the stand"),
    "5_eight_disc":    (["eight_disc_white"], "0.15mm STRUCTURAL @MK4S 0.4", ["--brim-width", "3"], "white"),
}


def slicer(args):
    r = subprocess.run([str(SLICER), "--datadir", str(DATADIR), "--printer-profile", PRINTER,
                        "--material-profile", FILAMENT, *args], capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"PrusaSlicer failed: {' '.join(args)}\n{r.stdout}{r.stderr}")
    return r.stdout + r.stderr


# --- support enforcers ------------------------------------------------------------
def enforced(mesh):
    """Downward faces of the front half that cannot print in the air: boss bottoms, USB duct
    underside, pin-guide undersides. Everything else (dome, window lip, slots, holes) prints bare."""
    c, n = mesh.triangles_center, mesh.face_normals
    down = n[:, 2] < -0.3
    pick = np.zeros(len(c), bool)
    for a in ball.EAR_ANGLES:
        bx, by = ball.EAR_R * math.cos(math.radians(a)), ball.EAR_R * math.sin(math.radians(a))
        pick |= (np.hypot(c[:, 0] - bx, c[:, 1] - by) < ball.EAR_PAD + 0.3) & \
                (abs(c[:, 2] - ball.Z_PLATE_TOP) < 0.3)
    pick |= (c[:, 0] < -24.5) & (abs(c[:, 1]) < 8.3) & (c[:, 2] < ball.USB_Z - 4.5) & (c[:, 2] > ball.USB_Z - 5.5)
    for bx, by in ball.BUTTONS:
        axis = np.hypot(c[:, 1] - by, c[:, 2] - ball.BUTTON_Z)
        pick |= (c[:, 0] > 21) & (axis > 1.6) & (axis < ball.GUIDE_R + 0.2) & (c[:, 2] < ball.BUTTON_Z)
    return down & pick


# --- 3MF --------------------------------------------------------------------------
def bed_centre(cfg):
    pts = [tuple(map(float, p.split("x"))) for p in re.search(r"^bed_shape = (.+)$", cfg, re.M).group(1).split(",")]
    return (min(p[0] for p in pts) + max(p[0] for p in pts)) / 2, (min(p[1] for p in pts) + max(p[1] for p in pts)) / 2


def write_3mf(path, parts, cfg, paint):
    meshes = [(p, trimesh.load(str(OUT / f"{p}.stl"))) for p in parts]
    cx, cy = bed_centre(cfg)
    widths = [m.extents[0] for _, m in meshes]
    x = cx - (sum(widths) + 8 * (len(widths) - 1)) / 2
    objects, items, volumes = [], [], []
    for i, (name, m) in enumerate(meshes, start=1):
        centre = (m.bounds[0][:2] + m.bounds[1][:2]) / 2
        v = m.vertices - [centre[0], centre[1], 0]
        marks = enforced(m) if paint.get(name) else np.zeros(len(m.faces), bool)
        verts = "\n".join(f'     <vertex x="{a:.5f}" y="{b:.5f}" z="{c:.5f}"/>' for a, b, c in v)
        tris = "\n".join(f'     <triangle v1="{a}" v2="{b}" v3="{c}"' + (' slic3rpe:custom_supports="4"' if k else "") + "/>"
                         for (a, b, c), k in zip(m.faces, marks))
        objects.append(f'  <object id="{i}" type="model">\n   <mesh>\n    <vertices>\n{verts}\n    </vertices>\n'
                       f'    <triangles>\n{tris}\n    </triangles>\n   </mesh>\n  </object>')
        items.append(f'  <item objectid="{i}" transform="1 0 0 0 1 0 0 0 1 {x + widths[i - 1] / 2:.3f} {cy:.3f} 0" printable="1"/>')
        volumes.append(f' <object id="{i}" instances_count="1">\n  <metadata type="object" key="name" value="{name}"/>\n'
                       f'  <volume firstid="0" lastid="{len(m.faces) - 1}">\n'
                       f'   <metadata type="volume" key="name" value="{name}"/>\n'
                       f'   <metadata type="volume" key="volume_type" value="ModelPart"/>\n'
                       f'   <mesh edges_fixed="0" degenerate_facets="0" facets_removed="0" facets_reversed="0" backwards_edges="0"/>\n'
                       f'  </volume>\n </object>')
        x += widths[i - 1] + 8
        if paint.get(name):
            print(f"    {name}: {int(marks.sum())} triangles painted as support enforcers")
    model = ('<?xml version="1.0" encoding="UTF-8"?>\n<model unit="millimeter" xml:lang="en-US" '
             'xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02" '
             'xmlns:slic3rpe="http://schemas.slic3r.org/3mf/2017/06">\n'
             ' <metadata name="slic3rpe:Version3mf">1</metadata>\n'
             f' <metadata name="Title">{path.stem}</metadata>\n <resources>\n' + "\n".join(objects) +
             '\n </resources>\n <build>\n' + "\n".join(items) + '\n </build>\n</model>\n')
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml",
                   '<?xml version="1.0" encoding="UTF-8"?>\n<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
                   '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
                   '<Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/></Types>')
        z.writestr("_rels/.rels",
                   '<?xml version="1.0" encoding="UTF-8"?>\n<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
                   '<Relationship Target="/3D/3dmodel.model" Id="rel-1" '
                   'Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/></Relationships>')
        z.writestr("3D/3dmodel.model", model)
        z.writestr("Metadata/Slic3r_PE.config", "; generated by cad/slice.py\n\n" +
                   "\n".join(f"; {l}" for l in cfg.splitlines() if l and not l.startswith("#")) + "\n")
        z.writestr("Metadata/Slic3r_PE_model.config",
                   '<?xml version="1.0" encoding="UTF-8"?>\n<config>\n' + "\n".join(volumes) + "\n</config>\n")


# --- G-code checks -----------------------------------------------------------------
MOVE = re.compile(r"G[0-3] ")


def support_points(gcode):
    """(x, y, z) of both ends of every support extrusion. Arcs (G2/G3) update the position too;
    un-retracts (E without X/Y) lay no plastic and come before the next feature's ;TYPE line."""
    kind, x, y, z, pts = None, 0.0, 0.0, 0.0, []
    for line in gcode.splitlines():
        if line.startswith(";TYPE:"):
            kind = line[6:]
        elif line.startswith(";Z:"):
            z = float(line[3:])
        elif MOVE.match(line):
            mx, my, me = re.search(r"X([-\d.]+)", line), re.search(r"Y([-\d.]+)", line), re.search(r"E([\d.]+)", line)
            x0, y0 = x, y
            x = float(mx.group(1)) if mx else x
            y = float(my.group(1)) if my else y
            if kind and kind.startswith("Support") and me and float(me.group(1)) > 0 and (mx or my):
                pts += [(x0, y0, z), (x, y, z)]
    return np.array(pts).reshape(-1, 3)


def check_front_support(pts, centre):
    """Support may only rise inside the shell and stop under the painted features."""
    if not len(pts):
        return "NO SUPPORT (painting not picked up)"
    r = np.hypot(pts[:, 0] - centre[0], pts[:, 1] - centre[1])
    z = pts[:, 2]
    wall = np.where(z < ball.SOCK_H, ball.SOCK_R, np.sqrt(np.maximum(ball.R_I ** 2 - z ** 2, 0)))
    touching = int((r > wall - 0.6).sum())
    top = z.max()
    return (f"support {len(pts)} moves, up to z={top:.1f} (bosses at {ball.Z_PLATE_TOP:.1f}), "
            f"{touching} near the shell wall, highest radius {r.max():.1f}")


def main():
    if not SLICER.exists() and not shutil.which(str(SLICER)):
        sys.exit(f"PrusaSlicer not found at {SLICER}: set PRUSA_SLICER")
    gdir = PRINT / "gcode-mk4s"
    gdir.mkdir(parents=True, exist_ok=True)
    for old in PRINT.glob("*.3mf"):
        old.unlink()
    total_g = total_min = 0.0
    for job, (parts, profile, extra, colour) in JOBS.items():
        opts = ["--print-profile", profile, *COMMON, *extra]
        ini = PRINT / f"{job}.ini"
        slicer([*opts, "--save", str(ini)])
        cfg = ini.read_text()
        tmf = PRINT / f"{job}.3mf"
        write_3mf(tmf, parts, cfg, {"front_half": True})
        ini.unlink()
        gfile = gdir / f"{job}.gcode"
        log = slicer([*opts, "--no-binary-gcode", "--dont-arrange", "--export-gcode", "--output", str(gfile), str(tmf)])
        g = gfile.read_text(errors="ignore")
        grams = float(re.search(r"; total filament used \[g\] = ([\d.]+)", g).group(1))
        t = re.search(r"; estimated printing time \(normal mode\) = (.+)", g).group(1)
        mins = sum(int(n) * {"d": 1440, "h": 60, "m": 1, "s": 1 / 60}[u] for n, u in re.findall(r"(\d+)([dhms])", t))
        total_g, total_min = total_g + grams, total_min + mins
        line = f"{job:16s} {colour:34s} {t:>12s} {grams:6.1f} g"
        pts = support_points(g)
        if job == "2_front_half":
            line += "\n    " + check_front_support(pts, bed_centre(cfg))
        elif len(pts):
            line += f"   (unexpected support: {len(pts)} moves)"
        print(line)
        for w in log.splitlines():
            if re.search(r"warn|issue|floating|stability|error", w, re.I):
                print("    slicer:", w.strip())
    print(f"{'total':16s} {'':34s} {total_min / 60:9.1f} h  {total_g:6.1f} g")


if __name__ == "__main__":
    main()

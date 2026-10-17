"""Bambu Lab print jobs: sliced project files for the A1 mini and the P2S.

For each printer and job: a plain 3MF with the parts and the painted support enforcers goes through the
Bambu Studio CLI with the printer's own profiles (resolved here, since the CLI does not follow the whole
inheritance chain), plus this project's settings. The CLI writes a .gcode.3mf: a Bambu project with the
sliced plate, ready to print, that also re-opens and re-slices in Bambu Studio. The G-code is then
checked the same way as cad/slice.py: time, grams, and that support grows only where painted.

Run: .venv/Scripts/python.exe cad/slice_bambu.py   (after cad/ball.py)
"""
import copy
import json
import os
import re
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

import numpy as np
import trimesh

sys.path.insert(0, str(Path(__file__).parent))
import ball  # noqa: E402
from slice import enforced  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
OUT, DEST = ROOT / "out", ROOT / "print" / "bambu"
STUDIO = Path(os.environ.get("BAMBU_STUDIO", Path.home() / "tools" / "bambu-dl" / "extracted" / "bambu-studio.exe"))
PROFILES = STUDIO.parent / "resources" / "profiles" / "BBL"

PRINTERS = {
    "a1mini": {"machine": "Bambu Lab A1 mini 0.4 nozzle", "suffix": "A1M", "fine": "0.16mm Optimal @BBL A1M"},
    "p2s": {"machine": "Bambu Lab P2S 0.4 nozzle", "suffix": "P2S", "fine": "0.16mm Standard @BBL P2S"},
}
FILAMENT = "Bambu PLA Basic @BBL {suffix}"

COMMON = {"wall_loops": "3", "sparse_infill_density": "15%", "seam_position": "aligned",
          "elefant_foot_compensation": "0.2", "enable_support": "0", "brim_type": "no_brim"}
BRIM = {"brim_type": "outer_only", "brim_width": "5"}
# Manual support: only the painted enforcers (bosses, USB duct, pin guides), straight down from the plate.
PAINTED = {"enable_support": "1", "support_type": "normal(manual)", "support_style": "snug",
           "support_on_build_plate_only": "1"}

JOBS = {
    # job: (parts, fine layers?, overrides, colour)
    "1_test_rings":    (["test_seam_front", "test_seam_back"], False, BRIM, "black"),
    "2_front_half":    (["front_half"], False, {**BRIM, **PAINTED}, "black"),
    "3_back_half":     (["back_half"], False, BRIM, "black"),
    "4_chassis_stand": (["chassis", "stand"], False, {}, "black (chassis hidden: any)"),
    "5_eight_disc":    (["eight_disc_white"], True, {"brim_type": "outer_only", "brim_width": "3"}, "white"),
}


# --- profiles ------------------------------------------------------------------------
def profile_index():
    index = {}
    for kind in ("machine", "process", "filament"):
        for f in (PROFILES / kind).glob("*.json"):
            try:
                index[(kind, json.loads(f.read_text(encoding="utf-8"))["name"])] = f
            except (KeyError, json.JSONDecodeError):
                pass
    return index


def resolve(index, kind, name):
    """The full profile: parents first, each child overriding."""
    data = json.loads(index[(kind, name)].read_text(encoding="utf-8"))
    parent = data.get("inherits")
    full = resolve(index, kind, parent) if parent else {}
    full.update({k: v for k, v in data.items() if k != "inherits"})
    return full


def write_profile(data, path, overrides=None):
    data = copy.deepcopy(data)
    for k, v in (overrides or {}).items():
        data[k] = [v] if isinstance(data.get(k), list) else v
    data["inherits"], data["from"] = "", "system"
    path.write_text(json.dumps(data, indent=1), encoding="utf-8")
    return path


# --- input 3MF: parts + painted support enforcers --------------------------------------
def input_3mf(path, parts):
    objects, items = [], []
    for i, name in enumerate(parts, start=1):
        m = trimesh.load(str(OUT / f"{name}.stl"))
        centre = (m.bounds[0][:2] + m.bounds[1][:2]) / 2
        v = m.vertices - [centre[0], centre[1], 0]
        marks = enforced(m) if name == "front_half" else np.zeros(len(m.faces), bool)
        verts = "\n".join(f'<vertex x="{a:.5f}" y="{b:.5f}" z="{c:.5f}"/>' for a, b, c in v)
        tris = "\n".join(f'<triangle v1="{a}" v2="{b}" v3="{c}"' + (' paint_supports="4"' if k else "") + "/>"
                         for (a, b, c), k in zip(m.faces, marks))
        objects.append(f'<object id="{i}" name="{name}" type="model"><mesh><vertices>\n{verts}\n</vertices>'
                       f'<triangles>\n{tris}\n</triangles></mesh></object>')
        items.append(f'<item objectid="{i}"/>')
    model = ('<?xml version="1.0" encoding="UTF-8"?>\n<model unit="millimeter" xml:lang="en-US" '
             'xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">\n<resources>\n'
             + "\n".join(objects) + "\n</resources>\n<build>" + "".join(items) + "</build>\n</model>\n")
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


# --- checks -------------------------------------------------------------------------------
MOVE = re.compile(r"G[0-3] ")


def support_and_parts(gcode):
    """Support extrusion end points, and the bounding box of everything printed, per the G-code."""
    kind, x, y, z = None, 0.0, 0.0, 0.0
    sup, lo, hi = [], [1e9, 1e9], [-1e9, -1e9]
    for line in gcode.splitlines():
        if line.startswith("; FEATURE:"):
            kind = line[10:].strip()
        elif line.startswith("; Z_HEIGHT:"):
            z = float(line[11:])
        elif MOVE.match(line):
            mx, my, me = re.search(r"X([-\d.]+)", line), re.search(r"Y([-\d.]+)", line), re.search(r"E([\d.]+)", line)
            x0, y0 = x, y
            x = float(mx.group(1)) if mx else x
            y = float(my.group(1)) if my else y
            if me and float(me.group(1)) > 0 and (mx or my) and kind and kind not in ("Custom", "Skirt", "Brim"):
                if "upport" in kind:
                    sup += [(x0, y0, z), (x, y, z)]
                lo, hi = [min(lo[0], x), min(lo[1], y)], [max(hi[0], x), max(hi[1], y)]
    return np.array(sup).reshape(-1, 3), lo, hi


def check_front(gcode):
    """Support must stay out of the two places that matter: the twist-lock notches at the rim
    (z < socket top) and the window ring that seats the glass (above the pin guides). Where the bosses
    and the USB duct join the shell, support under them touches the inside of the wall: expected."""
    sup, lo, hi = support_and_parts(gcode)
    if not len(sup):
        return "NO SUPPORT: the painting was not picked up"
    cx, cy = (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2
    r, z = np.hypot(sup[:, 0] - cx, sup[:, 1] - cy), sup[:, 2]
    in_rim = int(((z < ball.SOCK_H + 1) & (r > ball.SOCK_R - 3)).sum())
    too_high = int((z > ball.BUTTON_Z).sum())
    ok = in_rim == 0 and too_high == 0
    return (f"support up to z={z.max():.1f} (pin guides at {ball.BUTTON_Z:.1f}), "
            f"{in_rim} points in the rim/notches, {too_high} above the pin guides" + ("" if ok else "  <-- CHECK"))


def main():
    if not STUDIO.exists():
        sys.exit(f"Bambu Studio not found at {STUDIO}: set BAMBU_STUDIO")
    index = profile_index()
    DEST.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        for printer, p in PRINTERS.items():
            out = DEST / printer
            out.mkdir(exist_ok=True)
            for old in out.glob("*.3mf"):
                old.unlink()
            machine = write_profile(resolve(index, "machine", p["machine"]), tmp / "machine.json")
            base_filament = resolve(index, "filament", FILAMENT.format(**p))
            total_g = total_min = 0.0
            print(f"== {p['machine']}")
            for job, (parts, fine, extra, colour) in JOBS.items():
                name = p["fine"] if fine else f"0.20mm Standard @BBL {p['suffix']}"
                process = write_profile(resolve(index, "process", name), tmp / "process.json", {**COMMON, **extra})
                hexcolour = "#FFFFFF" if colour == "white" else "#161616"   # preview colour only
                filament = write_profile(base_filament, tmp / "filament.json", {"default_filament_colour": hexcolour,
                                                                                "filament_colour": hexcolour})
                src = tmp / f"{job}.3mf"
                input_3mf(src, parts)
                work = tmp / f"{printer}_{job}"
                work.mkdir()
                subprocess.run([str(STUDIO), "--slice", "0", "--arrange", "1",
                                "--load-settings", f"{machine};{process}", "--load-filaments", str(filament),
                                "--outputdir", str(work), "--export-3mf", f"{job}.gcode.3mf", str(src)],
                               check=True, capture_output=True)
                result = json.loads((work / "result.json").read_text())
                if result.get("return_code") != 0:
                    sys.exit(f"{printer} {job}: {result.get('error_string')}")
                project = work / f"{job}.gcode.3mf"
                with zipfile.ZipFile(project) as z:
                    g = z.read("Metadata/plate_1.gcode").decode(errors="ignore")
                    info = z.read("Metadata/slice_info.config").decode()
                t = re.search(r"; model printing time: ([^;]+);", g).group(1).strip()
                mins = sum(int(n) * {"d": 1440, "h": 60, "m": 1, "s": 1 / 60}[u] for n, u in re.findall(r"(\d+)([dhms])", t))
                grams = float(re.search(r"; total filament weight \[g\] : ([\d.]+)", g).group(1))
                total_g, total_min = total_g + grams, total_min + mins
                line = f"  {job:16s} {colour:28s} {t:>12s} {grams:6.1f} g"
                if job == "2_front_half":
                    line += "\n      " + check_front(g)
                elif len(support_and_parts(g)[0]):
                    line += "   (unexpected support)"
                if 'outside="true"' in info:
                    line += "   PARTS OUTSIDE THE BED"
                print(line)
                project.replace(out / f"{job}.gcode.3mf")
            print(f"  {'total':16s} {'':28s} {total_min / 60:9.1f} h  {total_g:6.1f} g")


if __name__ == "__main__":
    main()

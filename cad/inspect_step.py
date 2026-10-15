"""Print the bounding boxes of the solids in a STEP file (largest first)."""
import sys
from build123d import import_step

shape = import_step(sys.argv[1])
bb = shape.bounding_box()
print(f"overall  x {bb.min.X:8.2f}..{bb.max.X:8.2f}  y {bb.min.Y:8.2f}..{bb.max.Y:8.2f}  z {bb.min.Z:8.2f}..{bb.max.Z:8.2f}")
print(f"size     {bb.size.X:.2f} x {bb.size.Y:.2f} x {bb.size.Z:.2f}")
solids = shape.solids()
print(len(solids), "solids")
rows = []
for s in solids:
    b = s.bounding_box()
    rows.append((b.size.X * b.size.Y * b.size.Z, b, getattr(s, "label", "")))
rows.sort(key=lambda r: -r[0])
for vol, b, label in rows[:45]:
    print(f"{label[:28]:28s} x {b.min.X:7.2f}..{b.max.X:7.2f}  y {b.min.Y:7.2f}..{b.max.Y:7.2f}  z {b.min.Z:6.2f}..{b.max.Z:6.2f}  ({b.size.X:.1f}x{b.size.Y:.1f}x{b.size.Z:.1f})")

"""List STEP solids whose bounding box touches a region: inspect_region.py file xmin xmax zmin zmax"""
import sys
from build123d import import_step

shape = import_step(sys.argv[1])
x0, x1, z0, z1 = map(float, sys.argv[2:6])
for s in shape.solids():
    b = s.bounding_box()
    if b.max.X < x0 or b.min.X > x1 or b.max.Z < z0 or b.min.Z > z1:
        continue
    if b.size.X > 30:
        continue
    print(f"x {b.min.X:7.2f}..{b.max.X:7.2f}  y {b.min.Y:6.2f}..{b.max.Y:6.2f}  z {b.min.Z:7.2f}..{b.max.Z:7.2f}  ({b.size.X:.2f}x{b.size.Y:.2f}x{b.size.Z:.2f})")

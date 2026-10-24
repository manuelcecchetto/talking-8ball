"""Talk to the ball over USB: logs, console commands, screenshots.

  python ballctl.py COM5 log [seconds]          print the log (default 10 s)
  python ballctl.py COM5 reset [seconds]        reboot it and print the boot log
  python ballctl.py COM5 cmd "status" [secs]    send a console command, print what follows
  python ballctl.py COM5 snap out.png           screenshot of the screen as rendered
Needs pyserial (it comes with esptool).
"""
import base64
import struct
import sys
import time
import zlib

import serial


def open_port(port):
    s = serial.Serial(None, 115200, timeout=0.2)
    s.port = port
    s.dtr = False          # set before opening: toggling them on open resets the chip
    s.rts = False
    s.open()
    return s


def read_for(s, seconds, until=None, echo=False):
    """Reads for a while; with echo, prints as it goes, so a crash report before the board resets
    (and the port disappears) is not lost."""
    end, buf = time.time() + seconds, b""
    while time.time() < end:
        try:
            chunk = s.read(65536)
        except serial.SerialException:
            if echo:
                sys.stdout.write("\n[port gone: the board reset or switched off]\n")
            break
        if chunk:
            buf += chunk
            if echo:
                sys.stdout.write(chunk.decode(errors="replace"))
                sys.stdout.flush()
            if until and until in buf:
                break
    return buf


def png(path, w, h, rgb):
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def snap(s, path):
    s.reset_input_buffer()
    s.write(b"snap\n")
    data = read_for(s, 30, until=b"SNAP-END")
    start = data.find(b"SNAP-BEGIN")
    if start < 0 or b"SNAP-END" not in data:
        sys.exit("no snapshot received:\n" + data.decode(errors="replace")[-2000:])
    header, rest = data[start:].split(b"\n", 1)
    _, w, h, stride = header.split()
    w, h, stride = int(w), int(h), int(stride)
    body = rest[:rest.find(b"SNAP-END")]
    raw = b"".join(base64.b64decode(line) for line in body.split() if line and not line.startswith(b"I ("))
    rgb = bytearray(w * h * 3)
    for y in range(h):
        for x in range(w):
            (v,) = struct.unpack_from("<H", raw, y * stride + x * 2)
            i = (y * w + x) * 3
            rgb[i] = (v >> 11) * 255 // 31
            rgb[i + 1] = ((v >> 5) & 63) * 255 // 63
            rgb[i + 2] = (v & 31) * 255 // 31
    png(path, w, h, bytes(rgb))
    print(f"saved {path} ({w}x{h})")


def main():
    port, what = sys.argv[1], sys.argv[2]
    s = open_port(port)
    if what == "log":
        read_for(s, float(sys.argv[3]) if len(sys.argv) > 3 else 10, echo=True)
    elif what == "reset":
        s.rts = True; time.sleep(0.1); s.rts = False       # EN pulse through the USB-JTAG bridge
        s.close(); time.sleep(1.5)
        s = open_port(port)
        read_for(s, float(sys.argv[3]) if len(sys.argv) > 3 else 8, echo=True)
    elif what == "cmd":
        s.reset_input_buffer()
        s.write(sys.argv[3].encode() + b"\n")
        read_for(s, float(sys.argv[4]) if len(sys.argv) > 4 else 2, echo=True)
    elif what == "snap":
        snap(s, sys.argv[3])


if __name__ == "__main__":
    main()

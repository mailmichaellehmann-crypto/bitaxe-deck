#!/usr/bin/env python3
"""Liest die Screenshots (CONFIG_DECK_SNAPSHOT) über USB und speichert sie als PNG.
Aufruf: python3 tools/snapshots.py /dev/cu.usbmodem1101 ausgabeordner [sekunden]"""
import base64, os, struct, sys, time, zlib
import serial

port, outdir = sys.argv[1], sys.argv[2]
secs = float(sys.argv[3]) if len(sys.argv) > 3 else 120
os.makedirs(outdir, exist_ok=True)

def write_png(path, w, h, rgb):
    raw = b''.join(b'\x00' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
    open(path, 'wb').write(png)

p = serial.Serial(); p.port = port; p.baudrate = 115200; p.timeout = 0.5; p.dtr = False; p.rts = False
p.open()
end = time.time() + secs
cur, rows, buf, done = None, {}, b'', 0
while time.time() < end and done < (int(sys.argv[4]) if len(sys.argv) > 4 else 99):
    buf += p.read(65536)
    *lines, buf = buf.split(b'\n')
    for ln in lines:
        ln = ln.strip()
        if ln.startswith(b'SNAP '):
            _, i, w, h = ln.split(); cur = (int(i), int(w), int(h)); rows = {}
        elif ln.startswith(b'R') and cur and b' ' in ln:
            k, data = ln[1:].split(b' ', 1)
            try: rows[int(k)] = base64.b64decode(data)
            except Exception: pass
        elif ln.startswith(b'SNAPEND') and cur:
            i, w, h = cur
            rgb = bytearray(w * h * 3)
            for y in range(h):
                r = rows.get(y, bytes(w * 2))
                for x in range(w):
                    v = (r[2 * x] << 8) | r[2 * x + 1]  # LV_COLOR_16_SWAP: Big-Endian RGB565
                    o = (y * w + x) * 3
                    rgb[o] = ((v >> 11) & 31) * 255 // 31
                    rgb[o + 1] = ((v >> 5) & 63) * 255 // 63
                    rgb[o + 2] = (v & 31) * 255 // 31
            write_png(os.path.join(outdir, f'karte_{i:02d}.png'), w, h, bytes(rgb))
            print(f'Karte {i}: {len(rows)}/{h} Zeilen gespeichert', flush=True)
            cur = None; done += 1

import struct

W = H = 32
BG = (190, 110, 30, 255)   # BGRA: RGB(30,110,190)
FG = (255, 255, 255, 255)

F = ["11110", "10000", "10000", "11110", "10000", "10000", "10000"]
G = ["01110", "10001", "10000", "10111", "10001", "10001", "01110"]

pixels = [[BG for _ in range(W)] for _ in range(H)]

# rounded corners
for y in range(H):
    for x in range(W):
        if (x < 4 and y < 4 and x + y < 4) or \
           (x >= W - 4 and y < 4 and (W - 1 - x) + y < 4) or \
           (x < 4 and y >= H - 4 and x + (H - 1 - y) < 4) or \
           (x >= W - 4 and y >= H - 4 and (W - 1 - x) + (H - 1 - y) < 4):
            pixels[y][x] = (0, 0, 0, 0)

def draw(glyph, ox, oy, scale=2):
    for gy, row in enumerate(glyph):
        for gx, ch in enumerate(row):
            if ch != '1':
                continue
            for dy in range(scale):
                for dx in range(scale):
                    x, y = ox + gx * scale + dx, oy + gy * scale + dy
                    if 0 <= x < W and 0 <= y < H:
                        pixels[y][x] = FG

draw(F, 5, 9)
draw(G, 17, 9)

xor = bytearray()
for y in range(H - 1, -1, -1):
    for x in range(W):
        b, g, r, a = pixels[y][x]
        xor += bytes((b, g, r, a))

and_mask = bytearray(W // 8 * H)

header = struct.pack('<IiiHHIIiiII', 40, W, H * 2, 1, 32, 0,
                     len(xor) + len(and_mask), 0, 0, 0, 0)
image = header + bytes(xor) + bytes(and_mask)

ico = struct.pack('<HHH', 0, 1, 1)
ico += struct.pack('<BBBBHHII', W, H, 0, 0, 1, 32, len(image), 22)
ico += image

with open('E:/learn/Workbuddy/FastGithubCpp/app.ico', 'wb') as f:
    f.write(ico)
print('wrote app.ico', len(ico))

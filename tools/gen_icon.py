#!/usr/bin/env python3
"""生成应用图标 app.ico（深色圆角底 + 播放三角 + 数据点），多尺寸。"""
from PIL import Image, ImageDraw
import os

S = 512
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)

# 圆角底：竖直渐变
top = (26, 40, 68)
bot = (10, 17, 31)
mask = Image.new("L", (S, S), 0)
md = ImageDraw.Draw(mask)
r = int(S * 0.20)
md.rounded_rectangle([0, 0, S - 1, S - 1], radius=r, fill=255)
grad = Image.new("RGB", (1, S))
for y in range(S):
    t = y / (S - 1)
    grad.putpixel((0, y), tuple(int(top[i] * (1 - t) + bot[i] * t) for i in range(3)))
grad = grad.resize((S, S))
img.paste(grad, (0, 0), mask)

# 内描边
d.rounded_rectangle([3, 3, S - 4, S - 4], radius=r - 2, outline=(120, 160, 210, 60), width=3)

# 播放三角（强调色）
cx, cy = int(S * 0.50), int(S * 0.46)
w, h = int(S * 0.30), int(S * 0.36)
tri = [(cx - w // 2, cy - h // 2), (cx - w // 2, cy + h // 2), (cx + w // 2, cy)]
d.polygon(tri, fill=(88, 227, 156, 255))
# 三角描边提亮
d.line([tri[0], tri[1]], fill=(140, 245, 190, 255), width=2)

# 右下角：压缩包意象（三条短线 + 折角）
bz_x0, bz_y0 = int(S * 0.60), int(S * 0.70)
bz_w, bz_h = int(S * 0.24), int(S * 0.19)
d.rounded_rectangle([bz_x0, bz_y0, bz_x0 + bz_w, bz_y0 + bz_h],
                    radius=int(S * 0.035), outline=(200, 220, 245, 235), width=3)
for k in range(3):
    yy = bz_y0 + int(bz_h * (0.32 + 0.18 * k))
    d.line([bz_x0 + int(bz_w * 0.12), yy, bz_x0 + int(bz_w * 0.62), yy],
           fill=(200, 220, 245, 220), width=3)

out = os.path.join(os.path.dirname(__file__), "..", "src", "win", "app.ico")
sizes = [256, 128, 64, 48, 32, 24, 16]
img.save(out, format="ICO", sizes=[(s, s) for s in sizes])
print("icon ->", os.path.abspath(out))

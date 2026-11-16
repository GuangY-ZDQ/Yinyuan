#!/usr/bin/env bash
# 生成测试素材
set -e
cd "$(dirname "$0")/.."
A=tests/assets
mkdir -p "$A"

# ---- 外壳视频 ----
# 1) 普通 H.264，moov 在尾部（ffmpeg 默认）
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "testsrc2=size=320x240:rate=25" \
       -f lavfi -i "sine=frequency=440:sample_rate=44100" -t 3 \
       -c:v libx264 -preset veryfast -pix_fmt yuv420p -c:a aac -b:a 64k \
       "$A/cover_normal.mp4"
# 2) moov 前置（+faststart，网页/流式播放常见）
ffmpeg -hide_banner -loglevel error -y -i "$A/cover_normal.mp4" -c copy -movflags +faststart "$A/cover_faststart.mp4"
# 3) 带尾部 free 盒（用 free 填充，测试已有 free 后再追加）
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "testsrc2=size=160x120:rate=15" -t 2 \
       -c:v libx264 -preset veryfast -pix_fmt yuv420p -movflags +faststart -f mp4 "$A/cover_small.mp4"
# 4) 假 mp4（不是视频）
echo "this is not a video" > "$A/not_video.mp4"

# ---- 压缩包 ----
Z=tests/assets/zipwork
rm -rf "$Z"; mkdir -p "$Z"
# 压缩包内容：文本、随机二进制、子目录、中文名、大一点的文件
python3 - "$Z" <<'PY'
import sys, os, random
root = sys.argv[1]
os.makedirs(os.path.join(root, "sub/deep"), exist_ok=True)
open(os.path.join(root, "readme.txt"), "w", encoding="utf-8").write("hello hidden pack\n" * 200)
r = random.Random(42)
with open(os.path.join(root, "sub/data.bin"), "wb") as f:
    f.write(bytes(r.getrandbits(8) for _ in range(256 * 1024)))
with open(os.path.join(root, "sub/deep/中文名 文件.txt"), "w", encoding="utf-8") as f:
    f.write("中文内容测试\n")
os.makedirs(os.path.join(root, "空目录"), exist_ok=True)
PY

# a) 普通 deflate
(cd "$Z" && zip -q -r "../pack_normal.zip" .)
# b) 仅存储
(cd "$Z" && zip -q -0 -r "../pack_store.zip" .)
# c) 带注释
(cd "$Z" && zip -q -r "../pack_comment.zip" . && zip -q -z "../pack_comment.zip" <<< "这是压缩包注释 with ascii")
# d) 加密（密码 123qwe）
(cd "$Z" && zip -q -P 123qwe -r "../pack_crypt.zip" .)
# e) 多条目（制造很多中央目录项）
python3 - "$Z" <<'PY'
import sys, os
n = os.path.join(sys.argv[1], "many"); os.makedirs(n, exist_ok=True)
for i in range(300):
    open(os.path.join(n, f"f{i:03d}.txt"), "w").write(str(i))
PY
(cd "$Z" && zip -q -r "../pack_many.zip" .)
# f) 数据描述符（流式写入，bit 3 置位）
python3 - <<'PY'
import zipfile, io, os
class NS(io.RawIOBase):
    """不可 seek 的流，迫使 zipfile 使用数据描述符"""
    def __init__(self, path): self.f = open(path, "wb")
    def writable(self): return True
    def write(self, b): return self.f.write(b)
    def close(self): self.f.close()
buf = NS("tests/assets/pack_datadesc.zip")
with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("a.txt", "x" * 5000)
    z.writestr("dir/b.txt", "y" * 1000)
buf.close()
PY
# g) ZIP64 条目（force_zip64）
python3 - <<'PY'
import zipfile
with zipfile.ZipFile("tests/assets/pack_zip64.zip", "w", zipfile.ZIP_DEFLATED) as z:
    with z.open("big.bin", "w", force_zip64=True) as f:
        f.write(b"Z" * 1024 * 1024)
    z.writestr("small.txt", "ok")
PY
# h) 空压缩包
python3 -c "import zipfile;zipfile.ZipFile('tests/assets/pack_empty.zip','w').close()"
# i) 非 zip（7z 风格头）
printf '7z\xbc\xaf\x27\x1cJUNKDATA' > tests/assets/not_zip.7z

ls -l "$A"/*.mp4 "$A"/*.zip "$A"/*.7z

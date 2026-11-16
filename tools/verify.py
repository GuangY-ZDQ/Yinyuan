#!/usr/bin/env python3
"""校验合成出来的 .mp4：既要能当 MP4 解析，也要能当 ZIP 解压。

用法： verify.py <out.mp4> <original.zip> [--pwd 密码] [--raw]
"""
import sys, os, struct, zipfile

def walk_boxes(path, raw):
    """解析顶层盒。raw=True 时允许末尾出现非盒数据（纯追加模式）。"""
    size = os.path.getsize(path)
    boxes = []
    pos = 0
    with open(path, "rb") as f:
        while pos + 8 <= size:
            f.seek(pos)
            h = f.read(8)
            s32 = struct.unpack(">I", h[:4])[0]
            t = h[4:8].decode("latin1")
            if s32 == 0:
                end = size
            elif s32 == 1:
                f.seek(pos + 8)
                s64 = struct.unpack(">Q", f.read(8))[0]
                end = pos + s64
            else:
                if s32 < 8:
                    assert raw, f"盒 {t} 长度非法 {s32}"
                    break
                end = pos + s32
            if end > size:
                assert raw, f"盒 {t} 越界 end={end} size={size}"
                break
            boxes.append((pos, t, end - pos))
            pos = end
            if s32 == 0 or end == size:
                break
    return boxes, pos, size

def main():
    out, orig = sys.argv[1], sys.argv[2]
    pwd = None
    raw = "--raw" in sys.argv
    if "--pwd" in sys.argv:
        pwd = sys.argv[sys.argv.index("--pwd") + 1].encode()

    boxes, parsed_to, size = walk_boxes(out, raw)
    types = [b[1] for b in boxes]
    assert types and types[0] == "ftyp", f"首盒不是 ftyp：{types[:3]}"
    assert "moov" in types, "缺少 moov 盒，无法播放"

    if raw:
        with open(out, "rb") as f:
            f.seek(parsed_to)
            head = f.read(4)
        assert head == b"PK\x03\x04", f"末尾追加数据不是 ZIP 本地头：{head!r}"
    else:
        assert parsed_to == size, f"盒结构未解析到文件末尾 pos={parsed_to} size={size}"
        last = boxes[-1]
        assert last[1] == "free", f"最后一个盒应为 free，实际 {last[1]}"
        zs = os.path.getsize(orig)
        # free 盒 = 盒头 + 64~256 字节随机填充 + 整个压缩包
        assert (8 + zs <= last[2] <= 8 + zs + 256) or (16 + zs <= last[2] <= 16 + zs + 256), \
            f"free 盒长度 {last[2]} 与压缩包 {zs} 不匹配"

    # ---- ZIP 侧：条目列表、CRC、内容逐一比对 ----
    with zipfile.ZipFile(out) as zo, zipfile.ZipFile(orig) as zi:
        n_out, n_in = zo.namelist(), zi.namelist()
        assert n_out == n_in, f"条目列表不一致\n out={n_out[:5]}\n in ={n_in[:5]}"
        if pwd is None:
            bad = zo.testzip()
            assert bad is None, f"CRC 校验失败：{bad}"
        for name in n_in:
            a = zo.read(name, pwd=pwd) if not name.endswith("/") else b""
            b = zi.read(name, pwd=pwd) if not name.endswith("/") else b""
            assert a == b, f"内容不一致：{name} ({len(a)} vs {len(b)} bytes)"

    print(f"OK  {os.path.basename(out)}: {size} 字节, 盒={'/'.join(types)}, "
          f"zip 条目={len(n_in)}")

if __name__ == "__main__":
    main()

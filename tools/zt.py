import sys, zipfile
try:
    z = zipfile.ZipFile(sys.argv[1])
    infos = z.infolist()
    if not infos:
        sys.exit(1)
except Exception:
    sys.exit(1)
if any(i.flag_bits & 0x1 for i in infos):   # 加密包：需要密码
    sys.exit(1)
sys.exit(0 if z.testzip() is None else 1)

#!/usr/bin/env bash
# 完整测试矩阵
cd "$(dirname "$0")/.."
B=build/test_core
A=tests/assets
O=build/t
mkdir -p "$O"
fail=0
pass=0

check() { # name out orig [pwd] [raw]
    local name=$1 out=$2 orig=$3 pwd=${4:-} rawflag=${5:-}
    local vflags=() up=() base=0 got=0

    [ -n "$pwd" ] && vflags+=(--pwd "$pwd")
    [ "$rawflag" = "raw" ] && vflags+=(--raw)

    if python3 tools/verify.py "$out" "$orig" "${vflags[@]}" >/tmp/v.log 2>&1; then
        pass=$((pass+1)); echo "PASS  $name  $(tail -1 /tmp/v.log)"
    else
        fail=$((fail+1)); echo "FAIL  $name"; sed -n '1,6p' /tmp/v.log
    fi

    # 与原始压缩包对比解压器行为（python zipfile 会按末尾 EOCD 补偿前缀，
    # 与 Bandizip / WinRAR / 7-Zip 的判定方式一致；Info-ZIP unzip 不支持前置数据）
    python3 tools/zt.py "$orig" >/dev/null 2>&1; base=$?
    python3 tools/zt.py "$out"  >/dev/null 2>&1; got=$?
    if [ "$base" -eq "$got" ]; then
        pass=$((pass+1)); echo "PASS  $name (解压校验一致 rc=$got)"
    else
        fail=$((fail+1)); echo "FAIL  $name (解压校验不一致：原始=$base 输出=$got)"
    fi
}

echo "===== 1. 各种压缩包 × 普通外壳 ====="
for z in pack_normal pack_store pack_comment pack_many pack_datadesc pack_zip64; do
    out=$O/${z}__c.mp4
    if $B $A/cover_normal.mp4 $A/$z.zip "$out" 0 > /tmp/b.log 2>&1; then
        check "$z" "$out" "$A/$z.zip"
    else fail=$((fail+1)); echo "FAIL  $z (build)"; tail -3 /tmp/b.log; fi
done

echo "===== 2. 加密压缩包 ====="
out=$O/crypt__c.mp4
if $B $A/cover_normal.mp4 $A/pack_crypt.zip "$out" 0 >/tmp/b.log 2>&1; then
    check "crypt" "$out" "$A/pack_crypt.zip" "123qwe"
else fail=$((fail+1)); echo "FAIL crypt (build)"; tail -3 /tmp/b.log; fi

echo "===== 3. 三种外壳 × 三种追加方式 ====="
for c in cover_normal cover_faststart cover_small; do
    for m in 0 1 2; do
        out=$O/${c}__m${m}.mp4
        rf=""; [ "$m" = "1" ] && rf="raw"
        if $B $A/$c.mp4 $A/pack_normal.zip "$out" $m >/tmp/b.log 2>&1; then
            check "$c/mode$m" "$out" "$A/pack_normal.zip" "" "$rf"
        else fail=$((fail+1)); echo "FAIL  $c/mode$m (build)"; tail -3 /tmp/b.log; fi
    done
done

echo "===== 4. 播放性（ffmpeg 完整解码）====="
for f in "$O"/*.mp4; do
    if ffmpeg -hide_banner -v error -i "$f" -f null - 2>/tmp/ff.log; then
        pass=$((pass+1)); echo "PASS  ffmpeg 解码 $(basename "$f")"
    else fail=$((fail+1)); echo "FAIL  ffmpeg 解码 $(basename "$f")"; head -3 /tmp/ff.log; fi
done

echo "===== 5. 错误输入必须被拒绝 ====="
neg() { # name expect_substring cmd...
    local name=$1 pat=$2; shift 2
    if "$@" >/tmp/n.log 2>&1; then
        fail=$((fail+1)); echo "FAIL  $name 应该报错但成功了"
    else
        if grep -qE "$pat" /tmp/n.log; then
            pass=$((pass+1)); echo "PASS  $name -> $(grep -aoE "\[ERR\].*" /tmp/n.log | head -1)"
        else
            fail=$((fail+1)); echo "FAIL  $name 报错信息不符"; cat /tmp/n.log
        fi
    fi
}
neg "非视频当外壳"   "不是有效的 MP4"     $B $A/not_video.mp4 $A/pack_normal.zip $O/x1.mp4 0
neg "7z 当压缩包"    "不是有效 ZIP"       $B $A/cover_normal.mp4 $A/not_zip.7z $O/x2.mp4 0
neg "压缩包不存在"   "打不开压缩包"        $B $A/cover_normal.mp4 $A/nope.zip $O/x3.mp4 0
neg "外壳不存在"     "打不开外壳视频"      $B $A/nope.mp4 $A/pack_normal.zip $O/x4.mp4 0
neg "输出不可写"     "空间不足|无法创建"   $B $A/cover_normal.mp4 $A/pack_normal.zip /proc/x.mp4 0
neg "输出目录不存在" "无法创建输出文件"    $B $A/cover_normal.mp4 $A/pack_normal.zip /nope/x.mp4 0
neg "空压缩包"       "没有任何文件"        $B $A/cover_normal.mp4 $A/pack_empty.zip $O/x5.mp4 0

echo "===== 6. 覆盖同名输出 ====="
if $B $A/cover_normal.mp4 $A/pack_normal.zip $O/over.mp4 0 >/dev/null 2>&1 &&
   $B $A/cover_small.mp4 $A/pack_normal.zip $O/over.mp4 0 >/dev/null 2>&1 &&
   python3 tools/verify.py $O/over.mp4 $A/pack_normal.zip >/tmp/v2.log 2>&1; then
    pass=$((pass+1)); echo "PASS  覆盖同名输出"
else fail=$((fail+1)); echo "FAIL  覆盖同名输出"; cat /tmp/v2.log; fi

echo "===== 7.5 分卷压缩包 ====="
SP="$O/split"
rm -rf "$SP"; mkdir -p "$SP"
# 数字序号分卷（7-Zip / 好压 风格）：切片后字节拼接 = 原包
cp "$A/pack_normal.zip" "$SP/1.ZIP"
split -b 65536 -d -a 3 "$SP/1.ZIP" "$SP/p_"
for f in "$SP"/p_*; do mv "$f" "$SP/1.ZIP.${f##*p_}"; done
rm -f "$SP/1.ZIP"
NPART=$(ls "$SP"/1.ZIP.* | wc -l)

if $B "$A/cover_normal.mp4" "$SP/1.ZIP.001" "$SP/sp_out.mp4" 0 >/tmp/sp1.log 2>&1 &&
   python3 tools/verify.py "$SP/sp_out.mp4" "$A/pack_normal.zip" >/tmp/sp1v.log 2>&1; then
    pass=$((pass+1)); echo "PASS  分卷($NPART 片, .001) 合成并可解压"
else fail=$((fail+1)); echo "FAIL  分卷合成"; cat /tmp/sp1.log /tmp/sp1v.log; fi

# 选中间某一片，产物必须与选第一片完全一致
ZSZ=$(stat -c%s "$A/pack_normal.zip")
if $B "$A/cover_normal.mp4" "$SP/1.ZIP.002" "$SP/sp_mid.mp4" 0 >/tmp/sp2.log 2>&1 &&
   [ "$(tail -c $ZSZ "$SP/sp_mid.mp4" | md5sum)" = "$(tail -c $ZSZ "$SP/sp_out.mp4" | md5sum)" ]; then
    pass=$((pass+1)); echo "PASS  任选一片结果一致"
else fail=$((fail+1)); echo "FAIL  选中间片结果不一致"; cat /tmp/sp2.log; fi

# 片号越界
if $B "$A/cover_normal.mp4" "$SP/1.ZIP.009" "$SP/sp_bad.mp4" 0 >/tmp/sp3.log 2>&1; then
    fail=$((fail+1)); echo "FAIL  越界片号应报错"
else
    if grep -qE "不在已找到的分卷范围" /tmp/sp3.log; then
        pass=$((pass+1)); echo "PASS  片号越界 -> $(grep -aoE '\[ERR\].*' /tmp/sp3.log | head -1)"
    else fail=$((fail+1)); echo "FAIL  越界片号报错信息不符"; cat /tmp/sp3.log; fi
fi

# 缺最后一片
mv "$SP/1.ZIP.00$NPART" "$SP/keep.last" 2>/dev/null || mv "$SP/1.ZIP.$(printf '%03d' $((NPART-1)))" "$SP/keep.last"
if $B "$A/cover_normal.mp4" "$SP/1.ZIP.001" "$SP/sp_miss.mp4" 0 >/tmp/sp4.log 2>&1; then
    fail=$((fail+1)); echo "FAIL  缺片应报错"
else
    if grep -qE "分卷不完整|不是完整的 ZIP" /tmp/sp4.log; then
        pass=$((pass+1)); echo "PASS  缺片 -> $(grep -aoE '\[ERR\].*' /tmp/sp4.log | head -1 | cut -c1-60)..."
    else fail=$((fail+1)); echo "FAIL  缺片报错信息不符"; cat /tmp/sp4.log; fi
fi
mv "$SP/keep.last" "$SP/1.ZIP.00$NPART" 2>/dev/null || mv "$SP/keep.last" "$SP/1.ZIP.$(printf '%03d' $((NPART-1)))"

echo "===== 8. 残留文件检查 ====="
leftover=$(ls "$O"/*.part 2>/dev/null | wc -l)
if [ "$leftover" -eq 0 ]; then
    pass=$((pass+1)); echo "PASS  没有残留 .part"
else
    fail=$((fail+1)); echo "FAIL  残留 $leftover 个 .part"
fi

echo
echo "================ 合计 PASS=$pass FAIL=$fail ================"
[ "$fail" -eq 0 ]

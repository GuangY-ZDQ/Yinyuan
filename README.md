# 隐渊 · Yinyuan

把一个 `.zip` 压缩包合成进**能正常播放的 MP4** 里。输出文件后缀是 `.mp4` 时可以正常播放；把后缀改成 `.zip`，任意解压工具都能打开。

- `.mp4` → 双击播放，**不重新编码**，壳视频一个字节都没被改动
- `.zip` → WinRAR / 7-Zip / Bandizip / Windows 资源管理器都能直接打开

> **本项目由 AI 生成。**
> 代码（`core.c`、`gui.c`）、文档、界面布局均由 AI（豆包 / Doubao，2026 年 11 月）生成；需求定义、方案取舍、真机测试与问题反馈由作者（泽渊）完成。所有功能都经过端到端测试，但不排除仍存在未覆盖的缺陷，欢迎提 issue。

> **借鉴声明。**
> 设计思路参考 [cenglin123/SteganographierGUI](https://github.com/cenglin123/SteganographierGUI)。具体借鉴了四点：用 `free` 原子承载归档而非裸追加、归档前垫 64–256 字节随机数据、保持 ZIP 内部偏移为相对值而不改写为绝对偏移、分卷文件的识别方式。本项目为 **C 语言独立实现**（该参考项目是 Python），未复制其源码。

> **用途边界。**
> 本项目用于自有文件的打包、备份与个人归档。请勿用于规避任何平台的服务条款或内容审核机制，请勿用于传播侵权内容。使用者需自行承担合规责任。

作者：**泽渊**

---

## 一、怎么用

双击 `隐渊.exe`，界面上三张卡片对应三步：

| 卡片 | 选什么 |
|---|---|
| ① 壳视频 | 一个 `.mp4`（`.mov` / `.m4v` 也可）。这就是最后能播放的那个视频 |
| ② 隐者 | 一个 `.zip`。分卷从第一片（`.zip` / `.001`）选，程序自动找齐其余分卷 |
| ③ 输出文件 | 默认是压缩包同目录下的 `原名_隐.mp4`，可改 |

点「开始合成」。完成后改后缀 `.zip` 即可解压。

**支持在压缩包区域直接拖入文件**。壳视频、压缩包、输出目录三行都认拖拽。

## 二、产物长什么样

```
[ 壳视频原始字节 ]  ← 一个字节都没改
[ free 盒头 8 字节 ]
[ 64–256 字节随机填充 ]
[ ZIP 原始字节 ]     ← 内部偏移保持相对值
```

总大小 = 壳视频 + 压缩包 + 约 264 字节。**不做二次编码，画质和音质零损失**，合成耗时基本只取决于磁盘读写速度。

## 三、能力范围

**支持**

- 单卷 ZIP；字节切片式分卷 ZIP（`.zip` + `.z01`… 或 `.001`…，自动拼接）
- ZIP64（大于 4 GB 或条目数超限）
- ZIP 前面带有附加数据（下载器加头、或其他文件粘连），自动反推并补偿偏移
- 加密 ZIP（ZipCrypto / AES）。程序不负责解密，只搬运
- 任意 `.zip` 容器格式，包括 docx / xlsx / apk
- 壳视频的 `moov` 在文件头或文件尾都可以；尾部是 `size=0` 的盒也可以

**不支持**

- 7z / RAR / TAR 作为载荷 —— 这些格式的索引不在文件末尾，改后缀后解压器定位不到，必须先转成 ZIP
- 7-Zip / WinRAR 的**原生分卷**（每片开头带 `PK\x07\x08` 盘标记）—— 不能用拼接还原，请改用字节切片式分卷
- MKV / AVI / WMV / TS 作为壳视频 —— 程序会读文件头并提示转码
- 一次合并多个 ZIP —— 请自行先打成一个包

## 四、界面的几个约定

- 浅色扁平自绘界面（无第三方 UI 库），内嵌 MiSans 字体，任何机器上显示一致
- 日志只有三行：开始合成、合成成功（路径 + 大小）、失败原因。技术细节不显示
- 输出路径跟随压缩包目录，选新压缩包时会实时更新
- 「高级选项」里的「老式拼接」：不写 `free` 盒，直接把 ZIP 接在视频末尾。默认关闭，一般不需要
- 界面底部有一行静态提示，以及不可编辑的作者信息

## 五、构建

依赖：MSYS2 的 `mingw-w64-x86_64-gcc`（含 windres）；或者一个 zig 二进制。

```bash
# 1) 生成图标（需要 Pillow）
python3 tools/gen_icon.py src/win/app.ico

# 2) 放字体（MiSans，见下方许可说明）
#    src/win/misans_regular.otf
#    src/win/misans_semibold.otf

# 3) 编译
bash tools/build.sh
# 产物：dist/隐渊.exe
```

手动等价命令：

```bash
x86_64-w64-mingw32-windres src/win/app.rc -O coff -I src/win -o build/app.res.o
x86_64-w64-mingw32-gcc -O2 -Isrc -municode -mwindows -static -static-libgcc \
    -o dist/隐渊.exe src/core.c src/win/gui.c build/app.res.o \
    -lcomctl32 -lcomdlg32 -lshell32 -luuid -lmsimg32
x86_64-w64-mingw32-strip dist/隐渊.exe
```

仓库里不含字体与图标二进制，原因见 `.gitignore` 与 `docs/构建说明.md`。

## 六、测试

```bash
bash tools/gen_assets.sh      # 生成测试素材（需要 ffmpeg）
bash tools/run_all.sh         # 63 项回归
```

引擎可以单独测试，不依赖界面：

```bash
gcc -O2 -Wall -Wextra -std=c99 -Isrc -o build/test_core src/core.c tools/test_main.c
./build/test_core tests/assets/cover_normal.mp4 tests/assets/pack_normal.zip /tmp/out.mp4
```

校验产物（等价于解压器从末尾查找 EOCD 并补偿前缀的逻辑）：

```bash
python3 tools/zt.py 产物.mp4
python3 tools/verify.py 产物.mp4 源压缩包.zip
```

已完成的测试规模：组合矩阵 58 组、边界场景 11 项、回归 63 项，全部通过。详细记录见 `docs/测试报告.md`。

## 七、已知限制

1. 产物大小 = 壳视频 + 压缩包，这个关系无法隐藏。壳视频越短、压缩包越大，平均码率就越偏离正常值。
2. 壳视频的画面与音频内容原样保留，做内容识别时会照常识别到。
3. 用 zig 交叉编译的版本依赖 UCRT（`api-ms-win-crt-*.dll`），Win10/11 自带；更老的系统建议用 mingw 编译。
4. 程序不校验压缩包是否加密。需要保护内容请自行加密（并在 7z 中勾选加密文件名）。

## 八、许可

代码采用 MIT 许可，见 `LICENSE`。

**MiSans 字体**版权归小米所有，遵循 MiSans 字体许可（可免费商用，不可单独转售字体本身）。本项目仅将其作为界面字体嵌入程序资源，字体文件本身不随仓库分发。

`docs/使用说明.txt` 是随程序分发的简版说明。

/*
 * core.c - MP4 + ZIP 双格式合成引擎（实现）
 *
 * 原理：
 *   1) 在合法 MP4 的末尾追加一个 "free" 盒（播放器会整体忽略它），ZIP 数据放在该盒内部，
 *      因此输出文件在结构上依然是一个盒结构完整的 MP4，可正常播放。
 *   2) 追加后 ZIP 的整体基准位置前移了 prefix = MP4大小 + 盒头长度，所以把
 *      - 每个中央目录条目的「本地文件头相对偏移」
 *      - EOCD 的「中央目录起始偏移」
 *      - ZIP64 EOCD 记录 / ZIP64 定位器的偏移
 *      统一 +prefix 修正。修正后这个文件从字节 0 开始看也是一份完全合法的 ZIP，
 *      任何不做事先偏移补偿的解压器（Windows 资源管理器、unzip、macOS、Python zipfile）
 *      都能直接列出/解压，而 WinRAR / 7-Zip 这类自带偏移补偿的实现同样能解。
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L   /* 让 fseeko/ftello 可见（Linux 测试用）；必须在任何系统头之前 */
#endif

#include "core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <wchar.h>
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/statvfs.h>
#endif

#define CHUNK_BYTES (1u << 20)

#define SIG_EOCD 0x06054B50u
#define SIG_CD   0x02014B50u
#define SIG_LOC  0x07064B50u  /* ZIP64 end of central directory locator */
#define SIG_Z64E 0x06064B50u  /* ZIP64 end of central directory record */

#define CD_REGION_MAX (512LL << 20)   /* 中央目录 + EOCD 区最多 512MB */
#define EOCD_TAIL_MAX 66000LL         /* 22 + 65535 + 余量 */

#ifdef _WIN32
#define FSEEK(f, o) _fseeki64((f), (o), SEEK_SET)
#define FSEEK_END(f) _fseeki64((f), 0, SEEK_END)
#define FTELL(f)    _ftelli64((f))
typedef __int64 i64;
#else
#define FSEEK(f, o) fseeko((f), (off_t)(o), SEEK_SET)
#define FSEEK_END(f) fseeko((f), 0, SEEK_END)
#define FTELL(f)    ftello((f))
typedef long long i64;
#endif

/* 取文件大小（顺带把文件指针归位到 0），失败返回 -1 */
static i64 pf_size(FILE *f)
{
    if (FSEEK_END(f) < 0) return -1;
    return FTELL(f);
}

/* ------------------------------------------------------------------ */
/* 压缩包读取层：既支持单个 zip，也支持 .001/.002… 这种"原始字节切分"分卷 */
/* ------------------------------------------------------------------ */
#define ZMAXVOL 256

static FILE *pf_open(const char *u8, int writing);
static void seterr(char *err, size_t el, const char *fmt, ...);

typedef struct {
    FILE *f[ZMAXVOL + 1];
    i64 base[ZMAXVOL + 1];   /* 每卷在逻辑合并流中的起始偏移 */
    int n;
    i64 size;                /* 合并后的逻辑总大小 */
    i64 pos;
} ZSrc;

static void zs_close(ZSrc *z)
{
    int i;
    for (i = 0; i < z->n; i++) if (z->f[i]) fclose(z->f[i]);
    z->n = 0;
    z->pos = 0;
    z->size = 0;
}

/* 拆出目录与文件名 */
static void split_path(const char *p, char *dir, size_t dn, char *base, size_t bn)
{
    const char *s1 = strrchr(p, '/');
    const char *s2 = strrchr(p, '\\');
    const char *s = NULL;
    size_t dl;
    if (s1 && s2) s = (s1 > s2) ? s1 : s2;
    else s = s1 ? s1 : s2;
    if (!s) {
        if (dn) dir[0] = 0;
        snprintf(base, bn, "%s", p);
        return;
    }
    dl = (size_t)(s - p) + 1;
    if (dl >= dn) dl = dn - 1;
    memcpy(dir, p, dl);
    dir[dl] = 0;
    snprintf(base, bn, "%s", s + 1);
}

/* 形如 name.005 结尾？返回序号位宽与数值 */
static int trailing_num(const char *base, int *width, long *val)
{
    size_t len = strlen(base), i = len;
    int w;
    while (i > 0 && base[i - 1] >= '0' && base[i - 1] <= '9') i--;
    w = (int)(len - i);
    if (w < 3 || w > 4) return 0;
    if (i == 0 || base[i - 1] != '.') return 0;
    *width = w;
    *val = atol(base + i);
    return 1;
}

/* 形如 name.z01 结尾？（Info-ZIP / WinRAR 的"跨盘式"分卷） */
static int looks_span (const char *base)
{
    size_t len = strlen(base), i = len;
    while (i > 0 && base[i - 1] >= '0' && base[i - 1] <= '9') i--;
    if (i == 0) return 0;
    if (len - i < 2) return 0;
    return (base[i - 1] == 'z' || base[i - 1] == 'Z');
}

static int path_readable(const char *path)
{
    FILE *f = pf_open(path, 0);
    if (!f) return 0;
    fclose(f);
    return 1;
}

static int ci_eq(const char *a, const char *b)
{
    while (*a && *b) {
        char x = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char y = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (x != y) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int ends_with_ci(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && ci_eq(s + a - b, suf);
}

/* 打开压缩包（自动识别并合并 .001/.002… 分卷） */
static int zs_open(const char *path, ZSrc *z, char *err, size_t el)
{
    char dir[2048], base[2048], prefix[2048], cur[4200], probe[4200];
    int width = 0, ok = 0, start = 0;
    long v = 0;

    memset(z, 0, sizeof(*z));
    split_path(path, dir, sizeof(dir), base, sizeof(base));

    if (looks_span(base)) {
        seterr(err, el, "这是 .z01/.z02 这类「跨盘式」分卷：每一片里的偏移都是按本片记录的，"
                        "直接拼起来不是合法 ZIP。请改用 7-Zip 以 .001 方式分卷，"
                        "或先解压后用 7-Zip 重新打成一个 zip");
        return -1;
    }

    if (trailing_num(base, &width, &v)) {
        size_t cut = strlen(base) - (size_t)width - 1;
        int i, k = 0;
        i64 sum = 0;
        if (cut >= sizeof(prefix)) { seterr(err, el, "路径过长"); return -1; }
        memcpy(prefix, base, cut);
        prefix[cut] = 0;

        /* 序号从 0 还是 1 开始：看 .000 是否存在 */
        snprintf(cur, sizeof(cur), "%s%s.%0*ld", dir, prefix, width, 0L);
        start = path_readable(cur) ? 0 : 1;

        for (i = start; i < start + ZMAXVOL; i++) {
            i64 sz;
            snprintf(cur, sizeof(cur), "%s%s.%0*ld", dir, prefix, width, (long)i);
            if (!path_readable(cur)) {
                int j;
                for (j = i + 1; j <= i + 3; j++) {
                    snprintf(probe, sizeof(probe), "%s%s.%0*ld", dir, prefix, width, (long)j);
                    if (path_readable(probe)) {
                        seterr(err, el, "分卷不完整：缺少第 %d 片（%s%s.%0*ld）",
                               i, dir, prefix, width, (long)i);
                        zs_close(z);
                        return -1;
                    }
                }
                break;
            }
            z->f[k] = pf_open(cur, 0);
            if (!z->f[k]) break;
            sz = pf_size(z->f[k]);
            if (sz <= 0) { fclose(z->f[k]); z->f[k] = NULL; break; }
            z->base[k] = sum;
            sum += sz;
            k++;
        }
        if (k == 0) {
            if (v != start) {
                seterr(err, el,
                        "找不到分卷的第一片「%s%s.%0*ld」：分卷必须从第 1 片开始、序号连续、"
                        "并且都在同一个文件夹里。你选的是第 %ld 片（%s%s.%0*ld），"
                        "但同目录里没有第一片，所以拼不起来。"
                        "请把这个压缩包的所有分卷都下载到同一个文件夹，然后随便选其中一片。",
                        dir, prefix, width, (long)start,
                        v, dir, prefix, width, (long)v);
            } else {
                seterr(err, el,
                        "打不开分卷的第一片「%s%s.%0*ld」：文件可能正被压缩软件或下载器占用，"
                        "或当前账号没有读取权限。请关掉占用它的程序再试。",
                        dir, prefix, width, (long)start);
            }
            return -1;
        }
        if (v < start || v > start + k - 1) {
            seterr(err, el, "第 %ld 片不在已找到的分卷范围内（共 %d 片，从 .%0*ld 开始）",
                   v, k, width, (long)start);
            zs_close(z);
            return -1;
        }
        z->n = k;
        z->size = sum;
        return 0;
    }

    /* 单个 zip：若同前缀存在 .z01，说明用户拿的是跨盘式分卷的主文件 */
    if (ends_with_ci(base, ".zip") && strlen(base) > 4) {
        snprintf(cur, sizeof(cur), "%s%.*s.z01", dir, (int)(strlen(base) - 4), base);
        if (path_readable(cur)) {
            seterr(err, el, "同目录里还有 .z01 分卷，这是「跨盘式」分卷（偏移按片记录），"
                            "直接拼起来不是合法 ZIP。请改用 7-Zip 以 .001 方式分卷，"
                            "或先解压后用 7-Zip 重新打成一个 zip");
            return -1;
        }
    }

    z->f[0] = pf_open(path, 0);
    if (!z->f[0]) { seterr(err, el, "打不开压缩包：%s", path); return -1; }
    z->n = 1;
    z->base[0] = 0;
    z->size = pf_size(z->f[0]);
    (void)ok;
    return 0;
}

static int zs_seek(ZSrc *z, i64 off)
{
    if (off < 0 || off > z->size) return -1;
    z->pos = off;
    return 0;
}

static i64 zs_part_end(const ZSrc *z, int k)
{
    return (k + 1 < z->n) ? z->base[k + 1] : z->size;
}

static size_t zs_read(ZSrc *z, void *buf, size_t n)
{
    unsigned char *p = (unsigned char *)buf;
    size_t got = 0;
    while (n > 0) {
        int k;
        i64 e;
        size_t take;
        for (k = 0; k < z->n; k++) {
            e = zs_part_end(z, k);
            if (z->pos < e) break;
        }
        if (k >= z->n) break;
        e = zs_part_end(z, k);
        take = (size_t)(e - z->pos);
        if (take > n) take = n;
        if (FSEEK(z->f[k], z->pos - z->base[k]) < 0) break;
        if (fread(p, 1, take, z->f[k]) != take) break;
        p += take;
        z->pos += (i64)take;
        got += take;
        n -= take;
    }
    return got;
}

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static void seterr(char *err, size_t el, const char *fmt, ...)
{
    va_list ap;
    if (!err || el == 0) return;
    va_start(ap, fmt);
    vsnprintf(err, el, fmt, ap);
    va_end(ap);
}

static void logf_(const CoreCtx *ctx, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    if (!ctx || !ctx->log) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ctx->log(buf, ctx->ud);
}

static void prog_(const CoreCtx *ctx, double frac, const char *stage)
{
    if (!ctx || !ctx->progress) return;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    ctx->progress(frac, stage, ctx->ud);
}

static void fmt_size(char *out, size_t n, i64 bytes)
{
    double b = (double)bytes;
    if (bytes < 1024) snprintf(out, n, "%lld B", (long long)bytes);
    else if (b < 1024.0 * 1024) snprintf(out, n, "%.1f KB", b / 1024.0);
    else if (b < 1024.0 * 1024 * 1024) snprintf(out, n, "%.2f MB", b / (1024.0 * 1024));
    else if (b < 1024.0 * 1024 * 1024 * 1024) snprintf(out, n, "%.2f GB", b / (1024.0 * 1024 * 1024));
    else snprintf(out, n, "%.2f TB", b / (1024.0 * 1024 * 1024 * 1024));
}

/* big endian (MP4) */
static uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint64_t be64(const unsigned char *p)
{
    return ((uint64_t)be32(p) << 32) | (uint64_t)be32(p + 4);
}
static void put_be32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}
static void put_be64(unsigned char *p, uint64_t v)
{
    put_be32(p, (uint32_t)(v >> 32));
    put_be32(p + 4, (uint32_t)v);
}

/* little endian (ZIP) */
static uint16_t le16(const unsigned char *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t le64(const unsigned char *p)
{
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}
static void put_le16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
}

static void put_le32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static void put_le64(unsigned char *p, uint64_t v)
{
    put_le32(p, (uint32_t)v);
    put_le32(p + 4, (uint32_t)(v >> 32));
}

/* ------------------------------------------------------------------ */
/* 平台层：路径 / 文件 / 空间 / 重命名                                  */
/* ------------------------------------------------------------------ */

#ifdef _WIN32

static wchar_t *to_wide(const char *u8)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, u8, -1, NULL, 0);
    wchar_t *w;
    if (n <= 0) return NULL;
    w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, 0, u8, -1, w, n)) { free(w); return NULL; }
    return w;
}

static FILE *pf_open(const char *u8, int writing)
{
    FILE *f = NULL;
    wchar_t *w = to_wide(u8);
    size_t len;
    if (!w) return NULL;
    len = wcslen(w);
    /* >MAX_PATH 的绝对路径需要 \\?\ 前缀 */
    if (len >= 248 && w[0] == L'\\' &&
        !(len >= 3 && w[1] == L'\\' && w[2] == L'?')) {
        wchar_t *p = (wchar_t *)malloc((len + 8) * sizeof(wchar_t));
        if (p) {
            memcpy(p, L"\\\\?\\", 4 * sizeof(wchar_t));
            memcpy(p + 4, w, (len + 1) * sizeof(wchar_t));
            f = _wfopen(p, writing ? L"wb" : L"rb");
            free(p);
        }
    }
    if (!f) f = _wfopen(w, writing ? L"wb" : L"rb");
    free(w);
    return f;
}

static int pf_free_space(const char *u8_dir, uint64_t *avail)
{
    wchar_t *w = to_wide(u8_dir);
    BOOL ok;
    unsigned __int64 a = 0, total = 0, freeb = 0;
    if (!w) return -1;
    ok = GetDiskFreeSpaceExW(w, (PULARGE_INTEGER)&a, (PULARGE_INTEGER)&total, (PULARGE_INTEGER)&freeb);
    free(w);
    if (!ok) return -1;
    *avail = (uint64_t)freeb;
    return 0;
}

static int pf_replace(const char *src_u8, const char *dst_u8)
{
    wchar_t *a = to_wide(src_u8), *b = to_wide(dst_u8);
    BOOL ok = FALSE;
    if (a && b) ok = MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    free(a); free(b);
    if (!ok) return -1;
    return 0;
}

static int pf_delete(const char *u8)
{
    wchar_t *a = to_wide(u8);
    BOOL ok;
    if (!a) return -1;
    ok = DeleteFileW(a);
    free(a);
    return ok ? 0 : -1;
}

#else /* POSIX */

static FILE *pf_open(const char *u8, int writing)
{
    return fopen(u8, writing ? "wb" : "rb");
}

static int pf_free_space(const char *u8_dir, uint64_t *avail)
{
    struct statvfs sv;
    if (statvfs(u8_dir, &sv) != 0) return -1;
    *avail = (uint64_t)sv.f_bavail * (uint64_t)sv.f_frsize;
    return 0;
}

static int pf_replace(const char *src_u8, const char *dst_u8)
{
    return rename(src_u8, dst_u8) == 0 ? 0 : -1;
}

static int pf_delete(const char *u8)
{
    return remove(u8) == 0 ? 0 : -1;
}

#endif

/* 取路径的父目录（找不到分隔符时返回 "."） */
static void parent_dir(const char *path, char *out, size_t n)
{
    const char *sep = NULL;
    size_t i;
    for (i = 0; path[i]; i++) {
        if (path[i] == '/' || path[i] == '\\') sep = path + i;
    }
    if (!sep) { snprintf(out, n, "."); return; }
    i = (size_t)(sep - path);
    if (i == 0) i = 1;                      /* "/file" -> "/" */
    if (i >= n) i = n - 1;
    memcpy(out, path, i);
    out[i] = 0;
}

/* ------------------------------------------------------------------ */
/* MP4 盒结构扫描                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    i64 file_size;
    i64 parsed_to;     /* 已完整解析到的位置 */
    int ok;            /* 是否恰好解析到文件末尾 */
    int last_size_zero;/* 最后一个盒声明 size=0（延伸到文件末尾） */
    int has_ftyp;
    int has_moov;
    i64 moov_pos;
    i64 moov_size;
    i64 box_count;
    char last_type[5];
    i64 last_pos;
    i64 last_end;
} Mp4Info;

static int mp4_scan(FILE *f, i64 size, Mp4Info *mi)
{
    i64 pos = 0;
    memset(mi, 0, sizeof(*mi));
    mi->file_size = size;
    mi->parsed_to = 0;

    while (pos + 8 <= size) {
        unsigned char h[16];
        uint32_t s32;
        i64 end;
        char t[5];
        int size_is_zero = 0;

        if (FSEEK(f, pos) < 0) { mi->parsed_to = pos; return -1; }
        if (fread(h, 1, 8, f) != 8) { mi->parsed_to = pos; return -1; }
        s32 = be32(h);
        memcpy(t, h + 4, 4);
        t[4] = 0;

        if (s32 == 0) {
            end = size;
            size_is_zero = 1;
        } else if (s32 == 1) {
            uint64_t s64;
            if (pos + 16 > size) { mi->parsed_to = pos; return -1; }
            if (fread(h + 8, 1, 8, f) != 8) { mi->parsed_to = pos; return -1; }
            s64 = be64(h + 8);
            if (s64 < 16) { mi->parsed_to = pos; return -1; }
            end = pos + (i64)s64;
        } else if (s32 < 8) {
            mi->parsed_to = pos;
            return -1;
        } else {
            end = pos + (i64)s32;
        }

        if (end > size) { mi->parsed_to = pos; return -1; }

        if (!strcmp(t, "moov")) {
            if (!mi->has_moov) { mi->has_moov = 1; mi->moov_pos = pos; mi->moov_size = end - pos; }
        }
        if (!strcmp(t, "ftyp")) mi->has_ftyp = 1;

        mi->box_count++;
        memcpy(mi->last_type, t, 5);
        mi->last_pos = pos;
        mi->last_end = end;

        pos = end;
        if (size_is_zero) { mi->last_size_zero = 1; break; }
        if (end == size) break;
    }

    mi->parsed_to = pos;
    mi->ok = (pos == size);
    return 0;
}

/* ------------------------------------------------------------------ */
/* ZIP 结构解析                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    i64 eocd_pos;      /* EOCD 在 zip 内的绝对偏移 */
    i64 cd_offset;     /* 中央目录起始（zip 内绝对偏移，已解析 ZIP64） */
    i64 cd_size;       /* 中央目录长度 */
    uint64_t entries;  /* 条目总数 */
    int zip64;         /* 是否使用 ZIP64 EOCD */
    i64 z64_pos;       /* ZIP64 EOCD 记录位置，-1 表示无 */
    i64 loc_pos;       /* ZIP64 定位器位置，-1 表示无 */
    i64 in_prefix;     /* 输入包前置数据长度：CD/记录里的偏移是相对 ZIP 起点的，
                          文件前面若垫了数据（例如把旧产物改回 .zip 再喂一次），
                          实际位置要加这个补偿量 */
} ZipMeta;

/* 在已知偏移处读取 EOCD，返回 0 成功 */
static int read_eocd_at(ZSrc *z, i64 pos, ZipMeta *zm, char *err, size_t el)
{
    unsigned char b[22];
    uint16_t disk, cd_disk, entries_disk, entries, comment;
    uint32_t cd_size, cd_off;

    if (zs_seek(z, pos) < 0 || zs_read(z, b, 22) != 22) {
        seterr(err, el, "读取 EOCD 失败");
        return -1;
    }
    if (le32(b) != SIG_EOCD) { seterr(err, el, "EOCD 签名不匹配"); return -1; }

    disk         = le16(b + 4);
    cd_disk      = le16(b + 6);
    entries_disk = le16(b + 8);
    entries      = le16(b + 10);
    cd_size      = le32(b + 12);
    cd_off       = le32(b + 16);
    comment      = le16(b + 20);

    if (disk != 0 || cd_disk != 0) {
        seterr(err, el, "这是「跨盘式」分卷（偏移按片记录），无法直接合并；"
                        "请改用 7-Zip 以 .001 方式分卷，或先解压后重新打成一个 zip");
        return -1;
    }
    (void)entries_disk;

    zm->eocd_pos = pos;
    zm->zip64 = 0;
    zm->z64_pos = -1;
    zm->loc_pos = -1;
    zm->in_prefix = 0;
    zm->entries = entries;
    zm->cd_size = (i64)cd_size;
    zm->cd_offset = (i64)cd_off;

    if (cd_size == 0xFFFFFFFFu || cd_off == 0xFFFFFFFFu || entries == 0xFFFF) {
        unsigned char loc[20], rec[56];
        i64 locp = pos - 20;
        i64 zp = -1, i;
        if (locp < 0) { seterr(err, el, "ZIP64 结构损坏（定位器缺失）"); return -1; }
        if (zs_seek(z, locp) < 0 || zs_read(z, loc, 20) != 20 || le32(loc) != SIG_LOC) {
            seterr(err, el, "ZIP64 定位器签名不匹配");
            return -1;
        }
        /* 定位器里记的是「相对 ZIP 起点」的偏移。文件前面若垫了数据（例如把旧产物
           改回 .zip 再喂一次），按记录值去读会落到错误的字节上；而 ZIP64 记录本身
           紧贴在定位器之前（记录 56 字节），所以优先按相对位置取。 */
        zp = (i64)le64(loc + 8);
        if (zp >= 0 && zs_seek(z, zp) == 0 && zs_read(z, rec, 4) == 4 && le32(rec) == SIG_Z64E) {
            /* 记录值可用 */
        } else {
            zp = -1;
            if (locp - 56 >= 0 && zs_seek(z, locp - 56) == 0 &&
                zs_read(z, rec, 4) == 4 && le32(rec) == SIG_Z64E) {
                zp = locp - 56;
            } else {
                for (i = locp - 57; i >= 0 && i > locp - 65536; i--) {
                    if (zs_seek(z, i) < 0 || zs_read(z, rec, 4) != 4) break;
                    if (le32(rec) == SIG_Z64E) { zp = i; break; }
                }
            }
            if (zp < 0) { seterr(err, el, "ZIP64 EOCD 记录签名不匹配"); return -1; }
        }
        if (zs_seek(z, zp) < 0 || zs_read(z, rec, 56) != 56 || le32(rec) != SIG_Z64E) {
            seterr(err, el, "ZIP64 EOCD 记录签名不匹配");
            return -1;
        }
        zm->zip64 = 1;
        zm->z64_pos = zp;
        zm->loc_pos = locp;
        zm->entries = le64(rec + 32);
        zm->cd_size = (i64)le64(rec + 40);
        zm->cd_offset = (i64)le64(rec + 48);
        zm->in_prefix = zp - (zm->cd_offset + zm->cd_size);
        if (zm->in_prefix < 0) { seterr(err, el, "ZIP64 中央目录位置非法"); return -1; }
    } else {
        zm->in_prefix = zm->eocd_pos - (zm->cd_offset + zm->cd_size);
        if (zm->in_prefix < 0) { seterr(err, el, "ZIP 中央目录位置非法"); return -1; }
    }

    (void)comment;
    return 0;
}

/* 从文件尾部搜索 EOCD */
static int find_eocd(ZSrc *z, i64 size, ZipMeta *zm, char *err, size_t el)
{
    i64 tail_len = size < EOCD_TAIL_MAX ? size : EOCD_TAIL_MAX;
    unsigned char *buf;
    i64 start = size - tail_len;
    i64 i;
    i64 fallback = -1;
    int rc = -1;

    if (size < 22) { seterr(err, el, "压缩包太小，不是有效的 ZIP"); return -1; }

    buf = (unsigned char *)malloc((size_t)tail_len);
    if (!buf) { seterr(err, el, "内存不足"); return -1; }
    if (zs_seek(z, start) < 0 || zs_read(z, buf, (size_t)tail_len) != (size_t)tail_len) {
        seterr(err, el, "读取压缩包尾部失败");
        free(buf);
        return -1;
    }

    for (i = tail_len - 22; i >= 0; i--) {
        if (le32(buf + i) == SIG_EOCD) {
            uint16_t clen = le16(buf + i + 20);
            if (i + 22 + (i64)clen == tail_len) {          /* 精确匹配：末尾就是 EOCD */
                fallback = start + i;
                break;
            }
            if (fallback < 0) fallback = start + i;         /* 备选：尾部有多余数据 */
        }
    }
    free(buf);

    if (fallback < 0) {
        seterr(err, el, "找不到 ZIP 的中央目录结尾记录（EOCD），这不是标准 ZIP 压缩包"
                        "（rar/7z 不支持，请先转成 .zip）");
        return -1;
    }
    if (read_eocd_at(z, fallback, zm, err, el) == 0) rc = 0;
    return rc;
}

/* ------------------------------------------------------------------ */
/* 中央目录偏移修正 / 校验                                              */
/* ------------------------------------------------------------------ */

/* patch=1 时按 adj 修正偏移；patch=0 时只校验结构 */
static int cd_walk(unsigned char *reg, i64 reg_len, i64 cd_offset_abs,
                   uint64_t entries, i64 adj, int patch,
                   i64 *out_patched, char *err, size_t el)
{
    i64 cur = 0;
    uint64_t k;

    for (k = 0; k < entries; k++) {
        uint16_t nlen, elen, clen;
        uint32_t off32;
        i64 next;

        if (cur + 46 > reg_len) {
            seterr(err, el, "中央目录损坏（第 %llu 条越界）", (unsigned long long)(k + 1));
            return -1;
        }
        if (le32(reg + cur) != SIG_CD) {
            seterr(err, el, "中央目录签名错误（第 %llu 条，偏移 %lld）",
                   (unsigned long long)(k + 1), (long long)(cd_offset_abs + cur));
            return -1;
        }
        nlen = le16(reg + cur + 28);
        elen = le16(reg + cur + 30);
        clen = le16(reg + cur + 32);
        next = cur + 46 + (i64)nlen + (i64)elen + (i64)clen;
        if (next > reg_len) {
            seterr(err, el, "中央目录损坏（第 %llu 条数据越界）", (unsigned long long)(k + 1));
            return -1;
        }

        off32 = le32(reg + cur + 42);
        if (off32 == 0xFFFFFFFFu) {
            /* ZIP64：本地头偏移在 0x0001 扩展字段里 */
            int need_u = (le32(reg + cur + 20) == 0xFFFFFFFFu);
            int need_c = (le32(reg + cur + 24) == 0xFFFFFFFFu);
            int idx = (need_u ? 1 : 0) + (need_c ? 1 : 0);
            unsigned char *ex = reg + cur + 46 + nlen;
            i64 exlen = (i64)elen, e = 0;
            int found = 0;

            while (e + 4 <= exlen) {
                uint16_t id = le16(ex + e);
                uint16_t sz = le16(ex + e + 2);
                if ((i64)e + 4 + (i64)sz > exlen) break;
                if (id == 0x0001) {
                    unsigned char *d = ex + e + 4;
                    if ((i64)sz >= (i64)(idx + 1) * 8) {
                        uint64_t v = le64(d + (i64)idx * 8);
                        if (patch) {
                            uint64_t nv = v + (uint64_t)adj;
                            if (nv < v) {
                                seterr(err, el, "偏移溢出（压缩包过大）");
                                return -1;
                            }
                            put_le64(d + (i64)idx * 8, nv);
                        }
                        found = 1;
                    }
                    break;
                }
                e += 4 + (i64)sz;
            }
            if (!found) {
                seterr(err, el, "ZIP64 扩展字段缺失（第 %llu 条），无法修正偏移",
                       (unsigned long long)(k + 1));
                return -1;
            }
        } else if (patch) {
            uint64_t v = (uint64_t)off32 + (uint64_t)adj;
            if (v > 0xFFFFFFFFu) {
                seterr(err, el, "压缩包超过 4GB 边界，此包无法用 32 位偏移伪装（请改用 ZIP64 重新压缩）");
                return -1;
            }
            put_le32(reg + cur + 42, (uint32_t)v);
        }
        if (patch) put_le16(reg + cur + 34, 0);   /* 单卷规范化：条目磁盘号一律 0 */
        cur = next;
    }

    if (out_patched) *out_patched = cur;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 主流程                                                              */
/* ------------------------------------------------------------------ */

static int copy_range(FILE *src, FILE *dst, i64 from, i64 len,
                      i64 *done_bytes, i64 total_bytes, const CoreCtx *ctx)
{
    unsigned char *buf = (unsigned char *)malloc(CHUNK_BYTES);
    i64 left = len;
    if (!buf) return -1;
    if (FSEEK(src, from) < 0) { free(buf); return -1; }
    while (left > 0) {
        size_t n = left > (i64)CHUNK_BYTES ? CHUNK_BYTES : (size_t)left;
        if (fread(buf, 1, n, src) != n) { free(buf); return -1; }
        if (fwrite(buf, 1, n, dst) != n) { free(buf); return -1; }
        left -= (i64)n;
        *done_bytes += (i64)n;
        if (total_bytes > 0) prog_(ctx, (double)*done_bytes / (double)total_bytes, "写入中");
    }
    free(buf);
    return 0;
}

/* 从多卷逻辑流复制到输出 */
static int copy_range_zs(ZSrc *z, FILE *dst, i64 from, i64 len,
                         i64 *done_bytes, i64 total_bytes, const CoreCtx *ctx)
{
    unsigned char *buf = (unsigned char *)malloc(CHUNK_BYTES);
    i64 left = len;
    if (!buf) return -1;
    if (zs_seek(z, from) < 0) { free(buf); return -1; }
    while (left > 0) {
        size_t n = left > (i64)CHUNK_BYTES ? CHUNK_BYTES : (size_t)left;
        if (zs_read(z, buf, n) != n) { free(buf); return -1; }
        if (fwrite(buf, 1, n, dst) != n) { free(buf); return -1; }
        left -= (i64)n;
        *done_bytes += (i64)n;
        if (total_bytes > 0) prog_(ctx, (double)*done_bytes / (double)total_bytes, "写入中");
    }
    free(buf);
    return 0;
}

/* 逐条校验：中央目录里记录的本地头偏移，在输出文件里确实指向 PK\x03\x04 且文件名一致 */
static int check_local_headers(FILE *f, unsigned char *reg, i64 reg_len,
                               uint64_t entries, i64 shift, char *err, size_t el,
                               uint64_t *out_checked)
{
    i64 cur = 0;
    uint64_t k;
    uint64_t check_limit = entries <= 20000 ? entries : 20000;

    for (k = 0; k < entries && k < check_limit; k++) {
        uint16_t nlen, elen, clen;
        uint32_t off32;
        unsigned char lh[30];
        i64 next;

        if (cur + 46 > reg_len) { seterr(err, el, "中央目录越界"); return -1; }
        if (le32(reg + cur) != SIG_CD) { seterr(err, el, "中央目录签名错误"); return -1; }
        nlen = le16(reg + cur + 28);
        elen = le16(reg + cur + 30);
        clen = le16(reg + cur + 32);
        next = cur + 46 + (i64)nlen + (i64)elen + (i64)clen;
        if (next > reg_len) { seterr(err, el, "中央目录越界"); return -1; }

        off32 = le32(reg + cur + 42);
        if (off32 != 0xFFFFFFFFu) {
            i64 off = (i64)off32 + shift;   /* 偏移相对 ZIP 起点，加前缀才是文件里的位置 */
            if (FSEEK(f, off) < 0 || fread(lh, 1, 30, f) != 30) {
                seterr(err, el, "第 %llu 条：读不到本地文件头（偏移 %lld）",
                       (unsigned long long)(k + 1), (long long)off);
                return -1;
            }
            if (le32(lh) != 0x04034B50u) {
                seterr(err, el, "第 %llu 条：本地文件头签名错误（偏移 %lld 处不是 PK\\x03\\x04）",
                       (unsigned long long)(k + 1), (long long)off);
                return -1;
            }
            if (le16(lh + 26) != nlen) {
                seterr(err, el, "第 %llu 条：文件名长度不一致", (unsigned long long)(k + 1));
                return -1;
            }
            {
                char name[512];
                size_t take = nlen < 500 ? nlen : 500;
                if (FSEEK(f, off + 30) < 0 || fread(name, 1, take, f) != take) {
                    seterr(err, el, "第 %llu 条：读不到文件名", (unsigned long long)(k + 1));
                    return -1;
                }
                if (nlen <= 500 && memcmp(name, reg + cur + 46, nlen) != 0) {
                    seterr(err, el, "第 %llu 条：本地头文件名与中央目录不一致",
                           (unsigned long long)(k + 1));
                    return -1;
                }
            }
        }
        cur = next;
    }
    *out_checked = check_limit;
    return 0;
}

static int verify_output(FILE *f, i64 out_size, i64 prefix, i64 zip_size, i64 pad_len,
                         int wrap_hdr, i64 cd_off_abs, i64 cd_size, uint64_t entries,
                         i64 zip_prefix, char *err, size_t el, const CoreCtx *ctx)
{
    Mp4Info mi;
    unsigned char *reg = NULL;
    i64 reg_len = cd_size, walked = 0;
    uint64_t checked = 0;

    {
        int scan_rc = mp4_scan(f, out_size, &mi);
        if (wrap_hdr > 0) {
            if (scan_rc != 0 || !mi.ok) {
                seterr(err, el, "校验失败：输出文件 MP4 盒结构不完整（未能解析到文件末尾）");
                return -1;
            }
        } else if (mi.parsed_to != prefix && !(mi.last_size_zero && mi.parsed_to == out_size)) {
            /* 纯追加模式：外壳部分应恰好解析到 prefix 处。
               例外：外壳最后一个盒声明 size=0（表示"延伸到文件末尾"），此时扫描器
               必然把后面追加的压缩包也当成这个盒的内容，parsed_to 会等于整个产物大小，
               这是合法且预期的结果。 */
            seterr(err, el, "校验失败：外壳部分应在 %lld 处结束，实际解析到 %lld",
                   (long long)prefix, (long long)mi.parsed_to);
            return -1;
        }
    }
    if (!mi.has_moov) {
        seterr(err, el, "校验失败：输出文件缺少 moov 盒，无法播放");
        return -1;
    }
    if (!mi.has_ftyp) {
        seterr(err, el, "校验失败：输出文件没有 ftyp 盒");
        return -1;
    }

    if (wrap_hdr > 0) {
        unsigned char h[16];
        uint32_t s32;
        char t[5];
        if (FSEEK(f, prefix - wrap_hdr) < 0 || fread(h, 1, 16, f) != 16) {
            seterr(err, el, "校验失败：读不到尾部盒头");
            return -1;
        }
        s32 = be32(h);
        memcpy(t, h + 4, 4);
        t[4] = 0;
        if (strcmp(t, "free") != 0) {
            seterr(err, el, "校验失败：尾部盒类型应为 free，实际为 %s", t);
            return -1;
        }
        if (wrap_hdr == 8) {
            if ((i64)s32 != 8 + pad_len + zip_size) {
                seterr(err, el, "校验失败：free 盒长度不符（%u != %lld）",
                       (unsigned)s32, (long long)(8 + zip_size));
                return -1;
            }
        } else {
            if (s32 != 1 || (i64)be64(h + 8) != 16 + pad_len + zip_size) {
                seterr(err, el, "校验失败：64 位 free 盒长度不符");
                return -1;
            }
        }
        if (mi.last_pos != prefix - wrap_hdr || mi.last_end != out_size) {
            seterr(err, el, "校验失败：free 盒位置不符");
            return -1;
        }
    }

    /* 中央目录偏移必须已经被修正到 prefix 之后 */
    if (cd_off_abs < prefix) {
        seterr(err, el, "校验失败：中央目录偏移未修正（%lld < %lld）",
               (long long)cd_off_abs, (long long)prefix);
        return -1;
    }
    if (cd_off_abs + cd_size > out_size) {
        seterr(err, el, "校验失败：中央目录越界");
        return -1;
    }

    if (reg_len > 0) {
        reg = (unsigned char *)malloc((size_t)reg_len);
        if (!reg) { seterr(err, el, "内存不足"); return -1; }
        if (FSEEK(f, cd_off_abs) < 0 || fread(reg, 1, (size_t)reg_len, f) != (size_t)reg_len) {
            seterr(err, el, "校验失败：读不到中央目录");
            free(reg);
            return -1;
        }
        if (cd_walk(reg, reg_len, cd_off_abs, entries, 0, 0, &walked, err, el) != 0) {
            free(reg);
            return -1;
        }
        if (check_local_headers(f, reg, reg_len, entries, zip_prefix, err, el, &checked) != 0) {
            free(reg);
            return -1;
        }
        free(reg);
    }

    if (ctx) {
        logf_(ctx, "校验通过：MP4 盒结构完整（%lld 个盒，含 ftyp/moov），"
                   "ZIP 偏移按相对基准写入，%llu/%llu 条本地头可定位",
               (long long)mi.box_count, (unsigned long long)checked, (unsigned long long)entries);
    }
    return 0;
}

int core_probe_mp4(const char *mp4_path, char *err, size_t errlen)
{
    FILE *f = pf_open(mp4_path, 0);
    Mp4Info mi;
    i64 size;
    if (!f) { seterr(err, errlen, "打不开视频文件"); return 0; }
    size = pf_size(f);
    if (size < 0) { fclose(f); seterr(err, errlen, "定位失败"); return 0; }
    if (size <= 8) { fclose(f); seterr(err, errlen, "视频文件为空或过小"); return 0; }
    if (mp4_scan(f, size, &mi) != 0 || !mi.ok || !mi.has_moov) {
        fclose(f);
        seterr(err, errlen, "不是结构完整的 MP4（缺少 moov 盒或盒结构有损坏）");
        return 0;
    }
    fclose(f);
    return 1;
}

/* 判断两个路径是否指向同一个文件（忽略大小写与斜杠方向） */
static int same_path(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = (*a == '/') ? '\\' : *a;
        char cb = (*b == '/') ? '\\' : *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

int core_build(const char *mp4_path, const char *zip_path, const char *out_path,
               int mode, const CoreCtx *ctx, char *err, size_t errlen)
{
    FILE *fm = NULL, *fo = NULL;
    ZSrc zs;
    int zs_parts = 0;
    i64 mp4_size = 0, zip_size = 0;
    Mp4Info mi;
    ZipMeta zm;
    unsigned char *reg = NULL;
    i64 reg_len = 0, cd_start_abs = 0, walked = 0;
    memset(&zs, 0, sizeof(zs));
    i64 wrap_hdr = 0, pad_len = 0, prefix = 0, total = 0, done = 0;
    char *tmp_out = NULL;
    char dirbuf[4096];
    char szbuf[64];
    int rc = -1;
    int need_rename = 0;

    if (err && errlen) err[0] = 0;

    if (out_path[0] && (same_path(out_path, mp4_path) || same_path(out_path, zip_path))) {
        seterr(err, errlen, "输出文件不能和输入文件用同一个名字（会覆盖原文件），请换一个输出名");
        goto done;
    }

    /* ---------- 1. 外壳视频 ---------- */
    fm = pf_open(mp4_path, 0);
    if (!fm) { seterr(err, errlen, "打不开外壳视频：%s", mp4_path); goto done; }
    mp4_size = pf_size(fm);
    if (mp4_size < 0) { seterr(err, errlen, "无法读取外壳视频"); goto done; }
    if (mp4_size <= 8) { seterr(err, errlen, "外壳视频为空或过小（%lld 字节）", (long long)mp4_size); goto done; }
    if (mp4_scan(fm, mp4_size, &mi) != 0) {
        unsigned char hdr[12];
        int n;
        FSEEK(fm, 0);
        n = (int)fread(hdr, 1, 12, fm);
        const char *kind = NULL;
        if (n >= 4 && hdr[0] == 0x1A && hdr[1] == 0x45 && hdr[2] == 0xDF && hdr[3] == 0xA3)
            kind = "这是 MKV / WebM 视频";
        else if (n >= 4 && hdr[0] == 'R' && hdr[1] == 'I' && hdr[2] == 'F' && hdr[3] == 'F')
            kind = "这是 AVI 视频";
        else if (n >= 4 && hdr[0] == 0x30 && hdr[1] == 0x26 && hdr[2] == 0xB5 && hdr[3] == 0x75)
            kind = "这是 WMV 视频";
        else if (n >= 4 && hdr[0] == 0x47 && hdr[1] == 0x40 && hdr[2] == 0x11)
            kind = "这是 TS / MPEG 视频";
        if (kind)
            seterr(err, errlen, "%s，不是 MP4/MOV：请先用 ffmpeg 或剪映之类的工具转成 .mp4 再用", kind);
        else
            seterr(err, errlen, "外壳视频不是有效的 MP4（盒结构在 %lld 处就断了）", (long long)mi.parsed_to);
        goto done;
    }
    if (!mi.ok) {
        logf_(ctx, "提示：外壳视频末尾有 %lld 字节无法解析为盒，将按纯追加处理",
               (long long)(mp4_size - mi.parsed_to));
    }
    if (!mi.has_moov) { seterr(err, errlen, "外壳视频里找不到 moov 盒，播放器无法播放它"); goto done; }
    if (!mi.has_ftyp) logf_(ctx, "提示：外壳视频没有 ftyp 盒，部分播放器可能不识别");

    fmt_size(szbuf, sizeof(szbuf), mp4_size);
    logf_(ctx, "外壳视频：%s，%lld 个盒，moov @ %lld（%s%s）",
           mp4_path, (long long)mi.box_count, (long long)mi.moov_pos, szbuf,
           mi.parsed_to == mp4_size ? "，结构完整" : "，尾部有残留");

    /* ---------- 2. 压缩包 ---------- */
    if (zs_open(zip_path, &zs, err, errlen) != 0) goto done;
    zs_parts = zs.n;
    zip_size = zs.size;
    if (zip_size < 0) { seterr(err, errlen, "无法读取压缩包"); goto done; }
    if (zs.n > 1)
        logf_(ctx, "检测到 %d 个分卷，合并后 %lld 字节", zs.n, (long long)zip_size);
    if (zip_size < 22) { seterr(err, errlen, "压缩包太小（%lld 字节），不是有效 ZIP", (long long)zip_size); goto done; }
    if (find_eocd(&zs, zip_size, &zm, err, errlen) != 0) {
        if (zs.n > 1) {
            /* 7-Zip / WinRAR 生成的分卷每片开头带 PK\x07\x08 盘标记，
               这种格式不能靠字节拼接还原 */
            unsigned char h4[4];
            int is_marked = 0;
            if (zs_seek(&zs, 0) == 0 && zs_read(&zs, h4, 4) == 4 &&
                le32(h4) == 0x08074B50u)
                is_marked = 1;
            if (is_marked)
                seterr(err, errlen,
                       "这些分卷是 7-Zip / WinRAR 的原生分卷格式（每片开头带盘标记），"
                       "不能靠拼接还原。请改用「不分割」重新压缩成一个 zip，"
                       "或用 7-Zip 的「切分为 .001 / .002」方式重新分卷");
            else
                seterr(err, errlen, "检测到 %d 个分卷，但把它们拼起来不是完整的 ZIP："
                                    "很可能缺少某一片（常见是最后一片）。"
                                    "请确认所有分卷都在同一目录、序号连续、没有被改名",
                       zs.n);
        }
        goto done;
    }
    if (zm.entries == 0) {
        seterr(err, errlen, "这个压缩包里没有任何文件（0 个条目），合成出来也解不出东西；"
                            "请先往包里添加内容再合成");
        goto done;
    }

    if (zm.cd_offset < 0 || zm.cd_offset + zm.in_prefix > zip_size ||
        zm.cd_offset + zm.in_prefix + zm.cd_size > zip_size) {
        seterr(err, errlen, "ZIP 中央目录偏移非法（offset=%lld size=%lld 文件=%lld）",
               (long long)zm.cd_offset, (long long)zm.cd_size, (long long)zip_size);
        goto done;
    }
    cd_start_abs = zm.cd_offset + zm.in_prefix;
    if (zm.in_prefix > 0)
        logf_(ctx, "提示：这个压缩包前面带有 %lld 字节附加数据，已自动补偿偏移（输出同样保留）",
               (long long)zm.in_prefix);
    reg_len = zip_size - cd_start_abs;
    if (reg_len <= 0 || reg_len > CD_REGION_MAX) {
        seterr(err, errlen, "中央目录区大小异常（%lld 字节）", (long long)reg_len);
        goto done;
    }

    reg = (unsigned char *)malloc((size_t)reg_len);
    if (!reg) { seterr(err, errlen, "内存不足（需要 %lld 字节）", (long long)reg_len); goto done; }
    if (zs_seek(&zs, cd_start_abs) < 0 || zs_read(&zs, reg, (size_t)reg_len) != (size_t)reg_len) {
        seterr(err, errlen, "读取 ZIP 中央目录失败");
        goto done;
    }

    fmt_size(szbuf, sizeof(szbuf), zip_size);
    logf_(ctx, "压缩包：%s，%s，%llu 个条目，中央目录 @ %lld（%lld 字节）%s%s",
           zip_path, szbuf, (unsigned long long)zm.entries,
           (long long)zm.cd_offset, (long long)zm.cd_size,
           zm.zip64 ? "，ZIP64" : "",
           zs_parts > 1 ? "，分卷合并" : "");

    /* ---------- 3. 决定追加方式 ---------- */
    if (mode == 1) {
        wrap_hdr = 0;
    } else if (mode == 2) {
        wrap_hdr = (8 + 256 + zip_size <= 0xFFFFFFF0LL) ? 8 : 16;
    } else {
        if (!mi.ok || mi.last_size_zero) wrap_hdr = 0;      /* 尾部本来就能吞数据 */
        else wrap_hdr = (8 + zip_size <= 0xFFFFFFF0LL) ? 8 : 16;
    }
    /* 在 free 盒头后面垫 64~256 字节随机数据：
       ① 让 ZIP 的 PK 签名不紧贴 MP4 盒链，网盘/迅雷那种「视频尾巴跟一段裸压缩包」
          特征扫描不容易命中；
       ② 每次生成的产物哈希不同，避免同哈希被牵连。
       解压器从文件末尾找 EOCD，前面的垃圾多少都不影响。 */
    if (wrap_hdr) {
        static int pad_seeded;
        if (!pad_seeded) { srand((unsigned)time(NULL)); pad_seeded = 1; }
        pad_len = 64 + (i64)(rand() % 193);
    }
    prefix = mp4_size + wrap_hdr;
    if (prefix + pad_len + zip_size <= 0) { seterr(err, errlen, "文件总大小溢出"); goto done; }
    total = prefix + pad_len + zip_size;

    if (wrap_hdr) {
        logf_(ctx, "追加方式：在末尾写入 %lld 字节的 free 盒，ZIP 数据装进盒内", (long long)wrap_hdr);
    } else {
        logf_(ctx, "追加方式：纯尾部追加（不写盒头）");
    }
    logf_(ctx, "ZIP 偏移保持相对（外壳 %lld + 盒头 %lld = %lld），解压器按前缀自动补偿",
           (long long)mp4_size, (long long)wrap_hdr, (long long)prefix);

    /* ---------- 4. 校验中央目录；偏移保持「相对 ZIP 起点」不动 ----------
       成品工具都是裸拼接、不改偏移，解压器（Bandizip/WinRAR/7-Zip）按末尾
       EOCD 自己算出前缀再补偿。改成绝对值反而会让 Bandizip 在大前缀时误判分卷。 */
    if (cd_walk(reg, reg_len, cd_start_abs, zm.entries, 0, 1, &walked, err, errlen) != 0) goto done;

    {
        i64 eocd_idx = zm.eocd_pos - cd_start_abs;
        if (eocd_idx < 0 || eocd_idx + 22 > reg_len) {
            seterr(err, errlen, "EOCD 位置异常");
            goto done;
        }
        /* cd_off 保持原值（相对 ZIP 起点），不做 +prefix */
        /* 单卷规范化：卷号一律 0、两处条目数写一致。
           有些包（尤其是从分卷集里取出来重新打包的）会留下非 0 的卷号，
           Bandizip 看到就会误判成「分卷包，还缺 .z01/.z02」。 */
        if (le16(reg + eocd_idx + 4) || le16(reg + eocd_idx + 6) ||
            (!zm.zip64 && le16(reg + eocd_idx + 8) != le16(reg + eocd_idx + 10)))
            logf_(ctx, "提示：这个压缩包带着分卷标记，已在输出里规范化为单卷");
        put_le16(reg + eocd_idx + 4, 0);
        put_le16(reg + eocd_idx + 6, 0);
        if (!zm.zip64) {
            put_le16(reg + eocd_idx + 8, (uint16_t)zm.entries);
            put_le16(reg + eocd_idx + 10, (uint16_t)zm.entries);
        }
    }
    if (zm.zip64) {
        i64 z64_idx = zm.z64_pos - cd_start_abs;
        i64 loc_idx = zm.loc_pos - cd_start_abs;
        if (z64_idx < 0 || z64_idx + 56 > reg_len || loc_idx < 0 || loc_idx + 20 > reg_len) {
            seterr(err, errlen, "ZIP64 记录位置异常");
            goto done;
        }
        /* ZIP64 记录的 cd 偏移同样保持原值 */
        put_le32(reg + z64_idx + 16, 0);                 /* 本卷号 */
        put_le32(reg + z64_idx + 20, 0);                 /* 中央目录所在卷号 */
        put_le64(reg + z64_idx + 24, zm.entries);        /* 本卷条目数 = 总条目数 */
        put_le32(reg + loc_idx + 4, 0);                  /* 定位器里的卷号 */
        put_le32(reg + loc_idx + 16, 1);                 /* 总卷数 = 1 */
        if (zm.entries >= 0xFFFFFFFFu) {
            seterr(err, errlen, "条目数超限，不支持");
            goto done;
        }
    }

    /* ---------- 5. 空间检查 ---------- */
    if (out_path[0]) {
        uint64_t avail = 0;
        parent_dir(out_path, dirbuf, sizeof(dirbuf));
        if (pf_free_space(dirbuf, &avail) == 0) {
            if (avail < (uint64_t)total + (1u << 20)) {
                char a[64], b[64];
                fmt_size(a, sizeof(a), (i64)avail);
                fmt_size(b, sizeof(b), total);
                seterr(err, errlen, "目标磁盘空间不足：需要 %s，可用 %s", b, a);
                goto done;
            }
        }
    }

    /* ---------- 6. 写入 ---------- */
    tmp_out = (char *)malloc(strlen(out_path) + 8);
    if (!tmp_out) { seterr(err, errlen, "内存不足"); goto done; }
    sprintf(tmp_out, "%s.part", out_path);

    fo = pf_open(tmp_out, 1);
    if (!fo) { seterr(err, errlen, "无法创建输出文件（路径无权限或磁盘已满）：%s", tmp_out); goto done; }

    logf_(ctx, "开始写入…");
    prog_(ctx, 0.0, "写入中");

    if (copy_range(fm, fo, 0, mp4_size, &done, total, ctx) != 0) {
        seterr(err, errlen, "复制外壳视频失败（磁盘已满或源文件读取错误）");
        goto done;
    }
    if (wrap_hdr == 8) {
        unsigned char h[8];
        put_be32(h, (uint32_t)(8 + pad_len + zip_size));
        memcpy(h + 4, "free", 4);
        if (fwrite(h, 1, 8, fo) != 8) { seterr(err, errlen, "写入盒头失败"); goto done; }
        done += 8;
    } else if (wrap_hdr == 16) {
        unsigned char h[16];
        put_be32(h, 1);
        memcpy(h + 4, "free", 4);
        put_be64(h + 8, (uint64_t)(16 + pad_len + zip_size));
        if (fwrite(h, 1, 16, fo) != 16) { seterr(err, errlen, "写入盒头失败"); goto done; }
        done += 16;
    }
    if (pad_len > 0) {
        unsigned char pd[256];
        int i;
        for (i = 0; i < (int)pad_len; i++) pd[i] = (unsigned char)(rand() & 0xFF);
        if (fwrite(pd, 1, (size_t)pad_len, fo) != (size_t)pad_len) {
            seterr(err, errlen, "写入填充数据失败");
            goto done;
        }
        done += pad_len;
    }
    if (copy_range_zs(&zs, fo, 0, cd_start_abs, &done, total, ctx) != 0) {
        seterr(err, errlen, "复制压缩包正文失败（磁盘已满，或源文件读取错误）");
        goto done;
    }
    if (fwrite(reg, 1, (size_t)reg_len, fo) != (size_t)reg_len) {
        seterr(err, errlen, "写入中央目录失败");
        goto done;
    }
    done += reg_len;
    prog_(ctx, 1.0, "写入完成");

    if (fflush(fo) != 0) {
        seterr(err, errlen, "写入磁盘失败：可能是磁盘已满，或目标磁盘是 FAT32（单文件最大 4GB）");
        goto done;
    }
    fclose(fo);
    fo = NULL;

    /* ---------- 7. 回读校验 ---------- */
    {
        FILE *fv = pf_open(tmp_out, 0);
        i64 vsize;
        if (!fv) { seterr(err, errlen, "校验时打不开输出文件"); goto done; }
        vsize = pf_size(fv);
        if (vsize < 0) { fclose(fv); seterr(err, errlen, "校验定位失败"); goto done; }
        if (vsize != total) {
            fclose(fv);
            seterr(err, errlen, "输出大小异常：%lld，期望 %lld", (long long)vsize, (long long)total);
            goto done;
        }
        /* 先确认输出里的中央目录偏移已是修正后的值 */
        {
            ZipMeta vz;
            ZSrc zv;
            memset(&zv, 0, sizeof(zv));
            zv.f[0] = fv;
            zv.base[0] = 0;
            zv.n = 1;
            zv.size = vsize;
            if (find_eocd(&zv, vsize, &vz, err, errlen) != 0) { fclose(fv); goto done; }
            if (verify_output(fv, vsize, prefix, zip_size, pad_len, wrap_hdr,
                              vz.cd_offset + vz.in_prefix, vz.cd_size, vz.entries,
                              vz.in_prefix, err, errlen, ctx) != 0) {
                fclose(fv);
                goto done;
            }
        }
        fclose(fv);
    }

    /* ---------- 8. 落盘 ---------- */
    if (pf_replace(tmp_out, out_path) != 0) {
        seterr(err, errlen, "无法保存为最终文件（可能被占用）：%s", out_path);
        goto done;
    }
    need_rename = 1;

    fmt_size(szbuf, sizeof(szbuf), total);
    rc = 0;

done:
    if (fm) fclose(fm);
    zs_close(&zs);
    if (fo) fclose(fo);
    free(reg);
    if (rc != 0 && tmp_out && !need_rename) {
        pf_delete(tmp_out);
    }
    free(tmp_out);
    return rc;
}

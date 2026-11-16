/* gui.c —— 隐渊 · Win32 界面 + 命令行入口
 *
 * 设计要点：
 *  - 纯 Win32/GDI 自绘，无第三方 UI 库，单文件 exe 绿色运行
 *  - 卡片式浅色扁平布局，圆角与描边带 1px 抗锯齿
 *  - 所有可编辑内容仍是原生 EDIT（输入法、选中、粘贴全部正常）
 *  - 主界面不出现技术术语；「老式拼接」开关收进高级选项
 */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <wchar.h>

/* WM_DPICHANGED 在某些 SDK 版本里没有定义，这里兜底 */
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

#include "core.h"

#define MAXP      32768
#define KNAME     L"隐渊"
#define APPVER    L"1.0.0"

#define D(v)      MulDiv((v), g_dpi, 96)

/* ---------- 控件 ID ---------- */
#define ID_MP4_EDIT   1001
#define ID_ZIP_EDIT   1002
#define ID_OUT_EDIT   1003
#define ID_MP4_BTN    1004
#define ID_ZIP_BTN    1005
#define ID_OUT_BTN    1006
#define ID_RUN_BTN    1007
#define ID_OPEN_DIR   1008
#define ID_ADV_LINK   1009
#define ID_LEGACY     1010
#define ID_LOG        1011

#define WM_APP_LOG     (WM_APP + 1)
#define WM_APP_PROG2   (WM_APP + 2)
#define WM_APP_DONE    (WM_APP + 3)

/* ---------- 配色（浅色现代） ---------- */
#define C_BG        RGB(0xF3, 0xF4, 0xF7)
#define C_CARD      RGB(0xFF, 0xFF, 0xFF)
#define C_BORDER    RGB(0xE2, 0xE5, 0xEA)
#define C_BORDER_HI RGB(0xC2, 0xCB, 0xD8)
#define C_TEXT      RGB(0x1D, 0x21, 0x29)
#define C_MUTED     RGB(0x86, 0x90, 0x9C)
#define C_ACCENT    RGB(0x2B, 0x6C, 0xF6)
#define C_ACCENT_H  RGB(0x1E, 0x5B, 0xE0)
#define C_ACCENT_P  RGB(0x18, 0x4C, 0xC2)
#define C_TRACK     RGB(0xE8, 0xEA, 0xEE)
#define C_LOG_BG    RGB(0xFB, 0xFB, 0xFD)
#define C_OK        RGB(0x16, 0xA3, 0x4A)
#define C_ERR       RGB(0xE0, 0x3A, 0x2E)
#define C_DISABLED  RGB(0xF2, 0xF3, 0xF5)

/* ---------- 字体 ---------- */
static int   g_misans_ok;              /* 内嵌 MiSans 是否可用 */
static HFONT g_f_title, g_f_sub, g_f_label, g_f_hint, g_f_edit, g_f_btn, g_f_log, g_f_foot;
static int   g_dpi = 96;

/* ---------- 窗口与控件 ---------- */
static HWND  g_hwnd;
static HWND  g_ed_mp4, g_ed_zip, g_ed_out;
static HWND  g_bt_mp4, g_bt_zip, g_bt_out, g_bt_run, g_bt_open, g_bt_adv, g_chk_legacy;
static HWND  g_log;
static HBRUSH g_br_bg, g_br_card, g_br_log;

/* ---------- 状态 ---------- */
static wchar_t g_mp4[MAXP], g_zip[MAXP], g_out[MAXP];
static wchar_t g_out_auto[MAXP];   /* 上一次自动填入的输出路径；内容与它相同说明用户没改过 */
static int   g_busy;
static int   g_pct;
static int   g_adv_open;
static int   g_legacy;
static wchar_t g_status[512];
static wchar_t g_logbuf[65536];
static int   g_done_flash;              /* 0 无，1 成功，2 失败 */

/* ---------- 布局 ---------- */
typedef struct {
    RECT card[3], edit[3], browse[3];
    RECT run, open, link, adv, chk, prog, pct, log, status, author, title, sub, divider, hint;
    int  card_h, pad;
} Lay;
static Lay g_lay;

/* 按钮悬停状态（供自绘用）：0..4 依次为 浏览1/浏览2/另存为/开始合成/打开位置 */
static int g_hover[6];
static HWND g_btnh[6];

static void rebuild_tabring(void);
static void focus_next(HWND from, int back);

/* ================================================================== */
/* 基础工具                                                            */
/* ================================================================== */

static wchar_t *u8_to_w(const char *s)
{
    int n = (int)MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w;
    if (n <= 0) return NULL;
    w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static char *w_to_u8(const wchar_t *w)
{
    int n = (int)WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s;
    if (n <= 0) return NULL;
    s = (char *)malloc((size_t)n);
    if (!s) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

static int file_exists(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return (a != INVALID_FILE_ATTRIBUTES) && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int ends_with_i(const wchar_t *s, const wchar_t *suf)
{
    size_t a = wcslen(s), b = wcslen(suf);
    return a >= b && _wcsicmp(s + a - b, suf) == 0;
}

static void fmt_size_w(wchar_t *out, size_t n, unsigned long long bytes)
{
    if (bytes < 1024) _snwprintf(out, n - 1, L"%llu 字节", bytes);
    else if (bytes < 1024ULL * 1024) _snwprintf(out, n - 1, L"%.1f KB", (double)bytes / 1024.0);
    else if (bytes < 1024ULL * 1024 * 1024) _snwprintf(out, n - 1, L"%.1f MB", (double)bytes / 1048576.0);
    else _snwprintf(out, n - 1, L"%.2f GB", (double)bytes / 1073741824.0);
}

/* 去首尾空白、去掉成对引号（应对"复制为路径"） */
static void sanitize_path(wchar_t *s)
{
    size_t n, i;
    while (s[0] == L' ' || s[0] == L'\t') memmove(s, s + 1, (wcslen(s) + 1) * sizeof(wchar_t));
    n = wcslen(s);
    while (n > 0 && (s[n - 1] == L' ' || s[n - 1] == L'\t' || s[n - 1] == L'\r' || s[n - 1] == L'\n'))
        s[--n] = 0;
    if (n >= 2 && s[0] == L'"') {
        for (i = 0; i + 1 < n; i++) s[i] = s[i + 1];
        n--;
        s[n - 1] = 0;
    }
}

/* 1=视频 2=压缩包(含分卷) 0=其它 */
static int classify_name(const wchar_t *p)
{
    const wchar_t *dot = wcsrchr(p, L'.');
    if (!dot) return 0;
    if (!_wcsicmp(dot, L".mp4") || !_wcsicmp(dot, L".mov") || !_wcsicmp(dot, L".m4v")) return 1;
    if (!_wcsicmp(dot, L".zip")) return 2;
    {
        const wchar_t *e = dot + 1;
        int len = (int)wcslen(e), i, digits = 1, z = 0;
        if (len < 3 || len > 4) return 0;
        if (e[0] == L'z' || e[0] == L'Z') { z = 1; }
        for (i = z; i < len; i++) if (e[i] < L'0' || e[i] > L'9') { digits = 0; break; }
        if (digits) return 2;             /* .001 / .z01 / .zip 之外的序号片 */
    }
    return 0;
}

/* ================================================================== */
/* 绘图基础                                                            */
/* ================================================================== */

static COLORREF mixc(COLORREF a, COLORREF b, double t)   /* t=1 → b */
{
    int r = (int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t);
    int g = (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t);
    int bl = (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t);
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (bl < 0) bl = 0;
    if (bl > 255) bl = 255;
    return RGB(r, g, bl);
}

/* 抗锯齿圆角填充：逐行计算内缩量，边缘像素按覆盖率混色 */
static void rrect(HDC dc, int x, int y, int w, int h, int r, COLORREF fill, COLORREF bg)
{
    int row;
    HBRUSH br;
    if (w <= 0 || h <= 0) return;
    if (r > h / 2) r = h / 2;
    if (r <= 0) {
        RECT q; SetRect(&q, x, y, x + w, y + h);
        br = CreateSolidBrush(fill);
        FillRect(dc, &q, br);
        DeleteObject(br);
        return;
    }
    br = CreateSolidBrush(fill);
    for (row = 0; row < h; row++) {
        double fy = (double)row + 0.5, inset = 0.0;
        int s, l, rr;
        double cov;
        RECT span;
        if (fy < (double)r) {
            double dy = (double)r - fy;
            inset = (double)r - sqrt((double)r * (double)r - dy * dy);
        } else if (fy > (double)h - (double)r) {
            double dy = fy - ((double)h - (double)r);
            inset = (double)r - sqrt((double)r * (double)r - dy * dy);
        }
        if (inset < 0.0) inset = 0.0;
        s = (int)floor(inset);
        cov = 1.0 - (inset - (double)s);
        l = x + s;
        rr = x + w - s - 1;
        if (rr < l) continue;

        /* 左右边缘像素：按覆盖率与背景混色 */
        if (cov < 1.0) {
            SetPixelV(dc, l, y + row, mixc(bg, fill, cov));
            SetPixelV(dc, rr, y + row, mixc(bg, fill, cov));
            l++;
            rr--;
        }
        if (rr >= l) {
            SetRect(&span, l, y + row, rr + 1, y + row + 1);
            FillRect(dc, &span, br);
        }
    }
    DeleteObject(br);
}

/* 圆角卡片：外描边 + 内填充（两层，边缘都带 AA） */
static void card(HDC dc, int x, int y, int w, int h, COLORREF bg, COLORREF border, COLORREF fill)
{
    rrect(dc, x, y, w, h, D(10), border, bg);
    rrect(dc, x + 1, y + 1, w - 2, h - 2, D(9), fill, border);
}

static void txt(HDC dc, const wchar_t *s, HFONT f, COLORREF c, int x, int y, int w, int h, UINT fmt)
{
    RECT r;
    SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    SetRect(&r, x, y, x + w, y + h);
    DrawTextW(dc, s, -1, &r, fmt | DT_NOPREFIX);
}

/* ================================================================== */
/* 字体                                                                */
/* ================================================================== */

/* 把 exe 里的 MiSans 子集注册进当前进程（不落盘、不装系统） */
static void load_embedded_fonts(HINSTANCE hi)
{
    static const int ids[2] = { 1011, 1012 };
    int i, ok = 0;
    for (i = 0; i < 2; i++) {
        HRSRC r = FindResourceW(hi, MAKEINTRESOURCEW(ids[i]), RT_RCDATA);
        HGLOBAL g;
        void *p;
        DWORD n, cnt = 0;
        if (!r) continue;
        n = SizeofResource(hi, r);
        g = LoadResource(hi, r);
        if (!g || !n) continue;
        p = LockResource(g);
        if (!p) continue;
        if (AddFontMemResourceEx(p, n, NULL, &cnt) && cnt > 0) ok++;
    }
    g_misans_ok = ok;
}

static HFONT make_font(int pt, int weight, const wchar_t *face)
{
    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -D(pt);
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcsncpy(lf.lfFaceName, face, 31);
    return CreateFontIndirectW(&lf);
}

static void free_fonts(void)
{
    HFONT *all[] = { &g_f_title, &g_f_sub, &g_f_label, &g_f_hint,
                     &g_f_edit, &g_f_btn, &g_f_log, &g_f_foot };
    size_t i;
    for (i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        if (*all[i]) { DeleteObject(*all[i]); *all[i] = NULL; }
    }
}

static void create_fonts(void)
{
    const wchar_t *ui = g_misans_ok ? L"MiSans UI" : L"Microsoft YaHei UI";
    const wchar_t *mono = g_misans_ok ? L"MiSans UI" : L"Microsoft YaHei UI";
    free_fonts();
    g_f_title = make_font(30, FW_SEMIBOLD, ui);
    g_f_sub   = make_font(13, FW_NORMAL, ui);
    g_f_label = make_font(15, FW_SEMIBOLD, ui);
    g_f_hint  = make_font(12, FW_NORMAL, ui);
    g_f_edit  = make_font(15, FW_NORMAL, ui);
    g_f_btn   = make_font(14, FW_SEMIBOLD, ui);
    g_f_log   = make_font(13, FW_NORMAL, mono);
    g_f_foot  = make_font(12, FW_NORMAL, ui);
}

static void apply_fonts(void)
{
    if (g_ed_mp4) SendMessageW(g_ed_mp4, WM_SETFONT, (WPARAM)g_f_edit, TRUE);
    if (g_ed_zip) SendMessageW(g_ed_zip, WM_SETFONT, (WPARAM)g_f_edit, TRUE);
    if (g_ed_out) SendMessageW(g_ed_out, WM_SETFONT, (WPARAM)g_f_edit, TRUE);
    if (g_bt_mp4) SendMessageW(g_bt_mp4, WM_SETFONT, (WPARAM)g_f_btn, TRUE);
    if (g_bt_zip) SendMessageW(g_bt_zip, WM_SETFONT, (WPARAM)g_f_btn, TRUE);
    if (g_bt_out) SendMessageW(g_bt_out, WM_SETFONT, (WPARAM)g_f_btn, TRUE);
    if (g_bt_run) SendMessageW(g_bt_run, WM_SETFONT, (WPARAM)g_f_btn, TRUE);
    if (g_bt_open) SendMessageW(g_bt_open, WM_SETFONT, (WPARAM)g_f_btn, TRUE);
    if (g_bt_adv) SendMessageW(g_bt_adv, WM_SETFONT, (WPARAM)g_f_foot, TRUE);
    if (g_chk_legacy) SendMessageW(g_chk_legacy, WM_SETFONT, (WPARAM)g_f_edit, TRUE);
    if (g_log) SendMessageW(g_log, WM_SETFONT, (WPARAM)g_f_log, TRUE);
}

/* ================================================================== */
/* 布局                                                                */
/* ================================================================== */

static void layout_ui(void)
{
    RECT rc;
    int W, H, pad, ch, i;
    GetClientRect(g_hwnd, &rc);
    W = rc.right; H = rc.bottom;
    pad = D(26);
    memset(&g_lay, 0, sizeof(g_lay));
    g_lay.pad = pad;
    g_lay.card_h = D(92);
    ch = g_lay.card_h;

    g_lay.title.top = D(16);    g_lay.title.left = pad; g_lay.title.right = W - pad; g_lay.title.bottom = D(54);
    g_lay.sub.top = D(52);      g_lay.sub.left = pad;   g_lay.sub.right = W - pad;   g_lay.sub.bottom = D(76);

    for (i = 0; i < 3; i++) {
        int y = D(84) + i * (ch + D(12));
        g_lay.card[i].left = pad;
        g_lay.card[i].top = y;
        g_lay.card[i].right = W - pad;
        g_lay.card[i].bottom = y + ch;

        g_lay.edit[i].left = pad + D(16);
        g_lay.edit[i].top = y + D(46);
        g_lay.edit[i].bottom = y + D(46) + D(36);
        g_lay.browse[i].right = W - pad - D(16);
        g_lay.browse[i].left = g_lay.browse[i].right - D(88);
        g_lay.browse[i].top = g_lay.edit[i].top;
        g_lay.browse[i].bottom = g_lay.edit[i].bottom;
        g_lay.edit[i].right = g_lay.browse[i].left - D(10);
    }

    {
        int y = g_lay.card[2].bottom + D(16);
        g_lay.run.left = pad;  g_lay.run.top = y; g_lay.run.right = pad + D(156); g_lay.run.bottom = y + D(44);
        g_lay.open.left = g_lay.run.right + D(12); g_lay.open.top = y;
        g_lay.open.right = g_lay.open.left + D(176); g_lay.open.bottom = y + D(44);
        g_lay.link.right = W - pad; g_lay.link.left = g_lay.link.right - D(116);
        g_lay.link.top = y; g_lay.link.bottom = y + D(44);
        g_lay.adv.left = pad; g_lay.adv.right = W - pad;
        g_lay.adv.top = y + D(44) + D(12);
        g_lay.adv.bottom = g_lay.adv.top + D(82);
        g_lay.chk.left = pad + D(18); g_lay.chk.top = g_lay.adv.top + D(12);
        g_lay.chk.right = g_lay.chk.left + D(600); g_lay.chk.bottom = g_lay.chk.top + D(28);
    }

    {
        int py = (g_adv_open ? g_lay.adv.bottom : g_lay.run.bottom) + D(20);
        g_lay.prog.left = pad; g_lay.prog.top = py; g_lay.prog.right = W - pad - D(60);
        g_lay.prog.bottom = py + D(6);
        g_lay.pct.left = W - pad - D(56); g_lay.pct.top = py - D(8);
        g_lay.pct.right = W - pad; g_lay.pct.bottom = py + D(14);

        g_lay.log.left = pad; g_lay.log.top = py + D(26);
        g_lay.log.right = W - pad; g_lay.log.bottom = H - D(88);
        if (g_lay.log.bottom < g_lay.log.top + D(70)) g_lay.log.bottom = g_lay.log.top + D(70);

        /* 日志区下面留一条灰字提示 */
        g_lay.hint.left = pad + D(2); g_lay.hint.right = W - pad;
        g_lay.hint.top = g_lay.log.bottom + D(8);
        g_lay.hint.bottom = g_lay.hint.top + D(18);

        g_lay.divider.left = pad; g_lay.divider.right = W - pad;
        g_lay.divider.top = H - D(38); g_lay.divider.bottom = H - D(37);
        g_lay.status.left = pad; g_lay.status.top = H - D(34);
        g_lay.status.right = W - pad - D(130); g_lay.status.bottom = H - D(10);
        g_lay.author.right = W - pad; g_lay.author.left = g_lay.author.right - D(120);
        g_lay.author.top = H - D(34); g_lay.author.bottom = H - D(10);
    }

    MoveWindow(g_ed_mp4, g_lay.edit[0].left, g_lay.edit[0].top,
               g_lay.edit[0].right - g_lay.edit[0].left, g_lay.edit[0].bottom - g_lay.edit[0].top, TRUE);
    MoveWindow(g_ed_zip, g_lay.edit[1].left, g_lay.edit[1].top,
               g_lay.edit[1].right - g_lay.edit[1].left, g_lay.edit[1].bottom - g_lay.edit[1].top, TRUE);
    MoveWindow(g_ed_out, g_lay.edit[2].left, g_lay.edit[2].top,
               g_lay.edit[2].right - g_lay.edit[2].left, g_lay.edit[2].bottom - g_lay.edit[2].top, TRUE);
    for (i = 0; i < 3; i++)
        MoveWindow(g_btnh[i], g_lay.browse[i].left, g_lay.browse[i].top,
                   g_lay.browse[i].right - g_lay.browse[i].left,
                   g_lay.browse[i].bottom - g_lay.browse[i].top, TRUE);
    MoveWindow(g_bt_run, g_lay.run.left, g_lay.run.top,
               g_lay.run.right - g_lay.run.left, g_lay.run.bottom - g_lay.run.top, TRUE);
    MoveWindow(g_bt_open, g_lay.open.left, g_lay.open.top,
               g_lay.open.right - g_lay.open.left, g_lay.open.bottom - g_lay.open.top, TRUE);
    MoveWindow(g_bt_adv, g_lay.link.left, g_lay.link.top,
               g_lay.link.right - g_lay.link.left, g_lay.link.bottom - g_lay.link.top, TRUE);
    if (g_adv_open) {
        ShowWindow(g_bt_adv, SW_SHOW);
        ShowWindow(g_chk_legacy, SW_SHOW);
        MoveWindow(g_chk_legacy, g_lay.chk.left, g_lay.chk.top,
                   g_lay.chk.right - g_lay.chk.left, g_lay.chk.bottom - g_lay.chk.top, TRUE);
    } else {
        ShowWindow(g_chk_legacy, SW_HIDE);
    }
    MoveWindow(g_log, g_lay.log.left + D(12), g_lay.log.top + D(10),
               (g_lay.log.right - g_lay.log.left) - D(24),
               (g_lay.log.bottom - g_lay.log.top) - D(20), TRUE);

    rebuild_tabring();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

/* ================================================================== */
/* 绘制                                                                */
/* ================================================================== */

static void draw_button(HDC dc, int kind, const wchar_t *label, RECT rc, int state,
                        int enabled, COLORREF pbg)
{
    /* state: 0 普通 1 悬停 2 按下 3 选中(勾选) */
    int x = rc.left, y = rc.top, w = rc.right - rc.left, h = rc.bottom - rc.top;
    COLORREF bg = pbg;
    int radius = D(8);

    if (kind == 4) {                      /* 复选框：只画小方块 + 文字 */
        int b = D(18), bx = x, by = y + (h - b) / 2;
        HBRUSH cb = CreateSolidBrush(bg);
        RECT cq;
        SetRect(&cq, x, y, x + w, y + h);
        FillRect(dc, &cq, cb);
        DeleteObject(cb);
        COLORREF fill = (state == 3) ? C_ACCENT : ((state == 1) ? RGB(0xF2, 0xF5, 0xFA) : C_CARD);
        rrect(dc, bx, by, b, b, D(4), C_BORDER_HI, bg);
        rrect(dc, bx + 1, by + 1, b - 2, b - 2, D(3), fill, C_BORDER_HI);
        if (state == 3) {
            /* 对勾 */
            HPEN pn = CreatePen(PS_SOLID, D(2), RGB(255, 255, 255));
            HGDIOBJ old = SelectObject(dc, pn);
            MoveToEx(dc, bx + D(4), by + D(9), NULL);
            LineTo(dc, bx + D(7), by + D(13));
            LineTo(dc, bx + D(14), by + D(5));
            SelectObject(dc, old);
            DeleteObject(pn);
        }
        txt(dc, label, g_f_edit, enabled ? C_TEXT : C_MUTED,
            bx + b + D(10), y, w - b - D(10), h, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        return;
    }

    if (kind == 3) {                      /* 文字链接（箭头自己画，避免字体缺字） */
        COLORREF c = (state == 1) ? C_ACCENT_H : (enabled ? C_ACCENT : C_MUTED);
        HBRUSH fb = CreateSolidBrush(bg);
        POINT tri[3];
        int cx = x + w - D(7), cy = y + h / 2;
        RECT q;
        SetRect(&q, x, y, x + w, y + h);
        FillRect(dc, &q, fb);
        DeleteObject(fb);
        txt(dc, label, g_f_foot, c, x, y, w - D(16), h, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
        if (g_adv_open) {
            tri[0].x = cx - D(4); tri[0].y = cy + D(2);
            tri[1].x = cx + D(4); tri[1].y = cy + D(2);
            tri[2].x = cx;        tri[2].y = cy - D(3);
        } else {
            tri[0].x = cx - D(4); tri[0].y = cy - D(2);
            tri[1].x = cx + D(4); tri[1].y = cy - D(2);
            tri[2].x = cx;        tri[2].y = cy + D(3);
        }
        {
            HBRUSH tb = CreateSolidBrush(c);
            HGDIOBJ ob = SelectObject(dc, tb);
            HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
            Polygon(dc, tri, 3);
            SelectObject(dc, op);
            SelectObject(dc, ob);
            DeleteObject(tb);
        }
        return;
    }

    if (kind == 0) {                      /* 主按钮 */
        COLORREF f = C_ACCENT;
        if (!enabled) f = C_DISABLED;
        else if (state == 2) f = C_ACCENT_P;
        else if (state == 1) f = C_ACCENT_H;
        rrect(dc, x, y, w, h, radius, f, bg);
        txt(dc, label, g_f_btn, enabled ? RGB(255, 255, 255) : C_MUTED,
            x, y, w, h, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        return;
    }

    /* kind 1/2：次要按钮 */
    {
        COLORREF fill = C_CARD, tx = enabled ? C_TEXT : C_MUTED;
        if (!enabled) fill = C_DISABLED;
        else if (state == 2) fill = RGB(0xEE, 0xF1, 0xF6);
        else if (state == 1) fill = RGB(0xF7, 0xF9, 0xFC);
        if (kind == 2) radius = D(6);
        rrect(dc, x, y, w, h, radius, C_BORDER, bg);
        rrect(dc, x + 1, y + 1, w - 2, h - 2, radius - 1, fill, C_BORDER);
        txt(dc, label, g_f_btn, tx, x, y, w, h, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }
}

static void paint_all(HDC dcw)
{
    RECT rc;
    HBRUSH br;
    int i;
    static const wchar_t *labels[3] = {
        L"①  壳视频", L"②  隐者", L"③  输出文件"
    };
    static const wchar_t *hints[3] = {
        L"一个能正常播放的 .mp4（.mov / .m4v 也可以）",
        L".zip，或 .001 / .z01 这类分卷（选中任意一片即可）",
        L"默认「原名_隐.mp4」，与压缩包放在同一目录"
    };
    GetClientRect(g_hwnd, &rc);
    br = CreateSolidBrush(C_BG);
    FillRect(dcw, &rc, br);
    DeleteObject(br);

    txt(dcw, KNAME, g_f_title, C_TEXT, g_lay.title.left, g_lay.title.top,
        g_lay.title.right - g_lay.title.left, g_lay.title.bottom - g_lay.title.top,
        DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    txt(dcw, L"把压缩包藏进能正常播放的 MP4 · 改后缀 .zip 即可解压", g_f_sub, C_MUTED,
        g_lay.sub.left, g_lay.sub.top, g_lay.sub.right - g_lay.sub.left,
        g_lay.sub.bottom - g_lay.sub.top, DT_SINGLELINE | DT_VCENTER | DT_LEFT);

    for (i = 0; i < 3; i++) {
        int x = g_lay.card[i].left, y = g_lay.card[i].top;
        int w = g_lay.card[i].right - x, h = g_lay.card[i].bottom - y;
        card(dcw, x, y, w, h, C_BG, C_BORDER, C_CARD);
        txt(dcw, labels[i], g_f_label, C_TEXT, x + D(16), y + D(12), w - D(32), D(26),
            DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        txt(dcw, hints[i], g_f_hint, C_MUTED, x + D(16) + D(14), y + D(14),
            w - D(52), D(24), DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
        /* 输入框描边（聚焦时高亮） */
        {
            RECT e = g_lay.edit[i];
            HWND focus = GetFocus();
            HWND target = (i == 0) ? g_ed_mp4 : (i == 1) ? g_ed_zip : g_ed_out;
            COLORREF edge = (focus == target) ? C_ACCENT : C_BORDER;
            rrect(dcw, e.left - D(1), e.top - D(1),
                  (e.right - e.left) + D(2), (e.bottom - e.top) + D(2),
                  D(7), edge, C_CARD);
            rrect(dcw, e.left, e.top, e.right - e.left, e.bottom - e.top,
                  D(6), C_CARD, edge);
        }
    }

    /* 进度条 */
    {
        RECT p = g_lay.prog;
        int w = p.right - p.left, h = p.bottom - p.top;
        wchar_t pctbuf[32];
        rrect(dcw, p.left, p.top, w, h, h / 2, C_TRACK, C_BG);
        if (g_pct > 0) {
            int fw = (int)((double)w * (double)g_pct / 100.0);
            if (fw < h) fw = (g_pct > 0) ? h : 0;
            rrect(dcw, p.left, p.top, fw, h, h / 2,
                  g_busy ? C_ACCENT : (g_done_flash == 2 ? C_ERR : C_ACCENT), C_TRACK);
        }
        _snwprintf(pctbuf, 30, L"%d%%", g_pct);
        txt(dcw, pctbuf, g_f_hint, C_MUTED, g_lay.pct.left, g_lay.pct.top,
            g_lay.pct.right - g_lay.pct.left, g_lay.pct.bottom - g_lay.pct.top,
            DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
    }

    /* 日志卡片 */
    {
        RECT lg = g_lay.log;
        card(dcw, lg.left, lg.top, lg.right - lg.left, lg.bottom - lg.top,
             C_BG, C_BORDER, C_LOG_BG);
    }

    /* 日志区下方那条提示 */
    txt(dcw, L"传网盘前建议给压缩包加密码并勾选「加密文件名」；壳视频越短，产物的码率越显得异常。",
        g_f_foot, C_MUTED,
        g_lay.hint.left, g_lay.hint.top, g_lay.hint.right - g_lay.hint.left,
        g_lay.hint.bottom - g_lay.hint.top, DT_SINGLELINE | DT_VCENTER | DT_LEFT);

    /* 页脚 */
    {
        HBRUSH b2 = CreateSolidBrush(C_BORDER);
        RECT dv = g_lay.divider;
        FillRect(dcw, &dv, b2);
        DeleteObject(b2);
        txt(dcw, g_status[0] ? g_status : L"就绪", g_f_foot,
            g_done_flash == 2 ? C_ERR : (g_done_flash == 1 ? C_OK : C_MUTED),
            g_lay.status.left, g_lay.status.top, g_lay.status.right - g_lay.status.left,
            g_lay.status.bottom - g_lay.status.top, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        txt(dcw, L"作者：泽渊", g_f_foot, C_MUTED,
            g_lay.author.left, g_lay.author.top, g_lay.author.right - g_lay.author.left,
            g_lay.author.bottom - g_lay.author.top, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
    }

    /* 高级选项面板 */
    if (g_adv_open) {
        RECT a = g_lay.adv;
        card(dcw, a.left, a.top, a.right - a.left, a.bottom - a.top, C_BG, C_BORDER, C_CARD);
        txt(dcw, L"勾上后只在文件末尾直接追加数据（体积小 8 字节），个别解压工具可能打不开。"
                 L"默认不勾：更兼容。", g_f_hint, C_MUTED,
            a.left + D(18), a.top + D(48), (a.right - a.left) - D(36), D(26),
            DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    }
}

/* ================================================================== */
/* 按钮悬停（子类化）                                                  */
/* ================================================================== */

static LRESULT CALLBACK btn_sub(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref)
{
    int *hover = (int *)ref;
    (void)id;
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_KEYDOWN:
        if (w == VK_TAB) {
            focus_next(h, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            return 0;
        }
        break;
    case WM_MOUSEMOVE:
        if (!*hover) {
            TRACKMOUSEEVENT tme;
            *hover = 1;
            InvalidateRect(h, NULL, FALSE);
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = h;
            tme.dwHoverTime = 0;
            TrackMouseEvent(&tme);
        }
        break;
    case WM_MOUSELEAVE:
        *hover = 0;
        InvalidateRect(h, NULL, FALSE);
        break;
    }
    return DefSubclassProc(h, m, w, l);
}

/* ---------- 焦点环（Tab 顺序自己管，避免 Z 序乱跳） ---------- */
static HWND g_tabring[10];
static int  g_tabn;

static void rebuild_tabring(void)
{
    g_tabn = 0;
    if (g_ed_mp4) g_tabring[g_tabn++] = g_ed_mp4;
    if (g_ed_zip) g_tabring[g_tabn++] = g_ed_zip;
    if (g_ed_out) g_tabring[g_tabn++] = g_ed_out;
    if (g_bt_mp4) g_tabring[g_tabn++] = g_bt_mp4;
    if (g_bt_zip) g_tabring[g_tabn++] = g_bt_zip;
    if (g_bt_out) g_tabring[g_tabn++] = g_bt_out;
    if (g_bt_open) g_tabring[g_tabn++] = g_bt_open;
    if (g_bt_run) g_tabring[g_tabn++] = g_bt_run;
    if (g_bt_adv) g_tabring[g_tabn++] = g_bt_adv;
    if (g_adv_open && g_chk_legacy) g_tabring[g_tabn++] = g_chk_legacy;
}

static void focus_next(HWND from, int back)
{
    int i, n = g_tabn;
    if (n <= 0) return;
    for (i = 0; i < n; i++) if (g_tabring[i] == from) break;
    if (i >= n) { SetFocus(g_tabring[0]); return; }
    i = back ? (i - 1 + n) % n : (i + 1) % n;
    SetFocus(g_tabring[i]);
    if (g_tabring[i]) SendMessageW(g_tabring[i], EM_SETSEL, 0, (LPARAM)-1);
}

/* 输入框：回车触发合成；焦点变化让父窗口重画描边 */
static LRESULT CALLBACK edit_sub(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref)
{
    (void)id; (void)ref;
    switch (m) {
    case WM_KEYDOWN:
        if (w == VK_RETURN) {
            if (!g_busy) SendMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(ID_RUN_BTN, 0), 0);
            return 0;
        }
        if (w == VK_TAB) {
            int back = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            focus_next(h, back);
            SendMessageW(h, EM_SETSEL, 0, (LPARAM)-1);
            return 0;
        }
        break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(GetParent(h), NULL, FALSE);
        break;
    }
    return DefSubclassProc(h, m, w, l);
}

/* ================================================================== */
/* 日志与状态                                                          */
/* ================================================================== */

static void log_push(const wchar_t *line)
{
    SYSTEMTIME st;
    wchar_t ts[24], add[1200];
    size_t cur;
    GetLocalTime(&st);
    _snwprintf(ts, 20, L"[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    _snwprintf(add, 1100, L"%s%s", ts, line);

    cur = wcslen(g_logbuf);
    if (cur + wcslen(add) + 3 >= sizeof(g_logbuf) / sizeof(wchar_t)) {
        /* 丢掉最老的一半，保持内存可控 */
        wchar_t *nl = wcschr(g_logbuf, L'\n');
        if (nl) {
            size_t cut = (size_t)(nl - g_logbuf) + 1;
            size_t keep = wcslen(g_logbuf + cut);
            memmove(g_logbuf, g_logbuf + cut, (keep + 1) * sizeof(wchar_t));
            cur = wcslen(g_logbuf);
        } else {
            g_logbuf[0] = 0;
            cur = 0;
        }
    }
    if (cur) wcscat(g_logbuf, L"\r\n");
    wcscat(g_logbuf, add);
    SetWindowTextW(g_log, g_logbuf);
    SendMessageW(g_log, EM_SETSEL, (WPARAM)wcslen(g_logbuf), (LPARAM)wcslen(g_logbuf));
    SendMessageW(g_log, EM_SCROLLCARET, 0, 0);
}

static void set_status(const wchar_t *s)
{
    wcsncpy(g_status, s, 500);
    g_status[500] = 0;
    InvalidateRect(g_hwnd, &g_lay.status, FALSE);
}

/* ================================================================== */
/* 文件对话框                                                          */
/* ================================================================== */

#define ZIP_FILTER \
    L"压缩包与分卷 (*.zip;*.z01;*.001~*.007)\0" \
    L"*.zip;*.z01;*.z02;*.001;*.002;*.003;*.004;*.005;*.006;*.007\0" \
    L"只看 zip 整包 (*.zip)\0*.zip\0" \
    L"所有文件 (*.*)\0*.*\0\0"

static int pick_open(HWND src, wchar_t *out, size_t n, const wchar_t *filter,
                     const wchar_t *defext, const wchar_t *title)
{
    OPENFILENAMEW ofn;
    wchar_t buf[MAXP];
    memset(&ofn, 0, sizeof(ofn));
    memset(buf, 0, sizeof(buf));
    if (src) {
        GetWindowTextW(src, buf, MAXP);
        sanitize_path(buf);
        if (!file_exists(buf)) buf[0] = 0;
    }
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAXP;
    ofn.lpstrTitle = title;
    ofn.lpstrDefExt = defext;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
                OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) {
        DWORD ec = CommDlgExtendedError();
        if (ec) {
            wchar_t m[160];
            _snwprintf(m, 150, L"打开文件对话框失败（错误码 0x%08X）", (unsigned)ec);
            log_push(m);
        }
        return 0;
    }
    wcsncpy(out, buf, n - 1);
    out[n - 1] = 0;
    return 1;
}

static int pick_save(wchar_t *out, size_t n, const wchar_t *filter,
                     const wchar_t *defext, const wchar_t *title)
{
    OPENFILENAMEW ofn;
    wchar_t buf[MAXP];
    memset(&ofn, 0, sizeof(ofn));
    wcsncpy(buf, out, MAXP - 1);
    buf[MAXP - 1] = 0;
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAXP;
    ofn.lpstrTitle = title;
    ofn.lpstrDefExt = defext;
    ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&ofn)) {
        DWORD ec = CommDlgExtendedError();
        if (ec) {
            wchar_t m[160];
            _snwprintf(m, 150, L"保存文件对话框失败（错误码 0x%08X）", (unsigned)ec);
            log_push(m);
        }
        return 0;
    }
    wcsncpy(out, buf, n - 1);
    out[n - 1] = 0;
    return 1;
}

/* ================================================================== */
/* 取输入 / 推断输出                                                     */
/* ================================================================== */

static void read_edits(void)
{
    GetWindowTextW(g_ed_mp4, g_mp4, MAXP);
    GetWindowTextW(g_ed_zip, g_zip, MAXP);
    GetWindowTextW(g_ed_out, g_out, MAXP);
    sanitize_path(g_mp4);
    sanitize_path(g_zip);
    sanitize_path(g_out);
}

static void set_out_text(const wchar_t *t)
{
    SetWindowTextW(g_ed_out, t);
    wcsncpy(g_out_auto, t, MAXP - 1);
    g_out_auto[MAXP - 1] = 0;
}

/* 输出框里还是「上次自动填的」或者空着，就可以跟着压缩包改 */
static int out_is_auto(void)
{
    wchar_t cur[MAXP];
    GetWindowTextW(g_ed_out, cur, MAXP);
    if (!cur[0]) return 1;
    return g_out_auto[0] && _wcsicmp(cur, g_out_auto) == 0;
}

static void suggest_out(const wchar_t *pack)
{
    wchar_t buf[MAXP];
    size_t len, k;
    wcsncpy(buf, pack, MAXP - 8);
    buf[MAXP - 8] = 0;
    len = wcslen(buf);
    for (k = len; k > 0; k--) {
        if (buf[k - 1] == L'\\' || buf[k - 1] == L'/') break;
        if (buf[k - 1] == L'.') { buf[k - 1] = 0; break; }
    }
    if (wcslen(buf) > 4) {
        /* 分卷：1.ZIP.001 → 去掉 .001 与 .ZIP 只留 1 */
        size_t l2 = wcslen(buf);
        if (l2 > 4 && _wcsicmp(buf + l2 - 4, L".zip") == 0) buf[l2 - 4] = 0;
    }
    wcscat(buf, L"_隐.mp4");
    set_out_text(buf);
}

/* ================================================================== */
/* 工作线程                                                            */
/* ================================================================== */

typedef struct {
    wchar_t *mp4, *zip, *out;
    int mode;
    HWND hwnd;
    int last_pct;
} Job;

static Job *g_job;

static void job_free(Job *j)
{
    if (!j) return;
    free(j->mp4);
    free(j->zip);
    free(j->out);
    free(j);
}

static void cb_log(const char *msg, void *ud)
{
    /* 引擎的技术日志一律不展示给用户；界面上只有「开始合成 / 成功 / 失败」三行 */
    (void)msg;
    (void)ud;
}

static void cb_prog(double frac, const char *stage, void *ud)
{
    Job *j = (Job *)ud;
    int pct = (int)(frac * 100.0);
    (void)stage;
    if (pct == j->last_pct) return;
    j->last_pct = pct;
    PostMessageW(j->hwnd, WM_APP_PROG2, (WPARAM)pct, 0);
}

static unsigned __stdcall worker(void *p)
{
    Job *j = (Job *)p;
    char *m = w_to_u8(j->mp4);
    char *z = w_to_u8(j->zip);
    char *o = w_to_u8(j->out);
    char err[1200];
    int rc = -1;
    wchar_t *we;

    err[0] = 0;
    if (!m || !z || !o) {
        strcpy(err, "路径转换失败（可能含异常字符）");
    } else {
        CoreCtx ctx;
        ctx.log = cb_log;
        ctx.progress = cb_prog;
        ctx.ud = j;
        rc = core_build(m, z, o, j->mode, &ctx, err, sizeof(err));
    }
    free(m); free(z); free(o);

    we = u8_to_w(err[0] ? err : (rc ? "未知错误" : ""));
    if (!we) {
        we = (wchar_t *)malloc(64 * sizeof(wchar_t));
        if (we) wcscpy(we, L"未知错误");
    }
    if (!we) return 0;
    PostMessageW(j->hwnd, WM_APP_DONE, (WPARAM)rc, (LPARAM)we);
    return 0;
}

static void start_build(void)
{
    Job *j;
    HANDLE th;
    wchar_t msg[1024];

    read_edits();
    if (!g_mp4[0] || !g_zip[0]) {
        MessageBoxW(g_hwnd, L"还差一点：请先选好壳视频和压缩包（输出可以留空，会自动取名）。",
                    KNAME, MB_ICONWARNING | MB_OK);
        return;
    }
    if (!file_exists(g_mp4)) {
        MessageBoxW(g_hwnd, L"壳视频不存在，请重新选择。", KNAME, MB_ICONWARNING | MB_OK);
        return;
    }
    if (!file_exists(g_zip)) {
        MessageBoxW(g_hwnd, L"压缩包不存在（分卷请确认整组都在同一目录）。",
                    KNAME, MB_ICONWARNING | MB_OK);
        return;
    }
    if (!g_out[0]) {
        suggest_out(g_zip);
        read_edits();
    }
    if (!ends_with_i(g_out, L".mp4")) {
        wcsncat(g_out, L".mp4", MAXP - wcslen(g_out) - 1);
        SetWindowTextW(g_ed_out, g_out);
    }
    if (_wcsicmp(g_out, g_mp4) == 0 || _wcsicmp(g_out, g_zip) == 0) {
        MessageBoxW(g_hwnd, L"输出文件不能和输入文件同一个路径。", KNAME, MB_ICONWARNING | MB_OK);
        return;
    }
    if (file_exists(g_out)) {
        _snwprintf(msg, 1000, L"输出文件已存在，要覆盖吗？\r\n\r\n%s", g_out);
        if (MessageBoxW(g_hwnd, msg, KNAME, MB_ICONQUESTION | MB_YESNO) != IDYES) return;
    }

    j = (Job *)calloc(1, sizeof(Job));
    if (!j) return;
    j->mp4 = _wcsdup(g_mp4);
    j->zip = _wcsdup(g_zip);
    j->out = _wcsdup(g_out);
    j->mode = g_legacy ? 1 : 0;
    j->hwnd = g_hwnd;
    j->last_pct = -1;

    EnableWindow(g_bt_run, FALSE);
    EnableWindow(g_bt_open, FALSE);
    g_pct = 0;
    g_done_flash = 0;
    set_status(L"正在合成…");
    log_push(L"开始合成");
    InvalidateRect(g_hwnd, &g_lay.prog, FALSE);

    g_job = j;
    g_busy = 1;
    th = (HANDLE)_beginthreadex(NULL, 0, worker, j, 0, NULL);
    if (!th) {
        g_busy = 0;
        EnableWindow(g_bt_run, TRUE);
        EnableWindow(g_bt_open, TRUE);
        set_status(L"无法创建工作线程");
        job_free(j);
        g_job = NULL;
        MessageBoxW(g_hwnd, L"内存不足，无法启动处理线程。", KNAME, MB_ICONERROR | MB_OK);
    } else {
        CloseHandle(th);
    }
}

static void finish_build(int rc, wchar_t *err)
{
    wchar_t sz[64], st[900];
    WIN32_FILE_ATTRIBUTE_DATA fad;

    g_busy = 0;
    job_free(g_job);
    g_job = NULL;
    EnableWindow(g_bt_run, TRUE);
    EnableWindow(g_bt_open, TRUE);

    if (rc == 0) {
        unsigned long long total = 0;
        g_pct = 100;
        g_done_flash = 1;
        if (GetFileAttributesExW(g_out, GetFileExInfoStandard, &fad))
            total = ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        fmt_size_w(sz, 60, total);
        _snwprintf(st, 700, L"合成成功：%s（%s）", g_out, sz);
        set_status(st);
        log_push(st);
    } else {
        g_done_flash = 2;
        set_status(L"合成失败");
        _snwprintf(st, 900, L"合成失败：%s", err && err[0] ? err : L"未知错误");
        log_push(st);
        MessageBoxW(g_hwnd, st, KNAME, MB_ICONERROR | MB_OK);
    }
    InvalidateRect(g_hwnd, NULL, FALSE);
}

/* ================================================================== */
/* 命令行                                                              */
/* ================================================================== */

static void cli_usage(int console)
{
    const wchar_t *u =
        L"隐渊 —— 把压缩包藏进能正常播放的 MP4\n\n"
        L"用法：\n"
        L"  \"隐渊.exe\"                                   启动界面\n\n"
        L"  \"隐渊.exe\" -m 外壳.mp4 -z 压缩包.zip -o 输出.mp4 [-raw] [-q]\n"
        L"      -m    壳视频（.mp4）\n"
        L"      -z    要隐藏的压缩包（.zip，或分卷的第一片）\n"
        L"      -o    输出文件（.mp4）；省略时自动命名为「压缩包名.mp4」\n"
        L"      -raw  老式拼接（不写 free 盒，体积小 8 字节）\n"
        L"      -q    静默，只报错误\n";
    if (console) wprintf(u);
    else MessageBoxW(NULL, u, KNAME, MB_ICONINFORMATION | MB_OK);
}

typedef struct { int quiet; int console; } CliCtx;

static void cli_log(const char *msg, void *ud)
{
    CliCtx *c = (CliCtx *)ud;
    if (c->quiet || !c->console) return;
    printf("%s\n", msg);
}

static void cli_prog(double frac, const char *stage, void *ud)
{
    CliCtx *c = (CliCtx *)ud;
    (void)stage;
    if (c->quiet || !c->console) return;
    printf("\r%3d%%", (int)(frac * 100.0));
    fflush(stdout);
}

static int run_cli(int argc, wchar_t **argv, int console)
{
    int i, rc;
    wchar_t *mp4 = NULL, *zip = NULL, *out = NULL;
    wchar_t autoout[MAXP];
    int raw = 0, quiet = 0;
    char *m8, *z8, *o8;
    char err[1200];
    CliCtx cc;
    CoreCtx ctx;

    for (i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"-h") || !wcscmp(argv[i], L"--help")) { cli_usage(console); return 0; }
        else if (!wcscmp(argv[i], L"-m") && i + 1 < argc) mp4 = argv[++i];
        else if (!wcscmp(argv[i], L"-z") && i + 1 < argc) zip = argv[++i];
        else if (!wcscmp(argv[i], L"-o") && i + 1 < argc) out = argv[++i];
        else if (!wcscmp(argv[i], L"-raw")) raw = 1;
        else if (!wcscmp(argv[i], L"-q")) quiet = 1;
        else {
            wchar_t m[600];
            _snwprintf(m, 580, L"无法识别的参数：%s", argv[i]);
            if (console) { printf("无法识别的参数。\n"); cli_usage(console); }
            else MessageBoxW(NULL, m, KNAME, MB_ICONERROR | MB_OK);
            return 2;
        }
    }
    if (!mp4 || !zip) { cli_usage(console); return 2; }

    if (!out) {
        size_t len, k;
        wcsncpy(autoout, zip, MAXP - 8);
        autoout[MAXP - 8] = 0;
        len = wcslen(autoout);
        for (k = len; k > 0; k--) {
            if (autoout[k - 1] == L'\\' || autoout[k - 1] == L'/') break;
            if (autoout[k - 1] == L'.') { autoout[k - 1] = 0; break; }
        }
        wcscat(autoout, L"_隐.mp4");
        out = autoout;
    }

    m8 = w_to_u8(mp4); z8 = w_to_u8(zip); o8 = w_to_u8(out);
    if (!m8 || !z8 || !o8) return 3;

    cc.quiet = quiet;
    cc.console = console;
    ctx.log = cli_log;
    ctx.progress = cli_prog;
    ctx.ud = &cc;

    rc = core_build(m8, z8, o8, raw ? 1 : 0, &ctx, err, sizeof(err));
    if (!quiet && console) printf("\n");
    free(m8); free(z8); free(o8);

    if (rc != 0) {
        if (console) { printf("失败：%s\n", err[0] ? err : "未知错误"); }
        else MessageBoxW(NULL, err[0] ? u8_to_w(err) : L"未知错误", KNAME, MB_ICONERROR | MB_OK);
        return 1;
    }
    if (!quiet) {
        wchar_t m[1200];
        _snwprintf(m, 1180, L"完成：%s", out);
        if (console) wprintf(L"%s\n", m);
        else MessageBoxW(NULL, m, KNAME, MB_ICONINFORMATION | MB_OK);
    }
    return 0;
}

/* ================================================================== */
/* 控件创建                                                            */
/* ================================================================== */

static void create_controls(void)
{
    int i;
    static const wchar_t *btxt[3] = { L"浏览…", L"浏览…", L"另存为…" };
    static const DWORD bstyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW;

    g_ed_mp4 = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT, 0, 0, 10, 10, g_hwnd,
        (HMENU)(INT_PTR)ID_MP4_EDIT, NULL, NULL);
    g_ed_zip = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT, 0, 0, 10, 10, g_hwnd,
        (HMENU)(INT_PTR)ID_ZIP_EDIT, NULL, NULL);
    g_ed_out = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT, 0, 0, 10, 10, g_hwnd,
        (HMENU)(INT_PTR)ID_OUT_EDIT, NULL, NULL);

    g_bt_mp4 = CreateWindowExW(0, L"BUTTON", btxt[0], bstyle, 0, 0, 10, 10, g_hwnd,
        (HMENU)(INT_PTR)ID_MP4_BTN, NULL, NULL);
    g_bt_zip = CreateWindowExW(0, L"BUTTON", btxt[1], bstyle, 0, 0, 10, 10, g_hwnd,
        (HMENU)(INT_PTR)ID_ZIP_BTN, NULL, NULL);
    g_bt_out = CreateWindowExW(0, L"BUTTON", btxt[2], bstyle, 0, 0, 10, 10, g_hwnd,
        (HMENU)(INT_PTR)ID_OUT_BTN, NULL, NULL);
    g_bt_run = CreateWindowExW(0, L"BUTTON", L"开始合成", bstyle | WS_TABSTOP, 0, 0, 10, 10, g_hwnd,
        (HMENU)(INT_PTR)ID_RUN_BTN, NULL, NULL);
    g_bt_open = CreateWindowExW(0, L"BUTTON", L"打开输出所在文件夹", bstyle, 0, 0, 10, 10, g_hwnd,
        (HMENU)(INT_PTR)ID_OPEN_DIR, NULL, NULL);
    g_bt_adv = CreateWindowExW(0, L"BUTTON", L"高级选项", bstyle, 0, 0, 10, 10, g_hwnd,
        (HMENU)(INT_PTR)ID_ADV_LINK, NULL, NULL);
    g_chk_legacy = CreateWindowExW(0, L"BUTTON", L"老式拼接（体积小 8 字节）",
        bstyle, 0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)ID_LEGACY, NULL, NULL);

    g_log = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_LEFT,
        0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)ID_LOG, NULL, NULL);

    for (i = 0; i < 3; i++) {
        SendMessageW(g_ed_mp4, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(D(10), D(8)));
        SendMessageW(g_ed_zip, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(D(10), D(8)));
        SendMessageW(g_ed_out, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(D(10), D(8)));
        break;
    }
    SendMessageW(g_log, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(D(10), D(8)));

    SetWindowSubclass(g_ed_mp4, edit_sub, 0x2000, 0);
    SetWindowSubclass(g_ed_zip, edit_sub, 0x2001, 0);
    SetWindowSubclass(g_ed_out, edit_sub, 0x2002, 0);

    g_btnh[0] = g_bt_mp4; g_btnh[1] = g_bt_zip; g_btnh[2] = g_bt_out;
    g_btnh[3] = g_bt_run; g_btnh[4] = g_bt_open;
    for (i = 0; i < 5; i++)
        SetWindowSubclass(g_btnh[i], btn_sub, 0x1000 + (UINT_PTR)i, (DWORD_PTR)&g_hover[i]);

    apply_fonts();
}

/* ================================================================== */
/* 窗口过程                                                            */
/* ================================================================== */

static void update_adv_link(void)
{
    SetWindowTextW(g_bt_adv, g_adv_open ? L"高级选项" : L"高级选项");
    ShowWindow(g_chk_legacy, g_adv_open ? SW_SHOW : SW_HIDE);
    layout_ui();
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        g_br_bg = CreateSolidBrush(C_BG);
        g_br_card = CreateSolidBrush(C_CARD);
        g_br_log = CreateSolidBrush(C_LOG_BG);
        wcscpy(g_status, L"就绪");
        DragAcceptFiles(hwnd, TRUE);
        g_adv_open = 0;
        g_legacy = 0;
        create_controls();
        layout_ui();
        log_push(L"选好壳视频和压缩包，点「开始合成」；也可以把文件直接拖进窗口。");
        return 0;

    case WM_SIZE:
        if (g_ed_mp4) layout_ui();
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc;
        HDC mem;
        HBITMAP bmp;
        RECT rc;
        int w, h;
        dc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);
        w = rc.right; h = rc.bottom;
        mem = CreateCompatibleDC(dc);
        bmp = CreateCompatibleBitmap(dc, w, h);
        SelectObject(mem, bmp);
        paint_all(mem);
        BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT *)lp;
        RECT rc;
        int hover = 0, state = 0;
        rc = di->rcItem;
        if (di->CtlID == ID_MP4_BTN || di->CtlID == ID_ZIP_BTN || di->CtlID == ID_OUT_BTN) {
            int i = (di->CtlID == ID_MP4_BTN) ? 0 : (di->CtlID == ID_ZIP_BTN) ? 1 : 2;
            hover = g_hover[i];
            state = (di->itemState & ODS_SELECTED) ? 2 : (hover ? 1 : 0);
            draw_button(di->hDC, 2, i == 0 ? L"浏览…" : (i == 1 ? L"浏览…" : L"另存为…"),
                        rc, state, IsWindowEnabled(di->hwndItem), C_CARD);
        } else if (di->CtlID == ID_RUN_BTN) {
            hover = g_hover[3];
            state = (di->itemState & ODS_SELECTED) ? 2 : (hover ? 1 : 0);
            draw_button(di->hDC, 0, L"开始合成", rc, state, IsWindowEnabled(di->hwndItem), C_BG);
        } else if (di->CtlID == ID_OPEN_DIR) {
            hover = g_hover[4];
            state = (di->itemState & ODS_SELECTED) ? 2 : (hover ? 1 : 0);
            draw_button(di->hDC, 1, L"打开输出所在文件夹", rc, state, IsWindowEnabled(di->hwndItem), C_BG);
        } else if (di->CtlID == ID_ADV_LINK) {
            draw_button(di->hDC, 3, g_adv_open ? L"高级选项" : L"高级选项", rc, hover, TRUE, C_BG);
        } else if (di->CtlID == ID_LEGACY) {
            draw_button(di->hDC, 4, L"老式拼接（体积小 8 字节）", rc,
                        g_legacy ? 3 : (hover ? 1 : 0), IsWindowEnabled(di->hwndItem), C_CARD);
        }
        if (di->itemState & ODS_FOCUS) {
            /* 焦点框：细描边，不抢眼 */
        }
        return TRUE;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        HWND ctl = (HWND)lp;
        SetBkMode(dc, TRANSPARENT);
        if (ctl == g_log) {
            SetTextColor(dc, RGB(0x4E, 0x59, 0x69));
            SetBkColor(dc, C_LOG_BG);
            return (LRESULT)g_br_log;
        }
        SetTextColor(dc, C_TEXT);
        SetBkColor(dc, C_CARD);
        return (LRESULT)g_br_card;
    }

    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, C_TEXT);
        SetBkColor(dc, C_CARD);
        return (LRESULT)g_br_card;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp);

        if (id == ID_MP4_BTN) {
            wchar_t buf[MAXP];
            if (pick_open(g_ed_mp4, buf, MAXP,
                          L"视频文件 (*.mp4;*.mov;*.m4v)\0*.mp4;*.mov;*.m4v\0所有文件 (*.*)\0*.*\0\0",
                          L"mp4", L"选择壳视频"))
                SetWindowTextW(g_ed_mp4, buf);
        } else if (id == ID_ZIP_BTN) {
            wchar_t buf[MAXP];
            if (pick_open(g_ed_zip, buf, MAXP, ZIP_FILTER, L"zip", L"选择要藏进去的压缩包")) {
                SetWindowTextW(g_ed_zip, buf);
                if (out_is_auto()) suggest_out(buf);   /* 换包就换目录，实时跟随 */
            }
        } else if (id == ID_OUT_BTN) {
            wchar_t buf[MAXP];
            read_edits();
            wcsncpy(buf, g_out, MAXP - 1);
            buf[MAXP - 1] = 0;
            if (pick_save(buf, MAXP, L"MP4 视频 (*.mp4)\0*.mp4\0所有文件 (*.*)\0*.*\0\0",
                          L"mp4", L"输出文件保存为")) {
                SetWindowTextW(g_ed_out, buf);
                g_out_auto[0] = 0;        /* 用户亲手指定过，之后不再自动覆盖 */
            }
        } else if (id == ID_RUN_BTN) {
            if (!g_busy) start_build();
        } else if (id == ID_OPEN_DIR) {
            wchar_t args[MAXP + 40];
            read_edits();
            if (!g_out[0] || !file_exists(g_out)) {
                MessageBoxW(hwnd, L"还没有生成文件，先合成一次。", KNAME, MB_ICONINFORMATION | MB_OK);
                break;
            }
            _snwprintf(args, MAXP + 30, L"/select,\"%s\"", g_out);
            ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
        } else if (id == ID_ADV_LINK) {
            g_adv_open = !g_adv_open;
            update_adv_link();
        } else if (id == ID_LEGACY) {
            g_legacy = !g_legacy;
            InvalidateRect(g_chk_legacy, NULL, FALSE);
        }
        break;
    }

    case WM_APP_LOG: {
        wchar_t *w = (wchar_t *)lp;
        if (w) { log_push(w); free(w); }
        return 0;
    }

    case WM_APP_PROG2:
        g_pct = (int)wp;
        InvalidateRect(hwnd, &g_lay.prog, FALSE);
        InvalidateRect(hwnd, &g_lay.pct, FALSE);
        return 0;

    case WM_APP_DONE: {
        wchar_t *w = (wchar_t *)lp;
        finish_build((int)wp, w);
        if (w) free(w);
        return 0;
    }

    case WM_DROPFILES: {
        HDROP hd = (HDROP)wp;
        int n = DragQueryFileW(hd, 0xFFFFFFFF, NULL, 0);
        int i, got_video = 0, got_pack = 0;
        wchar_t f[MAXP];
        for (i = 0; i < n; i++) {
            int k;
            if (!DragQueryFileW(hd, i, f, MAXP)) continue;
            k = classify_name(f);
            if (k == 1) {
                SetWindowTextW(g_ed_mp4, f);
                got_video = 1;
            } else if (k == 2) {
                SetWindowTextW(g_ed_zip, f);
                if (out_is_auto()) suggest_out(f);
                got_pack = 1;
            } else {
                wchar_t cur[MAXP];
                GetWindowTextW(g_ed_mp4, cur, MAXP);
                if (!got_video && !cur[0]) { SetWindowTextW(g_ed_mp4, f); got_video = 1; }
                else if (!got_pack) {
                    SetWindowTextW(g_ed_zip, f);
                    if (out_is_auto()) suggest_out(f);
                    got_pack = 1;
                }
            }
        }
        DragFinish(hd);
        SetForegroundWindow(hwnd);
        return 0;
    }

    case WM_KEYDOWN:
        if (wp == VK_RETURN && !g_busy) { start_build(); return 0; }
        break;

    case WM_CLOSE:
        if (g_busy) {
            MessageBoxW(hwnd, L"正在合成，等它跑完再关吧。", KNAME, MB_ICONINFORMATION | MB_OK);
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        RECT r;
        r.left = 0; r.top = 0; r.right = D(900); r.bottom = D(710);
        AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
        mm->ptMinTrackSize.x = r.right - r.left;
        mm->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }

    case WM_DPICHANGED: {
        RECT *pr = (RECT *)lp;
        g_dpi = HIWORD(wp);
        create_fonts();
        apply_fonts();
        SetWindowPos(hwnd, NULL, pr->left, pr->top, pr->right - pr->left,
                     pr->bottom - pr->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout_ui();
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ================================================================== */
/* 入口                                                                */
/* ================================================================== */

static int register_and_show(HINSTANCE hi, int nShow)
{
    WNDCLASSEXW wc;
    HWND hwnd;
    RECT rc;
    HDC sdc;
    INITCOMMONCONTROLSEX icc;
    DWORD style = WS_OVERLAPPEDWINDOW;

    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icc);

    load_embedded_fonts(hi);

    sdc = GetDC(NULL);
    if (sdc) { g_dpi = GetDeviceCaps(sdc, LOGPIXELSX); ReleaseDC(NULL, sdc); }
    if (g_dpi <= 0) g_dpi = 96;

    create_fonts();

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1010));
    wc.hIconSm = wc.hIcon;
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"YinxiezheWnd";
    if (!RegisterClassExW(&wc)) return 1;

    rc.left = 0; rc.top = 0; rc.right = D(940); rc.bottom = D(760);
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    hwnd = CreateWindowExW(0, L"YinxiezheWnd", KNAME, style,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           rc.right - rc.left, rc.bottom - rc.top,
                           NULL, NULL, hi, NULL);
    if (!hwnd) return 1;

    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);
    return 0;
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE hp, LPWSTR cmdline, int nShow)
{
    int argc = 0;
    wchar_t **argv;
    (void)hp; (void)cmdline;

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;

    if (argc > 1) {
        int console = 0;
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
            SetConsoleOutputCP(CP_UTF8);
            console = 1;
        }
        {
            int rc = run_cli(argc, argv, console);
            LocalFree(argv);
            return rc;
        }
    }

    LocalFree(argv);
    {
        MSG msg;
        int rc = register_and_show(hi, nShow);
        if (rc) return rc;
        while (GetMessageW(&msg, NULL, 0, 0) > 0) {
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_TAB && g_hwnd) {
                focus_next(GetFocus(), (GetKeyState(VK_SHIFT) & 0x8000) != 0);
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return (int)msg.wParam;
    }
}

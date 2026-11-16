/* 仅用于 Linux 下的核心逻辑测试（不参与 EXE 编译） */
#include "../src/core.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int last_dec = -1;
static void on_log(const char *m, void *ud) { (void)ud; printf("[log] %s\n", m); }
static void on_prog(double f, const char *s, void *ud)
{
    int d = (int)(f * 10);
    (void)s; (void)ud;
    if (d != last_dec) { last_dec = d; printf("[prog] %3d%%\n", d * 10); }
}

int main(int argc, char **argv)
{
    char err[1024] = {0};
    CoreCtx ctx;
    int mode = 0, rc;
    if (argc < 4) {
        fprintf(stderr, "usage: %s <mp4> <zip> <out> [mode]\n", argv[0]);
        return 2;
    }
    if (argc >= 5) mode = atoi(argv[4]);
    ctx.log = on_log;
    ctx.progress = on_prog;
    ctx.ud = NULL;
    rc = core_build(argv[1], argv[2], argv[3], mode, &ctx, err, sizeof(err));
    if (rc != 0) { printf("[ERR] %s\n", err[0] ? err : "(unknown)"); return 1; }
    printf("[OK] saved %s\n", argv[3]);
    return 0;
}

/*
 * core.h - MP4 + ZIP 双格式合成引擎
 * 纯 C99，无第三方依赖；Windows / Linux 均可编译（便于在 Linux 上跑完整测试）。
 */
#ifndef MP4PACK_CORE_H
#define MP4PACK_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 回调上下文：所有字符串均为 UTF-8 */
typedef struct {
    void (*log)(const char *utf8_msg, void *ud);
    void (*progress)(double frac, const char *utf8_stage, void *ud);
    void *ud;
} CoreCtx;

/* mode: 0 = 自动（推荐），1 = 纯追加（不写 free 盒），2 = 强制写 free 盒 */
int core_build(const char *mp4_path, const char *zip_path, const char *out_path,
               int mode, const CoreCtx *ctx, char *err, size_t errlen);

/* 分析外壳视频，返回其盒结构是否完整（供 GUI 预检用），1=正常 0=异常 */
int core_probe_mp4(const char *mp4_path, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif
#endif

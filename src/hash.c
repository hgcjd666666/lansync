/*
 * hash.c -- 文件内容哈希
 *
 * 两种算法，取舍理由见 common.h。这里只讲实现：
 *
 *   GXHASH  拿 ARMv8-A 的 AES 指令当混合器，需要 CPU 支持；
 *           分块串链，规则见下（改它等于改协议）。
 *   SHA3    wolfSSL 的 SHA3-256，纯软件、任何 CPU 都能跑，流式无边界问题。
 *
 * 两种都输出 u64（SHA3 取哈希前 8 字节，大端），因为协议里 hash 字段就是 u64。
 * 反过来说：**同一个文件两种算法算出的值完全无关** —— 所以两端必须用同一种，
 * 由服务端在 HELLO_ACK 里定死。
 *
 * gxhash 的分块规则（改这里等于改协议，两端必须同时改）：
 *   每块固定 LS_CHUNK，最后一块是余数；空文件一块都不进。
 *   blk = gxhash64(块内容, len, SEED_BLOCK ^ 块起始偏移)
 *       偏移进 seed，所以交换两个块会被检出（纯异或累积则不会）。
 *   acc = gxhash64(&blk, 8, acc)
 *       块哈希串成一条链，顺序敏感；acc 初值是 SEED_ACC。
 *   空文件的 acc 就是 SEED_ACC，确定的非零值。
 */
#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/auxv.h>

#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/sha3.h>

/* ARM64 的 HWCAP_AES 位，值来自内核 arch/arm64/include/uapi/asm/hwcap.h。
   不直接 include 那个头是怕不同 libc 的路径不一致。 */
#define LS_HWCAP_AES (1UL << 3)

#define HASH_SEED_BLOCK 0x9E3779B97F4A7C15ULL
#define HASH_SEED_ACC   0xC2B2AE3D27D4EB4FULL

#if defined(__aarch64__)
/* Rust 侧导出的 C 接口（gxhash ffi，见 third_party/） */
extern uint64_t gxhash64(const void *buf, size_t len, int64_t seed);
#endif

static int g_gxhash_ok = -1;

int ls_gxhash_ok(void)
{
#if defined(__aarch64__)
    if (g_gxhash_ok < 0) {
        /* LANSYNC_NO_HASH=1 让 gxhash 假装不可用，用来测「这台机器没 AES」那条路
         * （此时仍会退到 SHA3，而不是彻底不比内容） */
        if (getenv("LANSYNC_NO_HASH")) {
            g_gxhash_ok = 0;
        } else {
            /* gxhash 硬依赖 aes 指令，没有就是一个 SIGILL，所以必须先问内核 */
            g_gxhash_ok = (getauxval(AT_HWCAP) & LS_HWCAP_AES) ? 1 : 0;
        }
    }
    return g_gxhash_ok;
#else
    return 0; /* 没有对应的 gxhash 目标文件 */
#endif
}

/* ---------------- 读数：把文件按 LS_CHUNK 切块交给回调 ---------------- */

typedef void (*chunk_fn)(void *ctx, const uint8_t *buf, size_t len, uint64_t off);

static int read_chunks(const char *path, chunk_fn fn, void *ctx)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        ls_set_last_error("打开失败：%s", strerror(errno));
        return -1;
    }

    uint8_t *buf = (uint8_t *)malloc(LS_CHUNK);
    if (!buf) {
        close(fd);
        ls_set_last_error("内存不足");
        return -1;
    }

    int rc = 0;
    uint64_t off = 0;
    for (;;) {
        size_t got = 0;
        /* 攒满一块再算，短读不改变分块边界 */
        while (got < LS_CHUNK) {
            ssize_t r = read(fd, buf + got, LS_CHUNK - got);
            if (r < 0) {
                if (errno == EINTR) {
                    continue;
                }
                ls_set_last_error("读取失败：%s", strerror(errno));
                rc = -1;
                goto out;
            }
            if (r == 0) {
                break; /* EOF */
            }
            got += (size_t)r;
        }
        if (got == 0) {
            break; /* 空文件，或正好读完 */
        }
        fn(ctx, buf, got, off);
        off += got;
        if (got < LS_CHUNK) {
            break; /* 最后一块 */
        }
    }

out:
    free(buf);
    close(fd);
    return rc;
}

/* ---------------- gxhash：分块串链 ---------------- */

typedef struct {
    uint64_t acc;
} GxCtx;

static void gx_chunk(void *ctx, const uint8_t *buf, size_t len, uint64_t off)
{
#if defined(__aarch64__)
    GxCtx *g = (GxCtx *)ctx;
    uint64_t blk = gxhash64(buf, len, (int64_t)(HASH_SEED_BLOCK ^ off));
    g->acc = gxhash64(&blk, sizeof blk, (int64_t)g->acc);
#else
    (void)ctx;
    (void)buf;
    (void)len;
    (void)off;
#endif
}

static int hash_gxhash(const char *path, uint64_t *out)
{
    GxCtx g;
    g.acc = HASH_SEED_ACC;
    if (read_chunks(path, gx_chunk, &g) != 0) {
        return -1;
    }
    *out = g.acc;
    return 0;
}

/* ---------------- SHA3-256：纯软件，流式 ---------------- */

typedef struct {
    wc_Sha3 sha;
    int     bad;
} Sha3Ctx;

static void sha3_chunk(void *ctx, const uint8_t *buf, size_t len, uint64_t off)
{
    Sha3Ctx *s = (Sha3Ctx *)ctx;
    (void)off;
    if (s->bad) {
        return;
    }
    if (wc_Sha3_256_Update(&s->sha, buf, (word32)len) != 0) {
        s->bad = 1;
    }
}

static int hash_sha3(const char *path, uint64_t *out)
{
    Sha3Ctx s;
    memset(&s, 0, sizeof(s));

    if (wc_InitSha3_256(&s.sha, NULL, INVALID_DEVID) != 0) {
        ls_set_last_error("SHA3 初始化失败");
        return -1;
    }
    if (read_chunks(path, sha3_chunk, &s) != 0) {
        return -1;
    }
    if (s.bad) {
        ls_set_last_error("SHA3 计算失败");
        return -1;
    }

    uint8_t digest[32];
    if (wc_Sha3_256_Final(&s.sha, digest) != 0) {
        ls_set_last_error("SHA3 收尾失败");
        return -1;
    }

    /* 协议里 hash 字段是 u64，所以取前 8 字节（大端）。
     * 0 被协议征用为「没算过」，撞上就挪一下 —— 概率 2^-64，代价却是零。 */
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v = (v << 8) | digest[i];
    }
    *out = (v == 0) ? 1u : v;
    return 0;
}

/* ---------------- 对外 ---------------- */

int ls_hash_file_algo(const char *path, int algo, uint64_t *out)
{
    if (path == NULL || out == NULL) {
        return -1;
    }
    if (algo == LS_HASH_GXHASH) {
        if (!ls_gxhash_ok()) {
            return -1;
        }
        return hash_gxhash(path, out);
    }
    if (algo == LS_HASH_SHA3) {
        return hash_sha3(path, out);
    }
    return -1;
}

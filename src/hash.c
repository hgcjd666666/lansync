/*
 * hash.c -- 文件内容哈希
 *
 * 用 gxhash（third_party/libgxhash_arm64.a）：它拿 ARMv8-A 的 AES 指令当混合器，
 * 实测 28.7 GB/s，比手机闪存顺序读快一个数量级。所以哈希永远不是瓶颈，
 * 真正的代价是「为了算它得把文件读一遍」——要不要付取决于场景，不是算得多快。
 *
 * 分块规则（改这里等于改协议，两端必须同时改）：
 *   每块固定 LS_CHUNK，最后一块是余数；空文件一块都不进。
 *   blk = gxhash64(块内容, len, SEED_BLOCK ^ 块起始偏移)
 *       偏移进 seed，所以交换两个块会被检出（纯异或累积则不会）。
 *   acc = gxhash64(&blk, 8, acc)
 *       块哈希串成一条链，顺序敏感；acc 初值是 SEED_ACC。
 * 空文件的 acc 就是 SEED_ACC，确定的非零值。
 *
 * 注意读数：read() 可能短读，必须攒满 LS_CHUNK 再算，
 * 否则分块边界会随 read 的返回时机变化，两端算出不同的值。
 */
#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/auxv.h>

#if defined(__aarch64__)

/* Rust 侧导出的 C 接口（gxhash ffi，见 third_party/） */
extern uint64_t gxhash64(const void *buf, size_t len, int64_t seed);

/* ARM64 的 HWCAP_AES 位，值来自内核 arch/arm64/include/uapi/asm/hwcap.h。
   不直接 include 那个头是怕不同 libc 的路径不一致。 */
#define LS_HWCAP_AES (1UL << 3)

#define HASH_SEED_BLOCK 0x9E3779B97F4A7C15ULL
#define HASH_SEED_ACC   0xC2B2AE3D27D4EB4FULL

/* 结果缓存：getauxval 只需问一次 */
static int g_hash_ok = -1;

int ls_hash_ok(void)
{
    if (g_hash_ok < 0) {
        /* LANSYNC_NO_HASH=1 让哈希功能假装缺席，用来测「CPU 没 AES」那条路径 */
        if (getenv("LANSYNC_NO_HASH")) {
            g_hash_ok = 0;
        } else {
            /* gxhash 硬依赖 aes 指令，没有就是一个 SIGILL，所以必须先问内核 */
            g_hash_ok = (getauxval(AT_HWCAP) & LS_HWCAP_AES) ? 1 : 0;
        }
    }
    return g_hash_ok;
}

int ls_hash_file(const char *path, uint64_t *out)
{
    if (!ls_hash_ok() || !out) {
        return -1;
    }

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

    uint64_t acc = HASH_SEED_ACC;
    uint64_t off = 0;
    int rc = -1;

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
                goto out;
            }
            if (r == 0) {
                break; /* EOF */
            }
            got += (size_t)r;
        }

        if (got == 0) {
            break; /* 空文件，或者正好读完 */
        }

        uint64_t blk = gxhash64(buf, got, (int64_t)(HASH_SEED_BLOCK ^ off));
        acc = gxhash64(&blk, sizeof blk, (int64_t)acc);
        off += got;

        if (got < LS_CHUNK) {
            break; /* 最后一块 */
        }
    }

    *out = acc;
    rc = 0;

out:
    free(buf);
    close(fd);
    return rc;
}

#else /* 非 aarch64：没有对应的 gxhash 目标文件，功能整体不可用 */

int ls_hash_ok(void)
{
    return 0;
}

int ls_hash_file(const char *path, uint64_t *out)
{
    (void)path;
    (void)out;
    return -1;
}

#endif

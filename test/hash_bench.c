/*
 * hash_bench.c -- 量一下各种哈希在本机的实际吞吐。
 *
 * 为什么需要它：gxhash 直接吃 ARMv8-A 的 AES 指令，CPU 没有就整个功能缺席
 * （同步退回「只比大小」）。这里回答「那还能拿什么顶上」，重点是
 * wolfSSL 里**已经链进来**的 SHA-256 —— 用它不增加任何体积。
 *
 * 纯内存操作，反复哈希同一块缓冲区，不碰磁盘。
 *
 * 用法：
 *   make hash-bench            # 在开发机上跑
 *   make hash-bench-android    # 出 Android 版，推到手机上跑（数字才代表手机）
 */
#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/sha256.h>
#include <wolfssl/wolfcrypt/sha512.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/auxv.h>

/* gxhash 的原始接口（third_party/libgxhash_arm64.a）。没有 AES 就是一条 SIGILL，
 * 所以只能在确认有能力之后再调。 */
extern uint64_t gxhash64(const void *data, size_t len, uint64_t seed);

#define BUF_BYTES (4u * 1024u * 1024u)

/* 轮数可用环境变量调（qemu 模拟下、或手机上想快点出结果时）。
 * 默认 256 轮 x 4 MiB ≈ 1 GiB，够稳。 */
static unsigned g_rounds = 256u;

static unsigned char *g_buf;

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void report(const char *name, double secs, const char *note)
{
    double mib = (double)g_rounds * (double)BUF_BYTES / (1024.0 * 1024.0);
    printf("  %-12s %9.1f MiB/s   %6.0f ms   %s\n", name, mib / secs,
           secs * 1000.0, note);
}

static void bench_sha256(void)
{
    wc_Sha256 sha;
    unsigned char out[WC_SHA256_DIGEST_SIZE];
    wc_InitSha256(&sha);
    double t0 = now_s();
    for (unsigned r = 0; r < g_rounds; r++) {
        wc_Sha256Update(&sha, g_buf, BUF_BYTES);
        wc_Sha256Final(&sha, out);
        wc_Sha256Reset(&sha);   /* 用 Reset 而不是 Free+Init：后者会重新分配内存 */
    }
    report("SHA-256", now_s() - t0, "wolfSSL，已在库里");
}

static void bench_sha512(void)
{
    wc_Sha512 sha;
    unsigned char out[WC_SHA512_DIGEST_SIZE];
    wc_InitSha512(&sha);
    double t0 = now_s();
    for (unsigned r = 0; r < g_rounds; r++) {
        wc_Sha512Update(&sha, g_buf, BUF_BYTES);
        wc_Sha512Final(&sha, out);
        wc_Sha512Reset(&sha);
    }
    report("SHA-512", now_s() - t0, "wolfSSL，已在库里");
}

static void bench_gxhash(void)
{
    uint64_t acc = 0;
    double t0 = now_s();
    for (unsigned r = 0; r < g_rounds; r++) {
        acc ^= gxhash64(g_buf, BUF_BYTES, r);
    }
    char note[64];
    snprintf(note, sizeof(note), "需要 AES 指令（acc=%llu）",
             (unsigned long long)acc);
    report("gxhash", now_s() - t0, note);
}

/* 分块大小对哈希有影响吗？顺手量一下 —— 同步时是按 256 KiB 走的。 */
static void bench_sha256_chunked(size_t chunk)
{
    wc_Sha256 sha;
    unsigned char out[WC_SHA256_DIGEST_SIZE];
    char name[32];
    snprintf(name, sizeof(name), "SHA-256/%zuK", chunk / 1024);

    wc_InitSha256(&sha);
    double t0 = now_s();
    for (unsigned r = 0; r < g_rounds; r++) {
        for (size_t off = 0; off < BUF_BYTES; off += chunk) {
            size_t n = BUF_BYTES - off < chunk ? BUF_BYTES - off : chunk;
            wc_Sha256Update(&sha, g_buf + off, (word32)n);
        }
        wc_Sha256Final(&sha, out);
        wc_Sha256Reset(&sha);
    }
    report(name, now_s() - t0, "按同步的分块方式");
}

int main(void)
{
    unsigned long hwcap = getauxval(AT_HWCAP);
    int has_aes   = (hwcap & (1UL << 3)) != 0;  /* HWCAP_AES */
    int has_sha2  = (hwcap & (1UL << 6)) != 0;  /* HWCAP_SHA2 */
    int has_sha512= (hwcap & (1UL << 21)) != 0; /* HWCAP_SHA512 */

    {
        const char *e = getenv("HASH_BENCH_ROUNDS");
        if (e != NULL && atoi(e) > 0) {
            g_rounds = (unsigned)atoi(e);
        }
    }
    printf("hash_bench -- 纯内存反复哈希，单次 %.0f MiB x %u 轮 = %.0f MiB\n",
           BUF_BYTES / 1048576.0, g_rounds,
           (double)g_rounds * BUF_BYTES / 1048576.0);
    printf("本机 HWCAP：AES=%s SHA2=%s SHA512=%s\n",
           has_aes ? "有" : "无", has_sha2 ? "有" : "无", has_sha512 ? "有" : "无");
    printf("（wolfSSL 那份库开的是 --enable-armasm：SHA-256 走三级运行时分发 ——\n"
           "  有 SHA2 扩展用汇编、只有 NEON 用 NEON 汇编、都没有回退纯 C）\n\n");

    g_buf = malloc(BUF_BYTES);
    if (!g_buf) {
        fprintf(stderr, "分配 %u 字节失败\n", BUF_BYTES);
        return 1;
    }
    /* 填点不像全零的数据，免得撞上某些实现的特判 */
    for (size_t i = 0; i < BUF_BYTES; i++) {
        g_buf[i] = (unsigned char)(i * 1103515245u >> 16);
    }

    bench_sha256();
    bench_sha512();
    bench_sha256_chunked(256 * 1024);

    if (has_aes) {
        bench_gxhash();
    } else {
        printf("  %-12s %9s   %6s   %s\n", "gxhash", "不可用", "-",
               "本机没有 AES 指令，跑它就是 SIGILL");
    }

    printf("\n对照：这两条是手机闪存/网络的量级，用来判断哈希会不会成为瓶颈\n");
    printf("  闪存顺序读   ~1500 MiB/s      网络（手机热点实测） ~37 MiB/s\n");

    free(g_buf);
    return 0;
}

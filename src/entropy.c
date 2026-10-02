/*
 * entropy.c -- 熵值分析：看一个目录里有多少数据「值得压」
 *
 * 压缩还没做，这个命令先把判断依据摆出来：对每个文件采样开头若干字节
 * 算香农熵，按区间汇总。熵低于 6 的（文本、日志、JSON、源码）压缩收益大；
 * 高于 7.5 的（jpg/mp4/zip/apk、已加密内容）再压基本等于白费 CPU。
 */
#include "common.h"

#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ENT_BUCKETS 3

typedef struct {
    uint32_t files;
    uint64_t bytes;
} Bucket;

/* 单线程命令，用静态缓冲够了 */
static const char *sz(uint64_t n)
{
    static char b[32];
    fmt_size(n, b, sizeof(b));
    return b;
}

static const char *bucket_name(int i)
{
    switch (i) {
    case 0: return "易压缩 (<6.0)  ";
    case 1: return "中等   (6.0~7.5)";
    default: return "已压缩 (>7.5)  ";
    }
}

static int bucket_of(double h)
{
    if (h < 6.0) {
        return 0;
    }
    if (h <= 7.5) {
        return 1;
    }
    return 2;
}

static void usage(void)
{
    printf(
        "lansync entropy -- 统计目录里的数据「有多值得压缩」\n"
        "\n"
        "用法: lansync entropy DIR [选项]\n"
        "\n"
        "  -n, --sample BYTES   每个文件采样多少字节算熵（默认 65536）\n"
        "  -v, --verbose        逐文件列出熵值\n"
        "  -q, --quiet          只输出汇总\n"
        "  -h, --help           显示本帮助\n");
}

int entropy_main(int argc, char **argv)
{
    static const struct option opts[] = {
        {"sample", required_argument, 0, 'n'},
        {"verbose", no_argument, 0, 'v'},
        {"quiet", no_argument, 0, 'q'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}};

    size_t sample = 64 * 1024;
    int verbose = 0;
    int c;

    while ((c = getopt_long(argc, argv, "n:vqh", opts, NULL)) != -1) {
        switch (c) {
        case 'n': sample = (size_t)strtoul(optarg, NULL, 10); break;
        case 'v': verbose = 1; break;
        case 'q': ls_set_quiet(1); break;
        case 'h': usage(); return 0;
        default: usage(); return 2;
        }
    }
    if (optind >= argc) {
        fprintf(stderr, "必须指定目录\n\n");
        usage();
        return 2;
    }
    if (sample == 0) {
        sample = 64 * 1024;
    }

    const char *dir = argv[optind];
    char root[LS_PATHMAX];
    if (!realpath(dir, root)) {
        fprintf(stderr, "目录不可用：%s (%s)\n", dir, strerror(errno));
        return 1;
    }

    FileList fl;
    char err[512];
    flist_init(&fl);
    if (fs_scan(root, &fl, err, sizeof(err)) != 0) {
        fprintf(stderr, "%s\n", err);
        flist_free(&fl);
        return 1;
    }

    uint64_t total_bytes = 0;
    for (size_t i = 0; i < fl.n; i++) {
        total_bytes += fl.v[i].size;
    }

    printf("目录 %s\n", root);
    printf("%zu 个文件，共 %s；每个文件采样前 %zu 字节算香农熵\n\n", fl.n,
           sz(total_bytes), sample);

    Bucket bk[ENT_BUCKETS];
    memset(bk, 0, sizeof(bk));

    for (size_t i = 0; i < fl.n; i++) {
        char full[LS_PATHMAX];
        if (path_join(full, sizeof(full), root, fl.v[i].rel) < 0) {
            continue;
        }
        double h = ls_entropy_file(full, sample);
        if (h < 0) {
            continue;
        }
        int b = bucket_of(h);
        bk[b].files++;
        bk[b].bytes += fl.v[i].size;
        if (verbose) {
            printf("  %.2f  %-10s %s\n", h, sz(fl.v[i].size), fl.v[i].rel);
        }
    }

    if (verbose) {
        printf("\n");
    }
    printf("熵值分布\n");
    for (int i = 0; i < ENT_BUCKETS; i++) {
        printf("  %s  %5u 个  %10s  %5.1f%%\n", bucket_name(i), bk[i].files, sz(bk[i].bytes),
               total_bytes ? 100.0 * (double)bk[i].bytes / (double)total_bytes : 0.0);
    }

    uint64_t raw = bk[0].bytes + bk[1].bytes;
    printf("\n");
    if (raw == 0) {
        printf("结论：几乎没有可压缩的数据，开压缩基本是白费 CPU。\n");
    } else {
        printf("结论：约 %s（%.0f%%）的数据熵值不高，压缩可能有明显收益。\n", sz(raw),
               total_bytes ? 100.0 * (double)raw / (double)total_bytes : 0.0);
    }

    flist_free(&fl);
    return 0;
}

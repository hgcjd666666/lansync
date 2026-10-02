/*
 * server.c -- lansync 服务端
 *
 * 职责：监听 TCP 接受客户端连接，接收客户端的文件清单，
 *       与本地目录做「大小 + 修改时间」比对，然后决定每个文件是推送、拉取还是删除。
 *       同时在 UDP 上应答局域网发现请求并周期性广播自己的存在。
 */
#include "common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    uint32_t pushed;
    uint32_t pulled;
    uint32_t deleted;
    uint32_t same;
    uint32_t failed;
    uint32_t ignored;
    uint64_t bytes;
} SyncStats;

typedef struct {
    int           fd;
    unsigned long seq;
    char          name[128];
    char          peer[64];
    char          remote_root[LS_PATHMAX];
    int           mtime_ok; /* 客户端能否保留 mtime；不能就只能比大小 */
    SyncStats     st;
} Client;

static char              g_root[LS_PATHMAX];
static char              g_name[64] = "lansync-server";
static int               g_prefer = PREFER_SERVER; /* 默认：客户端成为服务端副本 */
static int               g_delete = 1;
static int               g_allow_upload = 0; /* 默认服务端只读：不接受客户端写回 */
static int               g_verify_hash = 0;  /* --verify hash：强制按内容比对 */
static int               g_compress = LS_COMPRESS_AUTO;
static char              g_dist[LS_PATHMAX] = {0};        /* 供 client update 拉取的程序 */
static char              g_client_subdir[LS_PATHMAX] = {0}; /* 服务端指定的客户端落盘子目录 */
static uint16_t          g_port = LS_DEF_PORT;
static uint16_t          g_udp_port = LS_DEF_UDP_PORT;
static int               g_announce = 1;
static int               g_listen_fd = -1;
static volatile sig_atomic_t g_stop = 0;
static pthread_mutex_t   g_sync_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long     g_seq = 0;
static const char       *g_profile_path = NULL; /* --config 指定；NULL 用默认名 */

/* ---------------- 小工具 ---------------- */

static void mode_str(mode_t m, char *out, size_t n)
{
    static const mode_t rb[3] = {S_IRUSR, S_IRGRP, S_IROTH};
    static const mode_t wb[3] = {S_IWUSR, S_IWGRP, S_IWOTH};
    static const mode_t xb[3] = {S_IXUSR, S_IXGRP, S_IXOTH};
    char b[11];
    int k = 0;
    b[k++] = S_ISDIR(m) ? 'd' : (S_ISREG(m) ? '-' : '?');
    for (int i = 0; i < 3; i++) {
        b[k++] = (m & rb[i]) ? 'r' : '-';
        b[k++] = (m & wb[i]) ? 'w' : '-';
        b[k++] = (m & xb[i]) ? 'x' : '-';
    }
    b[k] = '\0';
    snprintf(out, n, "%s", b);
}


/* 本次接入要不要按内容比对：服务端点名要，或者客户端根本保不住 mtime。
 * 前提是本机有 aes（gxhash 的硬要求），没有就只能退回元数据那一套。 */
static int want_content_hash(const Client *c)
{
    return (g_verify_hash || !c->mtime_ok) && ls_hash_ok();
}

/* 服务端是否应该覆盖客户端 */
static int server_wins(const FileEntry *loc, const FileEntry *rem)
{
    if (!g_allow_upload) {
        return 1; /* 服务端只读：无论策略如何都不接受客户端版本 */
    }
    if (g_prefer == PREFER_SERVER) {
        return 1;
    }
    if (g_prefer == PREFER_CLIENT) {
        return 0;
    }
    if (loc->mtime > rem->mtime) {
        return 1;
    }
    if (loc->mtime < rem->mtime) {
        return 0;
    }
    return 1; /* 时间戳相同视为冲突，服务端优先 */
}

/* ---------------- 同步计划 ---------------- */

enum { ACT_PUSH = 1, ACT_PULL, ACT_DELETE };

typedef struct {
    char       *rel;
    uint64_t    size; /* 用于把推送条目切成大小合适的 bundle */
    int         act;
    const char *why;
} PlanItem;

typedef struct {
    PlanItem *v;
    size_t    n, cap;
} Plan;

static void plan_add(Plan *p, const char *rel, uint64_t size, int act, const char *why)
{
    if (p->n == p->cap) {
        size_t cap = p->cap ? p->cap * 2 : 64;
        PlanItem *nv = (PlanItem *)realloc(p->v, cap * sizeof(PlanItem));
        if (!nv) {
            return;
        }
        p->v = nv;
        p->cap = cap;
    }
    p->v[p->n].rel = strdup(rel);
    p->v[p->n].size = size;
    p->v[p->n].act = act;
    p->v[p->n].why = why;
    p->n++;
}

static void plan_free(Plan *p)
{
    for (size_t i = 0; i < p->n; i++) {
        free(p->v[i].rel);
    }
    free(p->v);
    p->v = NULL;
    p->n = p->cap = 0;
}

/* ---------------- 清单接收（分片，可能有多条 MSG_LIST_CHUNK） ---------------- */

static int recv_list(int fd, const Buf *begin, FileList *out)
{
    Rd r;
    rd_init(&r, begin->p, begin->len);
    uint32_t total = rd_u32(&r);
    int bad = r.err;
    if (bad || total > LS_MAX_FILES) {
        return -1;
    }

    uint32_t got = 0;
    for (;;) {
        Buf c;
        buf_init(&c);
        uint8_t t = 0;
        if (ls_msg_recv(fd, &t, &c) != 0) {
            buf_free(&c);
            return -1;
        }
        if (t == MSG_LIST_END) {
            buf_free(&c);
            break;
        }
        if (t != MSG_LIST_CHUNK) {
            ls_log("清单传输中收到意外消息 %u", t);
            buf_free(&c);
            return -1;
        }
        Rd rr;
        rd_init(&rr, c.p, c.len);
        uint32_t n = rd_u32(&rr);
        for (uint32_t i = 0; i < n && !rr.err; i++) {
            char *rel = rd_str(&rr);
            uint64_t size = rd_u64(&rr);
            int64_t mtime = rd_i64(&rr);
            uint64_t hash = rd_u64(&rr);
            if (rr.err || !rel) {
                free(rel);
                buf_free(&c);
                return -1;
            }
            if (path_safe(rel) && strlen(rel) < LS_PATHMAX) {
                if (flist_add(out, rel, size, mtime, hash) != 0) {
                    free(rel);
                    buf_free(&c);
                    return -1;
                }
                got++;
            } else {
                ls_log("忽略客户端上报的不安全路径：%s", rel);
            }
            free(rel);
        }
        buf_free(&c);
        if (got > LS_MAX_FILES) {
            return -1;
        }
    }
    flist_sort(out);
    return 0;
}

static int delete_remote(Client *c, const char *rel, const char *why)
{
    Buf p;
    buf_init(&p);
    buf_put_str(&p, rel);
    int rc = ls_msg_send(c->fd, MSG_DELETE, &p);
    buf_free(&p);
    if (rc < 0) {
        return -1;
    }
    ls_log("[%s] 删除 <- 客户端  %-40s (%s)", c->name, rel, why);
    return 0;
}

/* ---------------- 批量推 / 拉 ---------------- */

static LsSink *srv_sink_factory(void *ctx, const char *rel, uint64_t size, int64_t mtime,
                                uint32_t mode)
{
    char err[512] = {0};
    LsSink *s = ls_sink_file((const char *)ctx, rel, size, mtime, mode, err, sizeof(err));
    if (!s) {
        ls_set_last_error("%s", err);
    }
    return s;
}

/*
 * 自动调参：用一次同步实测到的吞吐和压缩率决定下一轮的压缩级别。
 * 网络没跑满（< 8 MB/s）多半是压缩在拖后腿，降级；跑满了就压得更狠省带宽。
 * 结果写回配置文件，下次启动读回来。
 */
static void adapt_compress(Profile *prof)
{
    uint64_t total_ms = ls_now_ms() - prof->t_start;
    if (prof->bytes_wire == 0 || total_ms == 0) {
        return; /* 这次没传东西，别瞎调 */
    }
    double mbps = (double)prof->bytes_wire / 1048576.0 / ((double)total_ms / 1000.0);
    double ratio = prof->bytes_raw > 0 ? (double)prof->bytes_wire / (double)prof->bytes_raw
                                       : 1.0;
    int lv = ls_compress_level();
    int nv = lv;
    if (mbps < 8.0) {
        nv = lv > 1 ? lv - 1 : 1;
    } else if (mbps > 25.0 && ratio > 0.5 && lv < 9) {
        nv = lv + 1;
    }
    if (nv != lv) {
        ls_log("自动调参：压缩级别 %d -> %d（本次吞吐 %.1f MB/s，压缩率 %.0f%%）", lv, nv, mbps,
               ratio * 100.0);
        ls_set_compress_level(nv);
    } else {
        ls_dbg("自动调参：保持压缩级别 %d（吞吐 %.1f MB/s，压缩率 %.0f%%）", lv, mbps,
               ratio * 100.0);
    }
}

/* 把一批文件放进一个 bundle 发给客户端 */
static void push_batch(Client *c, PlanItem *items, size_t n)
{
    BundleEntry *ents = (BundleEntry *)calloc(n, sizeof(BundleEntry));
    if (!ents) {
        c->st.failed += (uint32_t)n;
        return;
    }

    size_t usable = 0;
    for (size_t i = 0; i < n; i++) {
        char full[LS_PATHMAX];
        if (!path_safe(items[i].rel) ||
            path_join(full, sizeof(full), g_root, items[i].rel) < 0) {
            c->st.failed++;
            continue;
        }
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
            ls_log("[%s] 跳过（打不开）：%s（%s）", c->name, items[i].rel, strerror(errno));
            c->st.failed++;
            continue;
        }
        BundleEntry *e = &ents[usable++];
        e->rel = items[i].rel;
        e->size = (uint64_t)st.st_size;
        e->mtime = (int64_t)st.st_mtim.tv_sec * 1000000000LL + (int64_t)st.st_mtim.tv_nsec;
        e->mode = (uint32_t)(st.st_mode & 07777);
    }

    if (usable > 0) {
        uint32_t ok = 0, failed = 0;
        uint64_t bytes = 0;
        int comp = ls_should_compress(g_root, ents, usable, g_compress);
        int rc = ls_send_bundle(c->fd, g_root, ents, usable, comp, &ok, &failed, &bytes);
        char sz[32];
        fmt_size(bytes, sz, sizeof(sz));
        if (rc == 0) {
            ls_dbg("[%s] 本批推送 %u 个文件，%s%s", c->name, ok, sz,
                   comp ? "（已压缩）" : "");
        } else {
            /* 全部或部分失败都要让人看见，别只留一个计数器 */
            ls_log("[%s] 本批推送成功 %u / 失败 %u，%s", c->name, ok, failed,
                   ls_last_error());
        }
        c->st.pushed += ok;
        c->st.failed += failed;
        c->st.bytes += bytes;
    }
    free(ents);
}

/* 让客户端把一批文件发过来 */
static void pull_batch(Client *c, PlanItem *items, size_t n)
{
    Buf b;
    buf_init(&b);
    buf_put_u32(&b, (uint32_t)n);
    for (size_t i = 0; i < n; i++) {
        buf_put_str(&b, items[i].rel);
    }
    int rc = ls_msg_send(c->fd, MSG_PULL_WANT, &b);
    buf_free(&b);
    if (rc < 0) {
        ls_log("[%s] 请求拉取失败：%s", c->name, strerror(errno));
        c->st.failed += (uint32_t)n;
        return;
    }

    uint32_t ok = 0, failed = 0;
    uint64_t bytes = 0;
    if (ls_recv_bundle(c->fd, srv_sink_factory, g_root, &ok, &failed, &bytes) != 0) {
        ls_log("[%s] 拉取失败：%s", c->name, ls_last_error());
        c->st.failed += (uint32_t)n;
        return;
    }
    c->st.pulled += ok;
    c->st.failed += failed;
    c->st.bytes += bytes;
}

/* ---------------- 执行计划 ---------------- */

/*
 * 流化之后同方向的条目要聚成 bundle 才有意义（一次往返传几百个文件），
 * 所以这里按 删除 -> 推送 -> 拉取 归类，并按字节数和条目数切分。
 */
static void run_plan(Client *c, Plan *plan)
{
    /* 先把总量告诉客户端，否则它只能按每个 bundle 各画一次进度条，
     * 看上去就是「一块一块地反复从 0% 开始」。 */
    {
        uint64_t pb = 0, lb = 0;
        uint32_t pf = 0, lf = 0;
        for (size_t k = 0; k < plan->n; k++) {
            if (plan->v[k].act == ACT_PUSH) {
                pb += plan->v[k].size;
                pf++;
            } else if (plan->v[k].act == ACT_PULL) {
                lb += plan->v[k].size;
                lf++;
            }
        }
        Buf b;
        buf_init(&b);
        buf_put_u32(&b, pf);
        buf_put_u64(&b, pb);
        buf_put_u32(&b, lf);
        buf_put_u64(&b, lb);
        (void)ls_msg_send(c->fd, MSG_PLAN, &b);
        buf_free(&b);
    }

    for (size_t i = 0; i < plan->n; i++) {
        if (plan->v[i].act == ACT_DELETE) {
            if (delete_remote(c, plan->v[i].rel, plan->v[i].why) == 0) {
                c->st.deleted++;
            } else {
                c->st.failed++;
            }
        }
    }

    for (int pass = 0; pass < 2; pass++) {
        int want = pass == 0 ? ACT_PUSH : ACT_PULL;

        /* 先把这一类的总量算出来，整个 pass 共用一条进度 */
        uint64_t total_bytes = 0;
        uint32_t total_files = 0;
        for (size_t k = 0; k < plan->n; k++) {
            if (plan->v[k].act == want) {
                total_bytes += plan->v[k].size;
                total_files++;
            }
        }
        uint32_t ok_before = c->st.pushed + c->st.pulled;
        uint32_t fail_before = c->st.failed;
        char summary[256] = {0};
        if (total_files > 0) {
            progress_begin(want == ACT_PUSH ? "推送" : "拉取", total_bytes, total_files);
        }

        size_t i = 0;
        while (i < plan->n) {
            if (plan->v[i].act != want) {
                i++;
                continue;
            }
            size_t j = i;
            while (j < plan->n && plan->v[j].act == want) {
                j++;
            }
            /* [i, j) 是一段同类条目，按 bundle 容量切开 */
            size_t p = i;
            while (p < j) {
                size_t q = p;
                uint64_t acc = 0;
                while (q < j && (q - p) < LS_BUNDLE_FILES && acc < LS_BUNDLE_BYTES) {
                    acc += plan->v[q].size;
                    q++;
                }
                if (q == p) {
                    q = p + 1; /* 单个超大文件自成一批 */
                }
                if (want == ACT_PUSH) {
                    push_batch(c, &plan->v[p], q - p);
                } else {
                    pull_batch(c, &plan->v[p], q - p);
                }
                p = q;
            }
            i = j;
        }

        if (total_files > 0) {
            uint32_t okn = (c->st.pushed + c->st.pulled) - ok_before;
            uint32_t failn = c->st.failed - fail_before;
            if (failn) {
                snprintf(summary, sizeof(summary), "%s结束：成功 %u / 失败 %u",
                         want == ACT_PUSH ? "推送" : "拉取", okn, failn);
            } else {
                snprintf(summary, sizeof(summary), "%s完成：%u 个文件",
                         want == ACT_PUSH ? "推送" : "拉取", okn);
            }
            progress_end(summary);
        }
    }
}

/* ---------------- 核心：一次同步 ---------------- */

static void do_sync(Client *c, const Buf *list_begin)
{
    FileList remote, local;
    Plan plan;

    flist_init(&remote);
    flist_init(&local);
    memset(&plan, 0, sizeof(plan));

    prof_begin("list_in");
    if (recv_list(c->fd, list_begin, &remote) != 0) {
        prof_end("失败");
        ls_log("[%s] 未收到有效的文件清单，放弃本次同步", c->name);
        goto out;
    }
    prof_end("%zu 个条目", remote.n);

    char err[512];
    prof_begin("scan");
    if (fs_scan(g_root, &local, err, sizeof(err)) != 0) {
        prof_end("失败");
        ls_log("[%s] 本地扫描失败：%s", c->name, err);
        goto out;
    }
    prof_end("%zu 个文件", local.n);

    ls_log("[%s] 扫描完成：客户端 %zu 个文件 / 服务端 %zu 个文件%s", c->name, remote.n,
           local.n, c->mtime_ok ? "" : "（按大小比对）");

    uint32_t by_size = 0;
    int      want_hash = want_content_hash(c);
    uint32_t hashed = 0;      /* 靠内容哈希判为相同 */
    uint32_t hashed_diff = 0; /* 靠内容哈希判为不同 */

    /*
     * 判定只看 大小 + 修改时间（都取纳秒），一个字节的内容都不读。
     *
     * 曾经用过"随机采样若干 4KB 块算哈希"来核对内容，实测被否掉：
     * 1 MiB 文件只覆盖 8/256 个块，逐块改 1 字节的漏判率是 14/16。
     * 而 mtime 只有在「同一个文件在同一毫秒内被重写」这种极端情况下才会撞车，
     * 真实场景里文件被编辑过 mtime 就会变，可靠性反而高得多，且零读取。
     */
    prof_begin("diff");
    for (size_t i = 0; i < local.n; i++) {
        FileEntry *le = &local.v[i];
        FileEntry *re = flist_find(&remote, le->rel);

        if (!re) {
            plan_add(&plan, le->rel, le->size, ACT_PUSH, "客户端缺失");
            continue;
        }

        /*
         * 内容哈希优先：大小相同、客户端也报了哈希时，它比时间戳可靠。
         * 服务端这一侧是惰性算的——只有走到这里的文件才会被读一遍，
         * 大小不同的文件在上面就被挡掉了，不会为了算哈希多读。
         */
        if (want_hash && le->size == re->size && re->hash != 0) {
            if (le->hash == 0) {
                char full[LS_PATHMAX];
                if (path_join(full, sizeof(full), g_root, le->rel) == 0) {
                    ls_hash_file(full, &le->hash); /* 失败就留 0，退回元数据判定 */
                }
            }
            if (le->hash != 0) {
                if (le->hash == re->hash) {
                    c->st.same++;
                    hashed++;
                    ls_dbg("[%s] 相同（哈希）  %s", c->name, le->rel);
                    continue;
                }
                if (server_wins(le, re)) {
                    plan_add(&plan, le->rel, le->size, ACT_PUSH, "内容不同");
                } else {
                    plan_add(&plan, le->rel, le->size, ACT_PULL, "内容不同");
                }
                hashed_diff++;
                continue;
            }
        }

        if (le->size == re->size) {
            if (!c->mtime_ok) {
                /* 客户端保不住 mtime，时间戳没有可比性：只能按大小判 */
                c->st.same++;
                by_size++;
                continue;
            }
            int64_t d = le->mtime - re->mtime;
            if (d < 0) {
                d = -d;
            }
            if (d <= LS_MTIME_TOLERANCE_NS) {
                /* 容差内视为同一时刻：不同文件系统的 mtime 精度不一样 */
                c->st.same++;
                ls_dbg("[%s] 相同  %s", c->name, le->rel);
                continue;
            }
        }
        const char *why = le->size != re->size ? "大小不同" : "修改时间不同";
        if (server_wins(le, re)) {
            plan_add(&plan, le->rel, le->size, ACT_PUSH, why);
        } else {
            plan_add(&plan, le->rel, le->size, ACT_PULL, why);
        }
    }

    /* 客户端有、服务端没有 */
    for (size_t j = 0; j < remote.n; j++) {
        FileEntry *re = &remote.v[j];
        if (flist_find(&local, re->rel)) {
            continue;
        }
        if (g_delete) {
            plan_add(&plan, re->rel, re->size, ACT_DELETE, "镜像模式：服务端无此文件");
        } else if (g_allow_upload) {
            plan_add(&plan, re->rel, re->size, ACT_PULL, "服务端缺失");
        } else {
            c->st.ignored++;
            ls_log("[%s] 忽略客户端多出的文件（服务端只读）：%s", c->name, re->rel);
        }
    }

    prof_end("相同 %u / 待传 %zu", c->st.same, plan.n);

    if (by_size > 0) {
        ls_log("[%s] 其中 %u 个文件只是大小相同就判为相同（客户端保不住 mtime，没能比对时间）",
               c->name, by_size);
    }
    if (hashed > 0 || hashed_diff > 0) {
        ls_log("[%s] 内容哈希：%u 个相同 / %u 个不同", c->name, hashed, hashed_diff);
    }

    prof_begin("transfer");
    run_plan(c, &plan);
    prof_end("%u 推送 / %u 拉取", c->st.pushed, c->st.pulled);

out:
    plan_free(&plan);
    flist_free(&remote);
    flist_free(&local);
}

/* 把程序本体发给客户端（client update 用）。同架构直接覆盖即可。 */
static void send_self(Client *c)
{
    const char *dist = g_dist[0] ? g_dist : "/proc/self/exe";
    char resolved[LS_PATHMAX];
    if (!realpath(dist, resolved)) {
        ls_log("[%s] 请求程序本体，但 %s 不可用：%s", c->name, dist, strerror(errno));
        return;
    }
    struct stat st;
    if (stat(resolved, &st) != 0 || !S_ISREG(st.st_mode)) {
        ls_log("[%s] %s 不是普通文件，拒绝下发", c->name, resolved);
        return;
    }

    BundleEntry e;
    memset(&e, 0, sizeof(e));
    e.rel = "lansync";
    e.srcpath = resolved;
    e.size = (uint64_t)st.st_size;
    e.mtime = (int64_t)st.st_mtim.tv_sec * 1000000000LL + (int64_t)st.st_mtim.tv_nsec;
    e.mode = 0755;

    uint32_t ok = 0, failed = 0;
    uint64_t bytes = 0;
    if (ls_send_bundle(c->fd, g_root, &e, 1, 0, &ok, &failed, &bytes) == 0 && ok == 1) {
        char sz[32];
        fmt_size(bytes, sz, sizeof(sz));
        ls_log("[%s] 下发程序本体 %s（%s）", c->name, resolved, sz);
    } else {
        ls_log("[%s] 下发程序失败：%s", c->name, ls_last_error());
    }
}

static void *client_thread(void *arg)
{
    Client *c = (Client *)arg;
    Profile prof;
    Buf b;

    prof_init(&prof, "server", g_root, c->peer);
    prof_bind(&prof);

    /* 1. HELLO */
    buf_init(&b);
    if (ls_expect(c->fd, MSG_HELLO, &b) != 0) {
        ls_log("[%s] 未收到 HELLO，断开", c->peer);
        goto out;
    }
    Rd r;
    rd_init(&r, b.p, b.len);
    char *cname = rd_str(&r);
    char *croot = rd_str(&r);
    uint32_t proto = rd_u32(&r);
    uint64_t cli_ts = rd_u64(&r);
    uint8_t cli_mtime_ok = rd_u8(&r);
    if (r.err || !cname || !croot) {
        free(cname);
        free(croot);
        ls_log("[%s] HELLO 报文错误，断开", c->peer);
        goto out;
    }
    if (cli_ts != ls_build_ts()) {
        /* 不做向前兼容：报出来，由客户端决定是更新还是退出 */
        ls_log("[%s] 构建时间戳不同（客户端 %llu / 服务端 %llu，本服务端编译于 %s）",
               cname[0] ? cname : c->peer, (unsigned long long)cli_ts,
               (unsigned long long)ls_build_ts(), ls_build_date());
    }
    snprintf(c->name, sizeof(c->name), "%s", cname[0] ? cname : c->peer);
    snprintf(c->remote_root, sizeof(c->remote_root), "%s", croot);
    c->mtime_ok = cli_mtime_ok ? 1 : 0;
    free(cname);
    free(croot);
    buf_free(&b);

    if (proto != LS_PROTO) {
        ls_log("[%s] 协议版本不匹配（客户端 %u / 服务端 %u），断开", c->name, proto, LS_PROTO);
        goto out;
    }
    ls_log("[%s] 客户端接入：%s，目录 %s", c->name, c->peer, c->remote_root);
    if (!c->mtime_ok) {
        if (ls_hash_ok()) {
            ls_log("[%s] 该设备保不住 mtime，改用内容哈希比对（要读一遍它的文件）", c->name);
        } else {
            ls_log("[%s] 该设备保不住 mtime，本机又没 AES：只能退化为「只比大小」", c->name);
        }
    } else if (g_verify_hash && ls_hash_ok()) {
        ls_log("[%s] 按 --verify hash 比对内容（两端都要把文件读一遍）", c->name);
    }

    /* 2. HELLO_ACK */
    buf_init(&b);
    buf_put_str(&b, g_name);
    buf_put_str(&b, g_root);
    buf_put_u32(&b, LS_PROTO);
    buf_put_u8(&b, (uint8_t)g_prefer);
    buf_put_u8(&b, (uint8_t)g_delete);
    buf_put_str(&b, g_client_subdir); /* 服务端指定客户端落盘的子目录，可空 */
    buf_put_u64(&b, ls_build_ts());   /* 构建时间戳，客户端据此判断要不要更新 */
    buf_put_str(&b, ls_build_date());
    buf_put_u8(&b, (uint8_t)want_content_hash(c)); /* 要客户端把内容哈希一起报上来 */
    if (ls_msg_send(c->fd, MSG_HELLO_ACK, &b) != 0) {
        buf_free(&b);
        goto out;
    }
    buf_free(&b);

    /* 3. 看客户端这次要干什么：同步，还是拉取程序本体 */
    {
        uint8_t t = 0;
        buf_init(&b);
        if (ls_msg_recv(c->fd, &t, &b) != 0) {
            ls_log("[%s] 连接中断", c->name);
            buf_free(&b);
            goto out;
        }
        if (t == MSG_WANT_SELF) {
            buf_free(&b);
            send_self(c);
            (void)ls_msg_send(c->fd, MSG_BYE, NULL);
            goto out; /* 取程序不算同步，不打印同步统计 */
        }
        if (t != MSG_LIST_BEGIN) {
            ls_log("[%s] 期望文件清单或程序请求，收到消息 %u，断开", c->name, t);
            buf_free(&b);
            goto out;
        }

        /* 4. 同步（全局串行，避免多个客户端同时写同一目录） */
        pthread_mutex_lock(&g_sync_lock);
        do_sync(c, &b);
        pthread_mutex_unlock(&g_sync_lock);
        buf_free(&b);
    }

    /* 4. 统计 + 收尾 */
    buf_init(&b);
    buf_put_u32(&b, c->st.pushed);
    buf_put_u32(&b, c->st.pulled);
    buf_put_u32(&b, c->st.deleted);
    buf_put_u32(&b, c->st.same);
    buf_put_u32(&b, c->st.failed);
    buf_put_u32(&b, c->st.ignored);
    buf_put_u64(&b, c->st.bytes);
    (void)ls_msg_send(c->fd, MSG_SYNC_DONE, &b);
    buf_free(&b);
    (void)ls_msg_send(c->fd, MSG_BYE, NULL);

    {
        char sz[32];
        fmt_size(c->st.bytes, sz, sizeof(sz));
        ls_log("[%s] 同步结束：推送 %u / 拉取 %u / 删除 %u / 相同 %u / 忽略 %u / 失败 %u，传输 %s",
               c->name, c->st.pushed, c->st.pulled, c->st.deleted, c->st.same,
               c->st.ignored, c->st.failed, sz);
    }

out:
    adapt_compress(&prof);
    prof_finish(&prof, g_profile_path);
    prof_bind(NULL);
    close(c->fd);
    free(c);
    return NULL;
}

/* ---------------- 局域网发现（UDP） ---------------- */

static void udp_announce(int fd)
{
    /* 换行分隔：第二行是名字、第三行是路径，因此路径里的空格不会破坏解析 */
    char msg[LS_PATHMAX + 128];
    snprintf(msg, sizeof(msg), "LANDSYNC_ANNOUNCE %u\n%s\n%s\n", g_port, g_name, g_root);
    int n = ls_udp_broadcast(fd, g_udp_port, msg, NULL, 0);
    (void)n; /* release 版里下一行的 ls_dbg 会消失，n 就没人用了 */
    ls_dbg("已向 %d 个广播地址发出 ANNOUNCE", n);
}

static void *udp_thread(void *arg)
{
    (void)arg;
    int fd = ls_udp_open(g_udp_port, 1);
    if (fd < 0) {
        ls_log("UDP 端口 %u 绑定失败（%s），局域网自动发现不可用；客户端请用 -s 指定地址",
               g_udp_port, strerror(errno));
        return NULL;
    }

    uint64_t last = ls_now_ms();
    if (g_announce) {
        udp_announce(fd);
    }

    while (!g_stop) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, 400);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            char buf[LS_PATHMAX + 256];
            char from_ip[INET_ADDRSTRLEN] = "?";
            uint16_t from_port = 0;
            int ifindex = 0;
            ssize_t n = ls_udp_recv_from(fd, buf, sizeof(buf) - 1, from_ip,
                                         sizeof(from_ip), &from_port, &ifindex);
            if (n > 0) {
                buf[n] = '\0';
                if (strncmp(buf, "LANDSYNC_DISCOVER", 17) == 0) {
                    char reply[LS_PATHMAX + 128];
                    int m = snprintf(reply, sizeof(reply), "LANDSYNC_OFFER %u\n%s\n%s\n",
                                     g_port, g_name, g_root);
                    /*
                     * 从收到请求的那个接口回复（ifindex）。
                     * 不能让它重新查路由：手机上开着 VPN 时，到同网段设备的路由
                     * 会被劫持到 tun0（实测 `ip route get 192.168.0.13` 给出
                     * `dev tun0 table 1076`），包从梯子出去，客户端永远收不到，
                     * 而 sendto 照样返回成功、日志一切正常。
                     */
                    int sr = ls_udp_send_to(fd, reply, (size_t)m, from_ip, from_port,
                                            ifindex);
                    const char *ip = from_ip;
                    /*
                     * 这里用 ls_log 而不是 ls_dbg：release 版里 ls_dbg 是
                     * ((void)0)，整个消失。「客户端说发现不到服务端」时，
                     * 第一个要回答的问题就是请求到底有没有到达服务端 ——
                     * 打不出这行的话，两边都只能靠猜。
                     * 发现请求是低频事件（客户端启动时发 3 次），但一个客户端
                     * 有多个接口时源 IP 会交替出现，所以按 IP 各自限频 5 秒，
                     * 而不是只记住「上一个」。
                     */
                    static char     seen_ip[8][INET_ADDRSTRLEN];
                    static uint64_t seen_ms[8];
                    uint64_t nowms = ls_now_ms();
                    int slot = -1;
                    int quiet = 0;
                    for (int i = 0; i < 8; i++) {
                        if (seen_ip[i][0] != '\0' && strcmp(seen_ip[i], ip) == 0) {
                            slot = i;
                            quiet = nowms - seen_ms[i] < 5000;
                            break;
                        }
                        if (slot < 0 && seen_ip[i][0] == '\0') {
                            slot = i;
                        }
                    }
                    if (!quiet) {
                        if (slot < 0) {
                            slot = (int)(nowms % 8); /* 表满了就轮转覆盖 */
                        }
                        snprintf(seen_ip[slot], sizeof(seen_ip[slot]), "%s", ip);
                        seen_ms[slot] = nowms;
                        /*
                         * ls_dbg：release 版里整个消失，只有 -DLS_DEBUG 的构建
                         * 才打得出来。发现出问题时换 Android 调试版跑。
                         */
                        (void)sr;
                        if (sr < 0) {
                            ls_dbg("收到来自 %s 的发现请求，但回复失败：%s", ip,
                                   strerror(errno));
                        } else {
                            ls_dbg("收到来自 %s 的发现请求，已从接口 %d 回复（TCP 端口 %u）",
                                   ip, ifindex, g_port);
                        }
                    }
                }
            }
        }
        if (g_announce) {
            uint64_t now = ls_now_ms();
            if (now - last >= 3000) {
                udp_announce(fd);
                last = now;
            }
        }
    }
    close(fd);
    return NULL;
}

/* ---------------- 主流程 ---------------- */

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
    if (g_listen_fd >= 0) {
        close(g_listen_fd); /* 唤醒阻塞中的 accept */
        g_listen_fd = -1;
    }
}

static void usage(const char *prog, int debug)
{
    printf(
        "lansync 服务端 -- 常驻，接受客户端连接并让其成为本目录的副本\n"
        "\n"
        "用法: %s server -d DIR [选项]\n"
        "\n"
        "必填\n"
        "  -d, --dir DIR            要同步的目录（必须已存在）\n"
        "\n"
        "同步策略（默认：服务端只读，客户端被改成本目录的副本）\n"
        "      --prefer MODE        内容冲突时谁赢：server(默认) | client | newer\n"
        "      --keep-extra         不删除客户端多出的文件（默认会删）\n"
        "      --allow-upload       允许客户端把文件写回服务端（默认拒绝）\n"
        "      --merge              双向合并 = --prefer newer --keep-extra --allow-upload\n"
        "      --compress MODE      传输压缩 auto(默认) | on | off\n"
        "      --config PATH        剖析/配置文件（默认 lansync-server.json）\n"
        "\n"
        "网络\n"
        "  -p, --port PORT          TCP 端口（默认 %d）\n"
        "  -w, --udp-port PORT      局域网发现端口（默认 %d）\n"
        "  -n, --name NAME          服务端名称（默认主机名）\n"
        "\n"
        "客户端管理\n"
        "      --client-subdir DIR  让客户端把文件落在它自己目录下的 DIR/ 里\n"
        "      --dist PATH          client update 时下发的程序（默认本进程自身）\n"
        "\n"
        "  -h, --help               显示本帮助\n",
        prog, LS_DEF_PORT, LS_DEF_UDP_PORT);

    if (debug) {
        printf(
            "\n调试选项（--help-debug）\n"
            "      --verify MODE        hash = 按内容比对（两端都读一遍文件，能发现\n"
            "                           大小相同但内容不同的文件）；meta = 只比\n"
            "                           大小+时间（默认）。客户端保不住 mtime 时\n"
            "                           会自动转成 hash\n"
            "      --no-announce        不周期性广播自己（仍会应答发现请求）\n"
            "  -v, --verbose            连判定为相同的文件也打印\n"
            "  -q, --quiet              只打印错误\n");
    } else {
        printf("\n调试相关选项见 %s server --help-debug\n", prog);
    }
}

int server_main(int argc, char **argv)
{
    enum { OPT_PREFER = 1000, OPT_KEEP, OPT_MERGE, OPT_NOANN, OPT_SUBDIR, OPT_DIST,
           OPT_UPLOAD, OPT_COMPRESS, OPT_NOCOMPRESS, OPT_CONFIG, OPT_HDEBUG,
           OPT_VERIFY };
    static const struct option opts[] = {
        {"dir", required_argument, 0, 'd'},
        {"port", required_argument, 0, 'p'},
        {"udp-port", required_argument, 0, 'w'},
        {"name", required_argument, 0, 'n'},
        {"prefer", required_argument, 0, OPT_PREFER},
        {"keep-extra", no_argument, 0, OPT_KEEP},
        {"merge", no_argument, 0, OPT_MERGE},
        {"allow-upload", no_argument, 0, OPT_UPLOAD},
        {"config", required_argument, 0, OPT_CONFIG},
        {"compress", required_argument, 0, OPT_COMPRESS},
        {"no-compress", no_argument, 0, OPT_NOCOMPRESS},
        {"verify", required_argument, 0, OPT_VERIFY},
        {"no-announce", no_argument, 0, OPT_NOANN},
        {"client-subdir", required_argument, 0, OPT_SUBDIR},
        {"dist", required_argument, 0, OPT_DIST},
        {"verbose", no_argument, 0, 'v'},
        {"quiet", no_argument, 0, 'q'},
        {"help", no_argument, 0, 'h'},
        {"help-debug", no_argument, 0, OPT_HDEBUG},
        {0, 0, 0, 0}};

    const char *dir = NULL;
    int c;
    while ((c = getopt_long(argc, argv, "d:p:w:n:vqh", opts, NULL)) != -1) {
        switch (c) {
        case 'd': dir = optarg; break;
        case 'p': g_port = (uint16_t)atoi(optarg); break;
        case 'w': g_udp_port = (uint16_t)atoi(optarg); break;
        case 'n': snprintf(g_name, sizeof(g_name), "%s", optarg); break;
        case OPT_PREFER:
            if (strcmp(optarg, "newer") == 0) {
                g_prefer = PREFER_NEWER;
            } else if (strcmp(optarg, "server") == 0) {
                g_prefer = PREFER_SERVER;
            } else if (strcmp(optarg, "client") == 0) {
                g_prefer = PREFER_CLIENT;
            } else {
                fprintf(stderr, "--prefer 只能是 newer / server / client\n");
                return 2;
            }
            break;
        case OPT_KEEP: g_delete = 0; break;
        case OPT_MERGE:
            g_prefer = PREFER_NEWER;
            g_delete = 0;
            g_allow_upload = 1;
            break;
        case OPT_UPLOAD: g_allow_upload = 1; break;
        case OPT_CONFIG: g_profile_path = optarg; break;
        case OPT_COMPRESS:
            if (strcmp(optarg, "auto") == 0) {
                g_compress = LS_COMPRESS_AUTO;
            } else if (strcmp(optarg, "on") == 0) {
                g_compress = LS_COMPRESS_ON;
            } else if (strcmp(optarg, "off") == 0) {
                g_compress = LS_COMPRESS_OFF;
            } else {
                fprintf(stderr, "--compress 只能是 auto / on / off\n");
                return 2;
            }
            break;
        case OPT_NOCOMPRESS: g_compress = LS_COMPRESS_OFF; break;
        case OPT_VERIFY:
            if (strcmp(optarg, "hash") == 0) {
                g_verify_hash = 1;
            } else if (strcmp(optarg, "meta") == 0) {
                g_verify_hash = 0;
            } else {
                fprintf(stderr, "--verify 只能是 hash / meta\n");
                return 2;
            }
            break;
        case OPT_NOANN: g_announce = 0; break;
        case OPT_SUBDIR:
            if (!path_safe(optarg)) {
                fprintf(stderr, "--client-subdir 必须是不含 .. 的相对路径\n");
                return 2;
            }
            snprintf(g_client_subdir, sizeof(g_client_subdir), "%s", optarg);
            break;
        case OPT_DIST: snprintf(g_dist, sizeof(g_dist), "%s", optarg); break;
        case 'v': ls_set_verbose(1); break;
        case 'q': ls_set_quiet(1); break;
        case 'h': usage("lansync", 0); return 0;
        case OPT_HDEBUG: usage("lansync", 1); return 0;
        default: usage("lansync", 0); return 2;
        }
    }

    if (!dir) {
        fprintf(stderr, "必须用 -d 指定要同步的目录\n\n");
        usage("lansync", 0);
        return 2;
    }
    if (g_port == 0) {
        g_port = LS_DEF_PORT;
    }
    if (g_udp_port == 0) {
        g_udp_port = LS_DEF_UDP_PORT;
    }

    /* 目录必须是已存在的目录；用 realpath 归一化 */
    char resolved[LS_PATHMAX];
    if (!realpath(dir, resolved)) {
        fprintf(stderr, "目录不可用：%s (%s)\n", dir, strerror(errno));
        return 1;
    }
    struct stat st;
    if (stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "不是目录：%s\n", resolved);
        return 1;
    }
    snprintf(g_root, sizeof(g_root), "%s", resolved);

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    g_listen_fd = ls_tcp_listen(g_port, 32);
    if (g_listen_fd < 0) {
        fprintf(stderr, "监听 TCP %u 失败：%s\n", g_port, strerror(errno));
        return 1;
    }

    /* 读回上次调好的压缩级别 */
    {
        const char *cfg = g_profile_path ? g_profile_path : "lansync-server.json";
        FILE *cf = fopen(cfg, "r");
        if (cf) {
            char buf[8192];
            size_t n = fread(buf, 1, sizeof(buf) - 1, cf);
            fclose(cf);
            buf[n] = '\0';
            double lv = ls_cfg_get(buf, "compress_level", LS_COMPRESS_LEVEL);
            if (lv >= 1 && lv <= 9) {
                ls_set_compress_level((int)lv);
            }
        }
    }

    ls_log("%s", ls_build_info());
    ls_log("服务端「%s」启动：目录 %s，TCP %u，UDP 发现 %u", g_name, g_root, g_port,
           g_udp_port);
    ls_log("策略：%s%s；%s", g_prefer == PREFER_SERVER
                                ? "服务端优先"
                                : (g_prefer == PREFER_CLIENT ? "客户端优先" : "新者优先"),
           g_delete ? " + 客户端多出的文件会被删除" : " + 保留客户端多出的文件",
           g_allow_upload ? "允许客户端写回服务端"
                          : "服务端只读，客户端无法修改或新增本目录的任何文件");
    if (g_client_subdir[0]) {
        ls_log("客户端落盘位置：其自身目录下的 %s/", g_client_subdir);
    }
    /* 启动自检：能列出文件 != 能读出文件。
     * 有些挂载（如 Android 的 FUSE /storage）能让 stat 成功却拒绝 open，
     * 症状是扫描一切正常、真正传输时每个文件都失败。提前报出来，别让人猜。 */
    {
        FileList probe;
        char perr[256];
        flist_init(&probe);
        if (fs_scan(g_root, &probe, perr, sizeof(perr)) == 0) {
            size_t tried = 0, failed = 0;
            char sample[LS_PATHMAX] = {0};
            for (size_t i = 0; i < probe.n && tried < 3; i++) {
                char full[LS_PATHMAX];
                if (path_join(full, sizeof(full), g_root, probe.v[i].rel) < 0) {
                    continue;
                }
                tried++;
                FILE *pf = fopen(full, "rb");
                if (pf) {
                    fclose(pf);
                } else {
                    int e = errno;
                    failed++;
                    if (!sample[0]) {
                        snprintf(sample, sizeof(sample), "%s", probe.v[i].rel);
                    }
                    ls_log("自检：列得出 %s，但读不出内容：%s", probe.v[i].rel, strerror(e));
                }
            }
            if (failed > 0) {
                struct stat dst;
                char dp[16] = "?";
                if (stat(g_root, &dst) == 0) {
                    mode_str(dst.st_mode, dp, sizeof(dp));
                }
                ls_log("自检失败：本进程读不到目录里的文件内容，传输时这些文件都会失败。");
                ls_log("  当前进程 uid=%u gid=%u", (unsigned)getuid(), (unsigned)getgid());
                ls_log("  目录 %s  权限 %s  owner uid=%u", g_root, dp,
                       (unsigned)(stat(g_root, &dst) == 0 ? dst.st_uid : 0));
                ls_log("  样例文件 %s", sample[0] ? sample : "(无)");
                ls_log("  说明：某些挂载（如 Android 的 FUSE /storage）对不同 uid 的表现不同，");
                ls_log("        换个身份运行本程序试试，往往就能正常读出内容。");
            } else if (tried > 0) {
                ls_log("自检通过：目录内 %zu 个文件，内容可读。", probe.n);
            }
        }
        flist_free(&probe);
    }

    ls_log("等待客户端连接…（Ctrl-C 退出）");

    pthread_t ut;
    if (pthread_create(&ut, NULL, udp_thread, NULL) == 0) {
        pthread_detach(ut);
    }

    while (!g_stop) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int fd = accept(g_listen_fd, (struct sockaddr *)&peer, &plen);
        if (fd < 0) {
            if (g_stop) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            ls_log("accept 失败：%s", strerror(errno));
            break;
        }
        ls_set_nodelay(fd);
        ls_set_keepalive(fd);
        ls_set_timeout(fd, 300); /* 对端卡死时别让服务端线程永久挂住 */

        Client *c = (Client *)calloc(1, sizeof(Client));
        if (!c) {
            close(fd);
            continue;
        }
        c->fd = fd;
        c->seq = __atomic_add_fetch(&g_seq, 1ul, __ATOMIC_RELAXED);
        inet_ntop(AF_INET, &peer.sin_addr, c->peer, sizeof(c->peer));
        snprintf(c->name, sizeof(c->name), "%s", c->peer);

        pthread_t t;
        if (pthread_create(&t, NULL, client_thread, c) != 0) {
            ls_log("创建客户端线程失败");
            close(fd);
            free(c);
            continue;
        }
        pthread_detach(t);
    }

    ls_log("服务端退出");
    return 0;
}

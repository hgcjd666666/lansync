/*
 * client.c -- lansync 客户端
 *
 * 职责：启动后自动在局域网里找服务端（UDP 广播），连上去报到，
 *       把自己的文件清单（相对路径 + 大小 + mtime）上报，
 *       然后按服务端指令完成一次同步，最后退出。
 */
#include "common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    char     ip[64];
    uint16_t port;
    char     name[128];
    char     root[LS_PATHMAX];
} Offer;

typedef struct {
    uint32_t pushed;
    uint32_t pulled;
    uint32_t deleted;
    uint32_t same;
    uint32_t failed;
    uint64_t bytes;
} SyncStats;

/* ---------------- 局域网发现 ---------------- */

/* 报文格式（换行分隔，path 里可以有空格）：
 *   LANDSYNC_OFFER <tcp_port>\n<name>\n<root>\n
 *   LANDSYNC_ANNOUNCE <tcp_port>\n<name>\n<root>\n                       */
static int parse_offer(const char *buf, Offer *o)
{
    if (strncmp(buf, "LANDSYNC_OFFER", 14) != 0 &&
        strncmp(buf, "LANDSYNC_ANNOUNCE", 17) != 0) {
        return -1;
    }
    const char *sp = strchr(buf, ' ');
    if (!sp) {
        return -1;
    }
    int port = atoi(sp + 1);
    if (port <= 0 || port > 65535) {
        return -1;
    }
    o->port = (uint16_t)port;

    const char *name = strchr(sp, '\n');
    if (!name) {
        return -1;
    }
    name++;
    const char *nl = strchr(name, '\n');
    if (!nl) {
        return -1;
    }
    size_t nlen = (size_t)(nl - name);
    if (nlen >= sizeof(o->name)) {
        nlen = sizeof(o->name) - 1;
    }
    memcpy(o->name, name, nlen);
    o->name[nlen] = '\0';

    const char *root = nl + 1;
    const char *nl2 = strchr(root, '\n');
    size_t rlen = nl2 ? (size_t)(nl2 - root) : strlen(root);
    if (rlen >= sizeof(o->root)) {
        rlen = sizeof(o->root) - 1;
    }
    memcpy(o->root, root, rlen);
    o->root[rlen] = '\0';
    return 0;
}

static int discover(Offer *out, uint16_t udp_port, int timeout_ms, const char *filter)
{
    int fd = ls_udp_open(0, 1);
    if (fd < 0) {
        ls_log("无法创建 UDP socket：%s", strerror(errno));
        return -1;
    }

    for (int i = 0; i < 3; i++) {
        int n = ls_udp_broadcast(fd, udp_port, "LANDSYNC_DISCOVER 1\n");
        if (i == 0 && n <= 0) {
            ls_log("UDP 广播发送失败：%s", strerror(errno));
        }
        struct pollfd p;
        p.fd = fd;
        p.events = POLLIN;
        p.revents = 0;
        poll(&p, 1, 60);
    }

    uint64_t deadline = ls_now_ms() + (uint64_t)timeout_ms;
    int found = -1;
    while (ls_now_ms() < deadline) {
        uint64_t left = deadline - ls_now_ms();
        struct pollfd p;
        p.fd = fd;
        p.events = POLLIN;
        p.revents = 0;
        int pr = poll(&p, 1, (int)(left > 200 ? 200 : left));
        if (pr <= 0) {
            continue;
        }
        char buf[LS_PATHMAX + 512];
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) {
            continue;
        }
        buf[n] = '\0';
        Offer o;
        memset(&o, 0, sizeof(o));
        if (parse_offer(buf, &o) != 0) {
            continue;
        }
        inet_ntop(AF_INET, &from.sin_addr, o.ip, sizeof(o.ip));
        if (filter && *filter && strcmp(o.name, filter) != 0) {
            continue;
        }
        *out = o;
        found = 0;
        break;
    }
    close(fd);
    return found;
}

/* ---------------- 清单上报（分片，避免超过单条消息上限） ---------------- */

static int send_list(int fd, const FileList *l)
{
    Buf b;
    buf_init(&b);
    buf_put_u32(&b, (uint32_t)l->n);
    int rc = ls_msg_send(fd, MSG_LIST_BEGIN, &b);
    buf_free(&b);
    if (rc < 0) {
        return -1;
    }

    Buf items;
    buf_init(&items);
    uint32_t in_batch = 0;

    for (size_t i = 0; i < l->n; i++) {
        buf_put_str(&items, l->v[i].rel);
        buf_put_u64(&items, l->v[i].size);
        buf_put_i64(&items, l->v[i].mtime);
        in_batch++;

        if (in_batch >= LS_LIST_BATCH || items.len >= LS_LIST_BYTES) {
            Buf p;
            buf_init(&p);
            buf_put_u32(&p, in_batch);
            buf_put_bytes(&p, items.p, items.len);
            rc = ls_msg_send(fd, MSG_LIST_CHUNK, &p);
            buf_free(&p);
            buf_reset(&items);
            in_batch = 0;
            if (rc < 0) {
                buf_free(&items);
                return -1;
            }
        }
    }
    if (in_batch > 0) {
        Buf p;
        buf_init(&p);
        buf_put_u32(&p, in_batch);
        buf_put_bytes(&p, items.p, items.len);
        rc = ls_msg_send(fd, MSG_LIST_CHUNK, &p);
        buf_free(&p);
        if (rc < 0) {
            buf_free(&items);
            return -1;
        }
    }
    buf_free(&items);
    return ls_msg_send(fd, MSG_LIST_END, NULL);
}

/* ---------------- 落盘目标工厂 ---------------- */

typedef struct {
    const char *root;       /* 普通同步：相对它落盘 */
    const char *abs_target; /* update：直接写这个绝对路径 */
} SinkCtx;

static LsSink *cli_sink_factory(void *ctx, const char *rel, uint64_t size, int64_t mtime,
                                uint32_t mode)
{
    SinkCtx *sc = (SinkCtx *)ctx;
    char err[512] = {0};
    LsSink *s;
    if (sc->abs_target) {
        s = ls_sink_abs(sc->abs_target, size, mtime, mode, err, sizeof(err));
    } else {
        s = ls_sink_file(sc->root, rel, size, mtime, mode, err, sizeof(err));
    }
    if (!s) {
        ls_set_last_error("%s", err);
    }
    return s;
}

/* ---------------- 服务端要哪些文件，就打包发过去 ---------------- */

typedef struct {
    uint32_t pushed;
    uint32_t failed;
    uint64_t bytes;
} PullStat;

static void handle_pull_want(int fd, const char *root, const Buf *req, PullStat *out)
{
    Rd r;
    rd_init(&r, req->p, req->len);
    uint32_t n = rd_u32(&r);
    if (r.err || n > LS_MAX_FILES) {
        ls_log("拉取请求报文损坏");
        out->failed++;
        return;
    }

    BundleEntry *ents = (BundleEntry *)calloc(n ? n : 1, sizeof(BundleEntry));
    char **rels = (char **)calloc(n ? n : 1, sizeof(char *));
    if (!ents || !rels) {
        free(ents);
        free(rels);
        out->failed++;
        return;
    }

    size_t usable = 0;
    for (uint32_t i = 0; i < n; i++) {
        char *rel = rd_str(&r);
        if (r.err || !rel) {
            free(rel);
            break;
        }
        rels[i] = rel;
        char full[LS_PATHMAX];
        if (!path_safe(rel) || path_join(full, sizeof(full), root, rel) < 0) {
            out->failed++;
            continue;
        }
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
            ls_log("服务端要的文件本地打不开：%s（%s）", rel, strerror(errno));
            out->failed++;
            continue;
        }
        BundleEntry *e = &ents[usable++];
        e->rel = rel;
        e->size = (uint64_t)st.st_size;
        e->mtime = (int64_t)st.st_mtim.tv_sec * 1000000000LL + (int64_t)st.st_mtim.tv_nsec;
        e->mode = (uint32_t)(st.st_mode & 07777);
    }

    if (usable > 0) {
        uint32_t ok = 0, failed = 0;
        uint64_t bytes = 0;
        int rc = ls_send_bundle(fd, root, ents, usable, &ok, &failed, &bytes);
        if (rc == 0) {
            ls_dbg("上传 -> 服务端  %u 个文件", ok);
            out->pushed += ok;
            out->bytes += bytes;
        } else {
            ls_log("上传失败：%s", ls_last_error());
        }
        out->failed += failed;
    }

    for (uint32_t i = 0; i < n; i++) {
        free(rels[i]);
    }
    free(rels);
    free(ents);
}

/* ---------------- 主流程 ---------------- */

static void usage(const char *prog, int debug)
{
    printf(
        "lansync 客户端 -- 找到服务端，同步一次，然后退出\n"
        "\n"
        "用法: %s client [-d DIR] [-s 服务端地址] [选项]\n"
        "      %s client update [-s 服务端地址] [-o 路径] [选项]\n"
        "\n"
        "同步\n"
        "  -d, --dir DIR            本地目录（默认当前目录，不存在会自动创建）\n"
        "      --server-name S      只接受指定名字的服务端\n"
        "\n"
        "连接\n"
        "  -s, --server HOST        服务端 IP；不给则用 UDP 广播自动发现\n"
        "  -p, --port PORT          服务端 TCP 端口（默认 %d）\n"
        "  -w, --udp-port PORT      局域网发现端口（默认 %d）\n"
        "  -n, --name NAME          本机名称（默认主机名）\n"
        "  -t, --timeout MS         自动发现等待时间（默认 3000 毫秒）\n"
        "\n"
        "update 子命令\n"
        "  -o, --output PATH        保存到 PATH（默认覆盖正在运行的本程序）\n"
        "\n"
        "  -h, --help               显示本帮助\n",
        prog, prog, LS_DEF_PORT, LS_DEF_UDP_PORT);
    if (debug) {
        printf("\n调试选项（--help-debug）\n"
               "  -v, --verbose            详细输出\n"
               "  -q, --quiet              只打印错误\n");
    } else {
        printf("\n调试相关选项见 %s client --help-debug\n", prog);
    }
}

/* ---------------- client update：向服务端索取最新程序 ---------------- */

static void update_usage(const char *prog, int debug)
{
    printf(
        "lansync client update -- 从服务端取回最新程序并替换自己（同架构）\n"
        "\n"
        "用法: %s client update [-s 服务端地址] [-o PATH] [选项]\n"
        "\n"
        "  -o, --output PATH    保存到 PATH（默认覆盖正在运行的本程序）\n"
        "  -s, --server HOST    服务端 IP；不给则自动发现\n"
        "  -p, --port PORT      服务端 TCP 端口（默认 %d）\n"
        "  -w, --udp-port PORT  局域网发现端口（默认 %d）\n"
        "  -n, --name NAME      本机名称\n"
        "  -t, --timeout MS     自动发现等待时间（默认 3000）\n"
        "  -h, --help           显示本帮助\n",
        prog, LS_DEF_PORT, LS_DEF_UDP_PORT);
    if (debug) {
        printf("\n调试选项（--help-debug）\n"
               "  -v, --verbose        详细输出\n"
               "  -q, --quiet          只打印错误\n");
    }
}

static int client_update_main(int argc, char **argv)
{
    enum { OPT_SNAME = 1000, OPT_HDEBUG };
    static const struct option opts[] = {
        {"server", required_argument, 0, 's'},
        {"port", required_argument, 0, 'p'},
        {"udp-port", required_argument, 0, 'w'},
        {"name", required_argument, 0, 'n'},
        {"server-name", required_argument, 0, OPT_SNAME},
        {"timeout", required_argument, 0, 't'},
        {"output", required_argument, 0, 'o'},
        {"verbose", no_argument, 0, 'v'},
        {"quiet", no_argument, 0, 'q'},
        {"help", no_argument, 0, 'h'},
        {"help-debug", no_argument, 0, OPT_HDEBUG},
        {0, 0, 0, 0}};

    const char *server = NULL;
    const char *server_name = NULL;
    const char *out = NULL;
    char myname[128] = {0};
    char server_ip[64] = {0};
    char target[LS_PATHMAX] = {0};
    char dirbuf[LS_PATHMAX] = {0};
    char tmpname[64] = {0};
    char tmpfull[LS_PATHMAX] = {0};
    uint16_t port = LS_DEF_PORT;
    uint16_t udp_port = LS_DEF_UDP_PORT;
    uint16_t server_port = 0;
    int timeout_ms = 3000;
    int fd = -1;
    int rc = 1;
    Buf b;
    Offer o;
    uint64_t bytes = 0;
    int c;

    while ((c = getopt_long(argc, argv, "s:p:w:n:t:o:vqh", opts, NULL)) != -1) {
        switch (c) {
        case 's': server = optarg; break;
        case 'p': port = (uint16_t)atoi(optarg); break;
        case 'w': udp_port = (uint16_t)atoi(optarg); break;
        case 'n': snprintf(myname, sizeof(myname), "%s", optarg); break;
        case OPT_SNAME: server_name = optarg; break;
        case 't': timeout_ms = atoi(optarg); break;
        case 'o': out = optarg; break;
        case 'v': ls_set_verbose(1); break;
        case 'q': ls_set_quiet(1); break;
        case 'h': update_usage("lansync", 0); return 0;
        case OPT_HDEBUG: update_usage("lansync", 1); return 0;
        default: update_usage("lansync", 0); return 2;
        }
    }

    if (port == 0) {
        port = LS_DEF_PORT;
    }
    if (udp_port == 0) {
        udp_port = LS_DEF_UDP_PORT;
    }
    if (!myname[0] && gethostname(myname, sizeof(myname) - 1) != 0) {
        snprintf(myname, sizeof(myname), "client");
    }

    /* 目标：默认覆盖正在运行的自己 */
    if (out) {
        snprintf(target, sizeof(target), "%s", out);
    } else {
        ssize_t k = readlink("/proc/self/exe", target, sizeof(target) - 1);
        if (k <= 0) {
            ls_log("无法确定自身路径，请用 -o 指定输出位置");
            return 1;
        }
        target[k] = '\0';
    }
    {
        char t[LS_PATHMAX];
        char *slash;
        snprintf(t, sizeof(t), "%s", target);
        slash = strrchr(t, '/');
        if (slash) {
            *slash = '\0';
            snprintf(dirbuf, sizeof(dirbuf), "%s", t[0] ? t : "/");
        } else {
            snprintf(dirbuf, sizeof(dirbuf), ".");
        }
    }
    snprintf(tmpname, sizeof(tmpname), ".lansync-new-%d", (int)getpid());
    snprintf(tmpfull, sizeof(tmpfull), "%s/%s", dirbuf, tmpname);

    /* 找服务端 */
    server_port = port;
    if (server && *server) {
        snprintf(server_ip, sizeof(server_ip), "%s", server);
    } else {
        memset(&o, 0, sizeof(o));
        ls_log("正在搜索服务端…");
        if (discover(&o, udp_port, timeout_ms, server_name) != 0) {
            ls_log("没找到服务端，请用 -s 指定 IP");
            return 1;
        }
        snprintf(server_ip, sizeof(server_ip), "%s", o.ip);
        if (o.port) {
            server_port = o.port;
        }
        ls_log("发现服务端「%s」%s:%u", o.name, o.ip, o.port);
    }

    fd = ls_tcp_connect(server_ip, server_port);
    if (fd < 0) {
        ls_log("连接 %s:%u 失败：%s", server_ip, server_port, strerror(errno));
        return 1;
    }

    /* HELLO（update 不涉及目录，随便报一个） */
    buf_init(&b);
    buf_put_str(&b, myname);
    buf_put_str(&b, ".");
    buf_put_u32(&b, LS_PROTO);
    if (ls_msg_send(fd, MSG_HELLO, &b) != 0) {
        buf_free(&b);
        goto done;
    }
    buf_free(&b);

    buf_init(&b);
    if (ls_expect(fd, MSG_HELLO_ACK, &b) != 0) {
        ls_log("服务端没有正常应答");
        buf_free(&b);
        goto done;
    }
    buf_free(&b);

    if (ls_msg_send(fd, MSG_WANT_SELF, NULL) != 0) {
        ls_log("请求发送失败");
        goto done;
    }

    buf_init(&b);
    if (ls_expect(fd, MSG_BUNDLE_BEGIN, &b) != 0) {
        ls_log("服务端没有提供可分发的程序");
        buf_free(&b);
        goto done;
    }
    {
        SinkCtx sc;
        memset(&sc, 0, sizeof(sc));
        sc.abs_target = tmpfull;
        uint32_t okn = 0, failn = 0;
        if (ls_recv_bundle_body(fd, &b, cli_sink_factory, &sc, &okn, &failn, &bytes) != 0 ||
            okn != 1) {
            ls_log("下载失败：%s", ls_last_error());
            buf_free(&b);
            goto done;
        }
    }
    buf_free(&b);

    (void)chmod(tmpfull, 0755);
    if (rename(tmpfull, target) != 0) {
        ls_log("无法替换 %s：%s", target, strerror(errno));
        unlink(tmpfull);
        goto done;
    }
    {
        char sz[32];
        fmt_size(bytes, sz, sizeof(sz));
        ls_log("已更新 %s（%s）", target, sz);
    }
    rc = 0;

done:
    if (fd >= 0) {
        close(fd);
    }
    return rc;
}

int client_main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "update") == 0) {
        return client_update_main(argc - 1, argv + 1);
    }

    enum { OPT_SNAME = 1000, OPT_HDEBUG };
    static const struct option opts[] = {
        {"dir", required_argument, 0, 'd'},
        {"server", required_argument, 0, 's'},
        {"port", required_argument, 0, 'p'},
        {"udp-port", required_argument, 0, 'w'},
        {"name", required_argument, 0, 'n'},
        {"server-name", required_argument, 0, OPT_SNAME},
        {"timeout", required_argument, 0, 't'},
        {"verbose", no_argument, 0, 'v'},
        {"quiet", no_argument, 0, 'q'},
        {"help", no_argument, 0, 'h'},
        {"help-debug", no_argument, 0, OPT_HDEBUG},
        {0, 0, 0, 0}};

    const char *dir = NULL;
    const char *server = NULL;
    const char *server_name = NULL;
    char myname[128] = {0};
    uint16_t port = LS_DEF_PORT;
    uint16_t udp_port = LS_DEF_UDP_PORT;
    int timeout_ms = 3000;

    int c;
    while ((c = getopt_long(argc, argv, "d:s:p:w:n:t:vqh", opts, NULL)) != -1) {
        switch (c) {
        case 'd': dir = optarg; break;
        case 's': server = optarg; break;
        case 'p': port = (uint16_t)atoi(optarg); break;
        case 'w': udp_port = (uint16_t)atoi(optarg); break;
        case 'n': snprintf(myname, sizeof(myname), "%s", optarg); break;
        case OPT_SNAME: server_name = optarg; break;
        case 't': timeout_ms = atoi(optarg); break;
        case 'v': ls_set_verbose(1); break;
        case 'q': ls_set_quiet(1); break;
        case 'h': usage("lansync", 0); return 0;
        case OPT_HDEBUG: usage("lansync", 1); return 0;
        default: usage("lansync", 0); return 2;
        }
    }

    if (!dir) {
        dir = ".";
    }
    if (port == 0) {
        port = LS_DEF_PORT;
    }
    if (udp_port == 0) {
        udp_port = LS_DEF_UDP_PORT;
    }
    if (!myname[0]) {
        if (gethostname(myname, sizeof(myname) - 1) != 0) {
            snprintf(myname, sizeof(myname), "client");
        }
    }

    /* 目录不存在就建出来 */
    char root[LS_PATHMAX];
    struct stat st;
    if (stat(dir, &st) != 0) {
        if (mkdirs_for(dir) != 0 || stat(dir, &st) != 0) {
            fprintf(stderr, "目录不可用：%s (%s)\n", dir, strerror(errno));
            return 1;
        }
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "不是目录：%s\n", dir);
        return 1;
    }
    if (!realpath(dir, root)) {
        fprintf(stderr, "无法解析目录：%s (%s)\n", dir, strerror(errno));
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    /* 1. 找服务端 */
    char server_ip[64] = {0};
    uint16_t server_port = port;
    if (server && *server) {
        snprintf(server_ip, sizeof(server_ip), "%s", server);
    } else {
        ls_log("正在局域网内搜索服务端（UDP %u，最多等 %d ms）…", udp_port, timeout_ms);
        Offer o;
        memset(&o, 0, sizeof(o));
        if (discover(&o, udp_port, timeout_ms, server_name) != 0) {
            ls_log("没找到服务端。请确认服务端已启动，或用 -s 直接指定 IP。");
            return 1;
        }
        snprintf(server_ip, sizeof(server_ip), "%s", o.ip);
        if (o.port) {
            server_port = o.port;
        }
        ls_log("发现服务端「%s」%s:%u（目录 %s）", o.name, o.ip, o.port, o.root);
    }

    /* 2. 连接 */
    int fd = ls_tcp_connect(server_ip, server_port);
    if (fd < 0) {
        ls_log("连接 %s:%u 失败：%s", server_ip, server_port, strerror(errno));
        return 1;
    }
    ls_set_timeout(fd, 300);
    ls_log("已连接 %s:%u", server_ip, server_port);

    int rc = 0;
    FileList local;
    flist_init(&local);
    Buf b;
    buf_init(&b);
    SyncStats stats;
    memset(&stats, 0, sizeof(stats));

    /* 3. HELLO */
    buf_put_str(&b, myname);
    buf_put_str(&b, root);
    buf_put_u32(&b, LS_PROTO);
    if (ls_msg_send(fd, MSG_HELLO, &b) != 0) {
        ls_log("发送 HELLO 失败");
        rc = 1;
        goto out;
    }
    buf_free(&b);

    /* 4. HELLO_ACK */
    buf_init(&b);
    if (ls_expect(fd, MSG_HELLO_ACK, &b) != 0) {
        ls_log("服务端没有正常应答");
        rc = 1;
        goto out;
    }
    {
        Rd r;
        rd_init(&r, b.p, b.len);
        char *sname = rd_str(&r);
        char *sroot = rd_str(&r);
        uint32_t proto = rd_u32(&r);
        uint8_t prefer = rd_u8(&r);
        uint8_t del = rd_u8(&r);
        char *subdir = rd_str(&r);
        if (r.err || proto != LS_PROTO) {
            ls_log("服务端协议版本不匹配");
            free(sname);
            free(sroot);
            free(subdir);
            rc = 1;
            goto out;
        }
        ls_log("服务端「%s」目录 %s，策略 %s%s", sname ? sname : "?",
               sroot ? sroot : "?",
               prefer == PREFER_SERVER
                   ? "服务端优先"
                   : (prefer == PREFER_CLIENT ? "客户端优先" : "新者优先"),
               del ? " + 镜像删除" : "");
        free(sname);
        free(sroot);

        /* 服务端可以强制指定客户端把文件落在哪个子目录下 */
        if (subdir && *subdir && path_safe(subdir)) {
            char nr[LS_PATHMAX];
            if (snprintf(nr, sizeof(nr), "%s/%s", root, subdir) < (int)sizeof(nr)) {
                snprintf(root, sizeof(root), "%s", nr);
                mkdirs_for(root);
                ls_log("服务端指定落盘位置：%s/", subdir);
            } else {
                ls_log("服务端指定的落盘子目录过长，忽略");
            }
        }
        free(subdir);
    }
    buf_free(&b);

    /* 5. 扫描本地并上报清单 */
    {
        uint64_t t0 = ls_now_ms();
        char err[512];
        ls_log("正在扫描本地目录…");
        if (fs_scan(root, &local, err, sizeof(err)) != 0) {
            ls_log("扫描失败：%s", err);
            rc = 1;
            goto out;
        }
        ls_log("扫描完成：%zu 个文件，耗时 %llu ms", local.n,
               (unsigned long long)(ls_now_ms() - t0));
        if (send_list(fd, &local) != 0) {
            ls_log("上报清单失败");
            rc = 1;
            goto out;
        }
    }

    /* 6. 按服务端指令完成一次同步 */
    for (;;) {
        buf_init(&b);
        uint8_t t = 0;
        if (ls_msg_recv(fd, &t, &b) != 0) {
            ls_log("与服务端的连接中断");
            rc = 1;
            break;
        }

        if (t == MSG_PULL_WANT) {
            PullStat ps;
            memset(&ps, 0, sizeof(ps));
            handle_pull_want(fd, root, &b, &ps);
            buf_free(&b);
            stats.pushed += ps.pushed;
            stats.failed += ps.failed;
            stats.bytes += ps.bytes;
            continue;
        }

        if (t == MSG_BUNDLE_BEGIN) {
            SinkCtx sc;
            memset(&sc, 0, sizeof(sc));
            sc.root = root;
            uint32_t okn = 0, failn = 0;
            uint64_t bytes = 0;
            int rc2 = ls_recv_bundle_body(fd, &b, cli_sink_factory, &sc, &okn, &failn, &bytes);
            buf_free(&b);
            if (rc2 != 0) {
                ls_log("接收失败：%s", ls_last_error());
                rc = 1;
                break;
            }
            ls_dbg("下载 <- 服务端  %u 个文件", okn);
            stats.pulled += okn;
            stats.failed += failn;
            stats.bytes += bytes;
            continue;
        }

        if (t == MSG_DELETE) {
            Rd r;
            rd_init(&r, b.p, b.len);
            char *rel = rd_str(&r);
            buf_free(&b);
            char full[LS_PATHMAX];
            if (rel && path_safe(rel) && path_join(full, sizeof(full), root, rel) == 0) {
                if (unlink(full) == 0) {
                    ls_log("删除本地   %s", rel);
                    stats.deleted++;
                } else {
                    ls_log("删除失败（%s）：%s", strerror(errno), rel);
                    stats.failed++;
                }
            } else {
                stats.failed++;
            }
            free(rel);
            continue;
        }

        if (t == MSG_LOG) {
            Rd r;
            rd_init(&r, b.p, b.len);
            char *txt = rd_str(&r);
            buf_free(&b);
            ls_log("[服务端] %s", txt ? txt : "");
            free(txt);
            continue;
        }

        if (t == MSG_SYNC_DONE) {
            Rd r;
            rd_init(&r, b.p, b.len);
            uint32_t push = rd_u32(&r);
            uint32_t pull = rd_u32(&r);
            uint32_t del = rd_u32(&r);
            uint32_t same = rd_u32(&r);
            uint32_t fail = rd_u32(&r);
            rd_u32(&r); /* ignored：服务端忽略的客户端多余文件数 */
            uint64_t bytes = rd_u64(&r);
            buf_free(&b);
            char sz[32];
            fmt_size(bytes, sz, sizeof(sz));
            ls_log("");
            ls_log("===== 同步结果 =====");
            ls_log("服务端视角：推送 %u / 拉取 %u / 删除 %u / 相同 %u / 失败 %u，传输 %s",
                   push, pull, del, same, fail, sz);
            ls_log("本机视角  ：上传 %u / 下载 %u / 删除 %u / 失败 %u", stats.pushed, stats.pulled,
                   stats.deleted, stats.failed);
            ls_log("====================");
            continue;
        }

        if (t == MSG_BYE) {
            buf_free(&b);
            break;
        }

        ls_log("收到意外消息类型 %u，中止", t);
        buf_free(&b);
        rc = 1;
        break;
    }

out:
    flist_free(&local);
    close(fd);
    return rc;
}

/* ---------------- 独立的发现命令 ---------------- */

int discover_main(int argc, char **argv)
{
    static const struct option opts[] = {
        {"udp-port", required_argument, 0, 'w'},
        {"timeout", required_argument, 0, 't'},
        {"name", required_argument, 0, 'n'},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}};

    uint16_t udp_port = LS_DEF_UDP_PORT;
    int timeout_ms = 3000;
    const char *filter = NULL;
    int c;
    while ((c = getopt_long(argc, argv, "w:t:n:vh", opts, NULL)) != -1) {
        switch (c) {
        case 'w': udp_port = (uint16_t)atoi(optarg); break;
        case 't': timeout_ms = atoi(optarg); break;
        case 'n': filter = optarg; break;
        case 'v': ls_set_verbose(1); break;
        case 'h':
            printf("用法: %s discover [-w UDP端口] [-t 超时毫秒] [-n 只匹配该名字]\n", argv[0]);
            return 0;
        default: return 2;
        }
    }

    int fd = ls_udp_open(0, 1);
    if (fd < 0) {
        fprintf(stderr, "UDP socket 创建失败：%s\n", strerror(errno));
        return 1;
    }
    for (int i = 0; i < 3; i++) {
        ls_udp_broadcast(fd, udp_port, "LANDSYNC_DISCOVER 1\n");
        struct pollfd p;
        p.fd = fd;
        p.events = POLLIN;
        p.revents = 0;
        poll(&p, 1, 60);
    }

    int found = 0;
    uint64_t deadline = ls_now_ms() + (uint64_t)timeout_ms;
    while (ls_now_ms() < deadline) {
        struct pollfd p;
        p.fd = fd;
        p.events = POLLIN;
        p.revents = 0;
        if (poll(&p, 1, 200) <= 0) {
            continue;
        }
        char buf[LS_PATHMAX + 512];
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) {
            continue;
        }
        buf[n] = '\0';
        Offer o;
        memset(&o, 0, sizeof(o));
        if (parse_offer(buf, &o) != 0) {
            continue;
        }
        inet_ntop(AF_INET, &from.sin_addr, o.ip, sizeof(o.ip));
        if (filter && *filter && strcmp(o.name, filter) != 0) {
            continue;
        }
        printf("%s:%u  名称=%s  目录=%s\n", o.ip, o.port, o.name, o.root);
        found++;
    }
    close(fd);
    if (!found) {
        printf("没有发现任何服务端\n");
        return 1;
    }
    return 0;
}

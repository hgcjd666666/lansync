/*
 * common.c -- lansync 公共层实现
 *
 */
#include "common.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <math.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>


/* ======================= 进度 ======================= */

static pthread_mutex_t g_io_lock = PTHREAD_MUTEX_INITIALIZER;
static int             g_prog_active = 0;
static int             g_prog_tty = 0;
static char            g_prog_label[32];
static uint64_t        g_prog_total = 0, g_prog_done = 0;
static uint32_t        g_prog_total_files = 0, g_prog_done_files = 0;
static uint64_t        g_prog_t0 = 0, g_prog_last_draw = 0;
static int             g_prog_last_bucket = -1;
static int             g_prog_dirty = 0; /* 进度行当前是否画在终端上 */

void fmt_size(uint64_t n, char *out, size_t cap)
{
    static const char *unit[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = (double)n;
    int i = 0;
    while (v >= 1024.0 && i < 4) {
        v /= 1024.0;
        i++;
    }
    if (i == 0) {
        snprintf(out, cap, "%llu B", (unsigned long long)n);
    } else {
        snprintf(out, cap, "%.1f %s", v, unit[i]);
    }
}

/* 终端列数；拿不到就给个保守值 */
static int term_cols(void)
{
    struct winsize ws;
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return (int)ws.ws_col;
    }
    const char *c = getenv("COLUMNS");
    if (c && atoi(c) > 0) {
        return atoi(c);
    }
    return 80;
}

static void fmt_eta(double secs, char *out, size_t cap)
{
    if (!(secs > 0) || secs > 86400) {
        snprintf(out, cap, "--");
        return;
    }
    int s = (int)secs;
    if (s < 60) {
        snprintf(out, cap, "%ds", s);
    } else if (s < 3600) {
        snprintf(out, cap, "%dm%02ds", s / 60, s % 60);
    } else {
        snprintf(out, cap, "%dh%02dm", s / 3600, (s % 3600) / 60);
    }
}

/* 调用者必须已持有 g_io_lock */
static void prog_erase_locked(void)
{
    if (g_prog_active && g_prog_tty && g_prog_dirty) {
        fputs("\r\033[K", stderr);
        g_prog_dirty = 0;
    }
}

/* 调用者必须已持有 g_io_lock */
static void prog_draw_locked(int force)
{
    if (!g_prog_active) {
        return;
    }
    uint64_t now = ls_now_ms();

    int pct = g_prog_total ? (int)(g_prog_done * 100 / g_prog_total) : 100;
    if (pct > 100) {
        pct = 100;
    }

    if (!g_prog_tty) {
        /* 非终端：每 10% 打一行就够，别把日志刷爆。
         * 这里无视 force —— 日志后的重画不该额外多出一行。 */
        int bucket = pct / 10;
        if (bucket == g_prog_last_bucket) {
            return;
        }
        g_prog_last_bucket = bucket;
        char d[32], t[32];
        fmt_size(g_prog_done, d, sizeof(d));
        fmt_size(g_prog_total, t, sizeof(t));
        fprintf(stderr, "%s %d%%  %s / %s\n", g_prog_label, pct, d, t);
        return;
    }

    /*
     * 宽度必须卡死，绝不能折行：一旦折行，\r 只会回到折行后的行首，
     * 进度条就会一行行往下堆。所以这里按终端列数裁剪内容。
     */
    int cols = term_cols();

    /*
     * 窄终端兜底：只画「标签 + 百分比」，整行 9 列，任何终端都放得下。
     * 关键是仍然用 \r 原地刷新，绝不换行 —— 换行就全乱了。
     */
    if (cols < 30) {
        if (!force && now - g_prog_last_draw < 150) {
            return;
        }
        g_prog_last_draw = now;
        if (cols < 10) {
            fprintf(stderr, "\r\033[K%3d%%", pct);
        } else {
            fprintf(stderr, "\r\033[K%s %3d%%", g_prog_label, pct);
        }
        fflush(stderr);
        g_prog_dirty = 1;
        return;
    }

    if (!force && now - g_prog_last_draw < 150) {
        return; /* 限流，别把终端刷爆 */
    }
    g_prog_last_draw = now;

    char d[32], t[32], sp[32], eta[24];
    fmt_size(g_prog_done, d, sizeof(d));
    fmt_size(g_prog_total, t, sizeof(t));
    double secs = (double)(now - g_prog_t0) / 1000.0;
    double speed = secs > 0.05 ? (double)g_prog_done / secs : 0;
    fmt_size((uint64_t)speed, sp, sizeof(sp));
    fmt_eta(speed > 0 ? (double)(g_prog_total - g_prog_done) / speed : 0, eta, sizeof(eta));

    /* 后缀从详细到简略，挑第一个放得下的 */
    char tails[4][160];
    snprintf(tails[0], sizeof(tails[0]), "  %s/%s  %s/s  剩 %s", d, t, sp, eta);
    snprintf(tails[1], sizeof(tails[1]), "  %s/%s  剩 %s", d, t, eta);
    snprintf(tails[2], sizeof(tails[2]), "  %s", d);
    tails[3][0] = '\0';

    /* "推送 " (2 个汉字 = 4 列) + "[" + bar + "] " + "100%" */
    const int label_cols = 4;
    const int pct_cols = 4;
    const int overhead = label_cols + 1 + 2 + pct_cols + 1; /* 标签+空格+[]+百分比+空格 */

    const char *tail = "";
    int bar_w = 0;
    for (int i = 0; i < 4; i++) {
        int tl = (int)strlen(tails[i]);
        int room = cols - overhead - tl - 1;
        if (room >= 10) {
            tail = tails[i];
            bar_w = room;
            break;
        }
    }
    if (bar_w == 0) {
        /* 连最短的后缀都放不下：只要条和百分比 */
        bar_w = cols - overhead - 1;
        if (bar_w < 6) {
            bar_w = 6;
        }
    }
    char bar[256];
    /* 长度完全由终端宽度决定，不设人为上限：宽终端就把整行铺满 */
    if (bar_w > (int)sizeof(bar) - 1) {
        bar_w = (int)sizeof(bar) - 1;
    }
    int filled = pct * bar_w / 100;
    for (int i = 0; i < bar_w; i++) {
        bar[i] = i < filled ? '#' : '.';
    }
    bar[bar_w] = '\0';

    fprintf(stderr, "\r\033[K%s [%s] %3d%%%s", g_prog_label, bar, pct, tail);
    fflush(stderr);
    g_prog_dirty = 1;
}

void progress_begin(const char *label, uint64_t total_bytes, uint32_t total_files)
{
    pthread_mutex_lock(&g_io_lock);
    prog_erase_locked();
    snprintf(g_prog_label, sizeof(g_prog_label), "%s", label ? label : "");
    g_prog_total = total_bytes;
    g_prog_done = 0;
    g_prog_total_files = total_files;
    g_prog_done_files = 0;
    g_prog_t0 = ls_now_ms();
    g_prog_last_draw = 0;
    g_prog_last_bucket = -1;
    g_prog_active = 1;
    g_prog_tty = isatty(2);
    prog_draw_locked(1);
    pthread_mutex_unlock(&g_io_lock);
}

void progress_add(uint64_t bytes, uint32_t files)
{
    pthread_mutex_lock(&g_io_lock);
    if (g_prog_active) {
        g_prog_done += bytes;
        g_prog_done_files += files;
        prog_draw_locked(0);
    }
    pthread_mutex_unlock(&g_io_lock);
}

void progress_end(const char *summary)
{
    pthread_mutex_lock(&g_io_lock);
    if (!g_prog_active) {
        pthread_mutex_unlock(&g_io_lock);
        return;
    }
    if (g_prog_tty) {
        prog_erase_locked();
    }
    g_prog_active = 0;
    g_prog_dirty = 0;
    if (summary) {
        fputs(summary, stderr);
        fputc('\n', stderr);
    }
    fflush(stderr);
    pthread_mutex_unlock(&g_io_lock);
}

int progress_active(void)
{
    return g_prog_active;
}

/* ======================= 版本 ======================= */

const char *ls_version(void)
{
    return "1.0";
}

const char *ls_build_date(void)
{
    return LS_BUILD_DATE;
}

uint64_t ls_build_ts(void)
{
    return (uint64_t)LS_BUILD_TS;
}

const char *ls_build_info(void)
{
    static char b[128];
    snprintf(b, sizeof(b), "lansync %s (built %s)", ls_version(), ls_build_date());
    return b;
}

/* ======================= 熵 ======================= */

double ls_entropy_file(const char *path, size_t sample_bytes)
{
    if (sample_bytes == 0) {
        return -1;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    uint8_t *buf = (uint8_t *)malloc(sample_bytes);
    if (!buf) {
        fclose(f);
        return -1;
    }
    size_t n = fread(buf, 1, sample_bytes, f);
    int bad = ferror(f);
    fclose(f);
    if (bad || n == 0) {
        free(buf);
        return n == 0 && !bad ? 0.0 : -1;
    }

    uint64_t cnt[256];
    memset(cnt, 0, sizeof(cnt));
    for (size_t i = 0; i < n; i++) {
        cnt[buf[i]]++;
    }
    free(buf);

    double h = 0.0;
    for (int i = 0; i < 256; i++) {
        if (cnt[i]) {
            double p = (double)cnt[i] / (double)n;
            h -= p * (log(p) / log(2.0));
        }
    }
    return h;
}

/* ======================= 最近错误 ======================= */

static __thread char g_lasterr[512];

void ls_set_last_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_lasterr, sizeof(g_lasterr), fmt, ap);
    va_end(ap);
}

const char *ls_last_error(void)
{
    return g_lasterr[0] ? g_lasterr : "(无详细信息)";
}

/* ======================= 日志 ======================= */

static int g_quiet = 0;
static int g_verbose = 0;

void ls_set_quiet(int q) { g_quiet = q; }
void ls_set_verbose(int v) { g_verbose = v; }
int  ls_verbose(void) { return g_verbose; }

static void logv(const char *tag, const char *fmt, va_list ap)
{
    if (g_quiet) {
        return;
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);
    char line[2048];
    int n = snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] %s ",
                     tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(tv.tv_usec / 1000), tag);
    if (n < 0) {
        return;
    }
    size_t off = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;
    vsnprintf(line + off, sizeof(line) - off, fmt, ap);

    pthread_mutex_lock(&g_io_lock);
    prog_erase_locked(); /* 日志要独占一行，先把进度行擦掉 */
    fputs(line, stderr);
    fputc('\n', stderr);
    prog_draw_locked(1); /* 打完补回来 */
    pthread_mutex_unlock(&g_io_lock);
}

void ls_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    logv("INFO", fmt, ap);
    va_end(ap);
}

void ls_dbg(const char *fmt, ...)
{
    if (!g_verbose) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    logv("DBG ", fmt, ap);
    va_end(ap);
}

uint64_t ls_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* ======================= Buf ======================= */

void buf_init(Buf *b)
{
    b->p = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_free(Buf *b)
{
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

void buf_reset(Buf *b) { b->len = 0; }

void buf_reserve(Buf *b, size_t extra)
{
    if (b->len + extra <= b->cap) {
        return;
    }
    size_t want = b->len + extra;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < want) {
        cap *= 2;
    }
    uint8_t *np = (uint8_t *)realloc(b->p, cap);
    if (!np) {
        abort(); /* 内存耗尽：这是不可恢复的编程错误 */
    }
    b->p = np;
    b->cap = cap;
}

void buf_put_u8(Buf *b, uint8_t v)
{
    buf_reserve(b, 1);
    b->p[b->len++] = v;
}

void buf_put_u32(Buf *b, uint32_t v)
{
    buf_reserve(b, 4);
    b->p[b->len++] = (uint8_t)(v >> 24);
    b->p[b->len++] = (uint8_t)(v >> 16);
    b->p[b->len++] = (uint8_t)(v >> 8);
    b->p[b->len++] = (uint8_t)v;
}

void buf_put_u64(Buf *b, uint64_t v)
{
    buf_put_u32(b, (uint32_t)(v >> 32));
    buf_put_u32(b, (uint32_t)v);
}

void buf_put_i64(Buf *b, int64_t v) { buf_put_u64(b, (uint64_t)v); }

void buf_put_bytes(Buf *b, const void *data, size_t n)
{
    if (n == 0) {
        return;
    }
    buf_reserve(b, n);
    memcpy(b->p + b->len, data, n);
    b->len += n;
}

void buf_put_str(Buf *b, const char *s)
{
    size_t n = s ? strlen(s) : 0;
    buf_put_u32(b, (uint32_t)n);
    buf_put_bytes(b, s, n);
}

/* ======================= Rd ======================= */

void rd_init(Rd *r, const uint8_t *p, size_t len)
{
    r->p = p;
    r->len = len;
    r->pos = 0;
    r->err = 0;
}

const uint8_t *rd_bytes(Rd *r, size_t n)
{
    if (r->err || r->pos + n > r->len) {
        r->err = 1;
        return NULL;
    }
    const uint8_t *p = r->p + r->pos;
    r->pos += n;
    return p;
}

uint8_t rd_u8(Rd *r)
{
    const uint8_t *p = rd_bytes(r, 1);
    return p ? *p : 0;
}

uint32_t rd_u32(Rd *r)
{
    const uint8_t *p = rd_bytes(r, 4);
    if (!p) {
        return 0;
    }
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

uint64_t rd_u64(Rd *r)
{
    uint64_t hi = rd_u32(r);
    uint64_t lo = rd_u32(r);
    return (hi << 32) | lo;
}

int64_t rd_i64(Rd *r) { return (int64_t)rd_u64(r); }

char *rd_str(Rd *r)
{
    uint32_t n = rd_u32(r);
    if (r->err || n > LS_PATHMAX * 4) {
        r->err = 1;
        return NULL;
    }
    const uint8_t *p = rd_bytes(r, n);
    if (!p) {
        return NULL;
    }
    char *s = (char *)malloc((size_t)n + 1);
    if (!s) {
        r->err = 1;
        return NULL;
    }
    memcpy(s, p, n);
    s[n] = '\0';
    return s;
}

/* ======================= socket ======================= */

int ls_send_all(int fd, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int ls_recv_all(int fd, void *data, size_t len)
{
    uint8_t *p = (uint8_t *)data;
    while (len > 0) {
        ssize_t n = recv(fd, p, len, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1; /* 对端关闭 */
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* 每线程复用的发送缓冲。256KB 的分片如果每条消息都 malloc/free，
 * 会退化成 mmap/munmap 风暴，手机上尤其明显。 */
static __thread uint8_t *g_txbuf;
static __thread size_t   g_txcap;

int ls_msg_send(int fd, uint8_t type, const Buf *payload)
{
    size_t plen = payload ? payload->len : 0;
    uint8_t hdr[5];
    hdr[0] = type;
    hdr[1] = (uint8_t)(plen >> 24);
    hdr[2] = (uint8_t)(plen >> 16);
    hdr[3] = (uint8_t)(plen >> 8);
    hdr[4] = (uint8_t)plen;

    if (plen == 0) {
        return ls_send_all(fd, hdr, 5);
    }

    size_t need = 5 + plen;
    if (need > g_txcap) {
        uint8_t *np = (uint8_t *)realloc(g_txbuf, need);
        if (!np) {
            return -1;
        }
        g_txbuf = np;
        g_txcap = need;
    }
    memcpy(g_txbuf, hdr, 5);
    memcpy(g_txbuf + 5, payload->p, plen);
    return ls_send_all(fd, g_txbuf, need);
}

int ls_msg_recv(int fd, uint8_t *type, Buf *payload)
{
    uint8_t hdr[5];
    if (ls_recv_all(fd, hdr, 5) < 0) {
        return -1;
    }
    uint32_t plen = ((uint32_t)hdr[1] << 24) | ((uint32_t)hdr[2] << 16) |
                    ((uint32_t)hdr[3] << 8) | (uint32_t)hdr[4];
    if (plen > LS_MAX_PAYLOAD) {
        ls_log("协议错误：消息长度 %u 超限", plen);
        return -1;
    }
    buf_reset(payload);
    if (plen > 0) {
        buf_reserve(payload, plen);
        if (ls_recv_all(fd, payload->p, plen) < 0) {
            return -1;
        }
        payload->len = plen;
    }
    *type = hdr[0];
    return 0;
}

int ls_expect(int fd, uint8_t want, Buf *out)
{
    Buf b;
    buf_init(&b);
    uint8_t t = 0;
    if (ls_msg_recv(fd, &t, &b) < 0) {
        buf_free(&b);
        return -1;
    }
    if (t != want) {
        ls_dbg("期望消息类型 %u，收到 %u", want, t);
        buf_free(&b);
        return -2;
    }
    if (out) {
        *out = b; /* 转移所有权 */
    } else {
        buf_free(&b);
    }
    return 0;
}

void ls_set_nodelay(int fd)
{
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

void ls_set_timeout(int fd, int seconds)
{
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

void ls_set_keepalive(int fd)
{
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
}

int ls_tcp_listen(uint16_t port, int backlog)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, backlog) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int ls_tcp_connect(const char *host, uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    ls_set_nodelay(fd);
    ls_set_keepalive(fd);
    return fd;
}

int ls_udp_open(uint16_t port, int reuse)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    if (reuse) {
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/*
 * 向各网段的广播地址各发一份。多网卡（如 wlan0 + ap0）下只发
 * 255.255.255.255 会受默认路由影响走不到所有网段。
 */
int ls_udp_broadcast(int fd, uint16_t port, const char *text)
{
    size_t len = strlen(text);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);

    int sent = 0;

    /* 1) 全局广播 */
    sa.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    if (sendto(fd, text, len, 0, (struct sockaddr *)&sa, sizeof(sa)) >= 0) {
        sent++;
    }

    /* 2) 每个非回环、支持广播的接口，各发一份到其子网广播地址。
     *    不用 getifaddrs（bionic 从 API 24 才有），也不用 /proc/net/dev
     *    （Android 10+ 普通进程读不到），只用 SIOCGIFCONF，API 21 即可。 */
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        return sent;
    }
    char ifbuf[32 * sizeof(struct ifreq)];
    struct ifconf ifc;
    ifc.ifc_len = (int)sizeof(ifbuf);
    ifc.ifc_buf = ifbuf;
    if (ioctl(s, SIOCGIFCONF, &ifc) < 0) {
        close(s);
        return sent;
    }

    int nif = ifc.ifc_len / (int)sizeof(struct ifreq);
    for (int i = 0; i < nif; i++) {
        struct ifreq req;
        memset(&req, 0, sizeof(req));
        snprintf(req.ifr_name, IFNAMSIZ, "%s", ifc.ifc_req[i].ifr_name);

        if (ioctl(s, SIOCGIFFLAGS, &req) < 0) {
            continue;
        }
        if (!(req.ifr_flags & IFF_BROADCAST) || (req.ifr_flags & IFF_LOOPBACK)) {
            continue;
        }
        if (ioctl(s, SIOCGIFBRDADDR, &req) < 0) {
            continue;
        }
        struct sockaddr_in *br = (struct sockaddr_in *)&req.ifr_broadaddr;
        if (br->sin_addr.s_addr == 0) {
            continue;
        }
        sa.sin_addr = br->sin_addr;
        if (sendto(fd, text, len, 0, (struct sockaddr *)&sa, sizeof(sa)) >= 0) {
            sent++;
        }
    }
    close(s);
    return sent;
}

/* ======================= 文件 ======================= */

void flist_init(FileList *l)
{
    l->v = NULL;
    l->n = l->cap = 0;
}

void flist_free(FileList *l)
{
    for (size_t i = 0; i < l->n; i++) {
        free(l->v[i].rel);
    }
    free(l->v);
    l->v = NULL;
    l->n = l->cap = 0;
}

int flist_add(FileList *l, const char *rel, uint64_t size, int64_t mtime)
{
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 64;
        FileEntry *nv = (FileEntry *)realloc(l->v, cap * sizeof(FileEntry));
        if (!nv) {
            return -1;
        }
        l->v = nv;
        l->cap = cap;
    }
    FileEntry *e = &l->v[l->n];
    e->rel = strdup(rel);
    if (!e->rel) {
        return -1;
    }
    e->size = size;
    e->mtime = mtime;
    l->n++;
    return 0;
}

static int cmp_entry(const void *a, const void *b)
{
    const FileEntry *x = (const FileEntry *)a;
    const FileEntry *y = (const FileEntry *)b;
    return strcmp(x->rel, y->rel);
}

FileEntry *flist_find(const FileList *l, const char *rel)
{
    /* 列表已排序，用二分查找 */
    size_t lo = 0, hi = l->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(l->v[mid].rel, rel);
        if (c == 0) {
            return &l->v[mid];
        }
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

void flist_sort(FileList *l)
{
    if (l->n > 1) {
        qsort(l->v, l->n, sizeof(FileEntry), cmp_entry);
    }
}

static int scan_dir(const char *root, const char *rel, FileList *out, char *err,
                    size_t errlen);

static int scan_one(const char *root, const char *rel, const char *name,
                    FileList *out, char *err, size_t errlen)
{
    char childrel[LS_PATHMAX];
    char full[LS_PATHMAX];
    int n;
    if (*rel) {
        n = snprintf(childrel, sizeof(childrel), "%s/%s", rel, name);
    } else {
        n = snprintf(childrel, sizeof(childrel), "%s", name);
    }
    if (n < 0 || (size_t)n >= sizeof(childrel)) {
        snprintf(err, errlen, "路径过长，已跳过：%s/%s", rel, name);
        return -1;
    }
    if (path_join(full, sizeof(full), root, childrel) < 0) {
        snprintf(err, errlen, "路径过长，已跳过：%s", childrel);
        return -1;
    }

    struct stat st;
    if (lstat(full, &st) < 0) {
        return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        return scan_dir(root, childrel, out, err, errlen);
    }
    if (!S_ISREG(st.st_mode)) {
        return 0; /* 符号链接/设备/管道一律跳过 */
    }

    /* 只记录元数据，不读文件内容。mtime 用纳秒：秒级精度会让同一秒内
     * 创建/修改的不同内容文件在快速路径上被误判为相同。 */
    int64_t mt = (int64_t)st.st_mtim.tv_sec * 1000000000LL + (int64_t)st.st_mtim.tv_nsec;
    if (flist_add(out, childrel, (uint64_t)st.st_size, mt) < 0) {
        snprintf(err, errlen, "内存不足");
        return -1;
    }
    return 0;
}

static int scan_dir(const char *root, const char *rel, FileList *out, char *err,
                    size_t errlen)
{
    char dirpath[LS_PATHMAX];
    if (path_join(dirpath, sizeof(dirpath), root, rel) < 0) {
        return -1;
    }
    DIR *d = opendir(dirpath);
    if (!d) {
        snprintf(err, errlen, "无法打开目录：%s", dirpath);
        return -1;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        if (strncmp(de->d_name, LS_TMP_PREFIX, strlen(LS_TMP_PREFIX)) == 0) {
            continue; /* 我们自己的临时文件 */
        }
        scan_one(root, rel, de->d_name, out, err, errlen);
    }
    closedir(d);
    return 0;
}

int fs_scan(const char *root, FileList *out, char *err, size_t errlen)
{
    struct stat st;
    if (stat(root, &st) < 0) {
        snprintf(err, errlen, "目录不存在：%s", root);
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        snprintf(err, errlen, "不是目录：%s", root);
        return -1;
    }
    if (errlen) {
        err[0] = '\0';
    }
    if (scan_dir(root, "", out, err, errlen) < 0) {
        return -1;
    }
    qsort(out->v, out->n, sizeof(FileEntry), cmp_entry);
    return 0;
}

int path_safe(const char *rel)
{
    if (!rel || !*rel) {
        return 0;
    }
    if (rel[0] == '/' || rel[0] == '\\') {
        return 0;
    }
    if (strlen(rel) >= LS_PATHMAX) {
        return 0;
    }
    const char *p = rel;
    while (*p) {
        const char *seg = p;
        while (*p && *p != '/') {
            p++;
        }
        size_t n = (size_t)(p - seg);
        if (n == 0) {
            return 0; /* 空段：连续斜杠或结尾斜杠 */
        }
        if (n == 1 && seg[0] == '.') {
            return 0;
        }
        if (n == 2 && seg[0] == '.' && seg[1] == '.') {
            return 0;
        }
        if (*p == '/') {
            p++;
        }
    }
    return 1;
}

int path_join(char *out, size_t n, const char *root, const char *rel)
{
    int r;
    if (rel && *rel) {
        r = snprintf(out, n, "%s/%s", root, rel);
    } else {
        r = snprintf(out, n, "%s", root);
    }
    if (r < 0 || (size_t)r >= n) {
        return -1;
    }
    return 0;
}

int mkdirs_for(const char *fullpath)
{
    char tmp[LS_PATHMAX];
    size_t n = strlen(fullpath);
    if (n >= sizeof(tmp)) {
        return -1;
    }
    memcpy(tmp, fullpath, n + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
                /* 继续尝试，最终 open 会报错 */
            }
            *p = '/';
        }
    }
    return 0;
}

int set_mtime(const char *path, int64_t mtime_ns)
{
    struct timespec ts[2];
    ts[0].tv_sec = (time_t)(mtime_ns / 1000000000LL);
    ts[0].tv_nsec = (long)(mtime_ns % 1000000000LL);
    ts[1] = ts[0];
    return utimensat(AT_FDCWD, path, ts, 0);
}

/* ======================= 流：实现 ======================= */

/* ---- 文件源 ---- */
typedef struct {
    FILE *f;
} FileSource;

static ssize_t fsrc_read(LsSource *s, void *buf, size_t n)
{
    FileSource *fs = (FileSource *)s->impl;
    size_t got = fread(buf, 1, n, fs->f);
    if (got > 0) {
        return (ssize_t)got;
    }
    if (ferror(fs->f)) {
        return -1;
    }
    return 0;
}

static void fsrc_close(LsSource *s)
{
    FileSource *fs = (FileSource *)s->impl;
    if (fs) {
        if (fs->f) {
            fclose(fs->f);
        }
        free(fs);
    }
    free(s);
}

LsSource *ls_source_file(const char *path, char *err, size_t errlen)
{
    struct stat st;
    if (stat(path, &st) < 0) {
        snprintf(err, errlen, "打开源文件失败: %s (%s)", path, strerror(errno));
        return NULL;
    }
    if (!S_ISREG(st.st_mode)) {
        snprintf(err, errlen, "不是普通文件: %s", path);
        return NULL;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "打开源文件失败: %s (%s)", path, strerror(errno));
        return NULL;
    }
    LsSource *s = (LsSource *)calloc(1, sizeof(*s));
    FileSource *fs = (FileSource *)calloc(1, sizeof(*fs));
    if (!s || !fs) {
        free(s);
        free(fs);
        fclose(f);
        snprintf(err, errlen, "内存不足");
        return NULL;
    }
    fs->f = f;
    s->impl = fs;
    s->read = fsrc_read;
    s->close = fsrc_close;
    snprintf(s->name, sizeof(s->name), "%s", path);
    return s;
}

void ls_source_free(LsSource *s)
{
    if (s) {
        s->close(s);
    }
}

/* ---- 落盘汇（先写同目录临时文件，finish 时 rename）---- */
typedef struct {
    FILE   *f;
    char    tmp[LS_PATHMAX];
    char    dst[LS_PATHMAX];
    int64_t mtime;
    mode_t  mode;
} FileSink;

static int fsink_write(LsSink *s, const void *buf, size_t n)
{
    FileSink *fs = (FileSink *)s->impl;
    if (!fs->f) {
        return -1;
    }
    if (n == 0) {
        return 0;
    }
    if (s->written + n > s->expect) {
        ls_set_last_error("收到超出声明长度的数据");
        return -1;
    }
    if (fwrite(buf, 1, n, fs->f) != n) {
        ls_set_last_error("写入失败: %s", strerror(errno));
        return -1;
    }
    s->written += n;
    return 0;
}

static void fsink_abort(LsSink *s)
{
    FileSink *fs = (FileSink *)s->impl;
    if (!fs) {
        return;
    }
    if (fs->f) {
        fclose(fs->f);
        fs->f = NULL;
    }
    if (fs->tmp[0]) {
        unlink(fs->tmp);
        fs->tmp[0] = '\0';
    }
}

static int fsink_finish(LsSink *s)
{
    FileSink *fs = (FileSink *)s->impl;
    if (!fs->f) {
        return -1;
    }
    if (s->written != s->expect) {
        ls_set_last_error("长度不符（收到 %llu / 期望 %llu）",
                          (unsigned long long)s->written, (unsigned long long)s->expect);
        fsink_abort(s);
        return -1;
    }
    /* 不做 fsync：rename 本身原子，最多丢内容不会留半个文件，
     * 而每个文件都 fsync 会让大量小文件的同步慢一个数量级。 */
    if (fflush(fs->f) != 0 || fclose(fs->f) != 0) {
        fs->f = NULL;
        fsink_abort(s);
        ls_set_last_error("落盘失败: %s", strerror(errno));
        return -1;
    }
    fs->f = NULL;
    (void)chmod(fs->tmp, fs->mode);
    if (rename(fs->tmp, fs->dst) < 0) {
        ls_set_last_error("rename 失败 %s: %s", fs->dst, strerror(errno));
        fsink_abort(s);
        return -1;
    }
    fs->tmp[0] = '\0';
    (void)set_mtime(fs->dst, fs->mtime);
    return 0;
}

static void fsink_free(LsSink *s)
{
    if (!s) {
        return;
    }
    fsink_abort(s); /* 未 finish 的会被丢弃 */
    free(s->impl);
    free(s);
}

static LsSink *sink_new_at(const char *dst, uint64_t size, int64_t mtime, uint32_t mode,
                           char *err, size_t errlen)
{
    mode_t m = (mode_t)(mode ? (mode & 07777u) : 0644u);

    if (mkdirs_for(dst) < 0) {
        snprintf(err, errlen, "无法创建目录: %s", dst);
        return NULL;
    }

    /* 临时文件必须与目标同目录，rename 才是原子的 */
    char dir[LS_PATHMAX];
    int n = snprintf(dir, sizeof(dir), "%s", dst);
    if (n < 0 || (size_t)n >= sizeof(dir)) {
        snprintf(err, errlen, "路径过长");
        return NULL;
    }
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
    } else {
        snprintf(dir, sizeof(dir), ".");
    }

    static unsigned counter = 0;
    unsigned id = __atomic_add_fetch(&counter, 1u, __ATOMIC_RELAXED);
    char tmp[LS_PATHMAX];
    n = snprintf(tmp, sizeof(tmp), "%s/" LS_TMP_PREFIX "%d-%u", dir, (int)getpid(), id);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        snprintf(err, errlen, "临时路径过长");
        return NULL;
    }

    FILE *f = fopen(tmp, "wb");
    if (!f) {
        snprintf(err, errlen, "无法创建临时文件 %s (%s)", tmp, strerror(errno));
        return NULL;
    }

    LsSink *s = (LsSink *)calloc(1, sizeof(*s));
    FileSink *fs = (FileSink *)calloc(1, sizeof(*fs));
    if (!s || !fs) {
        free(s);
        free(fs);
        fclose(f);
        unlink(tmp);
        snprintf(err, errlen, "内存不足");
        return NULL;
    }
    fs->f = f;
    fs->mtime = mtime;
    fs->mode = m;
    snprintf(fs->tmp, sizeof(fs->tmp), "%s", tmp);
    snprintf(fs->dst, sizeof(fs->dst), "%s", dst);

    s->impl = fs;
    s->write = fsink_write;
    s->finish = fsink_finish;
    s->abort = fsink_abort;
    s->expect = size;
    snprintf(s->name, sizeof(s->name), "%s", dst);
    return s;
}

LsSink *ls_sink_file(const char *root, const char *rel, uint64_t size, int64_t mtime,
                     uint32_t mode, char *err, size_t errlen)
{
    if (!path_safe(rel)) {
        snprintf(err, errlen, "拒绝不安全路径: %s", rel ? rel : "(null)");
        return NULL;
    }
    char dst[LS_PATHMAX];
    if (path_join(dst, sizeof(dst), root, rel) < 0) {
        snprintf(err, errlen, "路径过长: %s", rel);
        return NULL;
    }
    return sink_new_at(dst, size, mtime, mode, err, errlen);
}

LsSink *ls_sink_abs(const char *fullpath, uint64_t size, int64_t mtime, uint32_t mode,
                    char *err, size_t errlen)
{
    return sink_new_at(fullpath, size, mtime, mode, err, errlen);
}

/* ---- 空汇：数据照单全收然后丢掉，只为保持流对齐 ---- */
static int nsink_write(LsSink *s, const void *buf, size_t n)
{
    (void)buf;
    s->written += n;
    return 0;
}
static int nsink_finish(LsSink *s)
{
    (void)s;
    return 0;
}
static void nsink_abort(LsSink *s)
{
    (void)s;
}

LsSink *ls_sink_null(uint64_t size)
{
    LsSink *s = (LsSink *)calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    s->write = nsink_write;
    s->finish = nsink_finish;
    s->abort = nsink_abort;
    s->expect = size;
    snprintf(s->name, sizeof(s->name), "(discard)");
    return s;
}

void ls_sink_free(LsSink *s)
{
    if (!s) {
        return;
    }
    if (s->impl == NULL) {
        free(s); /* 空汇 */
        return;
    }
    fsink_free(s);
}

/* ======================= bundle 传输 ======================= */

int ls_send_bundle(int fd, const char *root, const BundleEntry *ents, size_t n,
                   uint32_t *ok_n, uint32_t *failed_n, uint64_t *bytes)
{
    Buf b;
    uint8_t *io = NULL;
    uint64_t total = 0;
    uint32_t local_failed = 0;
    char first_local_err[512] = {0};

    uint64_t declared = 0;
    for (size_t i = 0; i < n; i++) {
        declared += ents[i].size;
    }

    buf_init(&b);
    buf_put_u32(&b, (uint32_t)n);
    buf_put_u64(&b, declared);
    if (ls_msg_send(fd, MSG_BUNDLE_BEGIN, &b) != 0) {
        buf_free(&b);
        ls_set_last_error("发送 BUNDLE_BEGIN 失败: %s", strerror(errno));
        return -1;
    }
    buf_free(&b);

    io = (uint8_t *)malloc(LS_CHUNK);
    if (!io) {
        ls_set_last_error("内存不足");
        return -1;
    }

    for (size_t i = 0; i < n; i++) {
        const BundleEntry *e = &ents[i];
        char full[LS_PATHMAX] = {0};
        char err[512] = {0};

        if (e->srcpath) {
            snprintf(full, sizeof(full), "%s", e->srcpath);
        } else if (!path_safe(e->rel) || path_join(full, sizeof(full), root, e->rel) < 0) {
            snprintf(err, sizeof(err), "路径非法或过长: %s", e->rel);
            ls_dbg("跳过 %s：%s", e->rel, err);
            local_failed++;
            /* 仍然发 ENTRY，接收端会因长度不符而丢弃，协议保持对齐 */
        }

        buf_init(&b);
        buf_put_str(&b, e->rel);
        buf_put_u64(&b, e->size);
        buf_put_i64(&b, e->mtime);
        buf_put_u32(&b, e->mode);
        int rc = ls_msg_send(fd, MSG_BUNDLE_ENTRY, &b);
        buf_free(&b);
        if (rc < 0) {
            free(io);
            ls_set_last_error("发送 BUNDLE_ENTRY 失败: %s", strerror(errno));
            return -1;
        }

        if (err[0]) {
            continue;
        }

        LsSource *src = ls_source_file(full, err, sizeof(err));
        if (!src) {
            if (!first_local_err[0]) {
                snprintf(first_local_err, sizeof(first_local_err), "%s", err);
            }
            ls_dbg("%s", err);
            ls_set_last_error("%s", err);
            local_failed++;
            continue; /* 一个字节都不发，接收端会因长度不符丢弃这个条目 */
        }

        Buf d;
        buf_init(&d);
        ssize_t got;
        int broken = 0;
        int readfail = 0;
        while ((got = src->read(src, io, LS_CHUNK)) > 0) {
            buf_reset(&d);
            buf_put_bytes(&d, io, (size_t)got);
            if (ls_msg_send(fd, MSG_BUNDLE_DATA, &d) < 0) {
                broken = 1;
                break;
            }
            total += (uint64_t)got;
            progress_add((uint64_t)got, 0);
        }
        if (!broken && got < 0) {
            readfail = 1;
        }
        buf_free(&d);
        ls_source_free(src);

        if (readfail) {
            /* 已经发出去一部分，长度必然不符，接收端会丢弃这个条目 */
            ls_set_last_error("读取源文件中途失败: %s", e->rel);
            local_failed++;
        }
        if (broken) {
            free(io);
            ls_set_last_error("发送数据失败: %s", strerror(errno));
            return -1;
        }
    }
    free(io);

    if (ls_msg_send(fd, MSG_BUNDLE_END, NULL) != 0) {
        ls_set_last_error("发送 BUNDLE_END 失败: %s", strerror(errno));
        return -1;
    }

    Buf ack;
    if (ls_expect(fd, MSG_BUNDLE_ACK, &ack) != 0) {
        ls_set_last_error("未收到 bundle 回执（连接中断或对端异常）");
        return -1;
    }
    Rd r;
    rd_init(&r, ack.p, ack.len);
    uint8_t ok = rd_u8(&r);
    char *msg = rd_str(&r);
    uint32_t done = rd_u32(&r);
    uint32_t fail = rd_u32(&r);
    if (!ok) {
        ls_set_last_error("对端报告失败（成功 %u / 失败 %u）: %s", done, fail,
                          msg ? msg : "(无说明)");
    }
    free(msg);
    buf_free(&ack);

    if (local_failed > 0) {
        ls_log("本端有 %u 个文件读不了，例如：%s", local_failed,
               first_local_err[0] ? first_local_err : "(未知)");
    }
    if (ok_n) {
        *ok_n = done;
    }
    if (failed_n) {
        /* 接收端的失败数已包含发送端就失败的条目 */
        *failed_n = fail;
    }
    if (bytes) {
        *bytes = total;
    }
    (void)local_failed;
    return ok ? 0 : -1;
}

int ls_recv_bundle(int fd, LsSinkFactory mk, void *ctx, uint32_t *ok_n, uint32_t *failed_n,
                   uint64_t *bytes)
{
    Buf b;
    buf_init(&b);
    if (ls_expect(fd, MSG_BUNDLE_BEGIN, &b) != 0) {
        buf_free(&b);
        ls_set_last_error("未收到 bundle 起始消息");
        return -1;
    }
    int rc = ls_recv_bundle_body(fd, &b, mk, ctx, ok_n, failed_n, bytes);
    buf_free(&b);
    return rc;
}

int ls_recv_bundle_body(int fd, const Buf *begin, LsSinkFactory mk, void *ctx,
                        uint32_t *ok_n, uint32_t *failed_n, uint64_t *bytes)
{
    Rd r;
    rd_init(&r, begin->p, begin->len);
    uint32_t count = rd_u32(&r);
    uint64_t declared = rd_u64(&r);
    if (r.err || count > LS_MAX_FILES) {
        ls_set_last_error("bundle 起始报文损坏");
        return -1;
    }

    /* 外层没在统计时自己起一个进度条（比如客户端接收） */
    int own_progress = !progress_active();
    if (own_progress) {
        progress_begin("接收", declared, count);
    }

    uint32_t ok = 0, failed = 0;
    uint64_t total = 0;
    uint32_t seen = 0;
    LsSink *cur = NULL;
    int cur_failed = 0;
    char first_err[512] = {0};
    Buf b;

    /* 结算当前条目：写满了就 finish，否则丢弃 */
#define SETTLE()                                                               \
    do {                                                                       \
        if (cur) {                                                             \
            if (cur_failed || cur->finish(cur) != 0) {                         \
                failed++;                                                      \
                if (!first_err[0]) {                                           \
                    snprintf(first_err, sizeof(first_err), "%s", ls_last_error()); \
                }                                                              \
            } else {                                                           \
                ok++;                                                          \
            }                                                                  \
            ls_sink_free(cur);                                                 \
            cur = NULL;                                                        \
            cur_failed = 0;                                                    \
        }                                                                      \
    } while (0)

    /*
     * 一直读到 BUNDLE_END 为止：条目收齐不等于流结束，多出来的 END 留在
     * 连接里会顶掉后面的消息。seen/count 只用来统计对端少发/多发。
     */
    int got_end = 0;
    while (!got_end) {
        buf_init(&b);
        uint8_t t = 0;
        if (ls_msg_recv(fd, &t, &b) != 0) {
            buf_free(&b);
            ls_set_last_error("bundle 传输中断");
            if (cur) {
                cur->abort(cur);
                ls_sink_free(cur);
                cur = NULL;
                cur_failed = 0;
            }
            if (count > seen) {
                failed += count - seen;
                seen = count;
            }
            goto ack;
        }

        if (t == MSG_BUNDLE_ENTRY) {
            SETTLE(); /* 上一个还开着说明它数据没发够 */

            Rd er;
            rd_init(&er, b.p, b.len);
            char *rel = rd_str(&er);
            uint64_t size = rd_u64(&er);
            int64_t mtime = rd_i64(&er);
            uint32_t mode = rd_u32(&er);
            int hdr_ok = !er.err && rel;
            buf_free(&b);
            if (!hdr_ok) {
                free(rel);
                ls_set_last_error("条目头损坏");
                return -1;
            }
            if (seen >= count) {
                /* 对端多发了条目：当作失败项继续读完它的数据 */
                ls_dbg("bundle 收到超出声明数量的条目");
            }
            seen++;

            char err[512] = {0};
            cur = mk ? mk(ctx, rel, size, mtime, mode) : NULL;
            if (!cur) {
                if (!first_err[0]) {
                    snprintf(first_err, sizeof(first_err), "%s",
                             err[0] ? err : "无法创建目标文件");
                }
                cur = ls_sink_null(size);
                cur_failed = 1; /* 数据照收但算失败 */
            }
            free(rel);
            if (!cur) {
                ls_set_last_error("内存不足");
                return -1;
            }
            if (size == 0) {
                SETTLE(); /* 空文件没有数据段 */
            }
            continue;
        }

        if (t == MSG_BUNDLE_DATA) {
            if (cur) {
                if (!cur_failed && cur->write(cur, b.p, b.len) != 0) {
                    if (!first_err[0]) {
                        snprintf(first_err, sizeof(first_err), "%s", ls_last_error());
                    }
                    /* 换成空汇，把剩下的数据读完，避免流错位 */
                    uint64_t done = cur->written;
                    uint64_t want = cur->expect;
                    cur->abort(cur);
                    ls_sink_free(cur);
                    cur = ls_sink_null(want);
                    cur->written = done;
                    cur_failed = 1;
                    if (cur) {
                        (void)cur->write(cur, b.p, b.len); /* 只记账，内容丢弃 */
                    }
                } else if (cur_failed) {
                    (void)cur->write(cur, b.p, b.len);
                }
                total += b.len;
                progress_add(b.len, 0);
                if (cur && cur->written >= cur->expect) {
                    SETTLE();
                    progress_add(0, 1);
                }
            }
            buf_free(&b);
            continue;
        }

        if (t == MSG_BUNDLE_END) {
            buf_free(&b);
            got_end = 1;
            break;
        }

        buf_free(&b);
        ls_set_last_error("bundle 中收到意外消息 %u", t);
        if (cur) {
            cur->abort(cur);
            ls_sink_free(cur);
            cur = NULL;
        }
        return -1;
    }

    SETTLE();

    if (seen < count) {
        failed += count - seen; /* 对端提前收尾，剩下的都算失败 */
    }

ack:
    {
        Buf ack;
        buf_init(&ack);
        /* 有失败就必须让对端知道，否则它会把「全部失败」当成功 */
        buf_put_u8(&ack, failed == 0 ? 1 : 0);
        buf_put_str(&ack, first_err[0] ? first_err : "ok");
        buf_put_u32(&ack, ok);
        buf_put_u32(&ack, failed);
        int rc = ls_msg_send(fd, MSG_BUNDLE_ACK, &ack);
        buf_free(&ack);
        if (rc < 0) {
            ls_set_last_error("回执发送失败: %s", strerror(errno));
            return -1;
        }
    }

#undef SETTLE

    if (own_progress) {
        char buf[768];
        char d[32];
        fmt_size(total, d, sizeof(d));
        if (failed) {
            snprintf(buf, sizeof(buf), "接收：成功 %u / 失败 %u，%s —— %.400s", ok, failed, d,
                     first_err[0] ? first_err : ls_last_error());
        } else {
            snprintf(buf, sizeof(buf), "接收完成：%u 个文件，%s", ok, d);
        }
        progress_end(buf);
    }

    if (ok_n) {
        *ok_n = ok;
    }
    if (failed_n) {
        *failed_n = failed;
    }
    if (bytes) {
        *bytes = total;
    }
    if (first_err[0]) {
        ls_set_last_error("%s", first_err);
    }
    return 0;
}

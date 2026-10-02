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
#include <zlib.h>
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
        fprintf(stderr, "\r\033[K%3d%%", pct);
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
    /* 预计剩余时间只写秒数：省掉「剩」和单位，窄终端上能多留几列给数据 */
    {
        int eta_s = 0;
        if (speed > 0 && g_prog_total > g_prog_done) {
            eta_s = (int)((double)(g_prog_total - g_prog_done) / speed);
        }
        if (eta_s > 99999) {
            eta_s = 99999;
        }
        snprintf(eta, sizeof(eta), "%d", eta_s);
    }

    /* 后缀从详细到简略，挑第一个放得下的 */
    char tails[4][160];
    /* 已完成就没必要显示剩余时间，免得结尾挂个语义不明的 0 */
    int show_eta = (eta[0] && pct < 100 && speed > 0); /* 没有速度时算不出剩余时间 */
    snprintf(tails[0], sizeof(tails[0]), show_eta ? "  %s/%s  %s/s  %s" : "  %s/%s  %s/s", d,
             t, sp, eta);
    snprintf(tails[1], sizeof(tails[1]), show_eta ? "  %s/%s  %s" : "  %s/%s", d, t, eta);
    snprintf(tails[2], sizeof(tails[2]), "  %s", d);
    tails[3][0] = '\0';

    /* "[bar] 100% " —— 不写方向标签，省 5 列给进度条本身 */
    const int pct_cols = 4;
    const int overhead = 2 + pct_cols + 1;

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

    fprintf(stderr, "\r\033[K[%s] %3d%%%s", bar, pct, tail);
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

/* ======================= 性能剖析 ======================= */

static __thread Profile *g_prof = NULL;

void prof_bind(Profile *p)
{
    g_prof = p;
}

Profile *prof_current(void)
{
    return g_prof;
}

void prof_init(Profile *p, const char *role, const char *dir, const char *peer)
{
    memset(p, 0, sizeof(*p));
    p->t_start = ls_now_ms();
    snprintf(p->role, sizeof(p->role), "%s", role ? role : "?");
    snprintf(p->dir, sizeof(p->dir), "%s", dir ? dir : "");
    snprintf(p->peer, sizeof(p->peer), "%s", peer ? peer : "");
    p->active = 1;
}

void prof_begin(const char *name)
{
    Profile *p = g_prof;
    if (!p || !p->active) {
        return;
    }
    p->cur_name = name;
    p->t_phase = ls_now_ms();
}

void prof_end(const char *detail_fmt, ...)
{
    Profile *p = g_prof;
    if (!p || !p->active || !p->cur_name) {
        return;
    }
    uint64_t ms = ls_now_ms() - p->t_phase;

    /* 同名阶段累加（比如多批传输） */
    ProfPhase *ph = NULL;
    for (int i = 0; i < p->n; i++) {
        if (strcmp(p->v[i].name, p->cur_name) == 0) {
            ph = &p->v[i];
            break;
        }
    }
    if (!ph) {
        if (p->n >= PROF_MAX_PHASES) {
            p->cur_name = NULL;
            return;
        }
        ph = &p->v[p->n++];
        ph->name = p->cur_name;
        ph->ms = 0;
        ph->detail[0] = '\0';
    }
    ph->ms += ms;

    if (detail_fmt) {
        char tmp[192];
        va_list ap;
        va_start(ap, detail_fmt);
        vsnprintf(tmp, sizeof(tmp), detail_fmt, ap);
        va_end(ap);
        snprintf(ph->detail, sizeof(ph->detail), "%s", tmp);
    }
    p->cur_name = NULL;
}

void prof_add_bytes(uint64_t raw, uint64_t wire)
{
    Profile *p = g_prof;
    if (!p || !p->active) {
        return;
    }
    p->bytes_raw += raw;
    p->bytes_wire += wire;
}

static void json_escape(const char *in, char *out, size_t cap)
{
    size_t k = 0;
    for (const char *s = in; *s && k + 8 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            out[k++] = '\\';
            out[k++] = (char)c;
        } else if (c < 0x20) {
            k += (size_t)snprintf(out + k, cap - k, "\\u%04x", c);
        } else {
            out[k++] = (char)c;
        }
    }
    out[k] = '\0';
}

void prof_finish(Profile *p, const char *path)
{
    if (!p || !p->active) {
        return;
    }
    if (p->cur_name) {
        prof_end(NULL);
    }
    uint64_t total = ls_now_ms() - p->t_start;

    /* 找瓶颈 */
    const ProfPhase *worst = NULL;
    uint64_t accounted = 0;
    for (int i = 0; i < p->n; i++) {
        accounted += p->v[i].ms;
        if (!worst || p->v[i].ms > worst->ms) {
            worst = &p->v[i];
        }
    }
    uint64_t other = total > accounted ? total - accounted : 0;

    char summary[512];
    if (worst && total > 0) {
        char d[32], w[32];
        fmt_size(p->bytes_raw, d, sizeof(d));
        fmt_size(p->bytes_wire, w, sizeof(w));
        snprintf(summary, sizeof(summary),
                 "耗时 %llu ms，瓶颈是「%s」占 %.0f%%；传输 %s（线上 %s）", 
                 (unsigned long long)total, worst->name,
                 100.0 * (double)worst->ms / (double)total, d, w);
    } else {
        snprintf(summary, sizeof(summary), "耗时 %llu ms（无分阶段数据）",
                 (unsigned long long)total);
    }
    ls_log("性能：%s", summary);

    char outpath[LS_PATHMAX];
    if (path && *path) {
        snprintf(outpath, sizeof(outpath), "%s", path);
    } else if (strcmp(p->role, "server") == 0) {
        /* 服务端要持久化压缩级别，供下次启动读回 */
        snprintf(outpath, sizeof(outpath), "lansync-server.json");
    } else {
        /* 客户端不需要配置文件：它默认几乎不发送数据，调参没意义。
         * 结论已经打在日志里了，够了。 */
        p->active = 0;
        return;
    }

    FILE *f = fopen(outpath, "w");
    if (!f) {
        ls_log("剖析结果写不出去：%s (%s)", outpath, strerror(errno));
        p->active = 0;
        return;
    }

    char esc_dir[LS_PATHMAX * 2], esc_peer[256];
    json_escape(p->dir, esc_dir, sizeof(esc_dir));
    json_escape(p->peer, esc_peer, sizeof(esc_peer));

    fprintf(f, "{\n");
    fprintf(f, "  \"role\": \"%s\",\n", p->role);
    fprintf(f, "  \"version\": \"%s\",\n", ls_version());
    fprintf(f, "  \"build\": \"%s\",\n", ls_build_date());
    fprintf(f, "  \"dir\": \"%s\",\n", esc_dir);
    fprintf(f, "  \"peer\": \"%s\",\n", esc_peer);
    fprintf(f, "  \"total_ms\": %llu,\n", (unsigned long long)total);
    fprintf(f, "  \"bytes_raw\": %llu,\n", (unsigned long long)p->bytes_raw);
    fprintf(f, "  \"bytes_wire\": %llu,\n", (unsigned long long)p->bytes_wire);
    fprintf(f, "  \"bottleneck\": \"%s\",\n", worst ? worst->name : "");
    fprintf(f, "  \"compress_level\": %d,\n", ls_compress_level());
    if (p->bytes_raw > 0 && total > 0) {
        fprintf(f, "  \"throughput_mbps\": %.1f,\n",
                (double)p->bytes_wire / 1048576.0 / ((double)total / 1000.0));
    }
    fprintf(f, "  \"phases\": [\n");
    int need_other = (other > 0);
    for (int i = 0; i < p->n; i++) {
        char esc[384];
        json_escape(p->v[i].detail, esc, sizeof(esc));
        int last = (i + 1 == p->n) && !need_other;
        fprintf(f, "    {\"name\": \"%s\", \"ms\": %llu, \"pct\": %.1f, \"detail\": \"%s\"}%s\n",
                p->v[i].name, (unsigned long long)p->v[i].ms,
                total ? 100.0 * (double)p->v[i].ms / (double)total : 0.0, esc,
                last ? "" : ",");
    }
    if (need_other) {
        fprintf(f, "    {\"name\": \"other\", \"ms\": %llu, \"pct\": %.1f, \"detail\": \"\"}\n",
                (unsigned long long)other, 100.0 * (double)other / (double)total);
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);

    ls_log("剖析结果已写入 %s", outpath);
    p->active = 0;
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
#ifdef LS_DEBUG
static int g_verbose = 0;
#endif

void ls_set_quiet(int q) { g_quiet = q; }

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

#ifdef LS_DEBUG
void ls_set_verbose(int v) { g_verbose = v; }
int  ls_verbose(void) { return g_verbose; }

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
#endif /* LS_DEBUG */

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
    /*
     * 要 IP_PKTINFO：收到包时才知道它从哪个接口进来，回复才能原路返回。
     * 开着 VPN 时这一步是关键，见 common.h 里的说明。
     * 老内核或不支持的平台会失败，那就当没这回事，退化成老行为。
     */
    setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one));
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

ssize_t ls_udp_recv_from(int fd, void *buf, size_t n, char *from_ip, size_t ipcap,
                         uint16_t *from_port, int *ifindex)
{
    struct sockaddr_in from;
    memset(&from, 0, sizeof(from));
    struct iovec iov;
    iov.iov_base = buf;
    iov.iov_len = n;
    char cbuf[CMSG_SPACE(sizeof(struct in_pktinfo))];
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_name = &from;
    msg.msg_namelen = sizeof(from);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof(cbuf);

    ssize_t r = recvmsg(fd, &msg, 0);
    if (r < 0) {
        return r;
    }
    if (from_ip && ipcap) {
        if (!inet_ntop(AF_INET, &from.sin_addr, from_ip, ipcap)) {
            snprintf(from_ip, ipcap, "?");
        }
    }
    if (from_port) {
        *from_port = ntohs(from.sin_port);
    }
    if (ifindex) {
        *ifindex = 0;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c != NULL; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO) {
                struct in_pktinfo pi;
                memcpy(&pi, CMSG_DATA(c), sizeof(pi));
                *ifindex = pi.ipi_ifindex;
                break;
            }
        }
    }
    return r;
}

int ls_udp_send_to(int fd, const void *buf, size_t n, const char *ip, uint16_t port,
                   int ifindex)
{
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &to.sin_addr) != 1) {
        return -1;
    }

    struct iovec iov;
    iov.iov_base = (void *)buf;
    iov.iov_len = n;
    char cbuf[CMSG_SPACE(sizeof(struct in_pktinfo))];
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_name = &to;
    msg.msg_namelen = sizeof(to);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    if (ifindex > 0) {
        /* 指定出接口，绕开路由表的接口选择——开着 VPN 时这就是生死的区别 */
        msg.msg_control = cbuf;
        msg.msg_controllen = CMSG_SPACE(sizeof(struct in_pktinfo));
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = IPPROTO_IP;
        c->cmsg_type = IP_PKTINFO;
        c->cmsg_len = CMSG_LEN(sizeof(struct in_pktinfo));
        struct in_pktinfo pi;
        memset(&pi, 0, sizeof(pi));
        pi.ipi_ifindex = ifindex;
        memcpy(CMSG_DATA(c), &pi, sizeof(pi));
    }
    return sendmsg(fd, &msg, 0);
}

/* 把发往的地址记进 detail（逗号分隔，发送失败的加 "!"） */
static void bc_detail_add(char *detail, size_t cap, const char *addr, int ok)
{
    if (!detail || cap == 0) {
        return;
    }
    size_t n = strlen(detail);
    if (n > 0) {
        if (n + 1 >= cap) {
            return;
        }
        detail[n++] = ',';
        detail[n] = '\0';
    }
    size_t alen = strlen(addr);
    if (n + alen + (ok ? 0 : 1) + 1 > cap) {
        return;
    }
    memcpy(detail + n, addr, alen);
    n += alen;
    if (!ok) {
        detail[n++] = '!';
    }
    detail[n] = '\0';
}

/*
 * 向各网段的广播地址各发一份。多网卡（如 wlan0 + ap0）下只发
 * 255.255.255.255 会受默认路由影响走不到所有网段。
 *
 * detail 里记下实际发往的地址，专门为了排查「发现不到服务端」：
 * 手机上接口一堆（wlan0/ap0/bt-pan/ccmni*），广播发错网段就等于没发。
 */
int ls_udp_broadcast(int fd, uint16_t port, const char *text, char *detail,
                     size_t detail_cap)
{
    size_t len = strlen(text);

    if (detail && detail_cap) {
        detail[0] = '\0';
    }

    int sent = 0;

    /* 1) 全局广播（不指定接口，按默认路由走） */
    int ok = ls_udp_send_to(fd, text, len, "255.255.255.255", port, 0) >= 0;
    if (ok) {
        sent++;
    }
    bc_detail_add(detail, detail_cap, "255.255.255.255", ok);

    /* 2) 每个非回环、支持广播的接口，各发一份到其子网广播地址。
     *    不用 getifaddrs（bionic 从 API 24 才有），也不用 /proc/net/dev
     *    （Android 10+ 普通进程读不到），只用 SIOCGIFCONF，API 21 即可。
     *    发送时带上接口 index：开着 VPN 时只靠路由表选接口会发错地方。 */
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
        bc_detail_add(detail, detail_cap, "(枚举接口失败)", 0);
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
        char ifname[IFNAMSIZ];
        snprintf(ifname, sizeof(ifname), "%s:", req.ifr_name);

        if (ioctl(s, SIOCGIFBRDADDR, &req) < 0) {
            bc_detail_add(detail, detail_cap, ifname, 0);
            continue;
        }
        struct sockaddr_in *br = (struct sockaddr_in *)&req.ifr_broadaddr;
        if (br->sin_addr.s_addr == 0) {
            continue;
        }

        /* 这个接口的 index，发送时用它把包钉在这个接口上 */
        struct ifreq ireq;
        memset(&ireq, 0, sizeof(ireq));
        snprintf(ireq.ifr_name, IFNAMSIZ, "%s", req.ifr_name);
        int ifidx = ioctl(s, SIOCGIFINDEX, &ireq) == 0 ? ireq.ifr_ifindex : 0;

        char ip[32];
        char addr[64];
        if (!inet_ntop(AF_INET, &br->sin_addr, ip, sizeof(ip))) {
            snprintf(ip, sizeof(ip), "?");
        }
        /* 带上接口名：发现不到服务端时，一眼能看出 wlan0 到底在不在里面 */
        snprintf(addr, sizeof(addr), "%s(%s)", ip, req.ifr_name);
        ok = ls_udp_send_to(fd, text, len, ip, port, ifidx) >= 0;
        if (ok) {
            sent++;
        }
        bc_detail_add(detail, detail_cap, addr, ok);
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

int flist_add(FileList *l, const char *rel, uint64_t size, int64_t mtime, uint64_t hash)
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
    e->hash = hash;
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
     * 创建/修改的不同内容文件在快速路径上被误判为相同。
     * hash 留 0：扫描默认零读取，要算内容哈希的场合自己扫完再补一遍。 */
    int64_t mt = (int64_t)st.st_mtim.tv_sec * 1000000000LL + (int64_t)st.st_mtim.tv_nsec;
    if (flist_add(out, childrel, (uint64_t)st.st_size, mt, 0) < 0) {
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

    /*
     * 把服务端的 mtime 写上去。
     *
     * 设不上**不算失败**：内容已经正确落盘了，而客户端启动时探测过自己能不能
     * 保住 mtime，保不住的话服务端那边本来就改用内容哈希比对了，不会因为时间戳
     * 是「现在」就反复全量重传。这里只记一笔，别把文件判死。
     */
    if (set_mtime(fs->dst, fs->mtime) != 0) {
        ls_dbg("时间戳没设上 %s: %s（内容已正确落盘，判定会走内容哈希）", fs->dst,
               strerror(errno));
    }
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

static int g_zlevel = LS_COMPRESS_LEVEL;

void ls_set_compress_level(int lv)
{
    if (lv < 1) {
        lv = 1;
    }
    if (lv > 9) {
        lv = 9;
    }
    g_zlevel = lv;
}

int ls_compress_level(void)
{
    return g_zlevel;
}

double ls_cfg_get(const char *json, const char *key, double def)
{
    if (!json || !key) {
        return def;
    }
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) {
        return def;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t') {
        p++;
    }
    char *end = NULL;
    double v = strtod(p, &end);
    return end == p ? def : v;
}

int ls_should_compress(const char *root, const BundleEntry *ents, size_t n, int mode)
{
    if (mode == LS_COMPRESS_ON) {
        return 1;
    }
    if (mode == LS_COMPRESS_OFF) {
        return 0;
    }
    double hsum = 0;
    size_t cnt = 0;
    for (size_t i = 0; i < n && cnt < 32; i++) {
        if (ents[i].size < 512) {
            continue; /* 太小的文件压不动，还白添压缩头 */
        }
        char full[LS_PATHMAX];
        const char *p = ents[i].srcpath;
        if (!p) {
            if (!path_safe(ents[i].rel) ||
                path_join(full, sizeof(full), root, ents[i].rel) < 0) {
                continue;
            }
            p = full;
        }
        double h = ls_entropy_file(p, 8192);
        if (h >= 0) {
            hsum += h;
            cnt++;
        }
    }
    if (cnt == 0) {
        return 0;
    }
    return (hsum / (double)cnt) < LS_COMPRESS_ENTROPY_MAX;
}

int ls_send_bundle(int fd, const char *root, const BundleEntry *ents, size_t n, int compress,
                   uint32_t *ok_n, uint32_t *failed_n, uint64_t *bytes)
{
    Buf b;
    uint8_t *io = NULL;
    uint8_t *zbuf = NULL;
    z_stream zs;
    int zok = 0;
    uint64_t total = 0;      /* 原始字节 */
    uint64_t wire = 0;       /* 实际发出去的字节（压缩后） */
    uint32_t local_failed = 0;
    char first_local_err[512] = {0};

    memset(&zs, 0, sizeof(zs));
    /*
     * 级别取 1 而不是默认的 6：局域网也就 30~80 MB/s，而 level 6 的压缩速度
     * 只有 ~50 MB/s，会把网络晾着；level 1 有 ~150 MB/s，压缩不再是瓶颈，
     * 压缩率只差几个百分点。
     */
    if (compress && deflateInit(&zs, g_zlevel) == Z_OK) {
        zok = 1;
        zbuf = (uint8_t *)malloc(LS_CHUNK);
        if (!zbuf) {
            deflateEnd(&zs);
            zok = 0;
        }
    }

    uint64_t declared = 0;
    for (size_t i = 0; i < n; i++) {
        declared += ents[i].size;
    }

    buf_init(&b);
    buf_put_u32(&b, (uint32_t)n);
    buf_put_u64(&b, declared);
    buf_put_u8(&b, (uint8_t)(zok ? 1 : 0));
    if (ls_msg_send(fd, MSG_BUNDLE_BEGIN, &b) != 0) {
        buf_free(&b);
        free(zbuf);
        if (zok) {
            deflateEnd(&zs);
        }
        ls_set_last_error("发送 BUNDLE_BEGIN 失败: %s", strerror(errno));
        return -1;
    }
    buf_free(&b);

    io = (uint8_t *)malloc(LS_CHUNK);
    if (!io) {
        free(zbuf);
        if (zok) {
            deflateEnd(&zs);
        }
        ls_set_last_error("内存不足");
        return -1;
    }

    /* 把一块压缩输出发出去；返回 -1 表示发送失败 */
    int send_pending = 0;

    for (size_t i = 0; i < n && !send_pending; i++) {
        const BundleEntry *e = &ents[i];
        char full[LS_PATHMAX] = {0};
        char err[512] = {0};

        if (e->srcpath) {
            snprintf(full, sizeof(full), "%s", e->srcpath);
        } else if (!path_safe(e->rel) || path_join(full, sizeof(full), root, e->rel) < 0) {
            snprintf(err, sizeof(err), "路径非法或过长: %s", e->rel);
            local_failed++;
        }

        buf_init(&b);
        buf_put_str(&b, e->rel);
        buf_put_u64(&b, e->size);
        buf_put_i64(&b, e->mtime);
        buf_put_u32(&b, e->mode);
        int rc = ls_msg_send(fd, MSG_BUNDLE_ENTRY, &b);
        buf_free(&b);
        if (rc < 0) {
            ls_set_last_error("发送 BUNDLE_ENTRY 失败: %s", strerror(errno));
            send_pending = 1;
            break;
        }
        if (err[0]) {
            continue; /* 一个字节都不发，接收端会因长度不符丢弃 */
        }

        LsSource *src = ls_source_file(full, err, sizeof(err));
        if (!src) {
            if (!first_local_err[0]) {
                snprintf(first_local_err, sizeof(first_local_err), "%s", err);
            }
            ls_set_last_error("%s", err);
            local_failed++;
            continue;
        }

        Buf d;
        buf_init(&d);
        ssize_t got;
        int readfail = 0;

        while ((got = src->read(src, io, LS_CHUNK)) > 0) {
            if (zok) {
                zs.next_in = io;
                zs.avail_in = (uInt)got;
                while (zs.avail_in > 0) {
                    zs.next_out = zbuf;
                    zs.avail_out = LS_CHUNK;
                    if (deflate(&zs, Z_NO_FLUSH) == Z_STREAM_ERROR) {
                        readfail = 1;
                        break;
                    }
                    size_t produced = LS_CHUNK - zs.avail_out;
                    if (produced) {
                        buf_reset(&d);
                        buf_put_bytes(&d, zbuf, produced);
                        if (ls_msg_send(fd, MSG_BUNDLE_DATA, &d) < 0) {
                            send_pending = 1;
                            break;
                        }
                        wire += produced;
                    }
                }
            } else {
                buf_reset(&d);
                buf_put_bytes(&d, io, (size_t)got);
                if (ls_msg_send(fd, MSG_BUNDLE_DATA, &d) < 0) {
                    send_pending = 1;
                    break;
                }
                wire += (uint64_t)got;
            }
            if (send_pending || readfail) {
                break;
            }
            total += (uint64_t)got;
            progress_add((uint64_t)got, 0);
        }
        if (!send_pending && !readfail && got < 0) {
            readfail = 1;
        }

        /* 条目收尾：flush 出本条目的全部压缩输出，边界才清晰 */
        if (zok && !send_pending && !readfail) {
            do {
                zs.next_out = zbuf;
                zs.avail_out = LS_CHUNK;
                if (deflate(&zs, Z_SYNC_FLUSH) == Z_STREAM_ERROR) {
                    readfail = 1;
                    break;
                }
                size_t produced = LS_CHUNK - zs.avail_out;
                if (produced) {
                    buf_reset(&d);
                    buf_put_bytes(&d, zbuf, produced);
                    if (ls_msg_send(fd, MSG_BUNDLE_DATA, &d) < 0) {
                        send_pending = 1;
                        break;
                    }
                    wire += produced;
                }
            } while (zs.avail_out == 0);
        }
        buf_free(&d);
        ls_source_free(src);

        if (readfail) {
            ls_set_last_error("读取源文件中途失败: %s", e->rel);
            local_failed++;
        }
    }
    free(io);
    free(zbuf);
    if (zok) {
        deflateEnd(&zs);
    }

    if (send_pending) {
        ls_set_last_error("发送数据失败: %s", strerror(errno));
        return -1;
    }

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

    if (zok && total > 0 && wire > 0) {
        ls_dbg("压缩：%llu -> %llu 字节（%.0f%%）", (unsigned long long)total,
               (unsigned long long)wire, 100.0 * (double)wire / (double)total);
    }
    prof_add_bytes(total, wire);
    if (local_failed > 0) {
        ls_log("本端有 %u 个文件读不了，例如：%s", local_failed,
               first_local_err[0] ? first_local_err : "(未知)");
    }
    if (ok_n) {
        *ok_n = done;
    }
    if (failed_n) {
        *failed_n = fail;
    }
    if (bytes) {
        *bytes = total;
    }
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
    uint8_t compressed = rd_u8(&r);
    if (r.err || count > LS_MAX_FILES) {
        ls_set_last_error("bundle 起始报文损坏");
        return -1;
    }

    /* 外层没在统计时自己起一个进度条（比如客户端接收） */
    int own_progress = !progress_active();
    if (own_progress) {
        progress_begin("接收", declared, count);
    }

    z_stream zs;
    uint8_t *zbuf = NULL;
    int zok = 0;
    memset(&zs, 0, sizeof(zs));
    if (compressed) {
        if (inflateInit(&zs) != Z_OK) {
            ls_set_last_error("无法初始化解压流");
            if (own_progress) {
                progress_end(NULL);
            }
            return -1;
        }
        zbuf = (uint8_t *)malloc(LS_CHUNK);
        if (!zbuf) {
            inflateEnd(&zs);
            ls_set_last_error("内存不足");
            if (own_progress) {
                progress_end(NULL);
            }
            return -1;
        }
        zok = 1;
    }

    uint32_t ok = 0, failed = 0;
    uint64_t total = 0;
    uint64_t wire_in = 0;
    uint32_t seen = 0;
    LsSink *cur = NULL;
    int cur_failed = 0;
    char first_err[512] = {0};
    Buf b;

    /* 结算当前条目：写满了就 finish，否则丢弃 */
#define SETTLE()                                                                    \
    do {                                                                            \
        if (cur) {                                                                  \
            if (cur_failed || cur->finish(cur) != 0) {                              \
                failed++;                                                           \
                if (!first_err[0]) {                                                \
                    snprintf(first_err, sizeof(first_err), "%s", ls_last_error());  \
                }                                                                   \
            } else {                                                                \
                ok++;                                                               \
            }                                                                       \
            ls_sink_free(cur);                                                      \
            cur = NULL;                                                             \
            cur_failed = 0;                                                         \
        }                                                                           \
    } while (0)

    /* 往当前汇里写 data；本地写不了就换成丢弃汇，保证流不错位 */
#define FEED(data, len)                                                             \
    do {                                                                            \
        size_t _room = (size_t)(cur->expect - cur->written);                        \
        size_t _take = (len) < _room ? (size_t)(len) : _room;                       \
        if (_take > 0) {                                                            \
            if (!cur_failed && cur->write(cur, (data), _take) != 0) {                \
                if (!first_err[0]) {                                                \
                    snprintf(first_err, sizeof(first_err), "%s", ls_last_error());  \
                }                                                                   \
                uint64_t _done = cur->written;                                      \
                uint64_t _want = cur->expect;                                       \
                cur->abort(cur);                                                    \
                ls_sink_free(cur);                                                  \
                cur = ls_sink_null(_want);                                          \
                cur_failed = 1;                                                     \
                if (cur) {                                                          \
                    cur->written = _done + _take;                                   \
                }                                                                   \
            } else if (cur_failed) {                                                \
                cur->written += _take;                                              \
            }                                                                       \
        }                                                                           \
        if (cur && cur->written >= cur->expect) {                                   \
            SETTLE();                                                               \
        }                                                                           \
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
                goto fail_out;
            }
            if (seen >= count) {
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
                cur_failed = 1;
            }
            free(rel);
            if (!cur) {
                ls_set_last_error("内存不足");
                goto fail_out;
            }
            if (size == 0) {
                SETTLE(); /* 空文件没有数据段 */
            }
            continue;
        }

        if (t == MSG_BUNDLE_DATA) {
            if (cur || zok) {
                wire_in += b.len;
                if (zok) {
                    zs.next_in = b.p;
                    zs.avail_in = (uInt)b.len;
                    int zbad = 0;
                    while (zs.avail_in > 0) {
                        zs.next_out = zbuf;
                        zs.avail_out = LS_CHUNK;
                        int zr = inflate(&zs, Z_NO_FLUSH);
                        size_t produced = LS_CHUNK - zs.avail_out;
                        if (produced > 0 && cur) {
                            FEED(zbuf, produced);
                            total += produced;
                            progress_add(produced, 0);
                        }
                        if (zr == Z_STREAM_END) {
                            break;
                        }
                        if (zr != Z_OK && zr != Z_BUF_ERROR) {
                            zbad = 1;
                            break;
                        }
                    }
                    if (zbad) {
                        buf_free(&b);
                        ls_set_last_error("解压失败（数据损坏或两端不一致）");
                        goto fail_out;
                    }
                } else if (cur) {
                    size_t room = (size_t)(cur->expect - cur->written);
                    if (b.len <= room) {
                        FEED(b.p, b.len);
                    } else {
                        FEED(b.p, room); /* 超出部分丢弃 */
                    }
                    total += b.len;
                    progress_add(b.len, 0);
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
        goto fail_out;
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
            if (zok) {
                inflateEnd(&zs);
                free(zbuf);
            }
            if (own_progress) {
                progress_end(NULL);
            }
            return -1;
        }
    }

#undef SETTLE
#undef FEED

    if (zok) {
        inflateEnd(&zs);
        free(zbuf);
    }

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

    prof_add_bytes(total, wire_in);

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

fail_out:
    if (cur) {
        cur->abort(cur);
        ls_sink_free(cur);
    }
    if (zok) {
        inflateEnd(&zs);
        free(zbuf);
    }
    if (own_progress) {
        progress_end(NULL);
    }
    return -1;
}

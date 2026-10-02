/*
 * common.h -- lansync 公共层
 *
 * 一切文件内容都通过「流」进出（LsSource / LsSink），传输层只认流不认文件。
 * 以后要加压缩、小文件内存缓冲、打包，只需在流上套一层，协议和传输层都不用动。
 */
#ifndef LANSYNC_COMMON_H
#define LANSYNC_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#define LS_PROTO        1

/* 编译时刻，由 Makefile 通过 -D 注入；直接 gcc 编译时为 0/unknown */
#ifndef LS_BUILD_TS
#define LS_BUILD_TS 0
#endif
#ifndef LS_BUILD_DATE
#define LS_BUILD_DATE "unknown"
#endif
#define LS_CHUNK        (256 * 1024)      /* 单条 DATA 消息的字节数 */
#define LS_MAX_PAYLOAD  (8 * 1024 * 1024) /* 单条消息上限，防恶意分配 */
#define LS_PATHMAX      4096
#define LS_DEF_PORT     9527
#define LS_DEF_UDP_PORT 9528
#define LS_TMP_PREFIX   ".lansync-tmp-"

/*
 * mtime 容差。不同文件系统的精度差很多：f2fs 记到纳秒，Android 的 FUSE
 * (/storage) 只剩秒。严格按纳秒比较会永远判「不同」，导致每次全量重传。
 */
#define LS_MTIME_TOLERANCE_NS (2LL * 1000000000LL)

/* 压缩模式 */
enum { LS_COMPRESS_AUTO = 0, LS_COMPRESS_ON = 1, LS_COMPRESS_OFF = 2 };
/* auto 模式下，采样平均熵低于这个值才压 */
#define LS_COMPRESS_ENTROPY_MAX 6.5

/* 一个 bundle 最多装多少字节 / 多少个条目，超过就切开 */
#define LS_BUNDLE_BYTES (64ull * 1024 * 1024)
#define LS_BUNDLE_FILES 2000

/* ---------------- 消息类型 ---------------- */
enum {
    MSG_HELLO      = 1,  /* C->S {str name, str root, u32 proto, u64 build_ts, u8 mtime_ok} */
    MSG_HELLO_ACK  = 2,  /* S->C {str sname, str sroot, u32 proto, u8 prefer, u8 del,
                                 str client_subdir} */
    MSG_LIST_BEGIN = 3,  /* C->S {u32 total} */
    MSG_LIST_CHUNK = 4,  /* C->S {u32 n, [str rel, u64 size, i64 mtime]...} */
    MSG_LIST_END   = 5,  /* C->S {} */
    MSG_DELETE     = 6,  /* S->C {str rel} */
    MSG_LOG        = 7,  /* S->C {str text} */
    MSG_BYE        = 8,  /* S->C {} */
    MSG_SYNC_DONE  = 9,  /* S->C {u32 push, u32 pull, u32 del, u32 same, u32 fail,
                                 u32 ignored, u64 bytes} */
    MSG_WANT_SELF  = 10, /* C->S {}  客户端索要服务端分发的程序本体 */
    /* ---- 流式传输：一个 bundle 里装若干文件，数据段是连续字节流 ---- */
    MSG_BUNDLE_BEGIN = 17, /* {u32 count, u64 total_bytes, u8 compressed} */
    MSG_BUNDLE_ENTRY = 18, /* {str rel, u64 size, i64 mtime, u32 mode} */
    MSG_BUNDLE_DATA  = 19, /* {bytes} 属于上一个 ENTRY 的内容 */
    MSG_BUNDLE_END   = 20, /* {} 一个 bundle 结束 */
    MSG_BUNDLE_ACK   = 21, /* {u8 ok, str msg, u32 done, u32 failed} */
    MSG_PULL_WANT    = 22, /* S->C {u32 n, [str rel]...} 服务端要客户端把这些发来 */
    MSG_PLAN         = 23  /* S->C {u32 push_files, u64 push_bytes, u32 pull_files,
                                     u64 pull_bytes} 先告诉客户端要做多少事，
                                     好让它的进度条贯穿整场同步而不是每块重来 */
};

/* 单个清单分片的条目上限 */
#define LS_LIST_BATCH   1000
#define LS_LIST_BYTES   (4 * 1024 * 1024)
#define LS_MAX_FILES    2000000u

enum { PREFER_NEWER = 0, PREFER_SERVER = 1, PREFER_CLIENT = 2 };

/* ---------------- 可增长字节缓冲 ---------------- */
typedef struct {
    uint8_t *p;
    size_t   len;
    size_t   cap;
} Buf;

void buf_init(Buf *b);
void buf_free(Buf *b);
void buf_reset(Buf *b);
void buf_reserve(Buf *b, size_t extra);
void buf_put_u8(Buf *b, uint8_t v);
void buf_put_u32(Buf *b, uint32_t v);
void buf_put_u64(Buf *b, uint64_t v);
void buf_put_i64(Buf *b, int64_t v);
void buf_put_bytes(Buf *b, const void *data, size_t n);
void buf_put_str(Buf *b, const char *s);

/* 只读游标；任何越界读取都会置 err 并返回 0/NULL */
typedef struct {
    const uint8_t *p;
    size_t         len;
    size_t         pos;
    int            err;
} Rd;

void            rd_init(Rd *r, const uint8_t *p, size_t len);
uint8_t         rd_u8(Rd *r);
uint32_t        rd_u32(Rd *r);
uint64_t        rd_u64(Rd *r);
int64_t         rd_i64(Rd *r);
const uint8_t  *rd_bytes(Rd *r, size_t n);
char           *rd_str(Rd *r); /* malloc 出来，调用方 free；失败返回 NULL */

/* ---------------- socket ---------------- */
int  ls_send_all(int fd, const void *data, size_t len);
int  ls_recv_all(int fd, void *data, size_t len);
int  ls_msg_send(int fd, uint8_t type, const Buf *payload);
int  ls_msg_recv(int fd, uint8_t *type, Buf *payload);
int  ls_expect(int fd, uint8_t want, Buf *out);
void ls_set_nodelay(int fd);
void ls_set_keepalive(int fd);
/* 收发超时（秒），0 表示不超时。防止对端卡死时本端永久阻塞 */
void ls_set_timeout(int fd, int seconds);
int  ls_tcp_listen(uint16_t port, int backlog);
int  ls_tcp_connect(const char *host, uint16_t port);

int  ls_udp_open(uint16_t port, int reuse);
int  ls_udp_broadcast(int fd, uint16_t port, const char *text);
uint64_t ls_now_ms(void);

/* ---------------- 流 ---------------- */
/*
 * LsSource：产生字节。read 返回 >0 表示读到的字节数，0 表示结束，-1 出错。
 * LsSink  ：消费字节。finish 时才算落定（文件汇是 rename），abort 则丢弃。
 *
 * 这一层是后续扩展的挂点：压缩、批量打包、小文件先攒内存，都只需要换一个实现。
 */
typedef struct LsSource LsSource;
struct LsSource {
    ssize_t (*read)(LsSource *s, void *buf, size_t n);
    void    (*close)(LsSource *s);
    void     *impl;
    char      name[LS_PATHMAX];
};

typedef struct LsSink LsSink;
struct LsSink {
    int  (*write)(LsSink *s, const void *buf, size_t n);
    int  (*finish)(LsSink *s);
    void (*abort)(LsSink *s);
    void     *impl;
    uint64_t  written;
    uint64_t  expect;
    char      name[LS_PATHMAX];
};

/* 打开一个文件作为数据源；失败返回 NULL 并填 err */
LsSource *ls_source_file(const char *path, char *err, size_t errlen);
/* 打开 root/rel 作为落盘目标（先写同目录临时文件，finish 时 rename） */
LsSink   *ls_sink_file(const char *root, const char *rel, uint64_t size, int64_t mtime,
                       uint32_t mode, char *err, size_t errlen);
/* 落到指定绝对路径（client update 用） */
LsSink   *ls_sink_abs(const char *fullpath, uint64_t size, int64_t mtime, uint32_t mode,
                      char *err, size_t errlen);
/* 丢弃一切数据，只记账。用于本地写不了时保持流对齐 */
LsSink   *ls_sink_null(uint64_t size);

/* 释放；sink 若未 finish 会被 abort */
void ls_source_free(LsSource *s);
void ls_sink_free(LsSink *s);

/* ---------------- 文件与目录 ---------------- */
typedef struct {
    char     *rel;   /* 相对 root 的路径，'/' 分隔 */
    uint64_t  size;
    int64_t   mtime; /* 纳秒精度：判定「有没有变」完全靠它 */
} FileEntry;

typedef struct {
    FileEntry *v;
    size_t     n, cap;
} FileList;

void       flist_init(FileList *l);
void       flist_free(FileList *l);
FileEntry *flist_find(const FileList *l, const char *rel);
int        flist_add(FileList *l, const char *rel, uint64_t size, int64_t mtime);
void       flist_sort(FileList *l);

/* 只 stat 不算内容哈希：几万个文件也很快 */
int  fs_scan(const char *root, FileList *out, char *err, size_t errlen);

/*
 * 采样文件开头若干字节，算香农熵（bit/byte，0~8）。
 * 用来判断「值不值得压」：< 6 基本是文本/日志/JSON，压缩收益大；
 * > 7.5 基本是已压缩数据（jpg/mp4/zip/apk）或加密数据，再压白费 CPU。
 * 返回 -1 表示文件读不了。
 */
double ls_entropy_file(const char *path, size_t sample_bytes);

int  path_safe(const char *rel);                 /* 拒绝绝对路径 / ".." / 空段 */
int  path_join(char *out, size_t n, const char *root, const char *rel);
int  mkdirs_for(const char *fullpath);
int  set_mtime(const char *path, int64_t mtime_ns);

/* ---------------- bundle：把若干文件当一个字节流传输 ---------------- */

typedef struct {
    char       *rel;    /* 对端看到的路径 */
    const char *srcpath;/* 非 NULL 时直接用它读数据，忽略 root/rel（client update 用） */
    uint64_t    size;
    int64_t     mtime;  /* 纳秒 */
    uint32_t    mode;   /* 权限位，0 表示用默认 */
} BundleEntry;

/*
 * 发送一组条目。数据段首尾相连，接收端按 ENTRY 声明的 size 切分。
 * 某个条目打不开或读一半出错，就少发它的数据——接收端会因长度不符丢弃它，
 * 协议不会错位，后面的条目照常。
 * ok_n / failed_n / bytes 可为 NULL。
 */
/* compress 非 0 时整批走一个 deflate 流；每个条目结束会 flush，
 * 保证「一个条目的数据解压后恰好是它的 size 字节」这个边界成立。 */
int ls_send_bundle(int fd, const char *root, const BundleEntry *ents, size_t n,
                   int compress, uint32_t *ok_n, uint32_t *failed_n, uint64_t *bytes);

/* 采样估算这批值不值得压：auto 模式最多抽 32 个文件的前 8 KiB 算平均熵。
 * 文本/日志/JSON 通常低于 6.5，值得压；jpg/mp4/zip 在 7.5 以上，压了白费 CPU。 */
int ls_should_compress(const char *root, const BundleEntry *ents, size_t n, int mode);

/* 接收端的落盘目标由工厂创建；工厂返回 NULL 表示该条目丢弃（计数为失败）。
 * ctx 由调用方定义。 */
typedef LsSink *(*LsSinkFactory)(void *ctx, const char *rel, uint64_t size,
                                 int64_t mtime, uint32_t mode);

int ls_recv_bundle(int fd, LsSinkFactory mk, void *ctx, uint32_t *ok_n,
                   uint32_t *failed_n, uint64_t *bytes);
/* begin 是已经收到的 MSG_BUNDLE_BEGIN 载荷（调用方先看了消息类型） */
int ls_recv_bundle_body(int fd, const Buf *begin, LsSinkFactory mk, void *ctx,
                        uint32_t *ok_n, uint32_t *failed_n, uint64_t *bytes);

/* ---------------- 进度 ---------------- */

/*
 * 传输期间在终端原地刷新一行进度；stderr 不是终端时退化成每 10% 一行，
 * 免得往日志文件里刷几百行。日志输出会先擦掉进度行、打完再补回来。
 */
void progress_begin(const char *label, uint64_t total_bytes, uint32_t total_files);
void progress_add(uint64_t bytes, uint32_t files);
void progress_end(const char *summary);
int  progress_active(void);

/* 人类可读的大小，如 "65.4 MiB" */
void fmt_size(uint64_t n, char *out, size_t cap);

/* 最近一次传输失败的原因（线程局部，供调用方打印） */
const char *ls_last_error(void);
void ls_set_last_error(const char *fmt, ...);

/* ---------------- 版本 ---------------- */

const char *ls_version(void);    /* "1.0" */
const char *ls_build_date(void); /* 编译时刻，人读 */
uint64_t    ls_build_ts(void);   /* 编译时刻，Unix 秒，用于比对 */
/* "lansync 1.0 (built 2025-10-02 03:47:53)" */
const char *ls_build_info(void);

/* ---------------- 日志 ---------------- */
void ls_set_quiet(int q);
void ls_set_verbose(int v);
int  ls_verbose(void);
void ls_log(const char *fmt, ...);
void ls_dbg(const char *fmt, ...);

#endif /* LANSYNC_COMMON_H */

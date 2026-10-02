/*
 * tls.c -- TLS 1.3 会话管理
 *
 * 一张 fd -> WOLFSSL* 的小表，`ls_send_all` / `ls_recv_all` 查它决定走加密。
 * 表小是因为 lansync 的并发连接本来就少（服务端每客户端一个线程，同一时刻
 * 通常就一两台设备在同步）。
 */
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tls.h"
#include "server_cert.h"

#define LS_MAX_CONN 16

static WOLFSSL_CTX *g_srv_ctx;
static WOLFSSL_CTX *g_cli_ctx;

typedef struct {
    int      fd;
    WOLFSSL *ssl;
} TlsConn;

static TlsConn         g_conn[LS_MAX_CONN];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t  g_once = PTHREAD_ONCE_INIT;

static void tls_global_init(void)
{
    wolfSSL_Init();
}

static void seterr(char *err, size_t errlen, const char *fmt, ...)
{
    if (err == NULL || errlen == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

/* 查表拿会话。返回后调用方独占使用该 ssl —— 同一 fd 不会被并发关闭
 * （连接的生命周期由它自己的线程管）。 */
static WOLFSSL *lookup(int fd)
{
    WOLFSSL *ssl = NULL;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < LS_MAX_CONN; i++) {
        if (g_conn[i].ssl != NULL && g_conn[i].fd == fd) {
            ssl = g_conn[i].ssl;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return ssl;
}

int ls_tls_registered(int fd)
{
    return lookup(fd) != NULL;
}

static int reg_put(int fd, WOLFSSL *ssl)
{
    int rc = -1;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < LS_MAX_CONN; i++) {
        if (g_conn[i].ssl == NULL) {
            g_conn[i].fd  = fd;
            g_conn[i].ssl = ssl;
            rc = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

static WOLFSSL *reg_take(int fd)
{
    WOLFSSL *ssl = NULL;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < LS_MAX_CONN; i++) {
        if (g_conn[i].ssl != NULL && g_conn[i].fd == fd) {
            ssl = g_conn[i].ssl;
            g_conn[i].ssl = NULL;
            g_conn[i].fd  = -1;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return ssl;
}

/* ---------------- 初始化 ---------------- */

int ls_tls_server_init(const char *certfile, const char *keyfile,
                       char *err, size_t errlen)
{
    pthread_once(&g_once, tls_global_init);

    WOLFSSL_CTX *ctx = wolfSSL_CTX_new(wolfTLSv1_3_server_method());
    if (ctx == NULL) {
        seterr(err, errlen, "创建 TLS 上下文失败");
        return -1;
    }
    if (wolfSSL_CTX_use_certificate_file(ctx, certfile,
                                         WOLFSSL_FILETYPE_PEM) != WOLFSSL_SUCCESS) {
        seterr(err, errlen, "载入证书失败：%s", certfile);
        wolfSSL_CTX_free(ctx);
        return -1;
    }
    if (wolfSSL_CTX_use_PrivateKey_file(ctx, keyfile,
                                        WOLFSSL_FILETYPE_PEM) != WOLFSSL_SUCCESS) {
        seterr(err, errlen, "载入私钥失败：%s", keyfile);
        wolfSSL_CTX_free(ctx);
        return -1;
    }
    /* 服务端不验证客户端证书 —— 客户端身份由应用层六位验证码管 */
    wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_NONE, NULL);

    g_srv_ctx = ctx;
    return 0;
}

int ls_tls_client_init(char *err, size_t errlen)
{
    pthread_once(&g_once, tls_global_init);

    WOLFSSL_CTX *ctx = wolfSSL_CTX_new(wolfTLSv1_3_client_method());
    if (ctx == NULL) {
        seterr(err, errlen, "创建 TLS 上下文失败");
        return -1;
    }
    /* 信任锚只有编译进来的那一张证书：别的公钥一律握不上手 */
    if (wolfSSL_CTX_load_verify_buffer(ctx, LS_SERVER_CERT, LS_SERVER_CERT_LEN,
                                       WOLFSSL_FILETYPE_ASN1) != WOLFSSL_SUCCESS) {
        seterr(err, errlen, "内置证书无法载入（构建有问题？）");
        wolfSSL_CTX_free(ctx);
        return -1;
    }
    wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_PEER, NULL);

    g_cli_ctx = ctx;
    return 0;
}

/* ---------------- 握手 ---------------- */

static int handshake(int fd, int server, char *err, size_t errlen)
{
    WOLFSSL_CTX *ctx = server ? g_srv_ctx : g_cli_ctx;
    if (ctx == NULL) {
        seterr(err, errlen, "TLS 未初始化（内部错误）");
        return -1;
    }

    WOLFSSL *ssl = wolfSSL_new(ctx);
    if (ssl == NULL) {
        seterr(err, errlen, "分配 TLS 会话失败");
        return -1;
    }
    wolfSSL_set_fd(ssl, fd);

    int r = server ? wolfSSL_accept(ssl) : wolfSSL_connect(ssl);
    if (r != WOLFSSL_SUCCESS) {
        int e = wolfSSL_get_error(ssl, r);
        const char *why = wolfSSL_ERR_reason_error_string(e);
        if (e == WOLFSSL_ERROR_WANT_READ || e == WOLFSSL_ERROR_WANT_WRITE) {
            seterr(err, errlen, "握手未完成（socket 提前断开）");
        } else {
            seterr(err, errlen, "TLS 握手失败：%s", why ? why : "未知原因");
        }
        wolfSSL_free(ssl);
        return -1;
    }

    if (reg_put(fd, ssl) != 0) {
        seterr(err, errlen, "TLS 连接数超限（%d）", LS_MAX_CONN);
        wolfSSL_free(ssl);
        return -1;
    }
    return 0;
}

int ls_tls_accept(int fd, char *err, size_t errlen)
{
    return handshake(fd, 1, err, errlen);
}

int ls_tls_connect(int fd, char *err, size_t errlen)
{
    return handshake(fd, 0, err, errlen);
}

void ls_tls_close(int fd)
{
    WOLFSSL *ssl = reg_take(fd);
    if (ssl != NULL) {
        wolfSSL_shutdown(ssl);
        wolfSSL_free(ssl);
    }
}

/* ---------------- 收发 ---------------- */

int ls_tls_send(int fd, const void *data, size_t len)
{
    WOLFSSL *ssl = lookup(fd);
    if (ssl == NULL) {
        return -1;
    }
    const unsigned char *p = (const unsigned char *)data;
    while (len > 0) {
        int chunk = (len > (size_t)INT_MAX) ? INT_MAX : (int)len;
        int n = wolfSSL_write(ssl, p, chunk);
        if (n <= 0) {
            int e = wolfSSL_get_error(ssl, n);
            if (e == WOLFSSL_ERROR_WANT_READ || e == WOLFSSL_ERROR_WANT_WRITE) {
                continue;
            }
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int ls_tls_recv(int fd, void *data, size_t len)
{
    WOLFSSL *ssl = lookup(fd);
    if (ssl == NULL) {
        return -1;
    }
    unsigned char *p = (unsigned char *)data;
    while (len > 0) {
        int chunk = (len > (size_t)INT_MAX) ? INT_MAX : (int)len;
        int n = wolfSSL_read(ssl, p, chunk);
        if (n <= 0) {
            int e = wolfSSL_get_error(ssl, n);
            if (e == WOLFSSL_ERROR_WANT_READ || e == WOLFSSL_ERROR_WANT_WRITE) {
                continue;
            }
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

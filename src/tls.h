/*
 * tls.h -- 传输层加密（TLS 1.3）
 *
 * lansync 的 TCP 通道只有这一条路：`ls_send_all` / `ls_recv_all` 只对**已登记
 * TLS 会话**的 fd 工作，没登记的一律返回错误。这样就不存在「某处忘了加密、
 * 悄悄走明文」的写法 —— 写错了会立刻失败，而不是静默降级。
 *
 * 身份模型：
 *   - 客户端内置服务端证书（src/server_cert.h），只认这一把公钥 —— 防中间人
 *   - 服务端持有对应私钥（本地文件），拿不到私钥就冒充不了服务端
 *   - 客户端自身的授权另走应用层六位验证码（见 server.c/client.c）
 *
 * 不加密 UDP 发现：那是握手前的广播，本来就没法加密，且只暴露服务端名与目录名。
 */
#ifndef LANSYNC_TLS_H
#define LANSYNC_TLS_H

#include <stddef.h>

/* 服务端：载入证书与私钥并建好上下文；失败返回 -1，原因写进 err。 */
int ls_tls_server_init(const char *certfile, const char *keyfile,
                       char *err, size_t errlen);

/* 客户端：信任锚是编译进来的证书，不需要任何文件。 */
int ls_tls_client_init(char *err, size_t errlen);

/* 在已连接的 fd 上完成握手，成功后该 fd 的收发自动走 TLS。
 * 失败返回 -1，原因写进 err。 */
int ls_tls_accept(int fd, char *err, size_t errlen);
int ls_tls_connect(int fd, char *err, size_t errlen);

/* 结束会话并注销；对没登记的 fd 是空操作。 */
void ls_tls_close(int fd);

/* 内部：给 ls_send_all / ls_recv_all 用。fd 未登记时一律返回 -1。 */
int ls_tls_send(int fd, const void *data, size_t len);
int ls_tls_recv(int fd, void *data, size_t len);
int ls_tls_registered(int fd);

#endif /* LANSYNC_TLS_H */

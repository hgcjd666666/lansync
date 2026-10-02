#!/usr/bin/env python3
"""把 certs/lansync-server.crt 的 DER 写成 src/server_cert.h（客户端内置的信任锚）。

由 certs/gen.sh 调用；单独跑也行，前提是 .tmp/lansync-server.crt.der 已存在。
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DER = os.path.join(ROOT, ".tmp", "lansync-server.crt.der")
OUT = os.path.join(ROOT, "src", "server_cert.h")

der = open(DER, "rb").read()

lines = []
for i in range(0, len(der), 12):
    lines.append("    " + " ".join("0x%02x," % b for b in der[i:i + 12]))
body = "\n".join(lines)

hdr = f"""/*
 * server_cert.h -- 自动生成，勿手改（由 certs/gen.sh 产出）
 *
 * 服务端自签证书的 DER 字节。客户端把它当作**唯一**信任锚：
 * 只有持有 certs/lansync-server.key 对应私钥的服务端才能完成 TLS 握手，
 * 局域网里冒充服务端（ARP 欺骗等）拿不到私钥，握手直接失败。
 *
 * 同时它也定了加密：TLS 1.3，密钥交换 X25519，认证 Ed25519，
 * 对称 ChaCha20-Poly1305 —— 传输内容对中间人不可读、不可改。
 *
 * 换密钥：跑 certs/gen.sh 重新生成，然后**两端都要重编**（客户端内置的是这份字节）。
 */
#ifndef LANSYNC_SERVER_CERT_H
#define LANSYNC_SERVER_CERT_H

#define LS_SERVER_CERT_LEN {len(der)}

static const unsigned char LS_SERVER_CERT[LS_SERVER_CERT_LEN] = {{
{body}
}};

#endif /* LANSYNC_SERVER_CERT_H */
"""

open(OUT, "w").write(hdr)
print(f"证书 {len(der)} 字节 -> src/server_cert.h")

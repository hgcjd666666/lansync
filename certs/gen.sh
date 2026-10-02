#!/bin/sh
# 生成服务端密钥对与自签证书，并把证书写成 src/server_cert.h。
#
# 换密钥后**两端都要重编** —— 客户端内置的是证书字节，那是它的唯一信任锚。
set -e
cd "$(dirname "$0")/.."

mkdir -p certs .tmp

openssl genpkey -algorithm ED25519 -out certs/lansync-server.key
chmod 600 certs/lansync-server.key

# 自签：CN 无所谓，认证靠「客户端内置了这张证书」而不是名字
openssl req -x509 -key certs/lansync-server.key -out certs/lansync-server.crt \
    -days 3650 -subj "/CN=lansync-server"

openssl x509 -in certs/lansync-server.crt -outform DER \
    -out .tmp/lansync-server.crt.der

python3 tools/gen_cert_header.py

echo
echo "证书指纹："
openssl x509 -in certs/lansync-server.crt -noout -fingerprint -sha256
echo "私钥：certs/lansync-server.key（服务端用，别外传）"
echo "证书：certs/lansync-server.crt（服务端用；同一份字节已在 src/server_cert.h 里）"

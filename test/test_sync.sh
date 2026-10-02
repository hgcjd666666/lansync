#!/usr/bin/env bash
# lansync 端到端测试
#
# 覆盖：相同文件不重传、大小不同、大小相同但 MD5 不同、两端各自独有、
#       子目录、大文件多分片、空文件、含空格/中文的文件名、镜像删除模式、
#       以及恶意客户端用 ../ 做路径穿越时是否被拒绝。
#
# 用法: bash test/test_sync.sh      或      make test
set -u

BIN=${BIN:-$(cd "$(dirname "$0")/.." && pwd)/build/lansync_d}
[ -x "$BIN" ] || { echo "找不到可执行文件 $BIN，先执行 make"; exit 1; }

PORT=${PORT:-19727}
UDPPORT=${UDPPORT:-19728}
# 默认在工作区内建临时目录：/tmp 可能被沙箱或系统清理，日志留不住
TESTROOT=${TESTROOT:-$(cd "$(dirname "$0")/.." && pwd)/.tmp}
mkdir -p "$TESTROOT"
WORK=$(mktemp -d "$TESTROOT/lansync-test.XXXXXX")
SRV_PID=""
PASS=0
FAIL=0

cleanup() {
    [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null
    wait 2>/dev/null
    if [ "${KEEP:-0}" = 1 ]; then
        echo "（工作目录保留在 $WORK）"
    else
        rm -rf "$WORK"
    fi
}
trap cleanup EXIT

ok()   { PASS=$((PASS+1)); printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  \033[31mFAIL\033[0m %s\n' "$1"; }
section() { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }

RUN=0
LASTLOG=""
LASTCLIENT=""

start_server() {   # start_server <dir> [额外参数...]
    local dir=$1; shift
    RUN=$((RUN + 1))
    LASTLOG="$WORK/server-$RUN.log"
    LASTCLIENT="$WORK/client-$RUN.log"
    "$BIN" server -d "$dir" -p "$PORT" -w "$UDPPORT" -n testsrv --no-announce \
        --config "$WORK/prof.json" "$@" >"$LASTLOG" 2>&1 &
    SRV_PID=$!
    sleep 0.4
    if ! kill -0 "$SRV_PID" 2>/dev/null; then
        echo "服务端启动失败:"; cat "$LASTLOG"; exit 1
    fi
}

stop_server() {
    [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null
    wait "$SRV_PID" 2>/dev/null
    SRV_PID=""
}

run_client() {     # run_client <dir> [额外参数...]
    local dir=$1; shift
    # 加超时：任何一端卡死都应该让测试失败，而不是把整个测试挂住
    timeout 120 "$BIN" client -d "$dir" -s 127.0.0.1 -p "$PORT" -n testcli "$@" \
        >"$LASTCLIENT" 2>&1
    local rc=$?
    [ "$rc" -eq 124 ] && echo "  (客户端 120 秒超时，疑似卡死)" >&2
    return $rc
}

# 校验两棵目录树完全一致
assert_same_tree() {
    local a=$1 b=$2 label=$3
    if diff -r --exclude=".lansync-tmp-*" "$a" "$b" >"$WORK/diff.out" 2>&1; then
        ok "$label：两棵目录树一致"
    else
        bad "$label：目录树不一致"; sed 's/^/       /' "$WORK/diff.out" | head -20
    fi
}

# ---------------------------------------------------------------- 用例 1
section "用例 1：双向合并（服务端优先）"
SRV="$WORK/c1/srv"; CLI="$WORK/c1/cli"
mkdir -p "$SRV/sub" "$CLI"

echo "same-content"          > "$SRV/a.txt";  echo "same-content"          > "$CLI/a.txt"
touch -d "2021-01-01 00:00:00" "$SRV/a.txt" "$CLI/a.txt"   # 内容与时间都一致才算相同
echo "server-long-content"   > "$SRV/b.txt";  echo "cli"                   > "$CLI/b.txt"
echo "size-same-diff-md5-A"  > "$SRV/c.txt";  echo "size-same-diff-md5-B"  > "$CLI/c.txt"
# 必须显式拉开 mtime：相邻两条 echo 可能落在同一毫秒，
# 那样两边元数据完全一致，按本方案会被判成相同（见用例 8）
touch -d "2020-01-01 00:00:00" "$SRV/c.txt"
touch -d "2021-01-01 00:00:00" "$CLI/c.txt"
echo "server-only"           > "$SRV/d.txt"
echo "client-only"           > "$CLI/e.txt"
mkdir -p "$SRV/sub/deep"; echo "nested" > "$SRV/sub/deep/n.txt"
echo "with space"            > "$SRV/有 空格 的名字.txt"
echo "中文内容测试"           > "$SRV/中文文件名.txt"
: > "$SRV/empty.bin"
: > "$CLI/empty.bin"
touch -d "2021-01-01 00:00:00" "$SRV/empty.bin" "$CLI/empty.bin"
head -c 1048576 /dev/urandom > "$SRV/big.bin"     # 1 MiB，跨多个 256KiB 分片

cp "$SRV/big.bin" "$WORK/big.expected"

start_server "$SRV" --prefer server --keep-extra --allow-upload
run_client "$CLI" >/dev/null
CLIENT_RC=$?
stop_server

[ "$CLIENT_RC" -eq 0 ] && ok "客户端退出码 0" || bad "客户端退出码 $CLIENT_RC"

assert_same_tree "$SRV" "$CLI" "用例1"
cmp -s "$CLI/big.bin" "$WORK/big.expected" && ok "1 MiB 大文件确实传到客户端且内容无损" \
    || bad "大文件内容损坏"
grep -q "推送 7 / 拉取 1 / 删除 0 / 相同 2" "$LASTLOG" \
    && ok "计数正确：推送 7（b/c/d/嵌套/空格名/中文名/big.bin），拉取 1（e），相同 2（a/empty）" \
    || { bad "计数不符"; grep "同步结束" "$LASTLOG" | sed 's/^/       /'; }
grep -q "传输 1.0 MiB" "$LASTLOG" && ok "传输字节统计正确（1.0 MiB）" \
    || { bad "传输字节统计错误"; grep "同步结束" "$LASTLOG" | sed 's/^/       /'; }

# ---------------------------------------------------------------- 用例 2
section "用例 2：大小与修改时间都一致 -> 判相同、不重传、不读内容"
SRV="$WORK/c2/srv"; CLI="$WORK/c2/cli"
mkdir -p "$SRV" "$CLI"
for i in 1 2 3 4 5; do echo "content-$i" > "$SRV/f$i.txt"; done
cp -a "$SRV/." "$CLI/"     # cp -a 保留 mtime，两边元数据完全一致

start_server "$SRV"
run_client "$CLI" >/dev/null
stop_server

grep -q "推送 0 / 拉取 0 / 删除 0 / 相同 5" "$LASTLOG" \
    && ok "5 个文件全部判为相同（元数据一致，一个字节都没读）" \
    || { bad "相同文件判定错误"; grep "同步结束" "$LASTLOG" | sed 's/^/       /'; }

# ---------------------------------------------------------------- 用例 3
section "用例 3：大小相同但修改时间不同 -> 按策略决定方向"
SRV="$WORK/c3/srv"; CLI="$WORK/c3/cli"
mkdir -p "$SRV" "$CLI"
printf 'AAAA' > "$SRV/x.bin"
printf 'BBBB' > "$CLI/x.bin"     # 同样 4 字节，内容不同
touch -d "2020-01-01" "$SRV/x.bin"
touch -d "2030-01-01" "$CLI/x.bin"   # 客户端更新，newer 策略下客户端赢

start_server "$SRV" --prefer newer --allow-upload
run_client "$CLI" >/dev/null
stop_server

[ "$(cat "$SRV/x.bin")" = "BBBB" ] && ok "newer 策略：mtime 较新的客户端版本覆盖了服务端" \
    || bad "冲突解决错误，服务端内容=$(cat "$SRV/x.bin")"
grep -q "拉取 1" "$LASTLOG" && ok "大小相同、时间不同被判为有差异并同步（newer 下客户端赢）" \
    || { bad "未识别差异"; grep -E "同步结束" "$LASTLOG" | sed 's/^/       /'; }

# ---------------------------------------------------------------- 用例 4
section "用例 4：镜像删除（默认行为）"
SRV="$WORK/c4/srv"; CLI="$WORK/c4/cli"
mkdir -p "$SRV" "$CLI"
echo "keep"     > "$SRV/keep.txt"
echo "extra"    > "$CLI/extra.txt"
echo "stale"    > "$CLI/stale.txt"

start_server "$SRV"          # 默认即镜像模式
run_client "$CLI" >/dev/null
stop_server

[ ! -e "$CLI/extra.txt" ] && [ ! -e "$CLI/stale.txt" ] \
    && ok "客户端多出的文件被删除" || bad "镜像删除未生效"
[ -e "$CLI/keep.txt" ] && ok "共有文件保留" || bad "共有文件被误删"

# ---------------------------------------------------------------- 用例 5
section "用例 5：恶意客户端用 ../ 做路径穿越（服务端必须拒绝写入）"
SRV="$WORK/c5/srv"; mkdir -p "$SRV"
VICTIM="$WORK/c5/VICTIM.txt"
echo "original" > "$VICTIM"

start_server "$SRV"
python3 - "$PORT" "$WORK/c5" <<'PYEOF'
import socket, struct, sys, os
port = int(sys.argv[1]); base = sys.argv[2]

def s(x):
    b = x.encode()
    return struct.pack('>I', len(b)) + b
def frame(t, payload=b''):
    return struct.pack('>BI', t, len(payload)) + payload

sk = socket.create_connection(('127.0.0.1', port), timeout=5)
# HELLO：协议字段要与当前版本一致（proto, build_ts, mtime_ok）
sk.sendall(frame(1, s('evil') + s(base) + struct.pack('>I', 1) +
                 struct.pack('>Q', 0) + struct.pack('>B', 1)))
# 读 HELLO_ACK
hdr = sk.recv(5)
# LIST_BEGIN + 一条含路径穿越的条目 + LIST_END
entries = b''
for bad in ['../VICTIM.txt', '../../etc/lansync_pwned', '/tmp/abs_pwned']:
    entries += s(bad) + struct.pack('>Q', 5) + struct.pack('>q', 0) + b'\x00'*16
sk.sendall(frame(3, struct.pack('>I', 3)))
sk.sendall(frame(4, struct.pack('>I', 3) + entries))
sk.sendall(frame(5))
sk.settimeout(3)
try:
    while sk.recv(65536):
        pass
except Exception:
    pass
sk.close()
PYEOF

sleep 0.5
stop_server

[ "$(cat "$VICTIM")" = "original" ] && ok "上级目录文件未被改写" || bad "路径穿越成功，文件被覆盖！"
[ ! -e /tmp/abs_pwned ] && ok "绝对路径未被写入" || { bad "绝对路径穿越成功！"; rm -f /tmp/abs_pwned; }
grep -q "不安全路径" "$LASTLOG" && ok "服务端记录了拒绝行为" || bad "服务端未提示拒绝"
[ -z "$(find "$SRV" -type f 2>/dev/null)" ] && ok "同步目录内没有落任何文件" \
    || bad "同步目录出现了文件: $(find "$SRV" -type f)"

# ---------------------------------------------------------------- 用例 6
section "用例 6：默认服务端只读——客户端动不了服务端任何文件"
SRV="$WORK/c6/srv"; CLI="$WORK/c6/cli"
mkdir -p "$SRV" "$CLI"
echo "server-version" > "$SRV/conflict.txt"
echo "client-version" > "$CLI/conflict.txt"
touch -d "2020-01-01 00:00:00" "$SRV/conflict.txt"
touch -d "2030-01-01 00:00:00" "$CLI/conflict.txt"   # 客户端更新、内容不同
echo "server-only"    > "$SRV/only-s.txt"
echo "client-extra"   > "$CLI/extra-c.txt"

BEFORE=$(cd "$SRV" && find . -type f | sort | xargs md5sum 2>/dev/null | md5sum)

start_server "$SRV"          # 全默认参数
run_client "$CLI" >/dev/null
stop_server

AFTER=$(cd "$SRV" && find . -type f | sort | xargs md5sum 2>/dev/null | md5sum)

[ "$BEFORE" = "$AFTER" ] && ok "服务端目录字节级未被改动（客户端更新的同名文件也没能覆盖它）" \
    || bad "服务端目录被客户端改动了！"
[ "$(cat "$CLI/conflict.txt")" = "server-version" ] && ok "客户端被改写成服务端版本" \
    || bad "客户端内容错误: $(cat "$CLI/conflict.txt")"
[ ! -e "$CLI/extra-c.txt" ] && ok "客户端多出的文件被删除（镜像）" || bad "客户端多出的文件没被删除"
[ -e "$CLI/only-s.txt" ] && ok "服务端独有文件推送到客户端" || bad "推送失败"

# ---------------------------------------------------------------- 用例 7
section "用例 7：--keep-extra 时客户端多出的文件被忽略，服务端仍不被写"
SRV="$WORK/c7/srv"; CLI="$WORK/c7/cli"
mkdir -p "$SRV" "$CLI"
echo "s" > "$SRV/s.txt"
echo "c" > "$CLI/c.txt"

start_server "$SRV" --keep-extra
run_client "$CLI" >/dev/null
stop_server

[ -e "$CLI/c.txt" ] && ok "客户端文件保留" || bad "客户端文件被误删"
[ ! -e "$SRV/c.txt" ] && ok "服务端没有被写入客户端多出的文件" || bad "服务端被写入了！"
grep -q "忽略客户端多出的文件" "$LASTLOG" && ok "日志明确记录了忽略行为" || bad "没有忽略日志"

# ---------------------------------------------------------------- 用例 8
section "用例 8：已知边界 —— mtime 与大小都相同就判相同，不看内容"
SRV="$WORK/c8/srv"; CLI="$WORK/c8/cli"
mkdir -p "$SRV" "$CLI"
printf 'AAAA' > "$SRV/same-time.bin"
printf 'BBBB' > "$CLI/same-time.bin"
touch -d "2021-06-01 12:00:00.123456789" "$SRV/same-time.bin"
touch -d "2021-06-01 12:00:00.123456789" "$CLI/same-time.bin"   # 纳秒都一致

start_server "$SRV"
run_client "$CLI" >/dev/null
stop_server

# 大小和 mtime 都被刻意做成完全一致，方案会判它们相同 —— 这是取舍，不是 bug
[ "$(cat "$CLI/same-time.bin")" = "BBBB" ] \
    && ok "mtime 与大小都一致时判为相同（内容差异被忽略，这是 mtime 方案的已知边界）" \
    || bad "行为与文档不符: $(cat "$CLI/same-time.bin")"
grep -q "相同 1" "$LASTLOG" && ok "计数为相同，没有触发任何传输" \
    || { bad "计数不符"; grep "同步结束" "$LASTLOG" | sed 's/^/       /'; }

# ---------------------------------------------------------------- 用例 9
section "用例 9：元数据一致时完全不读内容"
SRV="$WORK/c9/srv"; CLI="$WORK/c9/cli"
mkdir -p "$SRV" "$CLI"
for i in 1 2 3 4 5 6 7 8; do head -c 200000 /dev/urandom > "$SRV/f$i.bin"; done
cp -a "$SRV/." "$CLI/"

start_server "$SRV"
run_client "$CLI" >/dev/null
stop_server

grep -q "相同 8" "$LASTLOG" && ok "8 个 200KB 文件全部走元数据快速路径判为相同" \
    || { bad "快速路径计数不符"; grep "同步结束" "$LASTLOG" | sed 's/^/       /'; }
grep -q "核对" "$LASTLOG" && bad "不该有任何内容读取" \
    || ok "全程零内容读取（判定只看元数据）"

# ---------------------------------------------------------------- 用例 11
section "用例 11：传输后修改时间必须保留（否则下次同步无法走快速路径）"
SRV="$WORK/c11/srv"; CLI="$WORK/c11/cli"
mkdir -p "$SRV" "$CLI"
echo "content" > "$SRV/stamped.txt"
touch -d "2021-03-04 05:06:07" "$SRV/stamped.txt"
WANT=$(stat -c %Y "$SRV/stamped.txt")

start_server "$SRV"
run_client "$CLI" >/dev/null
stop_server

GOT=$(stat -c %Y "$CLI/stamped.txt" 2>/dev/null || echo 0)
[ "$GOT" = "$WANT" ] && ok "修改时间原样保留（$WANT）" \
    || bad "修改时间不对：服务端 $WANT / 客户端 $GOT（1970 说明发的是秒值而非纳秒）"

# ---------------------------------------------------------------- 用例 10
section "用例 10：1 MiB 大文件的差异也能判出，且不读内容"
SRV="$WORK/c10/srv"; CLI="$WORK/c10/cli"
mkdir -p "$SRV" "$CLI"
head -c 1048576 /dev/urandom > "$SRV/big.bin"
head -c 1048576 /dev/urandom > "$CLI/big.bin"    # 同样大小、内容完全不同
touch -d "2020-01-01" "$SRV/big.bin"
touch -d "2030-01-01" "$CLI/big.bin"             # 大小相同、时间不同

start_server "$SRV"
run_client "$CLI" >/dev/null
stop_server

cmp -s "$SRV/big.bin" "$CLI/big.bin" && ok "1 MiB 同大小、时间不同的文件被判为有差异并同步" \
    || bad "大文件差异未被识别"
grep -q "核对" "$LASTLOG" && bad "不该读内容" || ok "1 MiB 文件也是零内容读取"

# ---------------------------------------------------------------- 用例 12
section "用例 12：单个文件读不了，不能拖垮其余文件"
if [ "$(id -u)" = 0 ]; then
    ok "以 root 运行，chmod 000 无效，跳过本用例"
else
    SRV="$WORK/c12/srv"; CLI="$WORK/c12/cli"
    mkdir -p "$SRV" "$CLI"
    echo "secret" > "$SRV/a_unreadable.txt"
    echo "fine"   > "$SRV/b_ok.txt"
    echo "fine2"  > "$SRV/c_ok.txt"
    chmod 000 "$SRV/a_unreadable.txt"

    start_server "$SRV" --keep-extra
    run_client "$CLI" >/dev/null
    CLIENT_RC=$?
    stop_server
    chmod 644 "$SRV/a_unreadable.txt"

    [ "$CLIENT_RC" -eq 0 ] && ok "客户端正常结束（协议没有卡死）" \
        || bad "客户端退出码 $CLIENT_RC"
    [ -e "$CLI/b_ok.txt" ] && [ -e "$CLI/c_ok.txt" ] \
        && ok "读不了的那个文件没有拖垮其余文件" || bad "其余文件也丢了"
    grep -q "打开源文件失败" "$LASTLOG" && ok "失败原因指向具体文件与 errno" \
        || { bad "失败原因不明确"; grep "发送失败" "$LASTLOG" | head -2 | sed 's/^/       /'; }
    grep -q "失败 1" "$LASTLOG" && ok "只有那一个文件计入失败" \
        || { bad "失败计数不对"; grep "同步结束" "$LASTLOG" | sed 's/^/       /'; }
fi

# ---------------------------------------------------------------- 用例 13
section "用例 13：client update 能取回服务端程序（流化后最容易漏的功能）"
SRV="$WORK/c13/srv"; mkdir -p "$SRV" "$WORK/c13/out"

start_server "$SRV"
timeout 60 "$BIN" client update -s 127.0.0.1 -p "$PORT" -o "$WORK/c13/out/got" \
    >"$WORK/c13/update.log" 2>&1
UPD_RC=$?
stop_server

[ "$UPD_RC" -eq 0 ] && ok "update 正常结束" \
    || { bad "update 退出码 $UPD_RC"; sed 's/^/       /' "$WORK/c13/update.log" | tail -5; }
cmp -s "$BIN" "$WORK/c13/out/got" && ok "取回的程序与服务端二进制逐字节一致" \
    || bad "取回的二进制不一致"
[ -x "$WORK/c13/out/got" ] && ok "可执行位保留" || bad "丢了可执行位"
grep -q "下发程序本体" "$LASTLOG" && ok "服务端记录了本次分发" || bad "服务端没有分发记录"

# ---------------------------------------------------------------- 用例 14
section "用例 14：entropy 能区分文本和随机数据（压缩判断的依据）"
mkdir -p "$WORK/c14/txt" "$WORK/c14/rnd"
for i in 1 2 3 4 5; do
    printf '这是一段反复出现的文本内容，用来把熵值压低。\n%.0s' $(seq 1 200) > "$WORK/c14/txt/f$i.txt"
done
head -c 300000 /dev/urandom > "$WORK/c14/rnd/random.bin"

"$BIN" entropy "$WORK/c14/txt" >"$WORK/c14/txt.out" 2>&1
"$BIN" entropy "$WORK/c14/rnd" >"$WORK/c14/rnd.out" 2>&1

grep -q "易压缩" "$WORK/c14/txt.out" && grep -q "5 个" "$WORK/c14/txt.out" \
    && ok "文本目录的 5 个文件全部落进「易压缩」" \
    || { bad "文本目录判定异常"; sed 's/^/       /' "$WORK/c14/txt.out"; }
grep -q "已压缩" "$WORK/c14/rnd.out" \
    && ok "随机数据落进「已压缩」（再压没意义）" \
    || { bad "随机数据判定异常"; sed 's/^/       /' "$WORK/c14/rnd.out"; }

# ---------------------------------------------------------------- 用例 15
section "用例 15：客户端保不住 mtime 时，退化成只比大小（不再反复全量重传）"
SRV="$WORK/c15/srv"; CLI="$WORK/c15/cli"
mkdir -p "$SRV" "$CLI"
printf 'AAAA' > "$SRV/same-size.bin"; printf 'BBBB' > "$CLI/same-size.bin"
printf 'new-on-server' > "$SRV/only-s.txt"
mkdir -p "$CLI/sub"; printf 'x' > "$CLI/sub/keep.txt"

start_server "$SRV" --keep-extra
LANSYNC_FORCE_NO_MTIME=1 timeout 120 "$BIN" client -d "$CLI" -s 127.0.0.1 -p "$PORT" \
    -n testcli >"$LASTCLIENT" 2>&1
stop_server

grep -q "无法保留文件修改时间" "$LASTCLIENT" && ok "客户端自检发现自己保不住 mtime 并告警" \
    || { bad "客户端没有告警"; tail -5 "$LASTCLIENT" | sed 's/^/       /'; }
grep -q "退化为「只比大小」" "$LASTLOG" && ok "服务端据此切换策略" \
    || { bad "服务端没切换策略"; grep "客户端接入" "$LASTLOG" | sed 's/^/       /'; }
[ "$(cat "$CLI/same-size.bin")" = "BBBB" ] \
    && ok "同大小不同内容被判为相同（退化模式的已知代价）" \
    || bad "不该传的却传了"
[ -e "$CLI/only-s.txt" ] && ok "服务端独有的文件照常推送（首次同步不受影响）" \
    || bad "退化后连该传的都没传"

# 再跑一次，确认收敛（不会每次全量重传）
BEFORE=$(find "$CLI" -type f | wc -l)
start_server "$SRV" --keep-extra
LANSYNC_FORCE_NO_MTIME=1 timeout 120 "$BIN" client -d "$CLI" -s 127.0.0.1 -p "$PORT" \
    -n testcli >"$LASTCLIENT" 2>&1
stop_server
grep -q "推送 0 / 拉取 0" "$LASTLOG" && ok "第二次同步不再重传任何文件（已收敛）" \
    || { bad "又重传了"; grep "同步结束" "$LASTLOG" | sed 's/^/       /'; }
[ "$(find "$CLI" -type f | wc -l)" = "$BEFORE" ] && ok "文件数没变" || bad "文件数异常"

# ---------------------------------------------------------------- 用例 16
section "用例 16：压缩传输（auto 只看熵决定压不压）"
SRV="$WORK/c16/srv"; CLI="$WORK/c16/cli"
mkdir -p "$SRV" "$CLI"
# 低熵：重复文本 + 全零；高熵：随机数据
yes "这是一段会重复非常多次的文本，用来把熵压得很低。" 2>/dev/null | head -c 800000 > "$SRV/texty.log" || true
head -c 200000 /dev/zero > "$SRV/zeros.bin"
head -c 600000 /dev/urandom > "$SRV/random.bin"

start_server "$SRV" -v
run_client "$CLI" >/dev/null
stop_server

assert_same_tree "$SRV" "$CLI" "压缩传输"
grep -q "压缩：" "$LASTLOG" && ok "低熵内容触发了压缩" \
    || { bad "没有压缩"; grep -E "推送 ->|同步结束" "$LASTLOG" | sed 's/^/       /'; }
cmp -s "$SRV/random.bin" "$CLI/random.bin" && ok "高熵文件（压不动）也完整" || bad "随机文件损坏"
cmp -s "$SRV/texty.log" "$CLI/texty.log" && ok "低熵文件解压后一致" || bad "文本文件损坏"

# ---------------------------------------------------------------- 用例 17
section "用例 17：--no-compress 时不压缩，内容依然正确"
SRV="$WORK/c17/srv"; CLI="$WORK/c17/cli"
mkdir -p "$SRV" "$CLI"
yes "重复内容重复内容重复内容重复内容" 2>/dev/null | head -c 500000 > "$SRV/a.log" || true

start_server "$SRV" -v --no-compress
run_client "$CLI" >/dev/null
stop_server

grep -q "（已压缩）" "$LASTLOG" && bad "指定不压却压了" || ok "--no-compress 生效，没有压缩"
assert_same_tree "$SRV" "$CLI" "不压缩传输"

# ---------------------------------------------------------------- 汇总
printf '\n\033[1m========== 结果：%d 通过 / %d 失败 ==========\033[0m\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || { echo; echo "服务端日志:"; sed 's/^/  /' "$LASTLOG"; }
exit $([ "$FAIL" -eq 0 ] && echo 0 || echo 1)

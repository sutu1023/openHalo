#!/usr/bin/env bash
# Build the checked-out commit, run a local database, and keep the measurements.
set -euo pipefail
if [[ $(uname -s) != Darwin ]]; then
    echo '此实验比较 macOS 的两个硬件计数接口，请在 Mac 上运行。' >&2
    exit 1
fi

repo=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
lab=$(mktemp -d /tmp/openhalo-pmc.XXXXXXXX)
results=$(mktemp -d /tmp/openhalo-results.XXXXXXXX)
echo "实验结果目录 $results"
mkdir "$lab/src" "$lab/socket"
chmod 700 "$lab/socket"
export PATH="$lab/install/bin:$PATH"
export PGHOST="$lab/socket" PGPORT=56584 PGDATABASE=pmclab LC_ALL=C
# The port also names the socket. Each run has its own private socket directory.
finish() {
    local status=$?
    trap - EXIT
    if [[ -e "$lab/db/postmaster.pid" ]]; then
        if ! pg_ctl -D "$lab/db" -m fast -w stop >>"$results/build.log" 2>&1; then
            echo "停库失败，请检查 $lab/db 和 $results/build.log。" >&2
            exit 1
        fi
    fi
    if [[ $status == 0 ]]; then
        rm -rf "$lab"
    else
        echo "实验未完成。构建目录 $lab，日志 $results" >&2
        tail -n 12 "$results/build.log" >&2
    fi
    exit "$status"
}
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
: > "$results/build.log"
git -C "$repo" archive HEAD | tar -x -C "$lab/src"
cd "$lab/src"
kit="$lab/src/doc/halo-pmc"
echo '正在编译 OpenHalo…'
{
    ./configure --enable-debug --enable-cassert --prefix="$lab/install" CFLAGS='-O0 -g'
    make -j"$(sysctl -n hw.logicalcpu)"
    make install
    initdb -k --no-locale -E UTF8 -A trust -D "$lab/db"
    pg_ctl -D "$lab/db" -l "$results/server.log" \
        -o "-k $PGHOST -p $PGPORT -c listen_addresses=" -w start
    psql -X -d template1 -v ON_ERROR_STOP=1 -c 'CREATE DATABASE pmclab'
    psql -X -d "$PGDATABASE" -v ON_ERROR_STOP=1 -f "$kit/setup.sql"
} >>"$results/build.log" 2>&1
cd "$results"
echo '正在测量建表循环…'
(cd "$kit" && psql -X -d "$PGDATABASE" -f demo.sql) >hash-demo.txt 2>&1
grep 'HaloPMC target=MultiExecPrivateHash.loop period=0 samples=1' hash-demo.txt
grep -E 'PMC cycles raw=[1-9][0-9]* boundary_reads=2 ' hash-demo.txt
grep -E 'PMC instructions raw=[1-9][0-9]* boundary_reads=2 ' hash-demo.txt
echo '正在比较四种观测模式…'
python3 "$kit/benchmark.py" >query-times.txt
grep -E '^(median_ms|paired_rounds)' query-times.txt
echo '正在测量单次读取耗时…'
cc -O2 -Wall "$kit/read_cost.c" -o "$lab/read_cost"
"$lab/read_cost" >read-cost.txt
tail -n 1 read-cost.txt
echo "查看三份结果：$results/{hash-demo,query-times,read-cost}.txt"

#!/usr/bin/env bash
set -euxo pipefail

run=$1
graph=${2:-/data/csr/friendster}
input=build/inputs/$(basename "$graph")
threads=$(nproc)
sockets=$(lscpu -p=SOCKET | grep -v '^#' | sort -u | wc -l)

export OMPI_ALLOW_RUN_AS_ROOT=1 OMPI_ALLOW_RUN_AS_ROOT_CONFIRM=1
cd /local/repository/experiments/baseline
export LD_LIBRARY_PATH=$(uv run python -c "import pyarrow; print(pyarrow.get_library_dirs()[0])")

{
    hostname -f
    git -C /local/repository rev-parse HEAD
    git -C /local/repository submodule status
    lscpu
    free -g
} > "$run/settings" 2>&1

for system in ligra galois grape; do
    uv run $system.py build cc
done

uv run convert.py ligra "$graph" "$input"
uv run ligra.py run cc "$input" > "$run/ligra.log" 2>&1
rm "$input.config" "$input.adj" "$input.idx"

uv run convert.py galois "$graph" "$input"
uv run galois.py run cc "$input.sgr" -n 1 -t "$threads" > "$run/galois-n1.log" 2>&1
uv run galois.py run cc "$input.sgr" -n "$sockets" -t $((threads / sockets)) > "$run/galois-n$sockets.log" 2>&1
rm "$input.sgr"

uv run convert.py grape "$graph" "$input" -t "$threads"
uv run grape.py run cc "$input" -n 1 -t "$threads" > "$run/grape-n1.log" 2>&1
uv run grape.py run cc "$input" -n "$sockets" -t $((threads / sockets)) > "$run/grape-n$sockets.log" 2>&1
rm "$input.v" "$input.e"

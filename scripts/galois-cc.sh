#!/usr/bin/env bash
set -euxo pipefail

run=$1
graph=${2:-/data/csr/friendster}
input=build/inputs/$(basename "$graph")
threads=$(nproc)
ranks=2

export OMPI_ALLOW_RUN_AS_ROOT=1 OMPI_ALLOW_RUN_AS_ROOT_CONFIRM=1
cd /local/repository/experiments/baseline
export LD_LIBRARY_PATH=$(uv run python -c "import pyarrow; print(pyarrow.get_library_dirs()[0])")

{
    hostname -f
    git -C /local/repository rev-parse HEAD
    git -C /local/repository submodule status
    lscpu
    free -g
    df -h /local/repository
} > "$run/settings" 2>&1

uv run galois.py build cc
uv run galois.py build cc_shm

uv run convert.py galois "$graph" "$input"
uv run galois.py run-shm cc_shm "$input.sgr" Afforest -t "$threads" > "$run/shm-afforest.log" 2>&1
uv run galois.py run-shm cc_shm "$input.sgr" LabelProp -t "$threads" > "$run/shm-labelprop.log" 2>&1
uv run galois.py run cc "$input.sgr" -n 1 -t "$threads" > "$run/galois-n1.log" 2>&1
uv run galois.py run cc "$input.sgr" -n "$ranks" -t $((threads / ranks)) > "$run/galois-n$ranks.log" 2>&1
rm "$input.sgr"

#!/usr/bin/env bash
set -euo pipefail

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y \
    build-essential cmake git curl ca-certificates pkg-config \
    libboost-all-dev llvm-dev libfmt-dev libnuma-dev \
    openmpi-bin libopenmpi-dev \
    libucx-dev ucx-utils rdma-core ibverbs-utils perftest \
    libgflags-dev libgoogle-glog-dev \
    numactl time tmux htop
apt-get clean

curl -LsSf https://astral.sh/uv/install.sh | env UV_INSTALL_DIR=/usr/local/bin sh

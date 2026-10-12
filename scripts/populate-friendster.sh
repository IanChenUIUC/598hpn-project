#!/usr/bin/env bash
set -euo pipefail

url=https://snap.stanford.edu/data/bigdata/communities/com-friendster.ungraph.txt.gz
dest=/data/raw/com-friendster.ungraph.txt.gz

mountpoint -q /data
[[ -e $dest.ok ]] && exit 0

mkdir -p /data/raw
curl -fL -C - -o "$dest" "$url"
touch "$dest.ok"

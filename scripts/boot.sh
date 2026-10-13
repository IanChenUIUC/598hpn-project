#!/usr/bin/env bash
set -euo pipefail

task=$1
run=/results/$(date +%Y%m%dT%H%M%S)-$task

mountpoint -q /results
mkdir -p "$run"
/local/repository/scripts/$task.sh "$run" > "$run/log" 2>&1

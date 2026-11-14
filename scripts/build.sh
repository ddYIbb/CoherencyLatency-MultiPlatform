#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
PROJECT_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"

cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=arm64

JOBS="$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)"
cmake --build "$BUILD_DIR" --config Release -j"$JOBS"

echo
echo "Built: $BUILD_DIR/coherency_latency"
file "$BUILD_DIR/coherency_latency"

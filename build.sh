#!/usr/bin/env sh
# Build nxzilla on Linux (or any POSIX system).
set -e
cd "$(dirname "$0")"
g++ -std=c++17 -O2 -Wall -Wextra -o nxzilla nxzilla.cpp
echo "[build] Built $(pwd)/nxzilla"

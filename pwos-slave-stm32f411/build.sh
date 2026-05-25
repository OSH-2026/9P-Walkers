#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

cmake --preset Debug
cmake --build --preset Debug

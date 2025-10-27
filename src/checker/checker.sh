#!/usr/bin/env bash
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR" || exit 1
gcc -std=c11 -O2 -Wall checker.c -o checker -pthread -lhiredis
./checker 6
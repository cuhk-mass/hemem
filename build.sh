#!/bin/bash
set -x

SCRIPTDIR="$(dirname "$(readlink -f "$0")")"
PREFIX="$HOME"/.local

export PKG_CONFIG_PATH="$PREFIX"/lib64/pkgconfig C_INCLUDE_PATH="$PREFIX"/include LIBRARY_PATH="$PREFIX"/lib64

# capstone
cd "$SCRIPTDIR"/capstone
# rm -rf build
cmake -S . -B build -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_BUILD_TYPE=Release && cmake --build build --config Release --parallel --target=install

# syscall_intercept
cd "$SCRIPTDIR"/syscall_intercept
# rm -rf build
cmake -S . -B build -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_BUILD_TYPE=Release && cmake --build build --config Release --parallel --target=install

# hoard
cd "$SCRIPTDIR"/Hoard/src
make clean && make -j
cp libhoard.so "$PREFIX"/lib64

# hemem
cd "$SCRIPTDIR"/src
make clean && make -j
cp libhemem.so "$PREFIX"/lib64


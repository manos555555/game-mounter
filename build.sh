#!/bin/bash
# Build script for Game Mounter
# By Manos

set -e

# SDK path (Linux)
SDK_PATH="/opt/ps5-payload-sdk"

if [ ! -d "$SDK_PATH" ]; then
    echo "Error: PS5 Payload SDK not found at: $SDK_PATH"
    echo "Please install PS5 Payload SDK first"
    exit 1
fi

echo "========================================="
echo "  Game Mounter - Build Script"
echo "  By Manos"
echo "========================================="
echo "SDK Path: $SDK_PATH"
echo ""

# Compile using prospero-clang++
echo "[+] Compiling with prospero-clang++..."

"$SDK_PATH/bin/prospero-clang++" \
    -Wall -Werror \
    -I"$SDK_PATH/target/include_bsd" \
    -I"$SDK_PATH/target/include" \
    -L"$SDK_PATH/target/lib" \
    -lSceSystemService \
    -lSceUserService \
    -lSceAppInstUtil \
    -o game_mounter.elf \
    main.cpp

if [ -f game_mounter.elf ]; then
    echo ""
    echo "========================================="
    echo "[+] SUCCESS! Compiled game_mounter.elf"
    echo "========================================="
    ls -lh game_mounter.elf
    file game_mounter.elf
    echo ""
    echo "Deploy to PS5: /data/etaHEN/payloads/game_mounter.elf"
else
    echo "ERROR: Compilation failed!"
    exit 1
fi

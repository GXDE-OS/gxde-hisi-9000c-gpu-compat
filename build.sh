#!/bin/bash
# ============================================================
# gxde-hisi-9000c-gpu-compat 构建脚本
# 用法: ./build.sh          → 在上级目录生成 gxde-hisi-9000c-gpu-compat_*.deb
# 依赖: dpkg-buildpackage, debhelper, gcc
# ============================================================
set -e
cd "$(dirname "$0")"

echo "==> 构建 gxde-hisi-9000c-gpu-compat ..."
# -b 仅二进制; -us -uc 不签名; -d 跳过 build-dep 检查
dpkg-buildpackage -b -us -uc -d 2>&1 | tail -25

echo
echo "==> 产物:"
ls -lh ../gxde-hisi-9000c-gpu-compat_*.deb

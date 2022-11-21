#!/bin/bash
# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: Apache-2.0

set -eo pipefail

mkdir -p build && cd build
cmake \
  -DFWE_STATIC_LINK=On \
  -DFWE_SECURITY_COMPILE_FLAGS=On \
  -DCMAKE_TOOLCHAIN_FILE=/usr/local/aarch64-linux-gnu/lib/cmake/arm64-toolchain.cmake \
  -DBUILD_TESTING=Off \
  -DFWE_IOT_SDK_EXTRA_LIBS="/usr/local/aarch64-linux-gnu/lib/libcurl.a /usr/lib/aarch64-linux-gnu/libssl.a /usr/lib/aarch64-linux-gnu/libcrypto.a /usr/lib/aarch64-linux-gnu/libz.a" \
  ..
make -j`nproc`

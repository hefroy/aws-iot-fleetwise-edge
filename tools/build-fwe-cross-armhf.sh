#!/bin/bash
# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: Apache-2.0

set -eo pipefail

mkdir -p build && cd build
cmake \
  -DFWE_STATIC_LINK=On \
  -DFWE_SECURITY_COMPILE_FLAGS=On \
  -DCMAKE_TOOLCHAIN_FILE=/usr/local/arm-linux-gnueabihf/lib/cmake/armhf-toolchain.cmake \
  -DBUILD_TESTING=Off \
  -DFWE_IOT_SDK_EXTRA_LIBS="/usr/local/arm-linux-gnueabihf/lib/libcurl.a /usr/lib/arm-linux-gnueabihf/libssl.a /usr/lib/arm-linux-gnueabihf/libcrypto.a /usr/lib/arm-linux-gnueabihf/libz.a" \
  ..
make -j`nproc`

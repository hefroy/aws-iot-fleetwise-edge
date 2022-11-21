#!/bin/bash
# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: Apache-2.0

set -eo pipefail

mkdir -p build && cd build
cmake \
  -DFWE_STATIC_LINK=On \
  -DFWE_SECURITY_COMPILE_FLAGS=On \
  -DFWE_IOT_SDK_EXTRA_LIBS="/usr/local/lib/libcurl.a /usr/lib/$(gcc -dumpmachine)/libssl.a /usr/lib/$(gcc -dumpmachine)/libcrypto.a /usr/lib/$(gcc -dumpmachine)/libz.a" \
  ..
make -j`nproc`

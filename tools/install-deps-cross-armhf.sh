#!/bin/bash
# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

WITH_CAMERA_SUPPORT="false"

parse_args() {
    while [ "$#" -gt 0 ]; do
        case $1 in
        --with-camera-support)
            WITH_CAMERA_SUPPORT="true"
            ;;
        --help)
            echo "Usage: $0 [OPTION]"
            echo "  --with-camera-support  Installs dependencies required for camera support"
            exit 0
            ;;
        esac
        shift
    done
}

parse_args "$@"

ARCH=`dpkg --print-architecture`
if [ "${ARCH}" == "armhf" ]; then
    echo "Error: Architecture is already ${ARCH}, use install-deps-native.sh" >&2
    exit -1
fi

cp tools/armhf.list /etc/apt/sources.list.d/
mkdir -p /usr/local/arm-linux-gnueabihf/lib/cmake/
cp tools/armhf-toolchain.cmake /usr/local/arm-linux-gnueabihf/lib/cmake/

dpkg --add-architecture armhf
sed -i "s/deb http/deb [arch=${ARCH}] http/g" /etc/apt/sources.list
apt update
apt install -y \
    libssl-dev:armhf \
    libboost-system-dev:armhf \
    libboost-log-dev:armhf \
    libboost-thread-dev:armhf \
    build-essential \
    crossbuild-essential-armhf \
    cmake \
    unzip \
    git \
    wget \
    curl \
    zlib1g-dev:armhf \
    libsnappy-dev:armhf

mkdir -p deps-cross-armhf && cd deps-cross-armhf

if [ ! -d jsoncpp ]; then
    git clone -b 1.7.4 https://github.com/open-source-parsers/jsoncpp.git
    cd jsoncpp
    mkdir build && cd build
    cmake \
        -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_POSITION_INDEPENDENT_CODE=On \
        -DJSONCPP_WITH_TESTS=Off \
        -DJSONCPP_WITH_POST_BUILD_UNITTEST=Off \
        -DCMAKE_TOOLCHAIN_FILE=/usr/local/arm-linux-gnueabihf/lib/cmake/armhf-toolchain.cmake \
        -DCMAKE_INSTALL_PREFIX=/usr/local/arm-linux-gnueabihf \
        ..
    cd ../..
fi
make install -j`nproc` -C jsoncpp/build

if [ ! -d protobuf-21.7 ]; then
    wget -q https://github.com/protocolbuffers/protobuf/releases/download/v21.7/protobuf-all-21.7.tar.gz
    tar -zxf protobuf-all-21.7.tar.gz
    cd protobuf-21.7
    mkdir build && cd build
    ../configure
    cd ..
    mkdir build_armhf && cd build_armhf
    CC=arm-linux-gnueabihf-gcc CXX=arm-linux-gnueabihf-g++ \
        ../configure --host=arm-linux --prefix=/usr/local/arm-linux-gnueabihf
    cd ../..
fi
make install -j`nproc` -C protobuf-21.7/build
make install -j`nproc` -C protobuf-21.7/build_armhf
ldconfig

if [ ! -d can-isotp ]; then
    git clone https://github.com/hartkopp/can-isotp.git
    cd can-isotp
    git checkout beb4650660179963a8ed5b5cbf2085cc1b34f608
    cd ..
fi
cp can-isotp/include/uapi/linux/can/isotp.h /usr/include/linux/can

if [ ! -d curl ]; then
    git clone -b curl-7_86_0 https://github.com/curl/curl.git
    cd curl
    mkdir build && cd build
    cmake \
        -DBUILD_SHARED_LIBS=Off \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING=Off \
        -DBUILD_CURL_EXE=Off \
        -DZLIB_LIBRARY=/usr/lib/arm-linux-gnueabihf/libz.a \
        -DOPENSSL_SSL_LIBRARY=/usr/lib/arm-linux-gnueabihf/libssl.a \
        -DOPENSSL_CRYPTO_LIBRARY=/usr/lib/arm-linux-gnueabihf/libcrypto.a \
        -DCMAKE_TOOLCHAIN_FILE=/usr/local/arm-linux-gnueabihf/lib/cmake/armhf-toolchain.cmake \
        -DCMAKE_INSTALL_PREFIX=/usr/local/arm-linux-gnueabihf \
        ..
    cd ../..
fi
make install -j`nproc` -C curl/build
# The generated files are buggy, so remove them to use CMake's own FindCURL method:
rm -rf /usr/local/arm-linux-gnueabihf/lib/cmake/CURL

if [ ! -d aws-sdk-cpp ]; then
    git clone -b 1.9.253 --recursive https://github.com/aws/aws-sdk-cpp.git
    cd aws-sdk-cpp
    mkdir build && cd build
    cmake \
        -DENABLE_TESTING=OFF \
        -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_ONLY='s3-crt;iot' \
        -DAWS_CUSTOM_MEMORY_MANAGEMENT=ON \
        -DZLIB_LIBRARY=/usr/lib/arm-linux-gnueabihf/libz.a \
        -DCURL_LIBRARY=/usr/local/arm-linux-gnueabihf/lib/libcurl.a \
        -DCMAKE_TOOLCHAIN_FILE=/usr/local/arm-linux-gnueabihf/lib/cmake/armhf-toolchain.cmake \
        -DCMAKE_INSTALL_PREFIX=/usr/local/arm-linux-gnueabihf \
        ..
    cd ../..
fi
make install -j`nproc` -C aws-sdk-cpp/build

if [ ! -d googletest ]; then
    git clone -b release-1.10.0 https://github.com/google/googletest.git
    cd googletest
    mkdir build && cd build
    cmake \
        -DCMAKE_TOOLCHAIN_FILE=/usr/local/arm-linux-gnueabihf/lib/cmake/armhf-toolchain.cmake \
        -DCMAKE_INSTALL_PREFIX=/usr/local/arm-linux-gnueabihf \
        ..
    cd ../.. 
fi
make install -j`nproc` -C googletest/build

if [ ! -d benchmark ]; then
    git clone -b v1.6.1 https://github.com/google/benchmark.git
    cd benchmark
    mkdir build && cd build
    cmake \
        -DCMAKE_TOOLCHAIN_FILE=/usr/local/arm-linux-gnueabihf/lib/cmake/armhf-toolchain.cmake \
        -DCMAKE_INSTALL_PREFIX=/usr/local/arm-linux-gnueabihf \
        -DBENCHMARK_DOWNLOAD_DEPENDENCIES=on \
        -DCMAKE_BUILD_TYPE=Release \
        ..
    cd ../.. 
fi
make install -j`nproc` -C benchmark/build

sudo ldconfig

# AWS IoT FleetWise Edge camera support requires Fast-DDS and its dependencies:
if [ "${WITH_CAMERA_SUPPORT}" == "true" ]; then
    apt install -y \
        default-jre \
        libasio-dev \
        qemu-user-binfmt

    if [ ! -d tinyxml2 ]; then
        git clone -b 6.0.0 https://github.com/leethomason/tinyxml2.git
        cd tinyxml2
        mkdir build && cd build
        cmake \
            -DBUILD_SHARED_LIBS=OFF \
            -DBUILD_STATIC_LIBS=ON \
            -DBUILD_TESTS=OFF \
            -DCMAKE_POSITION_INDEPENDENT_CODE=On \
            -DCMAKE_TOOLCHAIN_FILE=/usr/local/arm-linux-gnueabihf/lib/cmake/armhf-toolchain.cmake \
            -DCMAKE_INSTALL_PREFIX=/usr/local/arm-linux-gnueabihf \
            ..
        cd ../..
    fi
    make install -j`nproc` -C tinyxml2/build

    if [ ! -d foonathan_memory_vendor ]; then
        git clone -b v1.1.0 https://github.com/eProsima/foonathan_memory_vendor.git
        cd foonathan_memory_vendor
        mkdir build && cd build
        cmake \
            -DBUILD_SHARED_LIBS=OFF \
            -Dextra_cmake_args="-DCMAKE_CROSSCOMPILING_EMULATOR=qemu-arm" \
            -DCMAKE_TOOLCHAIN_FILE=/usr/local/arm-linux-gnueabihf/lib/cmake/armhf-toolchain.cmake \
            -DCMAKE_INSTALL_PREFIX=/usr/local/arm-linux-gnueabihf \
            ..
        cd ../..
    fi
    make install -j`nproc` -C foonathan_memory_vendor/build

    if [ ! -d Fast-CDR ]; then
        git clone -b v1.0.21 https://github.com/eProsima/Fast-CDR.git
        cd Fast-CDR
        mkdir build && cd build
        cmake \
            -DBUILD_SHARED_LIBS=OFF \
            -DCMAKE_TOOLCHAIN_FILE=/usr/local/arm-linux-gnueabihf/lib/cmake/armhf-toolchain.cmake \
            -DCMAKE_INSTALL_PREFIX=/usr/local/arm-linux-gnueabihf \
            ..
        cd ../..
    fi
    make install -j`nproc` -C Fast-CDR/build

    if [ ! -d Fast-DDS ]; then
        git clone -b v2.3.4 https://github.com/eProsima/Fast-DDS.git
        cd Fast-DDS
        mkdir build && cd build
        cmake \
            -DBUILD_SHARED_LIBS=OFF \
            -DCOMPILE_TOOLS=OFF \
            -DCMAKE_CXX_FLAGS="-DUSE_FOONATHAN_NODE_SIZES=1" \
            -DCMAKE_TOOLCHAIN_FILE=/usr/local/arm-linux-gnueabihf/lib/cmake/armhf-toolchain.cmake \
            -DCMAKE_INSTALL_PREFIX=/usr/local/arm-linux-gnueabihf \
            ..
        cd ../..
    fi
    make install -j`nproc` -C Fast-DDS/build

    if [ ! -d Fast-DDS-Gen ]; then
        git clone -b v2.0.1 --recursive https://github.com/eProsima/Fast-DDS-Gen.git
        cd Fast-DDS-Gen
        ./gradlew assemble
        cd ..
    fi
    mkdir -p /usr/local/share/fastddsgen/java
    cp Fast-DDS-Gen/share/fastddsgen/java/fastddsgen.jar /usr/local/share/fastddsgen/java
    cp Fast-DDS-Gen/scripts/fastddsgen /usr/local/bin
fi

# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: Apache-2.0

FROM public.ecr.aws/ubuntu/ubuntu:18.04

RUN apt-get update \
    && apt-get install --no-install-recommends -y \
        ca-certificates \
        iproute2 \
        can-utils \
        jq \
        sed \
    && apt-get clean

ADD build/src/executionmanagement/aws-iot-fleetwise-edge /usr/bin/
ADD tools/container/start-fwe.sh                         /usr/bin
ADD tools/configure-fwe.sh                               /usr/bin/
ADD LICENSE                                              /usr/share/doc/aws-iot-fleetwise-edge/
ADD THIRD-PARTY-LICENSES                                 /usr/share/doc/aws-iot-fleetwise-edge/
ADD configuration/static-config.json                     /etc/aws-iot-fleetwise/

ENTRYPOINT /usr/bin/start-fwe.sh

#!/bin/bash
# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

INPUT_CONFIG_FILE=""
OUTPUT_CONFIG_FILE=""
CERT_FILE="/etc/aws-iot-fleetwise/certificate.pem"
KEY_FILE="/etc/aws-iot-fleetwise/private-key.key"
VEHICLE_NAME=""
ENDPOINT_URL=""
CAN_BUS0="vcan0"
LOG_LEVEL="Info"
PERSISTENCY_PATH="/var/aws-iot-fleetwise/"
TOPIC_PREFIX="\$aws/iotfleetwise/"

parse_args() {
    while [ "$#" -gt 0 ]; do
        case $1 in
        --input-config-file)
            INPUT_CONFIG_FILE=$2
            ;;
        --output-config-file)
            OUTPUT_CONFIG_FILE=$2
            ;;
        --vehicle-name)
            VEHICLE_NAME=$2
            ;;
        --endpoint-url)
            ENDPOINT_URL=$2
            ;;
        --certificate-file)
            CERT_FILE=$2
            ;;
        --private-key-file)
            KEY_FILE=$2
            ;;
        --can-bus0)
            CAN_BUS0=$2
            ;;
        --persistency-path)
            PERSISTENCY_PATH=$2
            ;;
        --topic-prefix)
            TOPIC_PREFIX=$2
            ;;
        --log-level)
            LOG_LEVEL=$2
            ;;
        --help)
            echo "Usage: $0 [OPTION]"
            echo "  --input-config-file <FILE>   Input JSON config file"
            echo "  --output-config-file <FILE>  Output JSON config file"
            echo "  --vehicle-name <NAME>        Vehicle name"
            echo "  --endpoint-url <URL>         IoT Core MQTT endpoint URL"
            echo "  --can-bus0 <BUS>             CAN bus 0, default: ${CAN_BUS0}"
            echo "  --certificate-file <FILE>    Certificate file, default: ${CERT_FILE}"
            echo "  --private-key-file <FILE>    Private key file, default: ${KEY_FILE}"
            echo "  --persistency-path <PATH>    Persistency path, default: ${PERSISTENCY_PATH}"
            echo "  --topic-prefix <PREFIX>      IoT MQTT topic prefix, default: ${TOPIC_PREFIX}"
            echo "  --log-level <LEVEL>          Log level. Either: Off, Error, Warning, Info, Trace. Default: ${LOG_LEVEL}"
            exit 0
            ;;
        esac
        shift
    done

    if [ "${INPUT_CONFIG_FILE}" == "" ]; then
        echo "Error: No input config file specified with --input-config-file <FILE>"
        exit -1
    fi
    if [ "${OUTPUT_CONFIG_FILE}" == "" ]; then
        echo "Error: No output config file specified with --output-config-file <FILE>"
        exit -1
    fi
    if [ "${VEHICLE_NAME}" == "" ]; then
        echo "Error: No Vehicle name specified with --vehicle-name <NAME>"
        exit -1
    fi
    if [ "${ENDPOINT_URL}" == "" ]; then
        echo "Error: No endpoint URL specified with --endpoint-url <URL>"
        exit -1
    fi
}

parse_args "$@"

if [ "`jq '.networkInterfaces[0].canInterface' ${INPUT_CONFIG_FILE}`" == "null" ] \
    || [ "`jq '.networkInterfaces[1].obdInterface' ${INPUT_CONFIG_FILE}`" == "null" ]; then
    echo "Error: Unexpected format in input config file"
    exit -1
fi

# Create the config file:
jq ".staticConfig.mqttConnection.endpointUrl=\"${ENDPOINT_URL}\"" ${INPUT_CONFIG_FILE} \
    | jq ".staticConfig.mqttConnection.clientId=\"${VEHICLE_NAME}\"" \
    | jq ".staticConfig.mqttConnection.collectionSchemeListTopic=\"${TOPIC_PREFIX}vehicles/${VEHICLE_NAME}/collection_schemes\"" \
    | jq ".staticConfig.mqttConnection.decoderManifestTopic=\"${TOPIC_PREFIX}vehicles/${VEHICLE_NAME}/decoder_manifests\"" \
    | jq ".staticConfig.mqttConnection.canDataTopic=\"${TOPIC_PREFIX}vehicles/${VEHICLE_NAME}/signals\"" \
    | jq ".staticConfig.mqttConnection.checkinTopic=\"${TOPIC_PREFIX}vehicles/${VEHICLE_NAME}/checkins\"" \
    | jq ".staticConfig.mqttConnection.certificateFilename=\"${CERT_FILE}\"" \
    | jq ".staticConfig.mqttConnection.privateKeyFilename=\"${KEY_FILE}\"" \
    | jq ".staticConfig.internalParameters.systemWideLogLevel=\"${LOG_LEVEL}\"" \
    | jq ".staticConfig.persistency.persistencyPath=\"${PERSISTENCY_PATH}\"" \
    | jq ".networkInterfaces[0].canInterface.interfaceName=\"${CAN_BUS0}\"" \
    | jq ".networkInterfaces[1].obdInterface.interfaceName=\"${CAN_BUS0}\"" \
    | jq ".networkInterfaces[1].obdInterface.pidRequestIntervalSeconds=5" \
    | jq ".networkInterfaces[1].obdInterface.dtcRequestIntervalSeconds=5" \
    | jq ".networkInterfaces[1].interfaceId=\"0\"" \
    | jq ".staticConfig.publishToCloudParameters.collectionSchemeManagementCheckinIntervalMs=5000" \
    > ${OUTPUT_CONFIG_FILE}

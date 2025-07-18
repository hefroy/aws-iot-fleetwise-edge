// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "aws/iotfleetwise/Schema.h"
#include "MqttClientWrapperMock.h"
#include "SenderMock.h"
#include "aws/iotfleetwise/AwsIotReceiver.h"
#include "aws/iotfleetwise/Clock.h"
#include "aws/iotfleetwise/ClockHandler.h"
#include "aws/iotfleetwise/CollectionSchemeIngestion.h"
#include "aws/iotfleetwise/CollectionSchemeIngestionList.h"
#include "aws/iotfleetwise/EnumUtility.h"
#include "aws/iotfleetwise/ICollectionScheme.h"
#include "aws/iotfleetwise/ICollectionSchemeList.h"
#include "aws/iotfleetwise/IConnectionTypes.h"
#include "aws/iotfleetwise/IDecoderManifest.h"
#include "aws/iotfleetwise/MessageTypes.h"
#include "aws/iotfleetwise/SignalTypes.h"
#include "aws/iotfleetwise/TimeTypes.h"
#include "aws/iotfleetwise/VehicleDataSourceTypes.h"
#include "checkin.pb.h"
#include "collection_schemes.pb.h"
#include "common_types.pb.h"
#include "decoder_manifest.pb.h"
#include <algorithm>
#include <aws/crt/Types.h>
#include <aws/crt/mqtt/Mqtt5Client.h>
#include <aws/crt/mqtt/Mqtt5Packets.h>
#include <cstdint>
#include <functional>
#include <gmock/gmock.h>
#include <google/protobuf/message.h>
#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#ifdef FWE_FEATURE_VISION_SYSTEM_DATA
#include <boost/variant.hpp>
#endif

namespace Aws
{
namespace IoTFleetWise
{

using ::testing::_;
using ::testing::Gt;
using ::testing::InvokeArgument;
using ::testing::MockFunction;
using ::testing::Return;
using ::testing::Sequence;
using ::testing::StrictMock;

static void
assertCheckin( const std::string &data, const std::vector<SyncID> &sampleDocList, Timestamp timeBeforeCheckin )
{
    std::shared_ptr<const Clock> clock = ClockHandler::getClock();

    // Create a multiset of ARNS documents we have in a checkin to compare against was was put in the checkin
    std::multiset<SyncID> documentSet( sampleDocList.begin(), sampleDocList.end() );

    // Deserialize the protobuf
    Schemas::CheckinMsg::Checkin sentCheckin;
    ASSERT_TRUE( sentCheckin.ParseFromString( data ) );

    // Make sure the size of the documents is the same
    ASSERT_EQ( sentCheckin.document_sync_ids_size(), sampleDocList.size() );

    // Iterate over all the documents found in the checkin
    for ( int i = 0; i < sentCheckin.document_sync_ids_size(); i++ )
    {
        ASSERT_GE( documentSet.count( sentCheckin.document_sync_ids( i ) ), 1 );
        // Erase the entry from the multiset
        documentSet.erase( documentSet.find( sentCheckin.document_sync_ids( i ) ) );
    }

    // Make sure we have erased all the elements from our set
    ASSERT_EQ( documentSet.size(), 0 );

    // Make sure the checkin time is after the time we took at the start of the test
    ASSERT_GE( sentCheckin.timestamp_ms_epoch(), timeBeforeCheckin );
    // Make sure the checkin time is before or equal to this time
    ASSERT_LE( sentCheckin.timestamp_ms_epoch(), clock->systemTimeSinceEpochMs() );
}

class SchemaTest : public ::testing::Test
{
protected:
    SchemaTest()
        : mReceiverDecoderManifest( mMqttClientWrapper, "topic" )
        , mReceiverCollectionSchemeList( mMqttClientWrapper, "topic" )
    {
    }

    void
    SetUp() override
    {
        mCollectionSchemeIngestion =
            std::make_unique<Schema>( mReceiverDecoderManifest, mReceiverCollectionSchemeList, mSender );

        mCollectionSchemeIngestion->subscribeToDecoderManifestUpdate(
            [&]( std::shared_ptr<IDecoderManifest> decoderManifest ) {
                mReceivedDecoderManifest = decoderManifest;
            } );
        mCollectionSchemeIngestion->subscribeToCollectionSchemeUpdate(
            [&]( std::shared_ptr<ICollectionSchemeList> collectionSchemeList ) {
                mReceivedCollectionSchemeList = collectionSchemeList;
            } );
    }

    static void
    sendMessageToReceiver( AwsIotReceiver &receiver, google::protobuf::MessageLite &protoMsg )
    {
        std::string protoSerializedBuffer;
        ASSERT_TRUE( protoMsg.SerializeToString( &protoSerializedBuffer ) );

        auto publishPacket = std::make_shared<Aws::Crt::Mqtt5::PublishPacket>();
        Aws::Crt::Mqtt5::PublishReceivedEventData eventData;
        eventData.publishPacket = publishPacket;
        publishPacket->WithPayload( Aws::Crt::ByteCursorFromArray(
            reinterpret_cast<const uint8_t *>( protoSerializedBuffer.data() ), protoSerializedBuffer.length() ) );

        receiver.onDataReceived( eventData );
    }

    std::string mCheckinTopic = "$aws/iotfleetwise/vehicles/thing-name/checkins";
    StrictMock<MqttClientWrapperMock> mMqttClientWrapper;
    AwsIotReceiver mReceiverDecoderManifest;
    AwsIotReceiver mReceiverCollectionSchemeList;
    StrictMock<Testing::SenderMock> mSender;
    std::unique_ptr<Schema> mCollectionSchemeIngestion;

    std::shared_ptr<IDecoderManifest> mReceivedDecoderManifest;
    std::shared_ptr<ICollectionSchemeList> mReceivedCollectionSchemeList;
};

TEST_F( SchemaTest, Checkins )
{
    Schema collectionSchemeIngestion( mReceiverDecoderManifest, mReceiverCollectionSchemeList, mSender );

    std::shared_ptr<const Clock> clock = ClockHandler::getClock();
    Timestamp timeBeforeCheckin = clock->systemTimeSinceEpochMs();

    // Create list of Arns
    std::vector<SyncID> sampleDocList;

    Sequence seq;
    EXPECT_CALL( mSender, mockedSendBuffer( mCheckinTopic, _, Gt( 0 ), _ ) )
        .Times( 3 )
        .InSequence( seq )
        .WillRepeatedly( InvokeArgument<3>( ConnectivityError::Success ) );
    EXPECT_CALL( mSender, mockedSendBuffer( mCheckinTopic, _, Gt( 0 ), _ ) )
        .InSequence( seq )
        .WillOnce( InvokeArgument<3>( ConnectivityError::NoConnection ) );

    MockFunction<void( bool success )> resultCallback;

    // Test an empty checkin
    EXPECT_CALL( resultCallback, Call( true ) ).Times( 1 );
    collectionSchemeIngestion.sendCheckin( sampleDocList, resultCallback.AsStdFunction() );
    ASSERT_EQ( mSender.getSentBufferDataByTopic( mCheckinTopic ).size(), 1 );
    ASSERT_NO_FATAL_FAILURE(
        assertCheckin( mSender.getSentBufferDataByTopic( mCheckinTopic )[0].data, sampleDocList, timeBeforeCheckin ) );

    // Add some doc arns
    sampleDocList.emplace_back( "DocArn1" );
    sampleDocList.emplace_back( "DocArn2" );
    sampleDocList.emplace_back( "DocArn3" );
    sampleDocList.emplace_back( "DocArn4" );

    // Test the previous doc list
    EXPECT_CALL( resultCallback, Call( true ) ).Times( 1 );
    collectionSchemeIngestion.sendCheckin( sampleDocList, resultCallback.AsStdFunction() );

    // Test with duplicates - this shouldn't occur but make sure it works anyways
    sampleDocList.emplace_back( "DocArn4" );
    EXPECT_CALL( resultCallback, Call( true ) ).Times( 1 );
    collectionSchemeIngestion.sendCheckin( sampleDocList, resultCallback.AsStdFunction() );

    // Second call should simulate a offboardconnectivity issue, the checkin message should fail to send.
    EXPECT_CALL( resultCallback, Call( false ) ).Times( 1 );
    collectionSchemeIngestion.sendCheckin( sampleDocList, resultCallback.AsStdFunction() );
    ASSERT_EQ( mSender.getSentBufferDataByTopic( mCheckinTopic ).size(), 4 );
    ASSERT_NO_FATAL_FAILURE(
        assertCheckin( mSender.getSentBufferDataByTopic( mCheckinTopic )[3].data, sampleDocList, timeBeforeCheckin ) );
}

/**
 * @brief This test writes a DecoderManifest object to a protobuf binary array. Then it uses this binary array to build
 * a DecoderManifestIngestion object. All the functions of that object are tested against the original proto.
 */
TEST_F( SchemaTest, DecoderManifestIngestion )
{
#define NODE_A 123
#define NODE_B 4892
#define CAN_SOURCE_FOR_NODE_A 0
#define CAN_SOURCE_FOR_NODE_B 1

    // Create a Decoder manifest protocol buffer and pack it with the data
    Schemas::DecoderManifestMsg::DecoderManifest protoDM;

    protoDM.set_sync_id( "arn:aws:iam::123456789012:user/Development/product_1234/*" );

    // Create a Proto CANSignal
    Schemas::DecoderManifestMsg::CANSignal *protoCANSignalA = protoDM.add_can_signals();

    protoCANSignalA->set_signal_id( 3908 );
    protoCANSignalA->set_interface_id( "123" );
    protoCANSignalA->set_message_id( 600 );
    protoCANSignalA->set_is_big_endian( false );
    protoCANSignalA->set_is_signed( false );
    protoCANSignalA->set_start_bit( 0 );
    protoCANSignalA->set_offset( 100 );
    protoCANSignalA->set_factor( 10 );
    protoCANSignalA->set_length( 8 );

    Schemas::DecoderManifestMsg::CANSignal *protoCANSignalB = protoDM.add_can_signals();

    protoCANSignalB->set_signal_id( 2987 );
    protoCANSignalB->set_interface_id( "123" );
    protoCANSignalB->set_message_id( 600 );
    protoCANSignalB->set_is_big_endian( false );
    protoCANSignalB->set_is_signed( false );
    protoCANSignalB->set_start_bit( 8 );
    protoCANSignalB->set_offset( 100 );
    protoCANSignalB->set_factor( 10 );
    protoCANSignalB->set_length( 8 );
    protoCANSignalB->set_primitive_type( Schemas::DecoderManifestMsg::PrimitiveType::BOOL );
    protoCANSignalA->set_signal_value_type( Schemas::DecoderManifestMsg::SignalValueType::INTEGER );

    Schemas::DecoderManifestMsg::CANSignal *protoCANSignalC = protoDM.add_can_signals();

    protoCANSignalC->set_signal_id( 50000 );
    protoCANSignalC->set_interface_id( "4892" );
    protoCANSignalC->set_message_id( 600 );
    protoCANSignalC->set_is_big_endian( false );
    protoCANSignalC->set_is_signed( false );
    protoCANSignalC->set_start_bit( 8 );
    protoCANSignalC->set_offset( 100 );
    protoCANSignalC->set_factor( 10 );
    protoCANSignalC->set_length( 8 );
    protoCANSignalC->set_primitive_type( Schemas::DecoderManifestMsg::PrimitiveType::FLOAT32 );
    protoCANSignalC->set_signal_value_type( Schemas::DecoderManifestMsg::SignalValueType::FLOATING_POINT );

    Schemas::DecoderManifestMsg::OBDPIDSignal *protoOBDPIDSignalA = protoDM.add_obd_pid_signals();
    protoOBDPIDSignalA->set_signal_id( 123 );
    protoOBDPIDSignalA->set_pid_response_length( 10 );
    protoOBDPIDSignalA->set_service_mode( 1 );
    protoOBDPIDSignalA->set_pid( 0x70 );
    protoOBDPIDSignalA->set_scaling( 1.0 );
    protoOBDPIDSignalA->set_offset( 0.0 );
    protoOBDPIDSignalA->set_start_byte( 0 );
    protoOBDPIDSignalA->set_byte_length( 1 );
    protoOBDPIDSignalA->set_bit_right_shift( 2 );
    protoOBDPIDSignalA->set_bit_mask_length( 2 );
    protoOBDPIDSignalA->set_primitive_type( Schemas::DecoderManifestMsg::PrimitiveType::INT16 );
    protoOBDPIDSignalA->set_is_signed( true );

    Schemas::DecoderManifestMsg::OBDPIDSignal *protoOBDPIDSignalB = protoDM.add_obd_pid_signals();
    protoOBDPIDSignalB->set_signal_id( 567 );
    protoOBDPIDSignalB->set_pid_response_length( 4 );
    protoOBDPIDSignalB->set_service_mode( 1 );
    protoOBDPIDSignalB->set_pid( 0x14 );
    protoOBDPIDSignalB->set_scaling( 0.0125 );
    protoOBDPIDSignalB->set_offset( -40.0 );
    protoOBDPIDSignalB->set_start_byte( 2 );
    protoOBDPIDSignalB->set_byte_length( 2 );
    protoOBDPIDSignalB->set_bit_right_shift( 0 );
    protoOBDPIDSignalB->set_bit_mask_length( 8 );
    protoOBDPIDSignalB->set_primitive_type( Schemas::DecoderManifestMsg::PrimitiveType::UINT32 );
    protoOBDPIDSignalB->set_signal_value_type( Schemas::DecoderManifestMsg::SignalValueType::INTEGER );

    Schemas::DecoderManifestMsg::OBDPIDSignal *protoOBDPIDSignalC = protoDM.add_obd_pid_signals();
    protoOBDPIDSignalC->set_signal_id( 888 );
    protoOBDPIDSignalC->set_pid_response_length( 4 );
    protoOBDPIDSignalC->set_service_mode( 1 );
    protoOBDPIDSignalC->set_pid( 0x14 );
    protoOBDPIDSignalC->set_scaling( 0.0125 );
    protoOBDPIDSignalC->set_offset( -40.0 );
    protoOBDPIDSignalC->set_start_byte( 2 );
    protoOBDPIDSignalC->set_byte_length( 2 );
    protoOBDPIDSignalC->set_bit_right_shift( 0 );
    protoOBDPIDSignalC->set_bit_mask_length( 8 );
    protoOBDPIDSignalC->set_primitive_type( Schemas::DecoderManifestMsg::PrimitiveType::FLOAT64 );
    protoOBDPIDSignalC->set_signal_value_type( Schemas::DecoderManifestMsg::SignalValueType::FLOATING_POINT );

    auto protoCustomDecodedSignalA = protoDM.add_custom_decoding_signals();
    protoCustomDecodedSignalA->set_signal_id( 789 );
    protoCustomDecodedSignalA->set_interface_id( "456" );
    protoCustomDecodedSignalA->set_custom_decoding_id( "custom-decoder-0" );

    auto protoCustomDecodedSignalB = protoDM.add_custom_decoding_signals();
    protoCustomDecodedSignalB->set_signal_id( 111 );
    protoCustomDecodedSignalB->set_interface_id( "456" );
    protoCustomDecodedSignalB->set_custom_decoding_id( "custom-decoder-1" );

    ASSERT_NO_FATAL_FAILURE( sendMessageToReceiver( mReceiverDecoderManifest, protoDM ) );

    // This should be false because we just copied the data and it needs to be built first
    ASSERT_FALSE( mReceivedDecoderManifest->isReady() );

    // Assert that we get an empty string when we call getID on an object that's not yet built
    ASSERT_EQ( mReceivedDecoderManifest->getID(), SyncID() );

    ASSERT_TRUE( mReceivedDecoderManifest->build() );
    ASSERT_TRUE( mReceivedDecoderManifest->isReady() );

    ASSERT_EQ( mReceivedDecoderManifest->getID(), protoDM.sync_id() );

    // Get a valid CANMessageFormat
    auto testCMF =
        mReceivedDecoderManifest->getCANMessageFormat( protoCANSignalA->message_id(), protoCANSignalA->interface_id() );
    ASSERT_TRUE( testCMF.isValid() );

    // Search the CANMessageFormat signals to find the signal format that corresponds to a specific signal
    // Then make sure the data matches the proto DecoderManifest definition of that signal
    auto sigFormat =
        std::find_if( testCMF.mSignals.begin(), testCMF.mSignals.end(), [&]( const CANSignalFormat &format ) {
            return format.mSignalID == protoCANSignalA->signal_id();
        } );

    ASSERT_NE( sigFormat, testCMF.mSignals.end() );
    ASSERT_EQ( mReceivedDecoderManifest->getCANFrameAndInterfaceID( sigFormat->mSignalID ).second,
               protoCANSignalA->interface_id() );
    ASSERT_EQ( mReceivedDecoderManifest->getCANFrameAndInterfaceID( sigFormat->mSignalID ).first,
               protoCANSignalA->message_id() );
    ASSERT_EQ( sigFormat->mIsBigEndian, protoCANSignalA->is_big_endian() );
    ASSERT_EQ( sigFormat->mIsSigned, protoCANSignalA->is_signed() );
    ASSERT_EQ( sigFormat->mFirstBitPosition, protoCANSignalA->start_bit() );
    ASSERT_EQ( sigFormat->mOffset, protoCANSignalA->offset() );
    ASSERT_EQ( sigFormat->mFactor, protoCANSignalA->factor() );
    ASSERT_EQ( sigFormat->mSizeInBits, protoCANSignalA->length() );
    ASSERT_EQ( sigFormat->mSignalType, SignalType::DOUBLE );
    ASSERT_EQ( sigFormat->mRawSignalType, RawSignalType::INTEGER );

    sigFormat = std::find_if( testCMF.mSignals.begin(), testCMF.mSignals.end(), [&]( const CANSignalFormat &format ) {
        return format.mSignalID == protoCANSignalB->signal_id();
    } );
    ASSERT_NE( sigFormat, testCMF.mSignals.end() );
    ASSERT_EQ( sigFormat->mSignalType, SignalType::BOOLEAN );
    ASSERT_EQ( sigFormat->mRawSignalType, RawSignalType::INTEGER );

    testCMF =
        mReceivedDecoderManifest->getCANMessageFormat( protoCANSignalC->message_id(), protoCANSignalC->interface_id() );
    sigFormat = std::find_if( testCMF.mSignals.begin(), testCMF.mSignals.end(), [&]( const CANSignalFormat &format ) {
        return format.mSignalID == protoCANSignalC->signal_id();
    } );
    ASSERT_EQ( sigFormat->mSignalType, SignalType::FLOAT );
    ASSERT_EQ( sigFormat->mRawSignalType, RawSignalType::FLOATING_POINT );

    // Make sure we get a pair of Invalid CAN and Node Ids, for an signal that the the decoder manifest doesn't have
    ASSERT_EQ( mReceivedDecoderManifest->getCANFrameAndInterfaceID( 9999999 ),
               std::make_pair( INVALID_CAN_FRAME_ID, INVALID_INTERFACE_ID ) );
    ASSERT_EQ( mReceivedDecoderManifest->getCANFrameAndInterfaceID( protoCANSignalC->signal_id() ),
               std::make_pair( protoCANSignalC->message_id(), protoCANSignalC->interface_id() ) );

    // Verify OBD-II PID Signals decoder manifest are correctly processed
    auto obdPIDDecoderFormat = mReceivedDecoderManifest->getPIDSignalDecoderFormat( 123 );
    ASSERT_EQ( obdPIDDecoderFormat.mPidResponseLength, protoOBDPIDSignalA->pid_response_length() );
    ASSERT_EQ( toUType( obdPIDDecoderFormat.mServiceMode ), protoOBDPIDSignalA->service_mode() );
    ASSERT_EQ( obdPIDDecoderFormat.mPID, protoOBDPIDSignalA->pid() );
    ASSERT_EQ( obdPIDDecoderFormat.mScaling, protoOBDPIDSignalA->scaling() );
    ASSERT_EQ( obdPIDDecoderFormat.mOffset, protoOBDPIDSignalA->offset() );
    ASSERT_EQ( obdPIDDecoderFormat.mStartByte, protoOBDPIDSignalA->start_byte() );
    ASSERT_EQ( obdPIDDecoderFormat.mByteLength, protoOBDPIDSignalA->byte_length() );
    ASSERT_EQ( obdPIDDecoderFormat.mBitRightShift, protoOBDPIDSignalA->bit_right_shift() );
    ASSERT_EQ( obdPIDDecoderFormat.mBitMaskLength, protoOBDPIDSignalA->bit_mask_length() );
    ASSERT_EQ( obdPIDDecoderFormat.mIsSigned, true );
    ASSERT_EQ( obdPIDDecoderFormat.mSignalType, SignalType::INT16 );
    ASSERT_EQ( obdPIDDecoderFormat.mRawSignalType, RawSignalType::INTEGER );

    obdPIDDecoderFormat = mReceivedDecoderManifest->getPIDSignalDecoderFormat( 567 );
    ASSERT_EQ( obdPIDDecoderFormat.mPidResponseLength, protoOBDPIDSignalB->pid_response_length() );
    ASSERT_EQ( toUType( obdPIDDecoderFormat.mServiceMode ), protoOBDPIDSignalB->service_mode() );
    ASSERT_EQ( obdPIDDecoderFormat.mPID, protoOBDPIDSignalB->pid() );
    ASSERT_EQ( obdPIDDecoderFormat.mScaling, protoOBDPIDSignalB->scaling() );
    ASSERT_EQ( obdPIDDecoderFormat.mOffset, protoOBDPIDSignalB->offset() );
    ASSERT_EQ( obdPIDDecoderFormat.mStartByte, protoOBDPIDSignalB->start_byte() );
    ASSERT_EQ( obdPIDDecoderFormat.mByteLength, protoOBDPIDSignalB->byte_length() );
    ASSERT_EQ( obdPIDDecoderFormat.mBitRightShift, protoOBDPIDSignalB->bit_right_shift() );
    ASSERT_EQ( obdPIDDecoderFormat.mBitMaskLength, protoOBDPIDSignalB->bit_mask_length() );
    ASSERT_EQ( obdPIDDecoderFormat.mSignalType, SignalType::UINT32 );
    ASSERT_EQ( obdPIDDecoderFormat.mRawSignalType, RawSignalType::INTEGER );

    obdPIDDecoderFormat = mReceivedDecoderManifest->getPIDSignalDecoderFormat( 888 );
    ASSERT_EQ( obdPIDDecoderFormat.mPidResponseLength, protoOBDPIDSignalC->pid_response_length() );
    ASSERT_EQ( toUType( obdPIDDecoderFormat.mServiceMode ), protoOBDPIDSignalC->service_mode() );
    ASSERT_EQ( obdPIDDecoderFormat.mPID, protoOBDPIDSignalC->pid() );
    ASSERT_EQ( obdPIDDecoderFormat.mScaling, protoOBDPIDSignalC->scaling() );
    ASSERT_EQ( obdPIDDecoderFormat.mOffset, protoOBDPIDSignalC->offset() );
    ASSERT_EQ( obdPIDDecoderFormat.mStartByte, protoOBDPIDSignalC->start_byte() );
    ASSERT_EQ( obdPIDDecoderFormat.mByteLength, protoOBDPIDSignalC->byte_length() );
    ASSERT_EQ( obdPIDDecoderFormat.mBitRightShift, protoOBDPIDSignalC->bit_right_shift() );
    ASSERT_EQ( obdPIDDecoderFormat.mBitMaskLength, protoOBDPIDSignalC->bit_mask_length() );
    ASSERT_EQ( obdPIDDecoderFormat.mSignalType, SignalType::DOUBLE );
    ASSERT_EQ( obdPIDDecoderFormat.mRawSignalType, RawSignalType::FLOATING_POINT );

    // There's no signal ID 890, hence this function shall return an INVALID_PID_DECODER_FORMAT
    obdPIDDecoderFormat = mReceivedDecoderManifest->getPIDSignalDecoderFormat( 890 );
    ASSERT_EQ( obdPIDDecoderFormat, NOT_FOUND_PID_DECODER_FORMAT );

    ASSERT_EQ( mReceivedDecoderManifest->getNetworkProtocol( 3908 ), VehicleDataSourceProtocol::RAW_SOCKET );
    ASSERT_EQ( mReceivedDecoderManifest->getNetworkProtocol( 2987 ), VehicleDataSourceProtocol::RAW_SOCKET );
    ASSERT_EQ( mReceivedDecoderManifest->getNetworkProtocol( 50000 ), VehicleDataSourceProtocol::RAW_SOCKET );
    ASSERT_EQ( mReceivedDecoderManifest->getNetworkProtocol( 123 ), VehicleDataSourceProtocol::OBD );
    ASSERT_EQ( mReceivedDecoderManifest->getNetworkProtocol( 567 ), VehicleDataSourceProtocol::OBD );

    auto customSignalDecoderFormat = mReceivedDecoderManifest->getCustomSignalDecoderFormat( 9999999 );
    ASSERT_EQ( customSignalDecoderFormat.mInterfaceId, INVALID_INTERFACE_ID );
    ASSERT_EQ( customSignalDecoderFormat.mDecoder, INVALID_CUSTOM_SIGNAL_DECODER );
    customSignalDecoderFormat = mReceivedDecoderManifest->getCustomSignalDecoderFormat( 789 );
    ASSERT_EQ( customSignalDecoderFormat.mInterfaceId, "456" );
    ASSERT_EQ( customSignalDecoderFormat.mDecoder, "custom-decoder-0" );
    customSignalDecoderFormat = mReceivedDecoderManifest->getCustomSignalDecoderFormat( 111 );
    ASSERT_EQ( customSignalDecoderFormat.mInterfaceId, "456" );
    ASSERT_EQ( customSignalDecoderFormat.mDecoder, "custom-decoder-1" );
}

/**
 * @brief This test writes an invalid DecoderManifest object to a protobuf binary array. The decoder manifest doesn't
 * contain CAN Node, CAN Signal, OBD Signal. When CollectionScheme Ingestion start building, it will return failure due
 * to invalid decoder manifest
 */
TEST_F( SchemaTest, SchemaInvalidDecoderManifestTest )
{
    // Create a Decoder manifest protocol buffer and pack it with the data
    Schemas::DecoderManifestMsg::DecoderManifest protoDM;

    protoDM.set_sync_id( "arn:aws:iam::123456789012:user/Development/product_1234/*" );

    ASSERT_NO_FATAL_FAILURE( sendMessageToReceiver( mReceiverDecoderManifest, protoDM ) );

    // This should be false because we just copied the data and it needs to be built first
    ASSERT_FALSE( mReceivedDecoderManifest->isReady() );

    // Assert that we get an empty string when we call getID on an unbuilt object
    ASSERT_EQ( mReceivedDecoderManifest->getID(), SyncID() );

    ASSERT_FALSE( mReceivedDecoderManifest->build() );
    ASSERT_FALSE( mReceivedDecoderManifest->isReady() );

    auto customSignalDecoderFormat = mReceivedDecoderManifest->getCustomSignalDecoderFormat( 9999999 );
    ASSERT_EQ( customSignalDecoderFormat.mInterfaceId, INVALID_INTERFACE_ID );
    ASSERT_EQ( customSignalDecoderFormat.mDecoder, INVALID_CUSTOM_SIGNAL_DECODER );
}

TEST_F( SchemaTest, CollectionSchemeIngestionList )
{
    CollectionSchemeIngestionList collectionSchemeIngestionList;

    // Try to build with no data - this should fail
    ASSERT_FALSE( collectionSchemeIngestionList.build() );

    // Try to copy empty data - this should fail
    ASSERT_FALSE( collectionSchemeIngestionList.copyData( nullptr, 0 ) );

    // Try using garbage data to copy and build
    std::string garbageString = "This is garbage data";

    // Copy the garbage data and make sure the copy works - copy only fails if no data present
    ASSERT_TRUE( collectionSchemeIngestionList.copyData( reinterpret_cast<const uint8_t *>( garbageString.data() ),
                                                         garbageString.length() ) );

    // Try to build with garbage data - this should fail
    ASSERT_FALSE( collectionSchemeIngestionList.build() );
}

TEST_F( SchemaTest, CollectionSchemeBasic )
{
    // Now lets try some real data :)
    Schemas::CollectionSchemesMsg::CollectionSchemes protoCollectionSchemesMsg;

    auto p1 = protoCollectionSchemesMsg.add_collection_schemes();
    auto p2 = protoCollectionSchemesMsg.add_collection_schemes();
    auto p3 = protoCollectionSchemesMsg.add_collection_schemes();

    // Make a list of collectionScheme ARNs
    std::vector<SyncID> collectionSchemeARNs = { "P1", "P2", "P3" };

    p1->set_campaign_sync_id( collectionSchemeARNs[0] );
    p2->set_campaign_sync_id( collectionSchemeARNs[1] );
    p3->set_campaign_sync_id( collectionSchemeARNs[2] );

    ASSERT_NO_FATAL_FAILURE( sendMessageToReceiver( mReceiverCollectionSchemeList, protoCollectionSchemesMsg ) );

    // Try to build - this should succeed because we have real data
    ASSERT_TRUE( mReceivedCollectionSchemeList->build() );

    // Make sure the is ready is good to go
    ASSERT_TRUE( mReceivedCollectionSchemeList->isReady() );

    ASSERT_EQ( mReceivedCollectionSchemeList->getCollectionSchemes().size(), 0 );
}

TEST_F( SchemaTest, EmptyCollectionSchemeIngestion )
{
    // Now we have data to pack our DecoderManifestIngestion object with!
    CollectionSchemeIngestion collectionSchemeTest{
#ifdef FWE_FEATURE_VISION_SYSTEM_DATA
        std::make_shared<CollectionSchemeIngestion::PartialSignalIDLookup>()
#endif
    };

    // isReady should evaluate to False
    ASSERT_TRUE( collectionSchemeTest.isReady() == false );

    // Confirm that Message Metadata is not ready as Build has not been called
    ASSERT_FALSE( collectionSchemeTest.getCollectionSchemeID().compare( SyncID() ) );
    ASSERT_FALSE( collectionSchemeTest.getDecoderManifestID().compare( SyncID() ) );
    ASSERT_TRUE( collectionSchemeTest.getStartTime() == std::numeric_limits<uint64_t>::max() );
    ASSERT_TRUE( collectionSchemeTest.getExpiryTime() == std::numeric_limits<uint64_t>::max() );
    ASSERT_TRUE( collectionSchemeTest.getAfterDurationMs() == std::numeric_limits<uint32_t>::max() );
    ASSERT_TRUE( collectionSchemeTest.isActiveDTCsIncluded() == false );
    ASSERT_TRUE( collectionSchemeTest.isTriggerOnlyOnRisingEdge() == false );
    ASSERT_TRUE( collectionSchemeTest.getCollectSignals().size() == 0 );
    ASSERT_TRUE( collectionSchemeTest.isPersistNeeded() == false );
    ASSERT_TRUE( collectionSchemeTest.isCompressionNeeded() == false );
    ASSERT_TRUE( collectionSchemeTest.getPriority() == std::numeric_limits<uint32_t>::max() );
    ASSERT_TRUE( collectionSchemeTest.getCondition() == nullptr );
    ASSERT_TRUE( collectionSchemeTest.getMinimumPublishIntervalMs() == std::numeric_limits<uint32_t>::max() );
    ASSERT_TRUE( collectionSchemeTest.getAllExpressionNodes().size() == 0 );
#ifdef FWE_FEATURE_VISION_SYSTEM_DATA
    ASSERT_TRUE( collectionSchemeTest.getS3UploadMetadata() == S3UploadMetadata() );
#endif
}

TEST_F( SchemaTest, CollectionSchemeIngestionHeartBeat )
{
    // Create a  collection scheme Proto Message
    Schemas::CollectionSchemesMsg::CollectionSchemes protoCollectionSchemesMsg;
    auto collectionSchemeTestMessage = protoCollectionSchemesMsg.add_collection_schemes();
    collectionSchemeTestMessage->set_campaign_sync_id( "arn:aws:iam::2.23606797749:user/Development/product_1234/*" );
    collectionSchemeTestMessage->set_decoder_manifest_sync_id( "model_manifest_12" );
    collectionSchemeTestMessage->set_start_time_ms_epoch( 1621448160000 );
    collectionSchemeTestMessage->set_expiry_time_ms_epoch( 2621448160000 );

    // Create a Time_based_collection_scheme
    Schemas::CollectionSchemesMsg::TimeBasedCollectionScheme *message1 =
        collectionSchemeTestMessage->mutable_time_based_collection_scheme();
    message1->set_time_based_collection_scheme_period_ms( 5000 );

    collectionSchemeTestMessage->set_after_duration_ms( 0 );
    collectionSchemeTestMessage->set_include_active_dtcs( true );
    collectionSchemeTestMessage->set_persist_all_collected_data( true );
    collectionSchemeTestMessage->set_compress_collected_data( true );
    collectionSchemeTestMessage->set_priority( 9 );

    // Add 3 Signals
    Schemas::CollectionSchemesMsg::SignalInformation *signal1 = collectionSchemeTestMessage->add_signal_information();
    signal1->set_signal_id( 0 );
    signal1->set_sample_buffer_size( 10000 );
    signal1->set_minimum_sample_period_ms( 1000 );
    signal1->set_fixed_window_period_ms( 1000 );
    signal1->set_condition_only_signal( false );

    Schemas::CollectionSchemesMsg::SignalInformation *signal2 = collectionSchemeTestMessage->add_signal_information();
    signal2->set_signal_id( 1 );
    signal2->set_sample_buffer_size( 10000 );
    signal2->set_minimum_sample_period_ms( 1000 );
    signal2->set_fixed_window_period_ms( 1000 );
    signal2->set_condition_only_signal( false );

    Schemas::CollectionSchemesMsg::SignalInformation *signal3 = collectionSchemeTestMessage->add_signal_information();
    signal3->set_signal_id( 2 );
    signal3->set_sample_buffer_size( 1000 );
    signal3->set_minimum_sample_period_ms( 100 );
    signal3->set_fixed_window_period_ms( 100 );
    signal3->set_condition_only_signal( true );

    ASSERT_NO_FATAL_FAILURE( sendMessageToReceiver( mReceiverCollectionSchemeList, protoCollectionSchemesMsg ) );

    ASSERT_TRUE( mReceivedCollectionSchemeList->build() );
    ASSERT_EQ( mReceivedCollectionSchemeList->getCollectionSchemes().size(), 1 );
    auto collectionSchemeTest = mReceivedCollectionSchemeList->getCollectionSchemes().at( 0 );

    // isReady should now evaluate to True
    ASSERT_TRUE( collectionSchemeTest->isReady() == true );

    // Confirm that the fields now match the set values in the proto message
    ASSERT_FALSE( collectionSchemeTest->getCollectionSchemeID().compare(
        "arn:aws:iam::2.23606797749:user/Development/product_1234/*" ) );
    ASSERT_FALSE( collectionSchemeTest->getDecoderManifestID().compare( "model_manifest_12" ) );
    ASSERT_TRUE( collectionSchemeTest->getStartTime() == 1621448160000 );
    ASSERT_TRUE( collectionSchemeTest->getExpiryTime() == 2621448160000 );
    ASSERT_TRUE( collectionSchemeTest->getAfterDurationMs() == 0 );
    ASSERT_TRUE( collectionSchemeTest->isActiveDTCsIncluded() == true );
    ASSERT_TRUE( collectionSchemeTest->isTriggerOnlyOnRisingEdge() == false );

    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().size() == 3 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 0 ).signalID == 0 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 0 ).sampleBufferSize == 10000 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 0 ).minimumSampleIntervalMs == 1000 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 0 ).fixedWindowPeriod == 1000 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 0 ).isConditionOnlySignal == false );

    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 1 ).signalID == 1 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 1 ).sampleBufferSize == 10000 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 1 ).minimumSampleIntervalMs == 1000 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 1 ).fixedWindowPeriod == 1000 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 1 ).isConditionOnlySignal == false );

    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 2 ).signalID == 2 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 2 ).sampleBufferSize == 1000 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 2 ).minimumSampleIntervalMs == 100 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 2 ).fixedWindowPeriod == 100 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 2 ).isConditionOnlySignal == true );

    ASSERT_TRUE( collectionSchemeTest->isPersistNeeded() == true );
    ASSERT_TRUE( collectionSchemeTest->isCompressionNeeded() == true );
    ASSERT_TRUE( collectionSchemeTest->getPriority() == 9 );
    // For time based collectionScheme the condition is always set to true hence: currentNode.booleanValue=true
    ASSERT_TRUE( collectionSchemeTest->getCondition()->booleanValue == true );
    ASSERT_TRUE( collectionSchemeTest->getCondition()->nodeType == ExpressionNodeType::BOOLEAN );
    // For time based collectionScheme the getMinimumPublishIntervalMs is the same as
    // set_time_based_collection_scheme_period_ms
    ASSERT_TRUE( collectionSchemeTest->getMinimumPublishIntervalMs() == 5000 );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().size(), 1 );

#ifdef FWE_FEATURE_VISION_SYSTEM_DATA
    // Verify Upload Metadata
    ASSERT_EQ( collectionSchemeTest->getS3UploadMetadata(), collectionSchemeTest->INVALID_S3_UPLOAD_METADATA );
#endif
}

TEST_F( SchemaTest, SchemaCollectionEventBased )
{
    const std::string DUMMY_CUSTOM_FUNCTION_NAME = "Dummy_Custom_Function";
    const std::string NOT_DUMMY_CUSTOM_FUNCTION_NAME = "Not_" + DUMMY_CUSTOM_FUNCTION_NAME;

    Schemas::CollectionSchemesMsg::CollectionSchemes protoCollectionSchemesMsg;
    auto collectionSchemeTestMessage = protoCollectionSchemesMsg.add_collection_schemes();
    collectionSchemeTestMessage->set_campaign_sync_id( "arn:aws:iam::2.23606797749:user/Development/product_1235/*" );
    collectionSchemeTestMessage->set_decoder_manifest_sync_id( "model_manifest_13" );
    collectionSchemeTestMessage->set_start_time_ms_epoch( 162144816000 );
    collectionSchemeTestMessage->set_expiry_time_ms_epoch( 262144816000 );

    // Create an Event/Condition Based CollectionScheme
    Schemas::CollectionSchemesMsg::ConditionBasedCollectionScheme *message =
        collectionSchemeTestMessage->mutable_condition_based_collection_scheme();
    message->set_condition_minimum_interval_ms( 650 );
    message->set_condition_language_version( 20 );
    message->set_condition_trigger_mode(
        Schemas::CollectionSchemesMsg::ConditionBasedCollectionScheme_ConditionTriggerMode_TRIGGER_ALWAYS );

    //  Build the AST Tree:
    //----------
    const SignalID SIGNAL_ID_1 = 19;
    const SignalID SIGNAL_ID_2 = 17;
    const SignalID SIGNAL_ID_3 = 3;

    auto *root = new Schemas::CommonTypesMsg::ConditionNode();
    message->set_allocated_condition_tree( root );
    auto *rootOp = new Schemas::CommonTypesMsg::ConditionNode_NodeOperator();
    root->set_allocated_node_operator( rootOp );
    rootOp->set_operator_( Schemas::CommonTypesMsg::ConditionNode_NodeOperator_Operator_LOGICAL_AND );

    //----------

    auto *left = new Schemas::CommonTypesMsg::ConditionNode();
    rootOp->set_allocated_left_child( left );
    auto *leftOp = new Schemas::CommonTypesMsg::ConditionNode_NodeOperator();
    left->set_allocated_node_operator( leftOp );
    leftOp->set_operator_( Schemas::CommonTypesMsg::ConditionNode_NodeOperator_Operator_LOGICAL_OR );

    auto *right = new Schemas::CommonTypesMsg::ConditionNode();
    rootOp->set_allocated_right_child( right );
    auto *rightOp = new Schemas::CommonTypesMsg::ConditionNode_NodeOperator();
    right->set_allocated_node_operator( rightOp );
    rightOp->set_operator_( Schemas::CommonTypesMsg::ConditionNode_NodeOperator_Operator_COMPARE_SMALLER );

    //----------

    auto *left_left = new Schemas::CommonTypesMsg::ConditionNode();
    leftOp->set_allocated_left_child( left_left );
    auto *left_leftOp = new Schemas::CommonTypesMsg::ConditionNode_NodeOperator();
    left_left->set_allocated_node_operator( left_leftOp );
    left_leftOp->set_operator_( Schemas::CommonTypesMsg::ConditionNode_NodeOperator_Operator_COMPARE_BIGGER );

    auto *left_right = new Schemas::CommonTypesMsg::ConditionNode();
    leftOp->set_allocated_right_child( left_right );
    auto *left_rightOp = new Schemas::CommonTypesMsg::ConditionNode_NodeOperator();
    left_right->set_allocated_node_operator( left_rightOp );
    left_rightOp->set_operator_( Schemas::CommonTypesMsg::ConditionNode_NodeOperator_Operator_COMPARE_NOT_EQUAL );

    auto *right_left = new Schemas::CommonTypesMsg::ConditionNode();
    rightOp->set_allocated_left_child( right_left );
    auto *right_leftOp = new Schemas::CommonTypesMsg::ConditionNode_NodeOperator();
    right_left->set_allocated_node_operator( right_leftOp );
    right_leftOp->set_operator_( Schemas::CommonTypesMsg::ConditionNode_NodeOperator_Operator_COMPARE_SMALLER_EQUAL );

    auto *right_right = new Schemas::CommonTypesMsg::ConditionNode();
    rightOp->set_allocated_right_child( right_right );
    auto *right_rightOp = new Schemas::CommonTypesMsg::ConditionNode_NodeOperator();
    right_right->set_allocated_node_operator( right_rightOp );
    right_rightOp->set_operator_( Schemas::CommonTypesMsg::ConditionNode_NodeOperator_Operator_COMPARE_BIGGER_EQUAL );

    //----------

    auto *left_left_left = new Schemas::CommonTypesMsg::ConditionNode();
    left_leftOp->set_allocated_left_child( left_left_left );
    left_left_left->set_node_signal_id( SIGNAL_ID_1 );

    auto *left_left_right = new Schemas::CommonTypesMsg::ConditionNode();
    left_leftOp->set_allocated_right_child( left_left_right );
    left_left_right->set_node_double_value( 1 );

    auto *left_right_left = new Schemas::CommonTypesMsg::ConditionNode();
    left_rightOp->set_allocated_left_child( left_right_left );
    auto *left_right_leftOp = new Schemas::CommonTypesMsg::ConditionNode_NodeOperator();
    left_right_left->set_allocated_node_operator( left_right_leftOp );
    left_right_leftOp->set_operator_(
        Schemas::CommonTypesMsg::ConditionNode_NodeOperator_Operator_ARITHMETIC_MULTIPLY );

    auto *left_right_right = new Schemas::CommonTypesMsg::ConditionNode();
    left_rightOp->set_allocated_right_child( left_right_right );
    auto *left_right_rightOp = new Schemas::CommonTypesMsg::ConditionNode_NodeOperator();
    left_right_right->set_allocated_node_operator( left_right_rightOp );
    left_right_rightOp->set_operator_( Schemas::CommonTypesMsg::ConditionNode_NodeOperator_Operator_ARITHMETIC_DIVIDE );

    auto *right_left_left = new Schemas::CommonTypesMsg::ConditionNode();
    right_leftOp->set_allocated_left_child( right_left_left );
    auto *right_left_leftOp = new Schemas::CommonTypesMsg::ConditionNode_NodeOperator();
    right_left_left->set_allocated_node_operator( right_left_leftOp );
    right_left_leftOp->set_operator_( Schemas::CommonTypesMsg::ConditionNode_NodeOperator_Operator_LOGICAL_NOT );

    auto *right_left_right = new Schemas::CommonTypesMsg::ConditionNode();
    right_leftOp->set_allocated_right_child( right_left_right );
    auto *right_left_rightOp = new Schemas::CommonTypesMsg::ConditionNode_NodeOperator();
    right_left_right->set_allocated_node_operator( right_left_rightOp );
    right_left_rightOp->set_operator_( Schemas::CommonTypesMsg::ConditionNode_NodeOperator_Operator_ARITHMETIC_PLUS );

    auto *right_right_left = new Schemas::CommonTypesMsg::ConditionNode();
    right_rightOp->set_allocated_left_child( right_right_left );
    auto *right_right_leftOp = new Schemas::CommonTypesMsg::ConditionNode_NodeOperator();
    right_right_left->set_allocated_node_operator( right_right_leftOp );
    right_right_leftOp->set_operator_( Schemas::CommonTypesMsg::ConditionNode_NodeOperator_Operator_ARITHMETIC_MINUS );

    auto *right_right_right = new Schemas::CommonTypesMsg::ConditionNode();
    right_rightOp->set_allocated_right_child( right_right_right );
    auto *right_right_rightOp = new Schemas::CommonTypesMsg::ConditionNode_NodeOperator();
    right_right_right->set_allocated_node_operator( right_right_rightOp );
    right_right_rightOp->set_operator_( Schemas::CommonTypesMsg::ConditionNode_NodeOperator_Operator_COMPARE_EQUAL );

    //----------

    auto *left_right_left_left = new Schemas::CommonTypesMsg::ConditionNode();
    left_right_leftOp->set_allocated_left_child( left_right_left_left );
    left_right_left_left->set_node_signal_id( SIGNAL_ID_1 );

    auto *left_right_left_right = new Schemas::CommonTypesMsg::ConditionNode();
    left_right_leftOp->set_allocated_right_child( left_right_left_right );
    left_right_left_right->set_node_double_value( 1 );

    auto *left_right_right_left = new Schemas::CommonTypesMsg::ConditionNode();
    left_right_rightOp->set_allocated_left_child( left_right_right_left );
    left_right_right_left->set_node_signal_id( SIGNAL_ID_1 );

    auto *left_right_right_right = new Schemas::CommonTypesMsg::ConditionNode();
    left_right_rightOp->set_allocated_right_child( left_right_right_right );
    left_right_right_right->set_node_double_value( 1 );

    auto *right_left_left_left = new Schemas::CommonTypesMsg::ConditionNode();
    right_left_leftOp->set_allocated_left_child( right_left_left_left );
    right_left_left_left->set_node_signal_id( SIGNAL_ID_1 );

    auto *right_left_right_left = new Schemas::CommonTypesMsg::ConditionNode();
    right_left_rightOp->set_allocated_left_child( right_left_right_left );
    right_left_right_left->set_node_signal_id( SIGNAL_ID_1 );

    auto *right_left_right_right = new Schemas::CommonTypesMsg::ConditionNode();
    right_left_rightOp->set_allocated_right_child( right_left_right_right );
    right_left_right_right->set_node_string_value( "1" );

    auto *right_right_left_left = new Schemas::CommonTypesMsg::ConditionNode();
    right_right_leftOp->set_allocated_left_child( right_right_left_left );
    right_right_left_left->set_node_signal_id( SIGNAL_ID_1 );

    auto *right_right_left_right = new Schemas::CommonTypesMsg::ConditionNode();
    right_right_leftOp->set_allocated_right_child( right_right_left_right );
    right_right_left_right->set_node_double_value( 1 );

    auto *right_right_right_left = new Schemas::CommonTypesMsg::ConditionNode();
    right_right_rightOp->set_allocated_left_child( right_right_right_left );
    right_right_right_left->set_node_string_value( DUMMY_CUSTOM_FUNCTION_NAME + "_1" );

    auto *right_right_right_right = new Schemas::CommonTypesMsg::ConditionNode();
    right_right_rightOp->set_allocated_right_child( right_right_right_right );

    auto *right_right_right_right_nodeFunction = new Schemas::CommonTypesMsg::ConditionNode_NodeFunction();
    right_right_right_right->set_allocated_node_function( right_right_right_right_nodeFunction );

    auto *right_right_right_right_customFunction =
        new Schemas::CommonTypesMsg::ConditionNode_NodeFunction_CustomFunction();
    right_right_right_right_nodeFunction->set_allocated_custom_function( right_right_right_right_customFunction );
    right_right_right_right_customFunction->set_function_name( DUMMY_CUSTOM_FUNCTION_NAME );

    Schemas::CommonTypesMsg::ConditionNode *right_right_right_right_customFunctionParam =
        right_right_right_right_customFunction->add_params();

    auto *right_right_right_right_customFunctionParam_nodeFunction =
        new Schemas::CommonTypesMsg::ConditionNode_NodeFunction();
    right_right_right_right_customFunctionParam->set_allocated_node_function(
        right_right_right_right_customFunctionParam_nodeFunction );

    auto *right_right_right_right_customFunctionParam_isNullFunction =
        new Schemas::CommonTypesMsg::ConditionNode_NodeFunction_IsNullFunction();
    right_right_right_right_customFunctionParam_nodeFunction->set_allocated_is_null_function(
        right_right_right_right_customFunctionParam_isNullFunction );

    auto *right_right_right_right_left = new Schemas::CommonTypesMsg::ConditionNode();
    right_right_right_right_customFunctionParam_isNullFunction->set_allocated_expression(
        right_right_right_right_left );
    right_right_right_right_left->set_node_signal_id( SIGNAL_ID_1 );

    //----------

    collectionSchemeTestMessage->set_after_duration_ms( 0 );
    collectionSchemeTestMessage->set_include_active_dtcs( true );
    collectionSchemeTestMessage->set_persist_all_collected_data( true );
    collectionSchemeTestMessage->set_compress_collected_data( true );
    collectionSchemeTestMessage->set_priority( 5 );

    // add FetchInformation 1
    Schemas::CollectionSchemesMsg::FetchInformation *fetchInformation1 =
        collectionSchemeTestMessage->add_signal_fetch_information();
    fetchInformation1->set_signal_id( SIGNAL_ID_2 );
    fetchInformation1->set_condition_language_version( 0 );

    auto *timeBasedFetchConfig1 = new Schemas::CollectionSchemesMsg::TimeBasedFetchConfig();
    fetchInformation1->set_allocated_time_based( timeBasedFetchConfig1 );
    timeBasedFetchConfig1->set_max_execution_count( 111 );
    timeBasedFetchConfig1->set_execution_frequency_ms( 222 );
    timeBasedFetchConfig1->set_reset_max_execution_count_interval_ms( 333 );

    Schemas::CommonTypesMsg::ConditionNode *fetchInformation1Action1 = fetchInformation1->add_actions();

    auto *fetchInformation1Action1NodeFunction = new Schemas::CommonTypesMsg::ConditionNode_NodeFunction();
    fetchInformation1Action1->set_allocated_node_function( fetchInformation1Action1NodeFunction );

    auto *fetchInformation1Action1IsNullFunction =
        new Schemas::CommonTypesMsg::ConditionNode_NodeFunction_IsNullFunction();
    fetchInformation1Action1NodeFunction->set_allocated_is_null_function( fetchInformation1Action1IsNullFunction );

    auto *fetchInformation1Action1IsNullFunctionExpression = new Schemas::CommonTypesMsg::ConditionNode();
    fetchInformation1Action1IsNullFunction->set_allocated_expression(
        fetchInformation1Action1IsNullFunctionExpression );
    fetchInformation1Action1IsNullFunctionExpression->set_node_signal_id( SIGNAL_ID_3 );

    // add FetchInformation 2
    Schemas::CollectionSchemesMsg::FetchInformation *fetchInformation2 =
        collectionSchemeTestMessage->add_signal_fetch_information();
    fetchInformation2->set_signal_id( SIGNAL_ID_3 );
    fetchInformation2->set_condition_language_version( 0 );

    auto *conditionBasedFetchConfig2 = new Schemas::CollectionSchemesMsg::ConditionBasedFetchConfig();
    fetchInformation2->set_allocated_condition_based( conditionBasedFetchConfig2 );
    conditionBasedFetchConfig2->set_condition_trigger_mode(
        Schemas::CollectionSchemesMsg::ConditionBasedFetchConfig_ConditionTriggerMode_TRIGGER_ALWAYS );

    auto *conditionBasedFetchConfig2Condition = new Schemas::CommonTypesMsg::ConditionNode();
    conditionBasedFetchConfig2->set_allocated_condition_tree( conditionBasedFetchConfig2Condition );
    conditionBasedFetchConfig2Condition->set_node_boolean_value( true );

    Schemas::CommonTypesMsg::ConditionNode *fetchInformation2Action1 = fetchInformation2->add_actions();

    auto *fetchInformation2Action1NodeFunction = new Schemas::CommonTypesMsg::ConditionNode_NodeFunction();
    fetchInformation2Action1->set_allocated_node_function( fetchInformation2Action1NodeFunction );

    auto *fetchInformation2Action1CustomFunction =
        new Schemas::CommonTypesMsg::ConditionNode_NodeFunction_CustomFunction();
    fetchInformation2Action1NodeFunction->set_allocated_custom_function( fetchInformation2Action1CustomFunction );
    fetchInformation2Action1CustomFunction->set_function_name( DUMMY_CUSTOM_FUNCTION_NAME );

    Schemas::CommonTypesMsg::ConditionNode *fetchInformation2Action2 = fetchInformation2->add_actions();

    auto *fetchInformation2Action2NodeFunction = new Schemas::CommonTypesMsg::ConditionNode_NodeFunction();
    fetchInformation2Action2->set_allocated_node_function( fetchInformation2Action2NodeFunction );

    auto *fetchInformation2Action2CustomFunction =
        new Schemas::CommonTypesMsg::ConditionNode_NodeFunction_CustomFunction();
    fetchInformation2Action2NodeFunction->set_allocated_custom_function( fetchInformation2Action2CustomFunction );
    fetchInformation2Action2CustomFunction->set_function_name( NOT_DUMMY_CUSTOM_FUNCTION_NAME );

    // add FetchInformation 3
    Schemas::CollectionSchemesMsg::FetchInformation *fetchInformation3 =
        collectionSchemeTestMessage->add_signal_fetch_information();
    fetchInformation3->set_signal_id( SIGNAL_ID_1 );
    fetchInformation3->set_condition_language_version( 0 );

    auto *conditionBasedFetchConfig3 = new Schemas::CollectionSchemesMsg::ConditionBasedFetchConfig();
    fetchInformation3->set_allocated_condition_based( conditionBasedFetchConfig3 );
    conditionBasedFetchConfig3->set_condition_trigger_mode(
        Schemas::CollectionSchemesMsg::ConditionBasedFetchConfig_ConditionTriggerMode_TRIGGER_ONLY_ON_RISING_EDGE );

    auto *conditionBasedFetchConfig3Condition = new Schemas::CommonTypesMsg::ConditionNode();
    conditionBasedFetchConfig3->set_allocated_condition_tree( conditionBasedFetchConfig3Condition );
    conditionBasedFetchConfig3Condition->set_node_boolean_value( false );

    // Add 3 Signals
    Schemas::CollectionSchemesMsg::SignalInformation *signal1 = collectionSchemeTestMessage->add_signal_information();
    signal1->set_signal_id( SIGNAL_ID_1 );
    signal1->set_sample_buffer_size( 5 );
    signal1->set_minimum_sample_period_ms( 500 );
    signal1->set_fixed_window_period_ms( 600 );
    signal1->set_condition_only_signal( true );

    Schemas::CollectionSchemesMsg::SignalInformation *signal2 = collectionSchemeTestMessage->add_signal_information();
    signal2->set_signal_id( SIGNAL_ID_2 );
    signal2->set_sample_buffer_size( 10000 );
    signal2->set_minimum_sample_period_ms( 1000 );
    signal2->set_fixed_window_period_ms( 1000 );
    signal2->set_condition_only_signal( false );

    Schemas::CollectionSchemesMsg::SignalInformation *signal3 = collectionSchemeTestMessage->add_signal_information();
    signal3->set_signal_id( SIGNAL_ID_3 );
    signal3->set_sample_buffer_size( 1000 );
    signal3->set_minimum_sample_period_ms( 100 );
    signal3->set_fixed_window_period_ms( 100 );
    signal3->set_condition_only_signal( true );

#ifdef FWE_FEATURE_VISION_SYSTEM_DATA
    auto *s3_upload_metadata = new Schemas::CollectionSchemesMsg::S3UploadMetadata();
    s3_upload_metadata->set_bucket_name( "testBucketName" );
    s3_upload_metadata->set_prefix( "testPrefix/" );
    s3_upload_metadata->set_region( "us-west-2" );
    s3_upload_metadata->set_bucket_owner_account_id( "012345678901" );
    collectionSchemeTestMessage->set_allocated_s3_upload_metadata( s3_upload_metadata );
#endif

    ASSERT_NO_FATAL_FAILURE( sendMessageToReceiver( mReceiverCollectionSchemeList, protoCollectionSchemesMsg ) );

    ASSERT_TRUE( mReceivedCollectionSchemeList->build() );
    ASSERT_EQ( mReceivedCollectionSchemeList->getCollectionSchemes().size(), 1 );
    auto collectionSchemeTest = mReceivedCollectionSchemeList->getCollectionSchemes().at( 0 );

    // isReady should now evaluate to True
    ASSERT_TRUE( collectionSchemeTest->isReady() == true );

    // Confirm that the fields now match the set values in the proto message
    ASSERT_FALSE( collectionSchemeTest->getCollectionSchemeID().compare(
        "arn:aws:iam::2.23606797749:user/Development/product_1235/*" ) );
    ASSERT_FALSE( collectionSchemeTest->getDecoderManifestID().compare( "model_manifest_13" ) );
    ASSERT_TRUE( collectionSchemeTest->getStartTime() == 162144816000 );
    ASSERT_TRUE( collectionSchemeTest->getExpiryTime() == 262144816000 );
    ASSERT_TRUE( collectionSchemeTest->getAfterDurationMs() == 0 );
    ASSERT_TRUE( collectionSchemeTest->isActiveDTCsIncluded() == true );
    ASSERT_TRUE( collectionSchemeTest->isTriggerOnlyOnRisingEdge() == false );

    // check FetchInformation
    ASSERT_TRUE( collectionSchemeTest->getAllFetchInformations().size() == 3 );

    ASSERT_TRUE( collectionSchemeTest->getAllFetchInformations().at( 0 ).signalID == SIGNAL_ID_2 );
    ASSERT_TRUE( collectionSchemeTest->getAllFetchInformations().at( 0 ).triggerOnlyOnRisingEdge == false );
    ASSERT_TRUE( collectionSchemeTest->getAllFetchInformations().at( 0 ).maxExecutionPerInterval == 111 );
    ASSERT_TRUE( collectionSchemeTest->getAllFetchInformations().at( 0 ).executionPeriodMs == 222 );
    ASSERT_TRUE( collectionSchemeTest->getAllFetchInformations().at( 0 ).executionIntervalMs == 333 );
    ASSERT_TRUE( collectionSchemeTest->getAllFetchInformations().at( 0 ).condition == nullptr );
    ASSERT_TRUE( collectionSchemeTest->getAllFetchInformations().at( 0 ).actions.size() == 1 );
    ASSERT_TRUE( collectionSchemeTest->getAllFetchInformations().at( 0 ).actions.at( 0 )->nodeType ==
                 ExpressionNodeType::IS_NULL_FUNCTION );
    ASSERT_TRUE( collectionSchemeTest->getAllFetchInformations().at( 0 ).actions.at( 0 )->left->signalID ==
                 SIGNAL_ID_3 );

    ASSERT_TRUE( collectionSchemeTest->getAllFetchInformations().at( 1 ).signalID == SIGNAL_ID_3 );
    ASSERT_TRUE( collectionSchemeTest->getAllFetchInformations().at( 1 ).triggerOnlyOnRisingEdge == false );
    ASSERT_TRUE( collectionSchemeTest->getAllFetchInformations().at( 1 ).maxExecutionPerInterval == 0 );
    ASSERT_TRUE( collectionSchemeTest->getAllFetchInformations().at( 1 ).executionPeriodMs == 0 );
    ASSERT_TRUE( collectionSchemeTest->getAllFetchInformations().at( 1 ).executionIntervalMs == 0 );
    ASSERT_TRUE( collectionSchemeTest->getAllFetchInformations().at( 1 ).condition->nodeType ==
                 ExpressionNodeType::BOOLEAN );
    ASSERT_TRUE( collectionSchemeTest->getAllFetchInformations().at( 1 ).condition->booleanValue == true );
    ASSERT_TRUE( collectionSchemeTest->getAllFetchInformations().at( 1 ).actions.size() == 2 );
    ASSERT_TRUE( collectionSchemeTest->getAllFetchInformations().at( 1 ).actions.at( 0 )->nodeType ==
                 ExpressionNodeType::CUSTOM_FUNCTION );
    ASSERT_TRUE( collectionSchemeTest->getAllFetchInformations().at( 1 ).actions.at( 0 )->function.customFunctionName ==
                 DUMMY_CUSTOM_FUNCTION_NAME );
    ASSERT_TRUE(
        collectionSchemeTest->getAllFetchInformations().at( 1 ).actions.at( 0 )->function.customFunctionParams.size() ==
        0 );
    ASSERT_TRUE( collectionSchemeTest->getAllFetchInformations().at( 1 ).actions.at( 1 )->nodeType ==
                 ExpressionNodeType::CUSTOM_FUNCTION );
    ASSERT_TRUE( collectionSchemeTest->getAllFetchInformations().at( 1 ).actions.at( 1 )->function.customFunctionName ==
                 NOT_DUMMY_CUSTOM_FUNCTION_NAME );
    ASSERT_TRUE(
        collectionSchemeTest->getAllFetchInformations().at( 1 ).actions.at( 1 )->function.customFunctionParams.size() ==
        0 );

    ASSERT_TRUE( collectionSchemeTest->getAllFetchInformations().at( 2 ).signalID == SIGNAL_ID_1 );
    ASSERT_TRUE( collectionSchemeTest->getAllFetchInformations().at( 2 ).triggerOnlyOnRisingEdge == true );
    ASSERT_TRUE( collectionSchemeTest->getAllFetchInformations().at( 2 ).maxExecutionPerInterval == 0 );
    ASSERT_TRUE( collectionSchemeTest->getAllFetchInformations().at( 2 ).executionPeriodMs == 0 );
    ASSERT_TRUE( collectionSchemeTest->getAllFetchInformations().at( 2 ).executionIntervalMs == 0 );
    ASSERT_TRUE( collectionSchemeTest->getAllFetchInformations().at( 2 ).condition->nodeType ==
                 ExpressionNodeType::BOOLEAN );
    ASSERT_TRUE( collectionSchemeTest->getAllFetchInformations().at( 2 ).condition->booleanValue == false );
    ASSERT_TRUE( collectionSchemeTest->getAllFetchInformations().at( 2 ).actions.size() == 0 );

    // Signals
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().size() == 3 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 0 ).signalID == SIGNAL_ID_1 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 0 ).sampleBufferSize == 5 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 0 ).minimumSampleIntervalMs == 500 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 0 ).fixedWindowPeriod == 600 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 0 ).isConditionOnlySignal == true );

    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 1 ).signalID == SIGNAL_ID_2 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 1 ).sampleBufferSize == 10000 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 1 ).minimumSampleIntervalMs == 1000 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 1 ).fixedWindowPeriod == 1000 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 1 ).isConditionOnlySignal == false );

    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 2 ).signalID == SIGNAL_ID_3 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 2 ).sampleBufferSize == 1000 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 2 ).minimumSampleIntervalMs == 100 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 2 ).fixedWindowPeriod == 100 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 2 ).isConditionOnlySignal == true );

    ASSERT_TRUE( collectionSchemeTest->isPersistNeeded() == true );
    ASSERT_TRUE( collectionSchemeTest->isCompressionNeeded() == true );
    ASSERT_TRUE( collectionSchemeTest->getPriority() == 5 );

    // For Event based collectionScheme the getMinimumPublishIntervalMs is the same as condition_minimum_interval_ms
    ASSERT_TRUE( collectionSchemeTest->getMinimumPublishIntervalMs() == 650 );

    // Verify the AST
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().size(), 28 );
    //----------
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).nodeType,
               ExpressionNodeType::OPERATOR_LOGICAL_AND );
    //----------
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).left->nodeType,
               ExpressionNodeType::OPERATOR_LOGICAL_OR );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).right->nodeType,
               ExpressionNodeType::OPERATOR_SMALLER );
    //----------
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).left->left->nodeType,
               ExpressionNodeType::OPERATOR_BIGGER );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).left->right->nodeType,
               ExpressionNodeType::OPERATOR_NOT_EQUAL );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).right->left->nodeType,
               ExpressionNodeType::OPERATOR_SMALLER_EQUAL );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).right->right->nodeType,
               ExpressionNodeType::OPERATOR_BIGGER_EQUAL );
    //----------
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).left->left->left->nodeType,
               ExpressionNodeType::SIGNAL );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).left->left->left->signalID, SIGNAL_ID_1 );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).left->left->right->nodeType,
               ExpressionNodeType::FLOAT );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).left->left->right->floatingValue, 1 );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).left->right->left->nodeType,
               ExpressionNodeType::OPERATOR_ARITHMETIC_MULTIPLY );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).left->right->right->nodeType,
               ExpressionNodeType::OPERATOR_ARITHMETIC_DIVIDE );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).right->left->left->nodeType,
               ExpressionNodeType::OPERATOR_LOGICAL_NOT );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).right->left->right->nodeType,
               ExpressionNodeType::OPERATOR_ARITHMETIC_PLUS );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).right->right->left->nodeType,
               ExpressionNodeType::OPERATOR_ARITHMETIC_MINUS );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).right->right->right->nodeType,
               ExpressionNodeType::OPERATOR_EQUAL );
    //----------
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).left->right->left->left->nodeType,
               ExpressionNodeType::SIGNAL );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).left->right->left->left->signalID, SIGNAL_ID_1 );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).left->right->left->right->nodeType,
               ExpressionNodeType::FLOAT );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).left->right->left->right->floatingValue, 1 );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).left->right->right->left->nodeType,
               ExpressionNodeType::SIGNAL );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).left->right->right->left->signalID, SIGNAL_ID_1 );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).left->right->right->right->nodeType,
               ExpressionNodeType::FLOAT );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).left->right->right->right->floatingValue, 1 );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).right->left->left->left->nodeType,
               ExpressionNodeType::SIGNAL );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).right->left->left->left->signalID, SIGNAL_ID_1 );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).right->left->left->right, nullptr );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).right->left->right->left->nodeType,
               ExpressionNodeType::SIGNAL );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).right->left->right->left->signalID, SIGNAL_ID_1 );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).right->left->right->right->nodeType,
               ExpressionNodeType::STRING );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).right->left->right->right->stringValue, "1" );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).right->right->left->left->nodeType,
               ExpressionNodeType::SIGNAL );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).right->right->left->left->signalID, SIGNAL_ID_1 );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).right->right->left->right->nodeType,
               ExpressionNodeType::FLOAT );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).right->right->left->right->floatingValue, 1 );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).right->right->right->left->nodeType,
               ExpressionNodeType::STRING );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).right->right->right->left->stringValue,
               DUMMY_CUSTOM_FUNCTION_NAME + "_1" );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).right->right->right->right->nodeType,
               ExpressionNodeType::CUSTOM_FUNCTION );
    ASSERT_EQ(
        collectionSchemeTest->getAllExpressionNodes().at( 0 ).right->right->right->right->function.customFunctionName,
        DUMMY_CUSTOM_FUNCTION_NAME );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes()
                   .at( 0 )
                   .right->right->right->right->function.customFunctionParams.size(),
               1 );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes()
                   .at( 0 )
                   .right->right->right->right->function.customFunctionParams[0]
                   ->nodeType,
               ExpressionNodeType::IS_NULL_FUNCTION );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes()
                   .at( 0 )
                   .right->right->right->right->function.customFunctionParams[0]
                   ->left->nodeType,
               ExpressionNodeType::SIGNAL );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes()
                   .at( 0 )
                   .right->right->right->right->function.customFunctionParams[0]
                   ->left->signalID,
               SIGNAL_ID_1 );
    //----------
    ASSERT_TRUE( collectionSchemeTest->getCondition()->booleanValue == false );

#ifdef FWE_FEATURE_VISION_SYSTEM_DATA
    S3UploadMetadata s3UploadMetadata;
    s3UploadMetadata.bucketName = "testBucketName";
    s3UploadMetadata.prefix = "testPrefix/";
    s3UploadMetadata.region = "us-west-2";
    s3UploadMetadata.bucketOwner = "012345678901";
    ASSERT_EQ( collectionSchemeTest->getS3UploadMetadata(), s3UploadMetadata );
#endif
}

#ifdef FWE_FEATURE_STORE_AND_FORWARD
TEST_F( SchemaTest, StoreAndForwardConfiguration )
{
    Schemas::CollectionSchemesMsg::CollectionSchemes protoCollectionSchemesMsg;
    auto *collectionSchemeTestMessage = protoCollectionSchemesMsg.add_collection_schemes();
    collectionSchemeTestMessage->set_campaign_sync_id( "arn:aws:iam::2.23606797749:user/Development/product_1235/*" );
    collectionSchemeTestMessage->set_decoder_manifest_sync_id( "model_manifest_13" );
    collectionSchemeTestMessage->set_start_time_ms_epoch( 162144816000 );
    collectionSchemeTestMessage->set_expiry_time_ms_epoch( 262144816000 );

    // Create an Event/Condition Based CollectionScheme
    Schemas::CollectionSchemesMsg::ConditionBasedCollectionScheme *message =
        collectionSchemeTestMessage->mutable_condition_based_collection_scheme();
    message->set_condition_minimum_interval_ms( 650 );
    message->set_condition_language_version( 20 );
    message->set_condition_trigger_mode(
        Schemas::CollectionSchemesMsg::ConditionBasedCollectionScheme_ConditionTriggerMode_TRIGGER_ALWAYS );

    // Create store and forward configuration
    auto *store_and_forward_configuration = collectionSchemeTestMessage->mutable_store_and_forward_configuration();
    auto *store_and_forward_entry = store_and_forward_configuration->add_partition_configuration();
    auto *storage_options = store_and_forward_entry->mutable_storage_options();
    auto *upload_options = store_and_forward_entry->mutable_upload_options();
    storage_options->set_maximum_size_in_bytes( 1000000 );
    storage_options->set_storage_location( "/storage" );
    storage_options->set_minimum_time_to_live_in_seconds( 1000000 );

    //  Build the AST Tree:
    //----------

    upload_options->mutable_condition_tree()->set_node_signal_id( 10 );
    message->mutable_condition_tree();

    //----------

    collectionSchemeTestMessage->set_after_duration_ms( 0 );
    collectionSchemeTestMessage->set_include_active_dtcs( true );
    collectionSchemeTestMessage->set_persist_all_collected_data( true );
    collectionSchemeTestMessage->set_compress_collected_data( true );
    collectionSchemeTestMessage->set_priority( 5 );

    // Add 3 Signals
    Schemas::CollectionSchemesMsg::SignalInformation *signal1 = collectionSchemeTestMessage->add_signal_information();
    signal1->set_signal_id( 19 );
    signal1->set_sample_buffer_size( 5 );
    signal1->set_minimum_sample_period_ms( 500 );
    signal1->set_fixed_window_period_ms( 600 );
    signal1->set_condition_only_signal( true );
    signal1->set_data_partition_id( 1 );

    Schemas::CollectionSchemesMsg::SignalInformation *signal2 = collectionSchemeTestMessage->add_signal_information();
    signal2->set_signal_id( 17 );
    signal2->set_sample_buffer_size( 10000 );
    signal2->set_minimum_sample_period_ms( 1000 );
    signal2->set_fixed_window_period_ms( 1000 );
    signal2->set_condition_only_signal( false );
    signal2->set_data_partition_id( 1 );

    Schemas::CollectionSchemesMsg::SignalInformation *signal3 = collectionSchemeTestMessage->add_signal_information();
    signal3->set_signal_id( 3 );
    signal3->set_sample_buffer_size( 1000 );
    signal3->set_minimum_sample_period_ms( 100 );
    signal3->set_fixed_window_period_ms( 100 );
    signal3->set_condition_only_signal( true );
    signal3->set_data_partition_id( 1 );

    ASSERT_NO_FATAL_FAILURE( sendMessageToReceiver( mReceiverCollectionSchemeList, protoCollectionSchemesMsg ) );

    ASSERT_TRUE( mReceivedCollectionSchemeList->build() );
    ASSERT_EQ( mReceivedCollectionSchemeList->getCollectionSchemes().size(), 1 );
    auto collectionSchemeTest = mReceivedCollectionSchemeList->getCollectionSchemes().at( 0 );

    // isReady should now evaluate to True
    ASSERT_TRUE( collectionSchemeTest->isReady() == true );

    // Confirm that the fields now match the set values in the proto message
    ASSERT_FALSE( collectionSchemeTest->getCollectionSchemeID().compare(
        "arn:aws:iam::2.23606797749:user/Development/product_1235/*" ) );
    ASSERT_FALSE( collectionSchemeTest->getDecoderManifestID().compare( "model_manifest_13" ) );
    ASSERT_TRUE( collectionSchemeTest->getStartTime() == 162144816000 );
    ASSERT_TRUE( collectionSchemeTest->getExpiryTime() == 262144816000 );
    ASSERT_TRUE( collectionSchemeTest->getAfterDurationMs() == 0 );
    ASSERT_TRUE( collectionSchemeTest->isActiveDTCsIncluded() == true );
    ASSERT_TRUE( collectionSchemeTest->isTriggerOnlyOnRisingEdge() == false );

    // Signals
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().size() == 3 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 0 ).signalID == 19 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 0 ).sampleBufferSize == 5 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 0 ).minimumSampleIntervalMs == 500 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 0 ).fixedWindowPeriod == 600 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 0 ).isConditionOnlySignal == true );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 0 ).dataPartitionId == 1 );

    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 1 ).signalID == 17 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 1 ).sampleBufferSize == 10000 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 1 ).minimumSampleIntervalMs == 1000 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 1 ).fixedWindowPeriod == 1000 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 1 ).isConditionOnlySignal == false );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 1 ).dataPartitionId == 1 );

    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 2 ).signalID == 3 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 2 ).sampleBufferSize == 1000 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 2 ).minimumSampleIntervalMs == 100 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 2 ).fixedWindowPeriod == 100 );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 2 ).isConditionOnlySignal == true );
    ASSERT_TRUE( collectionSchemeTest->getCollectSignals().at( 2 ).dataPartitionId == 1 );

    ASSERT_TRUE( collectionSchemeTest->isPersistNeeded() == true );
    ASSERT_TRUE( collectionSchemeTest->isCompressionNeeded() == true );
    ASSERT_TRUE( collectionSchemeTest->getPriority() == 5 );

    // For Event based collectionScheme the getMinimumPublishIntervalMs is the same as condition_minimum_interval_ms
    ASSERT_TRUE( collectionSchemeTest->getMinimumPublishIntervalMs() == 650 );

    // StoreAndForward
    ASSERT_EQ( collectionSchemeTest->getStoreAndForwardConfiguration().at( 0 ).uploadOptions.conditionTree->nodeType,
               ExpressionNodeType::SIGNAL );
    ASSERT_EQ( collectionSchemeTest->getStoreAndForwardConfiguration().at( 0 ).uploadOptions.conditionTree->signalID,
               10 );
    ASSERT_TRUE( collectionSchemeTest->getStoreAndForwardConfiguration().at( 0 ).storageOptions.maximumSizeInBytes ==
                 1000000 );
    ASSERT_TRUE( collectionSchemeTest->getStoreAndForwardConfiguration().at( 0 ).storageOptions.storageLocation ==
                 "/storage" );
    ASSERT_TRUE(
        collectionSchemeTest->getStoreAndForwardConfiguration().at( 0 ).storageOptions.minimumTimeToLiveInSeconds ==
        1000000 );
}
#endif

#ifdef FWE_FEATURE_VISION_SYSTEM_DATA
TEST_F( SchemaTest, SchemaCollectionWithComplexTypes )
{
    Schemas::CollectionSchemesMsg::CollectionSchemes protoCollectionSchemesMsg;
    auto collectionSchemeTestMessage = protoCollectionSchemesMsg.add_collection_schemes();
    collectionSchemeTestMessage->set_campaign_sync_id( "arn:aws:iam::2.52543243543:user/Development/complexdata/*" );
    collectionSchemeTestMessage->set_decoder_manifest_sync_id( "model_manifest_67" );
    collectionSchemeTestMessage->set_start_time_ms_epoch( 0 );
    collectionSchemeTestMessage->set_expiry_time_ms_epoch( 9262144816000 );

    // Create an Event/Condition Based CollectionScheme
    Schemas::CollectionSchemesMsg::ConditionBasedCollectionScheme *message =
        collectionSchemeTestMessage->mutable_condition_based_collection_scheme();
    message->set_condition_minimum_interval_ms( 650 );
    message->set_condition_language_version( 1000 );
    message->set_condition_trigger_mode(
        Schemas::CollectionSchemesMsg::ConditionBasedCollectionScheme_ConditionTriggerMode_TRIGGER_ALWAYS );

    // Build a AST Tree.
    // Root: Equal
    // Left Child: average Windows of partial signal 1 in complex type
    // Right Child: partial signal 2 in complex type + partial signal 1 in complex type
    auto *root = new Schemas::CommonTypesMsg::ConditionNode();
    auto *rootOp = new Schemas::CommonTypesMsg::ConditionNode_NodeOperator();
    rootOp->set_operator_( Schemas::CommonTypesMsg::ConditionNode_NodeOperator_Operator_COMPARE_EQUAL );

    auto *leftChild = new Schemas::CommonTypesMsg::ConditionNode();
    auto *leftChildFunction = new Schemas::CommonTypesMsg::ConditionNode_NodeFunction();
    auto *leftChildAvgWindow = new Schemas::CommonTypesMsg::ConditionNode_NodeFunction_WindowFunction();
    auto *leftChildPrimitivePrimitiveType = new Schemas::CommonTypesMsg::PrimitiveTypeInComplexSignal();
    auto *leftChildSignalPath = new Schemas::CommonTypesMsg::SignalPath();

    leftChildSignalPath->add_signal_path( 34574325 );
    leftChildSignalPath->add_signal_path( 5 );
    leftChildSignalPath->add_signal_path( 0 );
    leftChildSignalPath->add_signal_path( 1000352312 );

    leftChildPrimitivePrimitiveType->set_signal_id( 1234 );
    leftChildPrimitivePrimitiveType->set_allocated_signal_path( leftChildSignalPath );

    leftChildAvgWindow->set_window_type(
        Schemas::CommonTypesMsg::ConditionNode_NodeFunction_WindowFunction_WindowType_LAST_WINDOW_AVG );
    leftChildAvgWindow->set_allocated_primitive_type_in_signal( leftChildPrimitivePrimitiveType );

    leftChildFunction->set_allocated_window_function( leftChildAvgWindow );
    leftChild->set_allocated_node_function( leftChildFunction );

    auto *rightChild = new Schemas::CommonTypesMsg::ConditionNode();
    auto *rightChildOp = new Schemas::CommonTypesMsg::ConditionNode_NodeOperator();
    rightChildOp->set_operator_( Schemas::CommonTypesMsg::ConditionNode_NodeOperator_Operator_ARITHMETIC_PLUS );

    auto *rightChildLeft = new Schemas::CommonTypesMsg::ConditionNode();
    auto *rightChildLeftPrimitivePrimitiveType = new Schemas::CommonTypesMsg::PrimitiveTypeInComplexSignal();
    auto *rightChildLeftSignalPath = new Schemas::CommonTypesMsg::SignalPath();

    rightChildLeftSignalPath->add_signal_path( 34574325 );
    rightChildLeftSignalPath->add_signal_path( 5 );
    rightChildLeftSignalPath->add_signal_path( 0 );
    rightChildLeftSignalPath->add_signal_path( 42 ); // this is different

    rightChildLeftPrimitivePrimitiveType->set_signal_id( 1234 );
    rightChildLeftPrimitivePrimitiveType->set_allocated_signal_path( rightChildLeftSignalPath );

    rightChildLeft->set_allocated_node_primitive_type_in_signal( rightChildLeftPrimitivePrimitiveType );

    auto *rightChildRight = new Schemas::CommonTypesMsg::ConditionNode();
    auto *rightChildRightPrimitivePrimitiveType = new Schemas::CommonTypesMsg::PrimitiveTypeInComplexSignal();
    auto *rightChildRightSignalPath = new Schemas::CommonTypesMsg::SignalPath();

    rightChildRightSignalPath->add_signal_path( 34574325 );
    rightChildRightSignalPath->add_signal_path( 5 );
    rightChildRightSignalPath->add_signal_path( 0 );
    rightChildRightSignalPath->add_signal_path( 1000352312 ); // this is the same as leftChildSignalPath

    rightChildRightPrimitivePrimitiveType->set_signal_id( 1234 );
    rightChildRightPrimitivePrimitiveType->set_allocated_signal_path( rightChildRightSignalPath );

    rightChildRight->set_allocated_node_primitive_type_in_signal( rightChildRightPrimitivePrimitiveType );

    rightChildOp->set_allocated_left_child( rightChildLeft );
    rightChildOp->set_allocated_right_child( rightChildRight );
    rightChild->set_allocated_node_operator( rightChildOp );

    rootOp->set_allocated_left_child( leftChild );
    rootOp->set_allocated_right_child( rightChild );
    root->set_allocated_node_operator( rootOp );

    message->set_allocated_condition_tree( root );

    ASSERT_NO_FATAL_FAILURE( sendMessageToReceiver( mReceiverCollectionSchemeList, protoCollectionSchemesMsg ) );

    ASSERT_TRUE( mReceivedCollectionSchemeList->build() );
    ASSERT_EQ( mReceivedCollectionSchemeList->getCollectionSchemes().size(), 1 );
    auto collectionSchemeTest = mReceivedCollectionSchemeList->getCollectionSchemes().at( 0 );

    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().size(), 5 );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).nodeType,
               ExpressionNodeType::OPERATOR_EQUAL ); // assume first node is top root node

    ASSERT_NE( collectionSchemeTest->getAllExpressionNodes().at( 0 ).left, nullptr );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).left->function.windowFunction,
               WindowFunction::LAST_FIXED_WINDOW_AVG );
    auto leftGeneratedSignalID = collectionSchemeTest->getAllExpressionNodes().at( 0 ).left->signalID;
    ASSERT_EQ( leftGeneratedSignalID & INTERNAL_SIGNAL_ID_BITMASK,
               INTERNAL_SIGNAL_ID_BITMASK ); // check its an internal generated ID

    ASSERT_NE( collectionSchemeTest->getAllExpressionNodes().at( 0 ).right, nullptr );
    ASSERT_NE( collectionSchemeTest->getAllExpressionNodes().at( 0 ).right->left, nullptr );
    ASSERT_NE( collectionSchemeTest->getAllExpressionNodes().at( 0 ).right->right, nullptr );

    auto rightLeftGeneratedSignalId = collectionSchemeTest->getAllExpressionNodes().at( 0 ).right->left->signalID;
    auto rightRightGeneratedSignalId = collectionSchemeTest->getAllExpressionNodes().at( 0 ).right->right->signalID;
    ASSERT_EQ( rightLeftGeneratedSignalId & INTERNAL_SIGNAL_ID_BITMASK,
               INTERNAL_SIGNAL_ID_BITMASK ); // check its an internal generated ID
    ASSERT_EQ( rightRightGeneratedSignalId & INTERNAL_SIGNAL_ID_BITMASK,
               INTERNAL_SIGNAL_ID_BITMASK ); // check its an internal generated ID

    ASSERT_NE( leftGeneratedSignalID, rightLeftGeneratedSignalId );
    ASSERT_EQ( leftGeneratedSignalID, rightRightGeneratedSignalId );
}

TEST_F( SchemaTest, SchemaCollectionWithSamePartialSignal )
{
    Schemas::CollectionSchemesMsg::CollectionSchemes protoCollectionSchemesMsg;

    auto protoCollectionScheme1 = protoCollectionSchemesMsg.add_collection_schemes();
    protoCollectionScheme1->set_campaign_sync_id( "campaign1" );
    protoCollectionScheme1->set_decoder_manifest_sync_id( "dm1" );
    protoCollectionScheme1->set_start_time_ms_epoch( 0 );
    protoCollectionScheme1->set_expiry_time_ms_epoch( 9262144816000 );

    Schemas::CollectionSchemesMsg::SignalInformation *signal1 = protoCollectionScheme1->add_signal_information();
    signal1->set_signal_id( 200008 );
    signal1->set_sample_buffer_size( 100 );
    signal1->set_minimum_sample_period_ms( 1000 );
    signal1->set_fixed_window_period_ms( 1000 );
    signal1->set_condition_only_signal( false );

    auto *signalPath1 = new Schemas::CommonTypesMsg::SignalPath();
    signalPath1->add_signal_path( 34574325 );
    signalPath1->add_signal_path( 5 );
    signalPath1->add_signal_path( 0 );
    signalPath1->add_signal_path( 42 );

    signal1->set_allocated_signal_path( signalPath1 );

    // Add another campaign with exactly the same config
    auto protoCollectionScheme2 = protoCollectionSchemesMsg.add_collection_schemes();
    protoCollectionScheme2->CopyFrom( *protoCollectionScheme1 );
    protoCollectionScheme2->set_campaign_sync_id( "campaign2" );

    ASSERT_NO_FATAL_FAILURE( sendMessageToReceiver( mReceiverCollectionSchemeList, protoCollectionSchemesMsg ) );

    ASSERT_TRUE( mReceivedCollectionSchemeList->build() );
    ASSERT_EQ( mReceivedCollectionSchemeList->getCollectionSchemes().size(), 2 );
    auto collectionScheme1 = mReceivedCollectionSchemeList->getCollectionSchemes().at( 0 );
    auto collectionScheme2 = mReceivedCollectionSchemeList->getCollectionSchemes().at( 1 );

    ASSERT_EQ( collectionScheme1->getCollectSignals().size(), 1 );
    ASSERT_EQ( collectionScheme2->getCollectSignals().size(), 1 );

    // check its an internal generated ID
    auto signalId1 = collectionScheme1->getCollectSignals().at( 0 ).signalID;
    auto signalId2 = collectionScheme2->getCollectSignals().at( 0 ).signalID;
    ASSERT_NE( signalId1 & INTERNAL_SIGNAL_ID_BITMASK, 0 );
    ASSERT_NE( signalId2 & INTERNAL_SIGNAL_ID_BITMASK, 0 );
    // Internal IDs should be reused across collection schemes if they refer to the same partial signal
    ASSERT_EQ( signalId1, signalId2 );
}

TEST_F( SchemaTest, SchemaCollectionWithDifferentWayToSpecifySignalIDInExpression )
{
    Schemas::CollectionSchemesMsg::CollectionSchemes protoCollectionSchemesMsg;
    auto collectionSchemeTestMessage = protoCollectionSchemesMsg.add_collection_schemes();
    collectionSchemeTestMessage->set_campaign_sync_id( "arn:aws:iam::2.52543243543:user/Development/complexdata/*" );
    collectionSchemeTestMessage->set_decoder_manifest_sync_id( "model_manifest_67" );
    collectionSchemeTestMessage->set_start_time_ms_epoch( 0 );
    collectionSchemeTestMessage->set_expiry_time_ms_epoch( 9262144816000 );

    // Create an Event/Condition Based CollectionScheme
    Schemas::CollectionSchemesMsg::ConditionBasedCollectionScheme *message =
        collectionSchemeTestMessage->mutable_condition_based_collection_scheme();
    message->set_condition_minimum_interval_ms( 650 );
    message->set_condition_language_version( 1000 );
    message->set_condition_trigger_mode(
        Schemas::CollectionSchemesMsg::ConditionBasedCollectionScheme_ConditionTriggerMode_TRIGGER_ALWAYS );

    // Build a AST Tree.
    // Root: Equal
    // Left Child: average Window of signal 1 (specified in primitive_type_in_signal) * signal 2 (specified in
    // primitive_type_in_signal) Right Child: average Window of signal 3 (specified in signal_id) + signal 4 (specified
    // in signal_id)
    auto *root = new Schemas::CommonTypesMsg::ConditionNode();
    auto *rootOp = new Schemas::CommonTypesMsg::ConditionNode_NodeOperator();
    rootOp->set_operator_( Schemas::CommonTypesMsg::ConditionNode_NodeOperator_Operator_COMPARE_EQUAL );

    auto *leftChild = new Schemas::CommonTypesMsg::ConditionNode();
    auto *leftChildOp = new Schemas::CommonTypesMsg::ConditionNode_NodeOperator();
    leftChildOp->set_operator_( Schemas::CommonTypesMsg::ConditionNode_NodeOperator_Operator_ARITHMETIC_MULTIPLY );

    auto *leftChildLeft = new Schemas::CommonTypesMsg::ConditionNode();
    auto *leftChildLeftFunction = new Schemas::CommonTypesMsg::ConditionNode_NodeFunction();
    auto *leftChildLeftAvgWindow = new Schemas::CommonTypesMsg::ConditionNode_NodeFunction_WindowFunction();
    auto *leftChildLeftPrimitivePrimitiveType = new Schemas::CommonTypesMsg::PrimitiveTypeInComplexSignal();
    leftChildLeftPrimitivePrimitiveType->set_signal_id( 1 );
    leftChildLeftAvgWindow->set_window_type(
        Schemas::CommonTypesMsg::ConditionNode_NodeFunction_WindowFunction_WindowType_LAST_WINDOW_AVG );
    leftChildLeftAvgWindow->set_allocated_primitive_type_in_signal( leftChildLeftPrimitivePrimitiveType );
    leftChildLeftFunction->set_allocated_window_function( leftChildLeftAvgWindow );
    leftChildLeft->set_allocated_node_function( leftChildLeftFunction );

    auto *leftChildRight = new Schemas::CommonTypesMsg::ConditionNode();
    auto *leftChildRightPrimitivePrimitiveType = new Schemas::CommonTypesMsg::PrimitiveTypeInComplexSignal();
    leftChildRightPrimitivePrimitiveType->set_signal_id( 2 );
    leftChildRight->set_allocated_node_primitive_type_in_signal( leftChildRightPrimitivePrimitiveType );

    leftChildOp->set_allocated_left_child( leftChildLeft );
    leftChildOp->set_allocated_right_child( leftChildRight );
    leftChild->set_allocated_node_operator( leftChildOp );

    auto *rightChild = new Schemas::CommonTypesMsg::ConditionNode();
    auto *rightChildOp = new Schemas::CommonTypesMsg::ConditionNode_NodeOperator();
    rightChildOp->set_operator_( Schemas::CommonTypesMsg::ConditionNode_NodeOperator_Operator_ARITHMETIC_PLUS );

    auto *rightChildLeft = new Schemas::CommonTypesMsg::ConditionNode();
    auto *rightChildLeftFunction = new Schemas::CommonTypesMsg::ConditionNode_NodeFunction();
    auto *rightChildLeftAvgWindow = new Schemas::CommonTypesMsg::ConditionNode_NodeFunction_WindowFunction();

    rightChildLeftAvgWindow->set_signal_id( 3 );
    rightChildLeftFunction->set_allocated_window_function( rightChildLeftAvgWindow );
    rightChildLeft->set_allocated_node_function( rightChildLeftFunction );

    auto *rightChildRight = new Schemas::CommonTypesMsg::ConditionNode();
    auto *rightChildRightPrimitivePrimitiveType = new Schemas::CommonTypesMsg::PrimitiveTypeInComplexSignal();
    rightChildRightPrimitivePrimitiveType->set_signal_id( 4 );

    rightChildRight->set_allocated_node_primitive_type_in_signal( rightChildRightPrimitivePrimitiveType );

    rightChildOp->set_allocated_left_child( rightChildLeft );
    rightChildOp->set_allocated_right_child( rightChildRight );
    rightChild->set_allocated_node_operator( rightChildOp );

    rootOp->set_allocated_left_child( leftChild );
    rootOp->set_allocated_right_child( rightChild );
    root->set_allocated_node_operator( rootOp );

    message->set_allocated_condition_tree( root );

    ASSERT_NO_FATAL_FAILURE( sendMessageToReceiver( mReceiverCollectionSchemeList, protoCollectionSchemesMsg ) );

    ASSERT_TRUE( mReceivedCollectionSchemeList->build() );
    ASSERT_EQ( mReceivedCollectionSchemeList->getCollectionSchemes().size(), 1 );
    auto collectionSchemeTest = mReceivedCollectionSchemeList->getCollectionSchemes().at( 0 );

    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().size(), 7 );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).nodeType,
               ExpressionNodeType::OPERATOR_EQUAL ); // assume first node is top root node

    ASSERT_NE( collectionSchemeTest->getAllExpressionNodes().at( 0 ).left, nullptr );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).left->left->function.windowFunction,
               WindowFunction::LAST_FIXED_WINDOW_AVG );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).left->left->signalID, 1 );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).left->right->signalID, 2 );

    ASSERT_NE( collectionSchemeTest->getAllExpressionNodes().at( 0 ).right, nullptr );
    ASSERT_NE( collectionSchemeTest->getAllExpressionNodes().at( 0 ).right->left, nullptr );
    ASSERT_NE( collectionSchemeTest->getAllExpressionNodes().at( 0 ).right->right, nullptr );

    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).right->left->signalID, 3 );
    ASSERT_EQ( collectionSchemeTest->getAllExpressionNodes().at( 0 ).right->right->signalID, 4 );
}

TEST_F( SchemaTest, CollectionSchemeComplexHeartbeat )
{
    Schemas::CollectionSchemesMsg::CollectionSchemes protoCollectionSchemesMsg;
    auto collectionSchemeTestMessage = protoCollectionSchemesMsg.add_collection_schemes();
    collectionSchemeTestMessage->set_campaign_sync_id( "arn:aws:iam::2.23606797749:user/Development/product_1234/*" );
    collectionSchemeTestMessage->set_decoder_manifest_sync_id( "model_manifest_12" );
    collectionSchemeTestMessage->set_start_time_ms_epoch( 1621448160000 );
    collectionSchemeTestMessage->set_expiry_time_ms_epoch( 2621448160000 );

    // Create a Time_based_collection_scheme
    Schemas::CollectionSchemesMsg::TimeBasedCollectionScheme *message1 =
        collectionSchemeTestMessage->mutable_time_based_collection_scheme();
    message1->set_time_based_collection_scheme_period_ms( 5000 );

    collectionSchemeTestMessage->set_after_duration_ms( 0 );
    collectionSchemeTestMessage->set_include_active_dtcs( true );
    collectionSchemeTestMessage->set_persist_all_collected_data( true );
    collectionSchemeTestMessage->set_compress_collected_data( true );
    collectionSchemeTestMessage->set_priority( 9 );

    // Add two normal and one partial signal to collect
    Schemas::CollectionSchemesMsg::SignalInformation *signal1 = collectionSchemeTestMessage->add_signal_information();
    signal1->set_signal_id( 0 );
    signal1->set_sample_buffer_size( 100 );
    signal1->set_minimum_sample_period_ms( 1000 );
    signal1->set_fixed_window_period_ms( 1000 );
    signal1->set_condition_only_signal( false );

    Schemas::CollectionSchemesMsg::SignalInformation *signal2 = collectionSchemeTestMessage->add_signal_information();
    signal2->set_signal_id( 999 );
    signal2->set_sample_buffer_size( 500 );
    signal2->set_minimum_sample_period_ms( 1000 );
    signal2->set_fixed_window_period_ms( 1000 );
    signal2->set_condition_only_signal( false );

    // Add partial signal to collect
    Schemas::CollectionSchemesMsg::SignalInformation *signal3 = collectionSchemeTestMessage->add_signal_information();
    signal3->set_signal_id( 999 );
    signal3->set_sample_buffer_size( 800 );
    signal3->set_minimum_sample_period_ms( 1000 );
    signal3->set_fixed_window_period_ms( 1000 );
    signal3->set_condition_only_signal( false );

    auto *path1 = new Schemas::CommonTypesMsg::SignalPath();

    path1->add_signal_path( 34574325 );
    path1->add_signal_path( 5 );
    signal3->set_allocated_signal_path( path1 );

    ASSERT_NO_FATAL_FAILURE( sendMessageToReceiver( mReceiverCollectionSchemeList, protoCollectionSchemesMsg ) );

    ASSERT_TRUE( mReceivedCollectionSchemeList->build() );
    ASSERT_EQ( mReceivedCollectionSchemeList->getCollectionSchemes().size(), 1 );
    auto collectionSchemeTest = mReceivedCollectionSchemeList->getCollectionSchemes().at( 0 );

    ASSERT_EQ( collectionSchemeTest->getCollectSignals().size(), 3 );
    ASSERT_EQ( collectionSchemeTest->getCollectSignals().at( 0 ).signalID, 0 );
    ASSERT_EQ( collectionSchemeTest->getCollectSignals().at( 1 ).signalID, 999 );
    ASSERT_NE( collectionSchemeTest->getCollectSignals().at( 2 ).signalID, 999 );
    ASSERT_EQ( collectionSchemeTest->getCollectSignals().at( 2 ).signalID & INTERNAL_SIGNAL_ID_BITMASK,
               INTERNAL_SIGNAL_ID_BITMASK );

    auto plt = collectionSchemeTest->getPartialSignalIdToSignalPathLookupTable();
    ASSERT_NE( plt, collectionSchemeTest->INVALID_PARTIAL_SIGNAL_ID_LOOKUP );
}

TEST_F( SchemaTest, DecoderManifestIngestionComplexSignals )
{
    // Create a Decoder manifest protocol buffer and pack it with the data
    Schemas::DecoderManifestMsg::DecoderManifest protoDM;

    protoDM.set_sync_id( "arn:aws:iam::123456789012:user/Development/product_1234/*" );

    Schemas::DecoderManifestMsg::ComplexType *protoComplexType1 = protoDM.add_complex_types();
    auto *primitiveData = new Schemas::DecoderManifestMsg::PrimitiveData();

    primitiveData->set_primitive_type( Schemas::DecoderManifestMsg::UINT64 );
    protoComplexType1->set_type_id( 10 );
    protoComplexType1->set_allocated_primitive_data( primitiveData );

    Schemas::DecoderManifestMsg::ComplexType *protoComplexTypeForPrimitive2 = protoDM.add_complex_types();
    auto *primitiveData2 = new Schemas::DecoderManifestMsg::PrimitiveData();
    primitiveData2->set_primitive_type( Schemas::DecoderManifestMsg::BOOL );
    protoComplexTypeForPrimitive2->set_type_id( 11 );
    protoComplexTypeForPrimitive2->set_allocated_primitive_data( primitiveData2 );

    Schemas::DecoderManifestMsg::ComplexType *protoComplexTypeForPrimitive3 = protoDM.add_complex_types();
    auto *primitiveData3 = new Schemas::DecoderManifestMsg::PrimitiveData();
    primitiveData3->set_primitive_type( Schemas::DecoderManifestMsg::UINT8 );
    protoComplexTypeForPrimitive3->set_type_id( 12 );
    protoComplexTypeForPrimitive3->set_allocated_primitive_data( primitiveData3 );

    Schemas::DecoderManifestMsg::ComplexType *protoComplexTypeForPrimitive4 = protoDM.add_complex_types();
    auto *primitiveData4 = new Schemas::DecoderManifestMsg::PrimitiveData();
    primitiveData4->set_primitive_type( Schemas::DecoderManifestMsg::UINT16 );
    protoComplexTypeForPrimitive4->set_type_id( 13 );
    protoComplexTypeForPrimitive4->set_allocated_primitive_data( primitiveData4 );

    Schemas::DecoderManifestMsg::ComplexType *protoComplexTypeForPrimitive5 = protoDM.add_complex_types();
    auto *primitiveData5 = new Schemas::DecoderManifestMsg::PrimitiveData();
    primitiveData5->set_primitive_type( Schemas::DecoderManifestMsg::UINT32 );
    protoComplexTypeForPrimitive5->set_type_id( 14 );
    protoComplexTypeForPrimitive5->set_allocated_primitive_data( primitiveData5 );

    Schemas::DecoderManifestMsg::ComplexType *protoComplexTypeForPrimitive6 = protoDM.add_complex_types();
    auto *primitiveData6 = new Schemas::DecoderManifestMsg::PrimitiveData();
    primitiveData6->set_primitive_type( Schemas::DecoderManifestMsg::INT8 );
    protoComplexTypeForPrimitive6->set_type_id( 15 );
    protoComplexTypeForPrimitive6->set_allocated_primitive_data( primitiveData6 );

    Schemas::DecoderManifestMsg::ComplexType *protoComplexTypeForPrimitive7 = protoDM.add_complex_types();
    auto *primitiveData7 = new Schemas::DecoderManifestMsg::PrimitiveData();
    primitiveData7->set_primitive_type( Schemas::DecoderManifestMsg::INT16 );
    protoComplexTypeForPrimitive7->set_type_id( 16 );
    protoComplexTypeForPrimitive7->set_allocated_primitive_data( primitiveData7 );

    Schemas::DecoderManifestMsg::ComplexType *protoComplexTypeForPrimitive8 = protoDM.add_complex_types();
    auto *primitiveData8 = new Schemas::DecoderManifestMsg::PrimitiveData();
    primitiveData8->set_primitive_type( Schemas::DecoderManifestMsg::INT32 );
    protoComplexTypeForPrimitive8->set_type_id( 17 );
    protoComplexTypeForPrimitive8->set_allocated_primitive_data( primitiveData8 );

    Schemas::DecoderManifestMsg::ComplexType *protoComplexTypeForPrimitive9 = protoDM.add_complex_types();
    auto *primitiveData9 = new Schemas::DecoderManifestMsg::PrimitiveData();
    primitiveData9->set_primitive_type( Schemas::DecoderManifestMsg::INT64 );
    protoComplexTypeForPrimitive9->set_type_id( 18 );
    protoComplexTypeForPrimitive9->set_allocated_primitive_data( primitiveData9 );

    Schemas::DecoderManifestMsg::ComplexType *protoComplexTypeForPrimitive10 = protoDM.add_complex_types();
    auto *primitiveData10 = new Schemas::DecoderManifestMsg::PrimitiveData();
    primitiveData10->set_primitive_type( Schemas::DecoderManifestMsg::FLOAT32 );
    protoComplexTypeForPrimitive10->set_type_id( 19 );
    protoComplexTypeForPrimitive10->set_allocated_primitive_data( primitiveData10 );

    Schemas::DecoderManifestMsg::ComplexType *protoComplexTypeForPrimitive11 = protoDM.add_complex_types();
    auto *primitiveData11 = new Schemas::DecoderManifestMsg::PrimitiveData();
    primitiveData11->set_primitive_type( Schemas::DecoderManifestMsg::FLOAT64 );
    protoComplexTypeForPrimitive11->set_type_id( 21 );
    protoComplexTypeForPrimitive11->set_allocated_primitive_data( primitiveData11 );

    Schemas::DecoderManifestMsg::ComplexType *protoComplexType2 = protoDM.add_complex_types();
    auto *complexStruct = new Schemas::DecoderManifestMsg::ComplexStruct();

    auto *structMember1 = complexStruct->add_members();
    structMember1->set_type_id( 10 );

    auto *structMember2 = complexStruct->add_members();
    structMember2->set_type_id( 30 );

    protoComplexType2->set_type_id( 20 );
    protoComplexType2->set_allocated_struct_( complexStruct );

    Schemas::DecoderManifestMsg::ComplexType *protoComplexType3 = protoDM.add_complex_types();
    auto *complexArray = new Schemas::DecoderManifestMsg::ComplexArray();

    complexArray->set_type_id( 10 );
    complexArray->set_size( 10000 );
    protoComplexType3->set_type_id( 30 );
    protoComplexType3->set_allocated_array( complexArray );

    Schemas::DecoderManifestMsg::ComplexSignal *protoComplexSignal = protoDM.add_complex_signals();

    protoComplexSignal->set_signal_id( 123 );
    protoComplexSignal->set_interface_id( "ros2" );
    protoComplexSignal->set_message_id( "/topic/for/ROS:/vehicle/msgs/test.msg" );
    protoComplexSignal->set_root_type_id( 20 );

    ASSERT_NO_FATAL_FAILURE( sendMessageToReceiver( mReceiverDecoderManifest, protoDM ) );

    ASSERT_TRUE( mReceivedDecoderManifest->getComplexSignalDecoderFormat( 123 ).mInterfaceId.empty() );

    ASSERT_EQ( mReceivedDecoderManifest->getComplexDataType( 10 ).type(), typeid( InvalidComplexVariant ) );

    ASSERT_TRUE( mReceivedDecoderManifest->build() );
    ASSERT_TRUE( mReceivedDecoderManifest->isReady() );

    ASSERT_EQ( mReceivedDecoderManifest->getNetworkProtocol( 123 ), VehicleDataSourceProtocol::COMPLEX_DATA );

    auto complexDecoder = mReceivedDecoderManifest->getComplexSignalDecoderFormat( 123 );

    ASSERT_EQ( complexDecoder.mInterfaceId, "ros2" );
    ASSERT_EQ( complexDecoder.mMessageId, "/topic/for/ROS:/vehicle/msgs/test.msg" );
    ASSERT_EQ( complexDecoder.mRootTypeId, 20 );

    auto resultRoot = mReceivedDecoderManifest->getComplexDataType( 20 );
    ASSERT_EQ( resultRoot.type(), typeid( ComplexStruct ) );
    auto resultStruct = boost::get<ComplexStruct>( resultRoot );
    ASSERT_EQ( resultStruct.mOrderedTypeIds.size(), 2 );
    ASSERT_EQ( resultStruct.mOrderedTypeIds[0], 10 );
    ASSERT_EQ( resultStruct.mOrderedTypeIds[1], 30 );

    auto resultMember1 = mReceivedDecoderManifest->getComplexDataType( 10 );
    ASSERT_EQ( resultMember1.type(), typeid( PrimitiveData ) );
    auto resultPrimitive = boost::get<PrimitiveData>( resultMember1 );
    ASSERT_EQ( resultPrimitive.mPrimitiveType, SignalType::UINT64 );
    ASSERT_EQ( resultPrimitive.mScaling, 1.0 );
    ASSERT_EQ( resultPrimitive.mOffset, 0.0 );

    auto resultMember2 = mReceivedDecoderManifest->getComplexDataType( 30 );
    ASSERT_EQ( resultMember2.type(), typeid( ComplexArray ) );
    auto resultArray = boost::get<ComplexArray>( resultMember2 );
    ASSERT_EQ( resultArray.mSize, 10000 );
    ASSERT_EQ( resultArray.mRepeatedTypeId, 10 );

    ASSERT_EQ( boost::get<PrimitiveData>( mReceivedDecoderManifest->getComplexDataType( 11 ) ).mPrimitiveType,
               SignalType::BOOLEAN );
    ASSERT_EQ( boost::get<PrimitiveData>( mReceivedDecoderManifest->getComplexDataType( 12 ) ).mPrimitiveType,
               SignalType::UINT8 );
    ASSERT_EQ( boost::get<PrimitiveData>( mReceivedDecoderManifest->getComplexDataType( 13 ) ).mPrimitiveType,
               SignalType::UINT16 );
    ASSERT_EQ( boost::get<PrimitiveData>( mReceivedDecoderManifest->getComplexDataType( 14 ) ).mPrimitiveType,
               SignalType::UINT32 );
    ASSERT_EQ( boost::get<PrimitiveData>( mReceivedDecoderManifest->getComplexDataType( 15 ) ).mPrimitiveType,
               SignalType::INT8 );
    ASSERT_EQ( boost::get<PrimitiveData>( mReceivedDecoderManifest->getComplexDataType( 16 ) ).mPrimitiveType,
               SignalType::INT16 );
    ASSERT_EQ( boost::get<PrimitiveData>( mReceivedDecoderManifest->getComplexDataType( 17 ) ).mPrimitiveType,
               SignalType::INT32 );
    ASSERT_EQ( boost::get<PrimitiveData>( mReceivedDecoderManifest->getComplexDataType( 18 ) ).mPrimitiveType,
               SignalType::INT64 );
    ASSERT_EQ( boost::get<PrimitiveData>( mReceivedDecoderManifest->getComplexDataType( 19 ) ).mPrimitiveType,
               SignalType::FLOAT );
    ASSERT_EQ( boost::get<PrimitiveData>( mReceivedDecoderManifest->getComplexDataType( 21 ) ).mPrimitiveType,
               SignalType::DOUBLE );
}

TEST_F( SchemaTest, DecoderManifestWrong )
{
    // Create a Decoder manifest protocol buffer and pack it with the data
    Schemas::DecoderManifestMsg::DecoderManifest protoDM;

    protoDM.set_sync_id( "arn:aws:iam::123456789012:user/Development/product_1234/*" );

    Schemas::DecoderManifestMsg::ComplexType *protoComplexType1 = protoDM.add_complex_types();
    auto *primitiveData = new Schemas::DecoderManifestMsg::PrimitiveData();

    primitiveData->set_primitive_type( Schemas::DecoderManifestMsg::UINT64 );
    protoComplexType1->set_type_id( 10 );
    protoComplexType1->set_allocated_primitive_data( primitiveData );

    Schemas::DecoderManifestMsg::ComplexType *protoComplexType2 = protoDM.add_complex_types();
    auto *primitiveData2 = new Schemas::DecoderManifestMsg::PrimitiveData();

    // same id but different type. Should give a warning and ignore the second one
    primitiveData2->set_primitive_type( Schemas::DecoderManifestMsg::UINT32 );
    protoComplexType2->set_type_id( 10 );
    protoComplexType2->set_allocated_primitive_data( primitiveData2 );

    Schemas::DecoderManifestMsg::ComplexType *protoComplexType3 = protoDM.add_complex_types();
    auto *primitiveData3 = new Schemas::DecoderManifestMsg::PrimitiveData();

    primitiveData3->set_primitive_type(
        static_cast<Schemas::DecoderManifestMsg::PrimitiveType>( 0xBEEF ) ); // invalid enum
    protoComplexType3->set_type_id( 20 );
    protoComplexType3->set_allocated_primitive_data( primitiveData3 );

    Schemas::DecoderManifestMsg::ComplexSignal *protoComplexSignal = protoDM.add_complex_signals();

    protoComplexSignal->set_signal_id( 123 );
    protoComplexSignal->set_interface_id( "ros2" );
    protoComplexSignal->set_message_id( "/topic/for/ROS:/vehicle/msgs/test.msg" );
    protoComplexSignal->set_root_type_id( 10 );

    Schemas::DecoderManifestMsg::ComplexSignal *protoComplexSignal2 = protoDM.add_complex_signals();
    protoComplexSignal2->set_signal_id( 456 );
    // Empty interface id should result in a warning
    protoComplexSignal2->set_interface_id( "" );
    protoComplexSignal2->set_message_id( "/topic/for/ROS:/vehicle/msgs/test2.msg" );
    protoComplexSignal2->set_root_type_id( 10 );

    ASSERT_NO_FATAL_FAILURE( sendMessageToReceiver( mReceiverDecoderManifest, protoDM ) );

    ASSERT_TRUE( mReceivedDecoderManifest->build() );
    ASSERT_TRUE( mReceivedDecoderManifest->isReady() );

    auto resultMember1 = mReceivedDecoderManifest->getComplexDataType( 10 );
    ASSERT_EQ( resultMember1.type(), typeid( PrimitiveData ) );
    auto resultPrimitive = boost::get<PrimitiveData>( resultMember1 );
    ASSERT_EQ( resultPrimitive.mPrimitiveType, SignalType::UINT64 );
    ASSERT_EQ( resultPrimitive.mScaling, 1.0 );
    ASSERT_EQ( resultPrimitive.mOffset, 0.0 );

    // unkown types default to UINT8
    ASSERT_EQ( boost::get<PrimitiveData>( mReceivedDecoderManifest->getComplexDataType( 20 ) ).mPrimitiveType,
               SignalType::UINT8 );

    ASSERT_FALSE( mReceivedDecoderManifest->getComplexSignalDecoderFormat( 123 ).mInterfaceId.empty() );
    ASSERT_FALSE( mReceivedDecoderManifest->getComplexSignalDecoderFormat( 123 ).mMessageId.empty() );

    // Signal with empty interface ID should be ignored an not be set at all
    ASSERT_TRUE( mReceivedDecoderManifest->getComplexSignalDecoderFormat( 456 ).mInterfaceId.empty() );
    ASSERT_TRUE( mReceivedDecoderManifest->getComplexSignalDecoderFormat( 456 ).mMessageId.empty() );
}

TEST_F( SchemaTest, DecoderManifestIngestionComplexStringAsArray )
{
    // Create a Decoder manifest protocol buffer and pack it with the data
    Schemas::DecoderManifestMsg::DecoderManifest protoDM;

    protoDM.set_sync_id( "arn:aws:iam::123456789012:user/Development/product_1234/*" );

    Schemas::DecoderManifestMsg::ComplexType *protoComplexType1 = protoDM.add_complex_types();
    auto *stringDataUtf8 = new Schemas::DecoderManifestMsg::StringData();

    stringDataUtf8->set_size( 55 );
    stringDataUtf8->set_encoding( Schemas::DecoderManifestMsg::StringEncoding::UTF_8 );
    protoComplexType1->set_type_id( 100 );
    protoComplexType1->set_allocated_string_data( stringDataUtf8 );

    Schemas::DecoderManifestMsg::ComplexType *protoComplexType2 = protoDM.add_complex_types();
    auto *stringDataUtf16 = new Schemas::DecoderManifestMsg::StringData();

    stringDataUtf16->set_size( 77 );
    stringDataUtf16->set_encoding( Schemas::DecoderManifestMsg::StringEncoding::UTF_16 );
    protoComplexType2->set_type_id( 200 );
    protoComplexType2->set_allocated_string_data( stringDataUtf16 );

    Schemas::DecoderManifestMsg::ComplexType *protoComplexType3 = protoDM.add_complex_types();
    auto *complexStruct = new Schemas::DecoderManifestMsg::ComplexStruct();

    auto *structMember1 = complexStruct->add_members();
    structMember1->set_type_id( 100 );

    auto *structMember2 = complexStruct->add_members();
    structMember2->set_type_id( 200 );

    protoComplexType3->set_type_id( 20 );
    protoComplexType3->set_allocated_struct_( complexStruct );

    Schemas::DecoderManifestMsg::ComplexSignal *protoComplexSignal = protoDM.add_complex_signals();

    protoComplexSignal->set_signal_id( 123 );
    protoComplexSignal->set_interface_id( "ros2" );
    protoComplexSignal->set_message_id( "/topic/for/ROS:/vehicle/msgs/test.msg" );
    protoComplexSignal->set_root_type_id( 20 );

    ASSERT_NO_FATAL_FAILURE( sendMessageToReceiver( mReceiverDecoderManifest, protoDM ) );

    ASSERT_TRUE( mReceivedDecoderManifest->getComplexSignalDecoderFormat( 123 ).mInterfaceId.empty() );

    ASSERT_TRUE( mReceivedDecoderManifest->build() );
    ASSERT_TRUE( mReceivedDecoderManifest->isReady() );

    ASSERT_EQ( mReceivedDecoderManifest->getNetworkProtocol( 123 ), VehicleDataSourceProtocol::COMPLEX_DATA );

    auto resultMember2 = mReceivedDecoderManifest->getComplexDataType( 100 );
    ASSERT_EQ( resultMember2.type(), typeid( ComplexArray ) );
    auto resultArray = boost::get<ComplexArray>( resultMember2 );
    ASSERT_EQ( resultArray.mSize, 55 );
    ASSERT_EQ( boost::get<PrimitiveData>( mReceivedDecoderManifest->getComplexDataType( resultArray.mRepeatedTypeId ) )
                   .mPrimitiveType,
               SignalType::UINT8 );

    auto resultMember3 = mReceivedDecoderManifest->getComplexDataType( 200 );
    ASSERT_EQ( resultMember3.type(), typeid( ComplexArray ) );
    auto resultArray2 = boost::get<ComplexArray>( resultMember3 );
    ASSERT_EQ( resultArray2.mSize, 77 );
    ASSERT_EQ( boost::get<PrimitiveData>( mReceivedDecoderManifest->getComplexDataType( resultArray2.mRepeatedTypeId ) )
                   .mPrimitiveType,
               SignalType::UINT32 );
}
#endif

} // namespace IoTFleetWise
} // namespace Aws

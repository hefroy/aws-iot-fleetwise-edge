// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "CollectionSchemeManagerMock.h"
#include "CollectionSchemeManagerTest.h" // IWYU pragma: associated
#include "Testing.h"
#include "aws/iotfleetwise/CANInterfaceIDTranslator.h"
#include "aws/iotfleetwise/CheckinSender.h"
#include "aws/iotfleetwise/Clock.h"
#include "aws/iotfleetwise/ClockHandler.h"
#include "aws/iotfleetwise/IDecoderDictionary.h"
#include "aws/iotfleetwise/OBDDataTypes.h"
#include "aws/iotfleetwise/TimeTypes.h"
#include <gtest/gtest.h>
#include <map>
#include <unordered_set>

#ifdef FWE_FEATURE_VISION_SYSTEM_DATA
#include <boost/variant.hpp>
#endif
#ifdef FWE_FEATURE_LAST_KNOWN_STATE
#include "aws/iotfleetwise/LastKnownStateTypes.h"
#endif

namespace Aws
{
namespace IoTFleetWise
{

/** @brief
 * This test aims to test CollectionScheme Manager's Decoder Dictionary Extractor functionality
 * to extract the correct decoder dictionary from the decoder manifest and collectionScheme
 * step1:
 * Build the collectionScheme list: list1 containing two polices
 * Build the DM: DM1
 * Step2:
 * run decoderDictionaryExtractor with the input of list1 and DM1
 * Step3:
 * Exam the generated decoder dictionary
 *
 * Here's the Signal schema for this testing.
 *
 * CAN Channel 1
 * NodeID 10
 * CAN Frame0
 *     ID:             0x100
 *     collectType:    DECODE
 *     signalsID:      0, 1, 2, 3, 4, 5, 6, 7
 *
 *     ID:             0x110
 *     collectType:    DECODE
 *     signalsID:      8
 *
 * CAN Channel 2
 * NodeID 20
 * CAN Frame
 *     ID:             0x200
 *     collectType:    DECODE
 *     signalsID:      10, 17
 *
 * OBD-II (refer to J1979 DA specification)
 * PID 0x14 O2 Sensor
 *     num of bytes in response: 4
 *     Signal: O2 Sensor Voltage
 *          signalID: 0x1000
 *          start byte: 0
 *          num of byte: 2
 *          scaling: 0.0125
 *          offset: -40.0
 *     Signal: O2 Sensor SHRFT
 *          signalID: 0x1001
 *          start byte: 2
 *          num of byte: 2
 *          scaling: 0.0125
 *          offset: -40.0
 *
 * PID 0x70 Boost Pressure Control
 *     Signal: Boost Pressure A Control Status
 *          signalID: 0x1005
 *          start byte: 9
 *          num of byte: 1
 *          bit right shift: 0
 *          bit mask length: 2
 *     Signal: Boost Pressure B Control Status
 *          signalID: 0x1006
 *          start byte: 9
 *          num of byte: 1
 *          bit right shift: 2
 *          bit mask length: 2
 *
 * Custom decoded signal 0
 *   SignalID: 0x2000
 *   InterfaceID: 30
 *   Decoder: custom-decoder-0
 * Custom decoded signal 1
 *   SignalID: 0x2001
 *   InterfaceID: 31
 *   Decoder: custom-decoder-1
 *
 * CollectionScheme1 is interested in signal 0 ~ 8 and OBD Signals
 * CollectionScheme2 is interested in signal 10 ~ 17
 * CollectionScheme3 is interested in signal 25 at Node 20
 * CollectionScheme1 and CollectionScheme2 will be enabled at beginning. Later on CollectionScheme3 will be enabled.
 */
TEST( DecoderDictionaryExtractorTest, DecoderDictionaryExtractorTest )
{
    CANInterfaceIDTranslator canIDTranslator;
    canIDTranslator.add( "10" );
    canIDTranslator.add( "20" );
    CollectionSchemeManagerWrapper test( nullptr, canIDTranslator, std::make_shared<CheckinSender>( nullptr ), "DM1" );
    ASSERT_NE( canIDTranslator.getChannelNumericID( "10" ), canIDTranslator.getChannelNumericID( "20" ) );
    std::shared_ptr<const Clock> testClock = ClockHandler::getClock();
    /* mock currTime, and 3 collectionSchemes */
    TimePoint currTime = testClock->timeSinceEpoch();
    Timestamp startTime1 = currTime.systemTimeMs;
    Timestamp stopTime1 = startTime1 + SECOND_TO_MILLISECOND( 5 );
    Timestamp startTime2 = startTime1;
    Timestamp stopTime2 = startTime2 + SECOND_TO_MILLISECOND( 5 );
    Timestamp startTime3 = startTime1 + SECOND_TO_MILLISECOND( 6 );
    Timestamp stopTime3 = startTime3 + SECOND_TO_MILLISECOND( 5 );

    // Map to be used by Decoder Manifest Mock to return getCANFrameAndNodeID( SignalID signalId )
    std::unordered_map<SignalID, std::pair<CANRawFrameID, InterfaceID>> signalToFrameAndNodeID;

    // This is CAN Frame 0x100 at Node 10 decoding format. It's part of decoder manifest
    struct CANMessageFormat canMessageFormat0x100;

    // This is the Signal decoding format vector. It's part of decoder manifest
    // It will be used for decoding CAN Frame 0x100 into eight signals.
    std::vector<CANSignalFormat> signals0 = std::vector<CANSignalFormat>();

    // This signalInfo vector define a list of signals to collect. It's part of the collectionScheme1.
    ICollectionScheme::Signals_t signalInfo1 = ICollectionScheme::Signals_t();

    // Generate 8 signals to be decoded and collected
    for ( int i = 1; i < 9; ++i )
    {
        SignalCollectionInfo signal;
        signal.signalID = i;
        signalInfo1.emplace_back( signal );
        // Map signal 0 ~ 7 to Node ID 10, CAN Frame 0x100
        signalToFrameAndNodeID[signal.signalID] = { 0x100, "10" };

        CANSignalFormat sigFormat;
        sigFormat.mSignalID = i;
        signals0.emplace_back( sigFormat );
    }
    canMessageFormat0x100.mMessageID = 0x100;
    canMessageFormat0x100.mSizeInBytes = 8;
    canMessageFormat0x100.mIsMultiplexed = false;
    canMessageFormat0x100.mSignals = signals0;

    // This is CAN Frame 0x110 at Node 10 decoding format. It's part of decoder manifest
    struct CANMessageFormat canMessageFormat0x110;
    // Signal 9 will be part of CAN Frame 0x110 at Node 10
    SignalCollectionInfo signal9;
    signal9.signalID = 9;
    signalInfo1.emplace_back( signal9 );
    // CAN Frame 0x110 will only be decoded, its raw frame will not be collected
    signalToFrameAndNodeID[signal9.signalID] = { 0x110, "10" };
    canMessageFormat0x110.mMessageID = 0x101;
    canMessageFormat0x110.mSizeInBytes = 8;
    canMessageFormat0x110.mIsMultiplexed = false;
    CANSignalFormat sigFormat;
    sigFormat.mSignalID = 9;
    canMessageFormat0x110.mSignals = { sigFormat };

    // This vector defines a list of signals CollectionScheme2 wants to collect
    // All those signals belong to CAN Frame 0x200
    ICollectionScheme::Signals_t signalInfo2 = ICollectionScheme::Signals_t();
    for ( int i = 10; i < 18; ++i )
    {
        SignalCollectionInfo signal;
        signal.signalID = i;
        signalInfo2.emplace_back( signal );
        signalToFrameAndNodeID[signal.signalID] = { 0x200, "20" };
    }

    // This is CAN Frame 0x200 at Node 20 decoding format. It's part of decoder manifest
    // Note only signal 10 and 17 has decoding rule
    struct CANMessageFormat canMessageFormat0x200;
    std::vector<CANSignalFormat> signals200 = std::vector<CANSignalFormat>();
    CANSignalFormat sigFormat10;
    sigFormat10.mSignalID = 10;
    signals200.emplace_back( sigFormat10 );
    CANSignalFormat sigFormat4;
    CANSignalFormat sigFormat17;
    sigFormat17.mSignalID = 17;
    signals200.emplace_back( sigFormat17 );
    canMessageFormat0x200.mMessageID = 0x200;
    canMessageFormat0x200.mSizeInBytes = 8;
    canMessageFormat0x200.mIsMultiplexed = false;
    canMessageFormat0x200.mSignals = signals200;

    // This vector defines a list of signals CollectionScheme3 wants to collect
    // Signal 25 belong to CAN Frame 0x300 at Node 20
    ICollectionScheme::Signals_t signalInfo3 = ICollectionScheme::Signals_t();
    SignalCollectionInfo signal3;
    signal3.signalID = 25;
    signalInfo3.emplace_back( signal3 );
    signalToFrameAndNodeID[signal3.signalID] = { 0x300, "20" };
    struct CANMessageFormat canMessageFormat0x300;
    std::vector<CANSignalFormat> signals300 = std::vector<CANSignalFormat>();
    CANSignalFormat sigFormat25;
    sigFormat25.mSignalID = 25;
    signals300.emplace_back( sigFormat25 );
    canMessageFormat0x300.mMessageID = 0x300;
    canMessageFormat0x300.mSizeInBytes = 8;
    canMessageFormat0x300.mIsMultiplexed = false;
    canMessageFormat0x300.mSignals = signals300;

    // decoder Method for all CAN Frames at node 10
    std::unordered_map<CANRawFrameID, CANMessageFormat> formatMapNode10 = { { 0x100, canMessageFormat0x100 },
                                                                            { 0x101, CANMessageFormat() },
                                                                            { 0x200, CANMessageFormat() },
                                                                            { 0x110, canMessageFormat0x110 } };
    // decoder Method for all CAN Frames at node 20
    std::unordered_map<CANRawFrameID, CANMessageFormat> formatMapNode20 = { { 0x200, canMessageFormat0x200 },
                                                                            { 0x300, canMessageFormat0x300 } };

    // format Map used by Decoder Manifest Mock to response for getCANMessageFormat( CANRawFrameID canId,
    // CANInternalChannelID channelId
    // )
    std::unordered_map<InterfaceID, std::unordered_map<CANRawFrameID, CANMessageFormat>> formatMap = {
        { "10", formatMapNode10 }, { "20", formatMapNode20 } };

    // Here's input to decoder manifest for OBD PID Signal decoder information
    std::unordered_map<SignalID, PIDSignalDecoderFormat> signalIDToPIDDecoderFormat = {
        { 0x1000, PIDSignalDecoderFormat( 4, SID::CURRENT_STATS, 0x14, 0.0125, -40.0, 0, 2, 0, 8 ) },
        { 0x1001, PIDSignalDecoderFormat( 4, SID::CURRENT_STATS, 0x14, 0.0125, -40.0, 2, 2, 0, 8 ) },
        { 0x1005, PIDSignalDecoderFormat( 10, SID::CURRENT_STATS, 0x70, 1.0, 0.0, 9, 1, 0, 2 ) },
        { 0x1006, PIDSignalDecoderFormat( 10, SID::CURRENT_STATS, 0x70, 1.0, 0.0, 9, 1, 2, 2 ) } };

    // Here's input to decoder manifest for Custom Signal decoder information
    SignalIDToCustomSignalDecoderFormatMap signalIDToCustomDecoderFormat = {
        { 0x2000, CustomSignalDecoderFormat{ "30", "custom-decoder-0", 0x2000, SignalType::DOUBLE } },
        { 0x2001, CustomSignalDecoderFormat{ "31", "custom-decoder-1", 0x2001, SignalType::DOUBLE } },
    };

    // Add OBD-II PID signals to CollectionScheme 2
    SignalCollectionInfo obdPidSignal;
    obdPidSignal.signalID = 0x1000;
    signalInfo2.emplace_back( obdPidSignal );
    obdPidSignal.signalID = 0x1001;
    signalInfo2.emplace_back( obdPidSignal );
    obdPidSignal.signalID = 0x1005;
    signalInfo2.emplace_back( obdPidSignal );
    obdPidSignal.signalID = 0x1006;
    signalInfo2.emplace_back( obdPidSignal );

    // Add Custom Decoded signals to CollectionScheme 2
    SignalCollectionInfo customSignal;
    customSignal.signalID = 0x2000;
    signalInfo2.emplace_back( customSignal );
    customSignal.signalID = 0x2001;
    signalInfo2.emplace_back( customSignal );

    // Add an invalid network protocol signal. PM shall not add it to decoder dictionary
    SignalCollectionInfo inValidSignal;
    inValidSignal.signalID = 0x10000;
    signalInfo2.emplace_back( inValidSignal );

    auto collectionScheme1 =
        std::make_shared<ICollectionSchemeTest>( "COLLECTIONSCHEME1", "DM1", startTime1, stopTime1, signalInfo1 );
    auto collectionScheme2 =
        std::make_shared<ICollectionSchemeTest>( "COLLECTIONSCHEME2", "DM1", startTime2, stopTime2, signalInfo2 );
    auto collectionScheme3 =
        std::make_shared<ICollectionSchemeTest>( "COLLECTIONSCHEME3", "DM1", startTime3, stopTime3, signalInfo3 );
    std::vector<std::shared_ptr<ICollectionScheme>> list1;
    list1.emplace_back( collectionScheme1 );
    list1.emplace_back( collectionScheme2 );
    list1.emplace_back( collectionScheme3 );

    auto DM1 = std::make_shared<IDecoderManifestTest>(
        "DM1", formatMap, signalToFrameAndNodeID, signalIDToPIDDecoderFormat, signalIDToCustomDecoderFormat );

    // Set input as DM1, list1
    test.setDecoderManifest( DM1 );
    auto PL1 = std::make_shared<ICollectionSchemeListTest>( list1 );
    test.setCollectionSchemeList( PL1 );
    // Both collectionScheme1 and collectionScheme2 are expected to be enabled
    ASSERT_TRUE( test.updateMapsandTimeLine( currTime ) );
    // Invoke Decoder Dictionary Extractor function
    std::map<VehicleDataSourceProtocol, std::shared_ptr<DecoderDictionary>> decoderDictionaryMap;
    test.decoderDictionaryExtractor( decoderDictionaryMap );
    ASSERT_TRUE( decoderDictionaryMap.find( VehicleDataSourceProtocol::RAW_SOCKET ) != decoderDictionaryMap.end() );
    ASSERT_TRUE( decoderDictionaryMap.find( VehicleDataSourceProtocol::OBD ) != decoderDictionaryMap.end() );
    ASSERT_TRUE( decoderDictionaryMap.find( VehicleDataSourceProtocol::CUSTOM_DECODING ) !=
                 decoderDictionaryMap.end() );
    auto decoderDictionary =
        std::dynamic_pointer_cast<CANDecoderDictionary>( decoderDictionaryMap[VehicleDataSourceProtocol::RAW_SOCKET] );
    // Below section exam decoder dictionary
    // First, check whether dictionary has two top layer index: Channel1 and Channel2
    auto firstChannelId = canIDTranslator.getChannelNumericID( "10" );
    auto secondChannelId = canIDTranslator.getChannelNumericID( "20" );
    ASSERT_TRUE( decoderDictionary->canMessageDecoderMethod.find( firstChannelId ) !=
                 decoderDictionary->canMessageDecoderMethod.end() );
    ASSERT_TRUE( decoderDictionary->canMessageDecoderMethod.find( secondChannelId ) !=
                 decoderDictionary->canMessageDecoderMethod.end() );

    // Although 0x101 exit in Decoder Manifest but no CollectionScheme is interested in 0x101, hence decoder dictionary
    // will not include 0x101
    ASSERT_EQ( decoderDictionary->canMessageDecoderMethod[firstChannelId].count( 0x101 ), 0 );
    // CAN Frame 0x110 at Node 10 shall only have Signal 9 decoded.
    ASSERT_EQ( decoderDictionary->canMessageDecoderMethod[firstChannelId].count( 0x110 ), 1 );
    if ( decoderDictionary->canMessageDecoderMethod[firstChannelId].count( 0x110 ) == 1 )
    {
        auto decoderMethod = decoderDictionary->canMessageDecoderMethod[firstChannelId][0x110];
        ASSERT_EQ( decoderMethod.format.mSignals.size(), 1 );
        ASSERT_EQ( decoderMethod.format.mSignals[0].mSignalID, 9 );
    }
    // CAN Frame 0x200 at Node 20 shall have signal decoded. It contains signal 10 and 17
    ASSERT_EQ( decoderDictionary->canMessageDecoderMethod[secondChannelId].count( 0x200 ), 1 );
    auto decoderMethod = decoderDictionary->canMessageDecoderMethod[secondChannelId][0x200];
    // This CAN Frame is partially decoded to two signals
    ASSERT_EQ( decoderMethod.format.mSignals.size(), 2 );
    ASSERT_EQ( decoderMethod.format.mSignals[0].mSignalID, 10 );
    ASSERT_EQ( decoderMethod.format.mSignals[1].mSignalID, 17 );
    // CAN Frame 0x300 at Node 20 shall not exist in dictionary as CollectionScheme3 is not enabled yet
    ASSERT_EQ( decoderDictionary->canMessageDecoderMethod[secondChannelId].count( 0x300 ), 0 );
    // check the signalIDsToCollect from CAN decoder dictionary shall contain all the targeted CAN signals from
    // collectionSchemes Note minus 7 because 4 signals are OBD signals which will be included in OBD decoder dictionary
    // 2 custom decoded signals and one invalid signal
    ASSERT_EQ( decoderDictionary->signalIDsToCollect.size(), signalInfo1.size() + signalInfo2.size() - 7 );
    for ( auto const &signal : signalInfo1 )
    {
        ASSERT_EQ( decoderDictionary->signalIDsToCollect.count( signal.signalID ), 1 );
    }

    ASSERT_EQ( decoderDictionaryMap[VehicleDataSourceProtocol::OBD]->signalIDsToCollect.size(), 4 );
    const auto &obdPidDecoderDictionary =
        std::dynamic_pointer_cast<CANDecoderDictionary>( decoderDictionaryMap[VehicleDataSourceProtocol::OBD] )
            ->canMessageDecoderMethod;
    // Verify OBD PID Signals have correct decoder dictionary
    ASSERT_TRUE( obdPidDecoderDictionary.find( 0 ) != obdPidDecoderDictionary.end() );
    ASSERT_TRUE( obdPidDecoderDictionary.at( 0 ).find( 0x14 ) != obdPidDecoderDictionary.at( 0 ).end() );
    ASSERT_TRUE( obdPidDecoderDictionary.at( 0 ).find( 0x70 ) != obdPidDecoderDictionary.at( 0 ).end() );
    ASSERT_EQ( obdPidDecoderDictionary.at( 0 ).at( 0x14 ).format.mSizeInBytes, 4 );
    ASSERT_EQ( obdPidDecoderDictionary.at( 0 ).at( 0x14 ).format.mSignals.size(), 2 );
    auto formula = obdPidDecoderDictionary.at( 0 ).at( 0x14 ).format.mSignals[0];
    ASSERT_EQ( formula.mSignalID, 0x1000 );
    ASSERT_DOUBLE_EQ( formula.mFactor, 0.0125 );
    ASSERT_DOUBLE_EQ( formula.mOffset, -40.0 );
    ASSERT_EQ( formula.mFirstBitPosition, 0 );
    ASSERT_EQ( formula.mSizeInBits, 16 );
    formula = obdPidDecoderDictionary.at( 0 ).at( 0x14 ).format.mSignals[1];
    ASSERT_EQ( formula.mSignalID, 0x1001 );
    ASSERT_DOUBLE_EQ( formula.mFactor, 0.0125 );
    ASSERT_DOUBLE_EQ( formula.mOffset, -40.0 );
    ASSERT_EQ( formula.mFirstBitPosition, 16 );
    ASSERT_EQ( formula.mSizeInBits, 16 );
    ASSERT_EQ( obdPidDecoderDictionary.at( 0 ).at( 0x70 ).format.mSizeInBytes, 10 );
    ASSERT_EQ( obdPidDecoderDictionary.at( 0 ).at( 0x70 ).format.mSignals.size(), 2 );
    formula = obdPidDecoderDictionary.at( 0 ).at( 0x70 ).format.mSignals[0];
    ASSERT_EQ( formula.mSignalID, 0x1005 );
    ASSERT_DOUBLE_EQ( formula.mFactor, 1.0 );
    ASSERT_DOUBLE_EQ( formula.mOffset, 0.0 );
    ASSERT_EQ( formula.mFirstBitPosition, 72 );
    ASSERT_EQ( formula.mSizeInBits, 2 );
    formula = obdPidDecoderDictionary.at( 0 ).at( 0x70 ).format.mSignals[1];
    ASSERT_EQ( formula.mSignalID, 0x1006 );
    ASSERT_DOUBLE_EQ( formula.mFactor, 1.0 );
    ASSERT_DOUBLE_EQ( formula.mOffset, 0.0 );
    ASSERT_EQ( formula.mFirstBitPosition, 74 );
    ASSERT_EQ( formula.mSizeInBits, 2 );
    // Decoder Manifest doesn't contain PID 0x20, hence it shall not contain the decoder dictionary
    ASSERT_TRUE( obdPidDecoderDictionary.at( 0 ).find( 0x20 ) == obdPidDecoderDictionary.at( 0 ).end() );

    auto customDecoderDictionary = std::dynamic_pointer_cast<CustomDecoderDictionary>(
        decoderDictionaryMap[VehicleDataSourceProtocol::CUSTOM_DECODING] );
    ASSERT_EQ( customDecoderDictionary->customDecoderMethod.size(), 2 ); // 2 interfaces
    auto customDecoder = customDecoderDictionary->customDecoderMethod.find( "30" );
    ASSERT_TRUE( customDecoder != customDecoderDictionary->customDecoderMethod.end() );
    auto customDecoderSignalID = customDecoder->second.find( "custom-decoder-0" );
    ASSERT_TRUE( customDecoderSignalID != customDecoder->second.end() );
    ASSERT_EQ( customDecoderSignalID->second.mSignalID, 0x2000 );
    customDecoder = customDecoderDictionary->customDecoderMethod.find( "31" );
    ASSERT_TRUE( customDecoder != customDecoderDictionary->customDecoderMethod.end() );
    customDecoderSignalID = customDecoder->second.find( "custom-decoder-1" );
    ASSERT_TRUE( customDecoderSignalID != customDecoder->second.end() );
    ASSERT_EQ( customDecoderSignalID->second.mSignalID, 0x2001 );

    // Time travel to the point where Both collectionScheme1 and collectionScheme2 are retired and CollectionScheme 3
    // enabled
    ASSERT_TRUE( test.updateMapsandTimeLine( currTime + SECOND_TO_MILLISECOND( 6 ) ) );
    // decoder dictionary map is a local variable in PM worker thread, create a new one
    std::map<VehicleDataSourceProtocol, std::shared_ptr<DecoderDictionary>> decoderDictionaryMapNew;
    test.decoderDictionaryExtractor( decoderDictionaryMapNew );
    ASSERT_TRUE( decoderDictionaryMapNew.find( VehicleDataSourceProtocol::RAW_SOCKET ) !=
                 decoderDictionaryMapNew.end() );
    // OBD is only included in CollectionScheme 2 and it's already expired. Hence it will be an empty decoder dictionary
    // for OBD
    ASSERT_TRUE( decoderDictionaryMapNew.find( VehicleDataSourceProtocol::OBD ) != decoderDictionaryMapNew.end() );
    decoderDictionary =
        std::dynamic_pointer_cast<CANDecoderDictionary>( decoderDictionaryMapNew[VehicleDataSourceProtocol::OBD] );
    ASSERT_EQ( decoderDictionary, nullptr );
    ASSERT_TRUE( decoderDictionaryMapNew.find( VehicleDataSourceProtocol::CUSTOM_DECODING ) !=
                 decoderDictionaryMapNew.end() );
    customDecoderDictionary = std::dynamic_pointer_cast<CustomDecoderDictionary>(
        decoderDictionaryMapNew[VehicleDataSourceProtocol::CUSTOM_DECODING] );
    ASSERT_EQ( customDecoderDictionary, nullptr );

    decoderDictionary = std::dynamic_pointer_cast<CANDecoderDictionary>(
        decoderDictionaryMapNew[VehicleDataSourceProtocol::RAW_SOCKET] );
    // Now dictionary shall not contain anything for Node 10 as CollectionScheme1 is retired
    ASSERT_TRUE( decoderDictionary->canMessageDecoderMethod.find( firstChannelId ) ==
                 decoderDictionary->canMessageDecoderMethod.end() );
    ASSERT_TRUE( decoderDictionary->canMessageDecoderMethod.find( secondChannelId ) !=
                 decoderDictionary->canMessageDecoderMethod.end() );
    ASSERT_EQ( decoderDictionary->signalIDsToCollect.size(), signalInfo3.size() );
    for ( auto const &signal : signalInfo3 )
    {
        ASSERT_EQ( decoderDictionary->signalIDsToCollect.count( signal.signalID ), 1 );
    }
    if ( decoderDictionary->canMessageDecoderMethod.find( secondChannelId ) !=
         decoderDictionary->canMessageDecoderMethod.end() )
    {
        // CAN Frame 0x200 at Node 20 shall not exist as CollectionScheme2 retired
        ASSERT_EQ( decoderDictionary->canMessageDecoderMethod[secondChannelId].count( 0x200 ), 0 );
        // CAN Frame 0x300 at Node 20 shall exist in dictionary as CollectionScheme3 is enabled now
        ASSERT_EQ( decoderDictionary->canMessageDecoderMethod[secondChannelId].count( 0x300 ), 1 );
        auto decoderMethod = decoderDictionary->canMessageDecoderMethod[secondChannelId][0x300];
        // This CAN Frame is partially decoded to two signals
        ASSERT_EQ( decoderMethod.format.mSignals.size(), 1 );
        ASSERT_EQ( decoderMethod.format.mSignals[0].mSignalID, 25 );
    }

#ifdef FWE_FEATURE_LAST_KNOWN_STATE
    auto stateTemplate = std::make_shared<StateTemplateInformation>();
    stateTemplate->id = "LKS1";
    stateTemplate->decoderManifestID = "DM1";
    stateTemplate->updateStrategy = LastKnownStateUpdateStrategy::PERIODIC;
    stateTemplate->periodMs = 10;
    stateTemplate->signals.push_back( { 11, SignalType::DOUBLE } );
    test.setStateTemplates( std::make_shared<StateTemplatesDiff>( StateTemplatesDiff{ 123, { stateTemplate }, {} } ) );
    std::map<VehicleDataSourceProtocol, std::shared_ptr<DecoderDictionary>> decoderDictionaryMap3;
    test.decoderDictionaryExtractor( decoderDictionaryMap3 );
    decoderDictionary =
        std::dynamic_pointer_cast<CANDecoderDictionary>( decoderDictionaryMap3[VehicleDataSourceProtocol::RAW_SOCKET] );
    ASSERT_EQ( decoderDictionary->signalIDsToCollect.count( 11 ), 1 );
#endif

    // The following code validates that when we have first the OBD signals in the decoder manifest
    // and then the CAN signals, the extraction still functions. The above code starts processing
    // always the CAN Signals first as the first network type is CAN.
    CollectionSchemeManagerWrapper test2( nullptr, canIDTranslator, std::make_shared<CheckinSender>( nullptr ), "DM2" );
    std::vector<std::shared_ptr<ICollectionScheme>> list2;
    // Two collectionSchemes with 5 seconds expiry/
    // Timing is a problem on the target, making sure that we have a 100 ms of buffer
    // 1635951061244 is Wednesday, 3. November 2021 14:51:01.244 GMT.
    // Fixing it so that we don't need to deal with clock ticking issues on target
    auto collectionSchemeCAN =
        std::make_shared<ICollectionSchemeTest>( "CAN", "DM2", 1635951061244, 1635951061244 + 5000, signalInfo1 );
    auto collectionSchemeOBD =
        std::make_shared<ICollectionSchemeTest>( "OBD", "DM2", 1635951061244, 1635951061244 + 5000, signalInfo2 );
    list2.emplace_back( collectionSchemeOBD ); // OBD Signals
    list2.emplace_back( collectionSchemeCAN ); // CAN Signals

    auto DM2 = std::make_shared<IDecoderManifestTest>(
        "DM2", formatMap, signalToFrameAndNodeID, signalIDToPIDDecoderFormat, signalIDToCustomDecoderFormat );

    // Set input as DM1, list1
    test2.setDecoderManifest( DM2 );
    auto PL2 = std::make_shared<ICollectionSchemeListTest>( list2 );
    test2.setCollectionSchemeList( PL2 );
    // Both collectionScheme1 and collectionScheme2 are expected to be enabled
    ASSERT_TRUE( test2.updateMapsandTimeLine( { 1635951061244, 100 } ) );
    // Invoke Decoder Dictionary Extractor function
    std::map<VehicleDataSourceProtocol, std::shared_ptr<DecoderDictionary>> decoderDictionaryMap2;
    test2.decoderDictionaryExtractor( decoderDictionaryMap2 );
    ASSERT_TRUE( decoderDictionaryMap2.find( VehicleDataSourceProtocol::RAW_SOCKET ) != decoderDictionaryMap2.end() );
    ASSERT_TRUE( decoderDictionaryMap2.find( VehicleDataSourceProtocol::OBD ) != decoderDictionaryMap2.end() );
    ASSERT_TRUE( decoderDictionaryMap2.find( VehicleDataSourceProtocol::CUSTOM_DECODING ) !=
                 decoderDictionaryMap2.end() );
}

#ifdef FWE_FEATURE_VISION_SYSTEM_DATA
TEST( DecoderDictionaryExtractorTest, DecoderDictionaryComplexDataExtractor )
{
    CANInterfaceIDTranslator canIDTranslator;
    CollectionSchemeManagerWrapper test( nullptr, canIDTranslator, std::make_shared<CheckinSender>( nullptr ), "DM1" );

    std::shared_ptr<const Clock> testClock = ClockHandler::getClock();
    /* mock currTime, and 3 collectionSchemes */
    TimePoint currTime = testClock->timeSinceEpoch();
    Timestamp startTime1 = currTime.systemTimeMs;
    Timestamp stopTime1 = startTime1 + SECOND_TO_MILLISECOND( 5 );

    // Map to be used by Decoder Manifest Mock to return getCANFrameAndNodeID( SignalID signalId )
    std::unordered_map<SignalID, std::pair<CANRawFrameID, InterfaceID>> signalToFrameAndNodeID;

    ICollectionScheme::Signals_t signalInfo1;
    std::unordered_map<InterfaceID, std::unordered_map<CANRawFrameID, CANMessageFormat>> formatMap; // empty
    std::unordered_map<SignalID, PIDSignalDecoderFormat> signalIDToPIDDecoderFormat;                // empty
    SignalIDToCustomSignalDecoderFormatMap signalIDToCustomDecoderFormat;                           // empty

    ICollectionScheme::PartialSignalIDLookup partialSignalIDLookup;
    std::unordered_map<SignalID, ComplexSignalDecoderFormat> complexSignalMap;
    std::unordered_map<ComplexDataTypeId, ComplexDataElement> complexDataTypeMap;

    SignalCollectionInfo signal1;
    signal1.signalID = 0xFFFF0000;
    signalInfo1.push_back( signal1 );
    partialSignalIDLookup[signal1.signalID] = std::pair<SignalID, SignalPath>( 0x2000000, SignalPath{ 1, 2, 5 } );
    complexSignalMap[0x2000000] = ComplexSignalDecoderFormat{ "interface1", "messageID1", 30 };

    SignalCollectionInfo signal2;
    signal2.signalID = 0xFFF30000;
    signalInfo1.push_back( signal2 );
    partialSignalIDLookup[signal2.signalID] = std::pair<SignalID, SignalPath>( 0x2000000, SignalPath{ 1, 1, 7 } );

    SignalCollectionInfo signal6;
    signal6.signalID = 0xFFF60000;
    signalInfo1.push_back( signal6 );
    partialSignalIDLookup[signal6.signalID] = std::pair<SignalID, SignalPath>( 0x2000000, SignalPath{ 2, 2 } );

    SignalCollectionInfo signal4;
    signal4.signalID = 0xFFF40000;
    signalInfo1.push_back( signal4 );
    partialSignalIDLookup[signal4.signalID] = std::pair<SignalID, SignalPath>( 0x2000000, SignalPath{ 2 } );

    SignalCollectionInfo signal3;
    signal3.signalID = 0x01000000;
    signalInfo1.push_back( signal3 );
    complexSignalMap[0x01000000] = ComplexSignalDecoderFormat{ "interface1", "messageID3", 20 };

    SignalCollectionInfo signal5;
    signal5.signalID = 0xFFF50000;
    signalInfo1.push_back( signal5 );
    partialSignalIDLookup[signal5.signalID] = std::pair<SignalID, SignalPath>( signal3.signalID, SignalPath{ 0 } );

    complexDataTypeMap[30] = ComplexDataElement( ComplexStruct{ { 20, 10, 20 } } );
    complexDataTypeMap[20] = ComplexDataElement( ComplexArray{ 50000, 10 } );
    complexDataTypeMap[10] = ComplexDataElement( PrimitiveData{ SignalType::UINT64, 1.0, 0.0 } );

    auto collectionScheme1 = std::make_shared<ICollectionSchemeTest>(
        "COLLECTIONSCHEMECOMPLEXDATA1", "DM1", startTime1, stopTime1, signalInfo1, partialSignalIDLookup );

    std::vector<std::shared_ptr<ICollectionScheme>> list1;
    list1.emplace_back( collectionScheme1 );

    auto DM1 = std::make_shared<IDecoderManifestTest>( "DM1",
                                                       formatMap,
                                                       signalToFrameAndNodeID,
                                                       signalIDToPIDDecoderFormat,
                                                       signalIDToCustomDecoderFormat,
                                                       complexSignalMap,
                                                       complexDataTypeMap );

    // Set input as DM1, list1
    test.setDecoderManifest( DM1 );
    auto PL1 = std::make_shared<ICollectionSchemeListTest>( list1 );
    test.setCollectionSchemeList( PL1 );
    // Both collectionScheme1 and collectionScheme2 are expected to be enabled
    ASSERT_TRUE( test.updateMapsandTimeLine( currTime ) );

    InspectionMatrix inspectionMatrixOutput;
    FetchMatrix fetchMatrixOutput;
    test.matrixExtractor( inspectionMatrixOutput, fetchMatrixOutput );
    // Invoke Decoder Dictionary Extractor function
    std::map<VehicleDataSourceProtocol, std::shared_ptr<DecoderDictionary>> decoderDictionaryMap;
    test.decoderDictionaryExtractor( decoderDictionaryMap, inspectionMatrixOutput );

    auto dict = decoderDictionaryMap.find( VehicleDataSourceProtocol::COMPLEX_DATA );
    ASSERT_NE( dict, decoderDictionaryMap.end() );
    auto complexDict = std::dynamic_pointer_cast<ComplexDataDecoderDictionary>( dict->second );
    ASSERT_TRUE( complexDict );
    auto decoder = complexDict->complexMessageDecoderMethod["interface1"]["messageID1"];
    ASSERT_EQ( decoder.mSignalId, 0x2000000 );
    ASSERT_EQ( decoder.mCollectRaw, false );
    ASSERT_EQ( decoder.mSignalPaths.size(), 4 );
    // Check order correct
    ASSERT_EQ( decoder.mSignalPaths[0].mSignalPath, ( std::vector<uint32_t>{ 1, 1, 7 } ) );
    ASSERT_EQ( decoder.mSignalPaths[1].mSignalPath, ( std::vector<uint32_t>{ 1, 2, 5 } ) );
    ASSERT_EQ( decoder.mSignalPaths[2].mSignalPath, ( std::vector<uint32_t>{ 2 } ) );
    ASSERT_EQ( decoder.mSignalPaths[3].mSignalPath, ( std::vector<uint32_t>{ 2, 2 } ) );

    auto decoder2 = complexDict->complexMessageDecoderMethod["interface1"]["messageID3"];
    ASSERT_EQ( decoder2.mSignalId, 0x01000000 );
    ASSERT_EQ( decoder2.mCollectRaw, true );
    ASSERT_EQ( decoder2.mSignalPaths.size(), 1 );

    ASSERT_EQ( decoder.mRootTypeId, 30 );
    auto decodedStruct = boost::get<ComplexStruct>( decoder.mComplexTypeMap[30] );
    ASSERT_EQ( decodedStruct.mOrderedTypeIds[0], 20 );
    ASSERT_EQ( decodedStruct.mOrderedTypeIds[1], 10 );
    ASSERT_EQ( decodedStruct.mOrderedTypeIds[2], 20 );

    auto decodedArray = boost::get<ComplexArray>( decoder.mComplexTypeMap[20] );
    ASSERT_EQ( decodedArray.mSize, 50000 );
    ASSERT_EQ( decodedArray.mRepeatedTypeId, 10 );

    auto decodedPrimitive = boost::get<PrimitiveData>( decoder.mComplexTypeMap[10] );
    ASSERT_EQ( decodedPrimitive.mPrimitiveType, SignalType::UINT64 );
    ASSERT_EQ( decodedPrimitive.mScaling, 1.0 );
    ASSERT_EQ( decodedPrimitive.mOffset, 0.0 );

    ASSERT_EQ( inspectionMatrixOutput.conditions.size(), 1 );
    ASSERT_EQ( inspectionMatrixOutput.conditions.at( 0 ).signals.size(), 6 );

    auto &signals = inspectionMatrixOutput.conditions.at( 0 ).signals;
    ASSERT_EQ( signals.at( 0 ).signalID, signal1.signalID );
    ASSERT_EQ( signals.at( 0 ).signalType, SignalType::UINT64 );

    ASSERT_EQ( signals.at( 1 ).signalID, signal2.signalID );
    ASSERT_EQ( signals.at( 1 ).signalType, SignalType::UINT64 );

    ASSERT_EQ( signals.at( 2 ).signalID, signal6.signalID );
    ASSERT_EQ( signals.at( 2 ).signalType, SignalType::UINT64 );

    ASSERT_EQ( signals.at( 3 ).signalID, signal4.signalID );
    ASSERT_EQ( signals.at( 3 ).signalType, SignalType::UNKNOWN );

    ASSERT_EQ( signals.at( 4 ).signalID, signal3.signalID );
    ASSERT_EQ( signals.at( 4 ).signalType, SignalType::UNKNOWN );

    ASSERT_EQ( signals.at( 5 ).signalID, signal5.signalID );
    ASSERT_EQ( signals.at( 5 ).signalType, SignalType::UINT64 );
}

TEST( DecoderDictionaryExtractorTest, DecoderDictionaryInvalidPartialSignalIDAndInvalidComplexType )
{

    CANInterfaceIDTranslator canIDTranslator;
    CollectionSchemeManagerWrapper test( nullptr, canIDTranslator, std::make_shared<CheckinSender>( nullptr ), "DM1" );

    std::shared_ptr<const Clock> testClock = ClockHandler::getClock();
    /* mock currTime, and 3 collectionSchemes */
    TimePoint currTime = testClock->timeSinceEpoch();
    Timestamp startTime1 = currTime.systemTimeMs;
    Timestamp stopTime1 = startTime1 + SECOND_TO_MILLISECOND( 5 );

    // Map to be used by Decoder Manifest Mock to return getCANFrameAndNodeID( SignalID signalId )
    std::unordered_map<SignalID, std::pair<CANRawFrameID, InterfaceID>> signalToFrameAndNodeID;

    ICollectionScheme::Signals_t signalInfo1;
    std::unordered_map<InterfaceID, std::unordered_map<CANRawFrameID, CANMessageFormat>> formatMap; // empty
    std::unordered_map<SignalID, PIDSignalDecoderFormat> signalIDToPIDDecoderFormat;                // empty
    SignalIDToCustomSignalDecoderFormatMap signalIDToCustomDecoderFormat;                           // empty

    ICollectionScheme::PartialSignalIDLookup partialSignalIDLookup;
    std::unordered_map<SignalID, ComplexSignalDecoderFormat> complexSignalMap;
    std::unordered_map<ComplexDataTypeId, ComplexDataElement> complexDataTypeMap;

    SignalCollectionInfo signal1;
    signal1.signalID = 0xFF000000;
    signalInfo1.push_back( signal1 );
    // Set different signalId
    partialSignalIDLookup[signal1.signalID + 1] = std::pair<SignalID, SignalPath>( 0x2000000, SignalPath{ 1, 2, 5 } );
    complexSignalMap[0x2000000] = ComplexSignalDecoderFormat{ "interface1", "messageID1", 30 };

    SignalCollectionInfo signal2;
    signal2.signalID = 0xFF100000;
    signalInfo1.push_back( signal2 );
    partialSignalIDLookup[signal2.signalID] = std::pair<SignalID, SignalPath>( 0x2000001, SignalPath{ 1, 2, 5 } );
    complexSignalMap[0x2000001] = ComplexSignalDecoderFormat{ "interface1", "messageID2", 40 };

    // Wrong type id lookup for 40 will fail ads only 41 in map
    complexDataTypeMap[41] = ComplexDataElement( ComplexStruct{ { 20, 10, 20 } } );

    auto collectionScheme1 = std::make_shared<ICollectionSchemeTest>(
        "COLLECTIONSCHEMECOMPLEXDATA1", "DM1", startTime1, stopTime1, signalInfo1, partialSignalIDLookup );

    std::vector<std::shared_ptr<ICollectionScheme>> list1;
    list1.emplace_back( collectionScheme1 );

    auto DM1 = std::make_shared<IDecoderManifestTest>( "DM1",
                                                       formatMap,
                                                       signalToFrameAndNodeID,
                                                       signalIDToPIDDecoderFormat,
                                                       signalIDToCustomDecoderFormat,
                                                       complexSignalMap,
                                                       complexDataTypeMap );

    test.setDecoderManifest( DM1 );
    auto PL1 = std::make_shared<ICollectionSchemeListTest>( list1 );
    test.setCollectionSchemeList( PL1 );

    ASSERT_TRUE( test.updateMapsandTimeLine( currTime ) );
    // Invoke Decoder Dictionary Extractor function
    std::map<VehicleDataSourceProtocol, std::shared_ptr<DecoderDictionary>> decoderDictionaryMap;
    InspectionMatrix inspectionMatrix;
    test.decoderDictionaryExtractor( decoderDictionaryMap, inspectionMatrix );

    auto dict = decoderDictionaryMap.find( VehicleDataSourceProtocol::COMPLEX_DATA );
    ASSERT_NE( dict, decoderDictionaryMap.end() );
    auto complexDict = std::dynamic_pointer_cast<ComplexDataDecoderDictionary>( dict->second );
    ASSERT_TRUE( complexDict );
    ASSERT_EQ( complexDict->complexMessageDecoderMethod.size(), 1 ); // only second signal is in map and ERROR in log
}
#endif

} // namespace IoTFleetWise
} // namespace Aws

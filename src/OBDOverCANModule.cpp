// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "aws/iotfleetwise/OBDOverCANModule.h"
#include "aws/iotfleetwise/Assert.h"
#include "aws/iotfleetwise/ISOTPOverCANOptions.h"
#include "aws/iotfleetwise/LoggingModule.h"
#include "aws/iotfleetwise/MessageTypes.h"
#include "aws/iotfleetwise/SignalTypes.h"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <linux/can.h>
#include <linux/can/isotp.h>
#include <net/if.h>
#include <poll.h>
#include <set>
#include <sstream>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace Aws
{
namespace IoTFleetWise
{

// NOLINT below due to C++17 warning of redundant declarations that are required to maintain C++14 compatibility
constexpr int OBDOverCANModule::SLEEP_TIME_SECS;             // NOLINT
constexpr uint32_t OBDOverCANModule::MASKING_GET_BYTE;       // NOLINT Get last byte
constexpr uint32_t OBDOverCANModule::MASKING_SHIFT_BITS;     // NOLINT Shift 8 bits
constexpr uint32_t OBDOverCANModule::MASKING_TEMPLATE_TX_ID; // NOLINT All 29-bit tx id has the same bytes
constexpr uint32_t OBDOverCANModule::MASKING_REMOVE_BYTE;    // NOLINT

OBDOverCANModule::OBDOverCANModule( SignalBufferDistributor &signalBufferDistributor,
                                    std::string gatewayCanInterfaceName,
                                    uint32_t pidRequestIntervalSeconds,
                                    uint32_t dtcRequestIntervalSeconds,
                                    bool broadcastRequests )
    : mSignalBufferDistributor( signalBufferDistributor )
    , mPIDRequestIntervalSeconds( pidRequestIntervalSeconds )
    , mDTCRequestIntervalSeconds( dtcRequestIntervalSeconds )
    , mBroadcastRequests( broadcastRequests )
    , mGatewayCanInterfaceName( std::move( gatewayCanInterfaceName ) )
    , mOBDDataDecoder( mDecoderDictionary )
{
}

OBDOverCANModule::~OBDOverCANModule()
{
    // To make sure the thread stops during teardown of tests.
    if ( isAlive() )
    {
        stop();
    }
}

bool
OBDOverCANModule::start()
{
    // Prevent concurrent stop/init
    std::lock_guard<std::mutex> lock( mThreadMutex );
    // On multi core systems the shared variable mShouldStop must be updated for
    // all cores before starting the thread otherwise thread will directly end
    mShouldStop.store( false );
    // Make sure the thread goes into sleep immediately to wait for
    // the manifest to be available
    mDecoderManifestAvailable.store( false );
    // Do not request DTCs on startup
    mShouldRequestDTCs.store( false );
    if ( !mThread.create( [this]() {
             this->doWork();
         } ) )
    {
        FWE_LOG_TRACE( "OBD Module Thread failed to start" );
    }
    else
    {
        FWE_LOG_TRACE( "OBD Module Thread started" );
        mThread.setThreadName( "fwDIOBDModule" );
    }

    return mThread.isActive() && mThread.isValid();
}

bool
OBDOverCANModule::stop()
{
    if ( ( !mThread.isValid() ) || ( !mThread.isActive() ) )
    {
        return true;
    }

    std::lock_guard<std::mutex> lock( mThreadMutex );
    mShouldStop.store( true, std::memory_order_relaxed );
    FWE_LOG_TRACE( "OBD Module Thread requested to stop" );
    mWait.notify();
    mDataAvailableWait.notify();
    mThread.release();
    mShouldStop.store( false, std::memory_order_relaxed );
    FWE_LOG_TRACE( "Thread stopped" );
    if ( ( mBroadcastSocket >= 0 ) && ( close( mBroadcastSocket ) < 0 ) )
    {
        FWE_LOG_ERROR( "Failed to close broadcastSocket" );
    }
    return !mThread.isActive();
}

bool
OBDOverCANModule::shouldStop() const
{
    return mShouldStop.load( std::memory_order_relaxed );
}

void
OBDOverCANModule::doWork()
{
    // First we will auto detect ECUs
    bool finishECUsDetection = false;
    while ( ( !finishECUsDetection ) && ( !shouldStop() ) )
    {
        std::vector<uint32_t> canIDResponses;
        {
            bool decoderDictionaryEmpty = false;
            {
                std::lock_guard<std::mutex> lock( mDecoderDictMutex );
                decoderDictionaryEmpty = mDecoderDictionary.empty();
            }
            // If we don't have an OBD decoder manifest and we should not request DTCs,
            // Take the thread to sleep
            if ( ( !mShouldRequestDTCs.load( std::memory_order_relaxed ) ) && decoderDictionaryEmpty )
            {
                FWE_LOG_TRACE(
                    "No valid decoding dictionary available and DTC requests disabled, Module Thread going to sleep " );
                mDataAvailableWait.wait( Signal::WaitWithPredicate );
            }
        }
        // Now we will determine whether the ECUs are using extended IDs
        bool isExtendedID = false;
        autoDetectECUs( isExtendedID, canIDResponses );
        if ( canIDResponses.empty() )
        {
            isExtendedID = true;
            autoDetectECUs( isExtendedID, canIDResponses );
        }
        FWE_LOG_TRACE( "Detect size of ECUs:" + std::to_string( canIDResponses.size() ) );
        if ( !canIDResponses.empty() )
        {
            // If broadcast mode is enabled, open the broadcast socket:
            if ( mBroadcastRequests )
            {
                mBroadcastSocket = openISOTPBroadcastSocket( isExtendedID );
                // Failure to open broadcast socket is non recoverable, hence we will send signal out to terminate
                FWE_GRACEFUL_FATAL_ASSERT( mBroadcastSocket >= 0, "Failed to open broadcast", );
            }
            // Initialize ECU for each CAN ID in canIDResponse
            FWE_GRACEFUL_FATAL_ASSERT( initECUs( isExtendedID, canIDResponses, mBroadcastSocket ),
                                       "OBDOverCANECU failed to init. Check CAN ISO-TP module", );
            finishECUsDetection = true;
        }
        // As we haven't detected ECUs, wait for 1 second and try again
        else
        {
            FWE_LOG_TRACE( "Waiting for: " + std::to_string( SLEEP_TIME_SECS ) + " seconds" );
            mWait.wait( static_cast<uint32_t>( SLEEP_TIME_SECS * 1000 ) );
        }
    }

    mDTCTimer.reset();
    mPIDTimer.reset();
    // Flag to indicate whether we have acquired the supported PIDs from ECUs.
    bool hasAcquiredSupportedPIDs = false;
    while ( !shouldStop() )
    {
        // Check if we need to request PIDs and we have a decoder manifest to decode them.
        if ( mDecoderManifestAvailable.load( std::memory_order_relaxed ) )
        {
            // A new decoder manifest arrived. Pass it over to the OBD decoder.
            std::lock_guard<std::mutex> lock( mDecoderDictMutex );
            FWE_LOG_TRACE( "Decoder Manifest set on the OBD Decoder " );
            // Reset the atomic state
            mDecoderManifestAvailable.store( false, std::memory_order_relaxed );
        }
        // Is it time to request PIDs ?
        if ( ( mPIDRequestIntervalSeconds > 0 ) && ( mPIDTimer.getElapsedSeconds() >= mPIDRequestIntervalSeconds ) )
        {
            // Reschedule
            mPIDTimer.reset();
            // besides this thread, onChangeOfActiveDictionary can update mPIDsToRequestPerECU.
            // Use mutex to ensure only one thread is doing the update.
            std::lock_guard<std::mutex> lock( mDecoderDictMutex );
            // Request PID if decoder dictionary is valid and it is time to do so
            if ( !mDecoderDictionary.empty() )
            {
                // This should execute only once
                if ( !hasAcquiredSupportedPIDs )
                {
                    hasAcquiredSupportedPIDs = true;
                    assignPIDsToECUs();
                    // Reschedule
                    mPIDTimer.reset();
                }
                for ( auto &ecu : mECUs )
                {
                    auto numRequests = ecu->requestReceiveEmissionPIDs( SID::CURRENT_STATS );
                    flush( numRequests, *ecu );
                }
            }
        }
        if ( ( mDTCRequestIntervalSeconds > 0 ) && ( mDTCTimer.getElapsedSeconds() >= mDTCRequestIntervalSeconds ) )
        {
            // Reschedule
            mDTCTimer.reset();
            // Request DTC if specified by inspection matrix and it is time to do so
            if ( mShouldRequestDTCs.load( std::memory_order_relaxed ) )
            {
                bool successfulDTCRequest = false;
                DTCInfo dtcInfo;
                dtcInfo.receiveTime = mClock->systemTimeSinceEpochMs();
                for ( auto &ecu : mECUs )
                {
                    size_t numRequests = 0;
                    if ( ecu->getDTCData( dtcInfo, numRequests ) )
                    {
                        successfulDTCRequest = true;
                    }
                    flush( numRequests, *ecu );
                }
                // Also DTCInfo structs without any DTCs must be pushed to the queue because it means
                // there was a OBD request that did not return any SID::STORED_DTCs
                if ( successfulDTCRequest )
                {
                    mSignalBufferDistributor.push( CollectedDataFrame( std::make_shared<DTCInfo>( dtcInfo ) ) );
                }
            }
        }

        // Wait for the next cycle
        int64_t sleepTime = INT64_MAX;
        calcSleepTime( mPIDRequestIntervalSeconds, mPIDTimer, sleepTime );
        calcSleepTime( mDTCRequestIntervalSeconds, mDTCTimer, sleepTime );
        if ( sleepTime < 0 )
        {
            FWE_LOG_WARN( "Request time overdue by " + std::to_string( -sleepTime ) + " ms" );
        }
        else
        {
            FWE_LOG_TRACE( "Waiting for: " + std::to_string( sleepTime ) + " ms" );
            mWait.wait( static_cast<uint32_t>( sleepTime ) );
        }
    }
}

void
OBDOverCANModule::calcSleepTime( uint32_t requestIntervalSeconds, const Timer &timer, int64_t &outputSleepTime )
{
    if ( requestIntervalSeconds > 0 )
    {
        auto sleepTime = ( static_cast<int64_t>( requestIntervalSeconds ) * 1000 ) - timer.getElapsedMs().count();
        if ( sleepTime < outputSleepTime )
        {
            outputSleepTime = sleepTime;
        }
    }
}

void
OBDOverCANModule::flush( size_t count, OBDOverCANECU &exceptECU )
{
    if ( !mBroadcastRequests )
    {
        return;
    }
    uint32_t timeLeftMs = P2_TIMEOUT_DEFAULT_MS;
    for ( auto &ecu : mECUs )
    {
        if ( ecu.get() == &exceptECU )
        {
            continue;
        }
        for ( size_t i = 0; i < count; i++ )
        {
            uint32_t timeNeededMs = ecu->flush( timeLeftMs );
            if ( timeNeededMs >= timeLeftMs )
            {
                timeLeftMs = 0;
            }
            else
            {
                timeLeftMs -= timeNeededMs;
            }
        }
    }
}

bool
OBDOverCANModule::autoDetectECUs( bool isExtendedID, std::vector<uint32_t> &canIDResponses )
{
    struct sockaddr_can interfaceAddress = {};
    struct ifreq interfaceRequest = {};
    struct can_frame frame = {};
    // After 1 second timeout and stop ECU detection
    constexpr uint32_t MAX_WAITING_MS = 1000;
    // Open a CAN raw socket
    int rawSocket = socket( PF_CAN, SOCK_RAW, CAN_RAW );
    if ( rawSocket < 0 )
    {
        FWE_LOG_ERROR( "Failed to create socket: " + getErrnoString() );
        return false;
    }
    // Set the IF name
    strncpy( interfaceRequest.ifr_name, mGatewayCanInterfaceName.c_str(), sizeof( interfaceRequest.ifr_name ) - 1 );
    if ( ioctl( rawSocket, SIOCGIFINDEX, &interfaceRequest ) != 0 )
    {
        close( rawSocket );
        FWE_LOG_ERROR( "CAN interface is not accessible: " + getErrnoString() );
        return false;
    }

    interfaceAddress.can_family = AF_CAN;
    interfaceAddress.can_ifindex = interfaceRequest.ifr_ifindex;

    // Bind the socket
    if ( bind( rawSocket, reinterpret_cast<struct sockaddr *>( &interfaceAddress ), sizeof( interfaceAddress ) ) < 0 )
    {
        close( rawSocket );
        FWE_LOG_ERROR( "Failed to bind socket: " + getErrnoString() );
        return false;
    }
    // Set frame can_id and flags
    frame.can_id =
        isExtendedID ? ( (uint32_t)ECUID::BROADCAST_EXTENDED_ID | CAN_EFF_FLAG ) : (uint32_t)ECUID::BROADCAST_ID;
    frame.can_dlc = 8; // CAN DLC
    frame.data[0] = 2; // Single frame data length
    frame.data[1] = 1; // Service ID 01: Show current data
    frame.data[2] = 0; // PID 00: PIDs supported [01-20]

    // Send broadcast request
    if ( write( rawSocket, &frame, sizeof( struct can_frame ) ) != sizeof( struct can_frame ) )
    {
        close( rawSocket );
        FWE_LOG_ERROR( "Failed to write" );
        return false;
    }
    FWE_LOG_TRACE( "Sent broadcast request" );

    Timer broadcastTimer;
    broadcastTimer.reset();

    while ( !shouldStop() )
    {
        if ( broadcastTimer.getElapsedMs().count() > MAX_WAITING_MS )
        {
            FWE_LOG_TRACE( "Time elapsed: " + std::to_string( broadcastTimer.getElapsedMs().count() ) +
                           ", time to stop ECUs' detection" );
            break;
        }
        struct pollfd pfd = { rawSocket, POLLIN, 0 };
        int res = poll( &pfd, 1U, static_cast<int>( P2_TIMEOUT_DEFAULT_MS ) );
        if ( res < 0 )
        {
            close( rawSocket );
            FWE_LOG_ERROR( "Poll error" );
            return false;
        }
        if ( res == 0 ) // Time out
        {
            break;
        }
        // Get response
        if ( recv( rawSocket, &frame, sizeof( struct can_frame ), 0 ) < 0 )
        {
            close( rawSocket );
            FWE_LOG_ERROR( "Failed to read response" );
            return false;
        }

        // 11-bit range 7E8, 7E9, 7EA ... 7EF ==> [7E8, 7EF]
        // 29-bit range: [18DAF100, 18DAF1FF]
        auto frameCANId = isExtendedID ? ( frame.can_id & CAN_EFF_MASK ) : frame.can_id;
        auto smallestCANId =
            isExtendedID ? (uint32_t)ECUID::LOWEST_ECU_EXTENDED_RX_ID : (uint32_t)ECUID::LOWEST_ECU_RX_ID;
        auto biggestCANId =
            isExtendedID ? (uint32_t)ECUID::HIGHEST_ECU_EXTENDED_RX_ID : (uint32_t)ECUID::HIGHEST_ECU_RX_ID;
        // Check if CAN ID is valid
        if ( ( smallestCANId <= frameCANId ) && ( frameCANId <= biggestCANId ) )
        {
            canIDResponses.push_back( frameCANId );
        }
    }
    FWE_LOG_TRACE( "Detected number of ECUs: " + std::to_string( canIDResponses.size() ) );
    for ( std::size_t i = 0; i < canIDResponses.size(); ++i )
    {
        std::stringstream stream_rx;
        stream_rx << std::hex << canIDResponses[i];
        FWE_LOG_TRACE( "ECU with rx_id: " + stream_rx.str() );
    }
    // Close the socket
    if ( close( rawSocket ) < 0 )
    {
        FWE_LOG_ERROR( "Failed to close socket" );
        return false;
    }
    return true;
}

int
OBDOverCANModule::openISOTPBroadcastSocket( bool isExtendedID )
{
    // Socket CAN parameters
    struct sockaddr_can interfaceAddress = {};
    struct can_isotp_options optionalFlags = {};

    // Set the source
    interfaceAddress.can_addr.tp.tx_id =
        isExtendedID ? (uint32_t)ECUID::BROADCAST_EXTENDED_ID | CAN_EFF_FLAG : (uint32_t)ECUID::BROADCAST_ID;
    // Set flags to stop flow control messages being used for this socket
    optionalFlags.flags |= CAN_ISOTP_TX_PADDING | CAN_ISOTP_LISTEN_MODE | CAN_ISOTP_SF_BROADCAST;

    // Open a Socket
    int broadcastSocket = socket( PF_CAN, SOCK_DGRAM, CAN_ISOTP );
    if ( broadcastSocket < 0 )
    {
        FWE_LOG_ERROR( "Failed to create the ISOTP broadcast socket to IF: " + mGatewayCanInterfaceName +
                       " Error: " + getErrnoString() );
        return -1;
    }

    // Set the optional Flags
    if ( setsockopt( broadcastSocket, SOL_CAN_ISOTP, CAN_ISOTP_OPTS, &optionalFlags, sizeof( optionalFlags ) ) < 0 )
    {
        FWE_LOG_ERROR( "Failed to set ISO-TP socket option flags" );
        close( broadcastSocket );
        return -1;
    }
    // CAN PF and Interface Index
    interfaceAddress.can_family = AF_CAN;
    interfaceAddress.can_ifindex = static_cast<int>( if_nametoindex( mGatewayCanInterfaceName.c_str() ) );

    // Bind the socket
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
    if ( bind( broadcastSocket, (struct sockaddr *)&interfaceAddress, sizeof( interfaceAddress ) ) < 0 )
    {
        FWE_LOG_ERROR( "Failed to bind the ISOTP Socket to IF: " + mGatewayCanInterfaceName +
                       " Error: " + getErrnoString() );
        close( broadcastSocket );
        return -1;
    }
    FWE_LOG_TRACE( "ISOTP Socket connected to IF: " + mGatewayCanInterfaceName );
    return broadcastSocket;
}

constexpr uint32_t
OBDOverCANModule::getTxIDByRxID( bool isExtendedID, uint32_t rxId )
{
    // Calculate tx_id according to rx_id, operators are following:
    // If is 29-bit, e.g. rx_id = 0x18DAF159:
    //              rx_id & 0xFF = 0x59
    //              0x59 << 8 = 0x5900
    //              0x5900 | 0x18DA00F1 = 0x18DA59F1 = tx_id
    // If is 11-bit, e.g. rx_id = 0x7E8:
    //              rx_id - 0x8 = 0x7E0 = tx_id
    return isExtendedID ? ( ( rxId & MASKING_GET_BYTE ) << MASKING_SHIFT_BITS ) | MASKING_TEMPLATE_TX_ID
                        : rxId - MASKING_REMOVE_BYTE;
}

bool
OBDOverCANModule::initECUs( bool isExtendedID, std::vector<uint32_t> &canIDResponses, int broadcastSocket )
{
    // create a set of CAN ID in case there's duplication
    auto canIDSet = std::set<uint32_t>( canIDResponses.begin(), canIDResponses.end() );
    for ( auto rxID : canIDSet )
    {
        auto ecu = std::make_unique<OBDOverCANECU>( mGatewayCanInterfaceName,
                                                    mOBDDataDecoder,
                                                    rxID,
                                                    getTxIDByRxID( isExtendedID, rxID ),
                                                    isExtendedID,
                                                    mSignalBufferDistributor,
                                                    broadcastSocket );
        if ( !ecu->init() )
        {
            return false;
        }
        mECUs.push_back( std::move( ecu ) );
    }
    FWE_LOG_TRACE( "Initialize ECUs in size of: " + std::to_string( mECUs.size() ) );
    return true;
}

void
OBDOverCANModule::assignPIDsToECUs()
{
    // clear the PID allocation table
    mPIDAssigned.clear();
    for ( auto &ecu : mECUs )
    {
        // Get supported PIDs. FWE will either request it from ECU or get it from the buffer
        auto numRequests = ecu->requestReceiveSupportedPIDs( SID::CURRENT_STATS );
        flush( numRequests, *ecu );
        // Allocate PID to each ECU to request. Note that if the PID has been already assigned, it will not be
        // reassigned to another ECU
        ecu->updatePIDRequestList( SID::CURRENT_STATS, mPIDsRequestedByDecoderDict[SID::CURRENT_STATS], mPIDAssigned );
    }
}

bool
OBDOverCANModule::connect()
{
    if ( ( mPIDRequestIntervalSeconds == 0 ) && ( mDTCRequestIntervalSeconds == 0 ) )
    {
        FWE_LOG_TRACE( "Both PID and DTC interval seconds are set to 0. Thread will not be started." );
        return true;
    }
    return start();
}

bool
OBDOverCANModule::disconnect()
{
    return stop();
}

bool
OBDOverCANModule::isAlive()
{
    if ( ( !mThread.isValid() ) || ( !mThread.isActive() ) )
    {
        return false;
    }
    for ( auto &ecu : mECUs )
    {
        if ( !ecu->isAlive() )
        {
            return false;
        }
    }
    return true;
}

std::vector<PID>
OBDOverCANModule::getExternalPIDsToRequest()
{
    std::lock_guard<std::mutex> lock( mDecoderDictMutex );
    std::vector<PID> pids;
    for ( const auto &decoder : mDecoderDictionary )
    {
        pids.push_back( decoder.first );
    }
    return pids;
}

void
OBDOverCANModule::setExternalPIDResponse( PID pid, std::vector<uint8_t> response )
{
    std::lock_guard<std::mutex> lock( mDecoderDictMutex );
    if ( mDecoderDictionary.find( pid ) == mDecoderDictionary.end() )
    {
        FWE_LOG_WARN( "Unexpected PID response: " + std::to_string( pid ) );
        return;
    }
    EmissionInfo info;
    std::vector<PID> pids = { pid };
    size_t expectedResponseSize = 2 + mDecoderDictionary.at( pid ).mSizeInBytes;
    if ( response.size() < expectedResponseSize )
    {
        FWE_LOG_WARN( "Unexpected PID response length: " + std::to_string( pid ) );
        return;
    }
    response.resize( expectedResponseSize );
    if ( !mOBDDataDecoder.decodeEmissionPIDs( SID::CURRENT_STATS, pids, response, info ) )
    {
        return;
    }
    OBDOverCANECU::pushPIDs( info, mClock->systemTimeSinceEpochMs(), mSignalBufferDistributor, "" );
}

void
// coverity[autosar_cpp14_a8_4_11_violation] smart pointer needed to match the expected signature
OBDOverCANModule::onChangeInspectionMatrix( std::shared_ptr<const InspectionMatrix> inspectionMatrix )
{
    // We check here that at least one condition needs DTCs. If yes, we activate that.
    if ( inspectionMatrix )
    {
        for ( auto const &condition : inspectionMatrix->conditions )
        {
            if ( condition.includeActiveDtcs )
            {
                mShouldRequestDTCs.store( true, std::memory_order_relaxed );
                mDataAvailableWait.notify();
                FWE_LOG_INFO( "Requesting DTC is enabled" );
                return;
            }
        }
        // If we are here, that means no condition had DTC collection active. So disabling DTC requests.
        mShouldRequestDTCs.store( false, std::memory_order_relaxed );
        // No need to notify, thread will pick it up in the next cycle.
    }
}

void
OBDOverCANModule::onChangeOfActiveDictionary( ConstDecoderDictionaryConstPtr &dictionary,
                                              VehicleDataSourceProtocol networkProtocol )
{
    if ( networkProtocol != VehicleDataSourceProtocol::OBD )
    {
        return;
    }
    std::lock_guard<std::mutex> lock( mDecoderDictMutex );
    mDecoderDictionary.clear();
    // Here we up cast the decoder dictionary to CAN Decoder Dictionary to extract can decoder method
    auto canDecoderDictionaryPtr = std::dynamic_pointer_cast<const CANDecoderDictionary>( dictionary );
    if ( canDecoderDictionaryPtr == nullptr )
    {
        FWE_LOG_TRACE( "Received empty Decoder Manifest" );
        return;
    }
    // As OBD only has one port, we expect the decoder dictionary only has one channel
    if ( canDecoderDictionaryPtr->canMessageDecoderMethod.size() != 1 )
    {
        FWE_LOG_WARN( "Received Invalid Decoder Manifest, ignoring it" );
        return;
    }
    // Iterate through the received generic decoder dictionary to construct the OBD specific dictionary
    std::vector<PID> pidsRequestedByDecoderDict{};
    for ( const auto &canMessageDecoderMethod : canDecoderDictionaryPtr->canMessageDecoderMethod.cbegin()->second )
    {
        // The key is PID; The Value is decoder format
        mDecoderDictionary.emplace( canMessageDecoderMethod.first, canMessageDecoderMethod.second.format );
        // Check if this PID's decoder method contains signals to be collected by
        // Decoder Dictionary. If so, add the PID to pidsRequestedByDecoderDict
        // Note in worst case scenario when no OBD signals are to be collected, this will
        // iterate through the entire OBD signal lists which only contains a few hundreds signals.
        for ( const auto &signal : canMessageDecoderMethod.second.format.mSignals )
        {
            // if the signal is to be collected according to decoder dictionary, push
            // the corresponding PID to the pidsRequestedByDecoderDict
            if ( canDecoderDictionaryPtr->signalIDsToCollect.find( signal.mSignalID ) !=
                 canDecoderDictionaryPtr->signalIDsToCollect.end() )
            {
                pidsRequestedByDecoderDict.emplace_back( static_cast<PID>( canMessageDecoderMethod.first ) );
                // We know this PID needs to be requested, break to move on next PID
                break;
            }
        }
    }
    std::sort( pidsRequestedByDecoderDict.begin(), pidsRequestedByDecoderDict.end() );
    FWE_LOG_TRACE( "Decoder Dictionary requests PIDs: " + getStringFromBytes( pidsRequestedByDecoderDict ) );
    // For now we only support OBD Service Mode 1 PID
    mPIDsRequestedByDecoderDict[SID::CURRENT_STATS] = std::move( pidsRequestedByDecoderDict );

    // If the program already know the supported PIDs from ECU, below two update will update
    // For each ecu update the PIDs requested by the Dict
    assignPIDsToECUs();

    // Pass on the decoder manifest to the OBD Decoder and wake up the thread.
    // Before that we should interrupt the thread so that no further decoding
    // is done using the previous decoder, then assign the new decoder manifest,
    // the wake up the thread.
    mDecoderManifestAvailable.store( true, std::memory_order_relaxed );
    // Wake up the worker thread.
    mDataAvailableWait.notify();
    FWE_LOG_INFO( "Decoder Manifest Updated" );
}

} // namespace IoTFleetWise
} // namespace Aws

// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "aws/iotfleetwise/CANDataSource.h"
#include "aws/iotfleetwise/Assert.h"
#include "aws/iotfleetwise/EnumUtility.h"
#include "aws/iotfleetwise/LoggingModule.h"
#include "aws/iotfleetwise/TraceModule.h"
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ctime> // IWYU pragma: keep
#include <linux/can.h>
#include <linux/can/raw.h>
#include <linux/errqueue.h>
#include <linux/net_tstamp.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <utility>

#ifdef __COVERITY__
// coverity[misra_cpp_2008_rule_16_0_3_violation]
#undef CMSG_FIRSTHDR
// coverity[autosar_cpp14_a7_1_7_violation]
#define CMSG_FIRSTHDR( mhdr ) ( reinterpret_cast<struct cmsghdr *>( mhdr ) )
#endif

namespace Aws
{
namespace IoTFleetWise
{

CANDataSource::CANDataSource( CANChannelNumericID channelId,
                              CanTimestampType timestampTypeToUse,
                              std::string interfaceName,
                              bool forceCanFD,
                              uint32_t threadIdleTimeMs,
                              CANDataConsumer &consumer )
    : mIdleTimeMs{ threadIdleTimeMs }
    , mTimestampTypeToUse{ timestampTypeToUse }
    , mForceCanFD{ forceCanFD }
    , mChannelId{ channelId }
    , mIfName{ std::move( interfaceName ) }
    , mConsumer{ consumer }
{
}

CANDataSource::~CANDataSource()
{
    // To make sure the thread stops during teardown of tests.
    if ( isAlive() )
    {
        disconnect();
    }
}

bool
CANDataSource::start()
{
    // Prevent concurrent stop/init
    std::lock_guard<std::mutex> lock( mThreadMutex );
    // On multi core systems the shared variable mShouldStop must be updated for
    // all cores before starting the thread otherwise thread will directly end
    mShouldStop.store( false );
    if ( !mThread.create( [this]() {
             this->doWork();
         } ) )
    {
        FWE_LOG_TRACE( "CAN Data Source Thread failed to start" );
    }
    else
    {
        FWE_LOG_TRACE( "CAN Data Source Thread started" );
        // Thread name has channelID+1 to match legacy naming of threads in dashboards
        mThread.setThreadName( "fwVNLinuxCAN" + std::to_string( mChannelId + 1 ) );
    }
    return mThread.isActive() && mThread.isValid();
}

bool
CANDataSource::stop()
{
    std::lock_guard<std::mutex> lock( mThreadMutex );
    mShouldStop.store( true, std::memory_order_relaxed );
    mWait.notify();
    mThread.release();
    mShouldStop.store( false, std::memory_order_relaxed );
    FWE_LOG_TRACE( "Thread stopped" );
    return !mThread.isActive();
}

bool
CANDataSource::shouldStop() const
{
    return mShouldStop.load( std::memory_order_relaxed );
}

Timestamp
CANDataSource::extractTimestamp( struct msghdr &msgHeader )
{
    // This is a Linux header macro
    // coverity[misra_cpp_2008_rule_5_2_9_violation]
    // coverity[autosar_cpp14_m5_2_9_violation]
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
    struct cmsghdr *currentHeader = CMSG_FIRSTHDR( &msgHeader );
    Timestamp timestamp = 0;
    if ( mTimestampTypeToUse != CanTimestampType::POLLING_TIME )
    {
        while ( currentHeader != nullptr )
        {
            if ( currentHeader->cmsg_type == SO_TIMESTAMPING )
            {
                // With linux kernel 5.1 new return scm_timestamping64 was introduced
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
                scm_timestamping *timestampArray = (scm_timestamping *)( CMSG_DATA( currentHeader ) );
                // From https://www.kernel.org/doc/Documentation/networking/timestamping.txt
                // Most timestamps are passed in ts[0]. Hardware timestamps are passed in ts[2].
                if ( mTimestampTypeToUse == CanTimestampType::KERNEL_HARDWARE_TIMESTAMP )
                {
                    timestamp =
                        static_cast<Timestamp>( ( static_cast<Timestamp>( timestampArray->ts[2].tv_sec ) * 1000 ) +
                                                ( static_cast<Timestamp>( timestampArray->ts[2].tv_nsec ) / 1000000 ) );
                }
                else if ( mTimestampTypeToUse == CanTimestampType::KERNEL_SOFTWARE_TIMESTAMP ) // default
                {
                    timestamp =
                        static_cast<Timestamp>( ( static_cast<Timestamp>( timestampArray->ts[0].tv_sec ) * 1000 ) +
                                                ( static_cast<Timestamp>( timestampArray->ts[0].tv_nsec ) / 1000000 ) );
                }
            }
            currentHeader = CMSG_NXTHDR( &msgHeader, currentHeader );
        }
        TraceModule::get().setVariable( TraceVariable::MAX_SYSTEMTIME_KERNELTIME_DIFF,
                                        static_cast<uint64_t>( mClock->systemTimeSinceEpochMs() ) -
                                            static_cast<uint64_t>( timestamp ) );
    }
    if ( timestamp == 0 ) // either other timestamp are invalid(=0) or mTimestampTypeToUse == POLLING_TIME
    {
        TraceModule::get().incrementVariable( TraceVariable::POLLING_TIMESTAMP_COUNTER );
        timestamp = mClock->systemTimeSinceEpochMs();
    }
    return timestamp;
}

void
CANDataSource::doWork()
{
    // coverity[autosar_cpp14_a0_1_1_violation:FALSE] variable is used
    Timestamp lastFrameTime = 0;
    uint32_t activations = 0;
    bool wokeUpFromSleep =
        false; /**< This variable is true after the thread is woken up for example because a valid decoder manifest was
                  received until the thread sleeps for the next time when it is false again*/
    Timer logTimer;
    while ( true )
    {
        activations++;
        std::shared_ptr<const CANDecoderDictionary> decoderDictionary;
        {
            std::lock_guard<std::mutex> lock( mDecoderDictMutex );
            decoderDictionary = mDecoderDictionary;
        }
        if ( decoderDictionary == nullptr )
        {
            // We either just started or there was a decoder manifest update that we can't use
            // We should sleep
            FWE_LOG_TRACE( "No valid decoding dictionary available, Channel going to sleep" );
            mWait.wait( Signal::WaitWithPredicate );
            wokeUpFromSleep = true;
        }

        mTimer.reset();
        struct canfd_frame frame[PARALLEL_RECEIVED_FRAMES_FROM_KERNEL];
        struct iovec frame_buffer[PARALLEL_RECEIVED_FRAMES_FROM_KERNEL];
        struct mmsghdr msg[PARALLEL_RECEIVED_FRAMES_FROM_KERNEL];
        // we expect only one timestamp to return
        char cmsgReturnBuffer[PARALLEL_RECEIVED_FRAMES_FROM_KERNEL][CMSG_SPACE( sizeof( struct scm_timestamping ) )]{};

        // Setup all buffer to receive data
        for ( int i = 0; i < PARALLEL_RECEIVED_FRAMES_FROM_KERNEL; i++ )
        {
            frame_buffer[i].iov_base = &frame[i];
            frame_buffer[i].iov_len = sizeof( frame );
            msg[i].msg_hdr.msg_name = nullptr; // not interested in the source address
            msg[i].msg_hdr.msg_namelen = 0;
            msg[i].msg_hdr.msg_iov = &frame_buffer[i];
            msg[i].msg_hdr.msg_iovlen = 1;
            msg[i].msg_hdr.msg_control = &cmsgReturnBuffer[i];
            msg[i].msg_hdr.msg_controllen = sizeof( cmsgReturnBuffer[i] );
        }
        // In one syscall receive up to PARALLEL_RECEIVED_FRAMES_FROM_KERNEL frames in parallel
        int nmsgs = recvmmsg( mSocket, &msg[0], PARALLEL_RECEIVED_FRAMES_FROM_KERNEL, 0, nullptr );
        // coverity[autosar_cpp14_m19_3_1_violation]
        // coverity[misra_cpp_2008_rule_19_3_1_violation] errno needs to be used to recognize network down
        FWE_GRACEFUL_FATAL_ASSERT( ( nmsgs != -1 ) || ( errno != ENODEV ), "Network interface was removed", );
        // coverity[autosar_cpp14_m19_3_1_violation]
        // coverity[misra_cpp_2008_rule_19_3_1_violation] errno needs to be used to recognize network down
        if ( ( nmsgs == -1 ) && ( ( errno == ENETDOWN ) || ( errno == ENETUNREACH ) ) )
        {
            // coverity[autosar_cpp14_m19_3_1_violation]
            // coverity[misra_cpp_2008_rule_19_3_1_violation] errno needs to be used to recognize network down
            FWE_LOG_ERROR( "Network interface went down or unreachable with Syscall errno: " +
                           std::to_string( errno ) );
            // Not much to do here, Socket is still alive, when network is back, we continue to consume.
        }
        // Else, the socket is non blocking, so we might expect -1 as a nmsgs

        for ( int i = 0; i < nmsgs; i++ )
        {
            // After waking up the Socket Can, old messages in the kernel queue need to be ignored
            if ( !wokeUpFromSleep )
            {
                Timestamp timestamp = extractTimestamp( msg[i].msg_hdr );
                if ( timestamp < lastFrameTime )
                {
                    TraceModule::get().incrementAtomicVariable( TraceAtomicVariable::NOT_TIME_MONOTONIC_FRAMES );
                }
                lastFrameTime = timestamp;
                mReceivedMessages++;
                TraceVariable traceFrames = TraceVariable::READ_SOCKET_FRAMES_19; // Safe default ;
                uint32_t calculatedValue = mChannelId + toUType( TraceVariable::READ_SOCKET_FRAMES_0 );
                if ( calculatedValue <= toUType( TraceVariable::READ_SOCKET_FRAMES_19 ) )
                {
                    traceFrames = static_cast<TraceVariable>( calculatedValue );
                }
                TraceModule::get().setVariable( traceFrames, mReceivedMessages );
                std::lock_guard<std::mutex> lock( mDecoderDictMutex );
                mConsumer.processMessage(
                    mChannelId, decoderDictionary.get(), frame[i].can_id, frame[i].data, frame[i].len, timestamp );
            }
        }
        if ( nmsgs < PARALLEL_RECEIVED_FRAMES_FROM_KERNEL )
        {
            if ( logTimer.getElapsedMs().count() > static_cast<int64_t>( LoggingModule::LOG_AGGREGATION_TIME_MS ) )
            {
                // Nothing is in the ring buffer to consume. Go to idle mode for some time.
                FWE_LOG_TRACE( "Activations: " + std::to_string( activations ) +
                               ". Waiting for some data to come. Idling for: " + std::to_string( mIdleTimeMs ) +
                               " ms, processed " + std::to_string( mReceivedMessages ) + " frames" );
                activations = 0;
                logTimer.reset();
            }
            mWait.wait( static_cast<uint32_t>( mIdleTimeMs ) );
            // coverity[autosar_cpp14_a0_1_1_violation:FALSE] variable is used
            wokeUpFromSleep = false;
        }
        if ( shouldStop() )
        {
            break;
        }
    }
}

bool
CANDataSource::connect()
{
    mTimer.reset();

    // Socket CAN parameters
    struct sockaddr_can interfaceAddress = {};
    struct ifreq interfaceRequest = {};
    // Open a Socket but make sure it's not blocking to not
    // cause a thread hang.
    int canfd_on = 1;
    int type = SOCK_RAW | SOCK_NONBLOCK;
    mSocket = socket( PF_CAN, type, CAN_RAW );
    if ( mSocket < 0 )
    {
        FWE_LOG_ERROR( "Failed to create socket: " + getErrnoString() );
        return false;
    }
    // Switch Socket can_fd mode on or fallback with a log if it fails
    if ( setsockopt( mSocket, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &canfd_on, sizeof( canfd_on ) ) != 0 )
    {
        if ( mForceCanFD )
        {
            FWE_LOG_ERROR( "setsockopt CAN_RAW_FD_FRAMES FAILED" );
            return false;
        }
        else
        {
            FWE_LOG_INFO( "setsockopt CAN_RAW_FD_FRAMES FAILED, falling back to regular CAN" );
        }
    }

    // Set the IF Name, address
    if ( mIfName.size() >= sizeof( interfaceRequest.ifr_name ) )
    {
        return false;
    }
    (void)strncpy( interfaceRequest.ifr_name, mIfName.c_str(), sizeof( interfaceRequest.ifr_name ) - 1U );

    if ( ioctl( mSocket, SIOCGIFINDEX, &interfaceRequest ) != 0 )
    {
        FWE_LOG_ERROR( "CAN Interface with name " + mIfName + " is not accessible" );
        close( mSocket );
        return false;
    }
    if ( ( mTimestampTypeToUse == CanTimestampType::KERNEL_SOFTWARE_TIMESTAMP ) ||
         ( mTimestampTypeToUse == CanTimestampType::KERNEL_HARDWARE_TIMESTAMP ) )
    {
        const int timestampFlags = ( SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RX_SOFTWARE |
                                     SOF_TIMESTAMPING_SOFTWARE | SOF_TIMESTAMPING_RAW_HARDWARE );
        if ( setsockopt( mSocket, SOL_SOCKET, SO_TIMESTAMPING, &timestampFlags, sizeof( timestampFlags ) ) != 0 )
        {
            FWE_LOG_ERROR( "Hardware timestamp not supported by socket but requested by config" );
            close( mSocket );
            return false;
        }
    }

    interfaceAddress.can_family = AF_CAN;
    interfaceAddress.can_ifindex = interfaceRequest.ifr_ifindex;

    // Bind the socket
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
    if ( bind( mSocket, (struct sockaddr *)&interfaceAddress, sizeof( interfaceAddress ) ) < 0 )
    {
        FWE_LOG_ERROR( "Failed to bind socket: " + getErrnoString() );
        close( mSocket );
        return false;
    }
    // Start the main thread.
    return start();
}

bool
CANDataSource::disconnect()
{
    return stop() && ( close( mSocket ) == 0 );
}

bool
CANDataSource::isAlive()
{
    int error = 0;
    socklen_t len = sizeof( error );
    // Get the error status of the socket
    int retSockOpt = getsockopt( mSocket, SOL_SOCKET, SO_ERROR, &error, &len );
    if ( ( retSockOpt == -1 ) || ( !mThread.isValid() ) || ( !mThread.isActive() ) || ( error != 0 ) )
    {
        return false;
    }
    return true;
}

void
CANDataSource::onChangeOfActiveDictionary( ConstDecoderDictionaryConstPtr &dictionary,
                                           VehicleDataSourceProtocol networkProtocol )
{
    if ( networkProtocol != VehicleDataSourceProtocol::RAW_SOCKET )
    {
        return;
    }
    std::lock_guard<std::mutex> lock( mDecoderDictMutex );
    mDecoderDictionary = std::dynamic_pointer_cast<const CANDecoderDictionary>( dictionary );
    if ( dictionary == nullptr )
    {
        FWE_LOG_TRACE( "CAN Data Source : " + std::to_string( mChannelId ) +
                       " thread going to sleep until the decoder dictionary becomes active." );
    }
    else
    {
        FWE_LOG_TRACE( "Resuming Network data acquisition on Data Source: " + std::to_string( mChannelId ) );
        // Wake up the worker thread.
        mWait.notify();
    }
}

} // namespace IoTFleetWise
} // namespace Aws

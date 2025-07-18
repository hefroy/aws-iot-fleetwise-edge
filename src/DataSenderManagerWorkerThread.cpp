// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "aws/iotfleetwise/DataSenderManagerWorkerThread.h"
#include "aws/iotfleetwise/DataSenderTypes.h"
#include "aws/iotfleetwise/LoggingModule.h"
#include "aws/iotfleetwise/QueueTypes.h"
#include <algorithm>
#include <memory>
#include <string>
#include <utility>

namespace Aws
{
namespace IoTFleetWise
{

// coverity[autosar_cpp14_a0_1_1_violation:FALSE] variable is used
const uint32_t DataSenderManagerWorkerThread::MAX_NUMBER_OF_SIGNAL_TO_TRACE_LOG = 6;

DataSenderManagerWorkerThread::DataSenderManagerWorkerThread(
    const IConnectivityModule &connectivityModule,
    std::unique_ptr<DataSenderManager> dataSenderManager,
    uint64_t persistencyUploadRetryIntervalMs,
    std::vector<std::shared_ptr<DataSenderQueue>> dataToSendQueues )
    : mDataToSendQueues( std::move( dataToSendQueues ) )
    , mPersistencyUploadRetryIntervalMs{ persistencyUploadRetryIntervalMs }
    , mDataSenderManager( std::move( dataSenderManager ) )
    , mConnectivityModule( connectivityModule )
{
}

bool
DataSenderManagerWorkerThread::start()
{
    // Prevent concurrent stop/init
    std::lock_guard<std::mutex> lock( mThreadMutex );
    mShouldStop.store( false );
    if ( !mThread.create( [this]() {
             this->doWork();
         } ) )
    {
        FWE_LOG_TRACE( "Data Sender Manager Thread failed to start" );
    }
    else
    {
        FWE_LOG_TRACE( "Data Sender Manager Thread started" );
        mThread.setThreadName( "fwDSDataSendMng" );
    }

    return mThread.isActive() && mThread.isValid();
}

bool
DataSenderManagerWorkerThread::stop()
{
    // It might take several seconds to finish all running S3 async PutObject requests
    if ( ( !mThread.isValid() ) || ( !mThread.isActive() ) )
    {
        return true;
    }
    std::lock_guard<std::mutex> lock( mThreadMutex );
    mShouldStop.store( true, std::memory_order_relaxed );
    FWE_LOG_TRACE( "Request stop" );
    mWait.notify();
    mThread.release();
    FWE_LOG_TRACE( "Stop finished" );
    mShouldStop.store( false, std::memory_order_relaxed );
    return !mThread.isActive();
}

bool
DataSenderManagerWorkerThread::shouldStop() const
{
    return mShouldStop.load( std::memory_order_relaxed );
}

void
DataSenderManagerWorkerThread::doWork()
{
    bool uploadedPersistedDataOnce = false;

    while ( !shouldStop() )
    {
        mTimer.reset();
        uint64_t minTimeToWaitMs = UINT64_MAX;

        if ( mPersistencyUploadRetryIntervalMs > 0 )
        {
            uint64_t timeToWaitMs =
                mPersistencyUploadRetryIntervalMs -
                std::min( static_cast<uint64_t>( mRetrySendingPersistedDataTimer.getElapsedMs().count() ),
                          mPersistencyUploadRetryIntervalMs );
            minTimeToWaitMs = std::min( minTimeToWaitMs, timeToWaitMs );
        }

        if ( minTimeToWaitMs < UINT64_MAX )
        {
            FWE_LOG_TRACE( "Waiting for: " + std::to_string( minTimeToWaitMs ) + " ms. Persistency " +
                           std::to_string( mPersistencyUploadRetryIntervalMs ) + " configured, " +
                           std::to_string( mRetrySendingPersistedDataTimer.getElapsedMs().count() ) + " timer." );
            mWait.wait( static_cast<uint32_t>( minTimeToWaitMs ) );
        }
        else
        {
            mWait.wait( Signal::WaitWithPredicate );
            auto elapsedTimeMs = mTimer.getElapsedMs().count();
            FWE_LOG_TRACE( "Event arrived. Time elapsed waiting for the event: " + std::to_string( elapsedTimeMs ) +
                           " ms" );
        }

        for ( auto &queue : mDataToSendQueues )
        {
            queue->consumeAll(
                // coverity[autosar_cpp14_a8_4_11_violation] smart pointer needed to match the expected signature
                [this]( std::shared_ptr<const DataToSend> dataToSend ) {
                    mDataSenderManager->processData( *dataToSend );
                } );
        }

        if ( ( !uploadedPersistedDataOnce ) ||
             ( ( mPersistencyUploadRetryIntervalMs > 0 ) &&
               ( static_cast<uint64_t>( mRetrySendingPersistedDataTimer.getElapsedMs().count() ) >=
                 mPersistencyUploadRetryIntervalMs ) ) )
        {
            mRetrySendingPersistedDataTimer.reset();
            if ( mConnectivityModule.isAlive() )
            {
                mDataSenderManager->checkAndSendRetrievedData();
                uploadedPersistedDataOnce = true;
            }
        }
    }
}

bool
DataSenderManagerWorkerThread::isAlive()
{
    return mThread.isValid() && mThread.isActive();
}

void
DataSenderManagerWorkerThread::onDataReadyToPublish()
{
    mWait.notify();
}

DataSenderManagerWorkerThread::~DataSenderManagerWorkerThread()
{
    // To make sure the thread stops during teardown of tests.
    if ( isAlive() )
    {
        stop();
    }
}

} // namespace IoTFleetWise
} // namespace Aws

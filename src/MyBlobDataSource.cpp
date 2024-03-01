// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "MyBlobDataSource.h"
#include "LoggingModule.h"
#include <chrono>
#include <utility>
#include <vector>

namespace Aws
{
namespace IoTFleetWise
{

MyBlobDataSource::MyBlobDataSource( SignalBufferPtr signalBufferPtr,
                                    std::shared_ptr<RawData::BufferManager> rawDataBufferManager )
    : mSignalBufferPtr( std::move( signalBufferPtr ) )
    , mRawBufferManager( std::move( rawDataBufferManager ) )
{
    mThread = std::thread( [this]() {
        // Example code that pushes a message every 500ms:
        while ( !mStop )
        {
            pushData();
            std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
        }
    } );
}

MyBlobDataSource::~MyBlobDataSource()
{
    mStop = true;
    mThread.join();
}

void
MyBlobDataSource::pushData()
{
    std::lock_guard<std::mutex> lock( mDecoderDictionaryMutex );
    if ( mBlobSourceSignalId == INVALID_SIGNAL_ID )
    {
        FWE_LOG_TRACE( "No decoding info yet" );
        return;
    }

    FWE_LOG_TRACE( "Pushing blob" );
    // Example blob data:
    std::vector<uint8_t> blob{ 'H', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd', '!' };

    // Serialize in the CDR format defined in blob-nodes.json and blob-decoders.json which is just a
    // message containing a single byte array of unstructured (blob) data:
    // Add CDR header:
    std::vector<uint8_t> cdr{
        0, // UINT8 Dummy byte
        1, // UINT8 Encapsulation
        0, // UINT16-LSB Options
        0, // UINT16-MSB Options
    };
    // Add the UINT32 blob size:
    cdr.push_back( blob.size() & 0xFF );
    cdr.push_back( ( blob.size() >> 8 ) & 0xFF );
    cdr.push_back( ( blob.size() >> 16 ) & 0xFF );
    cdr.push_back( ( blob.size() >> 24 ) & 0xFF );
    // Add blob data:
    cdr.insert( cdr.end(), blob.begin(), blob.end() );

    // Ingest the message:
    auto timestamp = mClock->systemTimeSinceEpochMs();
    auto bufferHandle = mRawBufferManager->push( cdr.data(), cdr.size(), timestamp, mBlobSourceSignalId );
    if ( bufferHandle == RawData::INVALID_BUFFER_HANDLE )
    {
        FWE_LOG_WARN( "Raw message was rejected by RawBufferManager" );
        return;
    }
    // immediately set usage hint so buffer handle does not get directly deleted again
    mRawBufferManager->increaseHandleUsageHint(
        mBlobSourceSignalId, bufferHandle, RawData::BufferHandleUsageStage::COLLECTED_NOT_IN_HISTORY_BUFFER );
    auto collectedSignal =
        CollectedSignal( mBlobSourceSignalId, timestamp, bufferHandle, SignalType::RAW_DATA_BUFFER_HANDLE );
    CollectedSignalsGroup collectedSignalsGroup;
    collectedSignalsGroup.push_back( collectedSignal );
    if ( !mSignalBufferPtr->push( CollectedDataFrame( collectedSignalsGroup ) ) )
    {
        FWE_LOG_WARN( "Signal buffer full" );
    }
}

void
MyBlobDataSource::onChangeOfActiveDictionary( ConstDecoderDictionaryConstPtr &dictionary,
                                              VehicleDataSourceProtocol networkProtocol )
{
    if ( networkProtocol != VehicleDataSourceProtocol::COMPLEX_DATA )
    {
        return;
    }
    std::lock_guard<std::mutex> lock( mDecoderDictionaryMutex );
    mBlobSourceSignalId = INVALID_SIGNAL_ID;
    auto decoderDictionaryPtr = std::dynamic_pointer_cast<const ComplexDataDecoderDictionary>( dictionary );
    auto decoders = decoderDictionaryPtr->complexMessageDecoderMethod.find( BLOB_NETWORK_INTERFACE_ID );
    if ( decoders == decoderDictionaryPtr->complexMessageDecoderMethod.end() )
    {
        FWE_LOG_INFO( std::string( "No decoders found for interface ID " ) + BLOB_NETWORK_INTERFACE_ID );
        return;
    }
    auto decoder = decoders->second.find( BLOB_MESSAGE_ID );
    if ( decoder == decoders->second.end() )
    {
        FWE_LOG_INFO( std::string( "No decoder found for message ID " ) + BLOB_MESSAGE_ID );
        return;
    }
    mBlobSourceSignalId = decoder->second.mSignalId;
    FWE_LOG_INFO( "Signal ID for blob is " + std::to_string( mBlobSourceSignalId ) );

    // Note that there's no sanity check of the message format here, so if it doesn't match the
    // format pushed in pushData(), then cloud won't understand it.
}

} // namespace IoTFleetWise
} // namespace Aws

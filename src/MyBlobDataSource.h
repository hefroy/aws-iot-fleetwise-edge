// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Clock.h"
#include "ClockHandler.h"
#include "CollectionInspectionAPITypes.h"
#include "IDecoderDictionary.h"
#include "RawDataManager.h"
#include "VehicleDataSourceTypes.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace Aws
{
namespace IoTFleetWise
{

class MyBlobDataSource
{
public:
    MyBlobDataSource( SignalBufferPtr signalBufferPtr, std::shared_ptr<RawData::BufferManager> rawDataBufferManager );
    ~MyBlobDataSource();

    MyBlobDataSource( const MyBlobDataSource & ) = delete;
    MyBlobDataSource &operator=( const MyBlobDataSource & ) = delete;
    MyBlobDataSource( MyBlobDataSource && ) = delete;
    MyBlobDataSource &operator=( MyBlobDataSource && ) = delete;

    void onChangeOfActiveDictionary( ConstDecoderDictionaryConstPtr &dictionary,
                                     VehicleDataSourceProtocol networkProtocol );

private:
    // Note these must match the interface and message IDs sent in the decoder manifest from the cloud:
    static constexpr const char *BLOB_NETWORK_INTERFACE_ID = "MyBlobNetworkInterfaceId";
    static constexpr const char *BLOB_MESSAGE_ID = "MyBlobMessageId";

    std::mutex mDecoderDictionaryMutex;
    SignalID mBlobSourceSignalId{ INVALID_SIGNAL_ID };
    SignalBufferPtr mSignalBufferPtr;
    std::shared_ptr<RawData::BufferManager> mRawBufferManager;
    std::thread mThread;
    std::atomic_bool mStop{ false };
    std::shared_ptr<const Clock> mClock = ClockHandler::getClock();

    void pushData();
};

} // namespace IoTFleetWise
} // namespace Aws

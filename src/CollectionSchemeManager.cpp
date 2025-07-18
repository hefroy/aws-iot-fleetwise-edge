// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "aws/iotfleetwise/CollectionSchemeManager.h"
#include "aws/iotfleetwise/ICollectionScheme.h"
#include "aws/iotfleetwise/LoggingModule.h"
#include "aws/iotfleetwise/TraceModule.h"
#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Aws
{
namespace IoTFleetWise
{

CollectionSchemeManager::CollectionSchemeManager( std::shared_ptr<CacheAndPersist> schemaPersistencyPtr,
                                                  CANInterfaceIDTranslator &canIDTranslator,
                                                  std::shared_ptr<CheckinSender> checkinSender,
                                                  RawData::BufferManager *rawDataBufferManager
#ifdef FWE_FEATURE_REMOTE_COMMANDS
                                                  ,
                                                  GetActuatorNamesCallback getActuatorNamesCallback
#endif
                                                  ,
                                                  uint32_t idleTimeMs )
    : mCheckinSender( std::move( checkinSender ) )
    , mRawDataBufferManager( rawDataBufferManager )
#ifdef FWE_FEATURE_REMOTE_COMMANDS
    , mGetActuatorNamesCallback( std::move( getActuatorNamesCallback ) )
#endif
    , mSchemaPersistency( std::move( schemaPersistencyPtr ) )
    , mCANIDTranslator( canIDTranslator )
{
    if ( idleTimeMs != 0 )
    {
        mIdleTimeMs = idleTimeMs;
    }
}

CollectionSchemeManager::~CollectionSchemeManager()
{
    // To make sure the thread stops during teardown of tests.
    if ( isAlive() )
    {
        stop();
    }
    mEnabledCollectionSchemeMap.clear();
    mIdleCollectionSchemeMap.clear();
    while ( !mTimeLine.empty() )
    {
        mTimeLine.pop();
    }
}

bool
CollectionSchemeManager::start()
{
    std::lock_guard<std::mutex> lock( mThreadMutex );
    mShouldStop.store( false );
    if ( !mThread.create( [this]() {
             this->doWork();
         } ) )
    {
        FWE_LOG_ERROR( "Thread failed to start" );
    }
    else
    {
        FWE_LOG_INFO( "Thread started" );
        mThread.setThreadName( "fwDMColSchMngr" );
    }
    return mThread.isValid();
}

bool
CollectionSchemeManager::stop()
{
    std::lock_guard<std::mutex> lock( mThreadMutex );
    mShouldStop.store( true, std::memory_order_relaxed );
    /*
     * When main thread is servicing a collectionScheme, it sets up timer
     * and wakes up only when timer expires. If main thread needs to
     * be stopped any time, use notify() to wake up
     * immediately.
     */
    mWait.notify();
    mThread.release();
    mShouldStop.store( false, std::memory_order_relaxed );
    FWE_LOG_INFO( "Collection Scheme Thread stopped" );
    return true;
}

bool
CollectionSchemeManager::shouldStop() const
{
    return mShouldStop.load( std::memory_order_relaxed );
}

/* supporting functions for logging */
void
CollectionSchemeManager::printEventLogMsg( std::string &msg,
                                           const SyncID &id,
                                           const Timestamp &startTime,
                                           const Timestamp &stopTime,
                                           const TimePoint &currTime )
{
    msg += "ID( " + id + " )";
    msg += "Start( " + std::to_string( startTime ) + " milliseconds )";
    msg += "Stop( " + std::to_string( stopTime ) + " milliseconds )";
    msg += "at Current System Time ( " + std::to_string( currTime.systemTimeMs ) + " milliseconds ).";
    msg += "at Current Monotonic Time ( " + std::to_string( currTime.monotonicTimeMs ) + " milliseconds ).";
}

void
CollectionSchemeManager::printExistingCollectionSchemes( std::string &enableStr, std::string &idleStr )
{
    enableStr = "Enabled: ";
    idleStr = "Idle: ";
    for ( auto it = mEnabledCollectionSchemeMap.begin(); it != mEnabledCollectionSchemeMap.end(); it++ )
    {
        enableStr += it->second->getCollectionSchemeID();
        enableStr += ' ';
    }
    for ( auto it = mIdleCollectionSchemeMap.begin(); it != mIdleCollectionSchemeMap.end(); it++ )
    {
        idleStr += it->second->getCollectionSchemeID();
        idleStr += ' ';
    }
}

void
CollectionSchemeManager::printWakeupStatus( std::string &wakeupStr ) const
{
    wakeupStr = "Waking up to update the CollectionScheme: ";
    wakeupStr += mProcessCollectionScheme ? "Yes" : "No";
    wakeupStr += ", the DecoderManifest: ";
    wakeupStr += mProcessDecoderManifest ? "Yes" : "No";
#ifdef FWE_FEATURE_LAST_KNOWN_STATE
    wakeupStr += ", the StateTemplate: ";
    wakeupStr += mProcessStateTemplates ? "Yes" : "No";
#endif
}

void
CollectionSchemeManager::doWork()
{
    // Retrieve data from persistent storage
    static_cast<void>( retrieve( DataType::COLLECTION_SCHEME_LIST ) );
    static_cast<void>( retrieve( DataType::DECODER_MANIFEST ) );
#ifdef FWE_FEATURE_LAST_KNOWN_STATE
    static_cast<void>( retrieve( DataType::STATE_TEMPLATE_LIST ) );
#endif
    bool initialCheckinDocumentsUpdate = true;
    while ( true )
    {
        bool decoderManifestChanged = false;
        bool enabledCollectionSchemeMapChanged = false;
#ifdef FWE_FEATURE_LAST_KNOWN_STATE
        bool stateTemplatesChanged = false;
#endif
        if ( mProcessDecoderManifest )
        {
            mProcessDecoderManifest = false;
            TraceModule::get().sectionBegin( TraceSection::MANAGER_DECODER_BUILD );
            if ( processDecoderManifest() )
            {
                decoderManifestChanged = true;
            }
            TraceModule::get().sectionEnd( TraceSection::MANAGER_DECODER_BUILD );
        }
        if ( mProcessCollectionScheme )
        {
            mProcessCollectionScheme = false;
            TraceModule::get().sectionBegin( TraceSection::MANAGER_COLLECTION_BUILD );
            if ( processCollectionScheme() )
            {
                enabledCollectionSchemeMapChanged = true;
            }
            TraceModule::get().sectionEnd( TraceSection::MANAGER_COLLECTION_BUILD );
        }
#ifdef FWE_FEATURE_LAST_KNOWN_STATE
        if ( mProcessStateTemplates )
        {
            mProcessStateTemplates = false;
            TraceModule::get().sectionBegin( TraceSection::MANAGER_LAST_KNOWN_STATE_BUILD );
            if ( processStateTemplates() )
            {
                stateTemplatesChanged = true;
            }
            TraceModule::get().sectionEnd( TraceSection::MANAGER_LAST_KNOWN_STATE_BUILD );
        }
#endif
        auto checkTime = mClock->timeSinceEpoch();
        if ( checkTimeLine( checkTime ) )
        {
            enabledCollectionSchemeMapChanged = true;
        }

        bool documentsChanged = decoderManifestChanged
#ifdef FWE_FEATURE_LAST_KNOWN_STATE
                                || stateTemplatesChanged
#endif
                                || enabledCollectionSchemeMapChanged;

        if ( documentsChanged || initialCheckinDocumentsUpdate )
        {
            initialCheckinDocumentsUpdate = false;
            updateCheckinDocuments();
        }

        if ( documentsChanged )
        {
            TraceModule::get().sectionBegin( TraceSection::MANAGER_EXTRACTION );
            FWE_LOG_TRACE( "Start extraction at system time " + std::to_string( checkTime.systemTimeMs ) );
            auto inspectionMatrixOutput = std::make_shared<InspectionMatrix>();
            auto fetchMatrixOutput = std::make_shared<FetchMatrix>();
            if ( decoderManifestChanged || enabledCollectionSchemeMapChanged )
            {
                TraceModule::get().sectionBegin( TraceSection::COLLECTION_SCHEME_CHANGE_TO_FIRST_DATA );

                // Extract InspectionMatrix and FetchMatrix from mEnabledCollectionSchemeMap
                updateActiveCollectionSchemeListeners();
                matrixExtractor( *inspectionMatrixOutput, *fetchMatrixOutput );
                std::string enabled;
                std::string idle;
                printExistingCollectionSchemes( enabled, idle );
                FWE_LOG_INFO( "FWE activated collection schemes:" + enabled +
                              " using decoder manifest:" + mCurrentDecoderManifestID + " resulting in " +
                              std::to_string( inspectionMatrixOutput->conditions.size() ) + " inspection conditions" );
            }

#ifdef FWE_FEATURE_LAST_KNOWN_STATE
            if ( decoderManifestChanged || stateTemplatesChanged )
            {
                lastKnownStateUpdater( lastKnownStateExtractor() );
            }
#endif

            // Extract decoder dictionary
            std::map<VehicleDataSourceProtocol, std::shared_ptr<DecoderDictionary>> decoderDictionaryMap;
            decoderDictionaryExtractor( decoderDictionaryMap
#ifdef FWE_FEATURE_VISION_SYSTEM_DATA
                                        ,
                                        *inspectionMatrixOutput
#endif
            );

            // Only notify the listeners after both have been extracted since the decoder dictionary
            // extraction might have modified the inspection matrix.
            decoderDictionaryUpdater( decoderDictionaryMap );
            if ( decoderManifestChanged || enabledCollectionSchemeMapChanged )
            {
                inspectionMatrixUpdater( inspectionMatrixOutput );
                fetchMatrixUpdater( fetchMatrixOutput );
            }

            // Update the Raw Buffer Config
            if ( mRawDataBufferManager != nullptr )
            {
                std::unordered_map<RawData::BufferTypeId, RawData::SignalUpdateConfig> updatedSignals;
                updateRawDataBufferConfigStringSignals( updatedSignals );

#ifdef FWE_FEATURE_VISION_SYSTEM_DATA
                std::shared_ptr<ComplexDataDecoderDictionary> complexDataDictionary;
                auto decoderDictionary = decoderDictionaryMap.find( VehicleDataSourceProtocol::COMPLEX_DATA );
                if ( decoderDictionary != decoderDictionaryMap.end() && decoderDictionary->second != nullptr )
                {
                    complexDataDictionary =
                        std::dynamic_pointer_cast<ComplexDataDecoderDictionary>( decoderDictionary->second );
                    if ( complexDataDictionary == nullptr )
                    {
                        FWE_LOG_WARN( "Could not cast dictionary to ComplexDataDecoderDictionary" );
                    }
                }
                updateRawDataBufferConfigComplexSignals( complexDataDictionary.get(), updatedSignals );
#endif
                FWE_LOG_INFO( "Updating raw buffer configuration for " + std::to_string( updatedSignals.size() ) +
                              " signals" );
                mRawDataBufferManager->updateConfig( updatedSignals );
            }

            std::string decoderCanChannels = "0";
            if ( decoderDictionaryMap.find( VehicleDataSourceProtocol::RAW_SOCKET ) != decoderDictionaryMap.end() )
            {
                auto canDecoderDictionary = std::dynamic_pointer_cast<CANDecoderDictionary>(
                    decoderDictionaryMap[VehicleDataSourceProtocol::RAW_SOCKET] );
                if ( canDecoderDictionary != nullptr )
                {
                    decoderCanChannels = std::to_string( canDecoderDictionary->canMessageDecoderMethod.size() );
                }
            }
            std::string obdPids = "0";
            if ( decoderDictionaryMap.find( VehicleDataSourceProtocol::OBD ) != decoderDictionaryMap.end() )
            {
                auto obdDecoderDictionary = std::dynamic_pointer_cast<CANDecoderDictionary>(
                    decoderDictionaryMap[VehicleDataSourceProtocol::OBD] );
                if ( obdDecoderDictionary != nullptr && ( !obdDecoderDictionary->canMessageDecoderMethod.empty() ) )
                {
                    obdPids = std::to_string( obdDecoderDictionary->canMessageDecoderMethod.cbegin()->second.size() );
                }
            }
            FWE_LOG_INFO( "FWE activated Decoder Manifest:" + std::string( " using decoder manifest:" ) +
                          mCurrentDecoderManifestID + " resulting in decoding rules for " +
                          std::to_string( decoderDictionaryMap.size() ) +
                          " protocols. Decoder CAN channels: " + decoderCanChannels + " and OBD PIDs:" + obdPids );
            TraceModule::get().sectionEnd( TraceSection::MANAGER_EXTRACTION );
        }
        // get next timePoint from the minHeap top
        // check if it is a valid timePoint, it can be obsoleted if start Time or stop Time gets updated
        // Note that we intentionally use the system time instead of monotonic time because all elements
        // we add to the timeline need to happen at a specific point in time, there are no elements that
        // need to be handled in an interval. So if the system time ever changes, it means the elements
        // in the timeline should also be handled earlier or later depending on how the time jumped.
        auto currentSystemTime = mClock->systemTimeSinceEpochMs();
        if ( mTimeLine.empty() )
        {
            mWait.wait( Signal::WaitWithPredicate );
        }
        else if ( currentSystemTime >= mTimeLine.top().time.systemTimeMs )
        {
            // Next checkin time has already expired
        }
        else
        {
            uint32_t waitTimeMs =
                std::min( static_cast<uint32_t>( mTimeLine.top().time.systemTimeMs - currentSystemTime ), mIdleTimeMs );
            FWE_LOG_TRACE( "Going to wait for " + std::to_string( waitTimeMs ) + " ms" );
            mWait.wait( waitTimeMs );
        }
        /* now it is either timer expires, an update arrives from PI, or stop() is called */
        updateAvailable();
        std::string wakeupStr;
        printWakeupStatus( wakeupStr );
        FWE_LOG_TRACE( wakeupStr );
        if ( shouldStop() )
        {
            break;
        }
    }
}

void
CollectionSchemeManager::updateCheckinDocuments()
{
    // Create a list of active collectionSchemes and the current decoder manifest and send it to cloud
    std::vector<SyncID> checkinMsg;
    for ( auto it = mEnabledCollectionSchemeMap.begin(); it != mEnabledCollectionSchemeMap.end(); it++ )
    {
        checkinMsg.emplace_back( it->first );
    }
    for ( auto it = mIdleCollectionSchemeMap.begin(); it != mIdleCollectionSchemeMap.end(); it++ )
    {
        checkinMsg.emplace_back( it->first );
    }
    if ( !mCurrentDecoderManifestID.empty() )
    {
        checkinMsg.emplace_back( mCurrentDecoderManifestID );
    }
#ifdef FWE_FEATURE_LAST_KNOWN_STATE
    for ( auto &stateTemplate : mStateTemplates )
    {
        checkinMsg.emplace_back( stateTemplate.second->id );
    }
#endif

    mCheckinSender->onCheckinDocumentsChanged( checkinMsg );
}

/* callback function */
void
CollectionSchemeManager::onCollectionSchemeUpdate( std::shared_ptr<ICollectionSchemeList> collectionSchemeList )
{
    std::lock_guard<std::mutex> lock( mSchemaUpdateMutex );
    mCollectionSchemeListInput = std::move( collectionSchemeList );
    mCollectionSchemeAvailable = true;
    mWait.notify();
}

void
CollectionSchemeManager::onDecoderManifestUpdate( std::shared_ptr<IDecoderManifest> decoderManifest )
{
    std::lock_guard<std::mutex> lock( mSchemaUpdateMutex );
    mDecoderManifestInput = std::move( decoderManifest );
    mDecoderManifestAvailable = true;
    mWait.notify();
}

#ifdef FWE_FEATURE_LAST_KNOWN_STATE
void
CollectionSchemeManager::onStateTemplatesChanged( std::shared_ptr<LastKnownStateIngestion> lastKnownStateIngestion )
{
    std::lock_guard<std::mutex> lock( mSchemaUpdateMutex );
    mLastKnownStateIngestionInput = std::move( lastKnownStateIngestion );
    mStateTemplatesAvailable = true;
    mWait.notify();
}
#endif

void
CollectionSchemeManager::updateAvailable()
{
    std::lock_guard<std::mutex> lock( mSchemaUpdateMutex );
    if ( mCollectionSchemeAvailable && mCollectionSchemeListInput != nullptr )
    {
        mCollectionSchemeList = mCollectionSchemeListInput;
        mProcessCollectionScheme = true;
    }
    mCollectionSchemeAvailable = false;
    if ( mDecoderManifestAvailable && mDecoderManifestInput != nullptr )
    {
        mDecoderManifest = mDecoderManifestInput;
        mProcessDecoderManifest = true;
    }
    mDecoderManifestAvailable = false;
#ifdef FWE_FEATURE_LAST_KNOWN_STATE
    if ( mStateTemplatesAvailable && mLastKnownStateIngestionInput != nullptr )
    {
        mLastKnownStateIngestion = mLastKnownStateIngestionInput;
        mProcessStateTemplates = true;
    }
    mStateTemplatesAvailable = false;
#endif
}

bool
CollectionSchemeManager::connect()
{
    return start();
}

bool
CollectionSchemeManager::disconnect()
{
    return stop();
}

bool
CollectionSchemeManager::isAlive()
{
    return mThread.isValid() && mThread.isActive();
}

bool
CollectionSchemeManager::isCollectionSchemeLoaded()
{
    return ( ( !mEnabledCollectionSchemeMap.empty() ) || ( !mIdleCollectionSchemeMap.empty() ) );
}

/*
 * This function starts from protobuf-ed decodermanifest, and
 * runs through the following steps:
 * a. build decodermanifest
 * b. check if decodermanifest changes. Change of decodermanifest invokes
 * cleanup of collectionSchemeMaps.
 *
 * returns true when mEnabledCollectionSchemeMap changes.
 */
bool
CollectionSchemeManager::processDecoderManifest()
{
    if ( ( mDecoderManifest == nullptr ) || ( !mDecoderManifest->build() ) )
    {
        FWE_LOG_ERROR( " Failed to process the upcoming DecoderManifest." );
        TraceModule::get().incrementAtomicVariable( TraceAtomicVariable::COLLECTION_SCHEME_ERROR );
        return false;
    }
    // build is successful
    if ( mDecoderManifest->getID() == mCurrentDecoderManifestID )
    {
        FWE_LOG_TRACE( "Ignoring new decoder manifest with same name: " + mCurrentDecoderManifestID );
        // no change in decoder manifest
        return false;
    }
    FWE_LOG_TRACE( "Replace decoder manifest " + mCurrentDecoderManifestID + " with " + mDecoderManifest->getID() +
                   " while " + std::to_string( mEnabledCollectionSchemeMap.size() ) + " active and " +
                   std::to_string( mIdleCollectionSchemeMap.size() ) + " idle collection schemes loaded" );
    // store the new DM, update mCurrentDecoderManifestID
    mCurrentDecoderManifestID = mDecoderManifest->getID();
    store( DataType::DECODER_MANIFEST );

    // Notify components about custom signal decoder format map change
    mCustomSignalDecoderFormatMapChangeListeners.notify(
        mCurrentDecoderManifestID, mDecoderManifest->getSignalIDToCustomSignalDecoderFormatMap() );
    return true;
}

/*
 * This function start from protobuf-ed collectionSchemeList
 * runs through the following steps:
 * build collectionSchemeList
 * rebuild or update existing collectionScheme maps when needed
 *
 * returns true when enabledCollectionSchemeMap has changed
 */
bool
CollectionSchemeManager::processCollectionScheme()
{
    if ( ( mCollectionSchemeList == nullptr ) || ( !mCollectionSchemeList->build() ) )
    {
        FWE_LOG_ERROR( "Incoming CollectionScheme does not exist or fails to build!" );
        TraceModule::get().incrementAtomicVariable( TraceAtomicVariable::COLLECTION_SCHEME_ERROR );
        return false;
    }
    // Build is successful. Store collectionScheme
    store( DataType::COLLECTION_SCHEME_LIST );
    if ( isCollectionSchemeLoaded() )
    {
        // there are existing collectionSchemes, try to update the existing one
        return updateMapsandTimeLine( mClock->timeSinceEpoch() );
    }
    else
    {
        // collectionScheme maps are empty
        return rebuildMapsandTimeLine( mClock->timeSinceEpoch() );
    }
}

#ifdef FWE_FEATURE_LAST_KNOWN_STATE
bool
CollectionSchemeManager::processStateTemplates()
{
    if ( ( mLastKnownStateIngestion == nullptr ) || ( !mLastKnownStateIngestion->build() ) )
    {
        FWE_LOG_ERROR( "Incoming StateTemplate does not exist or fails to build" );
        TraceModule::get().incrementAtomicVariable( TraceAtomicVariable::STATE_TEMPLATE_ERROR );
        return false;
    }

    auto stateTemplatesDiff = mLastKnownStateIngestion->getStateTemplatesDiff();
    if ( stateTemplatesDiff == nullptr )
    {
        return false;
    }

    if ( stateTemplatesDiff->version < mLastStateTemplatesDiffVersion )
    {
        FWE_LOG_TRACE( "Ignoring state templates diff with version " + std::to_string( stateTemplatesDiff->version ) +
                       " as it is older than the current version " + std::to_string( mLastStateTemplatesDiffVersion ) );
        return false;
    }

    mLastStateTemplatesDiffVersion = stateTemplatesDiff->version;
    bool modified = false;

    for ( const auto &stateTemplateId : stateTemplatesDiff->stateTemplatesToRemove )
    {
        if ( mStateTemplates.erase( stateTemplateId ) != 0U )
        {
            modified = true;
        }
    }

    for ( const auto &stateTemplate : stateTemplatesDiff->stateTemplatesToAdd )
    {
        if ( mStateTemplates.find( stateTemplate->id ) != mStateTemplates.end() )
        {
            continue;
        }
        modified = true;
        mStateTemplates.emplace( stateTemplate->id, stateTemplate );
    }

    if ( modified )
    {
        store( DataType::STATE_TEMPLATE_LIST );
    }

    return modified;
}
#endif

/*
 * This function rebuild enableCollectionScheme map, idle collectionScheme map, and timeline.
 * In case a collectionScheme needs to start immediately, this function builds mEnableCollectionSchemeMap and returns
 * true. Otherwise, it returns false.
 */
bool
CollectionSchemeManager::rebuildMapsandTimeLine( const TimePoint &currTime )
{
    bool ret = false;
    std::vector<std::shared_ptr<ICollectionScheme>> collectionSchemeList;

    if ( mCollectionSchemeList == nullptr )
    {
        return false;
    }

    collectionSchemeList = mCollectionSchemeList->getCollectionSchemes();
    /* Separate collectionSchemes into Enabled and Idle bucket */
    for ( auto const &collectionScheme : collectionSchemeList )
    {
        auto startTime = collectionScheme->getStartTime();
        auto stopTime = collectionScheme->getExpiryTime();
        const auto &id = collectionScheme->getCollectionSchemeID();
        if ( startTime > currTime.systemTimeMs )
        {
            /* for idleCollectionSchemes, push both startTime and stopTime to timeLine */
            mIdleCollectionSchemeMap[id] = collectionScheme;
            mTimeLine.push( { timePointFromSystemTime( currTime, startTime ), id } );
            mTimeLine.push( { timePointFromSystemTime( currTime, stopTime ), id } );
        }
        else if ( stopTime > currTime.systemTimeMs )
        { /* At rebuild, if a collectionScheme's startTime has already passed, enable collectionScheme immediately
           */
            mEnabledCollectionSchemeMap[id] = collectionScheme;
            mTimeLine.push( { timePointFromSystemTime( currTime, stopTime ), id } );
            ret = true;
        }
    }

    std::string enableStr;
    std::string idleStr;
    printExistingCollectionSchemes( enableStr, idleStr );
    FWE_LOG_TRACE( enableStr + idleStr );
    return ret;
}

std::vector<SyncID>
CollectionSchemeManager::getCollectionSchemeArns()
{
    std::lock_guard<std::mutex> lock( mSchemaUpdateMutex );
    std::vector<SyncID> collectionSchemeArns;
    if ( mCollectionSchemeList != nullptr )
    {
        for ( auto &collectionScheme : mCollectionSchemeList->getCollectionSchemes() )
        {
            collectionSchemeArns.push_back( collectionScheme->getCollectionSchemeID() );
        }
    }
    return collectionSchemeArns;
}

/*
 * This function goes through collectionSchemeList and updates mIdleCollectionSchemeMap, mEnabledCollectionSchemeMap
 * and mTimeLine;
 * For each collectionScheme,
 * If it is enabled, check new StopTime and update mEnabledCollectionSchemeMap, mTimeline and flag Changed when needed;
 * Else
 * Update mIdleCollectionSchemeMap and mTimeLine when needed;
 *
 * Returns true when mEnabledCollectionSchemeMap changes.
 */
bool
CollectionSchemeManager::updateMapsandTimeLine( const TimePoint &currTime )
{
    bool ret = false;
    std::unordered_set<SyncID> newCollectionSchemeIDs;
    std::vector<std::shared_ptr<ICollectionScheme>> collectionSchemeList;

    if ( mCollectionSchemeList == nullptr )
    {
        return false;
    }

    collectionSchemeList = mCollectionSchemeList->getCollectionSchemes();
    for ( auto const &collectionScheme : collectionSchemeList )
    {
        /*
         * Once collectionScheme has a matching DM, try to locate the collectionScheme in existing maps
         * using collectionScheme ID.
         * If neither found in Enabled nor Idle maps, it is a new collectionScheme, and add it
         * to either enabled map (the collectionScheme might be already overdue due to some other delay
         * in delivering to FWE), or idle map( this is the usual case).
         *
         * If found in enabled map, this is an update to existing collectionScheme, since it is already
         * enabled, just go ahead update expiry time or, disable the collectionScheme if it is due to stop
         * since it is already enabled, just go ahead update expiry time;
         * If found in idle map, just go ahead update the start or stop time in case of any change,
         *  and also check if it is to be enabled immediately.
         *
         */
        Timestamp startTime = collectionScheme->getStartTime();
        Timestamp stopTime = collectionScheme->getExpiryTime();

        const auto &id = collectionScheme->getCollectionSchemeID();
        newCollectionSchemeIDs.insert( id );
        auto itEnabled = mEnabledCollectionSchemeMap.find( id );
        auto itIdle = mIdleCollectionSchemeMap.find( id );
        if ( itEnabled != mEnabledCollectionSchemeMap.end() )
        {
            /* found collectionScheme in Enabled map. this collectionScheme is running, check for StopTime only */
            auto currCollectionScheme = itEnabled->second;
            if ( stopTime <= currTime.systemTimeMs )
            {
                /* This collectionScheme needs to stop immediately */
                mEnabledCollectionSchemeMap.erase( id );
                ret = true;
                std::string completedStr;
                completedStr = "Stopping enabled CollectionScheme: ";
                printEventLogMsg( completedStr, id, startTime, stopTime, currTime );
                FWE_LOG_TRACE( completedStr );
            }
            else
            {
                if ( stopTime != currCollectionScheme->getExpiryTime() )
                {
                    /* StopTime changes on that collectionScheme, update with new CollectionScheme */
                    mEnabledCollectionSchemeMap[id] = collectionScheme;
                    mTimeLine.push( { timePointFromSystemTime( currTime, stopTime ), id } );
                }

                if ( *collectionScheme != *currCollectionScheme )
                {
                    mEnabledCollectionSchemeMap[id] = collectionScheme;
                    ret = true;
                }
            }
        }
        else if ( itIdle != mIdleCollectionSchemeMap.end() )
        {
            /* found in Idle map, need to check both StartTime and StopTime */
            auto currCollectionScheme = itIdle->second;
            if ( ( startTime <= currTime.systemTimeMs ) && ( stopTime > currTime.systemTimeMs ) )
            {
                /* this collectionScheme needs to start immediately */
                mIdleCollectionSchemeMap.erase( id );
                mEnabledCollectionSchemeMap[id] = collectionScheme;
                ret = true;
                mTimeLine.push( { timePointFromSystemTime( currTime, stopTime ), id } );
                std::string startStr;
                startStr = "Starting idle collectionScheme now: ";
                printEventLogMsg( startStr, id, startTime, stopTime, currTime );
                FWE_LOG_TRACE( startStr );
            }
            else if ( ( startTime > currTime.systemTimeMs ) &&
                      ( ( startTime != currCollectionScheme->getStartTime() ) ||
                        ( stopTime != currCollectionScheme->getExpiryTime() ) ) )
            {
                // this collectionScheme is an idle collectionScheme, and its startTime or ExpiryTime
                // or both need updated
                mIdleCollectionSchemeMap[id] = collectionScheme;
                mTimeLine.push( { timePointFromSystemTime( currTime, startTime ), id } );
                mTimeLine.push( { timePointFromSystemTime( currTime, stopTime ), id } );
            }
            else
            {
                mIdleCollectionSchemeMap[id] = collectionScheme;
            }
        }
        else
        {
            /*
             * This is a new collectionScheme, might need to activate immediately if startTime has passed
             * Otherwise, add it to idleMap
             */
            std::string addStr;
            addStr = "Adding new collectionScheme: ";
            printEventLogMsg( addStr, id, startTime, stopTime, currTime );
            FWE_LOG_TRACE( addStr );
            if ( ( startTime <= currTime.systemTimeMs ) && ( stopTime > currTime.systemTimeMs ) )
            {
                mEnabledCollectionSchemeMap[id] = collectionScheme;
                mTimeLine.push( { timePointFromSystemTime( currTime, stopTime ), id } );
                ret = true;
            }
            else if ( startTime > currTime.systemTimeMs )
            {
                mIdleCollectionSchemeMap[id] = collectionScheme;
                mTimeLine.push( { timePointFromSystemTime( currTime, startTime ), id } );
                mTimeLine.push( { timePointFromSystemTime( currTime, stopTime ), id } );
            }
        }
    }
    /* Check in newCollectionSchemeIDs set, if any Idle collectionScheme is missing from the set*/
    std::string removeStr;
    auto it = mIdleCollectionSchemeMap.begin();
    while ( it != mIdleCollectionSchemeMap.end() )
    {
        if ( newCollectionSchemeIDs.find( it->first ) == newCollectionSchemeIDs.end() )
        {
            removeStr += it->first;
            it = mIdleCollectionSchemeMap.erase( it );
        }
        else
        {
            it++;
        }
    }
    /* Check in newCollectionSchemeIDs set, if any enabled collectionScheme is missing from the set*/
    it = mEnabledCollectionSchemeMap.begin();
    while ( it != mEnabledCollectionSchemeMap.end() )
    {
        if ( newCollectionSchemeIDs.find( it->first ) == newCollectionSchemeIDs.end() )
        {
            removeStr += it->first;
            it = mEnabledCollectionSchemeMap.erase( it );
            ret = true;
        }
        else
        {
            it++;
        }
    }
    if ( !removeStr.empty() )
    {
        FWE_LOG_TRACE( "Removing collectionSchemes missing from PI updates: " + removeStr );
    }

    std::string enableStr;
    std::string idleStr;
    printExistingCollectionSchemes( enableStr, idleStr );
    FWE_LOG_TRACE( enableStr + idleStr );
    return ret;
}

/*
 * This function checks timeline,
 * 1. Timer has not expired but main thread wakes up because of PI updates,
 * this function always checks if it is a timer expiration first.
 * If not, simply exit, return false;
 * 2. Otherwise,
 *      get topTime and topCollectionSchemeID from top of MINheap,
 *      if collectionScheme in Enabled Map, and stopTime equal to topTime, time to disable this collectionScheme, this
 * is a true case; else if CollectionScheme in idle map, and startTime equals to topTime, time to enable this
 * collectionScheme, this is a true case; for the rest of the cases, all false;
 *
 * 3. search for the next valid timePoint to set up timer;
 *  Because client may update existing collectionSchemes with new start and stop time, the timepoint
 *  in mTimeline needs to be scanned to make sure next timer is set up for a valid collectionScheme at
 *  a valid time.
 *  The will save us from waking up at an obsolete timePoint and waste context switch.
 *
 * returns true when enabled map changes;
 */
bool
CollectionSchemeManager::checkTimeLine( const TimePoint &currTime )
{
    bool ret = false;
    if ( ( mTimeLine.empty() ) || ( currTime.systemTimeMs < mTimeLine.top().time.systemTimeMs ) )
    {
        // Timer has not expired, do nothing
        return ret;
    }
    while ( !mTimeLine.empty() )
    {
        const auto &topPair = mTimeLine.top();
        const auto &topCollectionSchemeID = topPair.id;
        const auto &topTime = topPair.time;

        // first find topCollectionSchemeID in mEnabledCollectionSchemeMap then mIdleCollectionSchemeMap
        // if we find a match in collectionScheme ID, check further if topTime matches this collectionScheme's
        // start/stop time
        bool foundInEnabled = true;
        auto it = mEnabledCollectionSchemeMap.find( topCollectionSchemeID );
        if ( it == mEnabledCollectionSchemeMap.end() )
        {
            it = mIdleCollectionSchemeMap.find( topCollectionSchemeID );
            if ( it == mIdleCollectionSchemeMap.end() )
            {
                // Could not find it in Enabled map nor in Idle map,
                // this collectionScheme must have been disabled earlier per
                // client request, this dataPair is obsolete, just drop it
                // keep searching for next valid TimePoint
                // to set up timer
                FWE_LOG_TRACE( "CollectionScheme not found: " + topCollectionSchemeID );
                mTimeLine.pop();
                continue;
            }
            foundInEnabled = false;
        }
        // found it, continue examining topTime
        std::shared_ptr<ICollectionScheme> currCollectionScheme;
        Timestamp timeOfInterest = 0ULL;
        if ( foundInEnabled )
        {
            // This collectionScheme is found in mEnabledCollectionSchemeMap
            // topCollectionSchemeID has been enabled, check if stop time matches
            currCollectionScheme = mEnabledCollectionSchemeMap[topCollectionSchemeID];
            timeOfInterest = currCollectionScheme->getExpiryTime();
        }
        else
        {
            // This collectionScheme is found in mIdleCollectionSchemeMap
            // topCollectionSchemeID has not been enabled, check if start time matches
            currCollectionScheme = mIdleCollectionSchemeMap[topCollectionSchemeID];
            timeOfInterest = currCollectionScheme->getStartTime();
        }
        if ( timeOfInterest != topTime.systemTimeMs )
        {
            // this dataPair has a valid collectionScheme ID, but the start time or stop time is already updated
            // not equal to topTime any more; This is an obsolete dataPair. Simply drop it and move on
            // to next pair
            FWE_LOG_TRACE( "Found collectionScheme: " + topCollectionSchemeID +
                           " but time does not match: "
                           "topTime " +
                           std::to_string( topTime.systemTimeMs ) + " timeFromCollectionScheme " +
                           std::to_string( timeOfInterest ) );
            mTimeLine.pop();
            continue;
        }
        // now we have a dataPair with valid collectionScheme ID, and valid start/stop time
        // Check if it is time to enable/disable this collectionScheme, or else
        // topTime is far down the timeline, it is a timePoint to set up next timer.
        if ( topTime.systemTimeMs <= currTime.systemTimeMs )
        {
            ret = true;
            // it is time to enable or disable this collectionScheme
            if ( !foundInEnabled )
            {
                // enable the collectionScheme
                mEnabledCollectionSchemeMap[topCollectionSchemeID] = currCollectionScheme;
                mIdleCollectionSchemeMap.erase( topCollectionSchemeID );
                std::string enableStr;
                enableStr = "Enabling idle collectionScheme: ";
                printEventLogMsg( enableStr,
                                  topCollectionSchemeID,
                                  currCollectionScheme->getStartTime(),
                                  currCollectionScheme->getExpiryTime(),
                                  topTime );
                FWE_LOG_INFO( enableStr );
            }
            else
            {
                // disable the collectionScheme
                std::string disableStr;
                disableStr = "Disabling enabled collectionScheme: ";
                printEventLogMsg( disableStr,
                                  topCollectionSchemeID,
                                  currCollectionScheme->getStartTime(),
                                  currCollectionScheme->getExpiryTime(),
                                  topTime );
                FWE_LOG_INFO( disableStr );
                mEnabledCollectionSchemeMap.erase( topCollectionSchemeID );
            }
        }
        else
        {
            // Successfully locate the next valid TimePoint
            // stop searching
            break;
        }
        // continue searching for next valid timepoint to set up timer
        mTimeLine.pop();
    }
    if ( !mTimeLine.empty() )
    {
        FWE_LOG_TRACE( "Top pair: " + std::to_string( mTimeLine.top().time.systemTimeMs ) + " " + mTimeLine.top().id +
                       " currTime: " + std::to_string( currTime.systemTimeMs ) );
    }
    return ret;
}

bool
CollectionSchemeManager::isCollectionSchemesInSyncWithDm()
{
    for ( const auto &collectionScheme : mEnabledCollectionSchemeMap )
    {
        if ( collectionScheme.second->getDecoderManifestID() != mCurrentDecoderManifestID )
        {
            FWE_LOG_INFO( "Decoder manifest out of sync: " + collectionScheme.second->getDecoderManifestID() + " vs. " +
                          mCurrentDecoderManifestID );
            return false;
        }
    }
    for ( const auto &collectionScheme : mIdleCollectionSchemeMap )
    {
        if ( collectionScheme.second->getDecoderManifestID() != mCurrentDecoderManifestID )
        {
            FWE_LOG_INFO( "Decoder manifest out of sync: " + collectionScheme.second->getDecoderManifestID() + " vs. " +
                          mCurrentDecoderManifestID );
            return false;
        }
    }
    return true;
}

void
CollectionSchemeManager::updateActiveCollectionSchemeListeners()
{
    // Create vector of active collection schemes to notify interested components about new schemes
    auto activeCollectionSchemesOutput = std::make_unique<ActiveCollectionSchemes>();

    if ( isCollectionSchemesInSyncWithDm() )
    {
        for ( const auto &enabledCollectionScheme : mEnabledCollectionSchemeMap )
        {
            activeCollectionSchemesOutput->activeCollectionSchemes.push_back( enabledCollectionScheme.second );
        }
    }

    mCollectionSchemeListChangeListeners.notify(
        // coverity[autosar_cpp14_a20_8_6_violation] can't use make_shared as unique_ptr is moved
        std::shared_ptr<const ActiveCollectionSchemes>( std::move( activeCollectionSchemesOutput ) ) );
}

} // namespace IoTFleetWise
} // namespace Aws

// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "aws/iotfleetwise/CollectionInspectionAPITypes.h"
#include "aws/iotfleetwise/SignalTypes.h"

namespace Aws
{
namespace IoTFleetWise
{
/**
 * @brief Translate the internal used Id to the Id used in config file and decoder manifest
 * Adding new items is not thread safe
 */
class CANInterfaceIDTranslator
{

public:
    void
    add( InterfaceID iid )
    {
        mLookup.emplace_back( mCounter, iid );
        mCounter++;
    }

    CANChannelNumericID
    getChannelNumericID( const InterfaceID &iid ) const
    {
        for ( const auto &l : mLookup )
        {
            if ( l.second == iid )
            {
                return l.first;
            }
        }
        return INVALID_CAN_SOURCE_NUMERIC_ID;
    };

    InterfaceID
    // coverity[autosar_cpp14_m3_2_2_violation:FALSE] not defined anywhere else
    // coverity[misra_cpp_2008_rule_3_2_2_violation:FALSE] not defined anywhere else
    // coverity[cert_dcl60_cpp_violation:FALSE] not defined anywhere else
    // coverity[ODR_VIOLATION:FALSE] not defined anywhere else
    getInterfaceID( CANChannelNumericID cid ) const
    {
        for ( const auto &l : mLookup )
        {
            if ( l.first == cid )
            {
                return l.second;
            }
        }
        return INVALID_INTERFACE_ID;
    };

private:
    std::vector<std::pair<CANChannelNumericID, InterfaceID>> mLookup;
    CANChannelNumericID mCounter{ 0 };
};

} // namespace IoTFleetWise
} // namespace Aws

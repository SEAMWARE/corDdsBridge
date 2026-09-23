//
// FILE            ddsRegister.cpp
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The one symbol this shared object exports.
//
// extern "C" is not optional: the broker looks the symbol up with dlsym by the
// name "bridgeRegister", and a C++ compiler would otherwise decorate it with
// its argument types.
//

#include "ddsBridge.hpp"                               // coraine::dds
#include "version.h"                                   // CORDDSBRIDGE_VERSION



extern "C" void bridgeRegister(BridgeDriver* driverP)
{
    //
    // ⭐ WHAT THE HOST SPEAKS, READ BEFORE ANYTHING IS WRITTEN.
    //
    // The host owns this struct and allocated it at ITS size. Zero means a host
    // from before the handshake existed, and the only safe reading of that is
    // 1 - the revision that was all there was at the time. See BridgeDriver.h.
    //
    const int hostAbi = (driverP->abiVersion > 0) ? driverP->abiVersion : 1;

    driverP->alias       = "dds";
    driverP->version     = CORDDSBRIDGE_VERSION;
    driverP->args        = nullptr;

    driverP->init        = coraine::dds::init;
    driverP->close       = coraine::dds::close;
    driverP->channelAdd  = coraine::dds::channelAdd;
    driverP->channelDel  = coraine::dds::channelDel;
    driverP->publish     = coraine::dds::publish;
    driverP->versionInfo = coraine::dds::versionInfo;

    //
    // ABI 2 slots, and ONLY if the host has them. On an older host these two
    // assignments would land past the end of its struct; skipping them leaves
    // it with a topic-only DDS bridge, which is what an ABI 1 host asked for.
    //
    if (hostAbi >= 2)
    {
        driverP->serviceInvoke = coraine::dds::serviceInvoke;
        driverP->serverIface   = coraine::dds::serverIface;
    }

    //
    // And now the field is the PLUGIN's, which is what the host reads back: it
    // logs a mismatch against its own and reports it in GET /version.
    //
    driverP->abiVersion = BRIDGE_ABI_VERSION;
}

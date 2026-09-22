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
    driverP->alias       = "dds";
    driverP->version     = CORDDSBRIDGE_VERSION;
    driverP->abiVersion  = BRIDGE_ABI_VERSION;
    driverP->args        = nullptr;

    driverP->init        = coraine::dds::init;
    driverP->close       = coraine::dds::close;
    driverP->channelAdd  = coraine::dds::channelAdd;
    driverP->channelDel  = coraine::dds::channelDel;
    driverP->publish     = coraine::dds::publish;
    driverP->versionInfo = coraine::dds::versionInfo;
}

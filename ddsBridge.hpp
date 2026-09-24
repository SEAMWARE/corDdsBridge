#ifndef CORDDSBRIDGE_DDSBRIDGE_HPP_
#define CORDDSBRIDGE_DDSBRIDGE_HPP_

//
// FILE            ddsBridge.hpp
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The one place C++ and C meet in this plugin.
//
// Everything the broker sees is C: corBridge's two headers, and the single
// exported symbol bridgeRegister. Everything the DDS Enabler sees is C++,
// because its API takes std::string and std::shared_ptr and cannot be reached
// from C at all - no amount of declaring mangled names helps, since a mangled
// name gets you the symbol and never the argument ABI.
//
// So the plugin is a C++ translation unit that includes the C contract inside
// extern "C". That is the whole trick, and it costs nothing: a plugin is
// already a separate shared object, so the broker stays PROJECT(coraine C) and
// never sees a C++ token.
//

extern "C"
{
#include "corBridge/BridgeDriver.h"                    // BridgeDriver, BridgeChannelKind, BridgeDirection
#include "corBridge/BridgeBroker.h"                    // BridgeBroker, BRIDGE_*
#include "corBridge/BridgeServer.h"                    // BridgeServer
}

#include <cstdint>                                     // int64_t
#include <string>                                      // std::string



namespace coraine {
namespace dds {

//
// The broker's side of the seam, kept for the life of the plugin. Set by
// init(), read by the Enabler's callback threads.
//
extern const BridgeBroker*  broker;


//
// bridgeAlias - what this plugin answers to, and what sampleIn is told
//
extern const char*          bridgeAlias;


int   init(const char* configFile, const BridgeBroker* brokerP);
void  close();
int   channelAdd(const char* endpoint, BridgeChannelKind kind, BridgeDirection direction);
int   channelDel(const char* endpoint);
int   publish(const char* endpoint, const char* json);
int   serviceInvoke(const char* endpoint, const char* json);
int   serviceInvokeTracked(const char* endpoint, const char* json, uint64_t token);
int   actionGoalSend(const char* endpoint, const char* json, uint64_t token);
int   actionGoalCancel(const char* endpoint, uint64_t token);
const BridgeServer* serverIface();
const char* versionInfo();

}  // namespace dds
}  // namespace coraine

#endif  // CORDDSBRIDGE_DDSBRIDGE_HPP_

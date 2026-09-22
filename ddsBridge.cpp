//
// FILE            ddsBridge.cpp
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <cstdio>                                      // snprintf
#include <cstring>                                     // strcmp
#include <map>                                         // std::map
#include <mutex>                                       // std::mutex
#include <string>                                      // std::string

#include <ddsenabler/dds_enabler_runner.hpp>           // create_dds_enabler
#include <ddsenabler/DDSEnabler.hpp>                   // DDSEnabler
#include <ddsenabler/CallbackSet.hpp>                  // CallbackSet

extern "C"
{
#include "ktrace/kTrace.h"                             // KT_I, KT_W, KT_E, KT_T
}

#include "ddsBridge.hpp"                               // Own interface
#include "version.h"                                   // CORDDSBRIDGE_VERSION



namespace coraine {
namespace dds {

const BridgeBroker*  broker      = nullptr;
const char*          bridgeAlias = "dds";

namespace {

std::shared_ptr<eprosima::ddsenabler::DDSEnabler>  enabler;

//
// Which endpoints the broker asked for.
//
// The Enabler hands over everything its allowlist admits, which in an ordinary
// configuration is everything on the domain. A robot fleet says a great deal
// that a given broker was never configured to want, and answering
// BRIDGE_NOT_FOUND for each of those means a Channel lookup, a trace line and a
// lock, per sample, for data nobody asked for. So the set is kept here and the
// sample is dropped before it crosses the seam.
//
std::map<std::string, BridgeDirection>  carried;
std::mutex                              carriedMutex;

char  versionBuffer[256];


// -----------------------------------------------------------------------------
//
// dataNotification - a sample arrived
//
// ⚠ AN ENABLER THREAD, not one of the broker's. Everything that makes the call
// safe is on the broker's side of sampleIn; nothing may be prepared here.
//
void dataNotification(const char* topicName, const char* json, int64_t publishTime)
{
    if ((topicName == nullptr) || (json == nullptr) || (broker == nullptr))
        return;

    {
        std::lock_guard<std::mutex> guard(carriedMutex);
        auto it = carried.find(topicName);

        if (it == carried.end())
            return;                                    // nobody asked for this topic

        if (it->second == BridgeDirectionOut)
            return;                                    // outbound only
    }

    broker->sampleIn(bridgeAlias, topicName, json, publishTime);
}



// -----------------------------------------------------------------------------
//
// logConsumer - the Enabler's own log messages, forwarded to the broker's log
//
// This is what BridgeBroker::logFunction exists for: a library with its own
// logging wants a sink taking file, line, function and a severity, and the
// broker has one. The plugin's OWN messages use KT_* directly, like any other
// plugin.
//
void logConsumer(const char* fileName, int lineNo, const char* funcName, int category, const char* msg)
{
    if (broker == nullptr)
        return;

    //
    // The Enabler's categories run from informational to error in its own
    // order; anything at or above its error level is an error here.
    //
    int severity = (category >= 2) ? BRIDGE_LOG_ERROR : (category == 1) ? BRIDGE_LOG_WARNING : BRIDGE_LOG_INFO;

    broker->logFunction(severity, fileName, lineNo, funcName, msg);
}

}  // anonymous namespace



// -----------------------------------------------------------------------------
//
// init -
//
int init(const char* configFile, const BridgeBroker* brokerP)
{
    if (brokerP == nullptr)
        return BRIDGE_ERR;

    broker = brokerP;

    if (configFile == nullptr)
    {
        KT_E("the dds bridge needs a configuration file - none was given (--bridgeConfig)");
        return BRIDGE_ERR;
    }

    eprosima::ddsenabler::CallbackSet callbacks{};

    callbacks.log                   = logConsumer;
    callbacks.dds.data_notification = dataNotification;

    //
    // The remaining callbacks stay null on purpose. Type and topic discovery,
    // services and actions are not carried yet, and a null entry is how the
    // Enabler is told so - the same convention the BridgeDriver uses.
    //
    if (eprosima::ddsenabler::create_dds_enabler(configFile, callbacks, enabler) == false)
    {
        KT_E("unable to create the DDS Enabler from '%s'", configFile);
        return BRIDGE_ERR;
    }

    KT_I("dds bridge up, configured from '%s'", configFile);

    return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// close -
//
// Must not return while a thread could still be inside dataNotification: the
// broker tears down what sampleIn reaches immediately afterwards. Releasing the
// Enabler is what stops its threads, and clearing the carried set first means a
// sample that slips through in between is dropped rather than delivered.
//
void close()
{
    {
        std::lock_guard<std::mutex> guard(carriedMutex);
        carried.clear();
    }

    enabler.reset();
    broker = nullptr;
}



// -----------------------------------------------------------------------------
//
// channelAdd -
//
int channelAdd(const char* endpoint, BridgeChannelKind kind, BridgeDirection direction)
{
    if ((endpoint == nullptr) || (*endpoint == 0))
        return BRIDGE_BAD_INPUT;

    if (kind != BridgeChannelTopic)
        return BRIDGE_UNSUPPORTED;                     // services and actions are not carried yet

    std::lock_guard<std::mutex> guard(carriedMutex);
    carried[endpoint] = direction;

    KT_T(0, "dds: carrying topic '%s'", endpoint);

    return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// channelDel -
//
int channelDel(const char* endpoint)
{
    if (endpoint == nullptr)
        return BRIDGE_BAD_INPUT;

    std::lock_guard<std::mutex> guard(carriedMutex);

    return (carried.erase(endpoint) > 0) ? BRIDGE_OK : BRIDGE_NOT_FOUND;
}



// -----------------------------------------------------------------------------
//
// publish -
//
// Called on a BROKER thread, inside the request that changed the attribute, so
// it hands the payload over and returns.
//
int publish(const char* endpoint, const char* json)
{
    if ((endpoint == nullptr) || (json == nullptr))
        return BRIDGE_BAD_INPUT;

    if (enabler == nullptr)
        return BRIDGE_ERR;

    //
    // std::string, right here, is the reason this file is C++.
    //
    return (enabler->publish(endpoint, json) == true) ? BRIDGE_OK : BRIDGE_ERR;
}



// -----------------------------------------------------------------------------
//
// versionInfo -
//
const char* versionInfo()
{
    snprintf(versionBuffer, sizeof(versionBuffer), "dds %s (DDS Enabler, Fast DDS)", CORDDSBRIDGE_VERSION);

    return versionBuffer;
}

}  // namespace dds
}  // namespace coraine

//
// FILE            ddsBridge.cpp
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <cstdio>                                      // snprintf, fopen, fread, fwrite
#include <cstring>                                     // strcmp, memcpy
#include <map>                                         // std::map
#include <mutex>                                       // std::mutex
#include <string>                                      // std::string
#include <vector>                                      // std::vector

#include <ddsenabler/dds_enabler_runner.hpp>           // create_dds_enabler
#include <ddsenabler/DDSEnabler.hpp>                   // DDSEnabler
#include <ddsenabler/CallbackSet.hpp>                  // CallbackSet

extern "C"
{
#include "ktrace/kTrace.h"                             // KT_I, KT_W, KT_E, KT_T
#include "kjson/kjson.h"                               // Kjson
#include "kjson/kjBufferCreate.h"                      // kjBufferCreate
#include "kjson/kjParse.h"                             // kjParse
#include "kjson/kjLookup.h"                            // kjLookup
#include "kjson/kjRender.h"                            // kjFastRender
#include "kjson/kjRenderSize.h"                        // kjFastRenderSize
#include "kalloc/KAlloc.h"                             // KAlloc
#include "kalloc/kaBufferInit.h"                       // kaBufferInit
#include "kalloc/kaBufferReset.h"                      // kaBufferReset
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


//
// What the DDS system has told us about itself.
//
// ⭐ THE BROKER CAN ONLY WORK ON WHAT IS ALREADY KNOWN IN THE DDS SYSTEM. It is
// a participant in that system, not the author of it: types and topics are
// defined by the applications on the domain, discovered as they announce
// themselves, and the broker's job is to remember what it was told and hand it
// back when the Enabler asks.
//
// So there is no way to declare a type here and none is wanted. A topic nobody
// has announced is a topic the broker cannot publish to, and saying so is the
// correct answer.
//
std::map<std::string, std::vector<unsigned char>>  typeStore;
std::map<std::string, eprosima::ddsenabler::participants::TopicInfo>  topicStore;
std::mutex                                         discoveryMutex;

//
// Where a discovered type is kept between runs. From dds.ngsild.typesDirectory;
// empty means types live only as long as the process, which is correct but
// means a restart is blind until every publisher has announced itself again.
//
std::string  typesDirectory;


// -----------------------------------------------------------------------------
//
// typePath - where a type's bytes live on disk
//
// A type name is an IDL identifier with '::' separators; '/' never occurs in
// one, so the name is a filename as it stands.
//
std::string typePath(const char* typeName)
{
    return typesDirectory + "/" + typeName + ".bin";
}



// -----------------------------------------------------------------------------
//
// typeNotification - the DDS system announced a type
//
// Kept in memory, and written to typesDirectory if there is one. The bytes are
// the Enabler's INTERNAL representation, not the IDL text: it is what
// type_query has to hand back, so it is what gets stored.
//
void typeNotification(const char*          typeName,
                      const char*          serializedType,
                      const unsigned char* serializedTypeInternal,
                      uint32_t             serializedTypeInternalSize,
                      const char*          dataPlaceholder)
{
    (void) serializedType;                             // the IDL text, of interest to a human and not to us
    (void) dataPlaceholder;

    if ((typeName == nullptr) || (serializedTypeInternal == nullptr) || (serializedTypeInternalSize == 0))
        return;

    {
        std::lock_guard<std::mutex> guard(discoveryMutex);
        typeStore[typeName] = std::vector<unsigned char>(serializedTypeInternal,
                                                         serializedTypeInternal + serializedTypeInternalSize);
    }

    KT_T(0, "dds: learned type '%s' (%u bytes)", typeName, serializedTypeInternalSize);

    if (typesDirectory.empty() == true)
        return;

    //
    // Written so a restart is not blind until every publisher happens to
    // announce itself again.
    //
    FILE* fP = fopen(typePath(typeName).c_str(), "wb");

    if (fP == nullptr)
    {
        KT_W("dds: cannot write type '%s' to '%s'", typeName, typesDirectory.c_str());
        return;
    }

    fwrite(serializedTypeInternal, 1, serializedTypeInternalSize, fP);
    fclose(fP);
}



// -----------------------------------------------------------------------------
//
// typeQuery - the Enabler needs a type's bytes
//
// From memory, else from typesDirectory. Ownership of the buffer passes to the
// Enabler through the unique_ptr, which is one of the several things in this
// API that cannot be expressed in C.
//
bool typeQuery(const char*                              typeName,
               std::unique_ptr<const unsigned char[]>&  serializedTypeInternal,
               uint32_t&                                serializedTypeInternalSize)
{
    if (typeName == nullptr)
        return false;

    {
        std::lock_guard<std::mutex> guard(discoveryMutex);
        auto it = typeStore.find(typeName);

        if (it != typeStore.end())
        {
            unsigned char* copy = new unsigned char[it->second.size()];

            memcpy(copy, it->second.data(), it->second.size());
            serializedTypeInternal.reset(copy);
            serializedTypeInternalSize = (uint32_t) it->second.size();

            return true;
        }
    }

    if (typesDirectory.empty() == true)
        return false;

    FILE* fP = fopen(typePath(typeName).c_str(), "rb");

    if (fP == nullptr)
        return false;

    fseek(fP, 0, SEEK_END);
    long size = ftell(fP);
    fseek(fP, 0, SEEK_SET);

    if (size <= 0)
    {
        fclose(fP);
        return false;
    }

    unsigned char* data = new unsigned char[size];

    if (fread(data, 1, (size_t) size, fP) != (size_t) size)
    {
        fclose(fP);
        delete[] data;
        return false;
    }
    fclose(fP);

    {
        std::lock_guard<std::mutex> guard(discoveryMutex);
        typeStore[typeName] = std::vector<unsigned char>(data, data + size);
    }

    serializedTypeInternal.reset(data);
    serializedTypeInternalSize = (uint32_t) size;

    KT_T(0, "dds: loaded type '%s' from disk (%ld bytes)", typeName, size);

    return true;
}



// -----------------------------------------------------------------------------
//
// topicNotification - the DDS system announced a topic
//
void topicNotification(const char* topicName, const eprosima::ddsenabler::participants::TopicInfo& topicInfo)
{
    if (topicName == nullptr)
        return;

    {
        std::lock_guard<std::mutex> guard(discoveryMutex);
        topicStore[topicName] = topicInfo;
    }

    KT_T(0, "dds: learned topic '%s' of type '%s'", topicName, topicInfo.type_name.c_str());
}



// -----------------------------------------------------------------------------
//
// topicQuery - the Enabler needs a topic's type and QoS
//
// ⭐ FALSE when the topic is unknown, and that is the whole point of
// implementing this. Answering true with an unfilled TopicInfo hands the
// Enabler an empty type name, and the failure then happens further in and looks
// like something else. A topic nobody has announced is a topic this broker
// cannot serialize for, and the honest answer is the useful one.
//
bool topicQuery(const char* topicName, eprosima::ddsenabler::participants::TopicInfo& topicInfo)
{
    if (topicName == nullptr)
        return false;

    std::lock_guard<std::mutex> guard(discoveryMutex);
    auto it = topicStore.find(topicName);

    if (it == topicStore.end())
    {
        KT_T(0, "dds: topic '%s' has not been announced on this domain", topicName);
        return false;
    }

    topicInfo = it->second;

    return true;
}



// -----------------------------------------------------------------------------
//
// sampleUnwrap - the message inside the Enabler's envelope
//
// The Enabler does not hand over the sample. It hands over an envelope with the
// sample inside it:
//
//   { "id": "01.0f.70.b7.01.00.d0.1d.00.00.00.00",
//     "rt/chatter": { "type": "std_msgs::msg::dds_::String_",
//                     "data": { "0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0":
//                               { "data": "Hello World: 9" } } },
//     "type": "fastdds" }
//
// and the broker's side of the seam is "the sample IS the value". Hand it that
// and the attribute's value becomes a writer's GUID and a map keyed by an
// instance handle, with the message three levels down - which is exactly what
// the first REAL publisher, a ROS 2 talker, put in the entity.
//
// So the envelope comes off here, where the knowledge of it belongs: the topic
// key, "data" under it, and the one instance under that. The broker keeps
// knowing nothing about DDS, and a sample in is the mirror of a sample out.
//
// Returns false when the json is not an envelope of that shape - an Enabler
// that changes it, or another producer - and the caller then passes the json on
// untouched rather than dropping a sample it could have delivered.
//
static bool sampleUnwrap(const char* topicName, const char* json, std::string& payload)
{
    //
    // kjParse works IN the buffer it is given, so the Enabler's string is
    // copied first - it belongs to the Enabler and is const.
    //
    std::string  copy(json);
    char         kallocBuffer[8192];
    KAlloc       kalloc;
    Kjson        kjson;
    bool         unwrapped = false;

    kaBufferInit(&kalloc, kallocBuffer, sizeof(kallocBuffer), 8 * 1024, nullptr, "ddsSample");

    Kjson*  kjP    = kjBufferCreate(&kjson, &kalloc);
    KjNode* treeP  = kjParse(kjP, (char*) copy.c_str());
    KjNode* topicP = ((treeP  != nullptr) && (treeP->type == KjObject))  ? kjLookup(treeP, topicName) : nullptr;
    KjNode* dataP  = ((topicP != nullptr) && (topicP->type == KjObject)) ? kjLookup(topicP, "data")   : nullptr;
    KjNode* sampleP = ((dataP != nullptr) && (dataP->type == KjObject))  ? dataP->value.firstChildP   : nullptr;

    if (sampleP != nullptr)
    {
        //
        // The instance handle is the member's NAME, and a rendered member is
        // "name":value - so the name goes before the render, not after it.
        //
        sampleP->name = (char*) "";

        payload.resize(kjFastRenderSize(sampleP));
        kjFastRender(sampleP, (char*) payload.data());
        payload.resize(strlen(payload.c_str()));
        unwrapped = true;
    }

    kaBufferReset(&kalloc, KTRUE);

    return unwrapped;
}



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

    //
    // Only what the publisher wrote crosses the seam, not the Enabler's
    // envelope around it.
    //
    std::string payload;

    if (sampleUnwrap(topicName, json, payload) == true)
        broker->sampleIn(bridgeAlias, topicName, payload.c_str(), publishTime);
    else
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

    //
    // dds.ngsild.typesDirectory, if the file names one. Read with the host's
    // own kjson - a plugin resolves the broker's symbols at dlopen, so there is
    // no JSON library to link here and no second parser to keep in step.
    //
    {
        //
        // fopen and not kFileRead: given an empty base, kFileRead does not
        // resolve a plain relative path, and the failure here is SILENT - the
        // typesDirectory would simply never be picked up for anyone who passed
        // --bridgeConfig a relative path.
        //
        FILE* fP = fopen(configFile, "r");

        if (fP != nullptr)
        {
            fseek(fP, 0, SEEK_END);
            long size = ftell(fP);
            fseek(fP, 0, SEEK_SET);

            std::vector<char> buf((size > 0) ? size + 1 : 1, 0);

            if ((size > 0) && (fread(buf.data(), 1, (size_t) size, fP) == (size_t) size))
            {
            char    kallocBuffer[8192];
            KAlloc  kalloc;
            Kjson   kjson;

            kaBufferInit(&kalloc, kallocBuffer, sizeof(kallocBuffer), 8 * 1024, nullptr, "ddsTypes");

            Kjson*  kjP   = kjBufferCreate(&kjson, &kalloc);
            KjNode* treeP = kjParse(kjP, buf.data());
            KjNode* ddsP  = (treeP  != nullptr) ? kjLookup(treeP, "dds")     : nullptr;
            KjNode* ngP   = (ddsP   != nullptr) ? kjLookup(ddsP, "ngsild")   : nullptr;
            KjNode* dirP  = (ngP    != nullptr) ? kjLookup(ngP, "typesDirectory") : nullptr;

            if ((dirP != nullptr) && (dirP->type == KjString) && (dirP->value.s != nullptr))
            {
                typesDirectory = dirP->value.s;
                KT_I("dds: types are kept in '%s'", typesDirectory.c_str());
            }

            kaBufferReset(&kalloc, KTRUE);
            }
            fclose(fP);
        }
    }

    eprosima::ddsenabler::CallbackSet callbacks{};

    callbacks.log                    = logConsumer;
    callbacks.dds.data_notification  = dataNotification;
    callbacks.dds.type_notification  = typeNotification;
    callbacks.dds.topic_notification = topicNotification;
    callbacks.dds.type_query         = typeQuery;
    callbacks.dds.topic_query        = topicQuery;

    //
    // The service and action callbacks stay null on purpose - not carried yet,
    // and a null entry is how the Enabler is told so, the same convention the
    // BridgeDriver uses.
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

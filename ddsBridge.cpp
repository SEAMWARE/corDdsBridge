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
#include <set>                                         // std::set
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
//
// What a Channel told us about an endpoint. The direction decides whether an
// arriving sample is wanted; the kind decides which of the transport's
// mechanisms the endpoint is - a topic is published to, a service is asked.
//
struct Carried
{
    BridgeChannelKind  kind;
    BridgeDirection    direction;
};

std::map<std::string, Carried>  carried;
std::mutex                      carriedMutex;

//
// unwanted - the topics the BROKER has already refused
//
// A topic nobody configured used to be dropped here, which was right while the
// broker's only answer to an unclaimed endpoint was BRIDGE_NOT_FOUND. It can
// have another one now - a catch-all entity, where every endpoint on the
// system shows up as an attribute - and that is a decision this plugin has no
// business making: it does not read the ngsild section of the file and must
// not start.
//
// So an unknown topic is handed over ONCE. If the broker says NOT_FOUND it
// goes in here and is never offered again, and a domain with a thousand
// unmapped topics costs a thousand calls in total rather than per second.
//
// ⚠ Emptied for an endpoint the moment a Channel claims it - see channelAdd.
// A stale refusal would outlive the configuration that caused it.
//
std::set<std::string>                   unwanted;

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



//
// Defined below, with the rest of the sample handling - a reply arrives inside
// the same envelope a sample does, so it comes off in the same place.
//
static bool sampleUnwrap(const char* topicName, const char* json, std::string& payload);



// -----------------------------------------------------------------------------
//
// DDS_SERVICE_REPLY - where a reply lands, and a name that is PROVISIONAL
//
// ⚠ The next release of NGSI-LD is expected to introduce a first-class Service
// Execution concept, and it will decide how an invocation and its answer appear
// in the model. Until it does, something has to be chosen, because a client
// that invokes a service has to read the answer somewhere - and what is chosen
// here is the shape already in field use, so that deployments and tooling that
// read it keep working.
//
// ⭐ IT IS SPELLED HERE AND NOWHERE ELSE. The broker is handed a sub-attribute
// name and stores it; it does not know this string, has no branch on it and
// never will. When Service Execution lands, what changes is this line and the
// translation around it - not the broker, and not the model.
//
#define DDS_SERVICE_REPLY  "ddsServiceReply"



// -----------------------------------------------------------------------------
//
// ServiceTypes - the request and reply types of one service
//
// ⭐ A SERVICE CANNOT BE LEARNED BY LISTENING, and that is the operational
// difference from a topic. A topic's type arrives with the first publisher;
// there is nobody to learn a service's types from before the first client
// speaks, and by then it is too late - the request has to be serialized to be
// sent at all. So they are configured, and the types themselves come off disk
// (see typeQuery).
//
// The file names them per service; the convention when it does not is the one
// the tooling in this world already uses, name + _Request / _Response.
//
struct ServiceTypes
{
    std::string  request;
    std::string  reply;
};

std::map<std::string, ServiceTypes>  serviceConfig;    // from the configuration file
std::mutex                           serviceMutex;

//
// Which services this process ANSWERS, and who answers them.
//
// ⛔ Empty in the broker, always. The broker is a client and has nothing to
// compute an answer with; this exists for a host that is not a broker - see
// BridgeServer.h.
//
std::map<std::string, BridgeServiceRequestFunc>  served;


// -----------------------------------------------------------------------------
//
// SERVICE_QOS - what is announced about a service's two topics
//
// The Enabler asks for QoS as text in its own serialized form. Reliable and
// volatile, which is what a request/reply exchange wants: an answer that
// arrives late is still an answer, an answer that is durable is an answer to a
// question nobody is waiting for any more.
//
#define SERVICE_QOS  "reliability: true\ndurability: false\nownership: false\nkeyed: false"



// -----------------------------------------------------------------------------
//
// serviceQuery - the Enabler needs a service's request and reply types
//
// ⭐ FALSE when the service was never configured, for the same reason
// topicQuery answers false for an unknown topic: an empty ServiceInfo would be
// accepted here and fail somewhere further in, looking like something else.
//
bool serviceQuery(const char* serviceName, eprosima::ddsenabler::participants::ServiceInfo& serviceInfo)
{
    if (serviceName == nullptr)
        return false;

    std::lock_guard<std::mutex> guard(serviceMutex);
    auto it = serviceConfig.find(serviceName);

    if (it == serviceConfig.end())
    {
        KT_T(0, "dds: service '%s' is not in the configuration - its types are unknown", serviceName);
        return false;
    }

    serviceInfo.request = eprosima::ddsenabler::participants::TopicInfo(it->second.request, SERVICE_QOS);
    serviceInfo.reply   = eprosima::ddsenabler::participants::TopicInfo(it->second.reply,   SERVICE_QOS);

    return true;
}



// -----------------------------------------------------------------------------
//
// serviceNotification - a service was discovered on the domain
//
// Nothing is kept: what this plugin would need from it - the type names - it
// already has from its own configuration, and it must, because a service that
// nobody has announced yet still has to be invocable. So this is a trace and
// no more, and it is worth having as one: "the peer is there" is the first
// thing anybody asks when a request goes unanswered.
//
void serviceNotification(const char* serviceName, const eprosima::ddsenabler::participants::ServiceInfo& serviceInfo)
{
    if (serviceName == nullptr)
        return;

    KT_T(0, "dds: service '%s' discovered (request '%s', reply '%s')",
         serviceName, serviceInfo.request.type_name.c_str(), serviceInfo.reply.type_name.c_str());
}



// -----------------------------------------------------------------------------
//
// serviceReplyNotification - an answer came back
//
// ⚠ AN ENABLER THREAD, as dataNotification.
//
// The requestId is not passed on. It correlates a reply to a request, which is
// this plugin's business and finishes here: the broker identifies the exchange
// by its ENDPOINT, because an endpoint is what a Channel binds to an attribute.
//
void serviceReplyNotification(const char* serviceName, const char* json, uint64_t requestId, int64_t publishTime)
{
    if ((serviceName == nullptr) || (json == nullptr) || (broker == nullptr))
        return;

    //
    // ⚠ abiVersion FIRST, and short-circuit order is doing real work here: a
    // broker built against ABI 1 allocated a struct that ENDS before
    // sampleQualifiedIn, so reading the member to test it for null would be
    // reading past it.
    //
    if ((broker->abiVersion < 2) || (broker->sampleQualifiedIn == nullptr))
    {
        KT_W("dds: a reply arrived on service '%s' but the host cannot take one - it predates the service contract", serviceName);
        return;
    }

    KT_T(0, "dds: reply to request %llu on service '%s'", (unsigned long long) requestId, serviceName);

    //
    // A reply comes wrapped exactly as a sample does, and for the same reason -
    // so it is unwrapped in the same place. Only what the server answered
    // crosses the seam.
    //
    std::string payload;

    if (sampleUnwrap(nullptr, json, payload) == true)
        broker->sampleQualifiedIn(bridgeAlias, serviceName, nullptr, DDS_SERVICE_REPLY, payload.c_str(), publishTime);
    else
        broker->sampleQualifiedIn(bridgeAlias, serviceName, nullptr, DDS_SERVICE_REPLY, json, publishTime);
}



// -----------------------------------------------------------------------------
//
// serviceRequestNotification - somebody is asking US something
//
// ⛔ NEVER REACHED IN THE BROKER, which announces no services and therefore
// receives no requests. It is the peer side, and it is here so that a host that
// is not a broker can be the thing the broker talks to.
//
void serviceRequestNotification(const char* serviceName, const char* json, uint64_t requestId, int64_t publishTime)
{
    if ((serviceName == nullptr) || (json == nullptr))
        return;

    BridgeServiceRequestFunc handler = nullptr;

    {
        std::lock_guard<std::mutex> guard(serviceMutex);
        auto it = served.find(serviceName);

        if (it != served.end())
            handler = it->second;
    }

    if (handler == nullptr)
    {
        KT_W("dds: a request arrived on service '%s', which nothing here answers", serviceName);
        return;
    }

    KT_T(0, "dds: request %llu on service '%s'", (unsigned long long) requestId, serviceName);

    //
    // Unwrapped like everything else the Enabler delivers. A host answering a
    // request is handed what the client ASKED, not the envelope it travelled
    // in - the same seam, in the same shape, in both directions.
    //
    std::string payload;

    if (sampleUnwrap(nullptr, json, payload) == true)
        handler(bridgeAlias, serviceName, payload.c_str(), requestId, publishTime);
    else
        handler(bridgeAlias, serviceName, json, requestId, publishTime);
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
// ⭐ topicName MAY BE NULL, and for a reply it has to be. The envelope is keyed
// by the TOPIC the message travelled on, and a service's reply travels on a
// topic whose name is derived from the service's by a convention that belongs
// to the RPC protocol in use - 'rr/<service>Reply' for ROS 2, something else
// for plain DDS. Rebuilding that name here would be this plugin guessing at
// something the Enabler already knows.
//
// So the envelope is recognised by its SHAPE instead: of its three members, one
// is the topic and it is the one holding 'data'. Which is also the better
// answer for topics, and it is used for them too - a sample whose envelope is
// keyed by a name other than the one it was delivered under used to fall
// through unwrapped, and the whole envelope became the value.
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
    KjNode* topicP = ((treeP != nullptr) && (treeP->type == KjObject) && (topicName != nullptr)) ? kjLookup(treeP, topicName) : nullptr;

    //
    // Not keyed by the name we expected, or we had no name to expect: the one
    // member that is an object carrying 'data' is the message.
    //
    if ((topicP == nullptr) && (treeP != nullptr) && (treeP->type == KjObject))
    {
        for (KjNode* childP = treeP->value.firstChildP; childP != nullptr; childP = childP->next)
        {
            if ((childP->type == KjObject) && (kjLookup(childP, "data") != nullptr))
            {
                topicP = childP;
                break;
            }
        }
    }

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

    bool claimed = false;

    {
        std::lock_guard<std::mutex> guard(carriedMutex);
        auto it = carried.find(topicName);

        if (it != carried.end())
        {
            if (it->second.direction == BridgeDirectionOut)
                return;                                // outbound only

            claimed = true;
        }
        else if (unwanted.find(topicName) != unwanted.end())
            return;                                    // offered once already, and refused
    }

    //
    // Only what the publisher wrote crosses the seam, not the Enabler's
    // envelope around it.
    //
    std::string payload;
    int         r;

    if (sampleUnwrap(topicName, json, payload) == true)
        r = broker->sampleIn(bridgeAlias, topicName, payload.c_str(), publishTime);
    else
        r = broker->sampleIn(bridgeAlias, topicName, json, publishTime);

    if ((claimed == false) && (r == BRIDGE_NOT_FOUND))
    {
        std::lock_guard<std::mutex> guard(carriedMutex);
        unwanted.insert(topicName);

        KT_T(0, "dds: the broker has no use for topic '%s' - not offering it again", topicName);
    }
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

            //
            // ⭐ THE SAME ENTRIES THE BROKER READS, AND A DIFFERENT HALF OF THEM.
            //
            // A services entry says two things: which entity attribute the
            // service is bound to, which is NGSI-LD and the broker's business,
            // and what its request and reply types are called, which is DDS and
            // is this plugin's. Neither side parses the other's half, and the
            // deployment describes one service in one place.
            //
            KjNode* servicesP = (ngP != nullptr) ? kjLookup(ngP, "services") : nullptr;

            if (servicesP != nullptr)
            {
                std::lock_guard<std::mutex> guard(serviceMutex);

                for (KjNode* entryP = servicesP->value.firstChildP; entryP != nullptr; entryP = entryP->next)
                {
                    if ((entryP->name == nullptr) || (entryP->type != KjObject))
                        continue;

                    KjNode*      reqP = kjLookup(entryP, "requestType");
                    KjNode*      repP = kjLookup(entryP, "replyType");
                    ServiceTypes types;

                    types.request = ((reqP != nullptr) && (reqP->type == KjString)) ? reqP->value.s : std::string(entryP->name) + "_Request";
                    types.reply   = ((repP != nullptr) && (repP->type == KjString)) ? repP->value.s : std::string(entryP->name) + "_Response";

                    serviceConfig[entryP->name] = types;

                    KT_I("dds: service '%s' (%s -> %s)", entryP->name, types.request.c_str(), types.reply.c_str());
                }
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

    callbacks.service.service_notification         = serviceNotification;
    callbacks.service.service_reply_notification   = serviceReplyNotification;
    callbacks.service.service_request_notification = serviceRequestNotification;
    callbacks.service.service_query                = serviceQuery;

    //
    // The action callbacks stay null on purpose - not carried yet, and a null
    // entry is how the Enabler is told so, the same convention the BridgeDriver
    // uses.
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
        unwanted.clear();
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

    if (kind == BridgeChannelAction)
        return BRIDGE_UNSUPPORTED;                     // actions are not carried yet

    //
    // ⭐ A SERVICE CHANNEL SUBSCRIBES TO NOTHING HERE, and that is the whole
    // difference. A topic has to be listened for, because a publisher decides
    // on its own when to speak. A service only ever answers, and it answers the
    // request this plugin sent - so the reply arrives through the Enabler's own
    // reply callback, correlated to that request, whether or not anybody asked
    // for the endpoint in advance.
    //
    // What the Channel buys is the other direction: the endpoint is now one
    // this plugin will accept a serviceInvoke() for, and one whose reply the
    // broker has somewhere to put.
    //
    std::lock_guard<std::mutex> guard(carriedMutex);
    carried[endpoint] = { kind, direction };
    unwanted.erase(endpoint);

    KT_T(0, "dds: carrying %s '%s'", (kind == BridgeChannelService) ? "service" : "topic", endpoint);

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
// serviceInvoke - ask a service something
//
// Called on a BROKER thread, inside the request that wrote the attribute. It
// hands the request over and returns: the answer arrives later, on an Enabler
// thread, through serviceReplyNotification.
//
// ⚠ The requestId the Enabler hands back is not kept. It correlates the reply
// the Enabler will deliver, and the Enabler does that correlation itself - it
// tells us which service the reply is for, which is the only thing anybody
// downstream needs. Keeping a map here would be keeping a second copy of an
// answer we are already given.
//
int serviceInvoke(const char* endpoint, const char* json)
{
    if ((endpoint == nullptr) || (json == nullptr))
        return BRIDGE_BAD_INPUT;

    if (enabler == nullptr)
        return BRIDGE_ERR;

    uint64_t requestId = 0;

    if (enabler->send_service_request(endpoint, json, requestId) == false)
    {
        //
        // The two ordinary reasons, and they are worth telling apart in the
        // log: nobody is serving the endpoint, or the payload does not fit the
        // request type. The Enabler answers false to both.
        //
        KT_W("dds: could not send a request to service '%s' - no server, or the payload does not fit '%s'",
             endpoint, json);
        return BRIDGE_ERR;
    }

    KT_T(0, "dds: request %llu sent to service '%s'", (unsigned long long) requestId, endpoint);

    return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// serviceServe - announce a service and answer it from here on
//
static int serviceServe(const char* endpoint, BridgeServiceRequestFunc handler)
{
    if ((endpoint == nullptr) || (handler == nullptr))
        return BRIDGE_BAD_INPUT;

    if (enabler == nullptr)
        return BRIDGE_ERR;

    {
        std::lock_guard<std::mutex> guard(serviceMutex);

        //
        // ⚠ The handler goes in BEFORE the announcement, not after. The moment
        // announce_service returns, the service exists on the domain and a
        // client that was waiting for it may already be asking - on an Enabler
        // thread, into serviceRequestNotification, which looks in this very
        // map. Registering afterwards leaves a window in which the first
        // request of a run is answered by nobody.
        //
        served[endpoint] = handler;
    }

    if (enabler->announce_service(endpoint) == false)
    {
        std::lock_guard<std::mutex> guard(serviceMutex);
        served.erase(endpoint);

        KT_W("dds: could not announce service '%s' - are its types configured, and on disk?", endpoint);
        return BRIDGE_ERR;
    }

    KT_I("dds: serving '%s'", endpoint);

    return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// serviceUnserve -
//
static int serviceUnserve(const char* endpoint)
{
    if (endpoint == nullptr)
        return BRIDGE_BAD_INPUT;

    {
        std::lock_guard<std::mutex> guard(serviceMutex);

        if (served.erase(endpoint) == 0)
            return BRIDGE_NOT_FOUND;
    }

    if (enabler != nullptr)
        enabler->revoke_service(endpoint);

    return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// serviceReply - answer one request
//
static int serviceReply(const char* endpoint, uint64_t requestId, const char* json)
{
    if ((endpoint == nullptr) || (json == nullptr))
        return BRIDGE_BAD_INPUT;

    if (enabler == nullptr)
        return BRIDGE_ERR;

    if (enabler->send_service_reply(endpoint, json, requestId) == false)
    {
        KT_W("dds: could not reply to request %llu on service '%s' - answered already, or never asked",
             (unsigned long long) requestId, endpoint);
        return BRIDGE_NOT_FOUND;
    }

    KT_T(0, "dds: replied to request %llu on service '%s'", (unsigned long long) requestId, endpoint);

    return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// ddsServer - the peer side of this plugin
//
static const BridgeServer ddsServer =
{
    BRIDGE_ABI_VERSION,
    serviceServe,
    serviceUnserve,
    serviceReply
};



// -----------------------------------------------------------------------------
//
// serverIface -
//
// ⛔ The broker never calls this. See BridgeServer.h.
//
const BridgeServer* serverIface()
{
    return &ddsServer;
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

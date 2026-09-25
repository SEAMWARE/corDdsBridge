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
#include <mutex>                                       // std::mutex, std::recursive_mutex
#include <chrono>                                      // std::chrono::steady_clock
#include <condition_variable>                          // std::condition_variable
#include <deque>                                       // std::deque
#include <thread>                                      // std::thread
#include <set>                                         // std::set
#include <string>                                      // std::string
#include <vector>                                      // std::vector

#include <link.h>                                      // dl_iterate_phdr, dl_phdr_info
#include <limits.h>                                    // PATH_MAX
#include <cstdlib>                                     // realpath

#include <fastdds/config.hpp>                          // FASTDDS_VERSION_STR
#include <fastcdr/config.h>                            // FASTCDR_VERSION_STR
#include <ddsenabler/library/config.h>                 // DDSENABLER_VERSION_MAJOR, DDSENABLER_VERSION_MINOR

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



// -----------------------------------------------------------------------------
//
// Actions - what the configuration says about each, and the QoS they announce
//
// A ROS 2 action is five DDS entities, and every one of their types follows
// from the action's own: <type>_SendGoal_Request_, _SendGoal_Response_,
// _GetResult_Request_, _GetResult_Response_ and _FeedbackMessage_, plus the two
// every action shares, action_msgs' CancelGoal and GoalStatusArray. So an
// actions entry names the action type once ("type") and the rest is derived.
//
// The status topic is TRANSIENT LOCAL, as ROS 2 publishes it - a client that
// joins late still reads where each goal stands. The rest is reliable and
// volatile, as a service's topics are.
//
#define ACTION_STATUS_QOS  "reliability: true\ndurability: true\nownership: false\nkeyed: false"

std::map<std::string, std::string>   actionConfig;     // endpoint -> action type, from the configuration file
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
// Upcalls - what the Enabler hands over, delivered to the broker by our own thread
//
// ⭐ EVERY ENABLER CALLBACK RUNS INSIDE THE ENABLER'S OWN LOCK (its Handler's
// mtx_, taken in add_data and held across the notification), and the same lock
// is taken by send_service_request. So nothing reached from a callback may wait
// for anything a thread calling the Enabler might hold - and the broker, reached
// from a callback, stores, takes its own locks, and may wait.
//
// It did deadlock, with two waited-for service requests in flight
// (serviceInvokeTracked holds trackedMutex across send_service_request, which
// wants mtx_; the reply callback held mtx_ and wanted trackedMutex): every
// request after it timed out, and the broker never answered another reply.
//
// So a callback only copies what arrived onto this queue and returns, and one
// thread of the plugin's own hands it to the broker, in arrival order, holding
// none of the Enabler's locks. The queue is bounded; a callback waits for room,
// which is the back-pressure the Enabler had anyway while the broker stored
// each sample inside its lock.
//
enum UpcallKind
{
    UpcallData,
    UpcallReply,
    UpcallGoalFeedback,
    UpcallGoalStatus,
    UpcallGoalResult
};

struct Upcall
{
    UpcallKind   kind;
    std::string  endpoint;
    std::string  json;
    uint64_t     requestId;
    int64_t      publishTime;

    // A goal's events only
    eprosima::ddsenabler::participants::UUID  goalUuid;
    int                                       statusCode;
    std::string                               statusMessage;
};

static const size_t              UPCALL_QUEUE_MAX = 10000;
static std::deque<Upcall>        upcallQueue;
static std::mutex                upcallMutex;
static std::condition_variable   upcallReady;
static std::condition_variable   upcallRoom;
static std::thread               upcallThread;
static bool                      upcallRunning = false;

static void dataDeliver(const char* topicName, const char* json, int64_t publishTime);
static void replyDeliver(const char* serviceName, const char* json, uint64_t requestId, int64_t publishTime);
static void goalDeliver(const Upcall& upcall);
static void goalSweep();



// -----------------------------------------------------------------------------
//
// upcallQueueAdd - on an ENABLER thread: copy, queue, return
//
static void upcallQueueAdd(UpcallKind kind, const char* endpoint, const char* json, uint64_t requestId, int64_t publishTime)
{
    std::unique_lock<std::mutex> lock(upcallMutex);

    upcallRoom.wait(lock, [] { return (upcallRunning == false) || (upcallQueue.size() < UPCALL_QUEUE_MAX); });

    if (upcallRunning == false)
        return;                                        // closing - nothing reaches the broker from here on

    Upcall upcall;

    upcall.kind        = kind;
    upcall.endpoint    = endpoint;
    upcall.json        = (json != nullptr) ? json : "";
    upcall.requestId   = requestId;
    upcall.publishTime = publishTime;
    upcall.statusCode  = 0;

    upcallQueue.push_back(std::move(upcall));
    upcallReady.notify_one();
}



// -----------------------------------------------------------------------------
//
// upcallGoalQueueAdd - a goal's event, on an ENABLER thread: copy, queue, return
//
static void upcallGoalQueueAdd(UpcallKind                                       kind,
                               const char*                                      actionName,
                               const eprosima::ddsenabler::participants::UUID&  goalUuid,
                               const char*                                      json,
                               int                                              statusCode,
                               const char*                                      statusMessage,
                               int64_t                                          publishTime)
{
    std::unique_lock<std::mutex> lock(upcallMutex);

    upcallRoom.wait(lock, [] { return (upcallRunning == false) || (upcallQueue.size() < UPCALL_QUEUE_MAX); });

    if (upcallRunning == false)
        return;

    Upcall upcall;

    upcall.kind          = kind;
    upcall.endpoint      = actionName;
    upcall.json          = (json != nullptr) ? json : "";
    upcall.requestId     = 0;
    upcall.publishTime   = publishTime;
    upcall.goalUuid      = goalUuid;
    upcall.statusCode    = statusCode;
    upcall.statusMessage = (statusMessage != nullptr) ? statusMessage : "";

    upcallQueue.push_back(std::move(upcall));
    upcallReady.notify_one();
}



// -----------------------------------------------------------------------------
//
// upcallLoop - the delivery thread
//
static void upcallLoop()
{
    for (;;)
    {
        Upcall upcall;

        {
            std::unique_lock<std::mutex> lock(upcallMutex);

            //
            // Timed: a goal whose result never comes is ended by goalSweep(),
            // and that has to happen whether or not anything else arrives.
            //
            upcallReady.wait_for(lock, std::chrono::milliseconds(200),
                                 [] { return (upcallRunning == false) || (upcallQueue.empty() == false); });

            if (upcallRunning == false)
                return;

            if (upcallQueue.empty() == true)
            {
                lock.unlock();
                goalSweep();
                continue;
            }

            upcall = std::move(upcallQueue.front());
            upcallQueue.pop_front();
            upcallRoom.notify_one();
        }

        if (upcall.kind == UpcallData)
            dataDeliver(upcall.endpoint.c_str(), upcall.json.c_str(), upcall.publishTime);
        else if (upcall.kind == UpcallReply)
            replyDeliver(upcall.endpoint.c_str(), upcall.json.c_str(), upcall.requestId, upcall.publishTime);
        else
            goalDeliver(upcall);
    }
}



// -----------------------------------------------------------------------------
//
// upcallsStart / upcallsStop -
//
// Stop discards what is still queued: it runs as the broker closes, and nothing
// may reach the broker once close() has returned.
//
static void upcallsStart()
{
    std::lock_guard<std::mutex> guard(upcallMutex);

    upcallRunning = true;
    upcallThread  = std::thread(upcallLoop);
}

static void upcallsStop()
{
    {
        std::lock_guard<std::mutex> guard(upcallMutex);

        if (upcallRunning == false)
            return;

        upcallRunning = false;
        upcallQueue.clear();
    }

    upcallReady.notify_all();
    upcallRoom.notify_all();

    if (upcallThread.joinable())
        upcallThread.join();
}



// -----------------------------------------------------------------------------
//
// dataNotification / serviceReplyNotification - the Enabler's callbacks: queue only
//
void dataNotification(const char* topicName, const char* json, int64_t publishTime)
{
    if ((topicName == nullptr) || (json == nullptr))
        return;

    upcallQueueAdd(UpcallData, topicName, json, 0, publishTime);
}

void serviceReplyNotification(const char* serviceName, const char* json, uint64_t requestId, int64_t publishTime)
{
    if ((serviceName == nullptr) || (json == nullptr))
        return;

    upcallQueueAdd(UpcallReply, serviceName, json, requestId, publishTime);
}



// -----------------------------------------------------------------------------
//
// Tracked requests - the broker's token, by the Enabler's request id
//
// A request somebody waits for (ddsSync, see serviceInvokeTracked) must have
// its reply handed back with the token the broker gave it. The Enabler knows
// the reply only by ITS request id, so this is the one place the two meet.
//
// ⭐ ONE LOCK ACROSS "SEND AND REMEMBER", and the reply's delivery takes it too.
// The request id exists only once send_service_request has returned, and the
// reply may be delivered before this thread gets to write the id down; taking
// the lock first makes the reply wait for the entry instead of missing it and
// going the untracked way.
//
// ⚠ The reply WAITS on the delivery thread (see Upcalls), never on an Enabler
// thread: send_service_request takes the Enabler's lock, and an Enabler thread
// waiting for this one while holding it was a deadlock.
//
// An entry whose reply never comes is forgotten after TRACKED_KEEP: the request
// waiting for it gave up long before, and the broker drops a reply that late
// anyway.
//
struct TrackedRequest
{
    uint64_t                               token;
    std::chrono::steady_clock::time_point  sentAt;
};

static std::recursive_mutex                    trackedMutex;
static std::map<uint64_t, TrackedRequest>      trackedRequests;
static const std::chrono::minutes              TRACKED_KEEP(10);



// -----------------------------------------------------------------------------
//
// trackedTake - the token a reply must carry back, 0 if it was not tracked
//
static uint64_t trackedTake(uint64_t requestId)
{
    std::lock_guard<std::recursive_mutex> guard(trackedMutex);
    auto it = trackedRequests.find(requestId);

    if (it == trackedRequests.end())
        return 0;

    uint64_t token = it->second.token;
    trackedRequests.erase(it);

    return token;
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
// Except for a request somebody WAITS for, whose reply goes back through
// replyIn() with the broker's own token - see the tracked requests above.
//
static void replyDeliver(const char* serviceName, const char* json, uint64_t requestId, int64_t publishTime)
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
    const char* answer = (sampleUnwrap(nullptr, json, payload) == true) ? payload.c_str() : json;
    uint64_t    token  = trackedTake(requestId);

    //
    // ABI 3 AND a non-NULL slot: a host that is not the broker (ftClient) is
    // built against the same header and fills in only what it needs.
    //
    if ((token != 0) && (broker->abiVersion >= 3) && (broker->replyIn != nullptr))
        broker->replyIn(bridgeAlias, serviceName, token, nullptr, DDS_SERVICE_REPLY, answer, publishTime);
    else
        broker->sampleQualifiedIn(bridgeAlias, serviceName, nullptr, DDS_SERVICE_REPLY, answer, publishTime);
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
static void dataDeliver(const char* topicName, const char* json, int64_t publishTime)
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

// -----------------------------------------------------------------------------
//
// Goals - the broker's token, by the Enabler's goal UUID
//
// The broker names a goal by the token it chose (actionGoalSend); the Enabler
// names it by the UUID send_action_goal makes, and every event it delivers
// carries only that. This is where the two meet.
//
// ⭐ ONE FINAL EVENT PER GOAL, AND IT IS THE LATER OF TWO. An accepted goal ends
// with a terminal status (on the status topic) AND a result (the get-result
// reply), in either order, on different Enabler threads - the Enabler keeps the
// goal until it has seen both. So the first of the two goes to the broker at
// once, and the second goes with final set. Nothing is held back.
//
// A goal with no result to come ends on its status alone: REJECTED (never
// accepted, so never asked for a result), and the Enabler's own "Action goal
// aborted", sent when it could not even ask for the result. And in case a
// result never comes for another reason, goalSweep() ends a goal RESULT_WAIT
// after its terminal status, with a final event of its own.
//
// goalMutex is taken on the delivery thread and by actionGoalSend/Cancel, across
// their Enabler calls - never on an Enabler thread (see Upcalls), so it cannot
// be part of a cycle with the Enabler's lock.
//
struct DdsGoal
{
    uint64_t                                  token;
    std::string                               endpoint;
    std::string                               uuidText;
    int                                       state        = BridgeGoalUnknown;
    bool                                      terminalSeen = false;
    bool                                      resultSeen   = false;
    std::chrono::steady_clock::time_point     terminalAt;
};

static std::mutex                                  goalMutex;
static std::map<std::string, DdsGoal>              goalsByUuid;     // uuidText -> goal
static std::map<uint64_t, std::string>             uuidByToken;     // token    -> uuidText
static const std::chrono::seconds                  RESULT_WAIT(5);

#define DDS_ACTION_FEEDBACK  "ddsActionFeedback"
#define DDS_ACTION_STATUS    "ddsActionStatus"
#define DDS_ACTION_RESULT    "ddsActionResult"



// -----------------------------------------------------------------------------
//
// uuidText - the canonical 8-4-4-4-12 form
//
static std::string uuidText(const eprosima::ddsenabler::participants::UUID& uuid)
{
    char buf[37];
    int  n = 0;

    for (int ix = 0; ix < 16; ix++)
    {
        n += snprintf(&buf[n], sizeof(buf) - n, "%02x", uuid[ix]);

        if ((ix == 3) || (ix == 5) || (ix == 7) || (ix == 9))
            buf[n++] = '-';
    }

    buf[n] = 0;
    return std::string(buf);
}



// -----------------------------------------------------------------------------
//
// goalState - the Enabler's StatusCode, as the seam's BridgeGoalState
//
static int goalState(int statusCode, int current)
{
    using eprosima::ddsenabler::participants::StatusCode;

    switch ((StatusCode) statusCode)
    {
    case StatusCode::ACCEPTED:   return BridgeGoalAccepted;
    case StatusCode::EXECUTING:  return BridgeGoalExecuting;
    case StatusCode::CANCELING:  return BridgeGoalCanceling;
    case StatusCode::SUCCEEDED:  return BridgeGoalSucceeded;
    case StatusCode::CANCELED:   return BridgeGoalCanceled;
    case StatusCode::ABORTED:    return BridgeGoalAborted;
    case StatusCode::REJECTED:   return BridgeGoalRejected;
    case StatusCode::TIMEOUT:    return BridgeGoalFailed;
    case StatusCode::FAILED:     return BridgeGoalFailed;
    default:                     return current;       // UNKNOWN, CANCEL_REQUEST_FAILED: not a state
    }
}



// -----------------------------------------------------------------------------
//
// statusName - the name ddsActionStatus carries, the Enabler's own
//
static const char* statusName(int statusCode)
{
    using eprosima::ddsenabler::participants::StatusCode;

    switch ((StatusCode) statusCode)
    {
    case StatusCode::ACCEPTED:              return "ACCEPTED";
    case StatusCode::EXECUTING:             return "EXECUTING";
    case StatusCode::CANCELING:             return "CANCELING";
    case StatusCode::SUCCEEDED:             return "SUCCEEDED";
    case StatusCode::CANCELED:              return "CANCELED";
    case StatusCode::ABORTED:               return "ABORTED";
    case StatusCode::REJECTED:              return "REJECTED";
    case StatusCode::TIMEOUT:               return "TIMEOUT";
    case StatusCode::FAILED:                return "FAILED";
    case StatusCode::CANCEL_REQUEST_FAILED: return "CANCEL_REQUEST_FAILED";
    default:                                return "UNKNOWN";
    }
}



// -----------------------------------------------------------------------------
//
// statusJson - { "code": ..., "message": ... }
//
static std::string statusJson(const char* code, const std::string& message)
{
    std::string out = "{\"code\":\"";

    out += code;
    out += "\",\"message\":\"";

    for (char c : message)
    {
        if ((c == '"') || (c == '\\'))
            out += '\\';

        if ((unsigned char) c >= 0x20)
            out += c;
    }

    out += "\"}";
    return out;
}



// -----------------------------------------------------------------------------
//
// goalEvent - hand one event of a goal to the broker. Caller holds goalMutex.
//
static void goalEvent(const DdsGoal& goal, bool final, const char* subAttrName, const char* json, int64_t publishTime)
{
    if ((broker == nullptr) || (broker->abiVersion < 4) || (broker->goalEventIn == nullptr))
        return;

    std::string alias = "urn:goal:" + goal.uuidText;

    //
    // ABI 5: which part of the goal the payload is - our envelope names say it,
    // and only the plugin knows them
    //
    if ((broker->abiVersion >= 5) && (broker->goalEventPartIn != nullptr))
    {
        int part = BridgeGoalPartNone;

        if      (subAttrName == nullptr)                             part = BridgeGoalPartNone;
        else if (strcmp(subAttrName, DDS_ACTION_STATUS)   == 0)      part = BridgeGoalPartStatus;
        else if (strcmp(subAttrName, DDS_ACTION_FEEDBACK) == 0)      part = BridgeGoalPartFeedback;
        else if (strcmp(subAttrName, DDS_ACTION_RESULT)   == 0)      part = BridgeGoalPartResult;

        broker->goalEventPartIn(bridgeAlias, goal.endpoint.c_str(), goal.token, goal.uuidText.c_str(), alias.c_str(),
                                goal.state, final, part, subAttrName, json, publishTime);
        return;
    }

    broker->goalEventIn(bridgeAlias, goal.endpoint.c_str(), goal.token, goal.uuidText.c_str(), alias.c_str(),
                        goal.state, final, subAttrName, json, publishTime);
}



// -----------------------------------------------------------------------------
//
// goalForget - the goal has had its final event. Caller holds goalMutex.
//
static void goalForget(const std::string& text)
{
    auto it = goalsByUuid.find(text);

    if (it == goalsByUuid.end())
        return;

    uuidByToken.erase(it->second.token);
    goalsByUuid.erase(it);
}



// -----------------------------------------------------------------------------
//
// isCancelReply - is this "status" the Enabler's answer to a CANCEL request?
//
// ⚠ The Enabler reports the reply to a cancel request through the SAME status
// callback, with the SAME codes, as the goal's own status: CANCELED there means
// "your cancel was accepted" (the goal is canceling, not canceled), REJECTED
// means the CANCEL was refused, not the goal. Only the message tells them apart
// - its cancel-reply messages are the ones that begin "Action cancel". Fragile,
// and worth an issue upstream; a code of its own would settle it.
//
static bool isCancelReply(const std::string& message)
{
    return message.compare(0, 13, "Action cancel") == 0;
}



// -----------------------------------------------------------------------------
//
// goalDeliver - a goal's event, on the delivery thread
//
static void goalDeliver(const Upcall& upcall)
{
    std::lock_guard<std::mutex> guard(goalMutex);

    std::string text = uuidText(upcall.goalUuid);
    auto        it   = goalsByUuid.find(text);

    if (it == goalsByUuid.end())
    {
        KT_T(0, "dds: an event for goal %s on '%s' - not a goal of ours, dropped", text.c_str(), upcall.endpoint.c_str());
        return;
    }

    DdsGoal& goal = it->second;

    if (upcall.kind == UpcallGoalFeedback)
    {
        goal.state = BridgeGoalExecuting;
        goalEvent(goal, false, DDS_ACTION_FEEDBACK, upcall.json.c_str(), upcall.publishTime);
        return;
    }

    if (upcall.kind == UpcallGoalResult)
    {
        goal.resultSeen = true;

        //
        // The later of the two ends the goal. The terminal status that came
        // first already set the state this event carries.
        //
        goalEvent(goal, goal.terminalSeen, DDS_ACTION_RESULT, upcall.json.c_str(), upcall.publishTime);

        if (goal.terminalSeen == true)
            goalForget(text);

        return;
    }

    //
    // A status - the goal's own, or the reply to a cancel request.
    //
    using eprosima::ddsenabler::participants::StatusCode;

    if (isCancelReply(upcall.statusMessage) == true)
    {
        //
        // Accepted: the goal is canceling, its end comes on the status topic.
        // Refused, or unknown to the server: the goal goes on as it was, and
        // ddsActionStatus says why the cancel did not happen.
        //
        const char* code = "CANCEL_REQUEST_FAILED";

        if ((StatusCode) upcall.statusCode == StatusCode::CANCELED)
        {
            goal.state = BridgeGoalCanceling;
            code       = "CANCELING";
        }

        std::string json = statusJson(code, upcall.statusMessage);
        goalEvent(goal, false, DDS_ACTION_STATUS, json.c_str(), upcall.publishTime);
        return;
    }

    goal.state = goalState(upcall.statusCode, goal.state);

    std::string json     = statusJson(statusName(upcall.statusCode), upcall.statusMessage);
    bool        terminal = BRIDGE_GOAL_TERMINAL(goal.state);

    if (terminal == false)
    {
        goalEvent(goal, false, DDS_ACTION_STATUS, json.c_str(), upcall.publishTime);
        return;
    }

    if (goal.terminalSeen == true)
        return;                                        // a terminal status repeated on the (durable) status topic

    goal.terminalSeen = true;
    goal.terminalAt   = std::chrono::steady_clock::now();

    //
    // No result will come for a goal that was never accepted, nor for one the
    // Enabler could not ask a result for ("Action goal aborted", from the goal
    // reply rather than the server) - the status is the end of it.
    //
    bool noResult = (goal.state == BridgeGoalRejected) || (upcall.statusMessage == "Action goal aborted");
    bool final    = (goal.resultSeen == true) || (noResult == true);

    goalEvent(goal, final, DDS_ACTION_STATUS, json.c_str(), upcall.publishTime);

    if (final == true)
        goalForget(text);
}



// -----------------------------------------------------------------------------
//
// goalSweep - end the goals whose result has not come RESULT_WAIT after their end
//
static void goalSweep()
{
    std::lock_guard<std::mutex> guard(goalMutex);
    auto                        now = std::chrono::steady_clock::now();

    for (auto it = goalsByUuid.begin(); it != goalsByUuid.end(); )
    {
        DdsGoal& goal = it->second;

        if ((goal.terminalSeen == true) && (goal.resultSeen == false) && (now - goal.terminalAt > RESULT_WAIT))
        {
            KT_W("dds: goal %s on '%s' ended without a result - closing it", goal.uuidText.c_str(), goal.endpoint.c_str());
            goalEvent(goal, true, nullptr, nullptr, 0);
            uuidByToken.erase(goal.token);
            it = goalsByUuid.erase(it);
        }
        else
            ++it;
    }
}



// -----------------------------------------------------------------------------
//
// The Enabler's action-client callbacks - queue only, as every callback here
//
void actionFeedbackNotification(const char* actionName, const char* json, const eprosima::ddsenabler::participants::UUID& goalId, int64_t publishTime)
{
    if ((actionName == nullptr) || (json == nullptr))
        return;

    std::string payload;
    const char* answer = (sampleUnwrap(nullptr, json, payload) == true) ? payload.c_str() : json;

    upcallGoalQueueAdd(UpcallGoalFeedback, actionName, goalId, answer, 0, nullptr, publishTime);
}

void actionResultNotification(const char* actionName, const char* json, const eprosima::ddsenabler::participants::UUID& goalId, int64_t publishTime)
{
    if ((actionName == nullptr) || (json == nullptr))
        return;

    std::string payload;
    const char* answer = (sampleUnwrap(nullptr, json, payload) == true) ? payload.c_str() : json;

    upcallGoalQueueAdd(UpcallGoalResult, actionName, goalId, answer, 0, nullptr, publishTime);
}

void actionStatusNotification(const char*                                      actionName,
                              const eprosima::ddsenabler::participants::UUID&  goalId,
                              eprosima::ddsenabler::participants::StatusCode   statusCode,
                              const char*                                      statusMessage,
                              int64_t                                          publishTime)
{
    if (actionName == nullptr)
        return;

    upcallGoalQueueAdd(UpcallGoalStatus, actionName, goalId, nullptr, (int) statusCode, statusMessage, publishTime);
}



// -----------------------------------------------------------------------------
//
// actionNotification - an action was discovered on the domain
//
void actionNotification(const char* actionName, const eprosima::ddsenabler::participants::ActionInfo& actionInfo)
{
    if (actionName == nullptr)
        return;

    KT_T(0, "dds: action '%s' discovered (goal '%s')", actionName, actionInfo.goal.request.type_name.c_str());
}



// -----------------------------------------------------------------------------
//
// actionQuery - the Enabler needs an action's types
//
// FALSE for an action the configuration does not name, as serviceQuery.
//
bool actionQuery(const char* actionName, eprosima::ddsenabler::participants::ActionInfo& actionInfo)
{
    using eprosima::ddsenabler::participants::TopicInfo;
    using eprosima::ddsenabler::participants::ServiceInfo;

    if (actionName == nullptr)
        return false;

    std::string type;

    {
        std::lock_guard<std::mutex> guard(serviceMutex);
        auto it = actionConfig.find(actionName);

        if (it == actionConfig.end())
        {
            KT_T(0, "dds: action '%s' is not in the configuration - its types are unknown", actionName);
            return false;
        }

        type = it->second;
    }

    actionInfo.goal     = ServiceInfo(TopicInfo(type + "_SendGoal_Request_",   SERVICE_QOS), TopicInfo(type + "_SendGoal_Response_",   SERVICE_QOS));
    actionInfo.result   = ServiceInfo(TopicInfo(type + "_GetResult_Request_",  SERVICE_QOS), TopicInfo(type + "_GetResult_Response_",  SERVICE_QOS));
    actionInfo.cancel   = ServiceInfo(TopicInfo("action_msgs::srv::dds_::CancelGoal_Request_",  SERVICE_QOS),
                                      TopicInfo("action_msgs::srv::dds_::CancelGoal_Response_", SERVICE_QOS));
    actionInfo.feedback = TopicInfo(type + "_FeedbackMessage_", SERVICE_QOS);
    actionInfo.status   = TopicInfo("action_msgs::msg::dds_::GoalStatusArray_", ACTION_STATUS_QOS);

    return true;
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

            //
            // Actions, the same way: the broker reads the NGSI-LD half, this
            // plugin reads "type" - the ROS 2 action type every one of the
            // action's DDS types is derived from (see actionQuery).
            //
            KjNode* actionsP = (ngP != nullptr) ? kjLookup(ngP, "actions") : nullptr;

            if (actionsP != nullptr)
            {
                std::lock_guard<std::mutex> guard(serviceMutex);

                for (KjNode* entryP = actionsP->value.firstChildP; entryP != nullptr; entryP = entryP->next)
                {
                    if ((entryP->name == nullptr) || (entryP->type != KjObject))
                        continue;

                    KjNode* typeP = kjLookup(entryP, "type");

                    if ((typeP == nullptr) || (typeP->type != KjString))
                    {
                        KT_W("dds: action '%s' names no \"type\" - its goals cannot be sent", entryP->name);
                        continue;
                    }

                    actionConfig[entryP->name] = typeP->value.s;
                    KT_I("dds: action '%s' (%s)", entryP->name, typeP->value.s);
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
    // The action CLIENT side. The server side (goal and cancel requests) stays
    // null: the broker sends goals, it does not run them.
    //
    callbacks.action.action_notification          = actionNotification;
    callbacks.action.action_feedback_notification = actionFeedbackNotification;
    callbacks.action.action_status_notification   = actionStatusNotification;
    callbacks.action.action_result_notification   = actionResultNotification;
    callbacks.action.action_query                 = actionQuery;

    //
    // Before the Enabler exists: its first callback needs somewhere to queue.
    //
    upcallsStart();

    if (eprosima::ddsenabler::create_dds_enabler(configFile, callbacks, enabler) == false)
    {
        KT_E("unable to create the DDS Enabler from '%s'", configFile);
        upcallsStop();
        return BRIDGE_ERR;
    }

    KT_I("dds bridge up, configured from '%s'", configFile);

    return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// close -
//
// Must not return while a thread could still be delivering to the broker: the
// broker tears down what sampleIn reaches immediately afterwards. Releasing the
// Enabler stops its threads - nothing more is queued - and stopping the
// delivery thread, which discards what was still queued, stops the rest.
//
void close()
{
    {
        std::lock_guard<std::mutex> guard(carriedMutex);
        carried.clear();
        unwanted.clear();
    }

    enabler.reset();
    upcallsStop();

    {
        std::lock_guard<std::mutex> guard(goalMutex);
        goalsByUuid.clear();
        uuidByToken.clear();
    }

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

    KT_T(0, "dds: carrying %s '%s'", (kind == BridgeChannelAction) ? "action" : (kind == BridgeChannelService) ? "service" : "topic", endpoint);

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
// serviceInvokeTracked - ask a service something, for a request that waits
//
// serviceInvoke(), plus the broker's token remembered next to the Enabler's
// request id, so that the reply can be handed back with it. See the tracked
// requests above, and BridgeDriver.h.
//
int serviceInvokeTracked(const char* endpoint, const char* json, uint64_t token)
{
    if ((endpoint == nullptr) || (json == nullptr) || (token == 0))
        return BRIDGE_BAD_INPUT;

    if (enabler == nullptr)
        return BRIDGE_ERR;

    std::lock_guard<std::recursive_mutex> guard(trackedMutex);

    //
    // Forget what can no longer be answered to anybody.
    //
    auto now = std::chrono::steady_clock::now();

    for (auto it = trackedRequests.begin(); it != trackedRequests.end(); )
    {
        if (now - it->second.sentAt > TRACKED_KEEP)
            it = trackedRequests.erase(it);
        else
            ++it;
    }

    uint64_t requestId = 0;

    if (enabler->send_service_request(endpoint, json, requestId) == false)
    {
        KT_W("dds: could not send a request to service '%s' - no server, or the payload does not fit '%s'",
             endpoint, json);
        return BRIDGE_ERR;
    }

    trackedRequests[requestId] = TrackedRequest{ token, now };

    KT_T(0, "dds: request %llu sent to service '%s', waited for (token %llu)",
         (unsigned long long) requestId, endpoint, (unsigned long long) token);

    return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// actionGoalSend - send a goal, ABI 4
//
// goalMutex is held across send_action_goal, and that is safe only because no
// Enabler thread ever takes it (see Upcalls): the UUID exists once the send
// returns, and an event for it may be on the delivery thread by then - holding
// the lock makes that event wait for the goal to be registered instead of
// being dropped as not ours.
//
int actionGoalSend(const char* endpoint, const char* json, uint64_t token)
{
    if ((endpoint == nullptr) || (json == nullptr) || (token == 0))
        return BRIDGE_BAD_INPUT;

    if (enabler == nullptr)
        return BRIDGE_ERR;

    std::lock_guard<std::mutex> guard(goalMutex);

    eprosima::ddsenabler::participants::UUID uuid;

    if (enabler->send_action_goal(endpoint, json, uuid) == false)
    {
        KT_W("dds: could not send a goal to action '%s' - no server, or the goal does not fit its type", endpoint);
        return BRIDGE_ERR;
    }

    DdsGoal goal;

    goal.token    = token;
    goal.endpoint = endpoint;
    goal.uuidText = uuidText(uuid);

    goalsByUuid[goal.uuidText] = goal;
    uuidByToken[token]         = goal.uuidText;

    KT_T(0, "dds: goal %s sent to action '%s' (token %llu)", goal.uuidText.c_str(), endpoint, (unsigned long long) token);

    return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// actionGoalCancel - ask for a goal to be cancelled, ABI 4
//
int actionGoalCancel(const char* endpoint, uint64_t token)
{
    if ((endpoint == nullptr) || (enabler == nullptr))
        return BRIDGE_BAD_INPUT;

    std::lock_guard<std::mutex> guard(goalMutex);
    auto                        it = uuidByToken.find(token);

    if (it == uuidByToken.end())
        return BRIDGE_NOT_FOUND;

    //
    // Back from the text to the Enabler's UUID - the text is what is kept, as
    // every event is matched by it.
    //
    eprosima::ddsenabler::participants::UUID uuid;
    const std::string&                       text = it->second;
    int                                      ix   = 0;

    for (size_t c = 0; (c + 1 < text.size()) && (ix < 16); c++)
    {
        if (text[c] == '-')
            continue;

        uuid[ix++] = (uint8_t) std::stoi(text.substr(c, 2), nullptr, 16);
        c++;
    }

    if (enabler->cancel_action_goal(endpoint, uuid) == false)
    {
        KT_W("dds: could not send the cancellation of goal %s on '%s'", text.c_str(), endpoint);
        return BRIDGE_ERR;
    }

    KT_T(0, "dds: cancellation of goal %s sent to '%s'", text.c_str(), endpoint);

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
// SoVersionQuery - what soVersion is looking for, and what it found
//
struct SoVersionQuery
{
    const char*  prefix;                               // "libddsenabler.so."
    std::string  version;                              // "1.2.2", or empty
};



// -----------------------------------------------------------------------------
//
// soVersionSeen - one loaded shared object, offered by dl_iterate_phdr
//
static int soVersionSeen(struct dl_phdr_info* info, size_t size, void* dataP)
{
    (void) size;

    SoVersionQuery* queryP = (SoVersionQuery*) dataP;

    if ((info->dlpi_name == nullptr) || (info->dlpi_name[0] == 0) || (queryP->version.empty() == false))
        return 0;

    const char* slash = strrchr(info->dlpi_name, '/');
    const char* base  = (slash != nullptr) ? slash + 1 : info->dlpi_name;

    if (strncmp(base, queryP->prefix, strlen(queryP->prefix)) != 0)
        return 0;

    //
    // ⭐ THE PATH IS THE SONAME, AND THE SONAME IS NOT THE VERSION. The loader
    // records what it was asked for - libddsenabler.so.1 - which is a symlink
    // carrying only the major. Resolving it reaches the real file, whose name
    // carries all of it.
    //
    char        resolved[PATH_MAX];
    const char* pathP = (realpath(info->dlpi_name, resolved) != nullptr) ? resolved : info->dlpi_name;
    const char* soP   = strstr(pathP, ".so.");

    if (soP != nullptr)
        queryP->version = soP + 4;

    return 1;                                          // found it - stop walking
}



// -----------------------------------------------------------------------------
//
// soVersion - the version in a LOADED library's file name
//
// ⭐ WHAT IS LOADED, NOT WHAT THIS WAS COMPILED AGAINST, and for a version
// endpoint that is the difference that matters. A plugin is a separate shared
// object built at a different time than the stack it links - the whole point of
// it being a plugin - so its headers answer a question nobody asked. The file
// the loader actually opened is the honest answer.
//
// ⚠ It is also the only way to get the Enabler's full version at all: its
// config.h defines DDSENABLER_VERSION_MAJOR and _MINOR and stops there, so the
// macros cannot tell 1.2.0 from 1.2.2 - which is exactly the distinction that
// matters, those being two different DDS stacks.
//
// Falls back to the caller's compiled-in string when the walk finds nothing:
// a version report that says something slightly stale beats one that says
// nothing.
//
static std::string soVersion(const char* prefix, const char* fallback)
{
    SoVersionQuery query{prefix, ""};

    dl_iterate_phdr(soVersionSeen, &query);

    return (query.version.empty() == false) ? query.version : std::string(fallback);
}



// -----------------------------------------------------------------------------
//
// versionInfo -
//
// Named libraries were not enough: "DDS Enabler, Fast DDS" tells an operator
// which stack is linked and nothing about WHICH of it, and eProsima moves fast
// enough that the answer changes underneath a deployment.
//
const char* versionInfo()
{
    static std::string fastdds   = soVersion("libfastdds.so.",     FASTDDS_VERSION_STR);
    static std::string fastcdr   = soVersion("libfastcdr.so.",     FASTCDR_VERSION_STR);
    static std::string enablerFb = std::to_string(DDSENABLER_VERSION_MAJOR) + "." + std::to_string(DDSENABLER_VERSION_MINOR);
    static std::string enabler   = soVersion("libddsenabler.so.",  enablerFb.c_str());

    snprintf(versionBuffer, sizeof(versionBuffer), "dds %s (Fast DDS %s, Fast CDR %s, DDS Enabler %s)",
             CORDDSBRIDGE_VERSION, fastdds.c_str(), fastcdr.c_str(), enabler.c_str());

    return versionBuffer;
}

}  // namespace dds
}  // namespace coraine

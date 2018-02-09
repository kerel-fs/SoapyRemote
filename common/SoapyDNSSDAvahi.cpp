// Copyright (c) 2018-2018 Josh Blum
// SPDX-License-Identifier: BSL-1.0

#include <SoapySDR/Logger.hpp>
#include "SoapyRemoteDefs.hpp"
#include "SoapyURLUtils.hpp"
#include "SoapyDNSSD.hpp"
#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-client/lookup.h>
#include <avahi-common/simple-watch.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <cstdlib> //atoi
#include <cstdio> //snprintf
#include <mutex>
#include <thread>
#include <tuple>
#include <map>

#define SOAPY_REMOTE_DNSSD_NAME "SoapyRemote"

#define SOAPY_REMOTE_DNSSD_TYPE "_soapy._tcp"

static AvahiProtocol ipVerToAvahiProtocol(const int ipVer)
{
    int protocol = AVAHI_PROTO_UNSPEC;
    if (ipVer == SOAPY_REMOTE_IPVER_UNSPEC) protocol = AVAHI_PROTO_UNSPEC;
    if (ipVer == SOAPY_REMOTE_IPVER_INET)   protocol = AVAHI_PROTO_INET;
    if (ipVer == SOAPY_REMOTE_IPVER_INET6)  protocol = AVAHI_PROTO_INET6;
    return protocol;
}

static int avahiProtocolToIpVer(const AvahiProtocol protocol)
{
    int ipVer = SOAPY_REMOTE_IPVER_UNSPEC;
    if (protocol == AVAHI_PROTO_UNSPEC) ipVer = SOAPY_REMOTE_IPVER_UNSPEC;
    if (protocol == AVAHI_PROTO_INET)   ipVer = SOAPY_REMOTE_IPVER_INET;
    if (protocol == AVAHI_PROTO_INET6)  ipVer = SOAPY_REMOTE_IPVER_INET6;
    return ipVer;
}

/***********************************************************************
 * Storage for avahi client
 **********************************************************************/
struct SoapyDNSSDImpl
{
    SoapyDNSSDImpl(void);
    ~SoapyDNSSDImpl(void);
    AvahiSimplePoll *simplePoll;
    std::thread *pollThread;
    AvahiClient *client;
    AvahiEntryGroup *group;
    AvahiServiceBrowser *browser;
    size_t resolversInFlight;
    bool browseComplete;

    std::recursive_mutex mutex;

    void add_result(
        AvahiIfIndex interface,
        AvahiProtocol protocol,
        const std::string &name,
        const std::string &type,
        const std::string &domain,
        const std::string &uuid,
        const std::string &host,
        uint16_t port);

    void remove_result(
        AvahiIfIndex interface,
        AvahiProtocol protocol,
        const std::string &name,
        const std::string &type,
        const std::string &domain);

    typedef std::tuple<AvahiIfIndex, AvahiProtocol, std::string, std::string, std::string> ResultKey;
    typedef std::tuple<std::string, int, std::string> ResultValue;
    std::map<ResultKey, ResultValue> results;
};

static void clientCallback(AvahiClient *c, AvahiClientState state, void *userdata);

SoapyDNSSDImpl::SoapyDNSSDImpl(void):
    simplePoll(nullptr),
    pollThread(nullptr),
    client(nullptr),
    group(nullptr),
    browser(nullptr),
    resolversInFlight(0),
    browseComplete(false)
{
    simplePoll = avahi_simple_poll_new();
    if (simplePoll == nullptr)
    {
        SoapySDR::logf(SOAPY_SDR_ERROR, "avahi_simple_poll_new() failed");
        return;
    }

    int error(0);
    client = avahi_client_new(avahi_simple_poll_get(simplePoll), AVAHI_CLIENT_NO_FAIL, &clientCallback, this, &error);
    if (client == nullptr or error != 0)
    {
        SoapySDR::logf(SOAPY_SDR_ERROR, "avahi_client_new() failed: %s", avahi_strerror(error));
        return;
    }
}

SoapyDNSSDImpl::~SoapyDNSSDImpl(void)
{
    if (simplePoll != nullptr) avahi_simple_poll_quit(simplePoll);
    if (pollThread != nullptr)
    {
        pollThread->join();
        delete pollThread;
    }
    if (browser != nullptr) avahi_service_browser_free(browser);
    if (group != nullptr) avahi_entry_group_free(group);
    if (client != nullptr) avahi_client_free(client);
    if (simplePoll != nullptr) avahi_simple_poll_free(simplePoll);
}

static void clientCallback(AvahiClient *c, AvahiClientState state, void *userdata)
{
    auto impl = (SoapyDNSSDImpl*)userdata;
    switch (state)
    {
    case AVAHI_CLIENT_S_RUNNING: //success
        SoapySDR::logf(SOAPY_SDR_DEBUG, "Avahi client running...");
        break;

    case AVAHI_CLIENT_S_COLLISION:
    case AVAHI_CLIENT_FAILURE: //error
        SoapySDR::logf(SOAPY_SDR_ERROR, "Avahi client failure: %s",  avahi_strerror(avahi_client_errno(c)));
        if (impl->simplePoll != nullptr) avahi_simple_poll_quit(impl->simplePoll);
        break;

    case AVAHI_CLIENT_S_REGISTERING:
    case AVAHI_CLIENT_CONNECTING:
        break;
    }
}

static void groupCallback(AvahiEntryGroup *g, AvahiEntryGroupState state, void* userdata)
{
    auto impl = (SoapyDNSSDImpl*)userdata;
    const auto c = avahi_entry_group_get_client(g);
    switch (state)
    {
    case AVAHI_ENTRY_GROUP_ESTABLISHED: //success
        SoapySDR::logf(SOAPY_SDR_DEBUG, "Avahi group established...");
        break;

    case AVAHI_ENTRY_GROUP_COLLISION:
    case AVAHI_ENTRY_GROUP_FAILURE: //error
        SoapySDR::logf(SOAPY_SDR_ERROR, "Avahi group failure: %s",  avahi_strerror(avahi_client_errno(c)));
        if (impl->simplePoll != nullptr) avahi_simple_poll_quit(impl->simplePoll);
        break;

    case AVAHI_ENTRY_GROUP_UNCOMMITED:
    case AVAHI_ENTRY_GROUP_REGISTERING:
        break;
    }
}

/***********************************************************************
 * SoapyDNSSD interface hooks
 **********************************************************************/
SoapyDNSSD::SoapyDNSSD(void):
    _impl(new SoapyDNSSDImpl())
{
    return;
}

SoapyDNSSD::~SoapyDNSSD(void)
{
    if (_impl != nullptr) delete _impl;
}

void SoapyDNSSD::printInfo(void)
{
    //summary of avahi client connection for server logging
    SoapySDR::logf(SOAPY_SDR_INFO, "Avahi version:  %s", avahi_client_get_version_string(_impl->client));
    SoapySDR::logf(SOAPY_SDR_INFO, "Avahi hostname: %s", avahi_client_get_host_name(_impl->client));
    SoapySDR::logf(SOAPY_SDR_INFO, "Avahi domain:   %s", avahi_client_get_domain_name(_impl->client));
    SoapySDR::logf(SOAPY_SDR_INFO, "Avahi FQDN:     %s", avahi_client_get_host_name_fqdn(_impl->client));
}

bool SoapyDNSSD::status(void)
{
    return avahi_client_get_state(_impl->client) != AVAHI_CLIENT_FAILURE;
}

void SoapyDNSSD::registerService(const std::string &uuid, const std::string &service, const int ipVer)
{
    auto &client = _impl->client;
    auto &group = _impl->group;
    group = avahi_entry_group_new(client, &groupCallback, this);
    if (group == nullptr)
    {
        SoapySDR::logf(SOAPY_SDR_ERROR, "avahi_entry_group_new() failed");
        return;
    }

    //create a name that is unique to this machine
    //the discovery side uses this name for tracking
    char name[1024];
    std::snprintf(name, sizeof(name), "%s @ %s", SOAPY_REMOTE_DNSSD_NAME, avahi_client_get_host_name(client));

    auto txt = avahi_string_list_add_pair(nullptr, "uuid", uuid.c_str());
    int ret = avahi_entry_group_add_service_strlst(
        group,
        AVAHI_IF_UNSPEC,
        ipVerToAvahiProtocol(ipVer),
        AvahiPublishFlags(0),
        name,
        SOAPY_REMOTE_DNSSD_TYPE,
        nullptr,
        nullptr,
        atoi(service.c_str()),
        txt);
    avahi_string_list_free(txt);

    if (ret != 0)
    {
        SoapySDR::logf(SOAPY_SDR_ERROR, "avahi_entry_group_add_service() failed: %s", avahi_strerror(ret));
        return;
    }

    ret = avahi_entry_group_commit(group);
    if (ret != 0)
    {
        SoapySDR::logf(SOAPY_SDR_ERROR, "avahi_entry_group_commit() failed: %s", avahi_strerror(ret));
        return;
    }

    _impl->pollThread = new std::thread(&avahi_simple_poll_loop, _impl->simplePoll);
}

/***********************************************************************
 * Implement host discovery
 **********************************************************************/
void SoapyDNSSDImpl::add_result(
    AvahiIfIndex interface,
    AvahiProtocol protocol,
    const std::string &name,
    const std::string &type,
    const std::string &domain,
    const std::string &uuid,
    const std::string &host,
    uint16_t port)
{
    if (uuid.empty()) return;

    const auto ipVer = avahiProtocolToIpVer(protocol);
    const auto addr = (protocol == AVAHI_PROTO_INET6)? (host + "%" + std::to_string(interface)):host;
    const auto serverURL = SoapyURL("tcp", addr, std::to_string(port)).toString();
    SoapySDR::logf(SOAPY_SDR_DEBUG, "SoapyDNSSD discovered %s [%s] IPv%d", serverURL.c_str(), uuid.c_str(), ipVer);
    auto key = SoapyDNSSDImpl::ResultKey(interface, protocol, name, type, domain);
    auto value = SoapyDNSSDImpl::ResultValue(uuid, ipVer, serverURL);
    std::lock_guard<std::recursive_mutex> l(mutex);
    results[key] = value;
}

void SoapyDNSSDImpl::remove_result(
    AvahiIfIndex interface,
    AvahiProtocol protocol,
    const std::string &name,
    const std::string &type,
    const std::string &domain)
{
    auto key = SoapyDNSSDImpl::ResultKey(interface, protocol, name, type, domain);
    std::string uuid;
    int ipVer;
    std::string serverURL;
    {
        std::lock_guard<std::recursive_mutex> l(mutex);
        auto it = results.find(key);
        if (it == results.end()) return;
        std::tie(uuid, ipVer, serverURL) = it->second;
        results.erase(it);
    }
    SoapySDR::logf(SOAPY_SDR_DEBUG, "SoapyDNSSD removed %s [%s] IPv%d", serverURL.c_str(), uuid.c_str(), ipVer);
}

static void resolverCallback(
    AvahiServiceResolver *r,
    AvahiIfIndex interface,
    AvahiProtocol protocol,
    AvahiResolverEvent event,
    const char *name,
    const char *type,
    const char *domain,
    const char */*host_name*/,
    const AvahiAddress *address,
    uint16_t port,
    AvahiStringList *txt,
    AvahiLookupResultFlags /*flags*/,
    void *userdata)
{
    auto impl = (SoapyDNSSDImpl*)userdata;

    if (event == AVAHI_RESOLVER_FOUND and address != nullptr)
    {
        //extract address
        char addrStr[AVAHI_ADDRESS_STR_MAX];
        avahi_address_snprint(addrStr, sizeof(addrStr), address);

        //extract key/value pairs
        std::map<std::string, std::string> fields;
        for (; txt != nullptr; txt = txt->next)
        {
            char *key(nullptr), *value(nullptr); size_t size(0);
            avahi_string_list_get_pair(txt, &key, &value, &size);
            if (key == nullptr or value == nullptr) continue;
            fields[key] = std::string(value, size);
            avahi_free(key);
            avahi_free(value);
        }

        impl->add_result(interface, protocol, name, type, domain, fields["uuid"], addrStr, port);
    }

    //cleanup
    impl->resolversInFlight--;
    avahi_service_resolver_free(r);
}

static void browserCallback(
    AvahiServiceBrowser *b,
    AvahiIfIndex interface,
    AvahiProtocol protocol,
    AvahiBrowserEvent event,
    const char *name,
    const char *type,
    const char *domain,
    AvahiLookupResultFlags /*flags*/,
    void *userdata)
{
    auto impl = (SoapyDNSSDImpl*)userdata;
    auto c = avahi_service_browser_get_client(b);

    switch (event) {
    case AVAHI_BROWSER_FAILURE:
        SoapySDR::logf(SOAPY_SDR_ERROR, "Avahi browser error: %s", avahi_strerror(avahi_client_errno(c)));
        impl->resolversInFlight = 0;
        impl->browseComplete = true;
        return;

    case AVAHI_BROWSER_NEW:
        if (avahi_service_resolver_new(
            c,
            interface,
            protocol,
            name,
            type,
            domain,
            protocol, //resolve using the same protocol version,
                //or we can get a v6 address when protocol was v4
            AvahiLookupFlags(0),
            resolverCallback,
            userdata) == nullptr
        ) SoapySDR::logf(SOAPY_SDR_ERROR, "avahi_service_resolver_new() failed: %s", avahi_strerror(avahi_client_errno(c)));
        else impl->resolversInFlight++;
        break;

    //don't care about removals, browser lifetime is short
    case AVAHI_BROWSER_REMOVE:
        impl->remove_result(interface, protocol, name, type, domain);
        break;

    //flags the results when the cache is exhausted (or all for now)
    case AVAHI_BROWSER_ALL_FOR_NOW:
    case AVAHI_BROWSER_CACHE_EXHAUSTED:
        impl->browseComplete = true;
        break;
    }
}

std::map<std::string, std::map<int, std::string>> SoapyDNSSD::getServerURLs(const int ipVerReq)
{
    std::lock_guard<std::recursive_mutex> l(_impl->mutex);

    auto &browser = _impl->browser;
    if (browser != nullptr) goto getResults;

    _impl->browser = avahi_service_browser_new(
        _impl->client,
        AVAHI_IF_UNSPEC,
        ipVerToAvahiProtocol(ipVerReq),
        SOAPY_REMOTE_DNSSD_TYPE,
        nullptr,
        AvahiLookupFlags(0),
        &browserCallback,
        _impl);

    if (browser == nullptr)
    {
        int error = avahi_client_errno(_impl->client);
        SoapySDR::logf(SOAPY_SDR_ERROR, "avahi_service_browser_new() failed: %s", avahi_strerror(error));
        return {};
    }

    //run the handler until the results are completed
    while (not _impl->browseComplete or _impl->resolversInFlight != 0)
    {
        avahi_simple_poll_iterate(_impl->simplePoll, -1);
    }

    //run in background for subsequent calls
    _impl->pollThread = new std::thread(&avahi_simple_poll_loop, _impl->simplePoll);

    getResults:
    std::map<std::string, std::map<int, std::string>> uuidToUrl;
    for (const auto &entry : _impl->results)
    {
        std::string uuid;
        int ipVer;
        std::string serverURL;
        std::tie(uuid, ipVer, serverURL) = entry.second;
        uuidToUrl[uuid][ipVer] = serverURL;
    }

    return uuidToUrl;
}

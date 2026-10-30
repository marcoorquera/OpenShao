#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "stream_upnp.h"

#ifndef _WIN32

namespace openshao::stream::upnp {

void Start(uint16_t) {}
void Stop() {}
std::string ExternalEndpoint() { return {}; }

} // namespace openshao::stream::upnp

#else

#ifndef MINIUPNP_STATICLIB
#define MINIUPNP_STATICLIB
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "core/log.h"

#include <miniupnpc.h>
#include <upnpcommands.h>
#include <upnperrors.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace openshao::stream::upnp {

namespace {

std::thread s_thread;
std::mutex s_mutex;
std::condition_variable s_cv;
bool s_stop = false;
std::string s_external_endpoint;

bool IsPrivateOrCgnat(const char* ip) {
    if (!ip) return false;
    unsigned a = 0, b = 0, c = 0, d = 0;
    char extra = 0;
    if (std::sscanf(ip, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    if (a == 10) return true;
    if (a == 172 && b >= 16 && b <= 31) return true;
    if (a == 192 && b == 168) return true;
    if (a == 100 && b >= 64 && b <= 127) return true;
    return false;
}

void Worker(uint16_t port) {
    WSADATA wsa{};
    const bool wsa_ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    if (!wsa_ok) {
        LOGI("stream: UPnP unavailable (%s)", "Winsock initialization failed");
        return;
    }

    auto check_stop = [&]() {
        std::lock_guard lock(s_mutex);
        return s_stop;
    };

    if (check_stop()) {
        WSACleanup();
        return;
    }

    int upnp_err = 0;
    struct UPNPDev* devlist = upnpDiscover(2000, nullptr, nullptr, UPNP_LOCAL_PORT_ANY, 0, 2, &upnp_err);

    if (check_stop()) {
        if (devlist) freeUPNPDevlist(devlist);
        WSACleanup();
        return;
    }

    if (!devlist) {
        const char* err_str = upnp_err ? strupnperror(upnp_err) : nullptr;
        LOGI("stream: UPnP unavailable (%s)", (err_str && *err_str) ? err_str : "no UPnP devices found");
        WSACleanup();
        return;
    }

    struct UPNPUrls urls{};
    struct IGDdatas data{};
    char lanaddr[64] = {0};
    char wanaddr[64] = {0};
    const int igd_status = UPNP_GetValidIGD(devlist, &urls, &data, lanaddr, sizeof(lanaddr), wanaddr, sizeof(wanaddr));
    freeUPNPDevlist(devlist);
    devlist = nullptr;

    if (check_stop()) {
        if (igd_status != 0) FreeUPNPUrls(&urls);
        WSACleanup();
        return;
    }

    if (igd_status != 1 && igd_status != 2) {
        const char* reason = "no IGD found";
        if (igd_status == 3) reason = "IGD reported not connected";
        else if (igd_status == 4) reason = "UPnP device not recognized as IGD";
        LOGI("stream: UPnP unavailable (%s)", reason);
        if (igd_status != 0) FreeUPNPUrls(&urls);
        WSACleanup();
        return;
    }

    char ext_ip[64] = {0};
    const int ext_err = UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, ext_ip);
    if (ext_err != UPNPCOMMAND_SUCCESS || ext_ip[0] == '\0') {
        if (wanaddr[0] != '\0') {
            std::snprintf(ext_ip, sizeof(ext_ip), "%s", wanaddr);
        } else {
            const char* err_str = strupnperror(ext_err);
            LOGI("stream: UPnP unavailable (%s)", (err_str && *err_str) ? err_str : "failed to get external IP");
            FreeUPNPUrls(&urls);
            WSACleanup();
            return;
        }
    }

    const std::string port_str = std::to_string(port);
    int r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                                port_str.c_str(), port_str.c_str(),
                                lanaddr, "OpenShao", "UDP", nullptr, "3600");
    if (r == 725) { // OnlyPermanentLeasesSupported
        r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                                port_str.c_str(), port_str.c_str(),
                                lanaddr, "OpenShao", "UDP", nullptr, "0");
    }

    if (check_stop()) {
        if (r == UPNPCOMMAND_SUCCESS) {
            UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype, port_str.c_str(), "UDP", nullptr);
        }
        FreeUPNPUrls(&urls);
        WSACleanup();
        return;
    }

    if (r != UPNPCOMMAND_SUCCESS) {
        const char* err_str = strupnperror(r);
        LOGI("stream: UPnP unavailable (%s)", (err_str && *err_str) ? err_str : ("error " + std::to_string(r)).c_str());
        FreeUPNPUrls(&urls);
        WSACleanup();
        return;
    }

    {
        std::lock_guard lock(s_mutex);
        s_external_endpoint = std::string(ext_ip) + ":" + port_str;
    }
    LOGI("stream: UPnP mapped UDP %u, public address %s", unsigned(port), ext_ip);
    if (IsPrivateOrCgnat(ext_ip)) {
        LOGI("stream: UPnP external address %s is not public; direct internet connection may not work", ext_ip);
    }

    while (true) {
        std::unique_lock lock(s_mutex);
        if (s_cv.wait_for(lock, std::chrono::minutes(30), [&] { return s_stop; })) {
            break;
        }
        lock.unlock();
        UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                            port_str.c_str(), port_str.c_str(),
                            lanaddr, "OpenShao", "UDP", nullptr, "3600");
    }

    UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype, port_str.c_str(), "UDP", nullptr);
    FreeUPNPUrls(&urls);
    {
        std::lock_guard lock(s_mutex);
        s_external_endpoint.clear();
    }
    WSACleanup();
}

} // namespace

void Start(uint16_t port) {
    Stop();
    std::lock_guard lock(s_mutex);
    s_stop = false;
    s_external_endpoint.clear();
    s_thread = std::thread(Worker, port);
}

void Stop() {
    {
        std::lock_guard lock(s_mutex);
        if (!s_thread.joinable()) {
            s_external_endpoint.clear();
            return;
        }
        s_stop = true;
    }
    s_cv.notify_all();
    if (s_thread.joinable()) {
        s_thread.join();
    }
    std::lock_guard lock(s_mutex);
    s_external_endpoint.clear();
}

std::string ExternalEndpoint() {
    std::lock_guard lock(s_mutex);
    return s_external_endpoint;
}

} // namespace openshao::stream::upnp

#endif


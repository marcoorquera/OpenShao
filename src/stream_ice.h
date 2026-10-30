#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace openshao::stream {

enum class IceState { Idle, Gathering, Ready, Connecting, Connected, Failed };

// One ICE agent for one peer. Packets are whole datagrams. Thread-safe.
class IceLink {
public:
    IceLink();
    ~IceLink();  // Stop()
    IceLink(const IceLink&) = delete;
    IceLink& operator=(const IceLink&) = delete;

    // Opens the local port and starts gathering candidates (host + STUN server-reflexive) without
    // waiting; State() becomes Ready once gathered. Tests pass use_stun = false to stay off the
    // internet. Start and Stop must not run concurrently with each other.
    bool Start(std::string& error, bool use_stun = true);
    void Stop();
    IceState State() const;
    // Empty until State() is Ready or later.
    std::string LocalDescription() const;
    // Starts connectivity checks, at once when Ready or after gathering when called earlier.
    // Call once; later calls fail.
    bool SetRemote(const std::string& description, std::string& error);
    bool Send(const void* data, size_t size);
    // Waits up to timeout_ms for one datagram. Returns its size, 0 on timeout, -1 after Stop().
    int Receive(uint8_t* buffer, size_t size, uint32_t timeout_ms);
    // Selected pair for logs, for example "192.168.1.5:50000 -> 203.0.113.7:42299 (srflx)".
    std::string Route() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openshao::stream

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "netplay.h"
#include "core/log.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <thread>
#include <vector>

namespace openshao::netplay {
namespace {

struct InProcessQueue {
    uint16_t port = 0;
    std::deque<std::pair<uint16_t, Packet>> packets;
};
std::mutex g_inproc_lock;
std::vector<InProcessQueue> g_inproc_queues;

void RegisterInProcessPort(uint16_t port) {
    std::lock_guard guard(g_inproc_lock);
    for (auto& q : g_inproc_queues) {
        if (q.port == port) { q.packets.clear(); return; }
    }
    g_inproc_queues.push_back({port, {}});
}

void UnregisterInProcessPort(uint16_t port) {
    std::lock_guard guard(g_inproc_lock);
    g_inproc_queues.erase(
        std::remove_if(g_inproc_queues.begin(), g_inproc_queues.end(),
                       [port](const InProcessQueue& q) { return q.port == port; }),
        g_inproc_queues.end());
}

bool SendInProcessPacket(uint16_t src_port, uint16_t dst_port, const Packet& pkt) {
    std::lock_guard guard(g_inproc_lock);
    for (auto& q : g_inproc_queues) {
        if (q.port == dst_port) {
            q.packets.emplace_back(src_port, pkt);
            return true;
        }
    }
    return false;
}

bool RecvInProcessPacket(uint16_t local_port, uint16_t& src_port_out, Packet& pkt_out) {
    std::lock_guard guard(g_inproc_lock);
    for (auto& q : g_inproc_queues) {
        if (q.port == local_port && !q.packets.empty()) {
            src_port_out = q.packets.front().first;
            pkt_out = q.packets.front().second;
            q.packets.pop_front();
            return true;
        }
    }
    return false;
}

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
void CloseNativeSocket(NativeSocket s) {
    if (s != INVALID_SOCKET) closesocket(s);
}
bool SetNonBlocking(NativeSocket s) {
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
}
bool EnsureSocketSubsystem(std::string& error) {
    static std::mutex init_lock;
    static bool initialized = false;
    std::lock_guard guard(init_lock);
    if (initialized) return true;
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        error = "Winsock initialization failed";
        return false;
    }
    initialized = true;
    return true;
}
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
void CloseNativeSocket(NativeSocket s) {
    if (s >= 0) close(s);
}
bool SetNonBlocking(NativeSocket s) {
    const int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) >= 0;
}
bool EnsureSocketSubsystem(std::string&) { return true; }
#endif

uint32_t GenerateSeed() {
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    uint32_t seed = uint32_t(now ^ (now >> 32));
    return seed ? seed : 0x5348414Fu;
}

int64_t SteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

Session& GlobalSession() {
    static Session instance;
    return instance;
}

} // namespace

WirePad PackPad(const input::Pad& pad) {
    WirePad wire{};
    wire.connected = pad.connected ? 1 : 0;
    wire.buttons = pad.buttons;
    std::memcpy(wire.analog, pad.analog, sizeof(wire.analog));
    wire.lx = pad.lx;
    wire.ly = pad.ly;
    wire.rx = pad.rx;
    wire.ry = pad.ry;
    return wire;
}

input::Pad UnpackPad(const WirePad& wire) {
    input::Pad pad{};
    pad.connected = wire.connected != 0;
    pad.buttons = wire.buttons;
    std::memcpy(pad.analog, wire.analog, sizeof(pad.analog));
    pad.lx = wire.lx;
    pad.ly = wire.ly;
    pad.rx = wire.rx;
    pad.ry = wire.ry;
    return pad;
}

bool ParseEndpoint(std::string_view text, std::string& host_out, uint16_t& port_out) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    if (text.empty()) return false;

    std::string_view host_part = text;
    std::string_view port_part;
    const auto colon = text.rfind(':');
    if (colon != std::string_view::npos) {
        host_part = text.substr(0, colon);
        port_part = text.substr(colon + 1);
        if (port_part.empty()) return false;
    }
    if (host_part.empty()) return false;

    uint16_t parsed_port = kDefaultPort;
    if (!port_part.empty()) {
        uint32_t value = 0;
        for (char c : port_part) {
            if (c < '0' || c > '9') return false;
            value = value * 10u + uint32_t(c - '0');
            if (value > 65535u) return false;
        }
        if (value == 0) return false;
        parsed_port = uint16_t(value);
    }

    host_out.assign(host_part);
    port_out = parsed_port;
    return true;
}

Session::~Session() {
    Stop();
}

int Session::LocalPlayerIndex() const {
    const Role r = role_.load(std::memory_order_acquire);
    if (r == Role::Host) return 0;
    if (r == Role::Client) return 1;
    return -1;
}

bool Session::SendPacket(const Packet& pkt) {
    if (socket_ == intptr_t(kInvalidSocket) || !has_peer_ || peer_addr_len_ == 0) return false;
    const auto sent = sendto(
        NativeSocket(socket_),
        reinterpret_cast<const char*>(&pkt),
        sizeof(pkt),
        0,
        reinterpret_cast<const sockaddr*>(peer_addr_.data()),
        socklen_t(peer_addr_len_));
    if (sent == sizeof(pkt)) return true;
    const auto* sin = reinterpret_cast<const sockaddr_in*>(peer_addr_.data());
    if (ntohl(sin->sin_addr.s_addr) == INADDR_LOOPBACK) {
        return SendInProcessPacket(bound_port_.load(std::memory_order_relaxed), ntohs(sin->sin_port), pkt);
    }
    return false;
}

void Session::StoreRemotePad(uint32_t frame, const input::Pad& pad, uint32_t state_hash, bool include_hash, bool in_gameplay) {
    auto& slot = remote_ring_[frame % kRingSize];
    if (slot.frame != frame || !slot.has_pad) {
        slot.frame = frame;
        slot.pad = pad;
        slot.pad.connected = true;
        slot.in_gameplay = in_gameplay;
        slot.has_pad = true;
        slot.state_hash = include_hash ? state_hash : 0;
        slot.has_hash = include_hash && state_hash != 0;
    } else {
        if (in_gameplay) slot.in_gameplay = true;
        if (include_hash && state_hash != 0 && !slot.has_hash) {
            slot.state_hash = state_hash;
            slot.has_hash = true;
        }
    }
    if (!has_remote_frame_ || frame > highest_remote_frame_) {
        highest_remote_frame_ = frame;
        has_remote_frame_ = true;
    }
}

void Session::PumpIncoming() {
    if (socket_ == intptr_t(kInvalidSocket)) return;
    for (;;) {
        sockaddr_storage from{};
        socklen_t from_len = sizeof(from);
        Packet pkt{};
        const auto received = recvfrom(
            NativeSocket(socket_),
            reinterpret_cast<char*>(&pkt),
            sizeof(pkt),
            0,
            reinterpret_cast<sockaddr*>(&from),
            &from_len);
        if (received <= 0) {
            uint16_t src_port = 0;
            if (!RecvInProcessPacket(bound_port_.load(std::memory_order_relaxed), src_port, pkt)) break;
        } else if (size_t(received) != sizeof(Packet)) {
            continue;
        }
        if (pkt.magic != kPacketMagic || pkt.version != kProtocolVersion) {
            continue;
        }
        const auto type = PacketType(pkt.type);
        if (type == PacketType::Handshake && role_.load(std::memory_order_relaxed) == Role::Host) {
            // Re-send HandshakeAck if the client's first ack was dropped.
            Packet ack{};
            ack.type = uint8_t(PacketType::HandshakeAck);
            ack.player_index = 0;
            ack.input_delay = uint8_t(input_delay_.load(std::memory_order_relaxed));
            ack.rng_seed = agreed_seed_.load(std::memory_order_relaxed);
            SendPacket(ack);
            continue;
        }
        if (type == PacketType::Disconnect) {
            LOGW("netplay: remote peer disconnected at frame %u", current_frame_.load(std::memory_order_relaxed));
            active_.store(false, std::memory_order_release);
            return;
        }
        if (type == PacketType::FrameInput) {
            const uint8_t expected_remote = role_.load(std::memory_order_relaxed) == Role::Host ? 1 : 0;
            if ((pkt.player_index & 0x03u) != expected_remote) continue;
            const bool cur_gameplay = (pkt.player_index & 0x80u) != 0;
            const bool prev_gameplay = (pkt.player_index & 0x40u) != 0;
            if (pkt.frame > 0) {
                StoreRemotePad(pkt.frame - 1, UnpackPad(pkt.prev_pad), 0, false, prev_gameplay);
            }
            StoreRemotePad(pkt.frame, UnpackPad(pkt.pad), pkt.state_hash, true, cur_gameplay);
        }
    }
}

bool Session::Start(const Config& config, std::string& error) {
    Stop();
    if (config.role == Role::None) {
        error = "Netplay role is not specified";
        return false;
    }
    if (config.input_delay == 0 || config.input_delay > kMaxDelay) {
        error = "Netplay input delay must be between 1 and 8 frames";
        return false;
    }
    if (!EnsureSocketSubsystem(error)) return false;

    std::lock_guard guard(mutex_);
    NativeSocket sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == kInvalidSocket) {
        error = "Cannot create UDP socket";
        return false;
    }
    if (!SetNonBlocking(sock)) {
        CloseNativeSocket(sock);
        error = "Cannot configure non-blocking UDP socket";
        return false;
    }

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(config.role == Role::Host ? config.port : 0);

    if (bind(sock, reinterpret_cast<const sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
        CloseNativeSocket(sock);
        error = "Cannot bind UDP socket on port " + std::to_string(config.port);
        return false;
    }

    sockaddr_in actual_addr{};
    socklen_t actual_len = sizeof(actual_addr);
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&actual_addr), &actual_len) == 0) {
        bound_port_.store(ntohs(actual_addr.sin_port), std::memory_order_release);
    } else {
        bound_port_.store(config.port, std::memory_order_release);
    }
    RegisterInProcessPort(bound_port_.load(std::memory_order_relaxed));

    socket_ = intptr_t(sock);
    role_.store(config.role, std::memory_order_release);
    current_frame_.store(0, std::memory_order_release);
    clock_frame_.store(0, std::memory_order_release);
    sub_frame_ticks_.store(0, std::memory_order_release);
    rdtsc_sub_ticks_.store(0, std::memory_order_release);
    desync_count_.store(0, std::memory_order_release);
    is_loopback_.store(false, std::memory_order_release);
    frame_timeout_ms_ = config.frame_timeout_ms ? config.frame_timeout_ms : 60000;
    highest_remote_frame_ = 0;
    has_remote_frame_ = false;
    has_peer_ = false;
    peer_addr_.fill(0);
    peer_addr_len_ = 0;
    for (auto& s : local_ring_) s = {};
    for (auto& s : remote_ring_) s = {};

    const auto start_time = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::milliseconds(config.handshake_timeout_ms ? config.handshake_timeout_ms : 30000);

    if (config.role == Role::Host) {
        const uint32_t seed = config.fixed_seed ? config.fixed_seed : GenerateSeed();
        const uint32_t delay = std::clamp(config.input_delay, 1u, kMaxDelay);
        agreed_seed_.store(seed, std::memory_order_release);
        input_delay_.store(delay, std::memory_order_release);
        LOGI("netplay: hosting on UDP port %u (delay=%u frames, seed=%08X), waiting for client...",
              unsigned(bound_port_.load()), delay, seed);

        while (std::chrono::steady_clock::now() - start_time < timeout) {
            sockaddr_in from{};
            socklen_t from_len = sizeof(from);
            Packet pkt{};
            auto n = recvfrom(sock, reinterpret_cast<char*>(&pkt), sizeof(pkt), 0,
                              reinterpret_cast<sockaddr*>(&from), &from_len);
            if (n <= 0) {
                uint16_t src_port = 0;
                if (RecvInProcessPacket(bound_port_.load(std::memory_order_relaxed), src_port, pkt)) {
                    from.sin_family = AF_INET;
                    from.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                    from.sin_port = htons(src_port);
                    n = sizeof(Packet);
                }
            }
            if (n == sizeof(Packet) && pkt.magic == kPacketMagic && pkt.version == kProtocolVersion &&
                PacketType(pkt.type) == PacketType::Handshake && pkt.player_index == 1) {
                std::memcpy(peer_addr_.data(), &from, sizeof(from));
                peer_addr_len_ = sizeof(from);
                has_peer_ = true;
                is_loopback_.store(ntohl(from.sin_addr.s_addr) == INADDR_LOOPBACK, std::memory_order_release);

                Packet ack{};
                ack.type = uint8_t(PacketType::HandshakeAck);
                ack.player_index = 0;
                ack.input_delay = uint8_t(delay);
                ack.rng_seed = seed;
                for (int attempt = 0; attempt < 3; ++attempt) SendPacket(ack);

                char ip_str[INET_ADDRSTRLEN]{};
                inet_ntop(AF_INET, &from.sin_addr, ip_str, sizeof(ip_str));
                LOGI("netplay: client connected from %s:%u", ip_str, unsigned(ntohs(from.sin_port)));
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        if (!has_peer_) {
            UnregisterInProcessPort(bound_port_.load(std::memory_order_relaxed));
            CloseNativeSocket(sock);
            socket_ = intptr_t(kInvalidSocket);
            role_.store(Role::None, std::memory_order_release);
            error = "Timed out waiting for client handshake on UDP port " + std::to_string(config.port);
            return false;
        }
    } else {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;
        addrinfo* res = nullptr;
        const std::string port_str = std::to_string(config.port);
        if (getaddrinfo(config.host_address.c_str(), port_str.c_str(), &hints, &res) != 0 || !res) {
            UnregisterInProcessPort(bound_port_.load(std::memory_order_relaxed));
            CloseNativeSocket(sock);
            socket_ = intptr_t(kInvalidSocket);
            role_.store(Role::None, std::memory_order_release);
            error = "Cannot resolve host address: " + config.host_address;
            return false;
        }
        std::memcpy(peer_addr_.data(), res->ai_addr, res->ai_addrlen);
        peer_addr_len_ = uint32_t(res->ai_addrlen);
        has_peer_ = true;
        if (res->ai_family == AF_INET) {
            const auto* sin = reinterpret_cast<const sockaddr_in*>(res->ai_addr);
            is_loopback_.store(ntohl(sin->sin_addr.s_addr) == INADDR_LOOPBACK, std::memory_order_release);
        }
        freeaddrinfo(res);

        LOGI("netplay: connecting to host %s:%u...", config.host_address.c_str(), unsigned(config.port));
        bool connected = false;
        auto last_send = start_time - std::chrono::milliseconds(100);

        while (std::chrono::steady_clock::now() - start_time < timeout) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_send >= std::chrono::milliseconds(50)) {
                Packet req{};
                req.type = uint8_t(PacketType::Handshake);
                req.player_index = 1;
                req.input_delay = uint8_t(std::clamp(config.input_delay, 1u, kMaxDelay));
                SendPacket(req);
                last_send = now;
            }

            sockaddr_in from{};
            socklen_t from_len = sizeof(from);
            Packet pkt{};
            auto n = recvfrom(sock, reinterpret_cast<char*>(&pkt), sizeof(pkt), 0,
                              reinterpret_cast<sockaddr*>(&from), &from_len);
            if (n <= 0) {
                uint16_t src_port = 0;
                if (RecvInProcessPacket(bound_port_.load(std::memory_order_relaxed), src_port, pkt)) {
                    n = sizeof(Packet);
                }
            }
            if (n == sizeof(Packet) && pkt.magic == kPacketMagic && pkt.version == kProtocolVersion) {
                if (PacketType(pkt.type) == PacketType::HandshakeAck && pkt.player_index == 0) {
                    const uint32_t delay = std::clamp<uint32_t>(pkt.input_delay, 1u, kMaxDelay);
                    input_delay_.store(delay, std::memory_order_release);
                    agreed_seed_.store(pkt.rng_seed, std::memory_order_release);
                    connected = true;
                    LOGI("netplay: connected to host %s:%u (delay=%u frames, seed=%08X)",
                         config.host_address.c_str(), unsigned(config.port), delay, pkt.rng_seed);
                    break;
                }
                if (PacketType(pkt.type) == PacketType::FrameInput && (pkt.player_index & 0x03u) == 0) {
                    const bool cur_gameplay = (pkt.player_index & 0x80u) != 0;
                    const bool prev_gameplay = (pkt.player_index & 0x40u) != 0;
                    if (pkt.frame > 0) StoreRemotePad(pkt.frame - 1, UnpackPad(pkt.prev_pad), 0, false, prev_gameplay);
                    StoreRemotePad(pkt.frame, UnpackPad(pkt.pad), pkt.state_hash, true, cur_gameplay);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        if (!connected) {
            UnregisterInProcessPort(bound_port_.load(std::memory_order_relaxed));
            CloseNativeSocket(sock);
            socket_ = intptr_t(kInvalidSocket);
            role_.store(Role::None, std::memory_order_release);
            has_peer_ = false;
            error = "Timed out connecting to host " + config.host_address + ":" + std::to_string(config.port);
            return false;
        }
    }

    // Seed initial delay window [0 .. delay - 1] with neutral connected pads on both sides.
    const uint32_t delay = input_delay_.load(std::memory_order_relaxed);
    input::Pad neutral{};
    neutral.connected = true;
    for (uint32_t f = 0; f < delay; ++f) {
        auto& local_slot = local_ring_[f % kRingSize];
        local_slot.frame = f;
        local_slot.pad = neutral;
        local_slot.state_hash = 0;
        local_slot.in_gameplay = false;
        local_slot.has_pad = true;
        local_slot.has_hash = false;

        StoreRemotePad(f, neutral, 0, false, false);
    }
    synced_pads_[0] = neutral;
    synced_pads_[1] = neutral;
    in_advance_.store(false, std::memory_order_release);
    last_advance_ms_.store(SteadyMs(), std::memory_order_release);
    active_.store(true, std::memory_order_release);
    return true;
}

void Session::Stop() {
    std::lock_guard guard(mutex_);
    if (socket_ != intptr_t(kInvalidSocket)) {
        if (active_.load(std::memory_order_relaxed) && has_peer_) {
            Packet bye{};
            bye.type = uint8_t(PacketType::Disconnect);
            bye.player_index = role_.load(std::memory_order_relaxed) == Role::Host ? 0 : 1;
            bye.frame = current_frame_.load(std::memory_order_relaxed);
            SendPacket(bye);
        }
        UnregisterInProcessPort(bound_port_.load(std::memory_order_relaxed));
        CloseNativeSocket(NativeSocket(socket_));
        socket_ = intptr_t(kInvalidSocket);
    }
    active_.store(false, std::memory_order_release);
    is_loopback_.store(false, std::memory_order_release);
    role_.store(Role::None, std::memory_order_release);
    bound_port_.store(0, std::memory_order_release);
    has_peer_ = false;
}

bool Session::AdvanceFrame(const input::Pad& local_pad, uint32_t local_state_hash, bool in_gameplay) {
    std::lock_guard guard(mutex_);
    if (!active_.load(std::memory_order_relaxed)) return false;
    in_advance_.store(true, std::memory_order_release);
    struct AdvanceScope {
        Session& s;
        ~AdvanceScope() {
            s.last_advance_ms_.store(SteadyMs(), std::memory_order_release);
            s.in_advance_.store(false, std::memory_order_release);
        }
    } advance_scope{*this};

    const uint32_t initial_frame = current_frame_.load(std::memory_order_relaxed);
    uint32_t frame = initial_frame;
    const uint32_t delay = input_delay_.load(std::memory_order_relaxed);
    const uint8_t base_player = role_.load(std::memory_order_relaxed) == Role::Host ? 0u : 1u;

    for (;;) {
        const uint32_t target_frame = frame + delay;
        const bool waiting_for_peer_load = (frame > initial_frame);

        input::Pad sanitized = waiting_for_peer_load ? input::Pad{} : local_pad;
        sanitized.connected = true;
        const uint32_t hash_to_send = waiting_for_peer_load ? 0u : local_state_hash;

        auto& local_target = local_ring_[target_frame % kRingSize];
        local_target.frame = target_frame;
        local_target.pad = sanitized;
        local_target.state_hash = hash_to_send;
        local_target.in_gameplay = in_gameplay;
        local_target.has_pad = true;
        local_target.has_hash = hash_to_send != 0;

        input::Pad prev_pad{};
        prev_pad.connected = true;
        bool prev_gameplay = false;
        if (target_frame > 0) {
            const auto& prev_slot = local_ring_[(target_frame - 1) % kRingSize];
            if (prev_slot.has_pad && prev_slot.frame == target_frame - 1) {
                prev_pad = prev_slot.pad;
                prev_gameplay = prev_slot.in_gameplay;
            }
        }

        Packet out{};
        out.type = uint8_t(PacketType::FrameInput);
        out.player_index = base_player | (in_gameplay ? 0x80u : 0u) | (prev_gameplay ? 0x40u : 0u);
        out.input_delay = uint8_t(delay);
        out.frame = target_frame;
        out.ack_frame = has_remote_frame_ ? highest_remote_frame_ : 0;
        out.rng_seed = agreed_seed_.load(std::memory_order_relaxed);
        out.state_hash = hash_to_send;
        out.pad = PackPad(sanitized);
        out.prev_pad = PackPad(prev_pad);

        SendPacket(out);
        PumpIncoming();

        const auto wait_start = std::chrono::steady_clock::now();
        auto last_resend = wait_start;
        const auto timeout = std::chrono::milliseconds(frame_timeout_ms_);

        while (active_.load(std::memory_order_relaxed)) {
            const auto& remote_slot = remote_ring_[frame % kRingSize];
            if (remote_slot.has_pad && remote_slot.frame == frame) {
                break;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - wait_start >= timeout) {
                LOGE("netplay: timed out waiting for remote frame %u, disconnecting", frame);
                active_.store(false, std::memory_order_release);
                return false;
            }
            if (now - last_resend >= std::chrono::milliseconds(4)) {
                out.ack_frame = has_remote_frame_ ? highest_remote_frame_ : 0;
                SendPacket(out);
                last_resend = now;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            PumpIncoming();
        }

        if (!active_.load(std::memory_order_relaxed)) return false;

        const auto& local_now = local_ring_[frame % kRingSize];
        const auto& remote_now = remote_ring_[frame % kRingSize];
        if (local_now.in_gameplay && !remote_now.in_gameplay) {
            ++frame;
            continue;
        }
        break;
    }

    const auto& local_now = local_ring_[frame % kRingSize];
    const auto& remote_now = remote_ring_[frame % kRingSize];
    const bool same_phase = (local_now.in_gameplay == remote_now.in_gameplay);

    if (same_phase && local_now.has_hash && remote_now.has_hash && local_now.state_hash != remote_now.state_hash) {
        const uint32_t count = desync_count_.fetch_add(1, std::memory_order_relaxed) + 1;
        LOGW("netplay: state hash mismatch at frame %u (local=%08X remote=%08X count=%u)",
             frame, local_now.state_hash, remote_now.state_hash, count);
    }

    if (!same_phase) {
        input::Pad neutral{};
        neutral.connected = true;
        synced_pads_[0] = neutral;
        synced_pads_[1] = neutral;
    } else if (role_.load(std::memory_order_relaxed) == Role::Host) {
        synced_pads_[0] = local_now.pad;
        synced_pads_[1] = remote_now.pad;
    } else {
        synced_pads_[0] = remote_now.pad;
        synced_pads_[1] = local_now.pad;
    }
    synced_pads_[0].connected = true;
    synced_pads_[1].connected = true;

    sub_frame_ticks_.store(0, std::memory_order_release);
    rdtsc_sub_ticks_.store(0, std::memory_order_release);
    if (same_phase) {
        clock_frame_.fetch_add(1, std::memory_order_release);
    }
    current_frame_.store(frame + 1, std::memory_order_release);
    return true;
}

// Movies and loading screens wait for guest time to pass before they present again,
// and guest time otherwise advances only on presented frames. Without a completed
// lockstep frame for a while, advance the guest clocks once per real vertical blank.
bool Session::TickIdleClock() {
    if (!active_.load(std::memory_order_acquire) || in_advance_.load(std::memory_order_acquire)) return false;
    if (SteadyMs() - last_advance_ms_.load(std::memory_order_acquire) < int64_t(kIdleClockThresholdMs)) {
        if (idle_ticks_.exchange(0, std::memory_order_acq_rel)) {
            LOGI("netplay: lockstep resumed at frame %u", current_frame_.load(std::memory_order_acquire));
        }
        return false;
    }
    if (idle_ticks_.fetch_add(1, std::memory_order_acq_rel) == 0) {
        LOGI("netplay: no lockstep frame at frame %u, guest clocks follow vertical blank",
             current_frame_.load(std::memory_order_acquire));
    }
    clock_frame_.fetch_add(1, std::memory_order_release);
    return true;
}

uint64_t Session::GuestPerfCounter() {
    // 3,375,000 Hz / 60 FPS = 56,250 ticks per frame.
    // Per-thread sub-frame step ensures background threads never perturb the main simulation thread.
    const uint64_t frame = clock_frame_.load(std::memory_order_acquire);
    static thread_local const Session* tl_session = nullptr;
    static thread_local uint64_t tl_frame = UINT64_MAX;
    static thread_local uint64_t tl_sub = 0;
    if (tl_session != this || tl_frame != frame) {
        tl_session = this;
        tl_frame = frame;
        tl_sub = 0;
    }
    const uint64_t sub = std::min<uint64_t>(tl_sub, 56000ull);
    tl_sub += 10;
    return (uint64_t(agreed_seed_.load(std::memory_order_relaxed) & 0xFFFFu) * 56250ull) + frame * 56250ull + sub;
}

uint64_t Session::GuestRdtsc() {
    // 733,333,333 Hz / 60 FPS = 12,222,222 cycles per frame.
    // Per-thread sub-frame increment keeps intra-frame RDTSC deltas and RNG seeding identical on both peers.
    const uint64_t frame = clock_frame_.load(std::memory_order_acquire);
    static thread_local const Session* tl_session = nullptr;
    static thread_local uint64_t tl_frame = UINT64_MAX;
    static thread_local uint64_t tl_sub = 0;
    if (tl_session != this || tl_frame != frame) {
        tl_session = this;
        tl_frame = frame;
        tl_sub = 0;
    }
    const uint64_t sub = std::min<uint64_t>(tl_sub, 12000000ull);
    tl_sub += 733;
    const uint64_t seed_base = uint64_t(agreed_seed_.load(std::memory_order_relaxed)) * 12222222ull;
    return seed_base + (frame * 733333333ull) / 60ull + sub;
}

uint64_t Session::GuestInterruptTime100ns() const {
    // 10,000,000 (100ns units/sec) / 60 FPS = 166,666.666... units per frame.
    const uint64_t frame = clock_frame_.load(std::memory_order_acquire);
    return (frame * 500000ull) / 3ull;
}

uint64_t Session::GuestSystemTime100ns() const {
    // Fixed base epoch (2026-01-01 in Windows FILETIME 100ns units) + agreed_seed offset + frame time.
    constexpr uint64_t kBaseFileTime = 134117856000000000ull;
    const uint64_t seed_offset = uint64_t(agreed_seed_.load(std::memory_order_relaxed)) * 10000ull;
    return kBaseFileTime + seed_offset + GuestInterruptTime100ns();
}

uint32_t Session::GuestTickCountMs() const {
    const uint64_t frame = clock_frame_.load(std::memory_order_acquire);
    return uint32_t((frame * 50ull) / 3ull);
}

input::Pad Session::GetSyncedPad(int port) const {
    if (port < 0 || port >= 2) return input::Pad{};
    std::lock_guard guard(mutex_);
    if (!active_.load(std::memory_order_relaxed)) return input::Pad{};
    return synced_pads_[size_t(port)];
}

bool Start(const Config& config, std::string& error) {
    return GlobalSession().Start(config, error);
}

void Stop() {
    GlobalSession().Stop();
}

bool IsActive() {
    return GlobalSession().IsActive();
}

bool IsLoopbackSession() {
    return GlobalSession().IsActive() && GlobalSession().IsLoopback();
}

Role CurrentRole() {
    return GlobalSession().CurrentRole();
}

int LocalPlayerIndex() {
    return GlobalSession().LocalPlayerIndex();
}

uint32_t CurrentFrame() {
    return GlobalSession().CurrentFrame();
}

uint32_t AgreedSeed() {
    return GlobalSession().AgreedSeed();
}

uint32_t InputDelay() {
    return GlobalSession().InputDelay();
}

uint32_t DesyncCount() {
    return GlobalSession().DesyncCount();
}

uint64_t GuestPerfCounter() {
    return GlobalSession().GuestPerfCounter();
}

uint64_t GuestRdtsc() {
    return GlobalSession().GuestRdtsc();
}

uint64_t GuestInterruptTime100ns() {
    return GlobalSession().GuestInterruptTime100ns();
}

uint64_t GuestSystemTime100ns() {
    return GlobalSession().GuestSystemTime100ns();
}

uint32_t GuestTickCountMs() {
    return GlobalSession().GuestTickCountMs();
}

bool AdvanceFrame(const input::Pad& local_pad, uint32_t local_state_hash, bool in_gameplay) {
    return GlobalSession().AdvanceFrame(local_pad, local_state_hash, in_gameplay);
}

input::Pad GetSyncedPad(int port) {
    return GlobalSession().GetSyncedPad(port);
}

bool TickIdleClock() {
    return GlobalSession().TickIdleClock();
}

} // namespace openshao::netplay

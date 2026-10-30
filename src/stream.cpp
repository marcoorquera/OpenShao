#include "stream.h"
#include "stream_ice.h"
#include "stream_invite.h"
#include "stream_upnp.h"
#include "stream_video.h"
#include "stream_view.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>
#include <objbase.h>
#include <timeapi.h>
#include <wrl/client.h>
#include <xaudio2.h>
#include <SDL3/SDL.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#endif

#include "core/log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

namespace openshao::stream {

#ifndef _WIN32

bool StartHost(uint16_t, std::string& error) {
    error = "LAN streaming is available on Windows";
    return false;
}
void StopHost() {}
bool HostActive() { return false; }
bool ClientConnected() { return false; }
std::string PublicEndpoint() { return {}; }
bool RemotePadState(RemotePad&) { return false; }
bool WantsVideo() { return false; }
void SubmitVideo(const uint8_t*, uint32_t, uint32_t, uint32_t, float) {}
void SubmitAudio(const float*, uint32_t, uint32_t) {}
int RunClient(const std::string&, uint16_t) {
    LOGE("stream: LAN streaming is available on Windows");
    return 2;
}
void CreateInvite() {}
bool AcceptReply(const std::string&, std::string& error) {
    error = "Internet play is available on Windows";
    return false;
}
bool JoinInvite(const std::string&, std::string& error) {
    error = "Internet play is available on Windows";
    return false;
}
OnlineStatus Online() {
    OnlineStatus status;
    status.step = OnlineStep::Failed;
    status.error = "Internet play is available on Windows";
    return status;
}
void CancelOnline() {}

#else

namespace {

using Microsoft::WRL::ComPtr;

constexpr uint32_t kInputIntervalMs = 8;
constexpr uint32_t kHelloIntervalMs = 250;
constexpr uint32_t kPrefillAudio = 3;    // 30 ms of 10 ms packets
constexpr uint32_t kMaxQueuedAudio = 12; // 120 ms of 10 ms packets
constexpr uint32_t kAudioSlots = 64;
constexpr uint32_t kReceiveTimeoutMs = 20;
constexpr uint32_t kTimingLogMs = 5000;

uint32_t NowMs() {
    return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool StartSockets(std::string& error) {
    static const bool ok = [] {
        WSADATA wsa{};
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    if (!ok) error = "Winsock initialization failed";
    return ok;
}

SOCKET OpenSocket(uint16_t port, std::string& error) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        error = "Cannot create UDP socket";
        return s;
    }
    int buffer = 4 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buffer), sizeof(buffer));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buffer), sizeof(buffer));
    // An ICMP port-unreachable reply otherwise makes every later recvfrom fail with WSAECONNRESET.
    BOOL report = FALSE;
    DWORD returned = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &returned, nullptr, nullptr);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        error = "Cannot bind UDP port " + std::to_string(port);
        closesocket(s);
        return INVALID_SOCKET;
    }
    DWORD timeout = kReceiveTimeoutMs;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    return s;
}

// One datagram from the LAN socket or the internet link; 0 or less after the receive timeout. The
// internet link has a single peer, so `from` keeps the caller's value there.
int ReceivePacket(SOCKET sock, IceLink* ice, std::vector<uint8_t>& buffer, sockaddr_in& from) {
    if (ice) {
        const int n = ice->Receive(buffer.data(), buffer.size(), kReceiveTimeoutMs);
        if (n < 0) std::this_thread::sleep_for(std::chrono::milliseconds(kReceiveTimeoutMs));
        return n;
    }
    int from_len = sizeof(from);
    return recvfrom(sock, reinterpret_cast<char*>(buffer.data()), int(buffer.size()), 0,
                    reinterpret_cast<sockaddr*>(&from), &from_len);
}

bool SameAddress(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

std::string AddressText(const sockaddr_in& a) {
    char ip[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

struct ComScope {
    HRESULT hr;
    explicit ComScope(DWORD model) : hr(CoInitializeEx(nullptr, model)) {}
    ~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); }
};

// ---------------------------------------------------------------- internet session

// The launcher prepares the link and the session takes it over in the same process.
struct OnlineSession {
    std::mutex lock;
    std::unique_ptr<IceLink> link;
    bool host = false;
    bool accepted = false;  // host: a reply code started the connectivity checks
    uint32_t session = 0;
    std::string code, error;
};
OnlineSession g_online;

// The link once both codes are exchanged on this side, for the given role.
IceLink* PreparedLink(bool host) {
    std::lock_guard guard(g_online.lock);
    if (!g_online.link || g_online.host != host || (host && !g_online.accepted)) return nullptr;
    return g_online.link.get();
}

// ---------------------------------------------------------------- host

struct Host {
    SOCKET sock = INVALID_SOCKET;
    IceLink* ice = nullptr;  // internet play; set before the threads start
    std::atomic<bool> running{false};
    std::atomic<bool> connected{false};
    std::atomic<uint32_t> seq{0};
    std::atomic<bool> force_keyframe{true};
    std::thread net;
    std::thread encoder;

    std::mutex peer_lock;
    sockaddr_in peer{};
    uint32_t last_seen = 0;
    RemotePad pad{};

    std::mutex frame_lock;
    std::condition_variable frame_cv;
    std::vector<uint8_t> frame;
    uint32_t frame_w = 0, frame_h = 0;
    uint16_t frame_aspect = 0;
    bool frame_ready = false;
    uint32_t frame_id = 0;

    AudioEncoder audio;
    std::atomic<uint32_t> video_bitrate{kVideoBitrate};
    std::atomic<bool> bitrate_changed{false};
    BitrateController bitrate_ctrl{kVideoBitrate};
    uint32_t last_bitrate_log_ms = 0;
};
Host g_host;

void HostSend(const void* data, size_t size) {
    sockaddr_in to;
    {
        std::lock_guard guard(g_host.peer_lock);
        if (!g_host.connected.load(std::memory_order_acquire)) return;
        to = g_host.peer;
    }
    if (g_host.ice) g_host.ice->Send(data, size);
    else sendto(g_host.sock, static_cast<const char*>(data), int(size), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

void HostSend(const std::vector<uint8_t>& packet) {
    HostSend(packet.data(), packet.size());
}

std::string HostPeerText() {
    return g_host.ice ? "the internet, " + g_host.ice->Route() : AddressText(g_host.peer);
}

void HostDisconnect(const char* reason) {
    std::lock_guard guard(g_host.peer_lock);
    if (!g_host.connected.exchange(false)) return;
    g_host.pad = {};
    LOGI("stream: Player 2 %s (%s)", reason, HostPeerText().c_str());
}

void HostNetwork() {
    std::vector<uint8_t> buffer(2048);
    while (g_host.running.load(std::memory_order_acquire)) {
        sockaddr_in from{};
        const int n = ReceivePacket(g_host.sock, g_host.ice, buffer, from);
        const uint32_t now = NowMs();
        Header header;
        const uint8_t* payload = nullptr;
        if (n > 0 && ParsePacket(buffer.data(), size_t(n), header, payload)) {
            const auto type = PacketType(header.type);
            std::unique_lock guard(g_host.peer_lock);
            const bool from_peer = g_host.connected.load(std::memory_order_acquire) && (g_host.ice || SameAddress(from, g_host.peer));
            if (type == PacketType::Hello && (from_peer || !g_host.connected.load(std::memory_order_acquire))) {
                if (!from_peer) {
                    g_host.peer = from;
                    g_host.pad = {};
                    g_host.connected.store(true, std::memory_order_release);
                    g_host.force_keyframe.store(true, std::memory_order_release);
                    g_host.bitrate_ctrl = BitrateController(kVideoBitrate);
                    g_host.video_bitrate.store(kVideoBitrate, std::memory_order_release);
                    g_host.bitrate_changed.store(true, std::memory_order_release);
                    g_host.last_bitrate_log_ms = 0;
                    LOGI("stream: Player 2 connected from %s", HostPeerText().c_str());
                }
                g_host.last_seen = now;
                guard.unlock();
                HostSend(BuildPacket(PacketType::Welcome, g_host.seq++, nullptr, 0));
            } else if (type == PacketType::Input && from_peer && header.size == sizeof(RemotePad)) {
                std::memcpy(&g_host.pad, payload, sizeof(RemotePad));
                g_host.last_seen = now;
            } else if (type == PacketType::KeyframeRequest && from_peer) {
                g_host.force_keyframe.store(true, std::memory_order_release);
                g_host.last_seen = now;
            } else if (type == PacketType::Stats && from_peer && header.size == sizeof(StreamStats)) {
                StreamStats stats{};
                std::memcpy(&stats, payload, sizeof(stats));
                g_host.last_seen = now;
                const uint32_t old_rate = g_host.video_bitrate.load(std::memory_order_acquire);
                const uint32_t new_rate = g_host.bitrate_ctrl.Update(stats, now);
                if (new_rate != old_rate) {
                    g_host.video_bitrate.store(new_rate, std::memory_order_release);
                    g_host.bitrate_changed.store(true, std::memory_order_release);
                    if (g_host.last_bitrate_log_ms == 0 || now - g_host.last_bitrate_log_ms >= 5000) {
                        const double loss = (stats.packets_expected > 0 && stats.packets_received <= stats.packets_expected)
                            ? 100.0 * (1.0 - double(stats.packets_received) / double(stats.packets_expected))
                            : 0.0;
                        LOGI("stream: video bitrate %u kbps (loss %.1f%%)", new_rate / 1000, loss);
                        g_host.last_bitrate_log_ms = now;
                    }
                }
            } else if (type == PacketType::Ping && from_peer && header.size == sizeof(PingPayload)) {
                g_host.last_seen = now;
                guard.unlock();
                HostSend(BuildPacket(PacketType::Pong, g_host.seq++, payload, sizeof(PingPayload)));
            } else if (type == PacketType::Bye && from_peer) {
                guard.unlock();
                HostDisconnect("left");
            }
        }
        bool timed_out = false;
        {
            std::lock_guard guard(g_host.peer_lock);
            timed_out = g_host.connected.load(std::memory_order_acquire) && now - g_host.last_seen > kPeerTimeoutMs;
        }
        if (timed_out) HostDisconnect("timed out");
    }
}

void HostEncoder() {
    ComScope com(COINIT_MULTITHREADED);
    const bool media = StartMediaFoundation();
    VideoEncoder encoder;
    encoder.SetBitrate(g_host.video_bitrate.load(std::memory_order_acquire));
    std::vector<uint8_t> pixels, nv12, encoded, parity;
    while (g_host.running.load(std::memory_order_acquire)) {
        if (g_host.bitrate_changed.exchange(false, std::memory_order_acq_rel)) {
            encoder.SetBitrate(g_host.video_bitrate.load(std::memory_order_acquire));
        }
        uint32_t w = 0, h = 0;
        uint16_t aspect = 0;
        {
            std::unique_lock guard(g_host.frame_lock);
            g_host.frame_cv.wait_for(guard, std::chrono::milliseconds(100), [] {
                return g_host.frame_ready || !g_host.running.load(std::memory_order_acquire);
            });
            if (!g_host.frame_ready) continue;
            pixels.swap(g_host.frame);
            w = g_host.frame_w;
            h = g_host.frame_h;
            aspect = g_host.frame_aspect;
            g_host.frame_ready = false;
        }
        uint32_t ew = 0, eh = 0;
        BgraToNv12(pixels.data(), w * 4, w, h, VideoScale(w, h), nv12, ew, eh);
        const bool force = g_host.force_keyframe.exchange(false, std::memory_order_acq_rel);
        bool key = false;
        if (!media || !ew || !eh || !encoder.Encode(nv12.data(), ew, eh, force, encoded, key)) {
            if (force) g_host.force_keyframe.store(true, std::memory_order_release);
            continue;
        }
        if (encoded.empty()) continue;
        VideoChunk chunk{};
        chunk.frame = ++g_host.frame_id;
        chunk.width = uint16_t(ew);
        chunk.height = uint16_t(eh);
        chunk.total = uint32_t(encoded.size());
        chunk.count = ChunkCount(encoded.size());
        chunk.flags = key ? kVideoFlagKeyframe : 0;
        chunk.aspect = aspect;
        // fec_count is 8 bits; a frame too large for it goes without parity.
        const uint32_t groups = (uint32_t(chunk.count) + kFecGroup - 1) / kFecGroup;
        chunk.fec_group = groups <= 255 ? kFecGroup : 0;
        chunk.fec_count = groups <= 255 ? uint8_t(groups) : 0;
        if (chunk.fec_group) BuildParity(encoded.data(), encoded.size(), chunk.count, kFecGroup, parity);

        const uint16_t total_chunks = chunk.count + chunk.fec_count;
        const size_t total_bytes = encoded.size() + size_t(chunk.fec_count) * kMaxChunk;
        const uint32_t current_rate = g_host.video_bitrate.load(std::memory_order_acquire);
        const uint64_t rate_2x = uint64_t(current_rate) * 2;
        const uint64_t target_duration_us = (rate_2x > 0 && total_chunks > 1)
            ? std::min<uint64_t>(uint64_t(total_bytes) * 8 * 1'000'000 / rate_2x, 12'000)
            : 0;

        const auto start_time = std::chrono::steady_clock::now();
        for (uint16_t idx = 0; idx < total_chunks; ++idx) {
            if (idx > 0 && target_duration_us > 0) {
                const auto target_time = start_time + std::chrono::microseconds(uint64_t(idx) * target_duration_us / (total_chunks - 1));
                const auto now = std::chrono::steady_clock::now();
                if (target_time > now) {
                    const auto rem_us = std::chrono::duration_cast<std::chrono::microseconds>(target_time - now).count();
                    const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(now - start_time).count();
                    if (elapsed_us < 12'000) {
                        std::this_thread::sleep_for(std::chrono::microseconds(rem_us));
                    }
                }
            }
            if (idx < chunk.count) {
                chunk.index = idx;
                const size_t offset = size_t(idx) * kMaxChunk;
                const size_t size = std::min(kMaxChunk, encoded.size() - offset);
                HostSend(BuildPacket(PacketType::Video, g_host.seq++, &chunk, sizeof(chunk), encoded.data() + offset, size));
            } else {
                const uint16_t g = idx - chunk.count;
                chunk.index = idx;
                const size_t offset = size_t(g) * kMaxChunk;
                HostSend(BuildPacket(PacketType::Video, g_host.seq++, &chunk, sizeof(chunk), parity.data() + offset, kMaxChunk));
            }
        }
    }
}

// ---------------------------------------------------------------- client

struct Client {
    SOCKET sock = INVALID_SOCKET;
    IceLink* ice = nullptr;  // internet play; set before the network thread starts
    sockaddr_in host{};
    HWND window = nullptr;
    std::atomic<bool> running{true};
    std::atomic<bool> connected{false};
    std::atomic<uint32_t> last_packet{0};

    // The network thread publishes decoded frames here; Paint takes them over so drawing never
    // holds the lock that the network thread needs.
    std::mutex frame_lock;
    std::vector<uint8_t> frame;
    uint32_t frame_w = 0, frame_h = 0;
    float frame_aspect = 4.f / 3.f;
    bool frame_new = false;
    std::vector<uint8_t> paint;  // interface thread only
    uint32_t paint_w = 0, paint_h = 0;
    float paint_aspect = 4.f / 3.f;
    // Drawing time in microseconds since the last timing report.
    std::atomic<uint64_t> paint_us{0};
    std::atomic<uint32_t> paint_max_us{0};
    std::atomic<uint32_t> paints{0};
    std::atomic<uint32_t> seq{0};

    // With a GPU viewer, frames stay NV12 and a render thread presents them; without it, the network
    // thread converts to BGRA and the window paints with GDI.
    std::unique_ptr<ViewRenderer> view;
    std::condition_variable render_cv;
    bool redraw = false;  // guarded by frame_lock

    IXAudio2* xaudio = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    IXAudio2SourceVoice* voice = nullptr;
    uint32_t audio_rate = 0;
    bool audio_playing = false;
    AudioDecoder audio;
    std::vector<int16_t> audio_slots;
    uint32_t audio_next = 0;

    bool fullscreen = false;
    int fullscreen_retries = 0;
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};

    // Smoothed round trip in ms, or -1 before the first reply of this connection.
    std::atomic<int32_t> ping_ms{-1};
    HFONT ping_font = nullptr;
    int ping_font_height = 0;
};
Client* g_client = nullptr;

void ClientSend(Client& c, PacketType type, uint32_t seq, const void* payload = nullptr, size_t size = 0) {
    const auto packet = BuildPacket(type, seq, payload, size);
    if (c.ice) c.ice->Send(packet.data(), packet.size());
    else sendto(c.sock, reinterpret_cast<const char*>(packet.data()), int(packet.size()), 0,
                reinterpret_cast<const sockaddr*>(&c.host), sizeof(c.host));
}

std::string ClientHostText(const Client& c) {
    return c.ice ? "the internet, " + c.ice->Route() : AddressText(c.host);
}

void ClientAudio(Client& c, const AudioChunk& chunk, const uint8_t* samples, size_t size) {
    if (chunk.codec != kAudioCodecOpus || chunk.rate != kAudioRate || chunk.frames != kAudioPacketFrames || !size)
        return;
    if (!c.voice || c.audio_rate != chunk.rate) {
        if (c.voice) {
            c.voice->DestroyVoice();
            c.voice = nullptr;
        }
        c.audio.Reset();
        c.audio_playing = false;
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = 2;
        wf.nSamplesPerSec = chunk.rate;
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = 4;
        wf.nAvgBytesPerSec = chunk.rate * 4;
        if (!c.xaudio || FAILED(c.xaudio->CreateSourceVoice(&c.voice, &wf))) {
            LOG_FIRST(1, "stream: audio output unavailable");
            c.voice = nullptr;
            return;
        }
        c.audio_rate = chunk.rate;
    }
    XAUDIO2_VOICE_STATE state{};
    c.voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    uint32_t queued = state.BuffersQueued;
    if (c.audio_playing && queued == 0) {
        c.voice->Stop();
        c.audio_playing = false;
    }
    c.audio.Decode(
        chunk, samples, size,
        [&]() -> int16_t* {
            return &c.audio_slots[size_t(c.audio_next % kAudioSlots) * kAudioPacketFrames * 2];
        },
        [&](const int16_t* slot) {
            ++c.audio_next;
            // Dropping a packet keeps the delay bounded when the host clock runs slightly faster.
            if (queued >= kMaxQueuedAudio) return;
            XAUDIO2_BUFFER buffer{};
            buffer.AudioBytes = kAudioPacketFrames * 2 * sizeof(int16_t);
            buffer.pAudioData = reinterpret_cast<const BYTE*>(slot);
            if (SUCCEEDED(c.voice->SubmitSourceBuffer(&buffer))) ++queued;
        });
    if (!c.audio_playing && queued >= kPrefillAudio) {
        c.voice->Start();
        c.audio_playing = true;
    }
}

void RequestRedraw(Client& c) {
    if (!c.view) {
        InvalidateRect(c.window, nullptr, FALSE);
        return;
    }
    {
        std::lock_guard guard(c.frame_lock);
        c.redraw = true;
    }
    c.render_cv.notify_one();
}

void ClientRender(Client& c) {
    std::vector<uint8_t> nv12;
    uint32_t w = 0, h = 0;
    float aspect = 4.f / 3.f;
    while (c.running.load(std::memory_order_acquire)) {
        bool fresh = false;
        {
            std::unique_lock guard(c.frame_lock);
            // The timeout also follows window size changes while the picture is still.
            c.render_cv.wait_for(guard, std::chrono::milliseconds(250), [&] {
                return c.frame_new || c.redraw || !c.running.load(std::memory_order_acquire);
            });
            if (!c.running.load(std::memory_order_acquire)) break;
            if (c.frame_new) {
                nv12.swap(c.frame);
                w = c.frame_w;
                h = c.frame_h;
                aspect = c.frame_aspect;
                c.frame_new = false;
                fresh = true;
            }
            c.redraw = false;
        }
        const auto start = std::chrono::steady_clock::now();
        const int ping = c.connected.load(std::memory_order_acquire) ? int(c.ping_ms.load(std::memory_order_acquire)) : -1;
        if (!c.view->Draw(fresh ? nv12.data() : nullptr, w, h, aspect, ping)) LOG_FIRST(1, "stream: the viewer could not draw a frame");
        if (!fresh) continue;
        const uint32_t us = uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count());
        c.paint_us.fetch_add(us, std::memory_order_relaxed);
        c.paints.fetch_add(1, std::memory_order_relaxed);
        if (us > c.paint_max_us.load(std::memory_order_relaxed)) c.paint_max_us.store(us, std::memory_order_relaxed);
    }
}

void ClientNetwork(Client& c) {
    ComScope com(COINIT_MULTITHREADED);
    const bool media = StartMediaFoundation();
    VideoDecoder decoder;
    FrameAssembler assembler;
    std::vector<uint8_t> buffer(2048), encoded, pixels, bgra;
    // Until a keyframe arrives, predicted frames would only show corrupted pictures.
    bool waiting_keyframe = true;
    bool has_frame = false;
    uint32_t last_frame = 0, last_request = 0;
    uint32_t last_stats = NowMs();
    uint32_t last_ping = 0;
    PingFilter ping;
    // Logged every kTimingLogMs so slow decoding or drawing on a device shows up in the log.
    struct Timing {
        uint32_t start = 0, inputs = 0, outputs = 0, decode_max_us = 0;
        uint64_t decode_us = 0;
    } timing;
    timing.start = NowMs();
    StreamStats stats{};
    SequenceCounter sequence;
    uint32_t last_assembler_dropped = 0;
    uint32_t last_assembler_recovered = 0;
    uint32_t last_seen_frame_id = 0;
    bool has_seen_frame_id = false;
    auto request_keyframe = [&] {
        const uint32_t now = NowMs();
        if (c.connected.load(std::memory_order_acquire) && now - last_request >= kKeyframeRequestMs) {
            ClientSend(c, PacketType::KeyframeRequest, 0);
            last_request = now;
        }
    };
    while (c.running.load(std::memory_order_acquire)) {
        sockaddr_in from = c.host;
        const int n = ReceivePacket(c.sock, c.ice, buffer, from);
        Header header;
        const uint8_t* payload = nullptr;
        const uint32_t now = NowMs();
        if (waiting_keyframe) request_keyframe();
        if (c.connected.load(std::memory_order_acquire) && now - last_stats >= kStatsIntervalMs) {
            ClientSend(c, PacketType::Stats, 0, &stats, sizeof(stats));
            stats = StreamStats{};
            last_stats = now;
        }
        if (now - timing.start >= kTimingLogMs) {
            const uint32_t paints = c.paints.exchange(0);
            const uint64_t paint_us = c.paint_us.exchange(0);
            const uint32_t paint_max = c.paint_max_us.exchange(0);
            if (timing.inputs)
                LOGI("stream: %u frames in, %u out; decode avg %.1f ms, max %.1f ms; %u draws, avg %.1f ms, max %.1f ms; ping %d ms",
                     timing.inputs, timing.outputs, double(timing.decode_us) / timing.inputs / 1000.0,
                     timing.decode_max_us / 1000.0, paints, paints ? double(paint_us) / paints / 1000.0 : 0.0,
                     paint_max / 1000.0, int(c.ping_ms.load(std::memory_order_acquire)));
            timing = Timing{};
            timing.start = now;
        }
        if (c.connected.load(std::memory_order_acquire) && now - last_ping >= kPingIntervalMs) {
            const PingPayload sent{now};
            ClientSend(c, PacketType::Ping, 0, &sent, sizeof(sent));
            last_ping = now;
        }
        if (n <= 0 || !SameAddress(from, c.host) || !ParsePacket(buffer.data(), size_t(n), header, payload)) continue;
        c.last_packet.store(NowMs(), std::memory_order_release);
        if (c.connected.load(std::memory_order_acquire)) sequence.Add(header.seq, stats);
        const auto type = PacketType(header.type);
        if (type == PacketType::Welcome) {
            if (!c.connected.exchange(true)) {
                c.audio.Reset();
                waiting_keyframe = true;
                has_frame = false;
                last_request = NowMs() - kKeyframeRequestMs;
                last_stats = NowMs();
                stats = StreamStats{};
                sequence.Reset();
                ping.Reset();
                c.ping_ms.store(-1, std::memory_order_release);
                last_ping = NowMs() - kPingIntervalMs;
                last_assembler_dropped = assembler.Dropped();
                last_assembler_recovered = assembler.Recovered();
                has_seen_frame_id = false;
                LOGI("stream: connected to host %s", ClientHostText(c).c_str());
                PostMessageW(c.window, WM_APP + 1, 0, 0);
            }
        } else if (type == PacketType::Video && header.size > sizeof(VideoChunk)) {
            VideoChunk chunk;
            std::memcpy(&chunk, payload, sizeof(chunk));
            if (chunk.codec != kVideoCodecH264 || !media) continue;

            if (!has_seen_frame_id || int32_t(chunk.frame - last_seen_frame_id) > 0) {
                if (has_seen_frame_id) {
                    const int32_t diff = int32_t(chunk.frame - last_seen_frame_id);
                    if (diff > 1) stats.frames_lost += uint32_t(diff - 1);
                }
                last_seen_frame_id = chunk.frame;
                has_seen_frame_id = true;
            }

            // Ignore parity chunks of frames already complete.
            if (chunk.index >= chunk.count && assembler.IsFrameComplete(chunk.frame)) {
                continue;
            }

            const bool complete = assembler.Add(chunk, payload + sizeof(chunk), header.size - sizeof(chunk), encoded);
            const uint32_t cur_dropped = assembler.Dropped();
            if (cur_dropped > last_assembler_dropped) {
                stats.frames_lost += (cur_dropped - last_assembler_dropped);
                last_assembler_dropped = cur_dropped;
            }
            const uint32_t cur_recovered = assembler.Recovered();
            if (cur_recovered > last_assembler_recovered) {
                stats.frames_recovered += (cur_recovered - last_assembler_recovered);
                last_assembler_recovered = cur_recovered;
            }
            if (!complete) continue;
            const bool key = (chunk.flags & kVideoFlagKeyframe) != 0;
            // A gap in frame ids means a lost frame that later frames reference.
            if (has_frame && chunk.frame != last_frame + 1) waiting_keyframe = true;
            has_frame = true;
            last_frame = chunk.frame;
            if (waiting_keyframe && !key) {
                request_keyframe();
                continue;
            }
            if (key) waiting_keyframe = false;
            bool failed = false;
            const auto decode_start = std::chrono::steady_clock::now();
            const bool decoded = decoder.Decode(encoded.data(), encoded.size(), chunk.width, chunk.height, pixels, failed);
            const uint32_t decode_us = uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - decode_start).count());
            ++timing.inputs;
            timing.decode_us += decode_us;
            timing.decode_max_us = std::max(timing.decode_max_us, decode_us);
            if (decoded) ++timing.outputs;
            if (!decoded) {
                if (failed) {
                    decoder.Reset();
                    waiting_keyframe = true;
                    request_keyframe();
                }
                continue;
            }
            if (!c.view) {
                Nv12ToBgra(pixels.data(), pixels.data() + size_t(chunk.width) * chunk.height, chunk.width,
                           chunk.width, chunk.height, bgra);
                pixels.swap(bgra);
            }
            {
                std::lock_guard guard(c.frame_lock);
                c.frame.swap(pixels);
                c.frame_w = chunk.width;
                c.frame_h = chunk.height;
                c.frame_aspect = chunk.aspect ? float(chunk.aspect) / 1000.f : 4.f / 3.f;
                c.frame_new = true;
            }
            RequestRedraw(c);
        } else if (type == PacketType::Pong && header.size == sizeof(PingPayload)) {
            PingPayload reply;
            std::memcpy(&reply, payload, sizeof(reply));
            const uint32_t rtt = now - reply.sent_ms;
            // A reply older than the timeout belongs to an earlier connection.
            if (rtt <= kPeerTimeoutMs) {
                c.ping_ms.store(int32_t(ping.Add(rtt)), std::memory_order_release);
                // Keeps the value current while the picture is still, when no frame triggers a repaint.
                RequestRedraw(c);
            }
        } else if (type == PacketType::Audio && header.size > sizeof(AudioChunk)) {
            AudioChunk chunk;
            std::memcpy(&chunk, payload, sizeof(chunk));
            ClientAudio(c, chunk, payload + sizeof(chunk), header.size - sizeof(chunk));
        } else if (type == PacketType::Bye) {
            c.audio.Reset();
            waiting_keyframe = true;
            has_frame = false;
            has_seen_frame_id = false;
            stats = StreamStats{};
            sequence.Reset();
            ping.Reset();
            c.ping_ms.store(-1, std::memory_order_release);
            if (c.connected.exchange(false)) LOGI("stream: host closed the session");
            PostMessageW(c.window, WM_APP + 1, 0, 0);
        }
    }
}

void ClientTitle(Client& c) {
    const wchar_t* title = c.connected.load(std::memory_order_acquire)
        ? L"OpenShao - Player 2 (F11 or double-click: full screen)"
        : L"OpenShao - Player 2 (connecting to host...)";
    SetWindowTextW(c.window, title);
}

constexpr UINT_PTR kFullscreenTimer = 1;
constexpr int kFullscreenRetries = 8;

// Covers the monitor. Returns true when the window already does.
bool CoverMonitor(Client& c) {
    MONITORINFO mi{sizeof(mi)};
    RECT now{};
    if (!GetMonitorInfoW(MonitorFromWindow(c.window, MONITOR_DEFAULTTOPRIMARY), &mi) || !GetWindowRect(c.window, &now))
        return true;
    if (EqualRect(&now, &mi.rcMonitor)) return true;
    SetWindowPos(c.window, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    return false;
}

void ToggleFullscreen(Client& c) {
    const LONG style = GetWindowLongW(c.window, GWL_STYLE);
    if (!c.fullscreen) {
        if (!GetWindowPlacement(c.window, &c.placement)) return;
        // A borderless popup that covers the monitor is what Wine and window managers treat as full screen.
        SetWindowLongW(c.window, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        CoverMonitor(c);
        // Under Wine, a desktop window manager can first fit the window above its panel; Wine then
        // drops the full-screen request because the window no longer covers the monitor. Asking again
        // once the window manager settled sends the request with the full monitor size.
        c.fullscreen_retries = kFullscreenRetries;
        SetTimer(c.window, kFullscreenTimer, 150, nullptr);
    } else {
        KillTimer(c.window, kFullscreenTimer);
        SetWindowLongW(c.window, GWL_STYLE, (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(c.window, &c.placement);
        SetWindowPos(c.window, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    c.fullscreen = !c.fullscreen;
}

// Draws the round trip in the top-right corner, like the ping counter of online games.
void PaintPing(Client& c, HDC dc, int cw, int ch) {
    const int32_t ms = c.ping_ms.load(std::memory_order_acquire);
    if (ms < 0 || !c.connected.load(std::memory_order_acquire)) return;
    const int height = std::max(14, ch / 30);
    if (!c.ping_font || c.ping_font_height != height) {
        if (c.ping_font) DeleteObject(c.ping_font);
        c.ping_font = CreateFontW(-height, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        c.ping_font_height = height;
    }
    wchar_t text[32];
    std::swprintf(text, 32, L"%d ms", int(ms));
    HGDIOBJ old_font = SelectObject(dc, c.ping_font);
    SIZE size{};
    GetTextExtentPoint32W(dc, text, int(std::wcslen(text)), &size);
    const int pad = height / 3, margin = height / 2;
    RECT box{cw - margin - size.cx - pad * 2, margin, cw - margin, margin + size.cy + pad};
    if (box.left < 0) box.left = 0;
    HBRUSH back = CreateSolidBrush(RGB(16, 16, 16));
    FillRect(dc, &box, back);
    DeleteObject(back);
    // Green, yellow and red follow the usual thresholds of online games.
    SetTextColor(dc, ms < 60 ? RGB(110, 220, 120) : ms < 120 ? RGB(240, 200, 80) : RGB(240, 90, 80));
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text, -1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old_font);
}

void Paint(Client& c, HDC dc) {
    const auto start = std::chrono::steady_clock::now();
    RECT rc;
    GetClientRect(c.window, &rc);
    const int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    {
        std::lock_guard guard(c.frame_lock);
        if (c.frame_new) {
            c.paint.swap(c.frame);
            c.paint_w = c.frame_w;
            c.paint_h = c.frame_h;
            c.paint_aspect = c.frame_aspect;
            c.frame_new = false;
        }
    }
    if (c.paint.empty() || !c.paint_w || !c.paint_h) {
        FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        PaintPing(c, dc, cw, ch);
        return;
    }
    // Letterbox the picture in the shape the host shows it, which follows its aspect ratio setting.
    int w = cw, h = int(float(cw) / c.paint_aspect + .5f);
    if (h > ch) { h = ch; w = int(float(ch) * c.paint_aspect + .5f); }
    const int x = (cw - w) / 2, y = (ch - h) / 2;
    HBRUSH black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RECT bars[4] = {{0, 0, cw, y}, {0, y + h, cw, ch}, {0, y, x, y + h}, {x + w, y, cw, y + h}};
    for (auto& bar : bars) if (bar.right > bar.left && bar.bottom > bar.top) FillRect(dc, &bar, black);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = LONG(c.paint_w);
    bi.bmiHeader.biHeight = -LONG(c.paint_h);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    StretchDIBits(dc, x, y, w, h, 0, 0, int(c.paint_w), int(c.paint_h), c.paint.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
    PaintPing(c, dc, cw, ch);
    const uint32_t us = uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count());
    c.paint_us.fetch_add(us, std::memory_order_relaxed);
    c.paints.fetch_add(1, std::memory_order_relaxed);
    if (us > c.paint_max_us.load(std::memory_order_relaxed)) c.paint_max_us.store(us, std::memory_order_relaxed);
}

LRESULT CALLBACK ClientProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Client* c = g_client;
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (c && !c->view) Paint(*c, dc);
        EndPaint(hwnd, &ps);
        if (c && c->view) RequestRedraw(*c);
        return 0;
    }
    case WM_SIZE:
        if (c && c->view) RequestRedraw(*c);
        break;
    case WM_APP + 1:
        if (c) {
            ClientTitle(*c);
            RequestRedraw(*c);
        }
        return 0;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) {
            if (c) ToggleFullscreen(*c);
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (wp == VK_F11 && !(lp & (1 << 30))) {
            if (c) ToggleFullscreen(*c);
            return 0;
        }
        break;
    case WM_TIMER:
        if (wp == kFullscreenTimer) {
            if (!c || !c->fullscreen || CoverMonitor(*c) || --c->fullscreen_retries <= 0) KillTimer(hwnd, kFullscreenTimer);
            return 0;
        }
        break;
    case WM_LBUTTONDBLCLK:
        // A touch screen reports a double tap as a double click.
        if (c) ToggleFullscreen(*c);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Mirrors the default OpenShao keyboard layout so both players learn one set of keys.
void ReadKeyboard(RemotePad& pad) {
    auto button = [&](int vk, SDL_GamepadButton b) { if (KeyDown(vk)) pad.buttons |= 1u << b; };
    button(VK_SPACE, SDL_GAMEPAD_BUTTON_SOUTH);
    button(VK_ESCAPE, SDL_GAMEPAD_BUTTON_EAST);
    button(VK_CONTROL, SDL_GAMEPAD_BUTTON_EAST);
    button('J', SDL_GAMEPAD_BUTTON_WEST);
    button('K', SDL_GAMEPAD_BUTTON_NORTH);
    button('C', SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    button('Q', SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    button(VK_RETURN, SDL_GAMEPAD_BUTTON_START);
    button(VK_BACK, SDL_GAMEPAD_BUTTON_BACK);
    button(VK_UP, SDL_GAMEPAD_BUTTON_DPAD_UP);
    button(VK_DOWN, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    button(VK_LEFT, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    button(VK_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    if (KeyDown(VK_SHIFT)) pad.axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = 32767;
    if (KeyDown('E')) pad.axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = 32767;
    const int x = (KeyDown('D') ? 1 : 0) - (KeyDown('A') ? 1 : 0);
    const int y = (KeyDown('S') ? 1 : 0) - (KeyDown('W') ? 1 : 0);
    if (x) pad.axes[SDL_GAMEPAD_AXIS_LEFTX] = int16_t(x > 0 ? 32767 : -32768);
    if (y) pad.axes[SDL_GAMEPAD_AXIS_LEFTY] = int16_t(y > 0 ? 32767 : -32768);
}

void ReadGamepad(SDL_Gamepad*& gamepad, RemotePad& pad) {
    if (gamepad && !SDL_GamepadConnected(gamepad)) {
        SDL_CloseGamepad(gamepad);
        gamepad = nullptr;
    }
    if (!gamepad) {
        int count = 0;
        if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
            if (count > 0) {
                gamepad = SDL_OpenGamepad(ids[0]);
                if (gamepad) LOGI("stream: Player 2 gamepad %s", SDL_GetGamepadName(gamepad));
            }
            SDL_free(ids);
        }
    }
    if (!gamepad) return;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && b < 32; ++b)
        if (SDL_GetGamepadButton(gamepad, SDL_GamepadButton(b))) pad.buttons |= 1u << b;
    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT && a < int(pad.axes.size()); ++a) {
        const int16_t value = SDL_GetGamepadAxis(gamepad, SDL_GamepadAxis(a));
        if (std::abs(int(value)) > std::abs(int(pad.axes[size_t(a)]))) pad.axes[size_t(a)] = value;
    }
}

} // namespace

bool StartHost(uint16_t port, std::string& error) {
    StopHost();
    g_host.ice = PreparedLink(true);
    if (!g_host.ice) {
        if (!StartSockets(error)) return false;
        g_host.sock = OpenSocket(port, error);
        if (g_host.sock == INVALID_SOCKET) return false;
    }
    g_host.connected.store(false);
    g_host.running.store(true);
    g_host.net = std::thread(HostNetwork);
    g_host.encoder = std::thread(HostEncoder);
    if (g_host.ice) {
        LOGI("stream: hosting over the internet, waiting for Player 2");
    } else {
        upnp::Start(port);
        LOGI("stream: hosting on UDP port %u, waiting for Player 2", unsigned(port));
    }
    return true;
}

void StopHost() {
    upnp::Stop();
    if (!g_host.running.exchange(false)) return;
    if (g_host.connected.load()) {
        const auto bye = BuildPacket(PacketType::Bye, g_host.seq++, nullptr, 0);
        for (int i = 0; i < 3; ++i) HostSend(bye);
    }
    g_host.frame_cv.notify_all();
    if (g_host.net.joinable()) g_host.net.join();
    if (g_host.encoder.joinable()) g_host.encoder.join();
    if (g_host.sock != INVALID_SOCKET) closesocket(g_host.sock);
    g_host.sock = INVALID_SOCKET;
    g_host.connected.store(false);
    if (g_host.ice) {
        g_host.ice = nullptr;
        CancelOnline();
    }
}

void CreateInvite() {
    CancelOnline();
    auto link = std::make_unique<IceLink>();
    std::string error;
    const bool started = link->Start(error);
    std::random_device device;
    std::lock_guard guard(g_online.lock);
    g_online.host = true;
    g_online.session = device() | 1u;
    if (started) g_online.link = std::move(link);
    else g_online.error = error;
}

bool AcceptReply(const std::string& reply, std::string& error) {
    std::lock_guard guard(g_online.lock);
    if (!g_online.link || !g_online.host || g_online.accepted || g_online.link->State() != IceState::Ready) {
        error = "Create an invite first and wait until its code appears.";
        return false;
    }
    ConnectionCode code;
    if (!DecodeCode(reply, CodeKind::Reply, code, error)) return false;
    if (code.session != g_online.session) {
        error = "This reply answers a different invite. Ask your friend to paste your latest invite code.";
        return false;
    }
    if (!g_online.link->SetRemote(code.description, error)) return false;
    g_online.accepted = true;
    return true;
}

bool JoinInvite(const std::string& invite, std::string& error) {
    ConnectionCode code;
    if (!DecodeCode(invite, CodeKind::Invite, code, error)) return false;
    CancelOnline();
    auto link = std::make_unique<IceLink>();
    if (!link->Start(error) || !link->SetRemote(code.description, error)) return false;
    std::lock_guard guard(g_online.lock);
    g_online.host = false;
    g_online.session = code.session;
    g_online.link = std::move(link);
    return true;
}

OnlineStatus Online() {
    std::lock_guard guard(g_online.lock);
    OnlineStatus status;
    status.host = g_online.host;
    if (!g_online.link) {
        status.step = g_online.error.empty() ? OnlineStep::Idle : OnlineStep::Failed;
        status.error = g_online.error;
        return status;
    }
    const IceState state = g_online.link->State();
    // The code is made once: the link keeps its port and credentials for the whole session.
    if (g_online.code.empty() && state >= IceState::Ready && state != IceState::Failed) {
        const std::string local = g_online.link->LocalDescription();
        if (!local.empty())
            g_online.code = EncodeCode({g_online.host ? CodeKind::Invite : CodeKind::Reply, g_online.session, local});
    }
    status.code = g_online.code;
    switch (state) {
        case IceState::Idle:
        case IceState::Gathering: status.step = OnlineStep::Preparing; break;
        case IceState::Ready: status.step = g_online.code.empty() ? OnlineStep::Preparing : OnlineStep::Waiting; break;
        case IceState::Connecting: status.step = g_online.code.empty() ? OnlineStep::Preparing : OnlineStep::Connecting; break;
        case IceState::Connected: status.step = OnlineStep::Connected; break;
        case IceState::Failed:
            status.step = OnlineStep::Failed;
            status.error = g_online.host ? "The connection failed. Create a new invite." : "The connection failed. Ask the host for a new invite.";
            break;
    }
    return status;
}

void CancelOnline() {
    std::unique_ptr<IceLink> link;
    {
        std::lock_guard guard(g_online.lock);
        link = std::move(g_online.link);
        g_online.host = g_online.accepted = false;
        g_online.session = 0;
        g_online.code.clear();
        g_online.error.clear();
    }
    if (link) link->Stop();
}

bool HostActive() { return g_host.running.load(std::memory_order_acquire); }

bool ClientConnected() { return g_host.connected.load(std::memory_order_acquire); }

std::string PublicEndpoint() { return upnp::ExternalEndpoint(); }

bool RemotePadState(RemotePad& pad) {
    std::lock_guard guard(g_host.peer_lock);
    if (!g_host.connected.load(std::memory_order_acquire)) return false;
    pad = g_host.pad;
    return true;
}

bool WantsVideo() {
    if (!g_host.connected.load(std::memory_order_acquire)) return false;
    std::lock_guard guard(g_host.frame_lock);
    return !g_host.frame_ready;
}

void SubmitVideo(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t pitch, float aspect) {
    if (!g_host.connected.load(std::memory_order_acquire) || !width || !height || width > 4096 || height > 4096) return;
    {
        std::lock_guard guard(g_host.frame_lock);
        g_host.frame.resize(size_t(width) * height * 4);
        for (uint32_t y = 0; y < height; ++y)
            std::memcpy(&g_host.frame[size_t(y) * width * 4], bgra + size_t(y) * pitch, size_t(width) * 4);
        g_host.frame_w = width;
        g_host.frame_h = height;
        g_host.frame_aspect = aspect > 0.5f && aspect < 8.f ? uint16_t(aspect * 1000.f + .5f) : 0;
        g_host.frame_ready = true;
    }
    g_host.frame_cv.notify_one();
}

void SubmitAudio(const float* stereo, uint32_t frames, uint32_t rate) {
    if (!g_host.connected.load(std::memory_order_acquire) || !stereo || !frames) return;
    if (!g_host.audio.Submit(stereo, frames, rate, [](const AudioChunk& chunk, const uint8_t* data, size_t size) {
            std::array<uint8_t, sizeof(Header) + sizeof(AudioChunk) + kMaxChunk> packet{};
            Header header{};
            header.type = uint8_t(PacketType::Audio);
            header.size = uint16_t(sizeof(AudioChunk) + size);
            header.seq = g_host.seq++;
            std::memcpy(packet.data(), &header, sizeof(Header));
            std::memcpy(packet.data() + sizeof(Header), &chunk, sizeof(AudioChunk));
            std::memcpy(packet.data() + sizeof(Header) + sizeof(AudioChunk), data, size);
            HostSend(packet.data(), sizeof(Header) + sizeof(AudioChunk) + size);
        })) {
        if (rate != kAudioRate) LOG_FIRST(1, "stream: unsupported audio sample rate %u Hz", unsigned(rate));
        else LOG_FIRST(1, "stream: audio encoder initialization failed");
    }
}

void ClientInput(Client& c) {
    // The default 15.6 ms timer tick would otherwise stretch the input interval.
    timeBeginPeriod(1);
    SDL_Gamepad* gamepad = nullptr;
    uint32_t last_input = 0, last_hello = 0;
    while (c.running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const uint32_t now = NowMs();
        if (c.connected.load(std::memory_order_acquire) &&
            now - c.last_packet.load(std::memory_order_acquire) > kPeerTimeoutMs) {
            c.connected.store(false);
            LOGW("stream: lost the host, reconnecting");
            PostMessageW(c.window, WM_APP + 1, 0, 0);
        }
        if (!c.connected.load(std::memory_order_acquire) && now - last_hello >= kHelloIntervalMs) {
            ClientSend(c, PacketType::Hello, c.seq++);
            last_hello = now;
        }
        if (c.connected.load(std::memory_order_acquire) && now - last_input >= kInputIntervalMs) {
            SDL_UpdateGamepads();
            RemotePad pad{};
            ReadGamepad(gamepad, pad);
            if (GetForegroundWindow() == c.window) ReadKeyboard(pad);
            ClientSend(c, PacketType::Input, c.seq++, &pad, sizeof(pad));
            last_input = now;
        }
    }
    if (gamepad) SDL_CloseGamepad(gamepad);
    timeEndPeriod(1);
}

int RunClient(const std::string& host, uint16_t port) {
    std::string error;
    Client c;
    c.ice = PreparedLink(false);
    if (!c.ice) {
        if (!StartSockets(error)) {
            LOGE("stream: %s", error.c_str());
            return 5;
        }
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
            LOGE("stream: cannot resolve host address %s", host.c_str());
            return 5;
        }
        std::memcpy(&c.host, res->ai_addr, sizeof(c.host));
        freeaddrinfo(res);
        c.sock = OpenSocket(0, error);
        if (c.sock == INVALID_SOCKET) {
            LOGE("stream: %s", error.c_str());
            return 5;
        }
    }

    ComScope com(COINIT_APARTMENTTHREADED);
    if (FAILED(XAudio2Create(&c.xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR)) || FAILED(c.xaudio->CreateMasteringVoice(&c.master)))
        LOGE("stream: XAudio2 unavailable, no sound");
    c.audio_slots.resize(size_t(kAudioSlots) * kAudioPacketFrames * 2);

    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) LOGE("stream: SDL gamepad initialization failed: %s", SDL_GetError());

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = ClientProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)) /* IDC_ARROW */;
    wc.lpszClassName = L"OpenShaoStreamClient";
    RegisterClassExW(&wc);
    // 960x720 when it fits; smaller screens get the largest 4:3 picture whose frame fits the work area.
    RECT work{0, 0, 1280, 1024};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    RECT frame{0, 0, 0, 0};
    AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
    const int border_w = frame.right - frame.left, border_h = frame.bottom - frame.top;
    int view_w = 960, view_h = 720;
    const int fit_w = (work.right - work.left) * 9 / 10 - border_w, fit_h = (work.bottom - work.top) * 9 / 10 - border_h;
    if (fit_w > 0 && fit_h > 0 && (view_w > fit_w || view_h > fit_h)) {
        view_w = std::min(fit_w, fit_h * 4 / 3);
        view_h = view_w * 3 / 4;
    }
    RECT rc{0, 0, view_w, view_h};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_client = &c;
    c.window = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                               rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, inst, nullptr);
    if (!c.window) {
        LOGE("stream: cannot create the viewer window");
        return 4;
    }
    ClientTitle(c);
    ShowWindow(c.window, SW_SHOW);
    auto view = std::make_unique<ViewRenderer>();
    std::string view_error;
    if (view->Open(c.window, view_error)) {
        c.view = std::move(view);
        LOGI("stream: the viewer draws with Direct3D 11");
    } else {
        LOGW("stream: %s; the viewer draws with GDI", view_error.c_str());
    }
    if (c.ice) LOGI("stream: connecting to the host over the internet");
    else LOGI("stream: connecting to host %s", AddressText(c.host).c_str());

    std::thread net(ClientNetwork, std::ref(c));
    // Input has its own thread so that slow drawing in full screen never delays Player 2's buttons.
    std::thread input(ClientInput, std::ref(c));
    std::thread render;
    if (c.view) render = std::thread(ClientRender, std::ref(c));
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    c.running.store(false);
    input.join();
    for (int i = 0; i < 3; ++i) ClientSend(c, PacketType::Bye, c.seq++);
    net.join();
    c.render_cv.notify_all();
    if (render.joinable()) render.join();
    c.view.reset();
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    if (c.voice) c.voice->DestroyVoice();
    if (c.master) c.master->DestroyVoice();
    if (c.xaudio) c.xaudio->Release();
    if (c.sock != INVALID_SOCKET) closesocket(c.sock);
    if (c.ice) CancelOnline();
    g_client = nullptr;
    if (c.ping_font) DeleteObject(c.ping_font);
    return 0;
}

#endif

} // namespace openshao::stream

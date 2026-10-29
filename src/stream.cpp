#include "stream.h"
#include "stream_upnp.h"

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
#include <wincodec.h>
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
#include <mutex>
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
void SubmitVideo(const uint8_t*, uint32_t, uint32_t, uint32_t) {}
void SubmitAudio(const float*, uint32_t, uint32_t) {}
int RunClient(const std::string&, uint16_t) {
    LOGE("stream: LAN streaming is available on Windows");
    return 2;
}

#else

namespace {

using Microsoft::WRL::ComPtr;

constexpr float kJpegQuality = 0.85f;
constexpr uint32_t kInputIntervalMs = 8;
constexpr uint32_t kHelloIntervalMs = 250;
constexpr uint32_t kPrefillAudio = 3;    // 30 ms of 10 ms packets
constexpr uint32_t kMaxQueuedAudio = 12; // 120 ms of 10 ms packets
constexpr uint32_t kAudioSlots = 64;

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
    DWORD timeout = 20;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    return s;
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

ComPtr<IWICImagingFactory> CreateFactory() {
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        LOGE("stream: Windows Imaging Component is unavailable");
    return factory;
}

bool EncodeJpeg(IWICImagingFactory* factory, const uint8_t* bgra, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    ComPtr<IWICBitmap> bitmap;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, &props)))
        return false;
    PROPBAG2 option{};
    option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT value;
    VariantInit(&value);
    value.vt = VT_R4;
    value.fltVal = kJpegQuality;
    props->Write(1, &option, &value);
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->Initialize(props.Get())) || FAILED(frame->SetSize(w, h)) || FAILED(frame->SetPixelFormat(&format)) ||
        FAILED(factory->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGR, w * 4, w * h * 4,
                                               const_cast<BYTE*>(bgra), &bitmap)) ||
        FAILED(frame->WriteSource(bitmap.Get(), nullptr)) || FAILED(frame->Commit()) || FAILED(encoder->Commit()))
        return false;
    STATSTG stat{};
    HGLOBAL global = nullptr;
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || FAILED(GetHGlobalFromStream(stream.Get(), &global))) return false;
    const auto* data = static_cast<const uint8_t*>(GlobalLock(global));
    if (!data) return false;
    out.assign(data, data + size_t(stat.cbSize.QuadPart));
    GlobalUnlock(global);
    return true;
}

bool DecodeJpeg(IWICImagingFactory* factory, const std::vector<uint8_t>& jpeg, std::vector<uint8_t>& bgra,
                uint32_t& w, uint32_t& h) {
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(jpeg.data()), DWORD(jpeg.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)) ||
        FAILED(converter->GetSize(&w, &h)))
        return false;
    bgra.resize(size_t(w) * h * 4);
    return SUCCEEDED(converter->CopyPixels(nullptr, w * 4, UINT(bgra.size()), bgra.data()));
}

// ---------------------------------------------------------------- host

struct Host {
    SOCKET sock = INVALID_SOCKET;
    std::atomic<bool> running{false};
    std::atomic<bool> connected{false};
    std::atomic<uint32_t> seq{0};
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
    bool frame_ready = false;
    uint32_t frame_id = 0;

    AudioEncoder audio;
};
Host g_host;

void HostSend(const void* data, size_t size) {
    sockaddr_in to;
    {
        std::lock_guard guard(g_host.peer_lock);
        if (!g_host.connected.load(std::memory_order_acquire)) return;
        to = g_host.peer;
    }
    sendto(g_host.sock, static_cast<const char*>(data), int(size), 0,
           reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

void HostSend(const std::vector<uint8_t>& packet) {
    HostSend(packet.data(), packet.size());
}

void HostDisconnect(const char* reason) {
    std::lock_guard guard(g_host.peer_lock);
    if (!g_host.connected.exchange(false)) return;
    g_host.pad = {};
    LOGI("stream: Player 2 %s (%s)", reason, AddressText(g_host.peer).c_str());
}

void HostNetwork() {
    std::vector<uint8_t> buffer(2048);
    while (g_host.running.load(std::memory_order_acquire)) {
        sockaddr_in from{};
        int from_len = sizeof(from);
        const int n = recvfrom(g_host.sock, reinterpret_cast<char*>(buffer.data()), int(buffer.size()), 0,
                               reinterpret_cast<sockaddr*>(&from), &from_len);
        const uint32_t now = NowMs();
        Header header;
        const uint8_t* payload = nullptr;
        if (n > 0 && ParsePacket(buffer.data(), size_t(n), header, payload)) {
            const auto type = PacketType(header.type);
            std::unique_lock guard(g_host.peer_lock);
            const bool from_peer = g_host.connected.load(std::memory_order_acquire) && SameAddress(from, g_host.peer);
            if (type == PacketType::Hello && (from_peer || !g_host.connected.load(std::memory_order_acquire))) {
                if (!from_peer) {
                    g_host.peer = from;
                    g_host.pad = {};
                    g_host.connected.store(true, std::memory_order_release);
                    LOGI("stream: Player 2 connected from %s", AddressText(from).c_str());
                }
                g_host.last_seen = now;
                guard.unlock();
                HostSend(BuildPacket(PacketType::Welcome, g_host.seq++, nullptr, 0));
            } else if (type == PacketType::Input && from_peer && header.size == sizeof(RemotePad)) {
                std::memcpy(&g_host.pad, payload, sizeof(RemotePad));
                g_host.last_seen = now;
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
    auto factory = CreateFactory();
    std::vector<uint8_t> pixels, jpeg;
    while (g_host.running.load(std::memory_order_acquire)) {
        uint32_t w = 0, h = 0, id = 0;
        {
            std::unique_lock guard(g_host.frame_lock);
            g_host.frame_cv.wait_for(guard, std::chrono::milliseconds(100), [] {
                return g_host.frame_ready || !g_host.running.load(std::memory_order_acquire);
            });
            if (!g_host.frame_ready) continue;
            pixels.swap(g_host.frame);
            w = g_host.frame_w;
            h = g_host.frame_h;
            id = ++g_host.frame_id;
            g_host.frame_ready = false;
        }
        if (!factory || !EncodeJpeg(factory.Get(), pixels.data(), w, h, jpeg)) {
            LOG_FIRST(1, "stream: video frame encoding failed");
            continue;
        }
        VideoChunk chunk{};
        chunk.frame = id;
        chunk.width = uint16_t(w);
        chunk.height = uint16_t(h);
        chunk.total = uint32_t(jpeg.size());
        chunk.count = ChunkCount(jpeg.size());
        for (uint16_t i = 0; i < chunk.count; ++i) {
            chunk.index = i;
            const size_t offset = size_t(i) * kMaxChunk;
            const size_t size = std::min(kMaxChunk, jpeg.size() - offset);
            HostSend(BuildPacket(PacketType::Video, g_host.seq++, &chunk, sizeof(chunk), jpeg.data() + offset, size));
        }
    }
}

// ---------------------------------------------------------------- client

struct Client {
    SOCKET sock = INVALID_SOCKET;
    sockaddr_in host{};
    HWND window = nullptr;
    std::atomic<bool> running{true};
    std::atomic<bool> connected{false};
    std::atomic<uint32_t> last_packet{0};

    std::mutex frame_lock;
    std::vector<uint8_t> frame;
    uint32_t frame_w = 0, frame_h = 0;

    IXAudio2* xaudio = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    IXAudio2SourceVoice* voice = nullptr;
    uint32_t audio_rate = 0;
    bool audio_playing = false;
    AudioDecoder audio;
    std::vector<int16_t> audio_slots;
    uint32_t audio_next = 0;

    bool fullscreen = false;
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
};
Client* g_client = nullptr;

void ClientSend(Client& c, PacketType type, uint32_t seq, const void* payload = nullptr, size_t size = 0) {
    const auto packet = BuildPacket(type, seq, payload, size);
    sendto(c.sock, reinterpret_cast<const char*>(packet.data()), int(packet.size()), 0,
           reinterpret_cast<const sockaddr*>(&c.host), sizeof(c.host));
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

void ClientNetwork(Client& c) {
    ComScope com(COINIT_MULTITHREADED);
    auto factory = CreateFactory();
    FrameAssembler assembler;
    std::vector<uint8_t> buffer(2048), jpeg, pixels;
    while (c.running.load(std::memory_order_acquire)) {
        sockaddr_in from{};
        int from_len = sizeof(from);
        const int n = recvfrom(c.sock, reinterpret_cast<char*>(buffer.data()), int(buffer.size()), 0,
                               reinterpret_cast<sockaddr*>(&from), &from_len);
        Header header;
        const uint8_t* payload = nullptr;
        if (n <= 0 || !SameAddress(from, c.host) || !ParsePacket(buffer.data(), size_t(n), header, payload)) continue;
        c.last_packet.store(NowMs(), std::memory_order_release);
        const auto type = PacketType(header.type);
        if (type == PacketType::Welcome) {
            if (!c.connected.exchange(true)) {
                c.audio.Reset();
                LOGI("stream: connected to host %s", AddressText(c.host).c_str());
                PostMessageW(c.window, WM_APP + 1, 0, 0);
            }
        } else if (type == PacketType::Video && header.size > sizeof(VideoChunk)) {
            VideoChunk chunk;
            std::memcpy(&chunk, payload, sizeof(chunk));
            if (!assembler.Add(chunk, payload + sizeof(chunk), header.size - sizeof(chunk), jpeg)) continue;
            uint32_t w = 0, h = 0;
            if (!factory || !DecodeJpeg(factory.Get(), jpeg, pixels, w, h)) continue;
            {
                std::lock_guard guard(c.frame_lock);
                c.frame.swap(pixels);
                c.frame_w = w;
                c.frame_h = h;
            }
            InvalidateRect(c.window, nullptr, FALSE);
        } else if (type == PacketType::Audio && header.size > sizeof(AudioChunk)) {
            AudioChunk chunk;
            std::memcpy(&chunk, payload, sizeof(chunk));
            ClientAudio(c, chunk, payload + sizeof(chunk), header.size - sizeof(chunk));
        } else if (type == PacketType::Bye) {
            c.audio.Reset();
            if (c.connected.exchange(false)) LOGI("stream: host closed the session");
            PostMessageW(c.window, WM_APP + 1, 0, 0);
        }
    }
}

void ClientTitle(Client& c) {
    const wchar_t* title = c.connected.load(std::memory_order_acquire)
        ? L"OpenShao - Player 2 (F11: full screen)"
        : L"OpenShao - Player 2 (connecting to host...)";
    SetWindowTextW(c.window, title);
}

void ToggleFullscreen(Client& c) {
    const LONG style = GetWindowLongW(c.window, GWL_STYLE);
    if (!c.fullscreen) {
        MONITORINFO mi{sizeof(mi)};
        if (!GetWindowPlacement(c.window, &c.placement) ||
            !GetMonitorInfoW(MonitorFromWindow(c.window, MONITOR_DEFAULTTOPRIMARY), &mi))
            return;
        SetWindowLongW(c.window, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(c.window, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(c.window, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(c.window, &c.placement);
        SetWindowPos(c.window, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    c.fullscreen = !c.fullscreen;
}

void Paint(Client& c, HDC dc) {
    RECT rc;
    GetClientRect(c.window, &rc);
    const int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    std::lock_guard guard(c.frame_lock);
    if (c.frame.empty() || !c.frame_w || !c.frame_h) {
        FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        return;
    }
    // The guest image is 4:3; letterbox it inside the window.
    int w = cw, h = cw * 3 / 4;
    if (h > ch) { h = ch; w = ch * 4 / 3; }
    const int x = (cw - w) / 2, y = (ch - h) / 2;
    HBRUSH black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RECT bars[4] = {{0, 0, cw, y}, {0, y + h, cw, ch}, {0, y, x, y + h}, {x + w, y, cw, y + h}};
    for (auto& bar : bars) if (bar.right > bar.left && bar.bottom > bar.top) FillRect(dc, &bar, black);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = LONG(c.frame_w);
    bi.bmiHeader.biHeight = -LONG(c.frame_h);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    StretchDIBits(dc, x, y, w, h, 0, 0, int(c.frame_w), int(c.frame_h), c.frame.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
}

LRESULT CALLBACK ClientProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Client* c = g_client;
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (c) Paint(*c, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_APP + 1:
        if (c) ClientTitle(*c);
        InvalidateRect(hwnd, nullptr, FALSE);
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
    if (!StartSockets(error)) return false;
    g_host.sock = OpenSocket(port, error);
    if (g_host.sock == INVALID_SOCKET) return false;
    g_host.connected.store(false);
    g_host.running.store(true);
    g_host.net = std::thread(HostNetwork);
    g_host.encoder = std::thread(HostEncoder);
    upnp::Start(port);
    LOGI("stream: hosting on UDP port %u, waiting for Player 2", unsigned(port));
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
    closesocket(g_host.sock);
    g_host.sock = INVALID_SOCKET;
    g_host.connected.store(false);
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

void SubmitVideo(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t pitch) {
    if (!g_host.connected.load(std::memory_order_acquire) || !width || !height || width > 4096 || height > 4096) return;
    {
        std::lock_guard guard(g_host.frame_lock);
        g_host.frame.resize(size_t(width) * height * 4);
        for (uint32_t y = 0; y < height; ++y)
            std::memcpy(&g_host.frame[size_t(y) * width * 4], bgra + size_t(y) * pitch, size_t(width) * 4);
        g_host.frame_w = width;
        g_host.frame_h = height;
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

int RunClient(const std::string& host, uint16_t port) {
    std::string error;
    Client c;
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
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = ClientProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)) /* IDC_ARROW */;
    wc.lpszClassName = L"OpenShaoStreamClient";
    RegisterClassExW(&wc);
    RECT rc{0, 0, 960, 720};
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
    LOGI("stream: connecting to host %s", AddressText(c.host).c_str());

    std::thread net(ClientNetwork, std::ref(c));
    SDL_Gamepad* gamepad = nullptr;
    uint32_t seq = 0, last_input = 0, last_hello = 0;
    for (bool quit = false; !quit;) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 4, QS_ALLINPUT);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) quit = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const uint32_t now = NowMs();
        if (c.connected.load(std::memory_order_acquire) &&
            now - c.last_packet.load(std::memory_order_acquire) > kPeerTimeoutMs) {
            c.connected.store(false);
            LOGW("stream: lost the host, reconnecting");
            ClientTitle(c);
        }
        if (!c.connected.load(std::memory_order_acquire) && now - last_hello >= kHelloIntervalMs) {
            ClientSend(c, PacketType::Hello, seq++);
            last_hello = now;
        }
        if (c.connected.load(std::memory_order_acquire) && now - last_input >= kInputIntervalMs) {
            SDL_UpdateGamepads();
            RemotePad pad{};
            ReadGamepad(gamepad, pad);
            if (GetForegroundWindow() == c.window) ReadKeyboard(pad);
            ClientSend(c, PacketType::Input, seq++, &pad, sizeof(pad));
            last_input = now;
        }
    }
    for (int i = 0; i < 3; ++i) ClientSend(c, PacketType::Bye, seq++);
    c.running.store(false);
    net.join();
    if (gamepad) SDL_CloseGamepad(gamepad);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    if (c.voice) c.voice->DestroyVoice();
    if (c.master) c.master->DestroyVoice();
    if (c.xaudio) c.xaudio->Release();
    closesocket(c.sock);
    g_client = nullptr;
    return 0;
}

#endif

} // namespace openshao::stream

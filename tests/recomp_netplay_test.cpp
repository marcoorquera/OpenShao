#include "netplay.h"
#include "core/log.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace logx {
void Write(Level, const char*, ...) {}
}

static void Check(bool ok, const char* description) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        std::exit(1);
    }
}

static bool SamePad(const input::Pad& a, const input::Pad& b) {
    return a.connected == b.connected &&
           a.buttons == b.buttons &&
           std::memcmp(a.analog, b.analog, sizeof(a.analog)) == 0 &&
           a.lx == b.lx && a.ly == b.ly && a.rx == b.rx && a.ry == b.ry;
}

int main() {
    using namespace openshao::netplay;

    // 1. WirePad pack/unpack fidelity
    input::Pad original{};
    original.connected = true;
    original.buttons = 0x9015;
    for (int i = 0; i < 8; ++i) original.analog[i] = uint8_t(i * 31 + 7);
    original.lx = -32768;
    original.ly = 32767;
    original.rx = -12345;
    original.ry = 23456;
    const WirePad wire = PackPad(original);
    const input::Pad restored = UnpackPad(wire);
    Check(SamePad(original, restored), "PackPad and UnpackPad preserve all 19 controller bytes");

    // 2. Endpoint parsing
    std::string host;
    uint16_t port = 0;
    Check(ParseEndpoint("192.168.1.42", host, port) && host == "192.168.1.42" && port == kDefaultPort,
          "default port when omitted");
    Check(ParseEndpoint(" 10.0.0.5:28000 ", host, port) && host == "10.0.0.5" && port == 28000,
          "custom port and whitespace trimming");
    Check(!ParseEndpoint("", host, port) &&
          !ParseEndpoint(":27886", host, port) &&
          !ParseEndpoint("127.0.0.1:", host, port) &&
          !ParseEndpoint("127.0.0.1:0", host, port) &&
          !ParseEndpoint("127.0.0.1:65536", host, port) &&
          !ParseEndpoint("127.0.0.1:abc", host, port),
          "invalid endpoints are rejected");

    // 3. Full lockstep UDP session between Host and Client threads over loopback
    Session host_session;
    Session client_session;
    constexpr uint32_t kDelay = 2;
    constexpr uint32_t kFrames = 120;
    constexpr uint32_t kTestSeed = 0xCAFEBABEu;

    std::vector<input::Pad> host_history_p1(kFrames), host_history_p2(kFrames);
    std::vector<input::Pad> client_history_p1(kFrames), client_history_p2(kFrames);
    bool host_ok = false, client_ok = false;

    std::thread host_thread([&] {
        Config cfg{};
        cfg.role = Role::Host;
        cfg.port = 0; // ephemeral port assigned by OS
        cfg.input_delay = kDelay;
        cfg.fixed_seed = kTestSeed;
        cfg.handshake_timeout_ms = 5000;
        cfg.frame_timeout_ms = 2000;
        std::string err;
        if (!host_session.Start(cfg, err)) {
            std::fprintf(stderr, "Host start failed: %s\n", err.c_str());
            return;
        }
        for (uint32_t f = 0; f < kFrames; ++f) {
            input::Pad local{};
            local.connected = true;
            local.buttons = uint16_t(0x1000u | (f & 0xFFu));
            local.analog[0] = uint8_t(f * 2u);
            local.lx = int16_t(f * 100);
            const uint32_t hash = (f == 60) ? 0x11111111u : (0xA0000000u + f);
            if (!host_session.AdvanceFrame(local, hash)) return;
            host_history_p1[f] = host_session.GetSyncedPad(0);
            host_history_p2[f] = host_session.GetSyncedPad(1);
        }
        host_ok = true;
    });

    // Wait until host binds its ephemeral UDP port
    uint16_t bound_port = 0;
    for (int wait = 0; wait < 200 && bound_port == 0; ++wait) {
        bound_port = host_session.BoundPort();
        if (!bound_port) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    Check(bound_port != 0, "host bound ephemeral UDP port");

    std::thread client_thread([&] {
        Config cfg{};
        cfg.role = Role::Client;
        cfg.host_address = "127.0.0.1";
        cfg.port = bound_port;
        cfg.input_delay = 1; // client adopts host's delay (kDelay = 2)
        cfg.handshake_timeout_ms = 5000;
        cfg.frame_timeout_ms = 2000;
        std::string err;
        if (!client_session.Start(cfg, err)) {
            std::fprintf(stderr, "Client start failed: %s\n", err.c_str());
            return;
        }
        for (uint32_t f = 0; f < kFrames; ++f) {
            input::Pad local{};
            local.connected = true;
            local.buttons = uint16_t(0x2000u | (f & 0xFFu));
            local.analog[1] = uint8_t(f * 3u);
            local.ry = int16_t(-int(f) * 100);
            const uint32_t hash = (f == 60) ? 0x22222222u : (0xA0000000u + f);
            if (!client_session.AdvanceFrame(local, hash)) return;
            client_history_p1[f] = client_session.GetSyncedPad(0);
            client_history_p2[f] = client_session.GetSyncedPad(1);
        }
        client_ok = true;
    });

    host_thread.join();
    client_thread.join();

    Check(host_ok && client_ok, "both host and client completed 120 lockstep frames");
    Check(host_session.AgreedSeed() == kTestSeed && client_session.AgreedSeed() == kTestSeed,
          "client synchronized RNG seed from host handshake");
    Check(host_session.IsLoopback() && client_session.IsLoopback(),
          "both host and client detect 127.0.0.1 loopback session");
    Check(host_session.GuestSystemTime100ns() == client_session.GuestSystemTime100ns() &&
          host_session.GuestInterruptTime100ns() == client_session.GuestInterruptTime100ns() &&
          host_session.GuestTickCountMs() == client_session.GuestTickCountMs(),
          "deterministic guest kernel clocks match identically between host and client");
    const uint64_t p_first = host_session.GuestPerfCounter();
    const uint64_t p_second = host_session.GuestPerfCounter();
    const uint64_t c_first = client_session.GuestPerfCounter();
    const uint64_t c_second = client_session.GuestPerfCounter();
    Check(p_first == c_first && p_second == c_second && p_second > p_first,
          "GuestPerfCounter advances monotonically within a frame and matches across peers");
    Check(host_session.InputDelay() == kDelay && client_session.InputDelay() == kDelay,
          "client synchronized input delay from host handshake");
    Check(host_session.DesyncCount() == 1 && client_session.DesyncCount() == 1,
          "state hash detector recorded exactly the single injected mismatch at frame 60");

    for (uint32_t f = 0; f < kFrames; ++f) {
        Check(SamePad(host_history_p1[f], client_history_p1[f]),
              "Host and Client saw identical Player 1 pad at every frame");
        Check(SamePad(host_history_p2[f], client_history_p2[f]),
              "Host and Client saw identical Player 2 pad at every frame");
        if (f < kDelay) {
            Check(host_history_p1[f].connected && host_history_p1[f].buttons == 0,
                  "initial delay window has neutral connected P1 pad");
            Check(host_history_p2[f].connected && host_history_p2[f].buttons == 0,
                  "initial delay window has neutral connected P2 pad");
        } else {
            const uint32_t src = f - kDelay;
            Check(host_history_p1[f].buttons == uint16_t(0x1000u | (src & 0xFFu)) &&
                  host_history_p1[f].analog[0] == uint8_t(src * 2u) &&
                  host_history_p1[f].lx == int16_t(src * 100),
                  "Player 1 input arrives at exact frame + delay");
            Check(host_history_p2[f].buttons == uint16_t(0x2000u | (src & 0xFFu)) &&
                  host_history_p2[f].analog[1] == uint8_t(src * 3u) &&
                  host_history_p2[f].ry == int16_t(-int(src) * 100),
                  "Player 2 input arrives at exact frame + delay");
        }
    }

    host_session.Stop();
    client_session.Stop();
    Check(!host_session.IsActive() && !client_session.IsActive(), "sessions stop cleanly");

    // 4. Global singleton API used by controller.cpp and d3d8.cpp
    Session peer_client;
    std::thread global_host([&] {
        Config cfg{};
        cfg.role = Role::Host;
        cfg.port = 27899;
        cfg.input_delay = 1;
        cfg.fixed_seed = 0x13572468u;
        cfg.handshake_timeout_ms = 5000;
        cfg.frame_timeout_ms = 2000;
        std::string err;
        Check(Start(cfg, err), "global Start(Host)");
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    {
        Config ccfg{};
        ccfg.role = Role::Client;
        ccfg.host_address = "127.0.0.1";
        ccfg.port = 27899;
        ccfg.handshake_timeout_ms = 5000;
        ccfg.frame_timeout_ms = 2000;
        std::string err;
        Check(peer_client.Start(ccfg, err), "peer Start(Client) against global host");
    }
    global_host.join();
    Check(IsActive() && CurrentRole() == Role::Host && LocalPlayerIndex() == 0 && AgreedSeed() == 0x13572468u,
          "global singleton state active after handshake");
    for (uint32_t f = 0; f < 4; ++f) {
        input::Pad p1{true, uint16_t(0x10 + f)}, p2{true, uint16_t(0x20 + f)};
        std::thread step([&] { Check(peer_client.AdvanceFrame(p2, 0x55AAu + f), "peer step"); });
        Check(AdvanceFrame(p1, 0x55AAu + f), "global step");
        step.join();
        Check(SamePad(GetSyncedPad(0), peer_client.GetSyncedPad(0)) &&
              SamePad(GetSyncedPad(1), peer_client.GetSyncedPad(1)),
              "global GetSyncedPad matches peer across frames");
    }
    Check(!GetSyncedPad(2).connected && !GetSyncedPad(-1).connected, "ports outside 0..1 report disconnected");
    peer_client.Stop();
    Stop();
    Check(!IsActive() && LocalPlayerIndex() == -1, "global Stop resets state");

    std::puts("PASS: WirePad serialization, endpoint parser, UDP handshake, 120-frame lockstep and desync detection");
    return 0;
}

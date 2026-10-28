#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include "core/input.h"

namespace openshao::netplay {

inline constexpr uint16_t kDefaultPort = 27886;
inline constexpr uint32_t kDefaultDelay = 1;
inline constexpr uint32_t kMaxDelay = 8;
inline constexpr uint32_t kPacketMagic = 0x4F414853u; // 'SHAO'
inline constexpr uint8_t kProtocolVersion = 1;

enum class Role : uint8_t {
    None = 0,
    Host = 1,
    Client = 2
};

enum class PacketType : uint8_t {
    Handshake = 1,
    HandshakeAck = 2,
    FrameInput = 3,
    Disconnect = 4
};

struct Config {
    Role role = Role::None;
    std::string host_address = "127.0.0.1";
    uint16_t port = kDefaultPort;
    uint32_t input_delay = kDefaultDelay;
    uint32_t handshake_timeout_ms = 30000;
    uint32_t frame_timeout_ms = 5000;
    uint32_t fixed_seed = 0; // 0 generates a time-based seed on Host
};

#pragma pack(push, 1)
struct WirePad {
    uint8_t connected = 0;
    uint16_t buttons = 0;
    uint8_t analog[8]{};
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

struct Packet {
    uint32_t magic = kPacketMagic;
    uint8_t version = kProtocolVersion;
    uint8_t type = 0;
    uint8_t player_index = 0;
    uint8_t input_delay = uint8_t(kDefaultDelay);
    uint32_t frame = 0;
    uint32_t ack_frame = 0;
    uint32_t rng_seed = 0;
    uint32_t state_hash = 0;
    WirePad pad{};
    WirePad prev_pad{};
};
#pragma pack(pop)

static_assert(sizeof(WirePad) == 19, "WirePad must match 19-byte packed controller state");
static_assert(sizeof(Packet) == 62, "Packet wire layout must remain fixed across platforms");

WirePad PackPad(const input::Pad& pad);
input::Pad UnpackPad(const WirePad& wire);
bool ParseEndpoint(std::string_view text, std::string& host_out, uint16_t& port_out);

class Session {
public:
    Session() = default;
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    bool Start(const Config& config, std::string& error);
    void Stop();

    bool IsActive() const { return active_.load(std::memory_order_acquire); }
    bool IsLoopback() const { return is_loopback_.load(std::memory_order_acquire); }
    Role CurrentRole() const { return role_.load(std::memory_order_acquire); }
    int LocalPlayerIndex() const;
    uint32_t CurrentFrame() const { return current_frame_.load(std::memory_order_acquire); }
    uint32_t AgreedSeed() const { return agreed_seed_.load(std::memory_order_acquire); }
    uint32_t InputDelay() const { return input_delay_.load(std::memory_order_acquire); }
    uint32_t DesyncCount() const { return desync_count_.load(std::memory_order_acquire); }
    uint16_t BoundPort() const { return bound_port_.load(std::memory_order_acquire); }

    uint64_t GuestPerfCounter();
    uint64_t GuestInterruptTime100ns() const;
    uint64_t GuestSystemTime100ns() const;
    uint32_t GuestTickCountMs() const;

    bool AdvanceFrame(const input::Pad& local_pad, uint32_t local_state_hash = 0);
    input::Pad GetSyncedPad(int port) const;

private:
    static constexpr size_t kRingSize = 128;
    struct FrameSlot {
        uint32_t frame = UINT32_MAX;
        input::Pad pad{};
        uint32_t state_hash = 0;
        bool has_pad = false;
        bool has_hash = false;
    };

    bool SendPacket(const Packet& pkt);
    void PumpIncoming();
    void StoreRemotePad(uint32_t frame, const input::Pad& pad, uint32_t state_hash, bool include_hash);

    mutable std::mutex mutex_;
    intptr_t socket_ = -1;
    std::array<uint8_t, 32> peer_addr_{};
    uint32_t peer_addr_len_ = 0;
    bool has_peer_ = false;

    std::atomic<bool> active_{false};
    std::atomic<bool> is_loopback_{false};
    std::atomic<Role> role_{Role::None};
    std::atomic<uint32_t> current_frame_{0};
    std::atomic<uint32_t> sub_frame_ticks_{0};
    std::atomic<uint32_t> agreed_seed_{0};
    std::atomic<uint32_t> input_delay_{kDefaultDelay};
    std::atomic<uint32_t> desync_count_{0};
    std::atomic<uint16_t> bound_port_{0};
    uint32_t frame_timeout_ms_ = 5000;
    uint32_t highest_remote_frame_ = 0;
    bool has_remote_frame_ = false;

    std::array<FrameSlot, kRingSize> local_ring_{};
    std::array<FrameSlot, kRingSize> remote_ring_{};
    std::array<input::Pad, 2> synced_pads_{};
};

bool Start(const Config& config, std::string& error);
void Stop();

bool IsActive();
bool IsLoopbackSession();
Role CurrentRole();
int LocalPlayerIndex();
uint32_t CurrentFrame();
uint32_t AgreedSeed();
uint32_t InputDelay();
uint32_t DesyncCount();

uint64_t GuestPerfCounter();
uint64_t GuestInterruptTime100ns();
uint64_t GuestSystemTime100ns();
uint32_t GuestTickCountMs();

bool AdvanceFrame(const input::Pad& local_pad, uint32_t local_state_hash = 0);
input::Pad GetSyncedPad(int port);

} // namespace openshao::netplay

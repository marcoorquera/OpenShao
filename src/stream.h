#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Host-authoritative LAN play: the host runs the only copy of the game in local Ko-op,
// streams video and audio, and receives Player 2's gamepad as a virtual SDL gamepad.
namespace openshao::stream {

inline constexpr uint16_t kDefaultPort = 27886;
inline constexpr uint32_t kMagic = 0x5453484Fu; // 'OHST'
inline constexpr uint8_t kVersion = 1;
// Keeps every datagram below a 1280-byte IPv6 minimum MTU.
inline constexpr size_t kMaxChunk = 1180;
inline constexpr uint32_t kPeerTimeoutMs = 3000;
inline constexpr uint32_t kAudioPacketFrames = 240;

enum class PacketType : uint8_t {
    Hello = 1,
    Welcome = 2,
    Input = 3,
    Video = 4,
    Audio = 5,
    Bye = 6
};

#pragma pack(push, 1)
struct Header {
    uint32_t magic = kMagic;
    uint8_t version = kVersion;
    uint8_t type = 0;
    uint16_t size = 0; // payload bytes after the header
    uint32_t seq = 0;
};

// SDL gamepad state: one bit per SDL_GamepadButton, axes in SDL_GamepadAxis order.
struct RemotePad {
    uint32_t buttons = 0;
    std::array<int16_t, 6> axes{};
};

struct VideoChunk {
    uint32_t frame = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t total = 0; // encoded frame bytes
    uint16_t index = 0;
    uint16_t count = 0;
};

struct AudioChunk {
    uint32_t rate = 0;
    uint16_t frames = 0; // interleaved stereo int16 frames that follow
    uint16_t reserved = 0;
};
#pragma pack(pop)

static_assert(sizeof(Header) == 12, "stream header layout must remain fixed");
static_assert(sizeof(RemotePad) == 16, "remote pad layout must remain fixed");
static_assert(sizeof(VideoChunk) == 16, "video chunk layout must remain fixed");
static_assert(sizeof(AudioChunk) == 8, "audio chunk layout must remain fixed");

bool ParsePacket(const uint8_t* data, size_t size, Header& header, const uint8_t*& payload);
std::vector<uint8_t> BuildPacket(PacketType type, uint32_t seq, const void* payload, size_t size,
                                 const void* extra = nullptr, size_t extra_size = 0);
uint16_t ChunkCount(size_t total);

// Reassembles one encoded frame at a time; a newer frame discards an incomplete older one.
class FrameAssembler {
public:
    bool Add(const VideoChunk& chunk, const uint8_t* data, size_t size, std::vector<uint8_t>& complete);
    uint32_t Dropped() const { return dropped_; }

private:
    uint32_t frame_ = 0;
    bool active_ = false;
    uint32_t total_ = 0;
    uint16_t count_ = 0;
    uint16_t received_ = 0;
    uint32_t last_complete_ = 0;
    bool has_complete_ = false;
    uint32_t dropped_ = 0;
    std::vector<uint8_t> data_;
    std::vector<bool> have_;
};

// Host side. All calls are cheap no-ops without a connected client.
bool StartHost(uint16_t port, std::string& error);
void StopHost();
bool HostActive();
bool ClientConnected();
bool RemotePadState(RemotePad& pad);
bool WantsVideo();
void SubmitVideo(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t pitch);
void SubmitAudio(const float* stereo, uint32_t frames, uint32_t rate);

// Client side: runs the viewer window until it closes. Returns the process exit code.
int RunClient(const std::string& host, uint16_t port);

} // namespace openshao::stream

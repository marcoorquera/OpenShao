#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct OpusEncoder;
struct OpusDecoder;

// Host-authoritative LAN play: the host runs the only copy of the game in local Ko-op,
// streams video and audio, and receives Player 2's gamepad as a virtual SDL gamepad.
namespace openshao::stream {

inline constexpr uint16_t kDefaultPort = 27886;
inline constexpr uint32_t kMagic = 0x5453484Fu; // 'OHST'
inline constexpr uint8_t kVersion = 4;
// Keeps every datagram below a 1280-byte IPv6 minimum MTU.
inline constexpr size_t kMaxChunk = 1180;
inline constexpr uint32_t kPeerTimeoutMs = 3000;
inline constexpr uint32_t kAudioRate = 48000;
inline constexpr uint32_t kAudioPacketFrames = 480;
inline constexpr uint16_t kAudioCodecOpus = 1;
inline constexpr uint32_t kAudioBitrate = 128000;
inline constexpr uint32_t kMaxPlcFrames = 3;
inline constexpr uint32_t kAudioResyncPackets = 50; // 500 ms
inline constexpr uint8_t kVideoCodecH264 = 1;
inline constexpr uint8_t kVideoFlagKeyframe = 1;
inline constexpr uint32_t kVideoFrameRate = 60;
inline constexpr uint32_t kVideoBitrate = 12'000'000;
inline constexpr uint32_t kVideoGopFrames = 240;    // a periodic keyframe repairs any missed request
inline constexpr uint32_t kVideoMaxWidth = 1280;
inline constexpr uint32_t kVideoMaxHeight = 960;
inline constexpr uint32_t kKeyframeRequestMs = 250;
inline constexpr uint8_t kFecGroup = 8;
inline constexpr uint32_t kStatsIntervalMs = 500;
inline constexpr uint32_t kMinVideoBitrate = 2'000'000;
inline constexpr uint32_t kMaxVideoBitrate = kVideoBitrate;

enum class PacketType : uint8_t {
    Hello = 1,
    Welcome = 2,
    Input = 3,
    Video = 4,
    Audio = 5,
    Bye = 6,
    KeyframeRequest = 7,
    Stats = 8
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
    uint8_t codec = kVideoCodecH264;
    uint8_t flags = 0;
    uint8_t fec_group = 0;
    uint8_t fec_count = 0;
};

struct AudioChunk {
    uint32_t rate = 0;
    uint16_t frames = 0; // stereo frames represented by the packet (480)
    uint16_t codec = 0;  // 1 = Opus
    uint32_t index = 0;  // per-audio packet counter
};

struct StreamStats {
    uint32_t packets_received = 0;  // host packets since the last Stats
    uint32_t packets_expected = 0;  // host sequence numbers passed since the last Stats
    uint32_t frames_recovered = 0; // frames completed thanks to parity
    uint32_t frames_lost = 0;      // frame ids skipped or never completed
};
#pragma pack(pop)

static_assert(sizeof(Header) == 12, "stream header layout must remain fixed");
static_assert(sizeof(RemotePad) == 16, "remote pad layout must remain fixed");
static_assert(sizeof(VideoChunk) == 20, "video chunk layout must remain fixed");
static_assert(sizeof(AudioChunk) == 12, "audio chunk layout must remain fixed");
static_assert(sizeof(StreamStats) == 16, "stream stats layout must remain fixed");

bool ParsePacket(const uint8_t* data, size_t size, Header& header, const uint8_t*& payload);
std::vector<uint8_t> BuildPacket(PacketType type, uint32_t seq, const void* payload, size_t size,
                                 const void* extra = nullptr, size_t extra_size = 0);
uint16_t ChunkCount(size_t total);
void BuildParity(const uint8_t* data, size_t total, uint16_t count, uint8_t group, std::vector<uint8_t>& parity);

// Integer downscale factor that fits a captured frame inside kVideoMaxWidth x kVideoMaxHeight.
uint32_t VideoScale(uint32_t width, uint32_t height);
// Size that the GPU shrinks a rendered frame to before readback: the largest even size inside
// kVideoMaxWidth x kVideoMaxHeight with the frame's aspect ratio. It never enlarges the frame.
void CaptureSize(uint32_t width, uint32_t height, uint32_t& out_width, uint32_t& out_height);
// BT.709 limited range. BgraToNv12 box-filters by `scale` and rounds the output size down to even.
void BgraToNv12(const uint8_t* bgra, uint32_t pitch, uint32_t width, uint32_t height, uint32_t scale,
                std::vector<uint8_t>& nv12, uint32_t& out_width, uint32_t& out_height);
void Nv12ToBgra(const uint8_t* y, const uint8_t* uv, uint32_t pitch, uint32_t width, uint32_t height,
                std::vector<uint8_t>& bgra);

// Accumulates float stereo input into 10 ms (480-frame) Opus packets.
class AudioEncoder {
public:
    AudioEncoder() = default;
    ~AudioEncoder();
    AudioEncoder(const AudioEncoder&) = delete;
    AudioEncoder& operator=(const AudioEncoder&) = delete;

    void Reset();

    template <typename Emit>
    bool Submit(const float* stereo, uint32_t frames, uint32_t rate, Emit&& emit) {
        if (!Prepare(rate)) return false;
        if (!stereo || !frames) return true;
        uint32_t offset = 0;
        while (offset < frames) {
            const uint32_t need = kAudioPacketFrames - buffered_;
            const uint32_t rem = frames - offset;
            const uint32_t take = need < rem ? need : rem;
            for (uint32_t i = 0; i < take * 2; ++i)
                pcm_[size_t(buffered_) * 2 + i] = stereo[size_t(offset) * 2 + i];
            buffered_ += take;
            offset += take;
            if (buffered_ == kAudioPacketFrames) {
                buffered_ = 0;
                AudioChunk chunk{};
                std::array<uint8_t, kMaxChunk> packet{};
                const size_t size = EncodeFrame(chunk, packet.data(), packet.size());
                if (size) emit(chunk, packet.data(), size);
            }
        }
        return true;
    }

private:
    bool Prepare(uint32_t rate);
    size_t EncodeFrame(AudioChunk& chunk, uint8_t* out, size_t max_size);

    OpusEncoder* encoder_ = nullptr;
    uint32_t rate_ = 0;
    uint32_t buffered_ = 0;
    uint32_t index_ = 0;
    std::array<float, kAudioPacketFrames * 2> pcm_{};
};

// Decodes Opus packets to interleaved stereo int16, recovering lost packets with FEC or PLC.
class AudioDecoder {
public:
    AudioDecoder() = default;
    ~AudioDecoder();
    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    void Reset();

    template <typename AcquireSlot, typename CommitSlot>
    bool Decode(const AudioChunk& chunk, const uint8_t* data, size_t size,
                AcquireSlot&& acquire_slot, CommitSlot&& commit_slot) {
        uint32_t gap = 0;
        if (!Prepare(chunk, data, size, gap)) return false;
        if (gap == 1) {
            int16_t* slot = acquire_slot();
            if (!DecodeFrame(data, size, true, slot)) return false;
            commit_slot(slot);
        } else if (gap > 1) {
            const uint32_t plc = gap < kMaxPlcFrames ? gap : kMaxPlcFrames;
            for (uint32_t i = 0; i < plc; ++i) {
                int16_t* slot = acquire_slot();
                if (!DecodeFrame(nullptr, 0, false, slot)) return false;
                commit_slot(slot);
            }
        }
        int16_t* slot = acquire_slot();
        if (!DecodeFrame(data, size, false, slot)) return false;
        commit_slot(slot);
        AdvanceIndex(chunk.index);
        return true;
    }

    bool Decode(const AudioChunk& chunk, const uint8_t* data, size_t size, std::vector<int16_t>& out);

private:
    bool Prepare(const AudioChunk& chunk, const uint8_t* data, size_t size, uint32_t& gap);
    bool DecodeFrame(const uint8_t* data, size_t size, bool fec, int16_t* out);
    void AdvanceIndex(uint32_t index);

    OpusDecoder* decoder_ = nullptr;
    uint32_t expected_index_ = 0;
    bool has_index_ = false;
};

// Reassembles one encoded frame at a time; a newer frame discards an incomplete older one.
class FrameAssembler {
public:
    bool Add(const VideoChunk& chunk, const uint8_t* data, size_t size, std::vector<uint8_t>& complete);
    uint32_t Dropped() const { return dropped_; }
    uint32_t Recovered() const { return recovered_; }
    bool IsFrameComplete(uint32_t frame) const { return has_complete_ && int32_t(frame - last_complete_) <= 0; }
    bool IsActive() const { return active_; }

private:
    void TryRecover(uint8_t g);

    uint32_t frame_ = 0;
    bool active_ = false;
    uint32_t total_ = 0;
    uint16_t count_ = 0;
    uint8_t fec_group_ = 0;
    uint8_t fec_count_ = 0;
    uint16_t received_ = 0;
    uint32_t last_complete_ = 0;
    bool has_complete_ = false;
    uint32_t dropped_ = 0;
    uint32_t recovered_ = 0;
    bool recovered_this_frame_ = false;
    std::vector<uint8_t> data_;
    std::vector<bool> have_;
    std::vector<bool> have_parity_;
    std::vector<uint8_t> parity_data_;
};

// Counts host packets for StreamStats by header sequence number. A packet counts as expected only
// once a later one arrives, so a report sent while a frame's parity is still in flight shows no loss.
class SequenceCounter {
public:
    void Add(uint32_t seq, StreamStats& stats);
    void Reset() { started_ = false; }

private:
    bool started_ = false;
    uint32_t highest_ = 0;
};

class BitrateController {
public:
    explicit BitrateController(uint32_t start = kVideoBitrate);
    uint32_t Update(const StreamStats& s, uint32_t now_ms);
    uint32_t Bitrate() const { return bitrate_; }

private:
    uint32_t bitrate_ = kVideoBitrate;
    uint32_t last_drop_ms_ = 0;
    bool has_dropped_ = false;
    uint32_t last_raise_ms_ = 0;
    bool has_raised_ = false;
    uint32_t clean_start_ms_ = 0;
    bool clean_active_ = false;
};

// Host side. All calls are cheap no-ops without a connected client.
bool StartHost(uint16_t port, std::string& error);
void StopHost();
bool HostActive();
bool ClientConnected();
std::string PublicEndpoint();
bool RemotePadState(RemotePad& pad);
bool WantsVideo();
void SubmitVideo(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t pitch);
void SubmitAudio(const float* stereo, uint32_t frames, uint32_t rate);

// Client side: runs the viewer window until it closes. Returns the process exit code.
int RunClient(const std::string& host, uint16_t port);

// Internet play. The launcher exchanges the connection codes in the same process before the session
// starts; StartHost and RunClient then use the prepared connection instead of a LAN socket.
enum class OnlineStep {
    Idle,
    Preparing,  // gathering this computer's addresses
    Waiting,    // host: the invite is ready, waiting for the reply code
    Connecting, // codes exchanged; Player 2 can start, and the host can start once a reply is accepted
    Connected,
    Failed,
};
struct OnlineStatus {
    OnlineStep step = OnlineStep::Idle;
    bool host = false;
    std::string code;  // the invite (host) or the reply (Player 2) to send to the other player
    std::string error;
};
void CreateInvite();
bool AcceptReply(const std::string& reply, std::string& error);
bool JoinInvite(const std::string& invite, std::string& error);
OnlineStatus Online();
void CancelOnline();

} // namespace openshao::stream

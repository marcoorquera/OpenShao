#pragma once
#include <cstdint>
#include <memory>
#include <vector>

// Media Foundation H.264 for the stream. Windows only; callers own COM and Media Foundation startup
// through StartMediaFoundation().
namespace openshao::stream {

bool StartMediaFoundation();

class VideoEncoder {
public:
    VideoEncoder();
    ~VideoEncoder();
    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    // Encodes one NV12 frame. Reopens the encoder when the size changes. `out` receives Annex B data,
    // which can be empty when the encoder buffers the frame.
    bool Encode(const uint8_t* nv12, uint32_t width, uint32_t height, bool keyframe,
                std::vector<uint8_t>& out, bool& is_keyframe);
    void SetBitrate(uint32_t bps);

private:
    struct State;
    std::unique_ptr<State> s_;
};

class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // Decodes one Annex B access unit to packed NV12 (see PackNv12) cropped to width x height.
    // Returns false when no frame came out or decoding failed; `failed` tells the two apart.
    bool Decode(const uint8_t* data, size_t size, uint32_t width, uint32_t height,
                std::vector<uint8_t>& nv12, bool& failed);
    void Reset();

private:
    struct State;
    std::unique_ptr<State> s_;
};

} // namespace openshao::stream

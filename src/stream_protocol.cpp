#include "stream.h"

#include <opus.h>

#include <cstring>

namespace openshao::stream {

AudioEncoder::~AudioEncoder() {
    Reset();
}

void AudioEncoder::Reset() {
    if (encoder_) {
        opus_encoder_destroy(encoder_);
        encoder_ = nullptr;
    }
    rate_ = 0;
    buffered_ = 0;
}

bool AudioEncoder::Prepare(uint32_t rate) {
    if (rate != rate_) {
        if (encoder_) {
            opus_encoder_destroy(encoder_);
            encoder_ = nullptr;
        }
        buffered_ = 0;
        rate_ = rate;
    }
    if (rate != kAudioRate) return false;
    if (!encoder_) {
        int err = OPUS_OK;
        encoder_ = opus_encoder_create(opus_int32(kAudioRate), 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
        if (!encoder_ || err != OPUS_OK) {
            if (encoder_) opus_encoder_destroy(encoder_);
            encoder_ = nullptr;
            return false;
        }
        opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(opus_int32(kAudioBitrate)));
        opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(5));
        opus_encoder_ctl(encoder_, OPUS_SET_INBAND_FEC(1));
        opus_encoder_ctl(encoder_, OPUS_SET_PACKET_LOSS_PERC(5));
    }
    return true;
}

size_t AudioEncoder::EncodeFrame(AudioChunk& chunk, uint8_t* out, size_t max_size) {
    if (!encoder_) return 0;
    const int bytes = opus_encode_float(encoder_, pcm_.data(), int(kAudioPacketFrames), out, opus_int32(max_size));
    if (bytes <= 0) return 0;
    chunk.rate = rate_;
    chunk.frames = uint16_t(kAudioPacketFrames);
    chunk.codec = kAudioCodecOpus;
    chunk.index = index_++;
    return size_t(bytes);
}

AudioDecoder::~AudioDecoder() {
    Reset();
}

void AudioDecoder::Reset() {
    if (decoder_) {
        opus_decoder_destroy(decoder_);
        decoder_ = nullptr;
    }
    expected_index_ = 0;
    has_index_ = false;
}

bool AudioDecoder::Prepare(const AudioChunk& chunk, const uint8_t* data, size_t size, uint32_t& gap) {
    if (chunk.codec != kAudioCodecOpus || chunk.rate != kAudioRate || chunk.frames != kAudioPacketFrames || !data || !size)
        return false;
    if (!decoder_) {
        int err = OPUS_OK;
        decoder_ = opus_decoder_create(opus_int32(kAudioRate), 2, &err);
        if (!decoder_ || err != OPUS_OK) {
            if (decoder_) opus_decoder_destroy(decoder_);
            decoder_ = nullptr;
            return false;
        }
    }
    if (!has_index_) {
        gap = 0;
    } else {
        const int32_t diff = int32_t(chunk.index - expected_index_);
        // A late duplicate is dropped; a large step back means the host restarted its counter.
        if (diff < 0 && diff > -int32_t(kAudioResyncPackets)) return false;
        gap = diff < 0 ? 0 : uint32_t(diff);
    }
    return true;
}

bool AudioDecoder::DecodeFrame(const uint8_t* data, size_t size, bool fec, int16_t* out) {
    if (!decoder_ || !out) return false;
    const int frames = opus_decode(decoder_, data, opus_int32(size), out, int(kAudioPacketFrames), fec ? 1 : 0);
    return frames == int(kAudioPacketFrames);
}

void AudioDecoder::AdvanceIndex(uint32_t index) {
    has_index_ = true;
    expected_index_ = index + 1;
}

bool AudioDecoder::Decode(const AudioChunk& chunk, const uint8_t* data, size_t size, std::vector<int16_t>& out) {
    std::array<int16_t, kAudioPacketFrames * 2> frame{};
    return Decode(
        chunk, data, size,
        [&]() { return frame.data(); },
        [&](const int16_t* pcm) { out.insert(out.end(), pcm, pcm + frame.size()); });
}

bool ParsePacket(const uint8_t* data, size_t size, Header& header, const uint8_t*& payload) {
    if (size < sizeof(Header)) return false;
    std::memcpy(&header, data, sizeof(Header));
    if (header.magic != kMagic || header.version != kVersion) return false;
    if (size_t(header.size) != size - sizeof(Header)) return false;
    payload = data + sizeof(Header);
    return true;
}

std::vector<uint8_t> BuildPacket(PacketType type, uint32_t seq, const void* payload, size_t size,
                                 const void* extra, size_t extra_size) {
    Header header{};
    header.type = uint8_t(type);
    header.size = uint16_t(size + extra_size);
    header.seq = seq;
    std::vector<uint8_t> out(sizeof(Header) + size + extra_size);
    std::memcpy(out.data(), &header, sizeof(Header));
    if (size) std::memcpy(out.data() + sizeof(Header), payload, size);
    if (extra_size) std::memcpy(out.data() + sizeof(Header) + size, extra, extra_size);
    return out;
}

uint16_t ChunkCount(size_t total) {
    return uint16_t((total + kMaxChunk - 1) / kMaxChunk);
}

bool FrameAssembler::Add(const VideoChunk& chunk, const uint8_t* data, size_t size, std::vector<uint8_t>& complete) {
    if (!chunk.count || chunk.index >= chunk.count || chunk.count != ChunkCount(chunk.total)) return false;
    const size_t offset = size_t(chunk.index) * kMaxChunk;
    const size_t expected = chunk.index + 1u == chunk.count ? chunk.total - offset : kMaxChunk;
    if (size != expected) return false;
    // Frame ids wrap; anything not newer than the last shown frame is a late duplicate.
    if (has_complete_ && int32_t(chunk.frame - last_complete_) <= 0) return false;
    if (!active_ || chunk.frame != frame_) {
        if (active_ && int32_t(chunk.frame - frame_) < 0) return false;
        if (active_) ++dropped_;
        active_ = true;
        frame_ = chunk.frame;
        total_ = chunk.total;
        count_ = chunk.count;
        received_ = 0;
        data_.assign(total_, 0);
        have_.assign(count_, false);
    }
    if (chunk.total != total_ || chunk.count != count_ || have_[chunk.index]) return false;
    std::memcpy(data_.data() + offset, data, size);
    have_[chunk.index] = true;
    if (++received_ != count_) return false;
    complete.swap(data_);
    active_ = false;
    has_complete_ = true;
    last_complete_ = frame_;
    return true;
}

} // namespace openshao::stream

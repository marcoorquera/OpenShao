#include "stream.h"

#include <opus.h>

#include <algorithm>
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

uint32_t VideoScale(uint32_t width, uint32_t height) {
    uint32_t scale = 1;
    while (width / scale > kVideoMaxWidth || height / scale > kVideoMaxHeight) ++scale;
    return scale;
}

namespace {

uint8_t Clamp8(int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }

} // namespace

void BgraToNv12(const uint8_t* bgra, uint32_t pitch, uint32_t width, uint32_t height, uint32_t scale,
                std::vector<uint8_t>& nv12, uint32_t& out_width, uint32_t& out_height) {
    if (!scale) scale = 1;
    out_width = (width / scale) & ~1u;
    out_height = (height / scale) & ~1u;
    nv12.resize(size_t(out_width) * out_height * 3 / 2);
    if (!out_width || !out_height) return;
    uint8_t* y_plane = nv12.data();
    uint8_t* uv_plane = y_plane + size_t(out_width) * out_height;
    const uint32_t area = scale * scale;
    for (uint32_t oy = 0; oy < out_height; oy += 2) {
        for (uint32_t ox = 0; ox < out_width; ox += 2) {
            int sum_r = 0, sum_g = 0, sum_b = 0;
            for (uint32_t dy = 0; dy < 2; ++dy) {
                for (uint32_t dx = 0; dx < 2; ++dx) {
                    uint32_t r = 0, g = 0, b = 0;
                    for (uint32_t sy = 0; sy < scale; ++sy) {
                        const uint8_t* row = bgra + size_t((oy + dy) * scale + sy) * pitch + size_t((ox + dx) * scale) * 4;
                        for (uint32_t sx = 0; sx < scale; ++sx) {
                            b += row[sx * 4];
                            g += row[sx * 4 + 1];
                            r += row[sx * 4 + 2];
                        }
                    }
                    const int ri = int(r / area), gi = int(g / area), bi = int(b / area);
                    y_plane[size_t(oy + dy) * out_width + ox + dx] = Clamp8(16 + ((47 * ri + 157 * gi + 16 * bi + 128) >> 8));
                    sum_r += ri;
                    sum_g += gi;
                    sum_b += bi;
                }
            }
            const int r = sum_r / 4, g = sum_g / 4, b = sum_b / 4;
            uint8_t* uv = uv_plane + size_t(oy / 2) * out_width + ox;
            uv[0] = Clamp8(128 + ((-26 * r - 87 * g + 112 * b + 128) >> 8));
            uv[1] = Clamp8(128 + ((112 * r - 102 * g - 10 * b + 128) >> 8));
        }
    }
}

void Nv12ToBgra(const uint8_t* y, const uint8_t* uv, uint32_t pitch, uint32_t width, uint32_t height,
                std::vector<uint8_t>& bgra) {
    bgra.resize(size_t(width) * height * 4);
    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t* yr = y + size_t(row) * pitch;
        const uint8_t* uvr = uv + size_t(row / 2) * pitch;
        uint8_t* out = bgra.data() + size_t(row) * width * 4;
        for (uint32_t col = 0; col < width; ++col) {
            const int c = 298 * (int(yr[col]) - 16);
            const int u = int(uvr[col & ~1u]) - 128;
            const int v = int(uvr[(col & ~1u) + 1]) - 128;
            out[col * 4] = Clamp8((c + 541 * u + 128) >> 8);
            out[col * 4 + 1] = Clamp8((c - 55 * u - 136 * v + 128) >> 8);
            out[col * 4 + 2] = Clamp8((c + 459 * v + 128) >> 8);
            out[col * 4 + 3] = 255;
        }
    }
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

void BuildParity(const uint8_t* data, size_t total, uint16_t count, uint8_t group, std::vector<uint8_t>& parity) {
    if (!data || !total || !count || !group) {
        parity.clear();
        return;
    }
    const uint16_t num_groups = uint16_t((count + group - 1) / group);
    parity.assign(size_t(num_groups) * kMaxChunk, 0);
    for (uint16_t g = 0; g < num_groups; ++g) {
        uint8_t* dst = parity.data() + size_t(g) * kMaxChunk;
        const size_t start = size_t(g) * group;
        const size_t end = std::min(size_t(start + group), size_t(count));
        for (size_t i = start; i < end; ++i) {
            const size_t offset = i * kMaxChunk;
            const size_t chunk_size = (offset < total) ? std::min(kMaxChunk, total - offset) : 0;
            const uint8_t* src = data + offset;
            for (size_t b = 0; b < chunk_size; ++b) {
                dst[b] ^= src[b];
            }
        }
    }
}

void FrameAssembler::TryRecover(uint8_t g) {
    if (!fec_group_ || g >= fec_count_ || !have_parity_[g]) return;
    const size_t start = size_t(g) * fec_group_;
    const size_t end = std::min(start + fec_group_, size_t(count_));
    size_t missing_count = 0;
    size_t missing_idx = 0;
    for (size_t i = start; i < end; ++i) {
        if (!have_[i]) {
            ++missing_count;
            missing_idx = i;
        }
    }
    if (missing_count != 1) return;

    std::array<uint8_t, kMaxChunk> rebuilt{};
    std::memcpy(rebuilt.data(), parity_data_.data() + size_t(g) * kMaxChunk, kMaxChunk);
    for (size_t i = start; i < end; ++i) {
        if (i == missing_idx) continue;
        const size_t offset_i = i * kMaxChunk;
        const size_t len_i = (i + 1 == count_) ? total_ - offset_i : kMaxChunk;
        const uint8_t* src = data_.data() + offset_i;
        for (size_t b = 0; b < len_i; ++b) {
            rebuilt[b] ^= src[b];
        }
    }
    const size_t missing_offset = missing_idx * kMaxChunk;
    const size_t missing_len = (missing_idx + 1 == count_) ? total_ - missing_offset : kMaxChunk;
    std::memcpy(data_.data() + missing_offset, rebuilt.data(), missing_len);
    have_[missing_idx] = true;
    ++received_;
    recovered_this_frame_ = true;
}

bool FrameAssembler::Add(const VideoChunk& chunk, const uint8_t* data, size_t size, std::vector<uint8_t>& complete) {
    if (!chunk.count || chunk.count != ChunkCount(chunk.total) || !data) return false;
    const uint16_t expected_fec = chunk.fec_group ? uint16_t((chunk.count + chunk.fec_group - 1) / chunk.fec_group) : 0;
    if (chunk.fec_count != expected_fec) return false;
    if (chunk.index >= chunk.count + chunk.fec_count) return false;

    const bool is_parity = chunk.index >= chunk.count;
    if (!is_parity) {
        const size_t offset = size_t(chunk.index) * kMaxChunk;
        const size_t expected = chunk.index + 1u == chunk.count ? chunk.total - offset : kMaxChunk;
        if (size != expected) return false;
    } else {
        if (size != kMaxChunk) return false;
    }

    // Frame ids wrap; anything not newer than the last shown frame is a late duplicate.
    if (has_complete_ && int32_t(chunk.frame - last_complete_) <= 0) return false;
    if (!active_ || chunk.frame != frame_) {
        if (active_ && int32_t(chunk.frame - frame_) < 0) return false;
        if (active_) ++dropped_;
        active_ = true;
        frame_ = chunk.frame;
        total_ = chunk.total;
        count_ = chunk.count;
        fec_group_ = chunk.fec_group;
        fec_count_ = chunk.fec_count;
        received_ = 0;
        recovered_this_frame_ = false;
        data_.assign(total_, 0);
        have_.assign(count_, false);
        have_parity_.assign(fec_count_, false);
        parity_data_.assign(size_t(fec_count_) * kMaxChunk, 0);
    }
    if (chunk.total != total_ || chunk.count != count_ ||
        chunk.fec_group != fec_group_ || chunk.fec_count != fec_count_) {
        return false;
    }
    if (!is_parity) {
        if (have_[chunk.index]) return false;
        const size_t offset = size_t(chunk.index) * kMaxChunk;
        std::memcpy(data_.data() + offset, data, size);
        have_[chunk.index] = true;
        ++received_;
        if (fec_group_ > 0) {
            TryRecover(uint8_t(chunk.index / fec_group_));
        }
    } else {
        const uint8_t g = uint8_t(chunk.index - count_);
        if (have_parity_[g]) return false;
        std::memcpy(parity_data_.data() + size_t(g) * kMaxChunk, data, kMaxChunk);
        have_parity_[g] = true;
        TryRecover(g);
    }
    if (received_ != count_) return false;
    complete.swap(data_);
    active_ = false;
    has_complete_ = true;
    last_complete_ = frame_;
    if (recovered_this_frame_) ++recovered_;
    return true;
}

BitrateController::BitrateController(uint32_t start)
    : bitrate_(std::clamp(start, kMinVideoBitrate, kMaxVideoBitrate)) {}

uint32_t BitrateController::Update(const StreamStats& s, uint32_t now_ms) {
    if (s.chunks_expected < 20) return bitrate_;

    const double loss = (s.chunks_received >= s.chunks_expected)
        ? 0.0
        : 1.0 - double(s.chunks_received) / double(s.chunks_expected);

    const bool lossy = (loss > 0.05 || s.frames_lost > 0);
    const bool clean = (loss < 0.01 && s.frames_lost == 0);

    if (lossy) {
        clean_active_ = false;
        if (!has_dropped_ || now_ms - last_drop_ms_ >= 1000) {
            bitrate_ = std::clamp(uint32_t(double(bitrate_) * 0.75), kMinVideoBitrate, kMaxVideoBitrate);
            last_drop_ms_ = now_ms;
            has_dropped_ = true;
        }
    } else if (!clean) {
        clean_active_ = false;
    } else {
        if (!clean_active_) {
            clean_active_ = true;
            clean_start_ms_ = now_ms;
        } else if (now_ms - clean_start_ms_ >= 3000) {
            if (!has_raised_ || now_ms - last_raise_ms_ >= 2000) {
                bitrate_ = std::clamp(uint32_t(double(bitrate_) * 1.10), kMinVideoBitrate, kMaxVideoBitrate);
                last_raise_ms_ = now_ms;
                has_raised_ = true;
            }
        }
    }
    return bitrate_;
}

} // namespace openshao::stream

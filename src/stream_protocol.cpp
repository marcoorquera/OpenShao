#include "stream.h"

#include <cstring>

namespace openshao::stream {

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

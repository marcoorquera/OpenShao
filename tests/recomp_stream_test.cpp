#include "stream.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

using namespace openshao::stream;

static void Check(bool ok, const char* description) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        std::exit(1);
    }
}

static std::vector<uint8_t> Encoded(size_t size, uint8_t seed) {
    std::vector<uint8_t> data(size);
    for (size_t i = 0; i < size; ++i) data[i] = uint8_t(seed + i * 31);
    return data;
}

static VideoChunk Chunk(uint32_t frame, const std::vector<uint8_t>& data, uint16_t index) {
    VideoChunk c{};
    c.frame = frame;
    c.width = 640;
    c.height = 480;
    c.total = uint32_t(data.size());
    c.count = ChunkCount(data.size());
    c.index = index;
    return c;
}

static size_t ChunkSize(const std::vector<uint8_t>& data, uint16_t index) {
    return std::min(kMaxChunk, data.size() - size_t(index) * kMaxChunk);
}

static bool Feed(FrameAssembler& a, uint32_t frame, const std::vector<uint8_t>& data, uint16_t index,
                 std::vector<uint8_t>& out) {
    return a.Add(Chunk(frame, data, index), data.data() + size_t(index) * kMaxChunk, ChunkSize(data, index), out);
}

int main() {
    // 1. Packet framing round trip and rejection of foreign or truncated datagrams
    RemotePad pad{};
    pad.buttons = 0x00012345u;
    pad.axes = {-32768, 32767, 100, -100, 0, 32767};
    auto packet = BuildPacket(PacketType::Input, 77, &pad, sizeof(pad));
    Header header{};
    const uint8_t* payload = nullptr;
    Check(ParsePacket(packet.data(), packet.size(), header, payload), "input packet parses");
    Check(PacketType(header.type) == PacketType::Input && header.seq == 77 && header.size == sizeof(RemotePad),
          "input packet header round trip");
    RemotePad parsed{};
    std::memcpy(&parsed, payload, sizeof(parsed));
    Check(parsed.buttons == pad.buttons && parsed.axes == pad.axes, "remote pad round trip");
    Check(!ParsePacket(packet.data(), packet.size() - 1, header, payload), "truncated packet is rejected");
    packet[0] ^= 0xFF;
    Check(!ParsePacket(packet.data(), packet.size(), header, payload), "foreign magic is rejected");

    // 2. Payload with a sub-header plus data, as the video and audio packets use
    const auto body = Encoded(kMaxChunk, 9);
    VideoChunk vc = Chunk(5, body, 0);
    packet = BuildPacket(PacketType::Video, 1, &vc, sizeof(vc), body.data(), body.size());
    Check(ParsePacket(packet.data(), packet.size(), header, payload) && header.size == sizeof(vc) + body.size() &&
          std::memcmp(payload + sizeof(vc), body.data(), body.size()) == 0,
          "video packet carries its chunk header and data");
    Check(packet.size() <= 1280 - 48, "video datagram fits a minimum IPv6 path with UDP and IP headers");

    // 3. Chunk counting
    Check(ChunkCount(1) == 1 && ChunkCount(kMaxChunk) == 1 && ChunkCount(kMaxChunk + 1) == 2, "chunk count rounding");

    // 4. Out-of-order reassembly
    {
        FrameAssembler a;
        const auto frame = Encoded(kMaxChunk * 3 + 17, 1);
        std::vector<uint8_t> out;
        Check(!Feed(a, 1, frame, 2, out) && !Feed(a, 1, frame, 0, out) && !Feed(a, 1, frame, 3, out),
              "incomplete frame is not delivered");
        Check(!Feed(a, 1, frame, 0, out), "duplicate chunk is ignored");
        Check(Feed(a, 1, frame, 1, out) && out == frame, "frame completes out of order");
        Check(!Feed(a, 1, frame, 1, out), "chunks of a delivered frame are ignored");
    }

    // 5. A newer frame replaces a frame that lost a chunk; older frames never come back
    {
        FrameAssembler a;
        const auto lost = Encoded(kMaxChunk * 2, 2), next = Encoded(kMaxChunk + 5, 3);
        std::vector<uint8_t> out;
        Check(!Feed(a, 10, lost, 0, out), "first chunk of a frame that loses its second");
        Check(!Feed(a, 11, next, 0, out) && Feed(a, 11, next, 1, out) && out == next, "newer frame is delivered");
        Check(a.Dropped() == 1, "the incomplete frame counts as dropped");
        Check(!Feed(a, 10, lost, 1, out), "late chunk of an older frame is ignored");
    }

    // 6. Frame ids wrap around
    {
        FrameAssembler a;
        const auto f1 = Encoded(100, 4), f2 = Encoded(200, 5);
        std::vector<uint8_t> out;
        Check(Feed(a, 0xFFFFFFFFu, f1, 0, out), "frame before the wrap");
        Check(Feed(a, 0, f2, 0, out) && out == f2, "frame after the wrap");
    }

    // 7. Malformed chunk descriptions
    {
        FrameAssembler a;
        const auto frame = Encoded(kMaxChunk * 2, 6);
        std::vector<uint8_t> out;
        VideoChunk bad = Chunk(1, frame, 0);
        bad.count = 5;
        Check(!a.Add(bad, frame.data(), kMaxChunk, out), "chunk count that disagrees with the size is rejected");
        bad = Chunk(1, frame, 2);
        Check(!a.Add(bad, frame.data(), kMaxChunk, out), "chunk index past the end is rejected");
        Check(!a.Add(Chunk(1, frame, 0), frame.data(), kMaxChunk - 1, out), "short chunk is rejected");
    }

    // 8. Opus encode/decode round trip with 1 kHz sine, single-packet loss (FEC) and burst loss (PLC cap)
    {
        constexpr uint32_t kTotalFrames = 50;
        constexpr uint32_t kTotalSamples = kTotalFrames * kAudioPacketFrames;
        std::vector<float> sine(size_t(kTotalSamples) * 2);
        for (uint32_t i = 0; i < kTotalSamples; ++i) {
            const float v = 0.5f * std::sin(2.0f * 3.14159265358979323846f * 1000.0f * float(i) / float(kAudioRate));
            sine[size_t(i) * 2] = v;
            sine[size_t(i) * 2 + 1] = -v;
        }

        AudioEncoder encoder;
        Check(!encoder.Submit(sine.data(), kAudioPacketFrames, 44100, [](const AudioChunk&, const uint8_t*, size_t) {}),
              "non-48 kHz audio rate is rejected");

        std::vector<std::pair<AudioChunk, std::vector<uint8_t>>> packets;
        // Feed in non-480 increments to verify accumulation across calls.
        uint32_t offset = 0;
        const uint32_t step = 300;
        while (offset < kTotalSamples) {
            const uint32_t count = std::min(step, kTotalSamples - offset);
            const bool ok = encoder.Submit(
                sine.data() + size_t(offset) * 2, count, kAudioRate,
                [&](const AudioChunk& chunk, const uint8_t* data, size_t size) {
                    packets.emplace_back(chunk, std::vector<uint8_t>(data, data + size));
                });
            Check(ok, "48 kHz Opus encoding succeeds");
            offset += count;
        }
        Check(packets.size() == kTotalFrames, "50 Opus packets are produced from 50x480 input frames");
        for (uint32_t i = 0; i < kTotalFrames; ++i) {
            const auto& chunk = packets[i].first;
            Check(chunk.rate == kAudioRate && chunk.frames == kAudioPacketFrames &&
                  chunk.codec == kAudioCodecOpus && chunk.index == i && !packets[i].second.empty(),
                  "Opus chunk metadata and index sequence");
        }

        AudioDecoder decoder;
        std::vector<int16_t> pcm;
        for (uint32_t i = 0; i < kTotalFrames; ++i) {
            if (i == 20) continue; // drop frame 20
            Check(decoder.Decode(packets[i].first, packets[i].second.data(), packets[i].second.size(), pcm),
                  "Opus packet decodes (with FEC on gap of 1)");
        }
        Check(pcm.size() == size_t(kTotalSamples) * 2, "output frame count matches 50 frames after 1-packet loss");

        auto frame_energy = [&](uint32_t frame_index) {
            uint64_t sum = 0;
            const size_t base = size_t(frame_index) * kAudioPacketFrames * 2;
            for (size_t i = 0; i < kAudioPacketFrames * 2; ++i) {
                const int64_t s = pcm[base + i];
                sum += uint64_t(s * s);
            }
            return sum;
        };
        Check(frame_energy(20) > 0, "concealed frame 20 has non-zero energy");
        uint64_t post_loss_energy = 0;
        for (uint32_t f = 21; f < kTotalFrames; ++f) post_loss_energy += frame_energy(f);
        Check(post_loss_energy > 0, "decoded audio after frame 20 loss has non-zero energy");

        // Larger gap: dropping 6 packets conceals at most 3 frames with PLC, then resyncs.
        decoder.Reset();
        pcm.clear();
        for (uint32_t i = 0; i < 10; ++i)
            Check(decoder.Decode(packets[i].first, packets[i].second.data(), packets[i].second.size(), pcm),
                  "initial packets before burst loss decode");
        Check(decoder.Decode(packets[16].first, packets[16].second.data(), packets[16].second.size(), pcm),
              "packet after 6-frame gap triggers capped PLC and resyncs");
        Check(pcm.size() == size_t(10 + kMaxPlcFrames + 1) * kAudioPacketFrames * 2,
              "burst loss conceals at most 3 PLC frames before resyncing");
    }

    std::puts("PASS: stream packet framing, chunking, frame reassembly and Opus audio");
    return 0;
}

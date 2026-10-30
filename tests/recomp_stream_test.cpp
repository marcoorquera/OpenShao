#include "stream.h"
#include "stream_invite.h"
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include <miniz.h>
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

    // 9. Video scale and BT.709 NV12 round trip
    {
        Check(VideoScale(640, 480) == 1 && VideoScale(1280, 960) == 1, "native guest sizes are not scaled");
        Check(VideoScale(2560, 1920) == 2 && VideoScale(1281, 960) == 2, "oversized captures are halved");
        uint32_t cw = 0, ch = 0;
        CaptureSize(640, 480, cw, ch);
        Check(cw == 640 && ch == 480, "a guest-sized frame is captured unchanged");
        CaptureSize(1920, 1440, cw, ch);
        Check(cw == 1280 && ch == 960, "a 3x render shrinks to the stream limit");
        CaptureSize(2560, 1440, cw, ch);
        Check(cw == 1280 && ch == 720, "a wide render keeps its aspect ratio");
        CaptureSize(1999, 1499, cw, ch);
        Check(cw <= kVideoMaxWidth && ch <= kVideoMaxHeight && cw % 2 == 0 && ch % 2 == 0 && VideoScale(cw, ch) == 1,
              "a shrunk capture is even and needs no further scaling");

        const uint8_t colors[][3] = {{0, 0, 0}, {255, 255, 255}, {200, 30, 40}, {20, 180, 60}, {30, 60, 220}, {128, 128, 128}};
        for (const auto& color : colors) {
            constexpr uint32_t w = 8, h = 6;
            std::vector<uint8_t> bgra(size_t(w) * h * 4);
            for (size_t i = 0; i < size_t(w) * h; ++i) {
                bgra[i * 4] = color[2];
                bgra[i * 4 + 1] = color[1];
                bgra[i * 4 + 2] = color[0];
                bgra[i * 4 + 3] = 255;
            }
            std::vector<uint8_t> nv12, back;
            uint32_t ow = 0, oh = 0;
            BgraToNv12(bgra.data(), w * 4, w, h, 1, nv12, ow, oh);
            Check(ow == w && oh == h && nv12.size() == size_t(w) * h * 3 / 2, "NV12 size matches an even frame");
            Nv12ToBgra(nv12.data(), nv12.data() + size_t(w) * h, w, w, h, back);
            for (size_t i = 0; i < size_t(w) * h; ++i)
                for (int c = 0; c < 3; ++c)
                    Check(std::abs(int(back[i * 4 + c]) - int(bgra[i * 4 + c])) <= 4, "BT.709 round trip stays within 4 levels");
        }

        std::vector<uint8_t> big(size_t(7) * 5 * 4, 255), nv12;
        uint32_t ow = 0, oh = 0;
        BgraToNv12(big.data(), 7 * 4, 7, 5, 1, nv12, ow, oh);
        Check(ow == 6 && oh == 4, "odd capture sizes round down to even");
        BgraToNv12(big.data(), 7 * 4, 7, 5, 2, nv12, ow, oh);
        Check(ow == 2 && oh == 2, "scaled capture sizes round down to even");
    }

    // 10. Video FEC parity round trip, drop limits, short last chunk, and out-of-order delivery
    {
        auto chunk_fec = [](uint32_t frame, const std::vector<uint8_t>& data, uint16_t index, uint8_t group) {
            VideoChunk c{};
            c.frame = frame;
            c.width = 640;
            c.height = 480;
            c.total = uint32_t(data.size());
            c.count = ChunkCount(data.size());
            c.fec_group = group;
            c.fec_count = group ? uint8_t((c.count + group - 1) / group) : 0;
            c.index = index;
            return c;
        };

        auto feed_fec = [&](FrameAssembler& a, uint32_t frame, const std::vector<uint8_t>& data,
                            const std::vector<uint8_t>& parity, uint16_t index, uint8_t group,
                            std::vector<uint8_t>& out) {
            VideoChunk c = chunk_fec(frame, data, index, group);
            if (index < c.count) {
                return a.Add(c, data.data() + size_t(index) * kMaxChunk, ChunkSize(data, index), out);
            } else {
                const size_t g = index - c.count;
                return a.Add(c, parity.data() + g * kMaxChunk, kMaxChunk, out);
            }
        };

        // 10a. 20-chunk frame with group 8: drop one data chunk per group -> completes byte-identical and Recovered() counts it
        {
            const auto frame = Encoded(kMaxChunk * 19 + 500, 10);
            Check(ChunkCount(frame.size()) == 20, "frame has 20 chunks");
            std::vector<uint8_t> parity;
            BuildParity(frame.data(), frame.size(), 20, 8, parity);
            Check(parity.size() == 3 * kMaxChunk, "3 parity chunks produced for 20 chunks with group 8");

            FrameAssembler a;
            std::vector<uint8_t> out;
            // Group 0 (0..7): drop 3. Group 1 (8..15): drop 11. Group 2 (16..19): drop 18.
            for (uint16_t i = 0; i < 20; ++i) {
                if (i == 3 || i == 11 || i == 18) continue;
                Check(!feed_fec(a, 1, frame, parity, i, 8, out), "incomplete frame while data chunks arrive");
            }
            Check(!feed_fec(a, 1, frame, parity, 20, 8, out), "incomplete after parity 0");
            Check(!feed_fec(a, 1, frame, parity, 21, 8, out), "incomplete after parity 1");
            Check(feed_fec(a, 1, frame, parity, 22, 8, out) && out == frame,
                  "20-chunk frame completes byte-identical with 1 dropped chunk per group");
            Check(a.Recovered() == 1, "frame completed thanks to parity increments Recovered()");
        }

        // 10b. Drop two data chunks in one group -> does not complete
        {
            const auto frame = Encoded(kMaxChunk * 19 + 500, 11);
            std::vector<uint8_t> parity;
            BuildParity(frame.data(), frame.size(), 20, 8, parity);
            FrameAssembler a;
            std::vector<uint8_t> out;
            // Drop chunks 2 and 3 in group 0
            for (uint16_t i = 0; i < 20; ++i) {
                if (i == 2 || i == 3) continue;
                Check(!feed_fec(a, 1, frame, parity, i, 8, out), "two-drop data chunk does not complete");
            }
            for (uint16_t p = 20; p < 23; ++p) {
                Check(!feed_fec(a, 1, frame, parity, p, 8, out), "parity cannot rebuild two missing chunks in one group");
            }
            Check(out.empty() && a.Recovered() == 0, "frame with two drops in one group never completes");
        }

        // 10c. Last-chunk recovery: drop the short last chunk -> rebuilt with the right length
        {
            const auto frame = Encoded(kMaxChunk * 3 + 321, 12);
            Check(ChunkCount(frame.size()) == 4, "frame has 4 chunks");
            std::vector<uint8_t> parity;
            BuildParity(frame.data(), frame.size(), 4, 8, parity);
            Check(parity.size() == kMaxChunk, "1 parity chunk for 4 chunks with group 8");

            FrameAssembler a;
            std::vector<uint8_t> out;
            for (uint16_t i = 0; i < 3; ++i) {
                Check(!feed_fec(a, 1, frame, parity, i, 8, out), "early data chunks do not complete");
            }
            // Drop short last chunk (index 3), feed parity chunk (index 4)
            Check(feed_fec(a, 1, frame, parity, 4, 8, out), "parity recovers short last chunk");
            Check(out.size() == frame.size() && out == frame, "rebuilt last chunk has correct truncated length");
            Check(a.Recovered() == 1, "short last chunk recovery counted");
        }

        // 10d. Parity chunks before data chunks arrive in any order
        {
            const auto frame = Encoded(kMaxChunk * 9 + 100, 13);
            Check(ChunkCount(frame.size()) == 10, "frame has 10 chunks");
            std::vector<uint8_t> parity;
            BuildParity(frame.data(), frame.size(), 10, 8, parity);
            Check(parity.size() == 2 * kMaxChunk, "2 parity chunks for 10 chunks with group 8");

            FrameAssembler a;
            std::vector<uint8_t> out;
            // Drop chunk 5 in group 0. Send parity chunks first, then data in scrambled order.
            Check(!feed_fec(a, 1, frame, parity, 11, 8, out), "parity 1 arrives first");
            Check(!feed_fec(a, 1, frame, parity, 10, 8, out), "parity 0 arrives second");
            const uint16_t arrival_order[] = {9, 8, 7, 6, 4, 3, 2, 1, 0};
            for (size_t i = 0; i < sizeof(arrival_order) / sizeof(arrival_order[0]); ++i) {
                const bool complete = feed_fec(a, 1, frame, parity, arrival_order[i], 8, out);
                if (i + 1 == sizeof(arrival_order) / sizeof(arrival_order[0])) {
                    Check(complete && out == frame, "scrambled arrival with parity first completes byte-identical");
                } else {
                    Check(!complete, "incomplete during scrambled arrival");
                }
            }
            Check(a.Recovered() == 1, "out of order parity recovery counted");
        }
    }

    // 11. BitrateController rate adaptation, clamps, limits, and small report handling
    {
        constexpr uint32_t start = 8'000'000;
        static_assert(start <= kMaxVideoBitrate);
        BitrateController bc(start);
        Check(bc.Bitrate() == start, "initial bitrate matches start");

        // Small reports (< 20 chunks expected) are ignored
        StreamStats small{};
        small.packets_expected = 19;
        small.packets_received = 0;
        small.frames_lost = 5;
        bc.Update(small, 1000);
        Check(bc.Bitrate() == start, "reports with packets_expected < 20 are ignored");

        // Loss > 5% drops bitrate by 25%, rate-limited to once per 1000 ms
        StreamStats lossy{};
        lossy.packets_expected = 100;
        lossy.packets_received = 90; // 10% loss
        lossy.frames_lost = 0;
        bc.Update(lossy, 1000);
        Check(bc.Bitrate() == 6'000'000, "10% loss drops 8 Mbps to 6 Mbps");
        bc.Update(lossy, 1500);
        Check(bc.Bitrate() == 6'000'000, "drop is rate-limited to once per 1000 ms");
        bc.Update(lossy, 2000);
        Check(bc.Bitrate() == 4'500'000, "drop after 1000 ms decreases rate to 4.5 Mbps");

        // frames_lost > 0 triggers drop even with 0% chunk loss
        StreamStats lost_frame{};
        lost_frame.packets_expected = 100;
        lost_frame.packets_received = 100;
        lost_frame.frames_lost = 1;
        bc.Update(lost_frame, 3000);
        Check(bc.Bitrate() == 3'375'000, "frames_lost > 0 drops bitrate");

        // Lower clamp holds at kMinVideoBitrate (2,000,000)
        bc.Update(lossy, 4000); // 3375000 * 0.75 = 2531250
        Check(bc.Bitrate() == 2'531'250, "drop to 2.53 Mbps");
        bc.Update(lossy, 5000); // 2531250 * 0.75 = 1898437 -> clamp 2000000
        Check(bc.Bitrate() == kMinVideoBitrate, "bitrate clamps to kMinVideoBitrate");
        bc.Update(lossy, 6000);
        Check(bc.Bitrate() == kMinVideoBitrate, "further loss stays clamped at min");

        // Clean reports (< 1% loss, no frames lost) raise rate after 3000 ms in a row, at most once per 2000 ms
        StreamStats clean{};
        clean.packets_expected = 100;
        clean.packets_received = 100;
        clean.frames_lost = 0;
        bc.Update(clean, 10000); // streak starts at 10000
        Check(bc.Bitrate() == kMinVideoBitrate, "clean report before 3000 ms does not raise rate");
        bc.Update(clean, 12000); // 2000 ms clean
        Check(bc.Bitrate() == kMinVideoBitrate, "clean report at 2000 ms does not raise rate");
        bc.Update(clean, 13000); // 3000 ms clean -> raise 2000000 * 1.10 = 2200000
        Check(bc.Bitrate() == 2'200'000, "clean reports for 3000 ms raise rate by 10%");
        bc.Update(clean, 14000); // 1000 ms since raise (< 2000 ms limit)
        Check(bc.Bitrate() == 2'200'000, "raise is rate-limited to once per 2000 ms");
        bc.Update(clean, 15000); // 2000 ms since raise -> raise 2200000 * 1.10 = 2420000
        Check(bc.Bitrate() == 2'420'000, "raise after 2000 ms increases rate again");

        // Broken clean streak resets 3000 ms requirement
        StreamStats mild_loss{};
        mild_loss.packets_expected = 100;
        mild_loss.packets_received = 98; // 2% loss: between 1% and 5% -> not clean, breaks streak
        mild_loss.frames_lost = 0;
        bc.Update(mild_loss, 16000);
        Check(bc.Bitrate() == 2'420'000, "mild loss does not drop rate");
        bc.Update(clean, 17000); // new streak starts at 17000
        bc.Update(clean, 19000); // 2000 ms clean
        Check(bc.Bitrate() == 2'420'000, "broken streak must wait full 3000 ms before raising");
        bc.Update(clean, 20000); // 3000 ms clean -> raise 2420000 * 1.10 = 2662000
        Check(bc.Bitrate() == 2'662'000, "clean for 3000 ms after break raises rate");

        // Upper clamp holds at kMaxVideoBitrate
        uint32_t t = 22000;
        for (int i = 0; i < 20; ++i) {
            bc.Update(clean, t);
            t += 2000;
        }
        Check(bc.Bitrate() == kMaxVideoBitrate, "bitrate clamps to kMaxVideoBitrate");
    }

    // 12. SequenceCounter loss accounting
    {
        // A report sent after a large frame's data but before its parity must show no loss.
        SequenceCounter sequence;
        StreamStats first{};
        for (uint32_t seq = 1000; seq < 1300; ++seq) sequence.Add(seq, first);
        Check(first.packets_expected == 300 && first.packets_received == 300,
              "packets still in flight are not expected yet");
        StreamStats second{};
        for (uint32_t seq = 1300; seq < 1338; ++seq) sequence.Add(seq, second);
        Check(second.packets_expected == 38 && second.packets_received == 38,
              "parity after a report is counted in the next report");

        // Gaps count as loss; a reordered packet only adds to received.
        StreamStats gaps{};
        sequence.Add(1340, gaps);
        sequence.Add(1339, gaps);
        sequence.Add(1345, gaps);
        // 1338 and 1341-1344 are missing
        Check(gaps.packets_expected == 8 && gaps.packets_received == 3, "skipped sequence numbers count as lost");

        // Sequence numbers wrap.
        SequenceCounter wrap;
        StreamStats wrapped{};
        wrap.Add(0xFFFFFFFEu, wrapped);
        wrap.Add(0xFFFFFFFFu, wrapped);
        wrap.Add(0u, wrapped);
        wrap.Add(1u, wrapped);
        Check(wrapped.packets_expected == 4 && wrapped.packets_received == 4, "sequence wrap is not loss");

        // A restart is not loss.
        StreamStats restart{};
        wrap.Add(5'000'000u, restart);
        Check(restart.packets_expected == 1 && restart.packets_received == 1, "a large jump restarts the count");
        wrap.Reset();
        StreamStats reset{};
        wrap.Add(7u, reset);
        Check(reset.packets_expected == 1 && reset.packets_received == 1, "Reset starts a new count");
    }

    // 12a. PackNv12 drops row padding and keeps the chroma rows
    {
        const uint32_t w = 4, h = 4, pitch = 6;
        std::vector<uint8_t> y(pitch * h), uv(pitch * h / 2), packed;
        for (size_t i = 0; i < y.size(); ++i) y[i] = uint8_t(i);
        for (size_t i = 0; i < uv.size(); ++i) uv[i] = uint8_t(100 + i);
        PackNv12(y.data(), uv.data(), pitch, w, h, packed);
        Check(packed.size() == w * h * 3 / 2, "packed NV12 has no padding");
        Check(packed[0] == 0 && packed[3] == 3 && packed[4] == 6 && packed[15] == 21, "luma rows skip the pitch padding");
        Check(packed[16] == 100 && packed[19] == 103 && packed[20] == 106 && packed[23] == 109, "chroma rows skip the pitch padding");
    }

    // 12b. PingFilter smoothing
    {
        PingFilter ping;
        Check(!ping.Started(), "ping starts without a value");
        Check(ping.Add(80) == 80, "first ping sample is shown directly");
        Check(ping.Add(200) == 110, "one late reply moves the ping by a quarter");
        for (int i = 0; i < 40; ++i) ping.Add(40);
        Check(ping.Value() == 40, "steady samples converge");
        ping.Reset();
        Check(!ping.Started() && ping.Add(15) == 15, "Reset forgets the previous ping");
    }

    // 13. Connection codes (Invite & Reply), whitespace tolerance and error handling
    {
        ConnectionCode invite{};
        invite.kind = CodeKind::Invite;
        invite.session = 0x12345678u;
        invite.description =
            "a=ice-ufrag:abcd1234\r\n"
            "a=ice-pwd:xyz987654321xyz987654321\r\n"
            "a=ice-options:trickle\r\n"
            "a=candidate:1 1 UDP 2130706431 192.168.1.50 50000 typ host\r\n"
            "a=candidate:2 1 UDP 1694498815 203.0.113.5 50000 typ srflx raddr 192.168.1.50 rport 50000\r\n"
            "a=end-of-candidates\r\n";

        std::string encoded_invite = EncodeCode(invite);
        Check(!encoded_invite.empty(), "invite encodes successfully");
        Check(encoded_invite.size() < 400, "typical invite code stays under 400 characters");
        std::printf("Typical invite code length: %zu characters\n", encoded_invite.size());

        // Round trip of invite
        ConnectionCode decoded_invite{};
        std::string error;
        Check(DecodeCode(encoded_invite, CodeKind::Invite, decoded_invite, error), "invite decodes successfully");
        Check(decoded_invite.kind == CodeKind::Invite, "decoded invite kind matches");
        Check(decoded_invite.session == invite.session, "decoded invite session matches");
        Check(decoded_invite.description.find("a=ice-options") == std::string::npos,
              "dropped line (a=ice-options) is gone after decoding");
        Check(decoded_invite.description.find("a=ice-ufrag:abcd1234") != std::string::npos, "ufrag preserved");
        Check(decoded_invite.description.find("a=ice-pwd:xyz987654321xyz987654321") != std::string::npos,
              "pwd preserved");
        Check(decoded_invite.description.find("a=end-of-candidates") != std::string::npos, "end marker preserved");

        // Round trip of reply
        ConnectionCode reply{};
        reply.kind = CodeKind::Reply;
        reply.session = invite.session;
        reply.description = invite.description;
        std::string encoded_reply = EncodeCode(reply);
        Check(!encoded_reply.empty(), "reply encodes successfully");
        ConnectionCode decoded_reply{};
        Check(DecodeCode(encoded_reply, CodeKind::Reply, decoded_reply, error), "reply decodes successfully");
        Check(decoded_reply.kind == CodeKind::Reply, "decoded reply kind matches");
        Check(decoded_reply.session == reply.session, "decoded reply session matches");
        Check(decoded_reply.description == decoded_invite.description,
              "decoded reply description matches decoded invite");

        // Whitespace and line breaks inserted into the code are ignored
        std::string spaced_code;
        for (size_t i = 0; i < encoded_invite.size(); ++i) {
            spaced_code.push_back(encoded_invite[i]);
            if (i % 15 == 0) {
                spaced_code.append("\r\n  \t ");
            }
        }
        ConnectionCode decoded_spaced{};
        Check(DecodeCode(spaced_code, CodeKind::Invite, decoded_spaced, error),
              "code with whitespace and line breaks decodes successfully");
        Check(decoded_spaced.session == invite.session && decoded_spaced.description == decoded_invite.description,
              "whitespace content matches");

        // Wrong kind: paste reply into invite
        ConnectionCode bad_code{};
        Check(!DecodeCode(encoded_reply, CodeKind::Invite, bad_code, error),
              "reply code rejected when expecting invite");
        Check(error == "This is a reply code. Paste it on the host.", "wrong kind message for reply -> invite");

        // Wrong kind: paste invite into reply
        Check(!DecodeCode(encoded_invite, CodeKind::Reply, bad_code, error),
              "invite code rejected when expecting reply");
        Check(error == "This is an invite code. Paste it in Join.", "wrong kind message for invite -> reply");

        // Wrong version digit
        std::string wrong_ver_digit = encoded_invite;
        wrong_ver_digit[3] = '9';
        Check(!DecodeCode(wrong_ver_digit, CodeKind::Invite, bad_code, error), "wrong version digit rejected");
        Check(error == "This code comes from a different OpenShao version. Both players need the same version.",
              "wrong version digit message");

        // Wrong kVersion in body
        {
            std::string desc = "a=ice-ufrag:abcd1234\r\na=ice-pwd:xyz\r\n";
            std::vector<uint8_t> body(6 + desc.size());
            uint16_t wrong_kver = 999;
            body[0] = uint8_t(wrong_kver & 0xFF);
            body[1] = uint8_t((wrong_kver >> 8) & 0xFF);
            body[2] = 0x78; body[3] = 0x56; body[4] = 0x34; body[5] = 0x12;
            std::memcpy(body.data() + 6, desc.data(), desc.size());
            mz_ulong dlen = mz_compressBound(static_cast<mz_ulong>(body.size()));
            std::vector<uint8_t> comp(dlen);
            mz_compress2(comp.data(), &dlen, body.data(), static_cast<mz_ulong>(body.size()), 9);
            comp.resize(dlen);

            // Base64url encode manually
            const char b64c[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
            std::string b64;
            size_t idx = 0;
            while (idx + 3 <= comp.size()) {
                uint32_t val = (uint32_t(comp[idx]) << 16) | (uint32_t(comp[idx + 1]) << 8) | uint32_t(comp[idx + 2]);
                b64.push_back(b64c[(val >> 18) & 0x3F]);
                b64.push_back(b64c[(val >> 12) & 0x3F]);
                b64.push_back(b64c[(val >> 6) & 0x3F]);
                b64.push_back(b64c[val & 0x3F]);
                idx += 3;
            }
            if (idx + 1 == comp.size()) {
                uint32_t val = uint32_t(comp[idx]) << 16;
                b64.push_back(b64c[(val >> 18) & 0x3F]);
                b64.push_back(b64c[(val >> 12) & 0x3F]);
            } else if (idx + 2 == comp.size()) {
                uint32_t val = (uint32_t(comp[idx]) << 16) | (uint32_t(comp[idx + 1]) << 8);
                b64.push_back(b64c[(val >> 18) & 0x3F]);
                b64.push_back(b64c[(val >> 12) & 0x3F]);
                b64.push_back(b64c[(val >> 6) & 0x3F]);
            }
            std::string bad_ver_code = "OSI1-" + b64;
            Check(!DecodeCode(bad_ver_code, CodeKind::Invite, bad_code, error),
                  "wrong body kVersion rejected");
            Check(error == "This code comes from a different OpenShao version. Both players need the same version.",
                  "wrong body kVersion message");
        }

        // Truncated text
        std::string truncated = encoded_invite.substr(0, encoded_invite.size() / 2);
        Check(!DecodeCode(truncated, CodeKind::Invite, bad_code, error), "truncated code rejected");
        Check(error == "The connection code is damaged or incomplete.", "truncated code error message");

        // Invalid base64 characters
        std::string invalid_b64 = encoded_invite;
        invalid_b64.back() = '!';
        Check(!DecodeCode(invalid_b64, CodeKind::Invite, bad_code, error), "invalid base64 characters rejected");
        Check(error == "The connection code is damaged or incomplete.", "invalid base64 characters error message");

        // Non-OpenShao string
        Check(!DecodeCode("https://example.com/not-a-code", CodeKind::Invite, bad_code, error),
              "non-OpenShao string rejected");
        Check(error == "Not an OpenShao connection code.", "non-OpenShao string error message");
    }

    std::puts("PASS: stream packet framing, chunking, frame reassembly, Opus audio, NV12 conversion and connection codes");
    return 0;
}

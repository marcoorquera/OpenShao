#include "stream_ice.h"
#include "stream_invite.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

using namespace openshao::stream;

static void Check(bool ok, const char* description) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        std::exit(1);
    }
}

int main() {
    IceLink link1;
    IceLink link2;

    std::string err1, err2;
    // use_stun = false: host candidates only, no internet access
    Check(link1.Start(err1, false), "link1 starts without STUN");
    Check(link2.Start(err2, false), "link2 starts without STUN");

    // Start returns while gathering; the launcher polls the state the same way.
    auto ready_by = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while ((link1.State() != IceState::Ready || link2.State() != IceState::Ready) &&
           std::chrono::steady_clock::now() < ready_by)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Check(link1.State() == IceState::Ready, "link1 state is Ready");
    Check(link2.State() == IceState::Ready, "link2 state is Ready");

    std::string desc1 = link1.LocalDescription();
    std::string desc2 = link2.LocalDescription();
    Check(!desc1.empty(), "link1 has non-empty local description");
    Check(!desc2.empty(), "link2 has non-empty local description");

    // Exchange via connection codes
    ConnectionCode invite_code{CodeKind::Invite, 0xABCDEFu, desc1};
    std::string invite_str = EncodeCode(invite_code);
    Check(!invite_str.empty(), "invite code encodes successfully");

    ConnectionCode decoded_invite{};
    Check(DecodeCode(invite_str, CodeKind::Invite, decoded_invite, err2), "join decodes invite code");
    Check(link2.SetRemote(decoded_invite.description, err2), "link2 sets remote description");
    Check(link2.State() == IceState::Connecting, "link2 state is Connecting");

    ConnectionCode reply_code{CodeKind::Reply, decoded_invite.session, desc2};
    std::string reply_str = EncodeCode(reply_code);
    Check(!reply_str.empty(), "reply code encodes successfully");

    ConnectionCode decoded_reply{};
    Check(DecodeCode(reply_str, CodeKind::Reply, decoded_reply, err1), "host decodes reply code");
    Check(link1.SetRemote(decoded_reply.description, err1), "link1 sets remote description");
    Check(link1.State() == IceState::Connecting, "link1 state is Connecting");

    // Wait up to 10 seconds for both agents to connect over host loopback
    auto start = std::chrono::steady_clock::now();
    while ((link1.State() != IceState::Connected || link2.State() != IceState::Connected) &&
           std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count() < 10) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    Check(link1.State() == IceState::Connected, "link1 connected");
    Check(link2.State() == IceState::Connected, "link2 connected");

    std::string route1 = link1.Route();
    std::string route2 = link2.Route();
    Check(!route1.empty(), "link1 route is non-empty");
    Check(!route2.empty(), "link2 route is non-empty");
    std::printf("link1 route: %s\n", route1.c_str());
    std::printf("link2 route: %s\n", route2.c_str());

    // Send datagram from link1 to link2
    const char msg1[] = "IceLink datagram 1 -> 2 test payload";
    Check(link1.Send(msg1, sizeof(msg1)), "link1 sends datagram");

    uint8_t rx_buf[256] = {0};
    int rx_len = link2.Receive(rx_buf, sizeof(rx_buf), 3000);
    Check(rx_len == sizeof(msg1), "link2 receives correct datagram length");
    Check(std::memcmp(rx_buf, msg1, sizeof(msg1)) == 0, "link2 receives matching payload");

    // Send datagram from link2 to link1
    const char msg2[] = "IceLink datagram 2 -> 1 reply payload";
    Check(link2.Send(msg2, sizeof(msg2)), "link2 sends datagram");

    std::memset(rx_buf, 0, sizeof(rx_buf));
    rx_len = link1.Receive(rx_buf, sizeof(rx_buf), 3000);
    Check(rx_len == sizeof(msg2), "link1 receives correct datagram length");
    Check(std::memcmp(rx_buf, msg2, sizeof(msg2)) == 0, "link1 receives matching payload");

    // Receive timeout returns 0
    rx_len = link1.Receive(rx_buf, sizeof(rx_buf), 50);
    Check(rx_len == 0, "receive returns 0 on timeout");

    // Both directions at once while another thread queries the links: a lock-order bug between
    // libjuice's callbacks and the API calls would hang here.
    {
        constexpr int kPackets = 3000;
        std::atomic<int> got1{0}, got2{0};
        std::atomic<bool> querying{true};
        auto send = [](IceLink& link) {
            std::vector<uint8_t> packet(1200, 0x5A);
            for (int i = 0; i < kPackets; ++i) {
                link.Send(packet.data(), packet.size());
                if (i % 64 == 63) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        };
        auto receive = [](IceLink& link, std::atomic<int>& got) {
            uint8_t buffer[2048];
            while (got < kPackets && link.Receive(buffer, sizeof(buffer), 500) > 0) ++got;
        };
        std::thread query([&] {
            while (querying) {
                (void)link1.Route();
                (void)link2.State();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        std::thread r1(receive, std::ref(link1), std::ref(got1));
        std::thread r2(receive, std::ref(link2), std::ref(got2));
        std::thread s1(send, std::ref(link1));
        std::thread s2(send, std::ref(link2));
        s1.join();
        s2.join();
        r1.join();
        r2.join();
        querying = false;
        query.join();
        std::printf("stress: link1 received %d, link2 received %d of %d\n", got1.load(), got2.load(), kPackets);
        Check(got1 >= kPackets * 9 / 10 && got2 >= kPackets * 9 / 10, "concurrent traffic arrives both ways");
    }

    // Stop() while another thread is still sending must return.
    {
        std::atomic<bool> sending{true};
        std::thread flood([&] {
            const uint8_t packet[1200] = {};
            while (sending) link2.Send(packet, sizeof(packet));
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        link1.Stop();
        sending = false;
        flood.join();
    }

    // Stop() causes Receive to return -1 and Send to return false
    Check(link1.Receive(rx_buf, sizeof(rx_buf), 50) == -1, "Receive returns -1 after Stop");
    Check(!link1.Send("fail", 4), "Send returns false after Stop");

    link2.Stop();
    Check(link2.Receive(rx_buf, sizeof(rx_buf), 50) == -1, "Receive returns -1 after Stop");
    Check(!link2.Send("fail", 4), "Send returns false after Stop");

    // Player 2 sets the invite while its own link is still gathering; the link applies it later.
    {
        IceLink host, guest;
        std::string error;
        Check(host.Start(error, false), "late host starts");
        auto by = std::chrono::steady_clock::now() + std::chrono::seconds(6);
        while (host.State() != IceState::Ready && std::chrono::steady_clock::now() < by)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        Check(host.State() == IceState::Ready, "late host is Ready");
        Check(guest.Start(error, false), "late guest starts");
        Check(guest.SetRemote(host.LocalDescription(), error), "guest accepts the invite while gathering");
        Check(!guest.SetRemote(host.LocalDescription(), error), "a second remote description is refused");
        by = std::chrono::steady_clock::now() + std::chrono::seconds(6);
        while (guest.LocalDescription().empty() && std::chrono::steady_clock::now() < by)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        Check(host.SetRemote(guest.LocalDescription(), error), "host accepts the reply");
        by = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while ((host.State() != IceState::Connected || guest.State() != IceState::Connected) &&
               std::chrono::steady_clock::now() < by)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        Check(host.State() == IceState::Connected && guest.State() == IceState::Connected,
              "a remote description set while gathering connects");
    }

    // Cancelling right after Start must return without waiting for gathering.
    {
        IceLink cancelled;
        std::string error;
        Check(cancelled.Start(error, false), "cancelled link starts");
        const auto before = std::chrono::steady_clock::now();
        cancelled.Stop();
        Check(cancelled.State() == IceState::Idle, "a stopped link is Idle");
        Check(std::chrono::steady_clock::now() - before < std::chrono::seconds(2), "Stop during gathering is quick");
    }

    std::puts("PASS: recomp_stream_ice");
    return 0;
}

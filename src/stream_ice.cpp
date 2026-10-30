#include "stream_ice.h"

#ifndef _WIN32

namespace openshao::stream {

struct IceLink::Impl {};

IceLink::IceLink() = default;
IceLink::~IceLink() = default;

bool IceLink::Start(std::string& error, bool) {
    error = "Internet play is available on Windows";
    return false;
}

void IceLink::Stop() {}

IceState IceLink::State() const { return IceState::Idle; }

std::string IceLink::LocalDescription() const { return {}; }

bool IceLink::SetRemote(const std::string&, std::string& error) {
    error = "Internet play is available on Windows";
    return false;
}

bool IceLink::Send(const void*, size_t) { return false; }

int IceLink::Receive(uint8_t*, size_t, uint32_t) { return -1; }

std::string IceLink::Route() const { return {}; }

} // namespace openshao::stream

#else

#include "core/log.h"
#include <juice/juice.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace openshao::stream {

namespace {

constexpr const char* kStunHost = "stun.l.google.com";
constexpr uint16_t kStunPort = 19302;
constexpr size_t kMaxQueued = 4096;
constexpr auto kGatherTimeout = std::chrono::seconds(5);
constexpr auto kRetryDelay = std::chrono::milliseconds(500);

void JuiceLog(juice_log_level_t level, const char* message) {
    if (level >= JUICE_LOG_LEVEL_ERROR) LOGE("stream: ice: %s", message);
    else if (level == JUICE_LOG_LEVEL_WARN) LOGW("stream: ice: %s", message);
}

void InitJuiceLogging() {
    static std::once_flag once;
    std::call_once(once, [] {
        juice_set_log_level(JUICE_LOG_LEVEL_WARN);
        juice_set_log_handler(JuiceLog);
    });
}

const char* StateName(IceState s) {
    switch (s) {
        case IceState::Idle: return "idle";
        case IceState::Gathering: return "gathering";
        case IceState::Ready: return "ready";
        case IceState::Connecting: return "connecting";
        case IceState::Connected: return "connected";
        case IceState::Failed: return "failed";
    }
    return "unknown";
}

} // namespace

// Lock order: libjuice runs its callbacks on its own thread while holding the agent's internal lock.
// Callbacks therefore only take signal_mutex or queue_mutex and never call libjuice. Every libjuice
// call, juice_destroy included, happens under agent_mutex, which no callback takes, so a callback can
// always finish and a destroy never waits on a lock its own agent thread needs.
struct IceLink::Impl {
    std::mutex agent_mutex;
    juice_agent_t* agent = nullptr;               // guarded by agent_mutex
    std::atomic<juice_agent_t*> current{nullptr}; // the agent whose callbacks still count

    std::mutex signal_mutex;
    std::condition_variable signal_cv;
    std::atomic<IceState> state{IceState::Idle};  // written under signal_mutex
    bool stopped = true;
    bool gathered = false;
    bool failed_event = false;
    bool connected_event = false;
    std::string remote;                           // set once by SetRemote
    std::string local;                            // the description the codes carry

    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::deque<std::vector<uint8_t>> queue;
    bool queue_open = false;

    std::thread worker;
    bool use_stun = true;
    uint16_t port = 0;
    std::string ufrag, pwd;

    // Caller holds signal_mutex.
    void SetState(IceState next) {
        if (state.exchange(next) != next) LOGI("stream: ice: %s", StateName(next));
    }

    static void OnState(juice_agent_t* agent, juice_state_t s, void* user) {
        auto* self = static_cast<Impl*>(user);
        if (agent != self->current.load()) return;
        std::lock_guard guard(self->signal_mutex);
        if (self->stopped) return;
        if (s == JUICE_STATE_CONNECTED || s == JUICE_STATE_COMPLETED) {
            if (self->state.load() != IceState::Connected) {
                self->SetState(IceState::Connected);
                self->connected_event = true;
                self->signal_cv.notify_all();
            }
        } else if (s == JUICE_STATE_FAILED && !self->remote.empty()) {
            self->SetState(IceState::Connecting);
            self->failed_event = true;
            self->signal_cv.notify_all();
        }
    }

    static void OnGathered(juice_agent_t* agent, void* user) {
        auto* self = static_cast<Impl*>(user);
        if (agent != self->current.load()) return;
        std::lock_guard guard(self->signal_mutex);
        self->gathered = true;
        self->signal_cv.notify_all();
    }

    static void OnRecv(juice_agent_t* agent, const char* data, size_t size, void* user) {
        auto* self = static_cast<Impl*>(user);
        if (agent != self->current.load()) return;
        std::lock_guard guard(self->queue_mutex);
        if (!self->queue_open) return;
        if (self->queue.size() >= kMaxQueued) self->queue.pop_front();
        self->queue.emplace_back(reinterpret_cast<const uint8_t*>(data), reinterpret_cast<const uint8_t*>(data) + size);
        self->queue_cv.notify_one();
    }

    // Creates an agent on `on_port` with the fixed credentials, publishes it and starts gathering.
    bool CreateAgent(uint16_t on_port) {
        juice_config_t config{};
        config.concurrency_mode = JUICE_CONCURRENCY_MODE_THREAD;
        if (use_stun) {
            config.stun_server_host = kStunHost;
            config.stun_server_port = kStunPort;
        }
        config.local_port_range_begin = on_port;
        config.local_port_range_end = on_port;
        config.cb_state_changed = OnState;
        config.cb_gathering_done = OnGathered;
        config.cb_recv = OnRecv;
        config.user_ptr = this;
        juice_agent_t* created = juice_create(&config);
        if (!created) return false;
        {
            std::lock_guard guard(signal_mutex);
            gathered = false;
        }
        std::lock_guard guard(agent_mutex);
        current.store(created);
        // The local socket is bound while gathering starts, so a port in use fails here.
        if (juice_set_local_ice_attributes(created, ufrag.c_str(), pwd.c_str()) != JUICE_ERR_SUCCESS ||
            juice_gather_candidates(created) != JUICE_ERR_SUCCESS) {
            current.store(nullptr);
            juice_destroy(created);
            return false;
        }
        agent = created;
        return true;
    }

    void DestroyAgent() {
        std::lock_guard guard(agent_mutex);
        current.store(nullptr);
        if (agent) juice_destroy(agent);
        agent = nullptr;
    }

    // Returns false once stopped.
    bool WaitGathered() {
        std::unique_lock lock(signal_mutex);
        if (!signal_cv.wait_for(lock, kGatherTimeout, [&] { return gathered || stopped; }) && use_stun)
            LOGW("stream: ice: the STUN server did not answer; only local addresses are offered");
        return !stopped;
    }

    bool ApplyRemote(const std::string& description) {
        std::lock_guard guard(agent_mutex);
        return agent && juice_set_remote_description(agent, description.c_str()) == JUICE_ERR_SUCCESS;
    }

    void FailRemote() {
        LOGW("stream: ice: the other player's code was rejected");
        std::lock_guard guard(signal_mutex);
        SetState(IceState::Failed);
    }

    // Replaces a failed agent with one that has the same port and credentials, so the codes the
    // players already exchanged stay valid.
    void Recreate() {
        DestroyAgent();
        while (true) {
            if (CreateAgent(port)) {
                if (!WaitGathered()) return;
                std::string description;
                {
                    std::lock_guard guard(signal_mutex);
                    description = remote;
                }
                if (!ApplyRemote(description)) FailRemote();
                return;
            }
            // The previous socket may still hold the port for a moment.
            std::unique_lock lock(signal_mutex);
            if (signal_cv.wait_for(lock, kRetryDelay, [&] { return stopped; })) return;
        }
    }

    void Work() {
        if (!WaitGathered()) return;
        std::string pending;
        {
            std::lock_guard guard(agent_mutex);
            char description[JUICE_MAX_SDP_STRING_LEN] = {};
            if (agent && juice_get_local_description(agent, description, sizeof(description)) == JUICE_ERR_SUCCESS) {
                std::lock_guard signal(signal_mutex);
                local = description;
            }
        }
        {
            std::lock_guard guard(signal_mutex);
            if (local.empty()) {
                LOGE("stream: ice: no local description");
                SetState(IceState::Failed);
                return;
            }
            // A remote description that arrived while gathering is applied now; see SetRemote.
            pending = remote;
            SetState(pending.empty() ? IceState::Ready : IceState::Connecting);
        }
        if (!pending.empty() && !ApplyRemote(pending)) FailRemote();

        while (true) {
            bool retry = false, connected = false;
            {
                std::unique_lock lock(signal_mutex);
                signal_cv.wait(lock, [&] { return stopped || failed_event || connected_event; });
                if (stopped) return;
                retry = std::exchange(failed_event, false);
                connected = std::exchange(connected_event, false);
            }
            if (connected) {
                std::lock_guard guard(agent_mutex);
                LOGI("stream: ice: route %s", RouteLocked().c_str());
            }
            if (retry) {
                LOGI("stream: ice: no working route yet, retrying");
                Recreate();
            }
        }
    }

    std::string RouteLocked() const {
        if (!agent || state.load() != IceState::Connected) return {};
        char local_address[JUICE_MAX_ADDRESS_STRING_LEN] = {};
        char remote_address[JUICE_MAX_ADDRESS_STRING_LEN] = {};
        if (juice_get_selected_addresses(agent, local_address, sizeof(local_address), remote_address,
                                         sizeof(remote_address)) != JUICE_ERR_SUCCESS)
            return {};
        std::string type = "unknown";
        char local_candidate[JUICE_MAX_CANDIDATE_SDP_STRING_LEN] = {};
        char remote_candidate[JUICE_MAX_CANDIDATE_SDP_STRING_LEN] = {};
        if (juice_get_selected_candidates(agent, local_candidate, sizeof(local_candidate), remote_candidate,
                                          sizeof(remote_candidate)) == JUICE_ERR_SUCCESS) {
            const std::string_view line(remote_candidate);
            if (const size_t at = line.find(" typ "); at != std::string_view::npos) {
                const size_t begin = at + 5;
                type = std::string(line.substr(begin, line.find_first_of(" \r\n", begin) - begin));
            }
        }
        return std::string(local_address) + " -> " + remote_address + " (" + type + ")";
    }

    // Safe to call again; Start and Stop must not run concurrently with each other.
    void Stop() {
        {
            std::lock_guard guard(signal_mutex);
            stopped = true;
            signal_cv.notify_all();
        }
        {
            std::lock_guard guard(queue_mutex);
            queue_open = false;
            queue.clear();
            queue_cv.notify_all();
        }
        if (worker.joinable()) worker.join();
        DestroyAgent();
        std::lock_guard guard(signal_mutex);
        state.store(IceState::Idle);
        remote.clear();
        local.clear();
    }
};

IceLink::IceLink() : impl_(std::make_unique<Impl>()) {}

IceLink::~IceLink() { Stop(); }

bool IceLink::Start(std::string& error, bool use_stun) {
    InitJuiceLogging();
    Stop();
    Impl& s = *impl_;

    std::random_device device;
    std::mt19937 rng(device());
    static constexpr char kIceChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::uniform_int_distribution<size_t> pick(0, sizeof(kIceChars) - 2);
    s.ufrag.clear();
    s.pwd.clear();
    for (int i = 0; i < 8; ++i) s.ufrag.push_back(kIceChars[pick(rng)]);
    for (int i = 0; i < 24; ++i) s.pwd.push_back(kIceChars[pick(rng)]);
    s.use_stun = use_stun;
    {
        std::lock_guard guard(s.signal_mutex);
        s.stopped = false;
        s.gathered = s.failed_event = s.connected_event = false;
        s.SetState(IceState::Gathering);
    }
    {
        std::lock_guard guard(s.queue_mutex);
        s.queue.clear();
        s.queue_open = true;
    }

    // The port stays fixed for the whole session: the codes name it, and a retried agent reuses it.
    std::uniform_int_distribution<uint32_t> ports(49152, 65535);
    bool created = false;
    for (int attempt = 0; attempt < 8 && !created; ++attempt) {
        s.port = uint16_t(ports(rng));
        created = s.CreateAgent(s.port);
    }
    if (!created) {
        error = "Cannot open a UDP port for internet play";
        Stop();
        std::lock_guard guard(s.signal_mutex);
        s.state.store(IceState::Failed);
        return false;
    }
    LOGI("stream: ice: local UDP port %u", unsigned(s.port));
    s.worker = std::thread(&Impl::Work, impl_.get());
    return true;
}

void IceLink::Stop() { impl_->Stop(); }

IceState IceLink::State() const { return impl_->state.load(); }

std::string IceLink::LocalDescription() const {
    std::lock_guard guard(impl_->signal_mutex);
    return impl_->local;
}

bool IceLink::SetRemote(const std::string& description, std::string& error) {
    Impl& s = *impl_;
    bool ready = false;
    {
        std::lock_guard guard(s.signal_mutex);
        const IceState now = s.state.load();
        if (s.stopped || !s.remote.empty() || description.empty() ||
            (now != IceState::Gathering && now != IceState::Ready)) {
            error = "The connection is not ready for this code";
            return false;
        }
        s.remote = description;
        // While gathering, the worker applies it once the local description exists.
        ready = now == IceState::Ready;
        if (ready) s.SetState(IceState::Connecting);
    }
    if (ready && !s.ApplyRemote(description)) {
        s.FailRemote();
        error = "The connection code is damaged or incomplete.";
        return false;
    }
    return true;
}

bool IceLink::Send(const void* data, size_t size) {
    Impl& s = *impl_;
    if (!data || !size || s.state.load() != IceState::Connected) return false;
    std::lock_guard guard(s.agent_mutex);
    return s.agent && juice_send(s.agent, static_cast<const char*>(data), size) == JUICE_ERR_SUCCESS;
}

int IceLink::Receive(uint8_t* buffer, size_t size, uint32_t timeout_ms) {
    Impl& s = *impl_;
    std::unique_lock lock(s.queue_mutex);
    if (!s.queue_open) return -1;
    if (!s.queue_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return !s.queue_open || !s.queue.empty(); }))
        return 0;
    if (!s.queue_open) return -1;
    const std::vector<uint8_t> packet = std::move(s.queue.front());
    s.queue.pop_front();
    const size_t copied = std::min(size, packet.size());
    if (copied && buffer) std::memcpy(buffer, packet.data(), copied);
    return int(copied);
}

std::string IceLink::Route() const {
    std::lock_guard guard(impl_->agent_mutex);
    return impl_->RouteLocked();
}

} // namespace openshao::stream

#endif

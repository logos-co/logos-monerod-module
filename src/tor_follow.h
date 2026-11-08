#pragma once

#include <chrono>
#include <string>

// Following tor_module's SOCKS port. monerod reads --tx-proxy once, so a port that moves needs
// a restart; this decides when. No node, no IPC, and the clock is passed in, for the tests.
namespace monerod {

using Clock = std::chrono::steady_clock;

// One restart for a port that settles; a reconnect or a close-and-reopen sends several events.
constexpr auto kFollowDebounce = std::chrono::seconds(3);
// At most one automatic restart per window, so a flapping port cannot loop the node.
constexpr auto kFollowMinGap = std::chrono::seconds(60);

class TorFollow {
public:
    // The node started with --tx-proxy at `inUse`; `automatic` when this restarted it.
    void started(const std::string& inUse, Clock::time_point now, bool automatic);
    // The node stopped, or runs without --tx-proxy: nothing to follow.
    void stopped();
    // tor_module's port as just seen: "" when it is off, unloaded or not answering. A closed
    // port never restarts the node, which would only fall back to tor_module's default.
    // Returns whether the pending restart changed.
    bool observe(const std::string& torSocks, Clock::time_point now);
    void drop() { m_pending.clear(); }

    const std::string& inUse() const { return m_inUse; }
    const std::string& pending() const { return m_pending; }
    // When the pending restart may run; Clock::time_point::max() without one.
    Clock::time_point dueAt() const;
    // The address to restart at, once due at `now`; "" otherwise.
    std::string due(Clock::time_point now) const;

private:
    std::string m_inUse, m_pending;
    Clock::time_point m_seenAt{}, m_lastAuto{};
    bool m_hadAuto = false;
};

}  // namespace monerod

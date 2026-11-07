// Following tor_module's SOCKS port: when the node restarts, with a clock the tests move.

#include <logos_test.h>

#include <string>
#include <vector>

#include "monerod_settings.h"
#include "tor_follow.h"

using json = nlohmann::json;
using monerod::Clock;
using monerod::TorFollow;
using std::chrono::milliseconds;
using std::chrono::seconds;

namespace {

const Clock::time_point t0 = Clock::time_point{} + std::chrono::hours(1);
const std::string kDefault = "127.0.0.1:9250";

// A status_changed event as it arrives: one string holding status()'s reply.
json torEvent(const char* state, int progress, const char* socks) {
    const json st = {{"ok", true}, {"state", state}, {"progress", progress},
                     {"socks", socks ? json(std::string("socks5h://") + socks) : json()}};
    return json::array({st.dump()});
}

std::string socksOf(const json& event) {
    std::string why;
    return monerod::torSocks(event[0], why);
}

}  // namespace

LOGOS_TEST(a_moved_port_restarts_the_node_after_the_debounce) {
    TorFollow f;
    f.started(kDefault, t0, false);
    LOGOS_ASSERT_TRUE(f.observe("127.0.0.1:41234", t0 + seconds(1)));
    LOGOS_ASSERT_EQ(f.pending(), std::string("127.0.0.1:41234"));
    LOGOS_ASSERT_EQ(f.due(t0 + seconds(1)), std::string());
    LOGOS_ASSERT_EQ(f.due(t0 + seconds(4) - milliseconds(1)), std::string());
    LOGOS_ASSERT_EQ(f.due(t0 + seconds(4)), std::string("127.0.0.1:41234"));
    f.started("127.0.0.1:41234", t0 + seconds(4), true);
    LOGOS_ASSERT_EQ(f.pending(), std::string());
    LOGOS_ASSERT_TRUE(f.dueAt() == Clock::time_point::max());
}

LOGOS_TEST(the_port_in_use_or_a_closed_one_never_restarts) {
    TorFollow f;
    f.started(kDefault, t0, false);
    LOGOS_ASSERT_FALSE(f.observe(kDefault, t0));
    LOGOS_ASSERT_FALSE(f.observe("", t0));                  // off, unloaded, not answering
    LOGOS_ASSERT_EQ(f.due(t0 + std::chrono::hours(1)), std::string());
    // A pending move is called off when the port closes, or comes back where it was.
    f.observe("127.0.0.1:41234", t0);
    LOGOS_ASSERT_TRUE(f.observe("", t0 + seconds(1)));
    LOGOS_ASSERT_EQ(f.due(t0 + seconds(10)), std::string());
    f.observe("127.0.0.1:41234", t0 + seconds(2));
    LOGOS_ASSERT_TRUE(f.observe(kDefault, t0 + seconds(3)));
    LOGOS_ASSERT_EQ(f.due(t0 + seconds(10)), std::string());
}

LOGOS_TEST(close_and_reopen_elsewhere_gives_one_restart) {
    TorFollow f;
    f.started(kDefault, t0, false);
    f.observe("", t0);
    f.observe("127.0.0.1:41234", t0 + milliseconds(50));
    // The same port seen again, by the poll or a progress event, keeps the debounce.
    LOGOS_ASSERT_FALSE(f.observe("127.0.0.1:41234", t0 + seconds(2)));
    LOGOS_ASSERT_EQ(f.due(t0 + milliseconds(3050)), std::string("127.0.0.1:41234"));
    // Another port starts it over.
    f.observe("127.0.0.1:41235", t0 + seconds(3));
    LOGOS_ASSERT_EQ(f.due(t0 + seconds(5)), std::string());
    LOGOS_ASSERT_EQ(f.due(t0 + seconds(6)), std::string("127.0.0.1:41235"));
}

LOGOS_TEST(one_automatic_restart_per_minute) {
    TorFollow f;
    f.started(kDefault, t0, false);
    f.observe("127.0.0.1:41234", t0);
    f.started("127.0.0.1:41234", t0 + seconds(3), true);
    // Moved again 7 s later: waits for the window, then fires once.
    f.observe("127.0.0.1:41235", t0 + seconds(10));
    LOGOS_ASSERT_TRUE(f.dueAt() == t0 + seconds(63));
    LOGOS_ASSERT_EQ(f.due(t0 + seconds(62)), std::string());
    LOGOS_ASSERT_EQ(f.due(t0 + seconds(63)), std::string("127.0.0.1:41235"));
    // Flapping inside the window still ends in one restart, at the last port.
    f.observe("127.0.0.1:41234", t0 + seconds(20));
    f.observe("127.0.0.1:41236", t0 + seconds(62));
    LOGOS_ASSERT_EQ(f.due(t0 + seconds(64)), std::string());
    LOGOS_ASSERT_EQ(f.due(t0 + seconds(65)), std::string("127.0.0.1:41236"));
    // A user's start does not open a new window.
    f.started("127.0.0.1:41236", t0 + seconds(65), false);
    f.observe("127.0.0.1:41237", t0 + seconds(66));
    LOGOS_ASSERT_EQ(f.due(t0 + seconds(69)), std::string("127.0.0.1:41237"));
}

LOGOS_TEST(started_at_the_default_restarts_only_if_tor_is_elsewhere) {
    TorFollow f;
    f.started(kDefault, t0, false);                         // tor_module was not loaded
    LOGOS_ASSERT_FALSE(f.observe(kDefault, t0 + seconds(30)));
    LOGOS_ASSERT_EQ(f.due(t0 + seconds(60)), std::string());
    f.observe("127.0.0.1:52011", t0 + seconds(60));         // 9250 was taken: tor_module's fallback
    LOGOS_ASSERT_EQ(f.due(t0 + seconds(63)), std::string("127.0.0.1:52011"));
}

LOGOS_TEST(nothing_is_followed_while_stopped) {
    TorFollow f;
    LOGOS_ASSERT_FALSE(f.observe("127.0.0.1:41234", t0));
    f.started(kDefault, t0, false);
    f.observe("127.0.0.1:41234", t0);
    f.stopped();
    LOGOS_ASSERT_EQ(f.due(t0 + seconds(10)), std::string());
    LOGOS_ASSERT_FALSE(f.observe("127.0.0.1:41235", t0 + seconds(11)));
    LOGOS_ASSERT_EQ(f.inUse(), std::string());
}

LOGOS_TEST(tor_events_drive_one_restart) {
    // Tor joining, then the Tor app moving the port with set_socks: off, then on elsewhere.
    const std::vector<json> events = {
        torEvent("connecting", 10, "127.0.0.1:9250"), torEvent("connecting", 45, "127.0.0.1:9250"),
        torEvent("ready", 100, "127.0.0.1:9250"),     torEvent("ready", 100, nullptr),
        torEvent("ready", 100, "127.0.0.1:19250"),    torEvent("blocked", 100, "127.0.0.1:19250"),
    };
    TorFollow f;
    f.started(kDefault, t0, false);
    int changes = 0;
    Clock::time_point at = t0;
    for (const json& e : events) {
        at += milliseconds(200);
        if (f.observe(socksOf(e), at)) ++changes;
    }
    LOGOS_ASSERT_EQ(changes, 1);
    LOGOS_ASSERT_EQ(f.due(at + seconds(2)), std::string());
    LOGOS_ASSERT_EQ(f.due(at + seconds(3)), std::string("127.0.0.1:19250"));
    // An unreadable event reads as no port: it never restarts the node.
    LOGOS_ASSERT_EQ(socksOf(json::array({"not json"})), std::string());
}

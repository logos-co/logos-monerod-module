// Settings, argv, tor_module's reply and the txProxy status: what start() and status() compute.

#include <logos_test.h>

#include <algorithm>
#include <string>

#include "monerod_settings.h"

using json = nlohmann::json;

namespace {

bool hasArg(const json& argv, const std::string& a) {
    return std::find(argv.begin(), argv.end(), json(a)) != argv.end();
}

int countPrefix(const json& argv, const std::string& prefix) {
    return static_cast<int>(std::count_if(argv.begin(), argv.end(), [&](const json& a) {
        return a.get<std::string>().rfind(prefix, 0) == 0;
    }));
}

}  // namespace

LOGOS_TEST(txProxy_defaults_on_for_mainnet_only) {
    LOGOS_ASSERT_TRUE(monerod::defaultSettings("mainnet", "/p")["txProxy"].get<bool>());
    LOGOS_ASSERT_FALSE(monerod::defaultSettings("stagenet", "/p")["txProxy"].get<bool>());
    LOGOS_ASSERT_FALSE(monerod::defaultSettings("testnet", "/p")["txProxy"].get<bool>());
    LOGOS_ASSERT_TRUE(monerod::defaultSettings("regtest", "/p").empty());
}

LOGOS_TEST(txProxy_is_refused_on_test_networks_and_must_be_bool) {
    const json d = monerod::defaultSettings("stagenet", "/p");
    LOGOS_ASSERT_CONTAINS(monerod::checkSetting("stagenet", "txProxy", true, d), "mainnet-only");
    LOGOS_ASSERT_EQ(monerod::checkSetting("stagenet", "txProxy", false, d), std::string());
    const json m = monerod::defaultSettings("mainnet", "/p");
    LOGOS_ASSERT_EQ(monerod::checkSetting("mainnet", "txProxy", false, m), std::string());
    LOGOS_ASSERT_EQ(monerod::checkSetting("mainnet", "txProxy", "yes", m), std::string("wrong type for txProxy"));
    LOGOS_ASSERT_EQ(monerod::checkSetting("mainnet", "nope", 1, m), std::string("unknown setting: nope"));
    LOGOS_ASSERT_EQ(monerod::checkSetting("mainnet", "outPeers", json::parse("8"), m), std::string());
}

LOGOS_TEST(stored_values_configure_would_refuse_are_ignored) {
    const json stored = {{"txProxy", true}, {"outPeers", "8"}, {"logLevel", 2}, {"gone", 1}};
    const json s = monerod::effectiveSettings("stagenet", "/p", stored);
    LOGOS_ASSERT_FALSE(s["txProxy"].get<bool>());
    LOGOS_ASSERT_EQ(s["outPeers"].get<int>(), -1);
    LOGOS_ASSERT_EQ(s["logLevel"].get<int>(), 2);
    LOGOS_ASSERT_FALSE(s.contains("gone"));
    LOGOS_ASSERT_FALSE(monerod::effectiveSettings("mainnet", "/p", {{"txProxy", false}})["txProxy"].get<bool>());
    LOGOS_ASSERT_TRUE(monerod::effectiveSettings("mainnet", "/p", nullptr)["txProxy"].get<bool>());
}

LOGOS_TEST(argv_always_carries_tx_proxy_when_on) {
    const json s = monerod::defaultSettings("mainnet", "/p");
    const json atTor = monerod::buildArgv("mainnet", s, "/p/l.log", "127.0.0.1:41234");
    LOGOS_ASSERT_TRUE(hasArg(atTor, "--tx-proxy=tor,127.0.0.1:41234,10"));
    LOGOS_ASSERT_EQ(countPrefix(atTor, "--tx-proxy"), 1);
    LOGOS_ASSERT_EQ(countPrefix(atTor, "--proxy"), 0);
    // No port from tor_module: still --tx-proxy, at its default port.
    const json noTor = monerod::buildArgv("mainnet", s, "/p/l.log", "");
    LOGOS_ASSERT_TRUE(hasArg(noTor, "--tx-proxy=tor,127.0.0.1:9250,10"));
    LOGOS_ASSERT_EQ(countPrefix(noTor, "--tx-proxy"), 1);
    json off = s;
    off["txProxy"] = false;
    LOGOS_ASSERT_EQ(countPrefix(monerod::buildArgv("mainnet", off, "/p/l.log", "127.0.0.1:41234"), "--tx-proxy"), 0);
}

// start(): tor_module's reply, or none, picks the port; it never drops the flag.
LOGOS_TEST(argv_from_any_tor_answer_carries_tx_proxy) {
    const json s = monerod::defaultSettings("mainnet", "/p");
    const json replies[] = {json{{"ok", true}, {"socks", "socks5h://127.0.0.1:41234"}},
                            json{{"ok", true}, {"socks", nullptr}}, json{{"ok", true}, {"state", "ready"}},
                            json{{"ok", false}, {"error", "boom"}}, json("not json"), json()};
    for (const json& reply : replies) {
        std::string why;
        const std::string socks = monerod::torSocks(reply, why);
        const json argv = monerod::buildArgv("mainnet", s, "/p/l.log", socks);
        LOGOS_ASSERT_EQ(countPrefix(argv, "--tx-proxy=tor,"), 1);
        LOGOS_ASSERT_TRUE(hasArg(argv, socks.empty() ? "--tx-proxy=tor,127.0.0.1:9250,10"
                                                     : "--tx-proxy=tor,127.0.0.1:41234,10"));
    }
}

LOGOS_TEST(argv_keeps_proxy_and_tx_proxy_apart) {
    json s = monerod::defaultSettings("mainnet", "/p");
    s["proxy"] = "127.0.0.1:9050";
    const json argv = monerod::buildArgv("mainnet", s, "/p/l.log", "127.0.0.1:9250");
    LOGOS_ASSERT_TRUE(hasArg(argv, "--proxy=127.0.0.1:9050"));
    LOGOS_ASSERT_TRUE(hasArg(argv, "--tx-proxy=tor,127.0.0.1:9250,10"));
}

LOGOS_TEST(argv_for_stagenet_unchanged) {
    json s = monerod::defaultSettings("stagenet", "/p");
    s["outPeers"] = 8;
    const json argv = monerod::buildArgv("stagenet", s, "/p/l.log", "");
    const json expected = {"--stagenet", "--data-dir=/p/chain", "--log-file=/p/l.log", "--log-level=0",
                           "--rpc-bind-ip=127.0.0.1", "--rpc-bind-port=38081", "--p2p-bind-port=38080",
                           "--no-zmq", "--check-updates=disabled", "--no-igd", "--out-peers=8"};
    LOGOS_ASSERT_EQ(argv.dump(), expected.dump());
}

LOGOS_TEST(tor_socks_from_status_reply) {
    std::string why;
    LOGOS_ASSERT_EQ(monerod::torSocks({{"ok", true}, {"socks", "socks5h://127.0.0.1:9250"}}, why),
                    std::string("127.0.0.1:9250"));
    LOGOS_ASSERT_EQ(why, std::string());
    // A Rust module's reply arrives as JSON text.
    LOGOS_ASSERT_EQ(monerod::torSocks(json(R"({"ok":true,"socks":"socks5h://127.0.0.1:41234"})"), why),
                    std::string("127.0.0.1:41234"));
    LOGOS_ASSERT_EQ(monerod::torSocks({{"ok", true}, {"socks", "socks5h://[::1]:9250"}}, why),
                    std::string("[::1]:9250"));
}

LOGOS_TEST(tor_socks_says_why_there_is_none) {
    std::string why;
    LOGOS_ASSERT_EQ(monerod::torSocks({{"ok", true}, {"socks", nullptr}}, why), std::string());
    LOGOS_ASSERT_CONTAINS(why, "off");
    LOGOS_ASSERT_EQ(monerod::torSocks({{"ok", true}, {"state", "ready"}}, why), std::string());
    LOGOS_ASSERT_CONTAINS(why, "no SOCKS port");
    LOGOS_ASSERT_EQ(monerod::torSocks({{"ok", false}, {"error", "boom"}}, why), std::string());
    LOGOS_ASSERT_EQ(why, std::string("tor_module: boom"));
    LOGOS_ASSERT_EQ(monerod::torSocks(json("not json"), why), std::string());
    LOGOS_ASSERT_CONTAINS(why, "unreadable");
    for (const char* bad : {"socks5h://localhost:9250", "socks5h://127.0.0.1", "http://127.0.0.1:9250",
                            "socks5h://127.0.0.1:0", "socks5h://127.0.0.1:70000", "socks5h://1.2.3:9250"}) {
        LOGOS_ASSERT_EQ(monerod::torSocks({{"ok", true}, {"socks", bad}}, why), std::string());
        LOGOS_ASSERT_CONTAINS(why, "unexpected SOCKS address");
    }
}

LOGOS_TEST(tor_peers_counts_outbound_tor_near_the_tip) {
    const json conns = json::parse(R"([
        {"address_type": 4, "incoming": false, "height": 3800662, "state": "normal"},
        {"address_type": 4, "incoming": false, "height": 3800660, "state": "normal"},
        {"address_type": 4, "incoming": false, "height": 3800659, "state": "normal"},
        {"address_type": 4, "incoming": false, "height": 0, "state": "before_handshake"},
        {"address_type": 4, "incoming": true, "height": 3800662, "state": "normal"},
        {"address_type": 1, "incoming": false, "height": 3800662, "state": "normal"}
    ])");
    LOGOS_ASSERT_EQ(monerod::torPeers(conns, 3800662), 2);
    LOGOS_ASSERT_EQ(monerod::torPeers(json::array(), 3800662), 0);
    LOGOS_ASSERT_EQ(monerod::torPeers(json(), 3800662), -1);
    LOGOS_ASSERT_EQ(monerod::torPeers(json::array({1}), 3800662), -1);
}

LOGOS_TEST(tx_proxy_state_waits_and_says_why) {
    const std::string at = "127.0.0.1:9250";
    auto st = monerod::txProxyState(at, at, "", 2);
    LOGOS_ASSERT_FALSE(st.waiting);
    LOGOS_ASSERT_EQ(st.reason, std::string());
    st = monerod::txProxyState(at, at, "", -1);          // not counted yet
    LOGOS_ASSERT_FALSE(st.waiting);
    st = monerod::txProxyState(at, at, "", 0);
    LOGOS_ASSERT_TRUE(st.waiting);
    LOGOS_ASSERT_FALSE(st.restart);
    LOGOS_ASSERT_EQ(st.reason, std::string("No Tor peer yet. Payments wait in the node until one connects."));
    for (int peers : {0, -1}) {
        st = monerod::txProxyState(at, "", "tor_module is not loaded", peers);
        LOGOS_ASSERT_TRUE(st.waiting);
        LOGOS_ASSERT_EQ(st.reason, std::string("Tor is not available: tor_module is not loaded. "
                                               "Payments wait in the node until it is and a Tor peer connects."));
    }
    // Something else serves the port and the node has Tor peers through it.
    st = monerod::txProxyState(at, "", "tor_module is not loaded", 3);
    LOGOS_ASSERT_FALSE(st.waiting);
    for (int peers : {-1, 0, 2}) {
        st = monerod::txProxyState(at, "127.0.0.1:41234", "", peers);
        LOGOS_ASSERT_TRUE(st.waiting);
        LOGOS_ASSERT_TRUE(st.restart);
        LOGOS_ASSERT_EQ(st.reason, std::string("Tor moved to 127.0.0.1:41234. Restart the node to use it; "
                                               "until then payments wait in the node."));
    }
}

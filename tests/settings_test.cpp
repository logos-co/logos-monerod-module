// Settings, argv and tor_module's reply: what start() hands to monerod.

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

LOGOS_TEST(argv_adds_tx_proxy_only_with_an_address) {
    const json s = monerod::defaultSettings("mainnet", "/p");
    const json with = monerod::buildArgv("mainnet", s, "/p/l.log", "127.0.0.1:9250");
    LOGOS_ASSERT_TRUE(hasArg(with, "--tx-proxy=tor,127.0.0.1:9250,10"));
    LOGOS_ASSERT_EQ(countPrefix(with, "--tx-proxy"), 1);
    LOGOS_ASSERT_EQ(countPrefix(with, "--proxy"), 0);
    const json without = monerod::buildArgv("mainnet", s, "/p/l.log", "");
    LOGOS_ASSERT_EQ(countPrefix(without, "--tx-proxy"), 0);
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

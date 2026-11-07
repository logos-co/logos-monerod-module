#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

// Settings, argv and Tor-reply parsing: no node, no IPC, so the unit tests can reach them.
namespace monerod {

// --tx-proxy's max outbound Tor peers. README "Transactions over Tor" says why 10.
constexpr int kTxProxyMaxPeers = 10;

// tor_module's default SOCKS port (logos-tor-module rust-lib/src/service.rs DEFAULT_SOCKS).
constexpr const char* kTorDefaultSocks = "127.0.0.1:9250";

bool knownNetwork(const std::string& network);

// Defaults for `network` ({} for an unknown one); the chain lives under `persistDir`.
nlohmann::json defaultSettings(const std::string& network, const std::string& persistDir);

// The defaults overlaid with the `stored` values that checkSetting() accepts.
nlohmann::json effectiveSettings(const std::string& network, const std::string& persistDir,
                                 const nlohmann::json& stored);

// Why `value` cannot be stored as `key` on `network`, or "" when it can.
std::string checkSetting(const std::string& network, const std::string& key,
                         const nlohmann::json& value, const nlohmann::json& defaults);

// Where --tx-proxy points: tor_module's SOCKS port `torSocks`, or kTorDefaultSocks without one.
std::string txProxySocks(const std::string& torSocks);

// monerod flags for `network` under settings `s`. With `txProxy` on they always carry
// --tx-proxy, at txProxySocks(torSocks), whether Tor is up or not.
nlohmann::json buildArgv(const std::string& network, const nlohmann::json& s,
                         const std::string& logFile, const std::string& torSocks);

// The IP:PORT of tor_module's SOCKS port from its status() reply, or "" with `why` set.
std::string torSocks(const nlohmann::json& reply, std::string& why);

// Outbound Tor connections the node can send through, from get_connections' result: those
// within 2 blocks of `height`, as levin_notify.cpp picks them. -1 if the result is unreadable.
int torPeers(const nlohmann::json& connections, uint64_t height);

struct TxProxyState {
    bool waiting = false;
    bool restart = false;
    std::string reason;
};

// For a node running with --tx-proxy: whether payments are held, and why. `torSocks` is
// tor_module's port as last seen ("" with `torWhy` when it gave none); `peers` as torPeers();
// `restartTo` the port a pending automatic restart moves to, due in `restartInSecs`.
TxProxyState txProxyState(const std::string& torSocks, const std::string& torWhy, int peers,
                          const std::string& restartTo, int64_t restartInSecs);

}  // namespace monerod

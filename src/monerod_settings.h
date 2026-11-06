#pragma once

#include <string>

#include <nlohmann/json.hpp>

// Settings, argv and Tor-reply parsing: no node, no IPC, so the unit tests can reach them.
namespace monerod {

// --tx-proxy's max outbound Tor peers. README "Transactions over Tor" says why 10.
constexpr int kTxProxyMaxPeers = 10;

bool knownNetwork(const std::string& network);

// Defaults for `network` ({} for an unknown one); the chain lives under `persistDir`.
nlohmann::json defaultSettings(const std::string& network, const std::string& persistDir);

// The defaults overlaid with the `stored` values that checkSetting() accepts.
nlohmann::json effectiveSettings(const std::string& network, const std::string& persistDir,
                                 const nlohmann::json& stored);

// Why `value` cannot be stored as `key` on `network`, or "" when it can.
std::string checkSetting(const std::string& network, const std::string& key,
                         const nlohmann::json& value, const nlohmann::json& defaults);

// monerod flags for `network` under settings `s`. A non-empty `txProxySocks` (IP:PORT)
// adds --tx-proxy through it.
nlohmann::json buildArgv(const std::string& network, const nlohmann::json& s,
                         const std::string& logFile, const std::string& txProxySocks);

// The IP:PORT of tor_module's SOCKS port from its status() reply, or "" with `why` set.
std::string torSocks(const nlohmann::json& reply, std::string& why);

}  // namespace monerod

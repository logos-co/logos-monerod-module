#include "monerod_settings.h"

#include <cctype>
#include <filesystem>

using json = nlohmann::json;

namespace monerod {

namespace {

struct NetPorts { int rpc, p2p; };

bool portsFor(const std::string& network, NetPorts& out) {
    if (network == "mainnet")  { out = {18081, 18080}; return true; }
    if (network == "testnet")  { out = {28081, 28080}; return true; }
    if (network == "stagenet") { out = {38081, 38080}; return true; }
    return false;
}

bool allDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

// monerod takes an IPv4 or [IPv6] literal and a port here, never a name.
bool ipPort(const std::string& hostPort) {
    const auto colon = hostPort.rfind(':');
    if (colon == std::string::npos || colon == 0) return false;
    const std::string host = hostPort.substr(0, colon), port = hostPort.substr(colon + 1);
    if (!allDigits(port) || port.size() > 5 || std::stoi(port) < 1 || std::stoi(port) > 65535) return false;
    if (host.front() == '[') return host.size() > 2 && host.back() == ']';
    int parts = 0;
    size_t start = 0;
    while (start <= host.size()) {
        const auto dot = host.find('.', start);
        const std::string part = host.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!allDigits(part) || part.size() > 3 || std::stoi(part) > 255) return false;
        ++parts;
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return parts == 4;
}

}  // namespace

bool knownNetwork(const std::string& network) {
    NetPorts p{};
    return portsFor(network, p);
}

json defaultSettings(const std::string& network, const std::string& persistDir) {
    NetPorts p{};
    if (!portsFor(network, p)) return json::object();
    return json{
        {"dataDir", persistDir.empty() ? "" : (std::filesystem::path(persistDir) / "chain").string()},
        {"rpcBindPort", p.rpc},
        {"p2pBindPort", p.p2p},
        {"pruneBlockchain", network == "mainnet"},  // ~60 GB instead of ~250 GB
        {"outPeers", -1},
        {"inPeers", -1},
        {"limitRateUp", -1},
        {"limitRateDown", -1},
        {"noIgd", true},
        {"offline", false},
        {"logLevel", 0},
        {"proxy", ""},
        // monerod knows Tor seed nodes for mainnet only (p2p/net_node.inl get_seed_nodes).
        {"txProxy", network == "mainnet"},
    };
}

std::string checkSetting(const std::string& network, const std::string& key,
                         const json& value, const json& defaults) {
    if (!defaults.contains(key)) return "unknown setting: " + key;
    const json& d = defaults[key];
    // nlohmann types a parsed 8 as unsigned and the default -1 as signed.
    const bool ok = d.is_number() ? value.is_number_integer()
                  : d.is_boolean() ? value.is_boolean() : value.is_string();
    if (!ok) return "wrong type for " + key;
    // With no Tor peer to reach, the node would hold its transactions forever.
    if (key == "txProxy" && value.get<bool>() && network != "mainnet")
        return "txProxy is mainnet-only: monerod knows no Tor peers on " + network;
    return "";
}

json effectiveSettings(const std::string& network, const std::string& persistDir, const json& stored) {
    json s = defaultSettings(network, persistDir);
    const json defaults = s;
    // A hand-edited file must not slip past configure()'s checks.
    if (stored.is_object())
        for (auto& [k, v] : stored.items())
            if (checkSetting(network, k, v, defaults).empty()) s[k] = v;
    return s;
}

std::string txProxySocks(const std::string& torSocks) {
    return torSocks.empty() ? kTorDefaultSocks : torSocks;
}

json buildArgv(const std::string& network, const json& s, const std::string& logFile,
               const std::string& torSocks) {
    json argv = json::array();
    if (network != "mainnet") argv.push_back("--" + network);
    argv.push_back("--data-dir=" + s["dataDir"].get<std::string>());
    argv.push_back("--log-file=" + logFile);
    argv.push_back("--log-level=" + std::to_string(s["logLevel"].get<int>()));
    argv.push_back("--rpc-bind-ip=127.0.0.1");
    argv.push_back("--rpc-bind-port=" + std::to_string(s["rpcBindPort"].get<int>()));
    argv.push_back("--p2p-bind-port=" + std::to_string(s["p2pBindPort"].get<int>()));
    argv.push_back("--no-zmq");
    argv.push_back("--check-updates=disabled");
    if (s["pruneBlockchain"].get<bool>()) argv.push_back("--prune-blockchain");
    if (s["noIgd"].get<bool>()) argv.push_back("--no-igd");
    if (s["offline"].get<bool>()) argv.push_back("--offline");
    static const json flag = {{"outPeers", "--out-peers="}, {"inPeers", "--in-peers="},
                              {"limitRateUp", "--limit-rate-up="},
                              {"limitRateDown", "--limit-rate-down="}};
    for (const char* k : {"outPeers", "inPeers", "limitRateUp", "limitRateDown"}) {
        const int v = s[k].get<int>();
        if (v >= 0) argv.push_back(flag[k].get<std::string>() + std::to_string(v));
    }
    if (!s["proxy"].get<std::string>().empty())
        argv.push_back("--proxy=" + s["proxy"].get<std::string>());
    // Even with Tor down: monerod then holds the node's own transactions instead of sending them
    // in the clear. No scheme is SOCKS4a, which tor_module serves; monerod rejects "socks5h://".
    if (s.value("txProxy", false))
        argv.push_back("--tx-proxy=tor," + txProxySocks(torSocks) + "," + std::to_string(kTxProxyMaxPeers));
    return argv;
}

std::string torSocks(const json& reply, std::string& why) {
    // A Rust module's string result arrives as JSON text inside a JSON string.
    const json st = reply.is_string() ? json::parse(reply.get<std::string>(), nullptr, false) : reply;
    if (!st.is_object()) { why = "tor_module answered something unreadable"; return ""; }
    if (st.contains("ok") && st["ok"].is_boolean() && !st["ok"].get<bool>()) {
        why = "tor_module: " + (st.contains("error") && st["error"].is_string()
                                    ? st["error"].get<std::string>() : std::string("not ok"));
        return "";
    }
    if (!st.contains("socks")) { why = "this tor_module reports no SOCKS port"; return ""; }
    if (st["socks"].is_null()) { why = "Tor's SOCKS port is off in the Tor app"; return ""; }
    std::string s = st["socks"].is_string() ? st["socks"].get<std::string>() : "";
    for (const char* scheme : {"socks5h://", "socks5://"})
        if (s.rfind(scheme, 0) == 0) { s.erase(0, std::string(scheme).size()); break; }
    if (!ipPort(s)) { why = "unexpected SOCKS address from tor_module: " + st["socks"].dump(); return ""; }
    why.clear();
    return s;
}

int torPeers(const json& connections, uint64_t height) {
    if (!connections.is_array()) return -1;
    int n = 0;
    for (const auto& c : connections) {
        if (!c.is_object()) return -1;
        // address_type 4 = tor (epee net/enums.h); levin_notify.cpp sends through outbound ones only.
        if (c.value("address_type", 0) == 4 && !c.value("incoming", true)
            && c.value("height", uint64_t{0}) + 2 >= height)
            ++n;
    }
    return n;
}

TxProxyState txProxyState(const std::string& inUse, const std::string& torSocks,
                          const std::string& torWhy, int peers) {
    TxProxyState t;
    // monerod takes the address once, at start: a port Tor moved to needs a restart.
    if (!torSocks.empty() && torSocks != inUse) {
        t.waiting = t.restart = true;
        t.reason = "Tor moved to " + torSocks + ". Restart the node to use it; until then payments wait in the node.";
    } else if (torSocks.empty() && peers <= 0) {
        t.waiting = true;
        t.reason = "Tor is not available: " + torWhy + ". Payments wait in the node until it is and a Tor peer connects.";
    } else if (peers == 0) {
        t.waiting = true;
        t.reason = "No Tor peer yet. Payments wait in the node until one connects.";
    }
    return t;
}

}  // namespace monerod

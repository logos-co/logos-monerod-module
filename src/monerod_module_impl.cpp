#include "monerod_module_impl.h"

#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>

#include "loopback_http.h"
#include "monerod_c.h"

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct NetPorts { int rpc, p2p; };

bool portsFor(const std::string& network, NetPorts& out) {
    if (network == "mainnet")  { out = {18081, 18080}; return true; }
    if (network == "testnet")  { out = {28081, 28080}; return true; }
    if (network == "stagenet") { out = {38081, 38080}; return true; }
    return false;
}

std::string takeString(const char* s) {
    std::string out = s ? s : "";
    MONEROD_free(s);
    return out;
}

StdLogosResult fail(const std::string& why) { return {false, {}, why}; }

void appendLine(const std::string& path, const std::string& line) {
    std::ofstream f(path, std::ios::app);
    if (f) f << line << '\n';
}

}  // namespace

// Started in the body: the thread uses members that are declared after it.
MonerodModuleImpl::MonerodModuleImpl() { m_chainThread = std::thread([this] { pollChain(); }); }
MonerodModuleImpl::~MonerodModuleImpl() {
    { std::lock_guard<std::mutex> lock(m_chainMutex); m_chainStop = true; }
    m_chainWake.notify_all();
    if (m_chainThread.joinable()) m_chainThread.join();
    if (m_stopThread.joinable()) m_stopThread.join();
    MONEROD_stop();
}

void MonerodModuleImpl::pollChain() {
    std::unique_lock<std::mutex> lock(m_chainMutex);
    while (!m_chainStop) {
        const uint64_t gen = m_chainGen;
        const std::string knownTip = m_tipHash;
        lock.unlock();
        json chain;
        std::string tipHash;
        uint64_t tipTime = 0;
        const json st = json::parse(takeString(MONEROD_status_json()), nullptr, false);
        const std::string url = st.is_object() && st.value("state", "") == "running"
                                    ? st.value("rpcUrl", "") : "";
        const auto colon = url.rfind(':');
        if (colon != std::string::npos) {
            const int port = std::atoi(url.c_str() + colon + 1);
            const json r = json::parse(loopbackPost(port, "/json_rpc",
                R"({"jsonrpc":"2.0","id":"0","method":"get_info"})", 30000, &m_chainStop), nullptr, false);
            if (r.is_object() && r.contains("result") && r["result"].is_object()) chain = r["result"];
            // Only once the top block has moved: a stalled chain must not cost a second call.
            if (chain.is_object() && chain.value("top_block_hash", std::string{}) != knownTip) {
                const json b = json::parse(loopbackPost(port, "/json_rpc",
                    R"({"jsonrpc":"2.0","id":"0","method":"get_last_block_header"})", 30000, &m_chainStop),
                    nullptr, false);
                if (b.is_object() && b.contains("result") && b["result"].is_object()
                    && b["result"].contains("block_header") && b["result"]["block_header"].is_object()) {
                    const json& hdr = b["result"]["block_header"];
                    tipHash = hdr.value("hash", std::string{});
                    tipTime = hdr.value("timestamp", uint64_t{0});
                }
            }
        }
        lock.lock();
        if (gen == m_chainGen && !chain.is_null()) {
            m_chain = std::move(chain);
            m_chainAt = std::chrono::steady_clock::now();
            // Both read off one header, so the pair holds even if a block landed mid-poll.
            if (tipTime && !tipHash.empty()) { m_tipTime = tipTime; m_tipHash = tipHash; }
        }
        m_chainWake.wait_for(lock, std::chrono::seconds(1), [this] { return m_chainStop.load(); });
    }
}

// A new run must not show the last run's chain, nor keep an answer that was in flight.
void MonerodModuleImpl::resetChain() {
    std::lock_guard<std::mutex> lock(m_chainMutex);
    m_chain = nullptr;
    m_tipTime = 0;
    m_tipHash.clear();
    ++m_chainGen;
}

void MonerodModuleImpl::onContextReady() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_persistDir = instancePersistencePath();
    if (m_persistDir.empty()) return;
    std::error_code ec;
    fs::create_directories(m_persistDir, ec);
    std::ifstream f(fs::path(m_persistDir) / "monerod.json");
    if (f) {
        try { m_config = json::parse(f); } catch (...) { m_config = json::object(); }
    }
}

// Async: a syncing node needs ~3 s to wind down its p2p loop, past the 3 s grace.
// The thread is a member and joined in the destructor, so it cannot outlive us.
LogosShutdown MonerodModuleImpl::aboutToUnload() {
    if (MONEROD_state() == MONEROD_STOPPED || MONEROD_state() == MONEROD_FAILED) {
        unloadLog("aboutToUnload: node not running");
        return LogosShutdown::Synchronous;
    }
    unloadLog("aboutToUnload: stopping node");
    m_stopThread = std::thread([this] {
        const auto t0 = std::chrono::steady_clock::now();
        MONEROD_stop();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        unloadLog("aboutToUnload: node stopped in " + std::to_string(ms) + " ms");
        unloadFinished();
    });
    return LogosShutdown::Asynchronous;
}

// A file, not stderr: the host closes our stdio before it unloads us.
void MonerodModuleImpl::unloadLog(const std::string& line) {
    if (!m_persistDir.empty())
        appendLine((fs::path(m_persistDir) / "unload.log").string(), line);
}

LogosMap MonerodModuleImpl::defaultConfig(const std::string& network) {
    NetPorts p{};
    if (!portsFor(network, p)) return json::object();
    return json{
        {"dataDir", m_persistDir.empty() ? "" : (fs::path(m_persistDir) / "chain").string()},
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
    };
}

json MonerodModuleImpl::settingsFor(const std::string& network) {
    json s = defaultConfig(network);
    if (m_config.contains(network) && m_config[network].is_object())
        for (auto& [k, v] : m_config[network].items())
            if (s.contains(k)) s[k] = v;
    return s;
}

LogosMap MonerodModuleImpl::getConfig(const std::string& network) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return settingsFor(network);
}

StdLogosResult MonerodModuleImpl::configure(const std::string& network, const LogosMap& config) {
    NetPorts p{};
    if (!portsFor(network, p)) return fail("unknown network: " + network);
    if (!config.is_object()) return fail("config must be an object");
    std::lock_guard<std::mutex> lock(m_mutex);
    const json defaults = defaultConfig(network);
    json& stored = m_config[network];
    if (!stored.is_object()) stored = json::object();
    for (auto& [k, v] : config.items()) {
        if (!defaults.contains(k)) return fail("unknown setting: " + k);
        const json& d = defaults[k];
        // nlohmann types a parsed 8 as unsigned and the default -1 as signed.
        const bool ok = d.is_number() ? v.is_number_integer()
                      : d.is_boolean() ? v.is_boolean() : v.is_string();
        if (!ok) return fail("wrong type for " + k);
        stored[k] = v;
    }
    persist();
    return {true, settingsFor(network)};
}

void MonerodModuleImpl::persist() {
    if (m_persistDir.empty()) return;
    const fs::path path = fs::path(m_persistDir) / "monerod.json";
    const fs::path tmp = path.string() + ".tmp";
    { std::ofstream f(tmp); f << m_config.dump(2); }
    std::error_code ec;
    fs::rename(tmp, path, ec);
}

StdLogosResult MonerodModuleImpl::start(const std::string& network) {
    NetPorts p{};
    if (!portsFor(network, p)) return fail("unknown network: " + network);
    json argv = json::array();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_persistDir.empty()) return fail("no persistence path yet");
        const json s = settingsFor(network);
        if (network != "mainnet") argv.push_back("--" + network);
        m_logFile = (fs::path(m_persistDir) / ("monerod-" + network + ".log")).string();
        argv.push_back("--data-dir=" + s["dataDir"].get<std::string>());
        argv.push_back("--log-file=" + m_logFile);
        argv.push_back("--log-level=" + std::to_string(s["logLevel"].get<int>()));
        argv.push_back("--rpc-bind-ip=127.0.0.1");
        argv.push_back("--rpc-bind-port=" + std::to_string(s["rpcBindPort"].get<int>()));
        argv.push_back("--p2p-bind-port=" + std::to_string(s["p2pBindPort"].get<int>()));
        argv.push_back("--no-zmq");
        argv.push_back("--check-updates=disabled");
        if (s["pruneBlockchain"].get<bool>()) argv.push_back("--prune-blockchain");
        if (s["noIgd"].get<bool>()) argv.push_back("--no-igd");
        if (s["offline"].get<bool>()) argv.push_back("--offline");
        for (const char* k : {"outPeers", "inPeers", "limitRateUp", "limitRateDown"}) {
            const int v = s[k].get<int>();
            if (v < 0) continue;
            static const json flag = {{"outPeers", "--out-peers="}, {"inPeers", "--in-peers="},
                                      {"limitRateUp", "--limit-rate-up="},
                                      {"limitRateDown", "--limit-rate-down="}};
            argv.push_back(flag[k].get<std::string>() + std::to_string(v));
        }
        if (!s["proxy"].get<std::string>().empty())
            argv.push_back("--proxy=" + s["proxy"].get<std::string>());
    }
    resetChain();
    if (MONEROD_start(argv.dump().c_str()) != 0)
        return fail(takeString(MONEROD_last_error()));
    emitState();
    return {true, status()};
}

StdLogosResult MonerodModuleImpl::stop() {
    MONEROD_stop();
    resetChain();
    emitState();
    return {true, status()};
}

std::string MonerodModuleImpl::rpcEndpoint(const std::string& network) {
    NetPorts p{};
    if (!portsFor(network, p)) return "";
    std::lock_guard<std::mutex> lock(m_mutex);
    return "http://127.0.0.1:" + std::to_string(settingsFor(network)["rpcBindPort"].get<int>());
}

LogosMap MonerodModuleImpl::status() {
    json st = json::parse(takeString(MONEROD_status_json()), nullptr, false);
    if (!st.is_object()) st = json{{"state", "failed"}, {"lastError", "unreadable status"}};
    st.erase("argv");
    st["height"] = 0; st["targetHeight"] = 0; st["synchronized"] = false;
    st["peersOut"] = 0; st["peersIn"] = 0; st["databaseSize"] = "0"; st["chainAgeSecs"] = -1;
    st["tipAgeSecs"] = -1;
    if (st.value("state", "") != "running") return st;

    std::lock_guard<std::mutex> lock(m_chainMutex);
    if (!m_chain.is_object()) return st;
    const json& info = m_chain;
    st["chainAgeSecs"] = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - m_chainAt).count();
    st["height"] = info.value("height", 0);
    st["targetHeight"] = info.value("target_height", 0);
    st["synchronized"] = info.value("synchronized", false);
    st["peersOut"] = info.value("outgoing_connections_count", 0);
    st["peersIn"] = info.value("incoming_connections_count", 0);
    // u64: a JSON number loses precision past 2^53, so it crosses as a string.
    st["databaseSize"] = std::to_string(info.value("database_size", uint64_t{0}));
    if (m_tipTime) {
        const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        // A block may carry a slightly future timestamp; never report a negative age.
        const int64_t tip = static_cast<int64_t>(m_tipTime);
        st["tipAgeSecs"] = now > tip ? now - tip : 0;
    }
    return st;
}

std::string MonerodModuleImpl::logTail(int64_t lines) {
    std::string path;
    { std::lock_guard<std::mutex> lock(m_mutex); path = m_logFile; }
    std::ifstream f(path);
    if (!f || lines <= 0) return "";
    std::deque<std::string> tail;
    for (std::string l; std::getline(f, l);) {
        tail.push_back(std::move(l));
        if (static_cast<int64_t>(tail.size()) > lines) tail.pop_front();
    }
    std::string out;
    for (auto& l : tail) { out += l; out += '\n'; }
    return out;
}

void MonerodModuleImpl::emitState() { monerodStateChanged(status().dump()); }

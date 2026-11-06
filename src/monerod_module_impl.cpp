#include "monerod_module_impl.h"

#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>

#include "logos_sdk.h"
#include "loopback_http.h"
#include "monerod_c.h"
#include "monerod_settings.h"

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

// tor_module's status() answers at once; this bounds an absent or stuck one.
constexpr int kTorBudgetMs = 1000;
// While the node runs with txProxy wanted, how often the poll thread re-reads Tor's port.
constexpr auto kTorPollEvery = std::chrono::seconds(30);

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
        if (!url.empty()) pollTor();
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

// Async, then waited for: a blocking call from a worker nests in the main thread's event loop.
json MonerodModuleImpl::askTor(std::string& why) {
    logos::LpClient* tor = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_torMutex);
        if (!m_tor) m_tor = &modules().dynamic("tor_module");  // dynamic() is not thread-safe
        tor = m_tor;
    }
    struct Reply { std::mutex m; std::condition_variable cv; bool done = false; json value; std::string err; };
    auto r = std::make_shared<Reply>();
    tor->invokeAsyncResult("status", json::array(), [r](json v, const logos::CallError& e) {
        std::lock_guard<std::mutex> lock(r->m);
        if (!e.ok())
            r->err = e.code == "object_unavailable" ? "tor_module is not loaded"
                                                    : "tor_module: " + (e.message.empty() ? e.code : e.message);
        r->value = std::move(v);
        r->done = true;
        r->cv.notify_all();
    }, kTorBudgetMs);
    std::unique_lock<std::mutex> lock(r->m);
    if (!r->cv.wait_for(lock, std::chrono::milliseconds(kTorBudgetMs + 200), [&] { return r->done; })) {
        why = "tor_module did not answer";
        return nullptr;
    }
    why = r->err;
    return why.empty() ? r->value : json();
}

// Every kTorPollEvery while the node runs with txProxy wanted, so status() can say when
// Tor's port has moved since the start. Asking also keeps the node listed in the Tor app.
void MonerodModuleImpl::pollTor() {
    {
        std::lock_guard<std::mutex> lock(m_chainMutex);
        if (!m_txWanted || (m_torPolled && std::chrono::steady_clock::now() - m_torAskedAt < kTorPollEvery))
            return;
        m_torAskedAt = std::chrono::steady_clock::now();
    }
    std::string why;
    const json reply = askTor(why);
    const std::string socks = why.empty() ? monerod::torSocks(reply, why) : "";
    std::lock_guard<std::mutex> lock(m_chainMutex);
    m_torPolled = true;
    m_torSocks = socks;
    m_torWhy = why;
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
    return monerod::defaultSettings(network, m_persistDir);
}

json MonerodModuleImpl::settingsFor(const std::string& network) {
    return monerod::effectiveSettings(network, m_persistDir,
                                      m_config.contains(network) ? m_config[network] : json());
}

LogosMap MonerodModuleImpl::getConfig(const std::string& network) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return settingsFor(network);
}

StdLogosResult MonerodModuleImpl::configure(const std::string& network, const LogosMap& config) {
    if (!monerod::knownNetwork(network)) return fail("unknown network: " + network);
    if (!config.is_object()) return fail("config must be an object");
    std::lock_guard<std::mutex> lock(m_mutex);
    const json defaults = defaultConfig(network);
    for (auto& [k, v] : config.items()) {
        const std::string why = monerod::checkSetting(network, k, v, defaults);
        if (!why.empty()) return fail(why);
    }
    json& stored = m_config[network];
    if (!stored.is_object()) stored = json::object();
    for (auto& [k, v] : config.items()) stored[k] = v;
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
    if (!monerod::knownNetwork(network)) return fail("unknown network: " + network);
    // Before asking Tor: a refused start must not start Tor nor touch the running node's state.
    const int state = MONEROD_state();
    if (state == MONEROD_STARTING || state == MONEROD_RUNNING || state == MONEROD_STOPPING)
        return fail("a daemon is already running in this process");
    json s;
    std::string logFile;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_persistDir.empty()) return fail("no persistence path yet");
        s = settingsFor(network);
        logFile = (fs::path(m_persistDir) / ("monerod-" + network + ".log")).string();
    }
    // Outside m_mutex: no lock is held across IPC.
    const bool wanted = s["txProxy"].get<bool>();
    std::string socks, why;
    if (wanted) {
        const json reply = askTor(why);
        if (why.empty()) socks = monerod::torSocks(reply, why);
    }
    const json argv = monerod::buildArgv(network, s, logFile, socks);
    { std::lock_guard<std::mutex> lock(m_mutex); m_logFile = logFile; }
    resetChain();
    if (MONEROD_start(argv.dump().c_str()) != 0)
        return fail(takeString(MONEROD_last_error()));
    {
        std::lock_guard<std::mutex> lock(m_chainMutex);
        m_txWanted = wanted;
        m_txMode = !wanted ? "off" : socks.empty() ? "unavailable" : "on";
        m_txReason = wanted && socks.empty() ? why : "";
        m_txSocks = socks;
        // The answer start() just got is the poll thread's first one.
        m_torPolled = wanted;
        m_torSocks = socks;
        m_torWhy = why;
        m_torAskedAt = std::chrono::steady_clock::now();
    }
    emitState();
    return {true, status()};
}

StdLogosResult MonerodModuleImpl::stop() {
    MONEROD_stop();
    resetChain();
    {
        std::lock_guard<std::mutex> lock(m_chainMutex);
        m_txWanted = false;
        m_txMode = "off";
        m_txReason.clear();
        m_txSocks.clear();
        m_torPolled = false;
    }
    emitState();
    return {true, status()};
}

std::string MonerodModuleImpl::rpcEndpoint(const std::string& network) {
    if (!monerod::knownNetwork(network)) return "";
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
    const std::string state = st.value("state", "");

    std::lock_guard<std::mutex> lock(m_chainMutex);
    const bool live = state == "starting" || state == "running" || state == "stopping";
    st["txProxy"] = live ? m_txMode : "off";
    st["txProxySocks"] = live ? m_txSocks : "";
    st["txProxyReason"] = live ? m_txReason : "";
    st["txProxyRestart"] = false;
    // monerod takes the address once, at start: a later change needs a restart.
    if (live && m_torPolled && m_txMode != "off" && m_torSocks != m_txSocks) {
        if (m_torSocks.empty()) {
            st["txProxyReason"] = "Tor's SOCKS port has been unavailable since the start (" + m_torWhy
                                  + "): transactions wait until it is back";
        } else {
            st["txProxyRestart"] = true;
            st["txProxyReason"] = (m_txMode == "on" ? "Tor's SOCKS port moved to " : "Tor's SOCKS port is up at ")
                                  + m_torSocks + ": restart the node to use it";
        }
    }
    if (state != "running" || !m_chain.is_object()) return st;
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

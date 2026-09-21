#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <logos_json.h>
#include <logos_module_context.h>
#include <logos_result.h>

/// A Monero node running in-process through libmonerod_c. One node at a time; its
/// lifetime is this module's lifetime.
class MonerodModuleImpl : public LogosModuleContext {
public:
    MonerodModuleImpl();
    ~MonerodModuleImpl();

    /// Merge `config` into the stored settings for `network` (mainnet|stagenet|testnet).
    StdLogosResult configure(const std::string& network, const LogosMap& config);

    /// Stored settings for `network`, with defaults filled in.
    LogosMap getConfig(const std::string& network);

    /// Default settings for `network`.
    LogosMap defaultConfig(const std::string& network);

    /// Start the node for `network`. Returns once its thread is running.
    StdLogosResult start(const std::string& network);

    /// Stop the node and wait for it to shut down.
    StdLogosResult stop();

    /// @code{.json}
    /// { "state": "stopped|starting|running|stopping|failed", "network": string,
    ///   "rpcUrl": string, "uptimeSecs": number, "dataDir": string, "version": string,
    ///   "lastError": string, "height": number, "targetHeight": number,
    ///   "synchronized": bool, "peersOut": number, "peersIn": number,
    ///   "databaseSize": string, "chainAgeSecs": number, "tipAgeSecs": number }
    /// @endcode
    /// Never waits on the node: the chain fields come from its last get_info answer,
    /// `chainAgeSecs` old (-1 before the first). `tipAgeSecs` is how far the top block's
    /// own timestamp lags the clock (-1 until one is known): `synchronized` stays true
    /// with no peers, so it is the only field separating a stalled chain from a current one.
    LogosMap status();

    /// Loopback RPC URL the node serves for `network`.
    std::string rpcEndpoint(const std::string& network);

    /// The last `lines` lines of the running (or last run) node's log.
    std::string logTail(int64_t lines);

logos_events:
    /// Emitted on every lifecycle transition, with the shape of status().
    void monerodStateChanged(const std::string& payloadJson);

protected:
    void onContextReady() override;
    LogosShutdown aboutToUnload() override;

private:
    nlohmann::json settingsFor(const std::string& network);
    void persist();
    void emitState();
    void unloadLog(const std::string& line);
    void pollChain();
    void resetChain();

    std::mutex m_mutex;
    std::thread m_stopThread;
    std::string m_persistDir;
    std::string m_logFile;
    nlohmann::json m_config = nlohmann::json::object();

    // get_info waits on monerod's block-batch locks, for seconds while it syncs, so a
    // thread polls it and status() serves the last answer.
    std::thread m_chainThread;
    std::atomic<bool> m_chainStop{false};
    std::mutex m_chainMutex;
    std::condition_variable m_chainWake;
    nlohmann::json m_chain;                            // guarded by m_chainMutex
    std::chrono::steady_clock::time_point m_chainAt;   // guarded by m_chainMutex
    uint64_t m_chainGen = 0;                           // guarded; bumped when the node starts or stops
    uint64_t m_tipTime = 0;                            // guarded; unix seconds of m_tipHash, 0 = unknown
    std::string m_tipHash;                             // guarded; the block m_tipTime came from
};

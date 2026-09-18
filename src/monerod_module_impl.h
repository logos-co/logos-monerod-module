#pragma once

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
    ///   "databaseSize": string }
    /// @endcode
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

    std::mutex m_mutex;
    std::thread m_stopThread;
    std::string m_persistDir;
    std::string m_logFile;
    nlohmann::json m_config = nlohmann::json::object();
};

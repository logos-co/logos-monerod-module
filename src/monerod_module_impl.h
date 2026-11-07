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

#include "tor_follow.h"

namespace logos { class LpClient; class LpSubscription; }
struct MonerodTorFeed;

/// A Monero node running in-process through libmonerod_c. One node at a time; its
/// lifetime is this module's lifetime.
class MonerodModuleImpl : public LogosModuleContext {
public:
    MonerodModuleImpl();
    ~MonerodModuleImpl();

    /// Merge `config` into the stored settings for `network` (mainnet|stagenet|testnet).
    StdLogosResult configure(const std::string& network, const LogosMap& config);

    /// Stored settings for `network`, with defaults filled in. `txProxy` (bool): send this
    /// node's own transactions only over Tor; true by default on mainnet, refused elsewhere.
    LogosMap getConfig(const std::string& network);

    /// Default settings for `network`.
    LogosMap defaultConfig(const std::string& network);

    /// Start the node for `network`. Returns once its thread is running. With `txProxy` on it
    /// always starts with --tx-proxy: at tor_module's SOCKS port, or its default 127.0.0.1:9250.
    /// It then follows that port: when tor_module reports another one, the node restarts there.
    StdLogosResult start(const std::string& network);

    /// Stop the node and wait for it to shut down.
    StdLogosResult stop();

    /// @code{.json}
    /// { "state": "stopped|starting|running|stopping|failed", "network": string,
    ///   "rpcUrl": string, "uptimeSecs": number, "dataDir": string, "version": string,
    ///   "lastError": string, "height": number, "targetHeight": number,
    ///   "synchronized": bool, "peersOut": number, "peersIn": number,
    ///   "databaseSize": string, "chainAgeSecs": number, "tipAgeSecs": number,
    ///   "txProxy": "on|off", "txProxySocks": string, "txProxyPeers": number,
    ///   "txProxyWaiting": bool, "txProxyReason": string, "txProxyRestart": bool }
    /// @endcode
    /// Never waits on the node: the chain fields come from its last get_info answer,
    /// `chainAgeSecs` old (-1 before the first). `tipAgeSecs` is how far the top block's
    /// own timestamp lags the clock (-1 until one is known): `synchronized` stays true
    /// with no peers, so it is the only field separating a stalled chain from a current one.
    /// `txProxy` is "on" when the running node was started with --tx-proxy ("off" while stopped);
    /// `txProxyPeers`, its outbound Tor connections (-1 unknown); `txProxyWaiting`, true while its
    /// payments cannot leave, `txProxyReason` saying why; `txProxyRestart`, true while a restart
    /// onto the port Tor moved to is pending.
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
    nlohmann::json askTor(std::string& why);
    void pollTor();
    void watchTor();
    // `asked`: for an answer to status(), torEvents() when it was asked; -1 for an event.
    void torSeen(const std::string& socks, const std::string& why, int64_t asked = -1);
    int64_t torEvents();
    bool followingTor();
    void followTor();
    bool restartTor();

    std::mutex m_mutex;
    // Held through start(), stop(), an automatic restart and the unload's stop: one at a time.
    std::mutex m_lifeMutex;
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

    // What the running node was started with (guarded by m_chainMutex)...
    bool m_txOn = false;
    std::string m_txSocks, m_runNetwork, m_runLog;
    nlohmann::json m_runSettings;
    bool m_unloading = false;
    // ...tor_module's SOCKS port as last seen, and the node's Tor peers (also guarded).
    std::string m_torSocks, m_torWhy;
    int64_t m_torEvents = 0;                           // events and lost subscriptions seen
    std::chrono::steady_clock::time_point m_torAskedAt;
    int m_torPeers = -1;
    monerod::TorFollow m_follow;                       // guarded by m_chainMutex
    std::thread m_followThread;
    std::condition_variable m_followWake;
    std::mutex m_torMutex;
    logos::LpClient* m_tor = nullptr;                  // guarded by m_torMutex
    std::unique_ptr<logos::LpSubscription> m_torSub;   // guarded by m_torMutex
    std::shared_ptr<MonerodTorFeed> m_feed;
};

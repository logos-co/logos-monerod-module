// A payment held while Tor is down must not reach a clearnet peer, before or after a restart.
// A wallet (monero-c) sends through the node under test (node.cpp: --tx-proxy at a dead port)
// while the watched peer (peer.cpp) is its only connection. The node restarts with the same
// argv and retries the payment; the peer's pool must never show it. The control restarts the
// node without --tx-proxy, and the peer gets it. Prints one line per check; exits 1 on a failure.
//
// driver WALLETS_DIR NODE_RPC PEER_RPC NODE_PID NODE_LOG
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

extern "C" {
#include "monero_wallet2_api_c.h"
}

using json = nlohmann::json;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

const char* const kSeed = "velvet lymph giddy number token physics poetry unquoted nibs useful sabotage limits benches "
                          "lifestyle eden nitrogen anvil fewest avoid batch vials washing fences goat unquoted";

const Clock::time_point t0 = Clock::now();
int failures = 0;

long secs() { return std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - t0).count(); }

void check(bool ok, const std::string& what) {
    std::printf("[%4lds] %s: %s\n", secs(), ok ? "ok" : "FAIL", what.c_str());
    std::fflush(stdout);
    if (!ok) ++failures;
}

void note(const std::string& what) {
    std::printf("[%4lds] %s\n", secs(), what.c_str());
    std::fflush(stdout);
}

json post(int port, const std::string& path, const json& body) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { close(fd); return json(); }
    const std::string b = body.dump();
    const std::string req = "POST " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                            "Content-Length: " + std::to_string(b.size()) + "\r\nConnection: close\r\n\r\n" + b;
    send(fd, req.data(), req.size(), 0);
    std::string got;
    char buf[65536];
    for (ssize_t n; (n = recv(fd, buf, sizeof buf, 0)) > 0;) got.append(buf, static_cast<size_t>(n));
    close(fd);
    const auto head = got.find("\r\n\r\n");
    return head == std::string::npos ? json() : json::parse(got.substr(head + 4), nullptr, false);
}

json jsonRpc(int port, const std::string& method) {
    const json r = post(port, "/json_rpc", {{"jsonrpc", "2.0"}, {"id", "0"}, {"method", method}});
    return r.is_object() && r.contains("result") ? r["result"] : json();
}

uint64_t height(int port) {
    const json r = post(port, "/get_height", json::object());
    return r.is_object() ? r.value("height", uint64_t{0}) : 0;
}

bool inPool(int port, const std::string& txid) {
    const json p = post(port, "/get_transaction_pool_hashes", json::object());
    if (!p.is_object()) return false;
    for (const auto& h : p.value("tx_hashes", json::array())) if (h == txid) return true;
    return false;
}

// The pool entry's last_relayed_time, 0 if it is not there.
uint64_t lastRelayed(int port, const std::string& txid) {
    const json p = post(port, "/get_transaction_pool", json::object());
    if (!p.is_object()) return 0;
    for (const auto& t : p.value("transactions", json::array()))
        if (t.value("id_hash", "") == txid) return t.value("last_relayed_time", uint64_t{0});
    return 0;
}

bool connectedOutClearnet(int port) {
    const json r = jsonRpc(port, "get_connections");
    if (!r.is_object()) return false;
    for (const auto& c : r.value("connections", json::array()))
        if (!c.value("incoming", true) && c.value("address_type", 0) == 1 && c.value("state", "") == "normal")
            return true;
    return false;
}

template <class F> bool within(std::chrono::seconds limit, F f) {
    const auto end = Clock::now() + limit;
    while (Clock::now() < end) {
        if (f()) return true;
        std::this_thread::sleep_for(250ms);
    }
    return f();
}

size_t countLines(const std::string& path, const std::string& needle) {
    std::ifstream f(path);
    size_t n = 0;
    for (std::string l; std::getline(f, l);) if (l.find(needle) != std::string::npos) ++n;
    return n;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 6) {
        std::fprintf(stderr, "usage: driver WALLETS_DIR NODE_RPC PEER_RPC NODE_PID NODE_LOG\n");
        return 2;
    }
    const std::string dir = argv[1], nodeLog = argv[5];
    const int node = std::stoi(argv[2]), peer = std::stoi(argv[3]);
    const pid_t nodePid = static_cast<pid_t>(std::stol(argv[4]));
    const std::string dropped = "Unable to send transaction(s) to tor";

    MONERO_WalletManagerFactory_setLogCategories("*:FATAL");
    void* wm = MONERO_WalletManagerFactory_getWalletManager();
    void* w = MONERO_WalletManager_recoveryWallet(wm, (dir + "/held").c_str(), "pw", kSeed,
                                                  MONERO_NetworkType_STAGENET, 0, 1, "");
    check(w && MONERO_Wallet_status(w) == 0, "wallet restored");
    if (!w || MONERO_Wallet_status(w) != 0) return 1;
    check(MONERO_Wallet_init(w, ("127.0.0.1:" + std::to_string(node)).c_str(), 0, "", "", false, false, ""),
          "wallet on the node's RPC");
    MONERO_Wallet_setTrustedDaemon(w, true);
    const std::string addr = MONERO_Wallet_address(w, 0, 0);

    // ── A private chain, mined by the peer, synced by the node ──
    check(within(60s, [&] { return connectedOutClearnet(node); }), "the node is connected to the peer");
    post(peer, "/start_mining", {{"miner_address", addr}, {"threads_count", 1}, {"do_background_mining", false},
                                 {"ignore_battery", true}});
    check(within(300s, [&] { return height(node) >= 90; }), "the node synced 90 blocks the peer mined");
    post(peer, "/stop_mining", json::object());
    check(within(60s, [&] { return height(node) == height(peer); }), "node and peer at one height");
    note("peer synchronized: " + jsonRpc(peer, "get_info").value("synchronized", json(false)).dump());
    // As logos-monero-wallet-core-module drives it: wallet2 skips a refresh while it thinks the node syncs.
    MONERO_Wallet_setAutoRefreshInterval(w, 1000);
    MONERO_Wallet_startRefresh(w);
    MONERO_Wallet_refreshAsync(w);
    check(within(300s, [&] { return MONERO_Wallet_unlockedBalance(w, 0) > 10000000000000ULL; }), "wallet funded");
    MONERO_Wallet_pauseRefresh(w);

    // ── The payment, through the node ──
    // Ring size 0 = the wallet's default for the chain's fork version, as the wallet module passes.
    void* ptx = MONERO_Wallet_createTransaction(w, addr.c_str(), "", 1000000000000ULL, 0, 0, 0, "", "");
    check(ptx && MONERO_PendingTransaction_status(ptx) == 0,
          std::string("payment built") + (ptx && MONERO_PendingTransaction_status(ptx) != 0
                                              ? std::string(": ") + MONERO_PendingTransaction_errorString(ptx) : ""));
    if (!ptx || MONERO_PendingTransaction_status(ptx) != 0) {
        MONERO_WalletManager_closeWallet(wm, w, false);   // its threads must not outlive main
        return 1;
    }
    const std::string txid = MONERO_PendingTransaction_txid(ptx, "");   // before commit, which clears it
    const size_t droppedBefore = countLines(nodeLog, dropped);
    const bool committed = MONERO_PendingTransaction_commit(ptx, "", false);
    check(committed && txid.size() == 64, "the node took the payment (send_raw_transaction OK), txid " + txid);
    check(within(10s, [&] { return inPool(node, txid); }), "it is in the node's pool");

    // The peer's pool is watched from here to the end of the held phase.
    std::atomic<bool> watching{true};
    std::atomic<long> peerGotAt{-1};
    std::thread watch([&] {
        while (watching) {
            if (peerGotAt < 0 && inPool(peer, txid)) peerGotAt = secs();
            std::this_thread::sleep_for(250ms);
        }
    });
    std::this_thread::sleep_for(20s);
    check(peerGotAt < 0, "20 s after the send the peer has not got it");
    check(countLines(nodeLog, dropped) > droppedBefore, "the node logged \"" + dropped + "\"");

    // ── Restart, the module's way: same argv, still --tx-proxy ──
    const uint64_t relayedBefore = lastRelayed(node, txid);
    kill(nodePid, SIGUSR1);
    std::this_thread::sleep_for(3s);
    check(within(120s, [&] { return height(node) >= 90 && connectedOutClearnet(node); }),
          "restarted with --tx-proxy, connected to the peer again");
    check(inPool(node, txid), "the payment is still in the node's pool");
    const size_t droppedAtRestart = countLines(nodeLog, dropped);
    uint64_t relayedAfter = 0;
    const bool retried = within(600s, [&] {
        relayedAfter = lastRelayed(node, txid);
        return relayedAfter > relayedBefore;
    });
    check(retried, "the node retried it after the restart (last_relayed_time " + std::to_string(relayedBefore)
                   + " -> " + std::to_string(relayedAfter) + ")");
    std::this_thread::sleep_for(30s);
    check(countLines(nodeLog, dropped) > droppedAtRestart, "the retry went to Tor and was dropped (logged again)");
    watching = false;
    watch.join();
    check(peerGotAt < 0, "the peer never got it: not at the send, the restart nor the retry");
    check(inPool(node, txid), "still held in the node's pool");

    // ── Control: restart without --tx-proxy; the next retry goes out in the clear ──
    const uint64_t relayedControl = lastRelayed(node, txid);
    kill(nodePid, SIGUSR2);
    std::this_thread::sleep_for(3s);
    check(within(120s, [&] { return height(node) >= 90 && connectedOutClearnet(node); }),
          "control: restarted without --tx-proxy, connected to the peer");
    const long controlFrom = secs();
    const bool reached = within(900s, [&] { return inPool(peer, txid); });
    check(reached, "control: the peer got it " + std::to_string(secs() - controlFrom) + " s after that restart");
    note("control: last_relayed_time " + std::to_string(relayedControl) + " -> " +
         std::to_string(lastRelayed(node, txid)));

    MONERO_WalletManager_closeWallet(wm, w, false);
    std::printf("%s\n", failures ? "SOME CHECKS FAILED" : "all checks passed");
    return failures ? 1 : 0;
}

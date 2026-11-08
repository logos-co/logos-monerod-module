// The node under test: libmonerod_c in-process, with the argv monerod_module builds
// (monerod::buildArgv, txProxy on, Tor's port dead) plus what keeps a private chain private.
// SIGUSR1 restarts it the module's way, same argv; SIGUSR2 restarts it without txProxy (the
// control); SIGTERM or SIGINT stops it. Prints each argv.
//
// node DATA_DIR RPC_PORT P2P_PORT PEER_P2P_PORT DEAD_SOCKS_PORT
#include <csignal>
#include <cstdio>
#include <string>

#include <nlohmann/json.hpp>

#include "monerod_c.h"
#include "monerod_settings.h"

using json = nlohmann::json;

int main(int argc, char** argv) {
    if (argc != 6) {
        std::fprintf(stderr, "usage: node DATA_DIR RPC_PORT P2P_PORT PEER_P2P_PORT DEAD_SOCKS_PORT\n");
        return 2;
    }
    const std::string dir = argv[1], peer = argv[4], dead = argv[5];
    json s = monerod::defaultSettings("stagenet", dir);
    s["rpcBindPort"] = std::stoi(argv[2]);
    s["p2pBindPort"] = std::stoi(argv[3]);
    s["logLevel"] = 1;            // shows "Unable to send transaction(s) to tor"
    // The module refuses txProxy outside mainnet (no Tor peers there); this drives buildArgv directly.
    s["txProxy"] = true;

    auto argvFor = [&](bool txProxy) {
        json t = s;
        t["txProxy"] = txProxy;
        json a = monerod::buildArgv("stagenet", t, dir + "/node.log", "127.0.0.1:" + dead);
        for (const char* extra : {"--fixed-difficulty=1", "--allow-local-ip", "--disable-dns-checkpoints",
                                  "--p2p-bind-ip=127.0.0.1"})
            a.push_back(extra);
        a.push_back("--add-exclusive-node=127.0.0.1:" + peer);   // the watched peer, and nothing else
        return a;
    };
    auto start = [&](bool txProxy) {
        const json a = argvFor(txProxy);
        std::printf("argv %s\n", a.dump().c_str());
        std::fflush(stdout);
        if (MONEROD_start(a.dump().c_str()) == 0) return true;
        const char* e = MONEROD_last_error();
        std::fprintf(stderr, "node: %s\n", e ? e : "start failed");
        return false;
    };

    sigset_t set;
    sigemptyset(&set);
    for (int sig : {SIGTERM, SIGINT, SIGUSR1, SIGUSR2}) sigaddset(&set, sig);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);   // before the daemon's threads, which inherit it
    if (!start(true)) return 1;
    for (;;) {
        int sig = 0;
        sigwait(&set, &sig);
        MONEROD_stop();
        if (sig != SIGUSR1 && sig != SIGUSR2) return 0;
        std::printf("restart\n");
        if (!start(sig == SIGUSR1)) return 1;
    }
}

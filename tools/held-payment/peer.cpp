// The watched clearnet peer: a plain stagenet monerod_c at difficulty 1 that dials only the node
// under test. It mines the private chain; its pool shows whether the node ever sent it a payment.
//
// peer DATA_DIR RPC_PORT P2P_PORT NODE_P2P_PORT
#include <csignal>
#include <cstdio>
#include <string>

#include "monerod_c.h"

int main(int argc, char** argv) {
    if (argc != 5) { std::fprintf(stderr, "usage: peer DATA_DIR RPC_PORT P2P_PORT NODE_P2P_PORT\n"); return 2; }
    const std::string dir = argv[1], rpc = argv[2], p2p = argv[3], node = argv[4];
    const std::string args = "[\"--stagenet\",\"--fixed-difficulty=1\",\"--no-zmq\",\"--no-igd\","
                             "\"--check-updates=disabled\",\"--disable-dns-checkpoints\",\"--allow-local-ip\","
                             "\"--data-dir=" + dir + "\",\"--log-file=" + dir + "/peer.log\",\"--log-level=0\","
                             "\"--rpc-bind-ip=127.0.0.1\",\"--rpc-bind-port=" + rpc + "\","
                             "\"--p2p-bind-ip=127.0.0.1\",\"--p2p-bind-port=" + p2p + "\","
                             "\"--add-exclusive-node=127.0.0.1:" + node + "\"]";
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    if (MONEROD_start(args.c_str()) != 0) {
        const char* e = MONEROD_last_error();
        std::fprintf(stderr, "peer: %s\n", e ? e : "start failed");
        return 1;
    }
    int sig = 0;
    sigwait(&set, &sig);
    MONEROD_stop();
    return 0;
}

# logos-monerod-module

`monerod_module` runs a Monero node **in-process**, through `libmonerod_c` from
[logos-monero-nix](https://github.com/logos-co/logos-monero-nix). No executable is bundled and no subprocess is
spawned; the node lives and dies with this module.

```bash
logosctl call monerod_module configure stagenet 'json:{"pruneBlockchain":false}'
logosctl call monerod_module getConfig mainnet  # every setting; txProxy is true on mainnet
logosctl call monerod_module start stagenet
logosctl call monerod_module status        # state, height, targetHeight, peers, ...
logosctl call monerod_module logTail 50
logosctl call monerod_module stop
```

Settings are per network (`mainnet`, `stagenet`, `testnet`) and persisted in the
module's instance directory; `defaultConfig(network)` lists every key. RPC binds to
`127.0.0.1` only. Mainnet defaults to a pruned chain (~60 GB rather than ~250 GB).

## Settings

`configure(network, map)` stores the keys it is given and refuses an unknown key, a wrong
type, or `txProxy: true` outside mainnet. `getConfig(network)` returns every key, defaults
filled in. Settings apply on the next `start()`. Ports read mainnet / stagenet / testnet.

| Key | Type | Default | monerod flag |
|---|---|---|---|
| `dataDir` | string | `chain` in the instance directory | `--data-dir` |
| `rpcBindPort` | int | 18081 / 38081 / 28081 | `--rpc-bind-port` (on `127.0.0.1`) |
| `p2pBindPort` | int | 18080 / 38080 / 28080 | `--p2p-bind-port` |
| `pruneBlockchain` | bool | true on mainnet, else false | `--prune-blockchain` |
| `outPeers`, `inPeers` | int | -1: monerod's own default | `--out-peers`, `--in-peers` |
| `limitRateUp`, `limitRateDown` | int, kB/s | -1: monerod's own default | `--limit-rate-up`, `--limit-rate-down` |
| `noIgd` | bool | true | `--no-igd` |
| `offline` | bool | false | `--offline` |
| `logLevel` | int | 0 | `--log-level` |
| `proxy` | string | empty | `--proxy`: every P2P connection through this SOCKS proxy |
| `txProxy` | bool | true on mainnet; false, and refused, elsewhere | `--tx-proxy=tor,<Tor's SOCKS port>,10` |

## Transactions over Tor (`txProxy`)

With `txProxy` on, the node sends the transactions a wallet hands it (`/send_raw_transaction`)
to Tor peers only, through the SOCKS port of the Tor that comes with Logos
([`tor_module`](https://github.com/logos-co/logos-tor-module)). Blocks still sync over the
ordinary network, and transactions the node relays for other nodes still go out there:
Monero's Tor connections carry handshakes, timed syncs and transactions only. Sources below
are the Monero tree logos-monero-nix pins (v0.18.5.4, `85f036b`).

- **Always `--tx-proxy`, Tor up or not.** `start()` asks `tor_module.status()` for `socks`
  (`socks5h://IP:PORT`) and passes `--tx-proxy=tor,IP:PORT,10`. When `tor_module` is not
  loaded, does not answer within about a second, or reports its SOCKS port off, the node still
  starts with `--tx-proxy`, at `127.0.0.1:9250`, tor_module's default port (`DEFAULT_SOCKS`,
  logos-tor-module `rust-lib/src/service.rs:37`). Blocks sync as usual; the node's payments
  wait for Tor. `tor_module` is called by name, not declared as a dependency: this module
  builds, loads and runs without it. Asking starts Tor if it was idle.
- **Another profile's Tor.** tor_module listens on a free port instead when 9250 is taken, as
  by another Logos profile's Tor (`service.rs:234`). If this node starts while its own Tor
  is unavailable and another Logos profile's Tor holds 9250, its payments go out over that Tor.
- **With no Tor peer, a payment waits; it is not sent in the clear.** A transaction from RPC
  has no p2p origin, so it goes to the Tor zone alone (`src/p2p/net_node.inl:2376`). The
  node records it as relayed, still as its own (`local`) transaction
  (`levin_notify.cpp:846-853`); a Tor "noise" channel without a connection drops its copy and
  logs `Unable to send transaction(s) to tor - no available outbound connections`
  (`levin_notify.cpp:335-342`). The pool re-relays it, again to Tor only
  (`cryptonote_core.cpp:1126-1127, 1148`), 5 minutes later, then at intervals that roughly
  double, up to 4 hours apart (`tx_pool.cpp:93-108, 814-821`), until it is 1.5 days old
  (`tx_pool.cpp:827-828`). The pool drops it at 3 days (`cryptonote_config.h:102`,
  `tx_pool.cpp:733`). Clearnet peers asking for missing pool transactions get only fluffed
  and mined ones (`tx_pool.cpp:679-681`). Monero's docs agree: the transaction is "kept for
  future broadcasting over an anonymity network" (`docs/ANONYMITY_NETWORKS.md:13-20`).
- **Across a restart, too.** The pool survives a restart (`tx_pool.cpp:1891-1914`) and this
  module starts with `--tx-proxy` whenever `txProxy` is on, so a held payment keeps waiting.
  `tools/held-payment` tests that (see Tests). Turning `txProxy` off sends what is waiting
  over the ordinary network after the next start (`ANONYMITY_NETWORKS.md:18-20`), from this
  computer's IP address, at the pool's next retry (598 s after the start in that test).
- **The wallet is not told.** `/send_raw_transaction` answers `"status": "OK"` once the
  pool accepts the transaction and the relay code has it; it does not check that a peer got
  it (`src/rpc/core_rpc_server.cpp:1394-1396`, whose TODO says so). The payment is in the
  node's pool, so a wallet shows it as pending; `txProxyWaiting` and `txProxyReason` say why
  it has not left.
- **When Tor comes back** on the same port, the node keeps dialling it and needs no restart
  (on another port, see "Following Tor's port" below). With no Tor peer on its list it
  retries Tor's seed nodes every round (`net_node.inl:1926, 1865-1875`). With Tor peers on its
  list, it skips any address that failed in the past hour (`net_node.inl:1557, 1816`;
  `cryptonote_config.h:155`), so its first Tor peer can take up to about an hour; a restart,
  such as the Monero Node app's Restart, clears that memory (`net_node.h:489`). Once a Tor
  peer is connected, a waiting payment leaves at the pool's next retry. Measured on mainnet:
  the first Tor peer came 400 s after tor_module loaded on a fresh node, and 171 s after on a
  node restarted with Tor peers on its list and Tor down for its first 90 s.
- **Clearnet can stall while Tor comes up.** One loop opens outbound connections for every
  zone (`net_node.inl:1913-1960`), and a Tor dial holds it for up to 45 s waiting on the
  SOCKS port (`cryptonote_config.h:145`, `net_node.cpp:366-371`). On the fresh mainnet node
  above, clearnet outbound peers fell to 0 for about 2 minutes while Tor was coming up, then
  recovered on their own.
- **Mainnet only.** monerod knows Tor seed nodes for mainnet alone; on stagenet and testnet
  `get_seed_nodes` returns none (`net_node.inl:876-888`), and each network keeps its own
  peer list (`net_node.h:217`), so a `--tx-proxy` node there would hold every transaction.
  `txProxy` is off on those networks and `configure` refuses to turn it on; a wallet there
  should use a remote node over Tor instead. There is no maintained list of stagenet or
  testnet onion peers to add.
- **Why 10 Tor peers.** The node sends over 2 noise channels (`CRYPTONOTE_NOISE_CHANNELS`,
  `cryptonote_config.h:119`), re-picked at random from its outbound Tor connections every
  5 minutes and up to 30 s (`cryptonote_config.h:114-115`, `levin_notify.cpp:730-741`,
  `src/net/dandelionpp.cpp:113-118`). More connections give that pick more peers, which is
  the docs' answer to one onion peer seeing two of your transactions
  (`ANONYMITY_NETWORKS.md:211-220`). 10 is the docs' own example
  (`ANONYMITY_NETWORKS.md:46`), just under monerod's default of 12
  (`net_node.inl:2845-2846`). The cost is small: besides handshakes and timed syncs, each
  channel sends 3 KiB of noise every 10-15 s (`cryptonote_config.h:116-118`).
- **`proxy` and `txProxy` are independent.** `proxy` (`--proxy`) puts every ordinary P2P
  connection, block sync included, through the SOCKS proxy you name
  (`net_node.inl:921-931`). `txProxy` adds separate Tor connections for this node's own
  transactions. Either, both or neither may be on; with both, the node's own transactions
  still go to Tor peers only.
- **Following Tor's port.** monerod reads `--tx-proxy` once, at start, so when Tor's port
  moves this module restarts the node there itself: stop, then start on the same network with
  the same settings and the new port. The pool survives that (`tx_pool.cpp:1891-1914`), so held
  payments keep waiting, and nothing goes out in the clear.
  - It hears of the port from tor_module's `status_changed` event, which carries `status()`'s
    reply, and acts only when `socks` changes. The subscription is taken at the first start
    with `txProxy` on, arms when tor_module loads, also later, and re-arms after it reloads.
    Events sent before an arm are lost, so each arm asks `status()` once. `status()` is still
    asked every 30 s as a fallback, which also keeps the node listed in the Tor app.
  - A new port restarts the node once it has held for 3 s, so closing and reopening the port
    gives one restart, and at most once a minute: a later move waits for that minute to end.
    Until then `txProxyRestart` is true and `txProxyReason` names the port.
  - A closed port (SOCKS off, tor_module unloaded or not answering) never restarts it: a
    restart could only fall back to 9250. The node keeps its port and waits, and restarts when
    a port opens somewhere else. tor_module closes its port on unload without an event, so a
    lost subscription counts as closed.
  - Only a node started with `txProxy` on, on mainnet, while it runs; never during a start or
    stop. A node started at 9250 without tor_module restarts only if Tor comes up elsewhere.

`status()` reports what the running node was started with:

| Field | Meaning |
|---|---|
| `txProxy` | `on`: the node was started with `--tx-proxy`, Tor up or not. `off`: `txProxy` is off, or the node is not running |
| `txProxySocks` | the `IP:PORT` `--tx-proxy` points at; empty when `off` |
| `txProxyPeers` | outbound Tor connections the node can send through, from `get_connections` (`address_type` 4, within 2 blocks of the node's height, as `levin_notify.cpp:150-151` picks them); -1 when not known yet |
| `txProxyWaiting` | true while payments cannot leave: no Tor peer, Tor not available, or a restart onto Tor's new port pending |
| `txProxyReason` | a sentence saying why, while waiting; empty otherwise |
| `txProxyRestart` | true while the node waits to restart onto the port Tor moved to: 3 s, or up to a minute after the last automatic restart |

`peersOut` counts clearnet outbound connections only (`core_rpc_server.cpp:530`); Tor ones
are in `txProxyPeers`.

`status()` reports `tipAgeSecs`: how far the top block's own timestamp now lags the clock.
monerod's `synchronized` flag is sticky and only ever means that no peer has contradicted
us, so a node that loses every peer keeps reporting it while its chain goes stale —
`tipAgeSecs` is what tells the two apart. It stays `-1` through the initial sync, because
`get_last_block_header` answers with a zeroed header until the node is synchronized.

It builds as a plain module (`"transport": "qt_remote_plain"`), so it runs in
`logos_host_plain` and another runtime can use it. A `logosctl` daemon exports it with
`peering: { exports: { enabled: true, modules: { monerod_module: { events: true } } } }`,
and a linked Basecamp imports it under Settings → Peering; every call and
`monerodStateChanged` then reach the daemon's node
([logos-peering](https://github.com/logos-co/logos-peering)).

On unload the node is stopped asynchronously and the outcome is written to
`unload.log` in the instance directory. A node killed mid-flush resumes from its last
committed height: the chain is LMDB, which is crash-safe.

## Tests

`nix build .#unit-tests` runs `tests/`: the settings, monerod's argv, the parsing of
`tor_module`'s reply, the `txProxy` status fields, and when a moved port restarts the node, fed
mocked `status_changed` events on a clock the tests move, with `monerod_c` mocked out.
`doctests/` runs the module under logosctl, with tor_module: it moves Tor's port with
`set_socks` while an offline mainnet node runs and checks that the node follows it.

`tools/held-payment/run.sh` (about 20 minutes, not in CI) shows that a payment held while Tor
is down does not reach a clearnet peer, before or after a restart. A wallet sends through a
node started with the argv `monerod::buildArgv` builds, `--tx-proxy` at a dead port, on a
private stagenet chain whose only other node is the watched peer. `txProxy` is mainnet-only in
`configure`, so the test calls `buildArgv` directly. The node is restarted the module's way,
same argv, and retries the payment to Tor; the peer's pool never shows it. In the control the
node restarts without `--tx-proxy`, and the peer gets the payment at the next retry.

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

- **Where the port comes from.** `start()` asks `tor_module.status()` for `socks`
  (`socks5h://IP:PORT`) and passes `--tx-proxy=tor,IP:PORT,10`. `tor_module` is called by
  name, not declared as a dependency: this module builds, loads and runs without it. The ask
  is bounded to about a second, and it starts Tor if Tor was idle.
- **No Tor, no `--tx-proxy`.** When `tor_module` is not loaded, does not answer, or reports
  its SOCKS port off, the node starts without `--tx-proxy`, `status().txProxy` reads
  `unavailable`, and `txProxyReason` says why. Its transactions then go out over the ordinary
  network (Dandelion++), as with `txProxy` off.
- **With no Tor peer yet, a transaction waits; it is not sent in the clear.** A transaction
  from RPC has no p2p origin, so it goes to the Tor zone alone
  (`src/p2p/net_node.inl:2376`). The node marks it relayed (`levin_notify.cpp:853`); a Tor
  "noise" channel without a connection drops its copy and logs `Unable to send
  transaction(s) to tor - no available outbound connections` (`levin_notify.cpp:335-342`).
  The transaction stays in the pool, which re-relays it, again to Tor only
  (`cryptonote_core.cpp:1126-1127, 1148`), 5 minutes later and then at a growing interval of
  up to 4 hours (`tx_pool.cpp:93-94, 103-108, 814-821`), until it is 1.5 days old
  (`tx_pool.cpp:827-828`). The pool drops it at 3 days (`cryptonote_config.h:102`,
  `tx_pool.cpp:733`). Monero's docs say the same: the transaction is "kept for future
  broadcasting over an anonymity network" (`docs/ANONYMITY_NETWORKS.md:13-20`).
- **The wallet is not told.** `/send_raw_transaction` answers `"status": "OK"` once the
  pool accepts the transaction and the relay code has it; it does not check that a peer got
  it (`src/rpc/core_rpc_server.cpp:1394-1396`, whose TODO says so).
- **A restart without Tor sends what waits.** The pool survives a restart
  (`tx_pool.cpp:1891-1914`), and a node started without `--tx-proxy` relays its waiting
  transactions over the ordinary network (`ANONYMITY_NETWORKS.md:18-20`). Since this module
  starts without `--tx-proxy` when Tor is unavailable, a transaction waiting from an earlier
  run with Tor leaves in the clear on such a start.
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
- **Tor's port changing while the node runs.** monerod takes `--tx-proxy` once, at start, and
  this module never restarts the node itself. While the node runs with `txProxy` on, the
  module asks `tor_module.status()` again every 30 s (which also keeps the node listed in the
  Tor app). If the port moved, or Tor came up after an `unavailable` start, `txProxyRestart`
  turns true and `txProxyReason` names the new port; if the port went away, `txProxyReason`
  says the node's transactions wait.

`status()` reports what the running node was started with:

| Field | Meaning |
|---|---|
| `txProxy` | `on`: `--tx-proxy` through `txProxySocks`. `unavailable`: `txProxy` is set, but the node started without it. `off`: not set, or the node is not running |
| `txProxySocks` | the `IP:PORT` in effect, or empty |
| `txProxyReason` | why it is `unavailable`, or what changed since the start; empty otherwise |
| `txProxyRestart` | true when a restart would put Tor's current port in effect |

`status()` reports `tipAgeSecs`: how far the top block's own timestamp now lags the clock.
monerod's `synchronized` flag is sticky and only ever means that no peer has contradicted
us, so a node that loses every peer keeps reporting it while its chain goes stale —
`tipAgeSecs` is what tells the two apart. It stays `-1` through the initial sync, because
`get_last_block_header` answers with a zeroed header until the node is synchronized.

On unload the node is stopped asynchronously and the outcome is written to
`unload.log` in the instance directory. A node killed mid-flush resumes from its last
committed height: the chain is LMDB, which is crash-safe.

## Tests

`nix build .#unit-tests` runs `tests/`: the settings, monerod's argv and the parsing of
`tor_module`'s reply, with `monerod_c` mocked out. `doctests/` runs the module under logosctl.

# logos-monerod-module

`monerod_module` runs a Monero node **in-process**, through `libmonerod_c` from
[logos-monero-nix](https://github.com/logos-co/logos-monero-nix). No executable is bundled and no subprocess is
spawned; the node lives and dies with this module.

```bash
logoscore call monerod_module configure stagenet 'json:{"pruneBlockchain":false}'
logoscore call monerod_module start stagenet
logoscore call monerod_module status        # state, height, targetHeight, peers, ...
logoscore call monerod_module logTail 50
logoscore call monerod_module stop
```

Settings are per network (`mainnet`, `stagenet`, `testnet`) and persisted in the
module's instance directory; `defaultConfig(network)` lists every key. RPC binds to
`127.0.0.1` only. Mainnet defaults to a pruned chain (~60 GB rather than ~250 GB).

`status()` reports `tipAgeSecs`: how far the top block's own timestamp now lags the clock.
monerod's `synchronized` flag is sticky and only ever means that no peer has contradicted
us, so a node that loses every peer keeps reporting it while its chain goes stale —
`tipAgeSecs` is what tells the two apart. It stays `-1` through the initial sync, because
`get_last_block_header` answers with a zeroed header until the node is synchronized.

On unload the node is stopped asynchronously and the outcome is written to
`unload.log` in the instance directory. A node killed mid-flush resumes from its last
committed height: the chain is LMDB, which is crash-safe.

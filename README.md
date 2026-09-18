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

On unload the node is stopped asynchronously and the outcome is written to
`unload.log` in the instance directory. A node killed mid-flush resumes from its last
committed height: the chain is LMDB, which is crash-safe.

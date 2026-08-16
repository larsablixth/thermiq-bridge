# Vendored sources

These two files are **not written here**. They are copies from
[larsablixth/thermiq_mqtt-ha](https://github.com/larsablixth/thermiq_mqtt-ha),
the Home Assistant integration this bridge is derived from, and everything
under `src/*_gen.c` is generated from them:

| File | Generates | Origin |
|---|---|---|
| `thermiq_regs.py` | `src/registers_gen.c` - registers, units, bounds, bitmasks, names | ThermIQ's, with later corrections in the fork |
| `heatpump_widget.j2` | `src/widget_gen.c` - the animated widget renderer | written for the fork; it has no upstream counterpart |

Do not edit them here. Edit them upstream, then re-vendor and regenerate:

```bash
base=https://raw.githubusercontent.com/larsablixth/thermiq_mqtt-ha/master
curl -fsSL "$base/custom_components/thermiq_mqtt/heatpump/thermiq_regs.py" -o vendor/thermiq_regs.py
curl -fsSL "$base/custom_components/thermiq_mqtt/frontend/heatpump_widget.j2" -o vendor/heatpump_widget.j2
make generate && make test
```

## Provenance

- Upstream: <https://github.com/larsablixth/thermiq_mqtt-ha>
- Taken from commit: `6d5021d7b436778750f5e5bf2e6064fd998a1611`
- Vendored on: 2026-08-16

Living in a separate repository means these copies can fall behind, which
would be the quiet kind of wrong: the bridge would keep decoding and drawing
last month's pump. The `upstream` job in CI fetches both files weekly and
fails if they differ from these, so drift becomes a red build rather than a
surprise. When it fires, re-vendor as above and commit the regenerated output.

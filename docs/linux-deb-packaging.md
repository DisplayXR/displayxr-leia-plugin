# Linux `.deb` packaging (Phase 2 of DisplayXR-runtime #781)

The Leia SR plug-in ships a Debian package that drops the display processor into
the runtime's built-in discovery dir, so on a box with the SR stack the Leia DP
claims the display **automatically — no env vars, no force-probe**. It is the
vendor companion to the runtime `.deb` (which ships sim-display as the built-in
fallback; see `displayxr-runtime/scripts/package_deb_linux.sh`).

## What the package does

| Concern | How |
|---|---|
| Discovery | Drops `DisplayXR-LeiaSR.so` + `050-leia-sr.json` (probe_order **50**) into `/usr/lib/displayxr/plugins/` — the dir the runtime searches by default (runtime #781 Phase 1). No registry, no env. |
| Claim vs fallback | `probe_order=50` beats sim-display's `200`. The **presence probe** (leia-plugin #99, on `main`) claims when the SR runtime is reachable and declines otherwise, so sim-display drives apps on a box without the SR stack. No `DXR_LEIA_FORCE_PROBE`. |
| SR runtime discovery | **No baked rpath.** The srSDK loader resolves `libLeiaSR_runtime.so` via `/etc/leia/sr/1/active_runtime.json` (registered by the SR runtime installer) → `$SR_RUNTIME_PATH` → plain dlopen. See `docs/leia-linux-sdk-contract.md` §7. The packager strips any `DT_RUNPATH` with `patchelf` so no build-machine path ships. |
| Dependencies | `Depends: displayxr-runtime` (hard prereq) + the libs `objdump` finds. `Recommends:` the SR runtime package — without it the plug-in still installs and its probe declines. |

## Build & test

```bash
# Track B (real srSDK weaver) — requires the commercial SR SDK. Run on an
# SR-equipped Linux box or a container with the SDK unpacked.
SRSDK_ROOT=/path/to/leiasr-sdk ./scripts/package_deb_leia.sh
#   -> dist/displayxr-leia-sr_<ver>_<arch>.deb

# Packaging-mechanics only (no SR SDK; Track A stub weaver). Validates build,
# rpath strip, Depends, dpkg, install-alongside-runtime, and probe_order 50.
./scripts/test_deb_leia.sh --stub          # Docker; needs a displayxr-runtime checkout next door
```

`test_deb_leia.sh --stub` installs the runtime `.deb` + the stub plug-in `.deb`
together in a pristine `ubuntu:24.04` and asserts: default → sim-display claims;
`DXR_LEIA_FORCE_PROBE=1` → leia-sr claims at probe_order 50. **Real weave / claim
over sim on real hardware is validated on the SR box (`--sdk`), not in the stub
path.**

## Open items before release

- ✅ **SR runtime package name = `leiasr-runtime`** — confirmed against LeiaSR
  `packaging/linux/deb/control.in` (ST-5525-linux-support branch; installs under
  `/opt/leiasr`). `Recommends: leiasr-runtime`.
- ⚠️ **SR-side integration gap:** the `leiasr-runtime` .deb bundles
  `libLeiaSR_runtime.so` under `/opt/leiasr/lib` but does **not** register
  `/etc/leia/sr/1/active_runtime.json`, add an `ld.so.conf.d` entry, or
  `ldconfig` that dir — so the srSDK loader's default resolution finds nothing
  as-is. The correct fix is on the SR side (register the active_runtime path in
  its `postinst`). Until then a deployed plug-in needs `SR_RUNTIME_PATH`
  (env) or a baked rpath to the stable `/opt/leiasr/lib` (a fixed *install*
  path, distinct from the dev/build-machine rpath the release model rejects).
  **Raise with George before shipping** — don't paper over it in the plug-in.
- **Track B build + hardware acceptance** run on an SR box (Suzhou / George).
- The `-DDXR_LEIA_SDK_DEV_RPATH=OFF` build option lands with the
  `linux-sdk-rpath-dev-only` branch; until it merges, the `patchelf` strip in the
  packager already produces a correct (rpath-free) release artifact.
- CI wiring + `versions.json` / meta-bundle inclusion (later; mirrors the
  runtime's out-of-scope list on #781).

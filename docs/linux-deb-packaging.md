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
| Dependencies | `Depends: displayxr-runtime` (hard prereq) + **versioned** system-library dependencies derived with `dpkg-shlibdeps`. `Recommends:` the SR runtime package — without it the plug-in still installs and its probe declines. |
| Portability | **One package for Ubuntu 22.04, 24.04 and 26.04.** See below. |

## One package for 22.04, 24.04 and 26.04

v2.7.0 installed on 24.04 only, for two independent reasons — the same pair the
runtime fixed in displayxr-runtime #1656 / PR #1659:

1. **The glibc floor is the build host's.** Built on the 24.04 runner, the `.so`
   needed `GLIBC_2.38` / `GLIBCXX_3.4.31`, while `Depends` carried an
   *unversioned* `libc6`. apt therefore installed it happily on 22.04 and the
   runtime then failed to `dlopen` the plug-in.
2. **Package names are not stable across releases.** It declared
   `libpipewire-0.3-0t64`, which does not exist on jammy (`libpipewire-0.3-0`).

The fix, all of it enforced rather than documented:

| Guard | Where | What it does |
|---|---|---|
| Build on the oldest release | CI `DebStub` + `Deb` jobs, `container: ubuntu:22.04` | The floor becomes 22.04's. |
| `dpkg-shlibdeps` | `package_deb_leia.sh` | Versioned `Depends`, so apt *refuses* a too-new package instead of installing one that cannot load. |
| `STABLE_SONAMES` | `package_deb_leia.sh` | A newly linked library fails the build unless its 22.04-derived package name resolves on all three releases. |
| `DXR_DEB_MAX_GLIBC` | CI sets `2.35` | A floor above the oldest supported release is a hard error at package time. |
| `DebInstall` matrix | CI, `scripts/verify_deb_install_linux.sh` | Installs into pristine 22.04 / 24.04 / 26.04 containers and runs `ldd -r` + `displayxr-cli selftest` there. |

`libpipewire` is the interesting allowlist entry: the rule is *"the name
`dpkg-shlibdeps` derives **on the oldest supported release** resolves on all
three"*. The 24.04/26.04 package `libpipewire-0.3-0t64` carries
`Provides: libpipewire-0.3-0 (= <version>)`, so the 22.04-derived
`libpipewire-0.3-0 (>= 0.3.x)` is satisfied there. The reverse direction has no
fallback — which is exactly the v2.7.0 bug.

Track A (stub) is what makes all of this verifiable on every PR: the vendor SDK
is not available to public CI, but the stub `.so` links the same system
libraries, so the derived `Depends` line is representative. The Track B package
is verified the same way whenever CI can build it.

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

`test_deb_leia.sh --stub` builds in an `ubuntu:22.04` builder, then installs the
runtime `.deb` + the stub plug-in `.deb` together in a pristine container of
**every** supported release (`DXR_TEST_RELEASES` to narrow it) and asserts:
Depends resolve from that release's archive, `ldd -r` is clean, default →
sim-display claims, `DXR_LEIA_FORCE_PROBE=1` → leia-sr claims at probe_order 50.
It is the local twin of CI's `DebStub` + `DebInstall` jobs and shares their
verifier, `scripts/verify_deb_install_linux.sh`. **Real weave / claim over sim on
real hardware is validated on the SR box (`--sdk`), not in the stub path.**

## Open items before release

- ✅ **SR runtime package name = `leiasr-runtime`** — confirmed against LeiaSR
  `packaging/linux/deb/control.in` (ST-5525-linux-support branch; installs under
  `/opt/leiasr`). `Recommends: leiasr-runtime`.
- ✅ **SR-side integration gap CLOSED** — the `leiasr-runtime` .deb now registers
  `/etc/leia/sr/1/active_runtime.json` itself, so the srSDK loader's default
  resolution finds `libLeiaSR_runtime.so` with no `SR_RUNTIME_PATH` and no baked
  rpath. Verified on `1.37.0.6048+gb9262217a0` (Ubuntu 26.04, 2026-09-20). The
  same .deb is also the **dev** package (headers under `/opt/leiasr/include/sr/`,
  `libsrSDK_loader.a`, `lib/cmake/srSDK/srSDKConfig.cmake`), so `SRSDK_ROOT=/opt/leiasr`
  is all a Track B build needs — there is no separate SDK dev package.
- **Track B build + hardware acceptance.** Build + headless acceptance ✅ on the
  in-house Ubuntu 26.04 box (`displayxr-cli selftest` all-pass, `leia-sr` active —
  see `docs/linux-track-b-runbook.md`). **On-panel weave acceptance still pending**
  there; the weave itself is validated on 22.04/NVIDIA (#81).
- The `-DDXR_LEIA_SDK_DEV_RPATH=OFF` build option lands with the
  `linux-sdk-rpath-dev-only` branch; until it merges, the `patchelf` strip in the
  packager already produces a correct (rpath-free) release artifact.
- CI wiring + `versions.json` / meta-bundle inclusion (later; mirrors the
  runtime's out-of-scope list on #781).

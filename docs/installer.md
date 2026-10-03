# Windows installer: install and uninstall in any order

`DisplayXRLeiaSRSetup-<version>.exe` (`installer/DisplayXRLeiaSRInstaller.nsi`) installs
the plug-in DLL, its two bundled Vulkan weavers and an uninstaller into
`%ProgramFiles%\DisplayXR\Plugins\LeiaSR\`, and registers the plug-in under
`HKLM\Software\DisplayXR\DisplayProcessors\leia-sr` (`Binary`, `ProbeOrder` 50, …),
which is the runtime's discovery contract
([`plugin-discovery.md`](https://github.com/DisplayXR/displayxr-runtime/blob/main/docs/specs/runtime/plugin-discovery.md)).

The three pieces it relates to are installed independently and in any order:
the **LeiaSR platform runtime**, the **NeurD** 2D→3D runtime, and the **DisplayXR
runtime** (usually via the bundle). Design: DisplayXR/displayxr-runtime#1803.

## Prerequisites: none

| State at install time | What the installer does |
|---|---|
| DisplayXR runtime not installed | Installs and registers anyway, logs *"DisplayXR runtime not installed yet: the plug-in will be used once the DisplayXR runtime is installed."* The registration key may exist before the runtime does; the runtime finds it when it starts. |
| Runtime installed, `Version` **below** the plug-in's ABI floor | Refuses (exit **5**; message box only when not silent). The floor is `MIN_RUNTIME_VERSION`, derived from `DXR_RUNTIME_GIT_TAG` in `installer/CMakeLists.txt`. A runtime with no recorded `Version` is not gated. |
| Runtime installed, new enough | Installs. |
| LeiaSR platform absent | Installs. There is **deliberately no SR check**: the plug-in loads without SR and reports the platform as absent at run time, so SR can be installed before or after. |
| Not 64-bit Windows | Refuses (exit **3**). |

Exit 4 ("runtime absent") is retired. No message box can block a silent run: every one
carries `/SD`, and the old in-section runtime check with its unconditional modal (which hung
silent bundle chains) is gone.

## What it does to running processes, and why Restart Manager

The plug-in DLL and its weavers are mapped by `displayxr-service.exe` for the whole life of
the process, and by any in-process OpenXR app. A mapped DLL cannot be replaced (the install
then aborts — `AllowSkipFiles off`, #461) or deleted (the uninstall used to fail silently
and leave the file behind).

The installer releases them with the **Windows Restart Manager** — the generic mechanism
every Windows installer is expected to use — through a small helper it ships,
`dxr-rm-close.exe` (`installer/rm-helper/`, NSIS has no Restart Manager plug-in):

1. `RmStartSession` → `RmRegisterResources(<the plug-in's files in the install dir>)` →
   `RmGetList` → `RmShutdown(RmForceShutdown)`. The DisplayXR service is a well-behaved
   Restart Manager application (runtime PR #1793): it exits cleanly on the close query and
   registered `--rm-restart` for its relaunch. `RmForceShutdown` is required because a
   library in the service can own a window that never pumps messages, which makes a
   non-forced shutdown give up at once.
2. The files are replaced (install) or deleted (uninstall).
3. `RmRestart` relaunches exactly what was closed and had registered for restart — the
   service comes back **non-elevated** (an elevated `--rm-restart` instance hands itself to
   `explorer.exe`). Processes that never registered (most apps) are not relaunched.

Restart Manager lets only the process that started a session shut down and restart in it, so
`dxr-rm-close.exe session <dir> <files…>` is one process that spans both phases and talks to
the installer through files in `<dir>`: `closed.txt` (result of phase 1), `go.txt`
(`restart` or `end`, written by the installer), `done.txt`, and a `log.txt` that the
installer copies into its details view as `[rm] …` lines. If the installer dies before
writing `go.txt`, the helper restarts what it closed after 15 minutes anyway.

Rules the installer follows:

- **Nothing new is started.** If nothing held the files, nothing is closed and nothing is
  restarted. The service is started only if it was running before.
- **`/NOSTART`** (passed by the bundle, which restarts the service once at the end of its
  chain): nothing is started or restarted — the helper ends its session with `end`.
- **Fallback** only when Restart Manager itself fails, times out or the helper is missing:
  `taskkill /f /im displayxr-service.exe`, and afterwards, if the service was running before
  and is not running now, start it with `explorer.exe "<Runtime InstallPath>\displayxr-service.exe"`.
  Never a plain `Exec` of the service: from the elevated installer that produced an
  **elevated** service which normal-integrity apps cannot reach (the pre-#1803 behaviour).
- An aborted install (`.onInstFailed`) still releases the helper and restores the service.

## Uninstall

Works with or without the DisplayXR runtime installed.

1. Deletes `HKLM\Software\DisplayXR\DisplayProcessors\leia-sr` first, so a service that
   comes back finds no plug-in and runs on the fallback display processor.
2. Restart Manager close as above (from a copy of `dxr-rm-close.exe` in the uninstaller's
   temp dir). With no runtime there is no service; RM then finds nothing (or only
   in-process apps) and closes nothing else.
3. Deletes the files. A file that is still mapped (a holder RM could not close) is
   scheduled for deletion at the next reboot (`Delete /REBOOTOK`) and says so in the details
   view, instead of being left behind silently.
4. `RmRestart` — the service, if it was running, comes back without the plug-in.
5. Removes `HKLM\Software\DisplayXR\Plugins\LeiaSR` and the Add/Remove Programs entry.

## Checking a run

The installer's details view ("Show details") shows the helper's trace, e.g.

```
[rm] register: C:\Program Files\DisplayXR\Plugins\LeiaSR\DisplayXR-LeiaSR.dll
[rm] 1 process(es) hold the files (reboot reasons 0x0):
[rm]   pid 1234  DisplayXR Service  [-]  type=other-window restartable=1 status=0x1
[rm] RmShutdown(RmForceShutdown): success in 412 ms
[rm] RmRestart: success
```

and the service log of the restarted instance has
`Started by a Restart Manager restart (integrity RID 0x2000, elevated=0).`

`dxr-rm-close.exe list <file>…` prints who holds a file without closing anything;
`dxr-rm-close.exe is-running <image.exe> [wait-ms]` is what the installer uses to check
for the service. A test-only holder (`rm-test-holder`, built only on request:
`cmake --build build --config Release --target rm-test-holder`) holds a file, can register
for restart, and exits on the Restart Manager close query exactly like the service. Launch
it **non-elevated** (e.g. via `explorer.exe <a .bat>`): Restart Manager never restarts an
elevated process.

#!/usr/bin/env python3
"""Assert the SR SDK pins are identical in the local build script and in CI.

The Windows arm does not vendor the Leia SR SDK: it fetches it at build time
from release tags on the PRIVATE LeiaInc/SR-SDK-Windows-Releases-Internal-Public
repo. Which tags -- and which directory the v2 zip unpacks to -- is spelled out
FIVE times in each of TWO files, in two different languages:

  scripts/build-windows.bat         set SR_TAG=...          (batch)
  .github/workflows/build-windows.yml   SR_TAG: ...         (YAML, jobs.Build.env)

They are duplicated because neither file can read the other: the .bat is what a
developer runs on their own box, the workflow env is what CI runs, and there is
no shared config file between a batch script and a GitHub Actions env block.
Both files carry a "KEEP IN SYNC" comment, and until this script existed that
comment was the ENTIRE enforcement mechanism.

That is the incident this guards against. The runtime repo learned the lesson
already and has scripts/check_cnsdk_pins.py in its lint.yml for the CNSDK pins;
the SR pins -- which are edited by hand, in two files, every time the SDK is
repinned -- had nothing. A half-applied repin is silent and asymmetric: CI goes
on building the old SDK while the developer's box builds the new one (or the
reverse), the DLL compiles clean either way, and the mismatch only shows up as
"works on my machine / doesn't in the release" much later.

What is checked
---------------
1. Each of SR_TAG, SR_VKSTAMP_TAG, SR_V2_TAG, SR_V2_DIR, SR_SDK_REPO appears
   EXACTLY ONCE in the .bat, exactly once in build-windows.yml, and the two
   values are equal. "Exactly once" matters as much as "equal": a second
   `set SR_TAG=` further down the .bat silently wins and would make a
   first-occurrence comparison lie.

2. build-linux.yml's SR_SDK_REPO equals the Windows one -- all sr-sdk-v* assets
   (Windows SDK, Vulkan weaver rescue DLLs, the Linux SDK) live on the one
   private repo, so if it is ever renamed all three call sites move together.
   build-linux.yml's SR_TAG is DELIBERATELY NOT compared: the Linux srSDK is a
   separate release, versioned like the SR runtime (1.37.0.x), and is expected
   to differ from the Windows pin. See the comment above it in that file.

3. SR_V2_TAG and SR_V2_DIR describe the SAME SR v2 build. The tag encodes
   <major.minor.patch>.<build>, the directory encodes <major.minor.patch>+<build>
   plus a git sha:

       SR_V2_TAG: sr-sdk-v2-1.37.0.1502
       SR_V2_DIR: LeiaSR-SDK-1.37.0+1502.1b85d46d17-win64-Release
                               ^^^^^^ ^^^^

   This is the check that earns its keep on the next repin. Moving to build 1563
   means editing four lines across two files (tag and dir, twice), and changing
   the tag without the dir -- or vice versa -- produces a download that succeeds
   and an unpack directory that does not exist, or worse, an unpack of the NEW
   zip into a path still named after the OLD build.

Exit codes: 0 = every pin agrees.  1 = drift, a missing pin, or a duplicate.

Usage: python3 scripts/check_sr_pins.py
"""

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BAT = REPO_ROOT / "scripts" / "build-windows.bat"
WIN_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "build-windows.yml"
LINUX_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "build-linux.yml"

# The five pins that must be identical in the .bat and in build-windows.yml.
# NEURD_SDK_REF / NEURD_SDK_REPO pin the private NeurD headers the 2D->3D lift
# module is built against (docs/lift-neurd.md) -- same two-file duplication,
# same drift hazard, so they ride the same check.
PINS = ("SR_TAG", "SR_VKSTAMP_TAG", "SR_V2_TAG", "SR_V2_DIR", "SR_SDK_REPO", "NEURD_SDK_REF", "NEURD_SDK_REPO")

# sr-sdk-v2-1.37.0.1502  ->  ("1.37.0", "1502")
SR_V2_TAG_RE = re.compile(r"^sr-sdk-v2-(\d+\.\d+\.\d+)\.(\d+)$")
# LeiaSR-SDK-1.37.0+1502.1b85d46d17-win64-Release  ->  ("1.37.0", "1502")
SR_V2_DIR_RE = re.compile(r"^LeiaSR-SDK-(\d+\.\d+\.\d+)\+(\d+)\.")

errors = []


def fail(msg):
    """Emit a GitHub-annotated error and remember that we failed."""
    print(f"::error::{msg}")
    errors.append(msg)


def rel(path):
    return path.relative_to(REPO_ROOT).as_posix()


def read(path):
    if not path.exists():
        fail(f"{rel(path)} is missing.")
        return None
    # The tracked files use CRLF; MULTILINE '$' matches before the '\n' but
    # leaves the '\r', so every captured value is rstrip()-ed below.
    return path.read_text(encoding="utf-8")


def bat_values(text, key):
    """All values of `set KEY=value` in a batch file (value runs to EOL)."""
    pattern = re.compile(rf"^[ \t]*set[ \t]+{re.escape(key)}=(.*)$", re.MULTILINE)
    return [m.rstrip() for m in pattern.findall(text)]


def yaml_values(text, key):
    """All values of `KEY: value` in a YAML env block (no YAML library)."""
    pattern = re.compile(
        rf"^\s*{re.escape(key)}:\s*['\"]?([^'\"#\s]+)['\"]?\s*(?:#.*)?$", re.MULTILINE
    )
    return [m.rstrip() for m in pattern.findall(text)]


def one(values, key, path, how):
    """Require exactly one declaration; return it, or None after failing."""
    if not values:
        fail(
            f"{key} not found in {rel(path)}. Every SR SDK pin must be declared "
            f"there ({how}). Fix: re-add it, or -- if it was genuinely retired --"
            f" drop it from PINS in scripts/check_sr_pins.py in the same change, "
            "so the guard never silently stops guarding."
        )
        return None
    if len(values) > 1:
        fail(
            f"{key} is declared {len(values)} times in {rel(path)}: {values}. "
            f"The last one wins at build time, so a stale earlier line is "
            f"invisible. Fix: keep exactly one {key} declaration per file."
        )
        return None
    return values[0]


def main():
    bat_text = read(BAT)
    win_text = read(WIN_WORKFLOW)
    linux_text = read(LINUX_WORKFLOW)
    if bat_text is None or win_text is None or linux_text is None:
        return 1

    # --- 1. the five pins, .bat vs build-windows.yml -----------------------
    agreed = {}
    for key in PINS:
        bat_val = one(bat_values(bat_text, key), key, BAT, f"set {key}=<value>")
        win_val = one(yaml_values(win_text, key), key, WIN_WORKFLOW, f"{key}: <value>")
        if bat_val is None or win_val is None:
            continue
        if bat_val != win_val:
            fail(
                f"SR PIN DRIFT on {key}: {rel(BAT)} says '{bat_val}' but "
                f"{rel(WIN_WORKFLOW)} says '{win_val}'."
            )
            fail(
                f"Fix: decide which SDK drop is intended, then set {key} to that "
                f"one value in BOTH files. A repin edits two files by hand; this "
                "is the half-applied case -- CI and a developer box would fetch "
                "different SR SDKs and both would build clean."
            )
            continue
        agreed[key] = bat_val

    # --- 2. build-linux.yml shares the artifact repo (not the tag) ---------
    linux_repo = one(
        yaml_values(linux_text, "SR_SDK_REPO"),
        "SR_SDK_REPO",
        LINUX_WORKFLOW,
        "SR_SDK_REPO: <value>",
    )
    win_repo = agreed.get("SR_SDK_REPO")
    if linux_repo is not None and win_repo is not None and linux_repo != win_repo:
        fail(
            f"SR_SDK_REPO differs between arms: Windows uses '{win_repo}', "
            f"{rel(LINUX_WORKFLOW)} uses '{linux_repo}'."
        )
        fail(
            "Fix: every sr-sdk-v* asset -- Windows SDK zips, the Vulkan weaver "
            "rescue DLLs, and the Linux SDK -- lives on ONE private repo. Point "
            "all three call sites at it. (build-linux.yml's SR_TAG is a separate "
            "release and is intentionally NOT compared.)"
        )

    # --- 3. SR_V2_TAG and SR_V2_DIR name the same build --------------------
    v2_tag = agreed.get("SR_V2_TAG")
    v2_dir = agreed.get("SR_V2_DIR")
    v2_note = None
    if v2_tag is not None and v2_dir is not None:
        tag_m = SR_V2_TAG_RE.match(v2_tag)
        dir_m = SR_V2_DIR_RE.match(v2_dir)
        if not tag_m:
            fail(
                f"SR_V2_TAG '{v2_tag}' does not look like "
                "'sr-sdk-v2-<major.minor.patch>.<build>'. Fix: use the real tag "
                "name from the SR_SDK_REPO release, or update SR_V2_TAG_RE in "
                "scripts/check_sr_pins.py if the tag scheme really changed."
            )
        if not dir_m:
            fail(
                f"SR_V2_DIR '{v2_dir}' does not look like "
                "'LeiaSR-SDK-<major.minor.patch>+<build>.<gitsha>-win64-Release'. "
                "Fix: SR_V2_DIR must be the directory name the release's zip "
                "unpacks to, verbatim."
            )
        if tag_m and dir_m:
            if tag_m.groups() != dir_m.groups():
                fail(
                    f"SR v2 tag/dir mismatch: SR_V2_TAG '{v2_tag}' names version "
                    f"{tag_m.group(1)} build {tag_m.group(2)}, but SR_V2_DIR "
                    f"'{v2_dir}' names version {dir_m.group(1)} build "
                    f"{dir_m.group(2)}."
                )
                fail(
                    "Fix: a repin must move BOTH -- the tag is what `gh release "
                    "download` asks for, the dir is what the zip unpacks to and "
                    "what LEIASR_V2_SDKROOT points at. Changing one and not the "
                    "other either downloads an asset that isn't there or unpacks "
                    "the new SDK under the old build's name. Copy the directory "
                    "name from the release asset, don't retype it."
                )
            else:
                v2_note = f"SR v2 {tag_m.group(1)} build {tag_m.group(2)}"

    if errors:
        print(f"::error::{len(errors)} SR pin problem(s) above.")
        return 1

    width = max(len(k) for k in PINS)
    print("SR SDK pins agree. ✓")
    print(
        f"  scripts/build-windows.bat == .github/workflows/build-windows.yml:\n"
    )
    for key in PINS:
        print(f"    {key.ljust(width)}  {agreed[key]}")
    print()
    if v2_note:
        print(f"  SR_V2_TAG / SR_V2_DIR both name {v2_note}. ✓")
    print(
        f"  build-linux.yml SR_SDK_REPO matches. ✓ (its SR_TAG is a separate "
        "Linux release and is not compared.)"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    sys.exit(main())

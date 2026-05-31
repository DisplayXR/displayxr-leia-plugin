---
name: dxr-release
description: Tag-and-publish a release for any DisplayXR component repo (shell-pvt, leia-plugin, mcp, demo-*). Detects the repo from `git remote`, picks the right CI workflow + versions.json field, tags HEAD, watches CI to green, watches the dispatched versions-bump.yml on displayxr-runtime, reports the bump + mirror outcome. NOT for displayxr-runtime itself (use the in-repo /release there) and NOT for displayxr-installer (use /installer-release — workflow_dispatch-only flow).
---

# dxr-release — generic component release for DisplayXR siblings

## Why this exists

Every DisplayXR component repo except the runtime + installer follows
the same release shape now: tag `vX.Y.Z`, the repo's CI builds +
attaches an installer to a GitHub Release, then a `DispatchVersionsBump`
job fires a `repository_dispatch` at `displayxr-runtime/versions-bump.yml`
which (a) bumps the matching `versions.json` field, (b) mirrors the
file to `displayxr-installer/versions.json` via the publish-bot.

Spec: [`displayxr-runtime/docs/specs/runtime/versions-json-autobump.md`](https://github.com/DisplayXR/displayxr-runtime/blob/main/docs/specs/runtime/versions-json-autobump.md).

This skill keeps the user in the loop on the full flow without making
them watch the GH Actions UI. Single tag command → full bump + mirror
confirmation in the terminal.

## Syntax

```
/dxr-release vX.Y.Z
/dxr-release patch    # auto-bump from latest v[0-9]+.[0-9]+.[0-9]+ tag
/dxr-release minor
/dxr-release major
```

## Supported repos + their per-repo config

The skill inspects `git remote get-url origin` and uses one of these
configs:

| Repo | versions.json field | CI workflow file | Dispatcher job name |
|---|---|---|---|
| `DisplayXR/displayxr-shell-pvt` | `shell` | `publish-shell-releases.yml` | inline step in `publish` job |
| `DisplayXR/displayxr-leia-plugin` | `leia_plugin` | `build-windows.yml` | `DispatchVersionsBump` |
| `DisplayXR/displayxr-mcp` | `mcp_tools` | `build.yml` | `dispatch-versions-bump` |
| `DisplayXR/displayxr-demo-gaussiansplat` | `gauss_demo` | `build-windows.yml` | `DispatchVersionsBump` |

If `git remote get-url origin` doesn't match any of the above,
**STOP** and tell the user this skill doesn't apply — point them at
the in-repo `/release` for runtime or `/installer-release` for the
installer.

## CRITICAL: Launch Subagent

**Use the Agent tool with `subagent_type="general-purpose"`** to
execute the workflow. The subagent inherits the user's git context
and runs the multi-step poll without blocking the main thread.

### Subagent prompt template

```
Run the dxr-release skill at .claude/skills/dxr-release/SKILL.md.
Version-spec: [USER_ARG]
Repo (detected from `git remote get-url origin`): [REPO_FULL_NAME]
Use the per-repo config table for [REPO_FULL_NAME].
Report back the final state in the format defined in PHASE 6.
```

---

## PHASE 1: PRE-FLIGHT

### Step 1.1: Detect repo
```bash
REMOTE=$(git remote get-url origin)
case "$REMOTE" in
  *displayxr-shell-pvt*)            REPO=DisplayXR/displayxr-shell-pvt;            FIELD=shell;        WORKFLOW=publish-shell-releases.yml ;;
  *displayxr-leia-plugin*)          REPO=DisplayXR/displayxr-leia-plugin;          FIELD=leia_plugin;  WORKFLOW=build-windows.yml ;;
  *displayxr-mcp*)                  REPO=DisplayXR/displayxr-mcp;                  FIELD=mcp_tools;    WORKFLOW=build.yml ;;
  *displayxr-demo-gaussiansplat*)   REPO=DisplayXR/displayxr-demo-gaussiansplat;   FIELD=gauss_demo;   WORKFLOW=build-windows.yml ;;
  *displayxr-runtime*)              echo "Use the in-repo /release for runtime."; exit 1 ;;
  *displayxr-installer*)            echo "Use /installer-release for installer."; exit 1 ;;
  *)                                echo "Not a recognized DisplayXR sibling repo."; exit 1 ;;
esac
echo "repo=$REPO field=$FIELD workflow=$WORKFLOW"
```

### Step 1.2: Resolve version-spec
- `vX.Y.Z` literal → use as-is (validate `^v[0-9]+\.[0-9]+\.[0-9]+$`)
- `patch` / `minor` / `major` → compute from `git tag --sort=-creatordate | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' | head -1`

### Step 1.3: Clean tree + on main + tag not taken
```bash
[ -z "$(git status --porcelain)" ] || { echo "Working tree dirty"; exit 1; }
[ "$(git branch --show-current)" = "main" ] || { echo "Not on main"; exit 1; }
git fetch origin --tags --quiet
git rev-parse "$NEW_TAG" 2>/dev/null && { echo "Tag $NEW_TAG already exists"; exit 1; }
git pull --ff-only origin main
```

### Step 1.4: Previous tag (for release-notes diff)
```bash
PREV_TAG=$(git tag --sort=-creatordate | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' | head -1)
```

---

## PHASE 2: TAG

### Step 2.1: Tag HEAD and push
Annotated tag with a brief auto-generated body.
```bash
git tag -a "$NEW_TAG" -m "$NEW_TAG

Commits since $PREV_TAG:
$(git log --oneline --no-merges "$PREV_TAG..HEAD" 2>/dev/null | head -20)"
git push origin "$NEW_TAG"
```

---

## PHASE 3: WATCH THE REPO'S CI

### Step 3.1: Find the tag's CI run
Loop on `gh run list --workflow=$WORKFLOW --branch=$NEW_TAG` until a
run appears (usually <30s after push).

```bash
for i in $(seq 1 12); do
  RUN_ID=$(gh run list -R "$REPO" --workflow="$WORKFLOW" --branch="$NEW_TAG" \
            --limit=1 --json databaseId --jq '.[0].databaseId // empty')
  [ -n "$RUN_ID" ] && break
  sleep 10
done
[ -z "$RUN_ID" ] && { echo "No CI run found for $NEW_TAG"; exit 1; }
```

### Step 3.2: Poll the run to completion
Workflow-completion poll. Typical wall-clock: leia ~20min, mcp ~5min,
shell ~15min, gauss ~25min.

```bash
while :; do
  S=$(gh run view "$RUN_ID" -R "$REPO" --json status,conclusion \
        --jq '.status + "/" + (.conclusion // "?")')
  echo "  ci: $S"
  [[ "$S" == completed* ]] && break
  sleep 30
done
CI_CONC="${S#completed/}"
```

### Step 3.3: Branch on outcome
- `success` → Phase 4
- anything else → STOP, report the failed jobs to user via `gh run view --log`. Do NOT attempt rollback — tags are sticky; if the user wants to retry they delete the tag and retag.

---

## PHASE 4: WATCH THE DISPATCHED versions-bump RUN

After the repo's CI succeeds, its `DispatchVersionsBump` job fires a
`repository_dispatch` at `displayxr-runtime/versions-bump.yml`. Find
and follow that run.

### Step 4.1: Find the dispatched run
```bash
BUMP_RUN=""
for i in $(seq 1 12); do
  BUMP_RUN=$(gh run list -R DisplayXR/displayxr-runtime \
              --workflow=versions-bump.yml --event=repository_dispatch \
              --limit=3 --created=">$(date -u -v-15M +%Y-%m-%dT%H:%M:%SZ)" \
              --json databaseId --jq '.[0].databaseId // empty')
  [ -n "$BUMP_RUN" ] && break
  sleep 15
done
```

### Step 4.2: Poll to completion
```bash
while :; do
  S=$(gh run view "$BUMP_RUN" -R DisplayXR/displayxr-runtime --json status,conclusion \
        --jq '.status + "/" + (.conclusion // "?")')
  echo "  bump: $S"
  [[ "$S" == completed* ]] && break
  sleep 15
done
BUMP_CONC="${S#completed/}"
```

### Step 4.3: Branch on outcome
- `success` → Phase 5. **Caveat for `leia_plugin`**: success can mean
  "ABI gate passed and bump landed" OR "ABI gate failed and skipped
  bump + opened issue." Both exit 0. Detect by checking the field on
  `runtime/main` (Step 5.1).
- `failure` → bot push failed (bypass mis-configured, or push race).
  Report log link; recommend re-running `versions-bump.yml` manually
  via `workflow_dispatch` once the cause is understood.

---

## PHASE 5: VERIFY SYNC

### Step 5.1: Confirm runtime/main has the new pin
```bash
PINNED=$(gh api repos/DisplayXR/displayxr-runtime/contents/versions.json \
           --jq '.content' | base64 -d | jq -r ".${FIELD}")
if [ "$PINNED" = "$NEW_TAG" ]; then
  echo "✓ versions.json[$FIELD] = $NEW_TAG on runtime/main"
else
  # leia ABI gate likely fired. Check for the auto-opened issue.
  if [ "$FIELD" = "leia_plugin" ]; then
    ISSUE=$(gh issue list --repo DisplayXR/displayxr-leia-plugin \
              --state open --label abi-mismatch --search "$NEW_TAG" \
              --json number,url --jq '.[0]')
    echo "ABI gate skipped the bump. Tracking issue: $ISSUE"
  else
    echo "Bump did not land — versions.json[$FIELD] = $PINNED, expected $NEW_TAG"
  fi
fi
```

### Step 5.2: Confirm installer mirror landed
```bash
diff <(gh api repos/DisplayXR/displayxr-runtime/contents/versions.json   --jq '.content' | base64 -d) \
     <(gh api repos/DisplayXR/displayxr-installer/contents/versions.json --jq '.content' | base64 -d) \
  && echo "✓ installer mirror matches runtime" \
  || echo "✗ installer mirror drifted — check the Mirror step in $BUMP_RUN"
```

### Step 5.3: Capture commit SHAs for the report
```bash
RT_BUMP_SHA=$(gh api repos/DisplayXR/displayxr-runtime/commits/main --jq '.sha[0:8]')
IN_MIRROR_SHA=$(gh api repos/DisplayXR/displayxr-installer/commits/main --jq '.sha[0:8]')
```

---

## PHASE 6: REPORT

```
Release $NEW_TAG published successfully!

Repo:        $REPO
CI:          $RUN_ID — $CI_CONC
Release:     https://github.com/$REPO_RELEASE_TARGET/releases/tag/$NEW_TAG
             (shell-pvt → displayxr-shell-releases; everyone else → same repo)

Auto-bump:
  versions.json[$FIELD] = $NEW_TAG   via $RT_BUMP_SHA on displayxr-runtime/main
  versions.json mirror              via $IN_MIRROR_SHA on displayxr-installer/main
  [OR for leia ABI miss: "ABI gate skipped the bump — runtime currently expects
   ABI v$RT_ABI, this leia tag reports v$LEIA_ABI. Tracking issue: $ISSUE.
   Rebuild leia against displayxr-runtime/main, tag a new release; the next
   dispatch will clear the path."]

Commits since $PREV_TAG: N
  [first 5 commit oneliners]
```

STOP. Do NOT edit release notes here — sibling repos use the GH
Release's auto-generated notes; if the user wants curated notes
they'll run `gh release edit` themselves. Different cadence from the
runtime, where curated notes are the norm.

---

## Notes

- This skill assumes the `displayxr-publish-bot` GitHub App is
  installed on `displayxr-runtime` + `displayxr-installer` with
  Contents:write. Confirmed for all repos as of 2026-05-29.
- The repo's CI workflow must contain a `DispatchVersionsBump`-style
  step (per the spec). The four repos in the table above all have
  this as of the spec's rollout commits — see
  `docs/specs/runtime/versions-json-autobump.md` §"Sibling-side snippets"
  for what to add if a NEW sibling repo joins the family.
- Tags are sticky. If a release goes sideways, deleting the tag is
  destructive (deletes the GH Release too). Prefer fixing forward
  with a patch release.

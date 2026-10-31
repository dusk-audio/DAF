# DAF - Dusk Audio Framework

## Hard fork: nothing goes out, nothing comes in

Forked from DISTRHO/DPF at `4238e1c7` (2025-10-23) and developed independently since. The
relationship is severed in **both** directions, deliberately:

- **Outbound:** no pushes, no pull requests, no issues on DISTRHO/DPF. Fork-specific work is not
  submitted upstream even when it looks generally useful.
- **Inbound:** never `git merge upstream/*` or `git pull upstream`. This tree is owned here, and an
  upstream merge would drag in code that has not been through this repository's CI gates.

- **No reconnaissance either.** The `upstream` remote has been **removed** (owner ruling,
  2026-08-22). Do not re-add it, do not fetch DISTRHO/DPF, do not `git log upstream/*`, do not
  diff against it, do not read it to explain a behaviour here, and do not cite it as a source. A
  remote you must never fetch is a trap, not a safety net. If a bug turns up in inherited code,
  it is our bug: fix it here from the code in front of you.

The rename from DPF to DAF (2026-08-22) exists to make that unambiguous. Two repositories sharing
the name "DPF" kept sending people and tooling to DISTRHO's tree by mistake. This one is DAF.

Remotes are set up to match:

| remote   | repository     | push                              |
|----------|----------------|-----------------------------------|
| `origin` | dusk-audio/DAF | yes, the default for every branch |

There is no second remote, by design. `remote.pushDefault` is `origin` and `gh` resolves to
`dusk-audio/DAF`, so `git push`, `gh pr create` and `gh run list` all target this repository
without arguments. If a clone ever lacks this, run:

```sh
git remote set-url origin https://github.com/dusk-audio/DAF.git
git config remote.pushDefault origin
gh repo set-default dusk-audio/DAF
```

Attribution is a separate matter from isolation: DAF is a fork of DISTRHO's excellent work, says
so in README.md, and keeps every original copyright notice and the ISC terms. Not merging from
upstream is an engineering decision, not a claim of authorship.

## Commit messages

No AI attribution: no `Co-Authored-By: Claude ...`, no `🤖 Generated with [Claude Code]`, no
`Claude-Session:` trailer. Write the message as the author of the change.

Three layers, because the tooling adds these by default and asking it not to is not enough:

- `~/.claude/settings.json` blanks the attribution, so nothing appends it in the first place.
- `.git/hooks/commit-msg` strips the lines as commits are written, whatever wrote them.
- `.git/hooks/pre-push` refuses to publish a commit that still carries them, which catches anything
  arriving from another checkout, a rebase, an amend or a cherry-pick.

The history was rewritten on 2026-07-28 to remove the trailers that had already been pushed, so
every commit in this repository is clean. Neither hook survives a fresh clone: re-add both when
setting up a new checkout, or the guarantee is only as good as the settings file.

## Pull requests

PRs are opened against `dusk-audio/DAF` `main`. This repository takes same-repo PRs only, which is
why the workflows trigger on push to `**` rather than on `pull_request`.

**Never against DISTRHO/DPF.** Two were opened there by accident (#533, #534, both closed within a
minute) because GitHub's post-push banner and the VS Code GitHub extension default a new PR's base
to the *parent of the fork network*, which no git config can override. Guards in place:

- `.vscode/settings.json` restricts the VS Code extension to the `origin` remote and stops it
  offering a PR after every push.
- The repository left DISTRHO's fork network on 2026-07-28, so DISTRHO is no longer a selectable
  base anywhere in the UI: `gh api repos/dusk-audio/DAF --jq '{fork,parent}'` reports
  `{"fork": false, "parent": null}`. This is a standalone repository at the git-hosting level; the
  `upstream` remote it once carried has since been removed as well (see the hard-fork section),
  so there is nothing left pointing at DISTRHO from this checkout.
  If a future clone ever reports `"fork": true` again, something was re-created from the wrong
  place.

## CI

Three workflows, all triggered on push to any branch: `build.yml` (make, the compiler default plus
C++11 and C++20, the default UI type plus one explicit opengl3 leg), `cmake.yml` (cmake matrix
incl. a native ARM Linux runner and a single MSVC x64 leg) and `wayland.yml` (Wayland-only
container build, X11 regression, clap-validator, pluginval + LV2 validation). Anything relying on
external package repositories is deliberately kept out of them.

Each workflow runs on every push, documentation included. Its first job, `changes`, runs
`.github/code-changed.sh` to decide whether anything needs building: on a branch it compares the
branch with main, on main what the push added. When only Markdown, `.vscode/` or a root `LICENSE*`
changed, every other job skips itself. The last job, `build-passed`, `cmake-passed` or
`wayland-passed`, passes when every job before it passed or was skipped that way, and those three
are the only checks the "Protect main" ruleset requires. A job added to a workflow must be added
to that workflow's `*-passed` `needs` as well, or nothing requires it. Do not go back to
`paths-ignore` on the trigger: a push it filtered out ran no workflow, so the required checks were
never reported, and a pull request whose last push only touched documentation could not be merged
without an admin override.

Each workflow also has a `runtime-backend` job for the `DGL_BACKEND=both` build, which picks X11 or
Wayland at runtime (see `dgl/src/runtime-backend.md`). In `build.yml` it runs the tests under Xvfb,
in `cmake.yml` it builds with CMake and builds an out-of-tree consumer, and in `wayland.yml` it runs
the tests under a headless Weston, then with Xvfb and Weston together, checks what the build
links and exports, and embeds the VST2, VST3 and CLAP plugins with `tests/plugin-embed-host.cpp`,
which each have to refuse under Weston alone and accept with Xvfb as well. `wayland.yml` also has `runtime-backend-without-wayland`, which removes the
Wayland, xkbcommon and EGL libraries and runs the tests on X11, and its Wayland-only job runs the
unit tests under a headless Weston as well.

Run steps in a container job default to `sh`, which is dash on Ubuntu and has no `pipefail`, so a
job with `container:` whose steps need bash sets `defaults.run.shell: bash`.

## Vendored framework components

`dgl/src/pugl-upstream` and `widgets/` are ordinary tracked directories, never submodules;
`.gitmodules` no longer exists. They were brought in as git subtrees, but PR #34 was
squash-merged, so their earlier commits are not reachable from any ref here and there is
no subtree history to pull against. Their former repositories, `dusk-audio/pugl` and
`dusk-audio/DAF-Widgets`, are archived with README redirects to DAF and keep that history.
Ongoing development belongs here.

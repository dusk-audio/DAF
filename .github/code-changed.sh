#!/usr/bin/env bash
# Decides whether a CI run has to build and test, for the `changes` job of each workflow. Prints
# "code=true" or "code=false", for $GITHUB_OUTPUT.
#
# This replaces paths-ignore on the push trigger. A push that paths-ignore filtered out ran no
# workflow at all, so the checks main requires were never reported, and a pull request whose head
# commit only touched documentation could not be merged without an admin override. Every workflow
# now runs on every push, its jobs skip themselves when this prints code=false, and its *-passed
# job reports the result either way.
#
# Reads GITHUB_EVENT_NAME, GITHUB_REF and GITHUB_SHA, which the runner sets, and BEFORE, the commit
# a push to main moved it from. Needs the history and origin/main.
set -euo pipefail

# Documentation and editor/agent configuration, which cannot change a build result. Anything under
# .github/ is deliberately absent: a workflow change must run the workflow it changed.
not_code='\.md$|^\.vscode/|^LICENSE[^/]*$'

if [ "$GITHUB_EVENT_NAME" != push ]; then
    # A run started by hand is wanted in full.
    echo "code=true"
    exit 0
fi

if [ "$GITHUB_REF" = refs/heads/main ]; then
    # What this push added to main, a merged pull request's changes for a merge.
    base=$BEFORE
else
    # Everything the branch changes from main, not only this push: a push that touches only
    # documentation must not pass a branch whose code failed on the push before it.
    base=$(git merge-base origin/main "$GITHUB_SHA") || base=
fi

# --no-renames: detecting renames would fetch file contents, which the blobless clone leaves out,
# and a rename lists both of its paths this way anyway.
if [ -z "$base" ] || ! changed=$(git diff --no-renames --name-only "$base" "$GITHUB_SHA"); then
    echo "Could not tell what changed, so building and testing everything" >&2
    echo "code=true"
    exit 0
fi

# An empty diff builds and tests everything too.
if [ -n "$changed" ] && ! grep -qvE "$not_code" <<<"$changed"; then
    echo "Only documentation or editor configuration changed, so skipping the build and tests" >&2
    echo "code=false"
else
    echo "code=true"
fi

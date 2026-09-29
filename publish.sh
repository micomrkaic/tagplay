#!/bin/sh
# This file is part of tagplay.
# Copyright (C) 2026  Mico
# GPL-3.0-or-later; see COPYING.
#
# Publish tagplay to https://github.com/micomrkaic/tagplay
#
# Usage:
#   ./publish.sh                              # commit "update", push
#   ./publish.sh "message"                    # commit with message
#   ./publish.sh RELEASE.tar.gz "message"     # the whole ritual in one
#                                             # verb: sync to origin,
#                                             # unpack, gate, publish
#
# The tarball form replaces the by-hand sequence (fetch, reset, untar,
# build, publish) that has now been half-performed twice. The build
# gate is strict: a clean rebuild must produce zero warnings and zero
# errors or nothing is committed.
#
# The remote is set ONCE, on first run, to the HTTPS URL. An existing
# origin (whatever its URL) is left strictly alone.
set -e

# The tarball form REPLACES this very script on disk mid-run; run from
# a private copy so the shell never reads a half-new file. The copy
# must remember where the repo is -- its own dirname is /tmp.
if [ -z "$TAGPLAY_PUBLISH_DIR" ]; then
    TAGPLAY_PUBLISH_DIR="$(cd "$(dirname "$0")" && pwd)"
    export TAGPLAY_PUBLISH_DIR
    cp "$0" "/tmp/tagplay_publish_$$.sh"
    exec sh "/tmp/tagplay_publish_$$.sh" "$@"
fi
cd "$TAGPLAY_PUBLISH_DIR"

MSG=update
case "$1" in
*.tar.gz)
    TARBALL=$1
    [ -f "$TARBALL" ] || { echo "publish.sh: no such tarball: $TARBALL"; exit 1; }
    MSG=${2:-update}
    if [ -d .git ] && git remote get-url origin >/dev/null 2>&1; then
        echo "publish.sh: syncing to origin/main"
        git fetch origin
        git reset --hard origin/main
    fi
    echo "publish.sh: unpacking $TARBALL"
    tar xzf "$TARBALL" --strip-components=1
    # a tarball cannot delete files; a DELETIONS manifest can.
    if [ -f DELETIONS ]; then
        while IFS= read -r path; do
            case "$path" in
            ""|\#*) continue ;;
            /*|*..*) echo "publish.sh: refusing DELETIONS path: $path"
                     exit 1 ;;
            esac
            if [ -e "$path" ]; then
                echo "publish.sh: deleting (per manifest): $path"
                rm -f "$path"
            fi
        done < DELETIONS
        rm -f DELETIONS      # the manifest itself is never committed
    fi
    if [ -d .git ] && git diff --quiet && \
       [ -z "$(git status --porcelain)" ]; then
        echo "publish.sh: tarball introduces no changes (already published?)"
        exit 1
    fi
    ;;
*)
    [ -n "$1" ] && MSG=$1
    ;;
esac

if [ ! -d .git ]; then
    git init -b main
    MSG="tagplay: search-driven music player with audiotard DSP"
fi

cat > .gitignore <<'GITEOF'
tagplay
tagview
tagplay-gui
*.o
testlib/
phototest/
*.tar.gz
.publish_build.log
web/tagplay.js
web/tagplay.wasm
web/tagplay.worker.js
third_party/wasm/src/
third_party/wasm/lib/
third_party/wasm/include/
third_party/wasm/emcache/
third_party/wasm/emconfig
# never publish credentials, whatever they were named
refurbkey*
id_rsa* id_ecdsa* id_ed25519*
*.pem
*.key
GITEOF

# one-time heal: binaries that slipped into tracking before the
# ignore list knew their names
for bin in tagview tagplay-gui tagplay; do
    if git ls-files --error-unmatch "$bin" >/dev/null 2>&1; then
        echo "publish.sh: untracking leaked binary: $bin"
        git rm --cached -q "$bin"
    fi
done

# the gate: a CLEAN rebuild, zero errors, zero warnings, or no commit.
# (an incremental build can hide warnings in objects it skips; that
# hole has bitten once already)
echo "publish.sh: gate: make clean && make"
make clean >/dev/null
if ! make >.publish_build.log 2>&1; then
    echo "publish.sh: REFUSING to commit: the build fails."
    tail -20 .publish_build.log
    exit 1
fi
if grep -qE "warning|error" .publish_build.log; then
    echo "publish.sh: REFUSING to commit: the build is not warning-clean:"
    grep -E "warning|error" .publish_build.log | head -10
    exit 1
fi
rm -f .publish_build.log

git add -A

# refuse to publish anything that looks like a private key or token:
# scan the staged content itself, not just filenames
if git diff --cached | grep -qE -- "-----BEGIN (OPENSSH |RSA |EC |DSA )?PRIVATE KEY|ghp_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,}"; then
    echo "publish.sh: REFUSING to commit: staged content contains what looks"
    echo "like a private key or access token. Unstage it and try again:"
    git diff --cached --name-only | sed 's/^/    /'
    git reset >/dev/null
    exit 1
fi

git commit -m "$MSG" || echo "nothing to commit"

if ! git remote get-url origin >/dev/null 2>&1; then
    git remote add origin https://github.com/micomrkaic/tagplay.git
fi

git push -u origin main

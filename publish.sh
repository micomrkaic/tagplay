#!/bin/sh
# This file is part of tagplay.
# Copyright (C) 2026  Mico
# GPL-3.0-or-later; see COPYING.
#
# Publish tagplay to https://github.com/micomrkaic/tagplay
#
# Usage:
#   ./publish.sh                 # commit "update" and push
#   ./publish.sh "message"       # commit with your message and push
#
# The remote is set ONCE, on first run, to the HTTPS URL. An existing
# origin (whatever its URL) is left strictly alone, so a manually
# configured remote survives upgrades of this script.
set -e
cd "$(dirname "$0")"

MSG=${1:-update}

if [ ! -d .git ]; then
    git init -b main
    MSG="tagplay: search-driven music player with audiotard DSP"
fi

cat > .gitignore <<'GITEOF'
tagplay
src/*.o
testlib/
*.tar.gz
# never publish credentials, whatever they were named
refurbkey*
id_rsa* id_ecdsa* id_ed25519*
*.pem
*.key
GITEOF

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

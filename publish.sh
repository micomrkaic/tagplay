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
GITEOF

git add -A
git commit -m "$MSG" || echo "nothing to commit"

if ! git remote get-url origin >/dev/null 2>&1; then
    git remote add origin https://github.com/micomrkaic/tagplay.git
fi

git push -u origin main

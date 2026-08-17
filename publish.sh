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
# First run initializes the repo, sets the remote, and pushes main.
# Assumes the GitHub repo exists and is empty (or contains only files
# you are happy to merge). Uses SSH by default; export TAGPLAY_HTTPS=1
# to use the HTTPS remote instead.
set -e
cd "$(dirname "$0")"

MSG=${1:-update}
if [ "${TAGPLAY_HTTPS:-1}" = "1" ]; then
    REMOTE=https://github.com/micomrkaic/tagplay.git
else
    REMOTE=git@github.com:micomrkaic/tagplay.git
fi

if [ ! -d .git ]; then
    git init -b main
    MSG="tagplay: search-driven music player with audiotard DSP"
fi

cat > .gitignore <<'EOF'
tagplay
src/*.o
testlib/
*.tar.gz
EOF

git add -A
git commit -m "$MSG" || echo "nothing to commit"

if git remote get-url origin >/dev/null 2>&1; then
    git remote set-url origin "$REMOTE"
else
    git remote add origin "$REMOTE"
fi

git push -u origin main

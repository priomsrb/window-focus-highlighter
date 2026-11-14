#!/usr/bin/env bash
# Usage: ./release.sh 1.2.3
# Bumps version.h, commits, tags v1.2.3 and pushes; GitHub Actions builds and publishes the release.
set -euo pipefail
v="${1:?usage: release.sh X.Y.Z}"
[[ "$v" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]] || { echo "version must be X.Y.Z"; exit 1; }
[ -z "$(git status --porcelain)" ] || { echo "working tree not clean"; exit 1; }
[ "$(git branch --show-current)" = main ] || { echo "release from main"; exit 1; }
grep -qx "## v$v" CHANGELOG.md || { echo "add a '## v$v' section to CHANGELOG.md first"; exit 1; }
cat > version.h <<EOT
/* Single source of truth for the app version. Bump with release.sh. */
#define APP_VER_MAJOR ${BASH_REMATCH[1]}
#define APP_VER_MINOR ${BASH_REMATCH[2]}
#define APP_VER_PATCH ${BASH_REMATCH[3]}
#define APP_VER_STR "$v"
EOT
git add version.h
git commit -m "Release v$v"
git tag "v$v"
git push origin main "v$v"

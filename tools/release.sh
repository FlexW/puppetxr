#!/usr/bin/env bash
# tools/release.sh <version>
# Starts a release (docs/building.md, Releasing): on an up to date, clean main, sets the CMake
# project version, turns the Unreleased section of CHANGELOG.md into <version> with today's
# date, commits "release <version>", tags v<version> and pushes both. The tag starts
# .github/workflows/release.yml, which drafts the GitHub release.

set -euo pipefail

if [[ $# -ne 1 || ! "$1" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]
then
    echo "usage: $0 <major.minor.patch>" >&2
    exit 2
fi
version=$1
tag=v$version

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

fail()
{
    echo "error: $*" >&2
    exit 1
}

#
# Checks
#

[[ "$(git rev-parse --abbrev-ref HEAD)" == main ]] || fail "not on main"
[[ -z "$(git status --porcelain --untracked-files=no)" ]] || fail "uncommitted changes"
git fetch --quiet --tags origin main
[[ "$(git rev-parse HEAD)" == "$(git rev-parse origin/main)" ]] || fail "main differs from origin/main"
! git rev-parse --quiet --verify "refs/tags/$tag" > /dev/null || fail "tag $tag exists"

current=$(sed -n 's/^project(puppetxr VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
[[ -n "$current" ]] || fail "no project version in CMakeLists.txt"
if [[ "$(printf '%s\n%s\n' "$current" "$version" | sort -V | head -n 1)" != "$current" ]]
then
    fail "$version is older than the current version $current"
fi

grep -q '^## Unreleased$' CHANGELOG.md || fail "no '## Unreleased' heading in CHANGELOG.md"
notes=$(awk '/^## Unreleased$/ { f = 1; next } f && /^## / { exit } f' CHANGELOG.md)
grep -q '[^[:space:]]' <<< "$notes" || fail "nothing under '## Unreleased' in CHANGELOG.md"

#
# Confirm
#

last=$(git describe --tags --abbrev=0 --match 'v*' 2> /dev/null || true)
echo "Commits since ${last:-the start}:"
git log --oneline --no-decorate ${last:+"$last.."}HEAD
echo
echo "Release notes of $tag:"
echo "$notes"
echo
read -r -p "Release $version and push $tag? [y/N] " answer
[[ "$answer" == y || "$answer" == Y ]] || fail "aborted"

#
# Release
#

sed -i "s/^project(puppetxr VERSION $current /project(puppetxr VERSION $version /" CMakeLists.txt
sed -i "s/^## Unreleased\$/## Unreleased\n\n## $version - $(date -u +%Y-%m-%d)/" CHANGELOG.md
git add CMakeLists.txt CHANGELOG.md
git commit --quiet -m "release $version"
git tag -a "$tag" -m "puppetxr $version"
git push --atomic origin main "$tag"

echo "Pushed $tag; the release workflow drafts the GitHub release, publish it after checking it."

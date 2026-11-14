#!/usr/bin/env bash
# tools/dist/dist-src.sh
# Builds the source release from HEAD (docs/building.md, Source releases): build/dist-src/out/
# holds puppetxr-<version>-src.tar.gz (the tracked files with Git LFS content resolved, the
# dependencies in third_party/ among them, so it configures offline) and a .buildinfo with the
# commit and checksum. Same commit gives a byte-identical tarball. Needs git, git-lfs, tar, gzip,
# sha256sum.

set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
build=$root/build/dist-src
out=$build/out
stage=$build/stage

if [[ $# -ne 0 ]]
then
    echo "usage: $0" >&2
    exit 2
fi

commit=$(git -C "$root" rev-parse HEAD)
if [[ -n "$(git -C "$root" status --porcelain --untracked-files=no)" ]]
then
    echo "warning: uncommitted changes are not in the source release of $commit" >&2
fi
epoch=$(git -C "$root" log -1 --format=%ct)
version=$(git -C "$root" show HEAD:CMakeLists.txt | sed -n 's/^project(puppetxr VERSION \([0-9.]*\).*/\1/p')
if [[ -z "$version" ]]
then
    echo "error: no project version in CMakeLists.txt" >&2
    exit 1
fi
name=puppetxr-$version-src

umask 022
rm -rf "$stage" "$out"
mkdir -p "$stage" "$out"

#
# Tracked files
#

git -C "$root" archive --format=tar --prefix="$name/" HEAD | tar -x -C "$stage"

# git archive writes LFS pointers; smudge them from HEAD's pointers, not the working tree
while IFS= read -r path
do
    git -C "$root" show "HEAD:$path" | git -C "$root" lfs smudge "$path" > "$stage/$name/$path"
    if head -c 40 "$stage/$name/$path" | grep -q '^version https://git-lfs'
    then
        echo "error: $path is still an LFS pointer" >&2
        exit 1
    fi
done < <(git -C "$root" lfs ls-files --name-only)

#
# Package
#

tar --sort=name --mtime="@$epoch" --owner=0 --group=0 --numeric-owner --format=gnu \
    -C "$stage" -cf - "$name" | gzip -9n > "$out/$name.tar.gz"

{
    echo "commit: $commit"
    echo "sha256:"
    (cd "$out" && sha256sum ./*.tar.gz | sed 's/^/  /')
} > "$out/$name.buildinfo"
cat "$out/$name.buildinfo"

echo "Source release in $out"

#!/usr/bin/env bash
# tools/dist/dist-src.sh
# Builds the source release from HEAD (docs/building.md, Source releases): build/dist-src/out/
# holds puppetxr-<version>-src.tar.gz (the tracked files, Git LFS content resolved), the matching
# -src-full.tar.gz that adds the FetchContent dependencies of third_party/CMakeLists.txt under
# third_party/vendor/ so it configures offline, and a .buildinfo with the commit and checksums.
# Same commit gives byte-identical tarballs. Needs git, git-lfs, curl, tar, gzip, sha256sum.

set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
build=$root/build/dist-src
out=$build/out
stage=$build/stage
downloads=$build/downloads

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
mkdir -p "$stage" "$out" "$downloads"

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
# Dependencies, as pinned in third_party/CMakeLists.txt
#

mkdir -p "$stage/$name-full"
cp -a "$stage/$name/." "$stage/$name-full/"
vendor=$stage/$name-full/third_party/vendor
mkdir -p "$vendor"

deps=$(awk '
    /^FetchContent_Declare\(/ { sub(/^FetchContent_Declare\(/, ""); dep = $1 }
    dep != "" && $1 == "URL" { url = $2 }
    dep != "" && $1 == "URL_HASH" { sub(/^SHA256=/, "", $2); print dep, url, $2; dep = "" }
' "$stage/$name/third_party/CMakeLists.txt")
if [[ -z "$deps" ]]
then
    echo "error: no FetchContent_Declare in third_party/CMakeLists.txt" >&2
    exit 1
fi

while read -r dep url hash
do
    archive=$downloads/$dep-$hash.tar.gz
    if [[ ! -f "$archive" ]] || ! echo "$hash  $archive" | sha256sum -c --quiet - 2>/dev/null
    then
        curl -fsSL --retry 4 -o "$archive.part" "$url"
        mv "$archive.part" "$archive"
    fi
    if ! echo "$hash  $archive" | sha256sum -c --quiet -
    then
        echo "error: $url does not match its URL_HASH" >&2
        exit 1
    fi
    mkdir -p "$vendor/$dep"
    tar -xzf "$archive" -C "$vendor/$dep" --strip-components=1 --no-same-owner
done <<< "$deps"

#
# Package
#

for tree in "$name" "$name-full"
do
    tar --sort=name --mtime="@$epoch" --owner=0 --group=0 --numeric-owner --format=gnu \
        -C "$stage" -cf - "$tree" | gzip -9n > "$out/$tree.tar.gz"
done

{
    echo "commit: $commit"
    echo "dependencies:"
    while read -r dep url hash
    do
        echo "  $dep $url"
    done <<< "$deps"
    echo "sha256:"
    (cd "$out" && sha256sum ./*.tar.gz | sed 's/^/  /')
} > "$out/$name.buildinfo"
cat "$out/$name.buildinfo"

echo "Source release in $out"

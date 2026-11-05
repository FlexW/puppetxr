#!/usr/bin/env bash
# tools/dist/dist-macos.sh [--no-tests]
# Builds the macOS release on this Mac (docs/building.md, macOS releases): preset mac-dist, a
# universal arm64 and x86_64 build, then build/mac-dist/out/ holds
# puppetxr-<version>-macos-universal.tar.gz (library, puppetxrctl, header, manifest, LICENSE), the
# matching -debug.tar.gz with the .dSYM bundles and a .buildinfo with the toolchain versions and
# checksums. Two fresh runs of the same commit give byte-identical tarballs. Signing is ad hoc.
#
# PUPPETXR_DIST_CMAKE_ARGS adds configure arguments (e.g. FETCHCONTENT_SOURCE_DIR_* overrides).

set -euo pipefail

tests=1
for arg in "$@"
do
    case "$arg" in
        --no-tests)
            tests=0
            ;;
        *)
            echo "usage: $0 [--no-tests]" >&2
            exit 2
            ;;
    esac
done

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
build=$root/build/mac-dist
out=$build/out

commit=$(git -C "$root" rev-parse HEAD)
if [[ -n "$(git -C "$root" status --porcelain --untracked-files=no)" ]]
then
    echo "warning: uncommitted changes, the release will not match $commit" >&2
    commit="$commit-dirty"
fi
epoch=$(git -C "$root" log -1 --format=%ct)

umask 022
# Static archives carry no time stamps, so the linked binaries repeat
export ZERO_AR_DATE=1
# Fresh, so a build tree of an older toolchain does not keep its cached flags
# shellcheck disable=SC2086
cmake --preset mac-dist --fresh ${PUPPETXR_DIST_CMAKE_ARGS:-}
cmake --build --preset mac-dist
if [[ "$tests" == 1 ]]
then
    ctest --preset mac-dist -E '^samples' --output-on-failure
else
    # The release checks run even without the tests
    ctest --preset mac-dist --output-on-failure -R '^(shipped_binaries_need_only_system_libraries|dylib_check_rejects_other_libraries|dsym_holds_the_debug_info_.*)$'
fi

version=$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$build/CMakeCache.txt")
name=puppetxr-$version-macos-universal
stage=$build/stage
rm -rf "$stage" "$out"
mkdir -p "$stage/$name-debug" "$out"
cmake --install "$build" --prefix "$stage/$name"
codesign --force --sign - "$stage/$name/lib/libpuppetxr.dylib" "$stage/$name/bin/puppetxrctl"
cp -R "$build"/bin/*.dSYM "$stage/$name-debug/"

# One time stamp, owner 0, no Apple metadata, sorted entries, gzip without a name or time
find "$stage" -exec touch -h -t "$(date -r "$epoch" +%Y%m%d%H%M.%S)" {} +
for tree in "$name" "$name-debug"
do
    (cd "$stage" && find "$tree" -print0 | LC_ALL=C sort -z | \
        COPYFILE_DISABLE=1 xargs -0 tar --no-recursion --uid 0 --gid 0 --uname root --gname wheel --no-mac-metadata -cf - | gzip -9n > "$out/$tree.tar.gz")
done

{
    echo "commit: $commit"
    echo "macOS: $(sw_vers -productVersion) ($(sw_vers -buildVersion))"
    echo "toolchain:"
    clang --version | head -1 | sed 's/^/  /'
    echo "  $(cmake --version | head -1)"
    echo "  ninja $(ninja --version)"
    echo "  glslc $(glslc --version | head -1)"
    echo "  $(spirv-cross --revision 2>/dev/null | head -1 || echo spirv-cross)"
    echo "sha256:"
    (cd "$out" && shasum -a 256 ./*.tar.gz | sed 's/^/  /')
} > "$out/$name.buildinfo"
cat "$out/$name.buildinfo"
echo "Release in $out"

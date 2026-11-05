#!/usr/bin/env bash
# tools/dist/dist.sh [--no-tests]
# Builds the Linux release in the container of tools/dist/Dockerfile (docs/building.md, Linux
# releases): preset dist, glibc 2.28 enforced by the tests, then build/dist-linux/out/ holds
# puppetxr-<version>-linux-<arch>.tar.gz, the matching -debug.tar.gz and a .buildinfo with the
# toolchain versions and checksums. Same commit and image give byte-identical tarballs.
#
# DOCKER (default docker, podman works) picks the engine, PUPPETXR_DIST_DOCKER_ARGS adds run
# arguments (e.g. "--network host" behind a local proxy), PUPPETXR_DIST_CMAKE_ARGS adds configure
# arguments (e.g. FETCHCONTENT_SOURCE_DIR_* overrides).

set -euo pipefail

MAX_GLIBC=2.28

#
# Inside the container
#

if [[ "${1:-}" == "--inside" ]]
then
    umask 022
    build=/src/build/dist-linux
    out=$build/out
    # Fresh, so a build tree of an older image does not keep that image's cached flags
    # shellcheck disable=SC2086
    cmake --preset dist -B "$build" --fresh -DPUPPETXR_MAX_GLIBC=$MAX_GLIBC ${PUPPETXR_DIST_CMAKE_ARGS:-}
    cmake --build "$build"
    if [[ "$PUPPETXR_DIST_TESTS" == 1 ]]
    then
        ctest --test-dir "$build" --output-on-failure -j "$(nproc)"
    else
        # The portability check runs even without the tests
        ctest --test-dir "$build" --output-on-failure -R '^(shipped_binaries_need_only_glibc|glibc_check_rejects_newer_glibc)$'
    fi

    version=$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$build/CMakeCache.txt")
    name=puppetxr-$version-linux-$(uname -m)
    stage=$build/stage
    rm -rf "$stage" "$out"
    mkdir -p "$stage/$name-debug" "$out"
    cmake --install "$build" --prefix "$stage/$name"
    cp "$build"/bin/*.debug "$stage/$name-debug/"
    for tree in "$name" "$name-debug"
    do
        tar --sort=name --mtime="@$SOURCE_DATE_EPOCH" --owner=0 --group=0 --numeric-owner --format=gnu \
            -C "$stage" -cf - "$tree" | gzip -9n > "$out/$tree.tar.gz"
    done

    {
        echo "commit: $PUPPETXR_DIST_COMMIT"
        echo "image layers: sha256:$PUPPETXR_DIST_IMAGE"
        echo "max glibc: $MAX_GLIBC"
        echo "packages:"
        rpm -q glibc gcc-toolset-15-gcc gcc-toolset-15-libstdc++-devel clang lld llvm cmake ninja-build | sed 's/^/  /'
        echo "glslc:"
        glslc --version | sed 's/^/  /'
        echo "sha256:"
        (cd "$out" && sha256sum ./*.tar.gz | sed 's/^/  /')
    } > "$out/$name.buildinfo"
    cat "$out/$name.buildinfo"
    exit 0
fi

#
# On the host
#

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
docker=${DOCKER:-docker}
image=puppetxr-dist:$(sha256sum "$root/tools/dist/Dockerfile" | cut -c1-16)

commit=$(git -C "$root" rev-parse HEAD)
if [[ -n "$(git -C "$root" status --porcelain --untracked-files=no)" ]]
then
    echo "warning: uncommitted changes, the release will not match $commit" >&2
    commit="$commit-dirty"
fi
epoch=$(git -C "$root" log -1 --format=%ct)

"$docker" build -t "$image" "$root/tools/dist"
# The layers, not the image id: BuildKit's provenance attestation gives every build of the same
# layers a new id
image_layers=$("$docker" image inspect --format '{{range .RootFS.Layers}}{{.}} {{end}}' "$image" | sha256sum | cut -d' ' -f1)

# As the calling user, so the build tree stays theirs
# shellcheck disable=SC2086
"$docker" run --rm \
    --user "$(id -u):$(id -g)" \
    -e HOME=/tmp \
    -e SOURCE_DATE_EPOCH="$epoch" \
    -e PUPPETXR_DIST_TESTS="$tests" \
    -e PUPPETXR_DIST_COMMIT="$commit" \
    -e PUPPETXR_DIST_IMAGE="$image_layers" \
    -e PUPPETXR_DIST_CMAKE_ARGS="${PUPPETXR_DIST_CMAKE_ARGS:-}" \
    -v "$root:/src" \
    ${PUPPETXR_DIST_DOCKER_ARGS:-} \
    "$image" bash /src/tools/dist/dist.sh --inside

echo "Release in $root/build/dist-linux/out"

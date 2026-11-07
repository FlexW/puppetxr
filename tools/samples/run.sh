#!/bin/sh
# Runs the sample tests (docs/testing.md "Sample apps") in the pinned image of
# tools/samples/Dockerfile, so references match on every machine and in CI.
#
#   tools/samples/run.sh [sample_tests.py args]   build into build/docker-samples, run the tests,
#                                                 e.g. --tests godot_composition_layers_gl --update
#   tools/samples/run.sh --shell                  a shell in the image at the checkout
#   tools/samples/run.sh --unit [preset]          configure, build and run a preset's unit tests
#                                                 (default gcc-debug) under Xvfb and lavapipe
#
# The image is x86_64, also on ARM hosts (emulated), since llvmpipe's pixels depend on the CPU.
# The checkout is mounted at its own path and the container runs as the calling user. A proxy in
# HTTPS_PROXY and a CA in SSL_CERT_FILE are passed through, for the build and for downloads.
# PUPPETXR_CMAKE_ARGS adds configure arguments (e.g. FETCHCONTENT_SOURCE_DIR_<dep> for local
# dependency checkouts), PUPPETXR_DOCKER_ARGS docker run arguments (e.g. -v to mount them).
# PUPPETXR_CCACHE=1 compiles through ccache, its cache under $build/home.

set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
image=puppetxr-samples
build=build/docker-samples

net=""
secret=""
if [ -n "${HTTPS_PROXY:-${https_proxy:-}}" ]; then
    net="--network host"
    secret="--build-arg HTTPS_PROXY=${HTTPS_PROXY:-${https_proxy:-}} --build-arg https_proxy=${HTTPS_PROXY:-${https_proxy:-}}"
fi
if [ -n "${SSL_CERT_FILE:-}" ] && [ -s "$SSL_CERT_FILE" ]; then
    secret="$secret --secret id=ca,src=$SSL_CERT_FILE"
fi
# CI loads the image from its cache (PUPPETXR_SAMPLES_IMAGE_READY); a build there would start over
if [ -z "${PUPPETXR_SAMPLES_IMAGE_READY:-}" ]; then
    # shellcheck disable=SC2086
    docker build -q --platform linux/amd64 $net $secret -t "$image" "$root/tools/samples" >/dev/null
fi

mounts="-v $root:$root"
env="-e HOME=$root/$build/home"
for var in HTTPS_PROXY https_proxy HTTP_PROXY http_proxy NO_PROXY no_proxy; do
    eval "value=\${$var:-}"
    if [ -n "$value" ]; then
        env="$env -e $var=$value"
    fi
done
if [ -n "${SSL_CERT_FILE:-}" ] && [ -s "$SSL_CERT_FILE" ]; then
    mounts="$mounts -v $SSL_CERT_FILE:/etc/puppetxr-proxy-ca.crt:ro"
    env="$env -e SSL_CERT_FILE=/etc/puppetxr-proxy-ca.crt -e GIT_SSL_CAINFO=/etc/puppetxr-proxy-ca.crt -e CURL_CA_BUNDLE=/etc/puppetxr-proxy-ca.crt"
fi
tty=""
if [ -t 0 ] && [ -t 1 ]; then
    tty="-it"
fi
if [ -n "${PUPPETXR_CCACHE:-}" ]; then
    launcher="-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
    PUPPETXR_CMAKE_ARGS="${PUPPETXR_CMAKE_ARGS:-} $launcher"
fi
mkdir -p "$root/$build/home"
run() {
    # shellcheck disable=SC2086
    docker run --rm --platform linux/amd64 $tty $net $mounts $env ${PUPPETXR_DOCKER_ARGS:-} --user "$(id -u):$(id -g)" -w "$root" "$image" "$@"
}

if [ "${1:-}" = "--shell" ]; then
    run bash
    exit
fi
if [ "${1:-}" = "--unit" ]; then
    run sh -c "Xvfb :99 -screen 0 1920x1080x24 >/dev/null 2>&1 &
        export DISPLAY=:99 VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json LIBGL_ALWAYS_SOFTWARE=1
        cmake --preset \"\$1\" ${PUPPETXR_CMAKE_ARGS:-} >/dev/null && cmake --build --preset \"\$1\" && ctest --preset \"\$1\" -E '^samples\$' -j\"\$(nproc)\"" sh "${2:-gcc-debug}"
    exit
fi
run sh -c "cmake -S . -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPUPPETXR_BUILD_SAMPLES=ON -DPUPPETXR_BUILD_TESTS=OFF ${PUPPETXR_CMAKE_ARGS:-} >/dev/null \
    && cmake --build $build \
    && python3 tools/sample_tests.py --samples $build/samples --runtime $build/puppetxr-dev.json --puppetxrctl $build/bin/puppetxrctl \
        --references tests/samples --out $build/samples_out --tolerance 1 \"\$@\"" sh "$@"

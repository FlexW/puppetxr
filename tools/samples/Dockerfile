# The sample test environment (tools/samples/run.sh, docs/testing.md "Sample apps"): the
# references are bit-exact only for one Mesa and LLVM, so every package comes from one dated
# snapshot of the Arch Linux Archive: Mesa 26.2.4 (llvmpipe, lavapipe) and LLVM 23.1.1. The base
# image is pinned by digest on ghcr.io, which does not rate-limit anonymous pulls.

FROM ghcr.io/archlinux/archlinux:base@sha256:09d7c61fa6a0b6ec21020d75dec1cf901208ee601407fe6ef0f7988c05b94d19

# Behind a TLS-inspecting proxy, run.sh passes its CA as the secret "ca". pacman's download
# sandbox fails under emulation (Rosetta rejects its seccomp filter), so it is off: it only
# isolates the downloads, the packages are the same
ARG SNAPSHOT=2026/10/15
RUN --mount=type=secret,id=ca,required=false \
    if [ -s /run/secrets/ca ]; then cp /run/secrets/ca /etc/ca-certificates/trust-source/anchors/proxy.crt && update-ca-trust; fi \
 && echo "Server = https://archive.archlinux.org/repos/${SNAPSHOT}/\$repo/os/\$arch" > /etc/pacman.d/mirrorlist \
 && pacman -Syyuu --noconfirm --disable-sandbox \
 && pacman -S --noconfirm --needed --disable-sandbox \
        gcc clang lld llvm cmake ninja git git-lfs python pkgconf shaderc glslang \
        libx11 libxext libxcursor libxi libxfixes libxrandr libxss libxtst libxinerama wayland wayland-protocols libxkbcommon \
        libglvnd mesa vulkan-swrast vulkan-icd-loader xorg-server-xvfb fontconfig zlib \
        libxxf86vm sdl2-compat meson glm tinyxxd glib2 vulkan-headers \
 && pacman -Scc --noconfirm \
 && test "$(pacman -Q mesa)" = "mesa 1:26.2.4-1" \
 && test "$(pacman -Q vulkan-swrast)" = "vulkan-swrast 1:26.2.4-1" \
 && test "$(pacman -Q llvm-libs)" = "llvm-libs 23.1.1-1"

# The checkout is mounted at the same path as on the host, owned by the host user
RUN git config --system --add safe.directory '*'

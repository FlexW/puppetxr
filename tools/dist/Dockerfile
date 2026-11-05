# The Linux release build environment (tools/dist/dist.sh, docs/building.md): AlmaLinux 8 for
# glibc 2.28, so the binaries run on every distribution since 2018, with today's compilers from
# gcc-toolset and llvm-toolset. The base image is pinned by digest and glslc by commit; the dnf
# packages follow AlmaLinux 8 updates, so dist.sh records their versions next to each release.

FROM docker.io/library/almalinux:8@sha256:8b469a3a78515e8a18ea8fc727e6a3679e1d0c5ba6f58d5b30be3d0d9b86cffe

RUN dnf -y install dnf-plugins-core \
 && dnf config-manager --set-enabled powertools \
 && dnf -y install \
        gcc-toolset-15-gcc gcc-toolset-15-gcc-c++ gcc-toolset-15-libstdc++-devel gcc-toolset-15-binutils \
        clang lld llvm \
        cmake ninja-build git python3 pkgconf-pkg-config tar gzip findutils \
        libX11-devel libXext-devel libXcursor-devel libXi-devel libXfixes-devel libXrandr-devel \
        libXScrnSaver-devel libXtst-devel wayland-devel wayland-protocols-devel libxkbcommon-devel \
        mesa-libGL-devel mesa-libEGL-devel \
        mesa-vulkan-drivers mesa-dri-drivers vulkan-loader \
 && dnf clean all

ENV PATH=/opt/rh/gcc-toolset-15/root/usr/bin:/usr/local/bin:$PATH

# glslc is not packaged for EL8. Pinned because its version decides the embedded SPIR-V bytes.
ARG SHADERC_TAG=v2026.4
ARG SHADERC_COMMIT=b64fd42c4a2a9d501c47aca5d3571f7673a5e9cc
RUN git -c advice.detachedHead=false clone --depth 1 --branch ${SHADERC_TAG} https://github.com/google/shaderc /tmp/shaderc \
 && test "$(git -C /tmp/shaderc rev-parse HEAD)" = "${SHADERC_COMMIT}" \
 && python3 /tmp/shaderc/utils/git-sync-deps \
 && cmake -S /tmp/shaderc -B /tmp/shaderc/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON \
        -DSPIRV_SKIP_EXECUTABLES=ON -DENABLE_GLSLANG_BINARIES=OFF \
 && ninja -C /tmp/shaderc/build glslc_exe \
 && install -m 755 /tmp/shaderc/build/glslc/glslc /usr/local/bin/glslc \
 && rm -rf /tmp/shaderc

# clang builds against the gcc-toolset's libstdc++ (its static archive is linked into the
# runtime): /etc/clang/*.cfg of the clang package names the toolset with --gcc-install-dir, which
# overrides --gcc-toolchain, so the toolset installed above must be the one it names. lld links,
# which full LTO needs.
ENV LDFLAGS=-fuse-ld=lld

# The EL8 Mesa enables lavapipe, which the tests run on, only with RH_SW_VULKAN set; without it
# vkCreateInstance of the driver fails silently
ENV RH_SW_VULKAN=1

WORKDIR /src

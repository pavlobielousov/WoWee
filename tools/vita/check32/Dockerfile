# VITA-37 spike: 32-bit ARM (armhf) cross-build + qemu-user test runner, on top of the desktop builder image
# (tools/vita/desktop-builder.Dockerfile, built by `desktop_check.sh image`). Proof of concept, see DEV_SETUP.md section 10.
# Build:  container build -f tools/vita/check32/Dockerfile -t wowee-armhf tools/vita/check32
FROM wowee-desktop-builder
# armhf comes from ports.ubuntu.com; the arm64 sources must be pinned to arm64 so apt does not look for armhf there.
RUN dpkg --add-architecture armhf && \
    sed -i 's/^Types: deb/Types: deb\nArchitectures: arm64/' /etc/apt/sources.list.d/ubuntu.sources && \
    printf 'Types: deb\nURIs: http://ports.ubuntu.com/ubuntu-ports\nSuites: noble noble-updates noble-security\nComponents: main universe\nArchitectures: armhf\nSigned-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg\n' > /etc/apt/sources.list.d/armhf.sources && \
    apt-get update && \
    apt-get install -y --no-install-recommends gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf qemu-user \
        libssl-dev:armhf zlib1g-dev:armhf && \
    rm -rf /var/lib/apt/lists/*
COPY armhf.cmake /opt/armhf.cmake
# SDL3 for armhf, with no video backend (SDL_UNIX_CONSOLE_BUILD): the tests only need the library to link.
RUN git clone --depth 1 --branch release-3.2.24 https://github.com/libsdl-org/SDL.git /tmp/SDL3 && \
    cmake -S /tmp/SDL3 -B /tmp/SDL3/build -G Ninja -DCMAKE_TOOLCHAIN_FILE=/opt/armhf.cmake -DCMAKE_BUILD_TYPE=Release \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_UNIX_CONSOLE_BUILD=ON \
        -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_VULKAN=OFF \
        -DSDL_PULSEAUDIO=OFF -DSDL_PIPEWIRE=OFF -DSDL_ALSA=OFF -DSDL_JACK=OFF -DSDL_SNDIO=OFF \
        -DCMAKE_INSTALL_PREFIX=/usr/arm-linux-gnueabihf && \
    cmake --build /tmp/SDL3/build && cmake --install /tmp/SDL3/build && rm -rf /tmp/SDL3
# Empty stand-in for libvulkan: the root CMake always makes the packet tests link Vulkan::Vulkan, and the host
# (arm64) libvulkan.so is the wrong architecture. No Vulkan code runs in those tests.
RUN mkdir -p /opt/stub && echo 'void wowee_vulkan_stub(void){}' > /tmp/v.c && \
    arm-linux-gnueabihf-gcc -shared -o /opt/stub/libvulkan.so /tmp/v.c && rm /tmp/v.c

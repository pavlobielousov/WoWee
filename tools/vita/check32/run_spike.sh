#!/bin/sh
# VITA-37 spike driver (proof, not the finished check32.sh): inside the wowee-armhf image, cross-configure the whole
# tree for armhf, build every test_* target, run ctest under qemu-arm. Run from the WoWee root:
#   container run --rm -i --memory 10G --cpus 8 --entrypoint /bin/bash -v "$PWD:/workspace" wowee-armhf -s < tools/vita/check32/run_spike.sh
# Excluded from ctest: sweep_guard (host Python sweeps, not 32-bit relevant, exceeds a 120 s timeout).
cd /workspace && mkdir -p build-armhf
L=/usr/lib/arm-linux-gnueabihf
# The explicit library paths matter: CMake otherwise picks the arm64 libssl/libz/libvulkan from /usr/lib/aarch64-linux-gnu.
cmake -S . -B build-armhf -G Ninja -DCMAKE_BUILD_TYPE=Release -DWOWEE_BUILD_TESTS=ON -DCMAKE_TOOLCHAIN_FILE=/opt/armhf.cmake \
  -DOPENSSL_SSL_LIBRARY=$L/libssl.so -DOPENSSL_CRYPTO_LIBRARY=$L/libcrypto.so -DOPENSSL_INCLUDE_DIR=/usr/include \
  -DZLIB_LIBRARY=$L/libz.so -DZLIB_INCLUDE_DIR=/usr/include \
  -DVulkan_LIBRARY=/opt/stub/libvulkan.so -DVulkan_INCLUDE_DIR=/usr/include > build-armhf/configure.log 2>&1 || { echo "configure failed"; exit 1; }
T=$(ninja -C build-armhf -t targets all | grep -oE '^test_[a-z0-9_]+' | sort -u | tr '\n' ' ')
ninja -C build-armhf -k 0 $T 2>&1 | tail -3
cd build-armhf && ctest -j8 -E sweep_guard --timeout 120 2>&1 | tail -15

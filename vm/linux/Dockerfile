# Environment A: Linux image with the pinned NTFS-3G + ntfsprogs (T-M1-05).
# Build through vm/linux/run.sh, which fetches and checksum-verifies the tarball with
# scripts/fetch-deps.sh and passes it in the build context. Nothing is downloaded here.
#
# Base: debian:13-slim, multi-arch index digest resolved 2026-11-13
# (docker buildx imagetools inspect debian:13-slim).
FROM debian:13-slim@sha256:a72b4433de1cf7d6bae0c4cee74d0547f58601a643b3375f070c51600d63b6b2

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential clang lld libclang-rt-dev cmake ninja-build pkg-config \
        autoconf automake libtool \
        libgcrypt20-dev libgnutls28-dev uuid-dev libattr1-dev \
        fuse3 kmod util-linux \
        python3 xxd coreutils file ca-certificates git gdb valgrind \
    && rm -rf /var/lib/apt/lists/*

# 1 = build NTFS-3G with AddressSanitizer + UBSan.
ARG ASAN=0
# Tarball name and digest come from third_party/deps.lock (passed by run.sh).
ARG NTFS3G_FILE
ARG NTFS3G_SHA256
ENV NTFS3G_PREFIX=/opt/ntfs-3g

COPY ${NTFS3G_FILE} /usr/src/${NTFS3G_FILE}
RUN set -eux; \
    cd /usr/src; \
    echo "${NTFS3G_SHA256}  ${NTFS3G_FILE}" | sha256sum -c -; \
    mkdir ntfs-3g-src; tar -xzf "${NTFS3G_FILE}" -C ntfs-3g-src --strip-components=1; \
    cd ntfs-3g-src; \
    if [ "$ASAN" = 1 ]; then \
        export CFLAGS="-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer" \
               LDFLAGS="-fsanitize=address,undefined"; \
    else \
        export CFLAGS="-g -O2"; \
    fi; \
    ./configure --prefix="${NTFS3G_PREFIX}" --with-fuse=internal \
        --enable-ntfsprogs --disable-ldconfig --disable-static \
        --disable-mount-helper --disable-plugins; \
    make -j"$(nproc)"; \
    make install; \
    cd /; rm -rf /usr/src/ntfs-3g-src "/usr/src/${NTFS3G_FILE}"; \
    echo "${NTFS3G_PREFIX}/lib" > /etc/ld.so.conf.d/ntfs-3g.conf; ldconfig

ENV PATH=/opt/ntfs-3g/bin:/opt/ntfs-3g/sbin:$PATH \
    PKG_CONFIG_PATH=/opt/ntfs-3g/lib/pkgconfig \
    ASAN_OPTIONS=detect_leaks=1:abort_on_error=0:log_path=/tmp/asan/asan \
    UBSAN_OPTIONS=print_stacktrace=1:log_path=/tmp/asan/ubsan \
    NTFS_ASAN=${ASAN}
RUN mkdir -p /tmp/asan && chmod 1777 /tmp/asan
WORKDIR /work
CMD ["bash"]

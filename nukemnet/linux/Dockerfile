FROM debian:bullseye-slim
ARG TARGET_ARCH=armv7l
ENV TARGET_ARCH=$TARGET_ARCH
ENV DEBIAN_FRONTEND=noninteractive
RUN printf '%s\n' \
    'deb http://archive.debian.org/debian bullseye main' \
    'deb-src http://archive.debian.org/debian bullseye main' \
    'deb http://archive.debian.org/debian-security bullseye-security main' \
    'deb-src http://archive.debian.org/debian-security bullseye-security main' > /etc/apt/sources.list \
 && case "$TARGET_ARCH" in x64) DEB_ARCH=amd64; COMPILER=g++ ;; arm64) DEB_ARCH=arm64; COMPILER=g++-aarch64-linux-gnu ;; armv7l) DEB_ARCH=armhf; COMPILER=g++-arm-linux-gnueabihf ;; *) exit 1 ;; esac \
 && dpkg --add-architecture "$DEB_ARCH" \
 && apt-get -o Acquire::Check-Valid-Until=false update \
 && apt-get install -y --no-install-recommends ca-certificates curl autoconf automake make patch \
    "$COMPILER" pkg-config "libsdl2-dev:$DEB_ARCH" "libsdl2-net-dev:$DEB_ARCH" \
    "libudev1:$DEB_ARCH" "libslirp-dev:$DEB_ARCH" "libpng-dev:$DEB_ARCH" "zlib1g-dev:$DEB_ARCH" "libasound2-dev:$DEB_ARCH" python3 zip \
    "libwayland-dev:$DEB_ARCH" "libxkbcommon-dev:$DEB_ARCH" "libegl1-mesa-dev:$DEB_ARCH" wayland-protocols libwayland-bin \
 && rm -rf /var/lib/apt/lists/*
COPY . /recipe/
RUN sh /recipe/build.sh
CMD ["sh", "-c", "cp /build/output/* /output/"]

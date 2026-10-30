# The build of the GitHub Actions workflow on Linux (ubuntu-22.04), to try
# it locally: the same compiler, CMake, Vulkan SDK, FFmpeg and AppImage tools
# (see build.sh).
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y \
    build-essential g++-12 pkg-config ninja-build git wget xz-utils python3-pip rsync file appstream patchelf \
    libasound2-dev libpulse-dev libaudio-dev libjack-dev libsndio-dev libpipewire-0.3-dev \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev \
    libxtst-dev libxkbcommon-dev libdrm-dev libgbm-dev libgl1-mesa-dev libgles2-mesa-dev \
    libegl1-mesa-dev libdbus-1-dev libibus-1.0-dev libudev-dev libthai-dev libusb-1.0-0-dev \
    libwayland-dev libdecor-0-dev liburing-dev libfribidi-dev \
    && rm -rf /var/lib/apt/lists/*
RUN wget -q https://github.com/Kitware/CMake/releases/download/v4.3.1/cmake-4.3.1-linux-x86_64.tar.gz -O /tmp/cmake.tgz && tar -xzf /tmp/cmake.tgz -C /opt && ln -s /opt/cmake-4.3.1-linux-x86_64/bin/* /usr/local/bin/ && rm /tmp/cmake.tgz
ARG VULKAN_SDK_VERSION=1.4.313.0
RUN mkdir -p /opt/vulkan-sdk && wget -q "https://sdk.lunarg.com/sdk/download/${VULKAN_SDK_VERSION}/linux/vulkansdk-linux-x86_64-${VULKAN_SDK_VERSION}.tar.xz" -O /tmp/vk.tar.xz \
    && tar -xf /tmp/vk.tar.xz -C /opt/vulkan-sdk && rm /tmp/vk.tar.xz
ENV VULKAN_SDK=/opt/vulkan-sdk/${VULKAN_SDK_VERSION}/x86_64
ENV PATH=${VULKAN_SDK}/bin:${PATH}
RUN mkdir -p /opt/ffmpeg && wget -q "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n8.1-latest-linux64-lgpl-shared-8.1.tar.xz" -O /tmp/ff.tar.xz \
    && tar -xf /tmp/ff.tar.xz -C /opt/ffmpeg && mv /opt/ffmpeg/ffmpeg-n8.1-latest-linux64-lgpl-shared-8.1 /opt/ffmpeg/linux64 && rm /tmp/ff.tar.xz
ENV LD_LIBRARY_PATH=/opt/ffmpeg/linux64/lib:${VULKAN_SDK}/lib
RUN wget -q https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage -O /usr/local/bin/appimagetool     && chmod +x /usr/local/bin/appimagetool

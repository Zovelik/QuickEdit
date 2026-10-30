#!/bin/bash
# The Linux build of the GitHub Actions workflow, in a container (see
# Dockerfile): builds, tests and, with PACKAGE=1, makes the AppImage and
# checks the libraries it needs. From the root of the repository:
#
#   docker build -t quickedit-linux tools/linux
#   docker run --rm -e PACKAGE=1 -v "$PWD:/src:ro" -v quickedit-build:/work quickedit-linux bash /src/tools/linux/build.sh
#
# The sources are copied into /work (a volume: the next builds are
# incremental), the Windows builds and FFmpeg left out.
set -e

mkdir -p /work/src
rsync -a --delete --exclude out --exclude .git --exclude 3rdparty/ffmpeg /src/ /work/src/
mkdir -p /work/src/3rdparty/ffmpeg
ln -sfn /opt/ffmpeg/linux64 /work/src/3rdparty/ffmpeg/linux64

cd /work/src
cmake -G Ninja -B /work/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc-12 -DCMAKE_CXX_COMPILER=g++-12 \
    -DFFMPEG_ROOT=/opt/ffmpeg/linux64 > /work/configure.log 2>&1 || { tail -40 /work/configure.log; exit 1; }

# The errors and warnings of QuickEdit's own sources (all in /work/build.log).
cmake --build /work/build --parallel -- -k 0 2>&1 | tee /work/build.log | grep -E "^/work/src/(src|tests)/.*(error|warning)|FAILED" | head -150
test -f /work/build/bin/QuickEdit && echo "BUILD OK" || { echo "BUILD FAILED"; exit 1; }

cd /work/build
ctest -C Release --output-on-failure | tail -5

if [ "$PACKAGE" != "1" ]; then
    exit 0
fi

# The AppImage, as the workflow makes it.
APPIMAGE_EXTRACT_AND_RUN=1 cpack -C Release > /work/package.log 2>&1 && echo "PACKAGE OK" || { tail -40 /work/package.log; echo "PACKAGE FAILED"; exit 1; }

# The libraries it needs: all in it but the Vulkan loader, which comes with
# the drivers of the system.
rm -rf /tmp/appimage && mkdir /tmp/appimage && cd /tmp/appimage
/work/build/QuickEdit-*-Linux.AppImage --appimage-extract > /dev/null 2>&1
ls -la /work/build/QuickEdit-*-Linux.AppImage
env -u LD_LIBRARY_PATH ldd squashfs-root/bin/QuickEdit | grep "not found" | grep -v libvulkan && exit 1 || echo "Libraries: all in the AppImage (but the Vulkan loader)"

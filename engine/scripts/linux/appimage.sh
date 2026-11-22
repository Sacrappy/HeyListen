#!/bin/bash
# The Linux AppImage: HeyListen Builder with everything it needs to build the game on the player's PC. No game data and
# nothing made from it is in it: the game is built from the player's own copy, as on Windows.
#
#   scripts/linux/deps.sh        (first: the compiler, Python, FFmpeg, SDL2, headers into build-linux/deps)
#   scripts/linux/appimage.sh    -> build-linux/HeyListen-<version>-Linux-x64.AppImage
# The same image builds the game and, with --play, starts the one it saved.
#
# Built on the oldest system it is for (CI: Ubuntu 22.04): the sysroot it carries is that system's C library, so the
# game it builds runs on glibc 2.35 and newer. work/ (hot_funcs.txt, hot_order.txt, hookshot_pieces.prof,
# image/chunks.txt: addresses and counts from playing, no game code) is carried along when it is there.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
B=$ROOT/build-linux
DEPS=${DEPS:-$B/deps}
APPDIR=$B/AppDir
VERSION=${HOOKSHOT_VERSION:-$(git describe --tags --always --match "v[0-9]*" 2>/dev/null || echo dev)}
export EMSDK=$DEPS/emsdk RX_CPM=$DEPS/cpm
PY=$DEPS/python/bin/python3

# ---- the Vulkan back end, built here once (it needs no game) ----
# (with AMD FSR 4: the player's build uses this back end as built (HOOKSHOT_VK_PREBUILT) and has none to compile, so FSR 4 is
# in it or nowhere. AMD's MIT FSR API headers come from third_party/fidelityfx, their licence goes below; AMD's runtime does
# not: on Linux it is the player's own amd_fidelityfx_upscaler_vk.so, and without it the Upscaler row does not offer FSR 4)
VKW=$B/vk-work
rm -rf "$VKW/sa/build/hookshot_vk.so"
OOT_WORK=$VKW RX_MARCH=x86-64-v3 RX_VK_ONLY=1 RX_RELEASE=1 HOOKSHOT_FSR=1 "$PY" -I nx2/build.py --standalone --jobs "${JOBS:-$(nproc)}" |
    tee "$B/vk-build.txt"
grep -q "with AMD FSR 4" "$B/vk-build.txt" || { echo "the Vulkan back end was built without AMD FSR 4 (third_party/fidelityfx)"; exit 1; }
VK=$VKW/sa/build/hookshot_vk.so
[ -f "$VK" ] || { echo "hookshot_vk.so was not built"; exit 1; }

# ---- the tools' native helpers, built now: the AppImage is read-only. a64emit with its C++ runtime inside (the one this system
# builds with is newer than older systems have), and the stamp build_emit.py checks, so it is not built again ----
E=third_party/a64-kit/lift
"$EMSDK/upstream/bin/clang++" -std=c++20 -O2 -shared -fPIC -static-libstdc++ -static-libgcc -Wno-everything \
    -o "$E/liba64emit.so" "$E/a64emit.cpp"
"$PY" -I -c "import hashlib, sys; f = ['$E/a64emit.cpp', '$E/suyu/arm64_to_c.h']; open('$E/liba64emit.so.sha1', 'w').write(hashlib.sha1(b''.join(open(x, 'rb').read() for x in f)).hexdigest())"
"$PY" -I third_party/a64-kit/lift/build_emit.py
"$PY" -I -c "import sys; sys.path.insert(0, 'nx2'); import zbic; zbic.lib()"

rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/share/hookshot" "$APPDIR/usr/lib/hookshot"
T=$APPDIR/usr/share/hookshot
L=$APPDIR/usr/lib/hookshot

# ---- the tools (the sources the player's build compiles, and the scripts) ----
cp -a nx2 host native LICENSE readme.md assets "$T/"   # (native: the native engine, compiled into the game by nx2/build.py)
# (HeyListen's credit to Hookshot and the licenses of what it carries: in the repository root, above engine/)
[ -f ../THIRD_PARTY_NOTICES.md ] && cp ../THIRD_PARTY_NOTICES.md "$T/"
printf '%s
' "$VERSION" > "$T/VERSION"   # (no git history in here: nx2/version.py reads the release name from it)
mkdir -p "$T/builder" "$T/gpu/render" "$T/gpu/vk" "$T/third_party"
cp -a builder/linux "$T/builder/"
cp -a gpu/render/*.h "$T/gpu/render/"
# (every gpu/vk header the host's sources include: v2.0.6 shipped without oot_cheat_items.h and no Linux build finished, issue 990.
# tests/test_appimage_headers.py checks that this covers what host/ includes)
for h in $(grep -rhoE 'gpu/vk/[A-Za-z0-9_]+[.]h' host | sort -u); do if [ -f "$h" ]; then cp -a "$h" "$T/gpu/vk/"; fi; done
# (the one compat header the host includes: the crash helper and the crash record read the switches by its rule)
mkdir -p "$T/gpu/vk/compat"; cp -a gpu/vk/compat/hookshot_env.h "$T/gpu/vk/compat/"
cp -a third_party/opus "$T/third_party/"
cp -a third_party/kenney_input "$T/third_party/"
cp -a third_party/kenney_input_keyboard "$T/third_party/"
# (zstd's decoder: nx2/build.py compiles it into the game for host/hl_stream.c)
mkdir -p "$T/third_party/zbic" "$T/third_party/a64-kit" "$T/third_party/zstd"
cp -a third_party/zstd/lib "$T/third_party/zstd/"
cp -a third_party/zbic/zbic.so third_party/zbic/LICENSE third_party/zbic/PROVENANCE.md "$T/third_party/zbic/"
cp -a third_party/a64-kit/* "$T/third_party/a64-kit/"
rm -f "$T/third_party/a64-kit/tools/lld-link.exe"
if [ -d work ]; then
    mkdir -p "$T/work/image"
    for f in hot_funcs.txt hot_order.txt hookshot_pieces.prof pipeline_states.bin; do [ -f "work/$f" ] && cp "work/$f" "$T/work/"; done
    [ -f work/image/chunks.txt ] && cp work/image/chunks.txt "$T/work/image/"
fi
find "$T" -name __pycache__ -prune -exec rm -rf {} +

# ---- what it builds with ----
cp -a "$DEPS/python" "$APPDIR/usr/python"
cp "$DEPS/tk/libtcl9tk9.0.so" "$DEPS/tk/libXft.so.2" "$DEPS/tk/libXrender.so.1" "$APPDIR/usr/python/lib/"
mkdir -p "$L/emsdk"
cp -a "$DEPS/emsdk/upstream" "$L/emsdk/"
cp -a "$DEPS/ffmpeg" "$L/ffmpeg"
cp "$VK" "$L/hookshot_vk.so"
cp "$DEPS/sdl2/libSDL2-2.0.so.0" "$L/libSDL2-2.0.so.0"
cp -a "$DEPS/licenses" "$T/licenses"   # (LLVM, FFmpeg, dav1d, SDL2: what this AppImage carries besides HeyListen and Hookshot)
# ---- the licences of the third-party parts compiled into hookshot_vk.so (tests/test_appimage_licenses.py) ----
mkdir -p "$T/licenses/imgui" "$T/licenses/zstd" "$T/licenses/libertinus" "$T/licenses/fidelityfx"
cp third_party/imgui/LICENSE.txt third_party/imgui/PROVENANCE.md "$T/licenses/imgui/"
cp third_party/zstd/LICENSE third_party/zstd/PROVENANCE.md "$T/licenses/zstd/"
cp third_party/fonts/libertinus/OFL.txt third_party/fonts/PROVENANCE.md "$T/licenses/libertinus/"
cp third_party/fidelityfx/LICENSE.md third_party/fidelityfx/README.md "$T/licenses/fidelityfx/"   # (AMD's FSR API headers, MIT)
# ---- end of the licences ----

# the sysroot: this system's C library (headers, start files, the libraries to link against) and GCC's runtime pieces
S=$L/sysroot
mkdir -p "$S/usr/lib/x86_64-linux-gnu" "$S/usr/lib/gcc/x86_64-linux-gnu" "$S/lib64"
ln -s usr/lib "$S/lib"
cp -a /usr/include "$S/usr/"
# (GCC's C++ headers stay, its newest version only: engine/native's C++ (hl/stream, hl/jobs...) is compiled at the player's
# build since the native engine came in, and the bundled clang finds them through the sysroot's GCC install below.
# Issue 14: the test build of main failed on the Deck with "'cstddef' file not found" once they were removed here)
rm -rf "$S/usr/include/llvm"* "$S/usr/include/clang"*
CXXV=$(basename "$(ls -d /usr/lib/gcc/x86_64-linux-gnu/*/ | sort -V | tail -1)")   # (the GCC whose runtime is carried below)
[ -d "/usr/include/c++/$CXXV" ] || { echo "no C++ headers for GCC $CXXV (g++-$CXXV / libstdc++-$CXXV-dev)"; exit 1; }
for d in "$S/usr/include/c++" "$S/usr/include/x86_64-linux-gnu/c++"; do
    [ -d "$d" ] && find "$d" -mindepth 1 -maxdepth 1 ! -name "$CXXV" -exec rm -rf {} +
done
M=/usr/lib/x86_64-linux-gnu
for f in crt1.o crti.o crtn.o Scrt1.o rcrt1.o libc.so libc.so.6 libc_nonshared.a libm.so libm.so.6 libm.a libmvec.so libmvec.so.1 \
         libmvec.a libpthread.so.0 libpthread.a libdl.so.2 libdl.a librt.so.1 librt.a libutil.a libgcc_s.so.1 ld-linux-x86-64.so.2; do
    # System aliases can have absolute targets (e.g. libmvec.so). Carry their bytes;
    # update parts refuse absolute-target symlinks, and must not use the host's library.
    [ -e "$M/$f" ] && cp -aL "$M/$f" "$S/usr/lib/x86_64-linux-gnu/" || true
done
cp -aL /lib64/ld-linux-x86-64.so.2 "$S/lib64/"
GCC=$(ls -d /usr/lib/gcc/x86_64-linux-gnu/*/ | sort -V | tail -1)
G=$S/usr/lib/gcc/x86_64-linux-gnu/$(basename "$GCC")
mkdir -p "$G"
# (libstdc++.a and libsupc++.a: the game links engine/native's C++ statically against them (nx2/build.py, with a sysroot),
# so the player's system needs no particular libstdc++)
for f in crtbegin.o crtbeginS.o crtbeginT.o crtend.o crtendS.o libgcc.a libgcc_eh.a libgcc_s.so libstdc++.a libsupc++.a; do
    [ -e "$GCC/$f" ] && cp -aL "$GCC/$f" "$G/" || true
done
# (libgcc_s.so is a linker script naming /lib/x86_64-linux-gnu/libgcc_s.so.1: found in the sysroot through lib -> usr/lib)

# ---- check: the bundled compiler builds and links a program against the sysroot, and it runs ----
cat > "$B/t.c" <<'EOF'
#include <pthread.h>
#include <stdio.h>
#include <math.h>
static void *f(void *p) { return p; }
int main(void) { pthread_t t; pthread_create(&t, 0, f, 0); pthread_join(t, 0); printf("ok %g\n", sqrt(2.0)); return 0; }
EOF
"$L/emsdk/upstream/bin/clang" --sysroot="$S" -fuse-ld=lld -O2 -march=x86-64-v3 "$B/t.c" -o "$B/t" -lpthread -lm
"$B/t"
# (and C++20 the way nx2/build.py compiles and links engine/native: the headers its sources include, libstdc++ linked in)
cat > "$B/t.cpp" <<'EOF'
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <span>
#include <vector>
int main() {
    std::atomic<uint64_t> a{1}; std::vector<int> v{3, 1, 2}; std::sort(v.begin(), v.end());
    std::function<int(int)> f = [](int x) { return x + 1; }; std::span<int> s(v);
    std::printf("ok %d %d %d\n", (int)a.fetch_add(1), f(s[0]), std::popcount(7u));
    return 0;
}
EOF
"$L/emsdk/upstream/bin/clang" --sysroot="$S" -fuse-ld=lld -std=c++20 -O2 -march=x86-64-v3 -c "$B/t.cpp" -o "$B/tcpp.o"
"$L/emsdk/upstream/bin/clang" --sysroot="$S" -fuse-ld=lld -O2 "$B/tcpp.o" -o "$B/tcpp" -l:libstdc++.a -lpthread -lm
"$B/tcpp"

# ---- the AppDir's own files ----
cp builder/linux/AppRun "$APPDIR/AppRun"
cp builder/linux/heylisten-builder.desktop "$APPDIR/"
cp assets/heylisten.png "$APPDIR/heylisten.png"
# (RecompLauncher reads this from inside the AppImage, at /recomplauncher.json: docs/recomplauncher.md)
cp ../recomplauncher.json "$APPDIR/recomplauncher.json"
mkdir -p "$APPDIR/usr/share/icons/hicolor/256x256/apps"
cp assets/heylisten.png "$APPDIR/usr/share/icons/hicolor/256x256/apps/heylisten.png"

# (the update list, beside AppRun and not inside a hashed part: heylisten-linux.txt names each part's checksum)
# (the parts are served from the linux-parts release of the repository this is built in, where
# scripts/linux/publish_parts.sh uploads them: a fork's CI uploads to the fork, so its list names its own parts. Outside CI
# the default is Sacrappy/HeyListen, the repository tools/release/make_release_linux.sh checks the list against)
python3 scripts/linux/make_parts.py "$APPDIR" "$VERSION" "$B/parts" \
    --base "https://github.com/${GITHUB_REPOSITORY:-Sacrappy/HeyListen}/releases/download/linux-parts"

OUT=$B/HeyListen-$VERSION-Linux-x64.AppImage
rm -f "$OUT"
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$DEPS/appimagetool" --runtime-file "$DEPS/runtime-x86_64" --comp zstd "$APPDIR" "$OUT"
echo "$OUT: $(du -h "$OUT" | cut -f1)"

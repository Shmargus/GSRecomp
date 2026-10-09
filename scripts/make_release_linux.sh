#!/usr/bin/env bash
# Makes the Linux player release, the counterpart of make_release.ps1. Run it
# on a Linux machine (or WSL) with GCC, CMake, Ninja and the SDL2/zlib
# development packages installed:
#
#   bash scripts/make_release_linux.sh [--rom <Golden Sun ROM>] [--no-test]
#                                      [--version <GitHub release tag>]
#   bash scripts/make_release_linux.sh --launcher-only [--version <tag>]
#   bash scripts/make_release_linux.sh --dev
#
# --launcher-only builds only the player launcher, zipped as
# GSRecomp-Release/linux/GoldenSunLauncher-linux-<version>.zip. Players drop
# it over GoldenSunLauncher in their folder; the launcher is not part of the
# game code's fingerprint, so nothing rebuilds.
# --dev does the same with the Windows developer launcher's extras (RAM
# self-heal, per-frame timing CSVs) into GSRecomp-Release/linux/dev/.
#
# The version (default "dev") is built into the launcher, which tells players
# when GitHub's newest release has a different tag (launcher_online.h).
#
# Result, in GSRecomp-Release/linux/:
#   GoldenSunRecompiled-linux-<date>.zip   the download (made on Linux, so it
#                                          keeps the programs' executable bit)
#   Golden Sun Recompiled/                 a copy of the folder to look at; on
#                                          a Windows drive its files lose that
#                                          bit, so give players the zip
# The folder is assembled in the Linux work directory ($GSR_LINUX_WORK,
# default ~/gsr-linux-release/release) for the same reason.
#
#   GoldenSunLauncher          the Linux launcher (Pick ROM, first-run build)
#   GoldenSunRecomp            the engine
#   builder/gsr_builder        ROM -> libGoldenSunGame.so
#   builder/gba_recompile      the translator
#   builder/data/              main/transient/overlay configs, the build plan
#   builder/engine/include/    the two headers the game code includes
#   builder/toolchain/         this machine's GCC, trimmed to the files the
#                              game code's compile and link open, with its
#                              own libraries (hostlib/) and the C library
#                              headers and link stubs (sysroot/)
#   overlay_toolchain/include/ the three overlay shim headers the engine's
#                              runtime self-heal compile includes
#
# Not included: libGoldenSunGame.so, any generated code, the ROM, the BIOS.
# The player's system provides glibc, SDL2 and OpenGL.
#
# With a ROM (--rom, or $GSR_ROM) it then builds the game code using ONLY the
# release folder's builder and toolchain, with the system compiler hidden, to
# prove the release is complete.
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
rom=${GSR_ROM:-}
version=${GSR_RELEASE_VERSION:-dev}
test_build=1
dev=0
launcher_only=0
ashley_edition=0
while [ $# -gt 0 ]; do
    case "$1" in
        --rom) rom=$2; shift 2 ;;
        --no-test) test_build=0; shift ;;
        --version) version=$2; shift 2 ;;
        --dev) dev=1; launcher_only=1; shift ;;
        --launcher-only) launcher_only=1; shift ;;
        --ashley-edition) ashley_edition=1; shift ;;
        *) echo "usage: $0 [--rom <path>] [--no-test] [--version <GitHub release tag>] [--ashley-edition] [--launcher-only | --dev]" >&2; exit 2 ;;
    esac
done
if [ "$ashley_edition" = 1 ] && [ "$launcher_only" = 1 ]; then
    echo "Ashley Edition needs a full release; turn off launcher only." >&2
    exit 2
fi

# The bug report service's address (tools/report_service) stays out of the
# public source, as in make_release.ps1: it is read from local/report_host.txt
# (one line, not in git) or $GSR_REPORT_HOST. Without it the launcher is built
# without Send report.
report_host=${GSR_REPORT_HOST:-}
if [ -z "$report_host" ] && [ -f "$repo/local/report_host.txt" ]; then
    report_host=$(head -n 1 "$repo/local/report_host.txt" | tr -d '\r[:space:]')
fi
[ -n "$report_host" ] ||
    echo "No local/report_host.txt: the launcher is built without Send report." >&2

# Build trees on the Linux file system: a Windows drive under WSL is slow.
work=${GSR_LINUX_WORK:-$HOME/gsr-linux-release}
build=$work/build
corpus=$repo/local/gs011_block
release_root=$repo/GSRecomp-Release/linux
stage=$work/release
out=$stage/"Golden Sun Recompiled"

# make: GCC runs the parallel LTO link (-flto=N) through it; without it the
# link falls back to one job at a time.
for tool in g++ cmake ninja make objdump readelf ldd zip; do
    command -v "$tool" >/dev/null || { echo "Missing $tool." >&2; exit 1; }
done
[ -f "$corpus/main/dispatch_table.cpp" ] || {
    echo "Missing $corpus. Run build_lto.bat (or scripts/gs.ps1 -To recompile and" >&2
    echo "tools/block_timing.py) on Windows first." >&2
    exit 1
}

# ---- 1. Engine, launcher, builder, translator ------------------------------
# The engine links against a library built from the block-timing corpus;
# only the engine ships. Players build their own library from their ROM.
echo "Configuring $build"
cmake -S "$repo" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG" \
    "-DCMAKE_C_FLAGS_RELEASE=-O2 -DNDEBUG" \
    -DGSR_BUILD_LOCAL_RUNNER=ON \
    -DGSR_SPLIT_GAME_CODE=ON \
    -DGSR_GAME_CODE_OPT=-Og \
    "-DGSR_GAME_CODE_ROOT=$corpus" \
    "-DGSR_GENERATED_DIR=$corpus/main" \
    "-DGSR_OVERLAY_ROOT=$corpus" \
    "-DGSR_STAMPS_DIR=$corpus/stamps" \
    -DGSR_ENABLE_LTO=ON \
    -DGSR_DEBUG_SYMBOLS=OFF \
    "-DGSR_RELEASE_VERSION=$version" \
    "-DGSR_REPORT_HOST=$report_host" \
    "-DGSR_LINUX_DEV_LAUNCHER=$([ "$dev" = 1 ] && echo ON || echo OFF)" >/dev/null
if [ "$launcher_only" = 1 ]; then
    cmake --build "$build" --target GoldenSunLauncher
    if [ "$dev" = 1 ]; then dest=$release_root/dev; name=GoldenSunLauncher-dev.zip
    else dest=$release_root; name=GoldenSunLauncher-linux-$version.zip; fi
    # Zipped on the Linux file system so the launcher keeps its executable bit.
    zipdir=$work/launcher-zip
    rm -rf "$zipdir"; mkdir -p "$zipdir" "$dest"
    install -m 755 -s "$build/GoldenSunLauncher" "$zipdir/"
    (cd "$zipdir" && zip -q -9 "$name" GoldenSunLauncher)
    cp "$zipdir/$name" "$dest/"
    rm -rf "$zipdir"
    echo "Launcher: $dest/$name"
    exit 0
fi
echo "Building (the game code part takes a while the first time)"
cmake --build "$build" --target GoldenSunRecomp GoldenSunLauncher gsr_builder gba_recompile
recompiler=$(find "$build" -name gba_recompile -type f -perm -u+x | head -1)
for f in "$build/GoldenSunRecomp" "$build/GoldenSunLauncher" "$build/gsr_builder" "$recompiler"; do
    [ -x "$f" ] || { echo "Build output missing: $f" >&2; exit 1; }
done

# ---- 2. A clean release folder ---------------------------------------------
rm -rf "$out"
builder=$out/builder
data=$builder/data
tc=$builder/toolchain
mkdir -p "$data/overlays" "$builder/engine/include" "$tc"
install -m 755 -s "$build/GoldenSunLauncher" "$build/GoldenSunRecomp" "$out/"
if [ "$ashley_edition" = 1 ]; then
    cp "$repo/assets/ashley_edition/edition_badge.bmp" \
       "$repo/assets/ashley_edition/edition_badge.txt" "$out/"
fi
install -m 755 -s "$build/gsr_builder" "$recompiler" "$builder/"

usa=$repo/config/usa
cp "$usa/game_code_plan.txt" "$usa/main.toml" "$usa/overlay-registry.inc" "$data/"
sed 's/#.*//' "$usa/game_code_plan.txt" | while read -r kind _ toml _; do
    if [ "$kind" = transient ]; then cp "$usa/$toml" "$data/"; fi
done
cp "$usa"/overlays/*.toml "$data/overlays/"
cp "$repo/gbarecomp/src/armv4t/runtime_arm.h" "$repo/gbarecomp/src/armv4t/runtime_arm_types.h" \
    "$builder/engine/include/"

# Self-heal: the overlay shim headers the engine's runtime compile includes.
mkdir -p "$out/overlay_toolchain/include"
for h in src/runtime/overlay_runtime_arm.h src/runtime/overlay_abi.h src/armv4t/runtime_arm_types.h; do
    [ -f "$repo/gbarecomp/$h" ] || { echo "Overlay header missing: $h" >&2; exit 1; }
    cp "$repo/gbarecomp/$h" "$out/overlay_toolchain/include/"
done

# ---- 3. Trimmed GCC ----------------------------------------------------------
# This machine's GCC, relocated: GCC finds its programs, its own headers and
# libgcc relative to bin/g++, so the folder mirrors /usr. The C library side
# (glibc headers, crt files, libc.so) goes in sysroot/, which the builder
# passes as --sysroot. Exactly the files a compile of the generated code and
# a link of the library open are copied, as make_release.ps1 does on Windows.
echo "Assembling the compiler"
gxx_real=$(readlink -f "$(command -v g++)")
gcc_ver=$(g++ -dumpversion)
target=$(g++ -dumpmachine)
libexec=$(dirname "$(g++ -print-prog-name=cc1plus)")
gcclib=$(dirname "$(g++ -print-libgcc-file-name)")
case "$libexec" in /usr/libexec/gcc/*) ;; *) echo "Unexpected GCC layout: $libexec" >&2; exit 1 ;; esac
case "$gcclib" in /usr/lib/gcc/*) ;; *) echo "Unexpected GCC layout: $gcclib" >&2; exit 1 ;; esac

copied_list=$work/toolchain-files.txt
: > "$copied_list"
# tc_copy <host file>: copy to its place in the toolchain (content, not links).
tc_copy() {
    local src dst
    src=$(realpath -s "$1")
    case "$src" in
        /usr/lib/gcc/*|/usr/libexec/gcc/*) dst=$tc/${src#/usr/} ;;
        /usr/include/c++/*|/usr/include/$target/c++/*) dst=$tc/${src#/usr/} ;;
        /usr/include/*|/usr/lib/*|/usr/lib64/*) dst=$tc/sysroot$src ;;
        /lib/*|/lib64/*) dst=$tc/sysroot/usr$src ;;
        *) echo "Toolchain file outside /usr: $src" >&2; exit 1 ;;
    esac
    [ -e "$dst" ] && return 0
    mkdir -p "$(dirname "$dst")"
    cp -L "$src" "$dst"
    echo "$(readlink -f "$src")" >> "$copied_list"
}

mkdir -p "$tc/bin" "$tc/hostlib" "$tc/libexec/gcc/$target/$gcc_ver"
cp -L "$gxx_real" "$tc/bin/g++"
echo "$gxx_real" >> "$copied_list"
programs=("$tc/bin/g++")
for p in cc1plus collect2 lto-wrapper liblto_plugin.so; do
    tc_copy "$libexec/$p"
    programs+=("$tc/libexec/gcc/$target/$gcc_ver/$p")
done
# as and ld beside cc1plus: GCC looks there before PATH.
for p in as ld; do
    real=$(readlink -f "$(command -v "$p")")
    cp -L "$real" "$tc/libexec/gcc/$target/$gcc_ver/$p"
    echo "$real" >> "$copied_list"
    programs+=("$tc/libexec/gcc/$target/$gcc_ver/$p")
done
# Every non-glibc library those programs load, recursively (ldd is recursive).
for p in "${programs[@]}"; do
    ldd "$p" | awk '/=> \//{print $3}' | while read -r lib; do
        case "$(basename "$lib")" in
            libc.so*|libm.so*|libdl.so*|libpthread.so*|librt.so*|ld-linux*|libresolv.so*) ;;
            *) if [ ! -e "$tc/hostlib/$(basename "$lib")" ]; then
                   cp -L "$lib" "$tc/hostlib/"
                   echo "$(readlink -f "$lib")" >> "$copied_list"
               fi ;;
        esac
    done
done

# Headers: every system header any generated file can include. One file that
# includes every distinct #include line of the corpus, listed with -M.
probe=$work/probe
rm -rf "$probe"; mkdir -p "$probe"
grep -rhE '^[[:space:]]*#[[:space:]]*include[[:space:]]*<' "$corpus" --include='*.cpp' --include='*.h' \
    | sed 's/^[[:space:]]*//' | sort -u > "$probe/all_includes.cpp"
cxxflags=(-O2 -g -DNDEBUG -std=c++20 -Og -g0 -DGBARECOMP_OUTLINE_BUS=1 -fPIC -fno-semantic-interposition)
g++ "${cxxflags[@]}" -I"$builder/engine/include" -M "$probe/all_includes.cpp" \
    | tr -d '\\' | tr ' ' '\n' | grep '^/usr/' | sort -u | while read -r h; do tc_copy "$h"; done
# The builder's sample: a real generated file, through every header it opens.
for f in main/dispatch_table.cpp main/symbol_map.cpp main/recompiled_000.cpp stamps/stamp_registry.cpp; do
    [ -f "$corpus/$f" ] || continue
    g++ "${cxxflags[@]}" -I"$corpus/main" -I"$builder/engine/include" -M "$corpus/$f" \
        | tr -d '\\' | tr ' ' '\n' | grep '^/usr/' | sort -u | while read -r h; do tc_copy "$h"; done
done

# Link inputs: what ld opens for the builder's link (-t lists them).
printf 'extern "C" int gsr_probe() { return 1; }\n' > "$probe/probe.cpp"
g++ "${cxxflags[@]}" -c "$probe/probe.cpp" -o "$probe/probe.o"
g++ -shared -Wl,-Bsymbolic -Wl,-t -o "$probe/probe.so" "$probe/probe.o" 2>&1 \
    | sed -E 's/^\(?([^()]*)\)?.*$/\1/' | grep '^/' | grep -v "^$probe/" | sort -u \
    > "$probe/link_inputs.txt"
while read -r f; do if [ -f "$f" ]; then tc_copy "$f"; fi; done < "$probe/link_inputs.txt"
# The linker scripts among them name further files (libc.so -> libc.so.6,
# libc_nonshared.a, ld-linux); copy those too.
for script in $(grep -lE '^(GROUP|INPUT)' $(cat "$probe/link_inputs.txt") 2>/dev/null || true); do
    grep -oE '/[^ )]+' "$script" | while read -r f; do if [ -f "$f" ]; then tc_copy "$f"; fi; done
done
# The start and end files of a shared library, in case -t missed them.
tc_copy "$gcclib/crtbeginS.o"
tc_copy "$gcclib/crtendS.o"
# The linker scripts (libc.so, libm.so) name /lib/... and /lib64/..., which on
# a usrmerge system are links to /usr/lib. The release holds no links at all
# (a zip unpacked or copied through a USB stick or Windows drive loses them,
# 2026-10-02), so point the scripts at the real /usr/lib copies instead.
grep -rlI '^GROUP\|^INPUT' "$tc/sysroot" | while read -r script; do
    sed -i -E 's#(^|[ (])/lib(64)?/#\1/usr/lib\2/#g' "$script"
done

# The bundled compiler on its own: no /usr include or library dir may appear
# in its search lists.
search=$(LD_LIBRARY_PATH="$tc/hostlib" "$tc/bin/g++" --sysroot="$tc/sysroot" -E -v -x c++ /dev/null 2>&1 \
    | sed -n '/#include <...> search starts here:/,/End of search list./p')
if echo "$search" | grep -qE '^ /usr/'; then
    echo "The bundled compiler still searches the system headers:" >&2
    echo "$search" >&2
    exit 1
fi

# ---- 4. Notes for players and third-party notices ----------------------------
cat > "$out/README.txt" <<'TXT'
Golden Sun Recompiled (Linux)
=============================

1. Start GoldenSunLauncher (double-click it, or ./GoldenSunLauncher in a
   terminal from this folder).
2. Click "Pick ROM" and choose your own Golden Sun (USA, Europe) ROM.
3. The first time, the game is prepared from your ROM. This takes a few
   minutes and happens only once. Then the game starts.

After that, "Pick ROM" starts the game straight away.

Needs: a 64-bit Linux with SDL2 and OpenGL (installed on SteamOS and on most
desktop distributions). If the launcher does not open, install your
distribution's SDL2 package (libsdl2-2.0-0 on Debian/Ubuntu, sdl2 on Arch,
SDL2 on Fedora).

This download contains no Golden Sun code or data. Everything from the game
comes from your ROM, on your computer.

Settings: press F1 in the game.
TXT

# dpkg may record a file under /lib or /usr/lib (usrmerge): ask for both.
packages=$(sort -u "$copied_list" | while read -r f; do
        dpkg -S "$f" 2>/dev/null || dpkg -S "${f#/usr}" 2>/dev/null || true
    done | cut -d: -f1 | tr ',' '\n' | sed 's/^ *//' | sort -u \
    | xargs -r dpkg-query -W -f='${Package} ${Version}\n' 2>/dev/null || true)
cat > "$out/THIRD-PARTY.txt" <<TXT
Third-party software in this folder
===================================

GoldenSunLauncher uses Dear ImGui (MIT license, https://github.com/ocornut/imgui).
The game uses SDL2 (zlib license, https://www.libsdl.org/) from your system.

builder/toolchain holds unmodified binaries, headers and libraries from the
$(. /etc/os-release && echo "$PRETTY_NAME") packages listed below: GCC and
binutils under the GNU GPL version 3 (the GCC runtime libraries under the GCC
Runtime Library Exception); the GNU C Library under the GNU LGPL; GMP, MPFR
and MPC under the GNU LGPL; ISL under the MIT license; zlib under the zlib
license; zstd under the BSD license; Jansson under the MIT license. The
source code of each exact package version is available from the
distribution's archive (apt-get source <package>=<version>) and from us on
request.

Package versions:
$packages
TXT

# ---- 5. Test: build the game code with only the release's own tools ---------
if [ "$test_build" = 1 ] && [ -n "$rom" ]; then
    echo "Testing: building the game code in a scratch copy of the release, with"
    echo "only its own builder and compiler (the release itself stays without it)."
    scratch=$work/test
    rm -rf "$scratch"; mkdir -p "$scratch"
    cp -a "$out" "$scratch/"
    test_root=$scratch/"Golden Sun Recompiled"
    # Hide the system compiler: its programs, headers and libgcc.
    hide=(/usr/include /usr/lib/gcc /usr/libexec/gcc)
    hide_files=()
    for p in g++ gcc c++ cc as ld; do
        f=$(command -v "$p" || true); [ -n "$f" ] && hide_files+=("$(readlink -f "$f")")
    done
    unshare -m bash -c '
        set -e
        for d in '"${hide[*]}"'; do mount -t tmpfs none "$d"; done
        for f in '"${hide_files[*]}"'; do mount --bind /dev/null "$f"; done
        "$1/builder/gsr_builder" --rom "$2"
    ' _ "$test_root" "$rom" | grep --line-buffered -E '^@(stage|error|done)' | sed -u 's/^@/  test: /' || true
    [ -f "$test_root/libGoldenSunGame.so" ] || {
        echo "The test build failed. Log: $test_root/builder/work/build-log.txt" >&2; exit 1; }
    rm -rf "$scratch"
    echo "  The release builds the game from the ROM on its own (test copy removed)."
elif [ "$test_build" = 1 ]; then
    echo "No ROM given (--rom or GSR_ROM): skipped the test build."
fi

# ---- 6. The download ---------------------------------------------------------
stamp=$(date +%Y-%m-%d)
edition_suffix=
if [ "$ashley_edition" = 1 ]; then edition_suffix=-Ashley; fi
archive=$release_root/GoldenSunRecompiled-linux$edition_suffix-$stamp.zip
mkdir -p "$release_root"
rm -f "$archive"
links=$(find "$out" -type l)
if [ -n "$links" ]; then
    echo "The release must not contain links (they break when copied):" >&2
    echo "$links" >&2
    exit 1
fi
(cd "$stage" && zip -qr -9 "$archive" "Golden Sun Recompiled")
rm -rf "$release_root/Golden Sun Recompiled"
cp -r "$out" "$release_root/"
echo "Release folder: $release_root/Golden Sun Recompiled ($(du -sh "$out" | cut -f1))"
echo "Download:       $archive ($(du -h "$archive" | cut -f1))"

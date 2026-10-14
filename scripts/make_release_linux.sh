#!/usr/bin/env bash
# Combined Linux release: one root launcher/artwork and both game engines.
# Uses the existing local/gs011_block and TLA local/tla_linux_block corpora.
# --launcher-only updates the root launcher/artwork.
# --rom / --tla-rom (or GSR_ROM / TLA_ROM) select optional builder-test ROMs.
# --no-test skips both tests; --dev preserves developer-launcher updates.
# Linux builds and archive staging stay on the Linux filesystem so executable
# permissions survive. No ROM, BIOS, game DLL/SO or generated code ships.
set -euo pipefail

collection_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
repo="$collection_root/The Broken Seal"
engine="$collection_root/engine"
rom=${GSR_ROM:-}
tla_rom=${TLA_ROM:-}
version=${GSR_RELEASE_VERSION:-dev}
repository=${GSR_RELEASE_REPOSITORY:-Shmargus/GSRecomp}
test_build=1
dev=0
launcher_only=0
ashley_edition=0
while [ $# -gt 0 ]; do
    case "$1" in
        --rom) rom=$2; shift 2 ;;
        --tla-rom) tla_rom=$2; shift 2 ;;
        --no-test) test_build=0; shift ;;
        --version) version=$2; shift 2 ;;
        --repository) repository=$2; shift 2 ;;
        --dev) dev=1; launcher_only=1; shift ;;
        --launcher-only) launcher_only=1; shift ;;
        --ashley-edition) ashley_edition=1; shift ;;
        *) echo "usage: $0 [--rom <path>] [--tla-rom <path>] [--no-test] [--version <GitHub release tag>] [--repository <owner/repo>] [--ashley-edition] [--launcher-only | --dev]" >&2; exit 2 ;;
    esac
done
[[ "$version" =~ ^[A-Za-z0-9._-]+$ ]] || { echo "Version may only contain letters, digits, '.', '-' and '_'." >&2; exit 2; }
[[ "$repository" =~ ^[A-Za-z0-9-]+/[A-Za-z0-9_.-]+$ ]] || { echo 'Repository must be a GitHub owner/repository name.' >&2; exit 2; }
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
build=$work/build-collection
tla_repo=$(cd "$repo/../The Lost Age" && pwd)
tla_build=$work/build-tla
tla_corpus=$tla_repo/local/tla_linux_block
corpus=$repo/local/gs011_block
release_root=$collection_root/Releases/linux
stage=$work/release
collection=$stage/"Golden Sun Recompiled"
out=$collection/games/"The Broken Seal"
tla_out=$collection/games/"The Lost Age"

# Every recursive cleanup must resolve inside the work or release directory.
work=$(realpath -m "$work")
release_root=$(realpath -m "$release_root")
[ "$work" != / ] && [ "$release_root" != / ] || { echo "Unsafe release path." >&2; exit 1; }
safe_remove() {
    local resolved
    resolved=$(realpath -m "$1")
    case "$resolved" in "$work"/*|"$release_root"/*) rm -rf -- "$resolved" ;;
        *) echo "Refusing to remove outside release directories: $resolved" >&2; exit 1 ;;
    esac
}
copy_collection() {
    local destination=$1
    mkdir -p "$destination/assets"
    install -m 755 -s "$build/GoldenSunCollection" "$destination/GoldenSunLauncher"
    for asset in launcher_logo.png cartridge_broken_seal.png cartridge_lost_age.png; do
        cp "$collection_root/assets/$asset" "$destination/assets/"
    done
}

# make: GCC runs the parallel LTO link (-flto=N) through it; without it the
# link falls back to one job at a time.
for tool in g++ cmake ninja make objdump readelf ldd zip python3; do
    command -v "$tool" >/dev/null || { echo "Missing $tool." >&2; exit 1; }
done
[ -f "$corpus/main/dispatch_table.cpp" ] || {
    echo "Missing $corpus. Run build_dev.bat -Game BrokenSeal (or scripts/gs.ps1 -To recompile and" >&2
    echo "scripts/block_timing.py) on Windows first." >&2
    exit 1
}

if [ "$launcher_only" != 1 ]; then
[ -f "$tla_corpus/main/dispatch_table.cpp" ] || {
    echo "Missing $tla_corpus. Prepare The Lost Age Linux corpus first." >&2; exit 1;
}
fi

# ---- 1. Engine, launcher, builder, translator ------------------------------
# The engine links against a library built from the block-timing corpus;
# only the engine ships. Players build their own library from their ROM.
echo "Configuring $build"
cmake -S "$repo" -B "$build" -G Ninja \
    "-DGBARECOMP_ROOT=$engine" \
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
    "-DGSR_RELEASE_REPOSITORY=$repository" \
    "-DGSR_REPORT_HOST=$report_host" \
    "-DGSR_LINUX_DEV_LAUNCHER=$([ "$dev" = 1 ] && echo ON || echo OFF)" >/dev/null
if [ "$launcher_only" != 1 ]; then
dependency_source() {
    local name=$1 directory=$2 source vendored=$2
    source=$(sed -n -E "s/^FETCHCONTENT_SOURCE_DIR_${name}:(PATH|STRING)=//p" "$build/CMakeCache.txt" | head -1)
    if [ -n "$source" ] && [ -d "$source" ]; then printf '%s\n' "$source"; return; fi
    if [ "$name" = TOMLPLUSPLUS ]; then vendored=tomlpp; fi
    for source in "$build/_deps/$directory-src" "$engine/third_party/$vendored"; do
        if [ -d "$source" ]; then printf '%s\n' "$source"; return; fi
    done
    echo "The $name source directory is missing from the configured release dependencies." >&2; return 1
}
imgui_source=$(dependency_source IMGUI imgui)
toml_source=$(dependency_source TOMLPLUSPLUS tomlplusplus)
# A fresh release tree avoids stale absolute paths in the developer build.
# Use the existing Linux block corpus; never regenerate it through Windows.
cmake -S "$tla_repo" -B "$tla_build" -G Ninja \
    "-DGBARECOMP_ROOT=$engine" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O2 -DNDEBUG" \
    "-DCMAKE_C_FLAGS_RELWITHDEBINFO=-O2 -DNDEBUG" \
    "-DTLA_GENERATED_DIR=$tla_corpus/main" \
    -DTLA_SPLIT_GAME_CODE=ON -DTLA_GAME_CODE_OPT=-Og \
    -DTLA_ENABLE_LTO=ON \
    -DTLA_RELEASE_LAUNCHER=ON \
    "-DGSR_LINUX_DEV_LAUNCHER=$([ "$dev" = 1 ] && echo ON || echo OFF)" \
    -DTLA_PLAYER_RELEASE=ON \
    "-DFETCHCONTENT_SOURCE_DIR_IMGUI=$imgui_source" \
    "-DFETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS=$toml_source" \
    "-DGSR_RELEASE_VERSION=$version" "-DGSR_REPORT_HOST=$report_host" \
    "-DGSR_RELEASE_REPOSITORY=$repository" >/dev/null
fi
if [ "$launcher_only" = 1 ]; then
    cmake --build "$build" --target GoldenSunCollection
    if [ "$dev" = 1 ]; then dest=$release_root/dev; name=GoldenSunLauncher-dev.zip
    else dest=$release_root; name=GoldenSunLauncher-linux-$version.zip; fi
    # Zipped on the Linux file system so the launcher keeps its executable bit.
    zipdir=$work/launcher-zip
    safe_remove "$zipdir"; mkdir -p "$zipdir" "$dest"
    copy_collection "$zipdir"
    (cd "$zipdir" && zip -qr -9 "$name" GoldenSunLauncher assets)
    cp "$zipdir/$name" "$dest/"
    safe_remove "$zipdir"
    echo "Launcher: $dest/$name"
    exit 0
fi
echo "Building (the game code part takes a while the first time)"
cmake --build "$build" --target GoldenSunRecomp GoldenSunCollection gsr_builder gba_recompile
cmake --build "$tla_build" --target TLARecomp tla_builder gba_recompile
recompiler=$(find "$build" -name gba_recompile -type f -perm -u+x | head -1)
for f in "$build/GoldenSunRecomp" "$build/GoldenSunCollection" "$build/gsr_builder" "$recompiler"; do
    [ -x "$f" ] || { echo "Build output missing: $f" >&2; exit 1; }
done

# ---- 2. A clean release folder ---------------------------------------------
safe_remove "$collection"
copy_collection "$collection"
builder=$out/builder
data=$builder/data
tc=$builder/toolchain
mkdir -p "$data/overlays" "$builder/engine/include" "$tc"
install -m 755 -s "$build/GoldenSunRecomp" "$out/"
if [ "$ashley_edition" = 1 ]; then
    cp "$collection_root/assets/ashley_edition/edition_badge.bmp" \
       "$collection_root/assets/ashley_edition/edition_badge.txt" "$out/"
fi
install -m 755 -s "$build/gsr_builder" "$recompiler" "$builder/"

usa=$repo/config/usa
cp "$usa/game_code_plan.txt" "$usa/main.toml" "$usa/overlay-registry.inc" "$data/"
sed 's/#.*//' "$usa/game_code_plan.txt" | while read -r kind _ toml _; do
    if [ "$kind" = transient ]; then cp "$usa/$toml" "$data/"; fi
done
cp "$usa"/overlays/*.toml "$data/overlays/"
cp "$engine/src/armv4t/runtime_arm.h" "$engine/src/armv4t/runtime_arm_types.h" \
    "$builder/engine/include/"

# Self-heal: the overlay shim headers the engine's runtime compile includes.
mkdir -p "$out/overlay_toolchain/include"
for h in src/runtime/overlay_runtime_arm.h src/runtime/overlay_abi.h src/armv4t/runtime_arm_types.h; do
    [ -f "$engine/$h" ] || { echo "Overlay header missing: $h" >&2; exit 1; }
    cp "$engine/$h" "$out/overlay_toolchain/include/"
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
safe_remove "$probe"; mkdir -p "$probe"
grep -rhE '^[[:space:]]*#[[:space:]]*include[[:space:]]*<' "$corpus" "$tla_corpus" --include='*.cpp' --include='*.h' \
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

# TLA's runtime header may open different system headers than GS1's.
for f in main/dispatch_table.cpp main/symbol_map.cpp main/recompiled_000.cpp; do
    [ -f "$tla_corpus/$f" ] || continue
    g++ "${cxxflags[@]}" -I"$tla_corpus/main" -I"$engine/src/armv4t" -M "$tla_corpus/$f" \
        | tr -d '\\' | tr ' ' '\n' | grep '^/usr/' | sort -u | while read -r h; do tc_copy "$h"; done
done
for kind in recompiled.cpp dispatch_table.cpp expected.cpp; do
    src=$(find "$tla_corpus/reloc" -name "$kind" -type f -print -quit)
    [ -n "$src" ] || continue
    g++ "${cxxflags[@]}" -I"$(dirname "$src")" -I"$engine/src/armv4t" -M "$src" \
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

Start GoldenSunLauncher in the collection's top folder and select
The Broken Seal. On first play, choose your own Golden Sun (USA, Europe) ROM.
Preparation happens once; later selections start the game directly.

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

# The Lost Age packages only its own config, translator, headers and engine.
tla_builder=$tla_out/builder
tla_recompiler=$(find "$tla_build" -name gba_recompile -type f -perm -u+x | head -1)
for f in "$tla_build/TLARecomp" "$tla_build/tla_builder" "$tla_recompiler"; do
    [ -x "$f" ] || { echo "The Lost Age output missing: $f" >&2; exit 1; }
done
mkdir -p "$tla_builder/data/reloc" "$tla_builder/engine/include"
install -m 755 -s "$tla_build/TLARecomp" "$tla_out/"
if [ "$ashley_edition" = 1 ]; then
    cp "$collection_root/assets/ashley_edition/edition_badge.bmp" \
       "$collection_root/assets/ashley_edition/edition_badge.txt" "$tla_out/"
fi
install -m 755 -s "$tla_build/tla_builder" "$tla_builder/gsr_builder"
install -m 755 -s "$tla_recompiler" "$tla_builder/"
cp "$tla_repo/config/tla/main.toml" "$tla_builder/data/"
cp "$tla_repo"/config/tla/reloc/*.toml "$tla_builder/data/reloc/"
cp "$engine/src/armv4t/runtime_arm.h" "$engine/src/armv4t/runtime_arm_types.h" "$tla_builder/engine/include/"
mkdir -p "$tla_out/overlay_toolchain/include"
for h in src/runtime/overlay_runtime_arm.h src/runtime/overlay_abi.h src/armv4t/runtime_arm_types.h; do
    cp "$engine/$h" "$tla_out/overlay_toolchain/include/"
done
cp -r "$tc" "$tla_builder/"
cp "$out/THIRD-PARTY.txt" "$collection/"
cat > "$collection/README.txt" <<'TXT'
Golden Sun Recompiled (Linux)
=============================

Start GoldenSunLauncher and click The Broken Seal or The Lost Age.
The selected cartridge breathes and glows. On first play, accept the preparation
prompt and choose your own matching USA/Europe ROM. Later selections start
the game directly. New releases are offered before starting either game.
Selected .gba files are copied into ROMs beside the launcher. Existing matching
.sav files and savestates are copied into Saves; the originals are kept.
Conflicting imports are preserved in Saves/Imported. Each game keeps its own
settings in its folder under games/.
The root launcher handles preparation, updates and starting both games.
Keep the games folders beside it; each contains that game's engine and tools.
Needs a 64-bit Linux system with SDL2 and OpenGL.
Settings: press F1 in the game.
This download contains no game code, ROM, BIOS or generated game library.
TXT

# Resolve a moved checkout's local ROM path without changing local settings.
resolve_test_rom() {
    python3 - "$1" "$2" <<'PY'
import json, pathlib, re, sys
repo = pathlib.Path(sys.argv[1])
specified = sys.argv[2]
if specified:
    candidate = pathlib.Path(specified)
    if not candidate.is_file():
        raise SystemExit(f"Test ROM does not exist: {candidate}")
    print(candidate.resolve())
else:
    try:
        value = json.loads((repo / 'config/local.json').read_text()).get('rom', '')
    except (OSError, ValueError):
        value = ''
    if re.match(r'^[A-Za-z]:[/\\]', value):
        value = '/mnt/' + value[0].lower() + value[2:].replace('\\', '/')
    candidate = pathlib.Path(value)
    if value and not candidate.is_absolute():
        candidate = repo / candidate
    if value and candidate.is_file():
        print(candidate.resolve())
    else:
        matches = list(repo.glob('*.gba'))
        if len(matches) == 1:
            print(matches[0].resolve())
PY
}

# ---- 5. Test: build the game code with only the release's own tools ---------
if [ "$test_build" = 1 ]; then
    rom=$(resolve_test_rom "$repo" "$rom")
    tla_rom=$(resolve_test_rom "$tla_repo" "$tla_rom")
    scratch=$work/test
    safe_remove "$scratch"; mkdir -p "$scratch"
    # Hide the system compiler: its programs, headers and libgcc.
    hide=(/usr/include /usr/lib/gcc /usr/libexec/gcc)
    hide_files=()
    for p in g++ gcc c++ cc as ld; do
        f=$(command -v "$p" || true); [ -n "$f" ] && hide_files+=("$(readlink -f "$f")")
    done
    test_game() {
        local source=$1 test_rom=$2 library=$3 name=$4 test_root=$scratch/game
        if [ -z "$test_rom" ]; then
            echo "No ROM available for $name: skipped its test build."; return
        fi
        safe_remove "$test_root"
        cp -a "$source" "$test_root"
        echo "Testing $name with only its packaged builder and compiler."
        if ! unshare -m bash -c '
            set -e
            for d in '"${hide[*]}"'; do mount -t tmpfs none "$d"; done
            for f in '"${hide_files[*]}"'; do mount --bind /dev/null "$f"; done
            "$1/builder/gsr_builder" --rom "$2"
        ' _ "$test_root" "$test_rom" | grep --line-buffered -E '^@(stage|error|done)' | sed -u 's/^@/  test: /'; then
            echo "The $name test failed. Log: $test_root/builder/work/build-log.txt" >&2; exit 1
        fi
        [ -f "$test_root/$library" ] || {
            echo "The $name test produced no $library. Log: $test_root/builder/work/build-log.txt" >&2; exit 1; }
        safe_remove "$test_root"
        echo "  $name builds its game from the ROM on its own."
    }
    test_game "$out" "$rom" libGoldenSunGame.so "The Broken Seal"
    test_game "$tla_out" "$tla_rom" libTLAGame.so "The Lost Age"
    safe_remove "$scratch"
fi

# ---- 6. The download ---------------------------------------------------------
stamp=$(date +%Y-%m-%d)
edition_suffix=
if [ "$ashley_edition" = 1 ]; then edition_suffix=-Ashley; fi
archive=$release_root/GoldenSunRecompiled-linux$edition_suffix-$stamp.zip
mkdir -p "$release_root"
rm -f "$archive"
# Audit both games before the archive is assembled.
for forbidden in GoldenSunGame.dll TLAGame.dll libGoldenSunGame.so libTLAGame.so gssplash.jpg '*.gba' '*.sav' '*.state*' '*.ini' 'recompiled*.cpp' 'bios*.bin'; do
    hit=$(find "$collection" -type f -iname "$forbidden" -print -quit)
    [ -z "$hit" ] || { echo "Forbidden release file: $hit" >&2; exit 1; }
done
links=$(find "$collection" -type l)
if [ -n "$links" ]; then
    echo "The release must not contain links (they break when copied):" >&2
    echo "$links" >&2
    exit 1
fi
(cd "$stage" && zip -qr -9 "$archive" "Golden Sun Recompiled")
safe_remove "$release_root/Golden Sun Recompiled"
cp -r "$collection" "$release_root/"
echo "Release folder: $release_root/Golden Sun Recompiled ($(du -sh "$collection" | cut -f1))"
echo "Download:       $archive ($(du -h "$archive" | cut -f1))"

#!/usr/bin/env bash
# Native Linux build (Windows: ../scripts/build_game.ps1 -Game LostAge); never uses Windows build outputs.
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$repo"
rom=
jobs=${CMAKE_BUILD_PARALLEL_LEVEL:-1}
regenerate=0
usage() {
    echo 'Usage: bash scripts/tla_build.sh --rom /path/to/game.gba [--jobs N] [--recompile]'
    echo 'PYTHON selects the Python interpreter. Default: one compiler job; use --jobs to adjust.'
}
while (($#)); do
    case "$1" in
        --rom|--jobs)
            (($# >= 2)) || { usage; exit 1; }
            if [[ $1 == --rom ]]; then rom=$2; else jobs=$2; fi
            shift 2 ;;
        --recompile) regenerate=1; shift ;;
        --help|-h) usage; exit 0 ;;
        *) usage; exit 1 ;;
    esac
done
[[ $(uname -s) == Linux ]] || { echo 'This script requires Linux.' >&2; exit 1; }
[[ $jobs =~ ^[1-9][0-9]*$ ]] || { echo '--jobs must be a positive integer.' >&2; exit 1; }
[[ -n $rom && -f $rom ]] || { echo 'Supply --rom with your USA/Europe ROM.' >&2; exit 1; }
rom=$(realpath -- "$rom")
python=${PYTHON:-python3}
"$python" -c 'import unicorn, capstone'
"$python" - "$rom" <<'PY'
import hashlib, pathlib, sys
if hashlib.sha1(pathlib.Path(sys.argv[1]).read_bytes()).hexdigest() != 'b500663220cb9bf56b9f8e8c0c544f1d6fa3a824':
    sys.exit('Wrong ROM: expected Golden Sun: The Lost Age USA/Europe.')
PY
for program in cmake ninja g++ git pkg-config flock; do
    command -v "$program" >/dev/null || { echo "Missing dependency: $program" >&2; exit 1; }
done
pkg-config --exists sdl2 gl zlib || { echo 'Install SDL2, OpenGL and zlib development packages.' >&2; exit 1; }
build="$repo/build/tla-linux"
mkdir -p "$build"
exec 9>"$build/build.lock"
flock -n 9 || { echo 'A Linux build is already running.' >&2; exit 1; }
generated="$repo/local/tla_linux"
# The collection root's engine/ is the shared engine; CMake pins its dependencies.
cmake_args=(-G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo)
# Optional local dependency overrides for offline developer builds.
if [[ -n ${FETCHCONTENT_SOURCE_DIR_IMGUI:-} ]]; then
    cmake_args+=("-DFETCHCONTENT_SOURCE_DIR_IMGUI=$FETCHCONTENT_SOURCE_DIR_IMGUI")
fi
if [[ -n ${FETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS:-} ]]; then
    cmake_args+=("-DFETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS=$FETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS")
fi
engine=$(cd -- "$repo/../engine" && pwd)
# A translator tree configured from the engine's previous location needs a fresh configure.
tools_fresh=()
if [[ -f $build/tools/CMakeCache.txt ]] && ! grep -qxF "CMAKE_HOME_DIRECTORY:INTERNAL=$engine" "$build/tools/CMakeCache.txt"; then
    tools_fresh=(--fresh)
fi
cmake -S "$engine" -B "$build/tools" "${tools_fresh[@]}" "${cmake_args[@]}" -DGBARECOMP_LINK_BIOS=OFF
cmake --build "$build/tools" --target gba_recompile --parallel "$jobs"
if [[ $regenerate == 1 || ! -f $generated/main/.complete || ! -f $generated/main/dispatch_table.cpp || ! -f $generated/reloc/tla_reloc_images.inc ]]; then
    "$python" tools/tla_recompile.py --rom "$rom" \
        --recompile "$build/tools/gba_recompile" --out "$generated" --cache "$build/recomp_cache"
fi
for part in main reloc; do
    "$python" "$repo/../scripts/block_timing.py" "$generated/$part" "$repo/local/tla_linux_block/$part"
done
cmake -S "$repo" -B "$build" "${cmake_args[@]}" "-DGBARECOMP_ROOT=$engine" \
    -DTLA_GENERATED_DIR="$repo/local/tla_linux_block/main" \
    -DTLA_SPLIT_GAME_CODE=ON -DTLA_ENABLE_LTO=ON -DTLA_LTO_JOBS="$jobs"
collection=$(cd -- "$repo/.." && pwd)
record="$collection/scripts/developer_build_stamp.py"
snapshot="$build/collection-build-inputs.txt"
"$python" "$record" --root "$collection" --game LostAge --build-dir "$build" --begin --snapshot "$snapshot"
cmake --build "$build" --target TLARecomp TLAGame --parallel "$jobs"
"$python" "$record" --root "$collection" --game LostAge --build-dir "$build" --finish --snapshot "$snapshot"
echo "Built $build/TLARecomp and libTLAGame.so. Launch GoldenSunLauncher from the collection root to play."

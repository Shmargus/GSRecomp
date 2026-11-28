#!/usr/bin/env python3
"""Build the Android launcher app (APK) that builds both games on the phone.

    python scripts/android/build_apk.py [--check-games] [--package-only]

The same end result as the PC player release, for Android: the APK carries
only our own code (the launcher, both engines, the player builders, the
translator), a Clang that runs on the phone, and the configuration data.
The player picks their ROM on the phone; the launcher translates and
compiles the game there, as the PC launcher does. Nothing from a ROM is in
the APK, so it can be shared like the PC release.

  --check-games   also compile both games' already-generated code (from this
                  PC's ROM) for the phone, as the phone will, and check every
                  guest jump became a host jump. Nothing of it is packed.
  --package-only  repack without compiling.

Tools (BUILDING.md "Android"): NDK r29, build-tools, platform, JDK 21, the
SDL 2.32.10 and LLVM 21.1.8 sources under %LOCALAPPDATA%\\Android.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent

TOOLCHAIN_LOCATION = ROOT / "local" / "android" / "toolchain.txt"
configured_location = (
    TOOLCHAIN_LOCATION.read_text(encoding="utf-8-sig").strip() if TOOLCHAIN_LOCATION.is_file() else ""
)
ANDROID_DIR = Path(configured_location) if configured_location else Path(os.environ.get("LOCALAPPDATA", "")) / "Android"
SDK = ANDROID_DIR / "Sdk" if configured_location else Path(os.environ.get("ANDROID_HOME") or ANDROID_DIR / "Sdk")
NDK = SDK / "ndk" / "29.0.14206865"
ANDROID_TOOLCHAIN = NDK / "build" / "cmake" / "android.toolchain.cmake"
NDK_LLVM = NDK / "toolchains" / "llvm" / "prebuilt" / "windows-x86_64"
BUILD_TOOLS = SDK / "build-tools" / "36.1.0"
ANDROID_JAR = SDK / "platforms" / "android-36" / "android.jar"
JDK = ANDROID_DIR / "jdk-21"
SDL2_SOURCE = ANDROID_DIR / "SDL2-2.32.10"
LLVM_SOURCE = ANDROID_DIR / "llvm"            # llvm-project 21.1.8 (llvm, clang, lld)
LLVM_BUILD = ANDROID_DIR / "llvm-build"
# The phone's Clang and lld, built with ThinLTO and without stack protectors
# or unwind tables in the compiler itself (the NDK adds both by default).
# The plain Release build (llvm-build/android) is kept for comparison.
PHONE_LLVM_BUILD = LLVM_BUILD / "android-lto"
MIN_SDK = 28   # player_builder kAndroidApi; posix_spawn in bionic
TARGET_SDK = 35
ABI = "arm64-v8a"
PACKAGE = "io.github.shmargus.gsrecomp"
LABEL = "Golden Sun Recompiled"
OUTPUT = ROOT / "Releases" / "android" / "GoldenSunRecompiled.apk"
# --profiling: the developer launcher (GSR_LINUX_DEV_LAUNCHER), so every game
# session runs the cost probe and present cadence and writes the frame
# timing CSVs beside its log (session_linux.cpp). Debuggable, for simpleperf.
PROFILING_OUTPUT = ROOT / "Releases" / "android" / "GoldenSunRecompiled-profiling.apk"
# Test builds let adb read the app's own files (run-as) and logs; set False
# for an APK that is shared.
DEBUGGABLE = True
# The release version shown as the app's version in Android's settings
# (--version); without one, the build time.
APP_VERSION = None

KEYSTORE = ROOT / "local" / "android" / "gsrecomp-sideload.keystore"
KEY_ALIAS = "gsrecomp"
# Android only needs the same key every time, so a new APK installs over the
# old one and keeps the player's saves; this is not a secret.
KEY_PASSWORD = "gsrecomp"

TBS = ROOT / "The Broken Seal"
TLA = ROOT / "The Lost Age"
TBS_BUILD = TBS / "build" / "gs011_android"
TLA_BUILD = TLA / "build" / "tla_android"


def fail(message: str) -> None:
    print(f"\nAndroid build stopped: {message}", file=sys.stderr)
    sys.exit(1)


def missing_tools() -> list[Path]:
    return [
        path for path in (NDK, BUILD_TOOLS, ANDROID_JAR, JDK, SDL2_SOURCE, LLVM_SOURCE / "llvm")
        if not path.exists()
    ]


def run(cmd: list, **kwargs) -> subprocess.CompletedProcess:
    print("  >", " ".join(str(c) for c in cmd[:3]), "...", flush=True)
    result = subprocess.run([str(c) for c in cmd], **kwargs)
    if result.returncode != 0:
        fail(f"{Path(str(cmd[0])).name} failed (exit {result.returncode})")
    return result


def tool_env() -> dict:
    env = dict(os.environ)
    env["JAVA_HOME"] = str(JDK)
    env["PATH"] = str(JDK / "bin") + os.pathsep + env.get("PATH", "")
    return env


def ninja() -> str:
    found = shutil.which("ninja")
    if not found:
        fail("ninja is not on PATH")
    return found


def no_other_build() -> None:
    """Only one build may run at a time: overlapping builds corrupt libraries."""
    out = subprocess.run(["tasklist"], capture_output=True, text=True).stdout.lower()
    busy = [n for n in ("ninja.exe", "cmake.exe", "cc1plus.exe", "clang++.exe",
                        "clang.exe", "ld.lld.exe") if n in out]
    if busy:
        fail("another build is running (" + ", ".join(busy) + "). Wait for it to finish.")


def android_cmake(source: Path, build: Path, extra: list) -> None:
    cache = build / "CMakeCache.txt"
    fresh = []
    if cache.is_file():
        entries = dict(re.findall(
            r"^(CMAKE_TOOLCHAIN_FILE|GSR_ANDROID_SDL2_DIR):[^=]+=(.*)$",
            cache.read_text(encoding="utf-8"), re.M,
        ))
        if (Path(entries.get("CMAKE_TOOLCHAIN_FILE", "")) != ANDROID_TOOLCHAIN
                or Path(entries.get("GSR_ANDROID_SDL2_DIR", "")) != SDL2_SOURCE):
            fresh = ["--fresh"]
        elif (build / "build.ninja").exists():
            return
    print(f"Configuring {build} ...")
    run(["cmake", *fresh, "-S", source, "-B", build, "-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={ninja()}",
         f"-DCMAKE_TOOLCHAIN_FILE={ANDROID_TOOLCHAIN}",
         f"-DANDROID_ABI={ABI}", f"-DANDROID_PLATFORM=android-{MIN_SDK}",
         "-DANDROID_STL=c++_shared", "-DCMAKE_BUILD_TYPE=Release",
         f"-DGSR_ANDROID_SDL2_DIR={SDL2_SOURCE}", *extra])


# ---- Clang and lld that run on the phone --------------------------------

def build_phone_llvm() -> Path:
    """One program (LLVM's multi-tool driver) holding Clang and lld, built
    for the phone from the LLVM 21.1.8 source. Tablegen runs on this PC, so
    it is built first with this PC's MinGW GCC."""
    android = PHONE_LLVM_BUILD
    llvm = android / "bin" / "llvm"
    if llvm.exists() and (android / "lib" / "clang").is_dir():
        return llvm
    host = LLVM_BUILD / "host"
    tblgen = host / "bin"
    common = ["-DCMAKE_BUILD_TYPE=Release", "-DLLVM_TARGETS_TO_BUILD=AArch64",
              "-DLLVM_INCLUDE_TESTS=OFF", "-DLLVM_INCLUDE_BENCHMARKS=OFF",
              "-DLLVM_INCLUDE_EXAMPLES=OFF", "-DCLANG_INCLUDE_TESTS=OFF",
              "-DLLVM_ENABLE_ZSTD=OFF", "-DLLVM_ENABLE_ZLIB=OFF", "-DLLVM_ENABLE_LIBXML2=OFF"]
    if not (tblgen / "clang-tblgen.exe").exists():
        print("Building LLVM's table generators for this PC ...")
        run(["cmake", "-S", LLVM_SOURCE / "llvm", "-B", host, "-G", "Ninja",
             f"-DCMAKE_MAKE_PROGRAM={ninja()}",
             "-DCMAKE_C_COMPILER=C:/msys64/mingw64/bin/gcc.exe",
             "-DCMAKE_CXX_COMPILER=C:/msys64/mingw64/bin/g++.exe",
             "-DLLVM_ENABLE_PROJECTS=clang", *common])
        run([ninja(), "-C", host, "llvm-tblgen", "clang-tblgen", "llvm-min-tblgen"])
    if not (android / "build.ninja").exists():
        print("Configuring Clang and lld for the phone ...")
        run(["cmake", "-S", LLVM_SOURCE / "llvm", "-B", android, "-G", "Ninja",
             f"-DCMAKE_MAKE_PROGRAM={ninja()}",
             f"-DCMAKE_TOOLCHAIN_FILE={ANDROID_TOOLCHAIN}",
             f"-DANDROID_ABI={ABI}", f"-DANDROID_PLATFORM=android-{MIN_SDK}",
             "-DANDROID_STL=c++_static", "-DLLVM_ENABLE_PROJECTS=clang;lld",
             "-DLLVM_HOST_TRIPLE=aarch64-linux-android",
             f"-DLLVM_DEFAULT_TARGET_TRIPLE=aarch64-linux-android{MIN_SDK}",
             f"-DLLVM_NATIVE_TOOL_DIR={tblgen}",
             f"-DLLVM_TABLEGEN={tblgen / 'llvm-tblgen.exe'}",
             f"-DCLANG_TABLEGEN={tblgen / 'clang-tblgen.exe'}",
             "-DLLVM_TOOL_LLVM_DRIVER_BUILD=ON", "-DLLVM_INCLUDE_DOCS=OFF",
             "-DLLVM_INCLUDE_UTILS=OFF", "-DCLANG_INCLUDE_DOCS=OFF",
             "-DCLANG_ENABLE_ARCMT=OFF", "-DCLANG_ENABLE_STATIC_ANALYZER=OFF",
             "-DCLANG_BUILD_TOOLS=OFF", "-DLLVM_BUILD_TOOLS=OFF",
             "-DLLVM_ENABLE_TERMINFO=OFF", "-DLLVM_ENABLE_LIBEDIT=OFF",
             "-DLLVM_ENABLE_LIBPFM=OFF", "-DLLVM_ENABLE_PIC=ON",
             "-DLLVM_ENABLE_LTO=Thin", "-DLLVM_PARALLEL_LINK_JOBS=1",
             "-DCMAKE_C_FLAGS=-fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables",
             "-DCMAKE_CXX_FLAGS=-fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables",
             *common])
    print("Building Clang and lld for the phone (long, the first time) ...")
    run([ninja(), "-C", android, "llvm-driver", "clang-resource-headers"])
    # The driver build "links" every tool name to the program; Windows makes
    # each a full copy (about 1.5 GB each, 65 GB in all). Only llvm is used.
    for copy in (android / "bin").iterdir():
        if copy.name != "llvm":
            copy.unlink()
    return llvm


# ---- our programs ----------------------------------------------------------

def corpus(path: Path, check: str) -> Path:
    if not (path / check).exists():
        fail(f"no generated game code in {path}. The Android build configures from this PC's "
             "game code (as the Linux release does); build both games on Windows first.")
    return path


def release_settings(version: str | None) -> list[str]:
    """What makes the launcher an official one, as make_release.ps1 sets it:
    the release version (default: the one the last release used, from
    Releases/make_release.log) and the report service's address
    (The Broken Seal/local/report_host.txt, never in git). The report
    service refuses launchers that say "dev"."""
    if not version:
        log = ROOT / "Releases" / "make_release.log"
        found = re.search(r"Version:\s*(\S+)", log.read_text(encoding="utf-8-sig")) if log.exists() else None
        version = found.group(1) if found else "dev"
    host_file = TBS / "local" / "report_host.txt"
    host = host_file.read_text(encoding="utf-8").strip() if host_file.exists() else ""
    print(f"Launcher version {version}; Send report {'on' if host else 'off (no report_host.txt)'}")
    return [f"-DGSR_RELEASE_VERSION={version}", f"-DGSR_REPORT_HOST={host}"]


def build_native(check_games: bool, version: str | None, profiling: bool = False) -> None:
    tbs_corpus = corpus(TBS / "local" / "gs011_block", "main/dispatch_table.cpp")
    tla_corpus = corpus(TLA / "local" / "tla_block", "main/dispatch_table.cpp")
    android_cmake(TBS, TBS_BUILD, [
        "-DGSR_BUILD_LOCAL_RUNNER=ON", "-DGSR_SPLIT_GAME_CODE=ON", "-DGSR_ENABLE_LTO=OFF",
        "-DGSR_DEBUG_SYMBOLS=OFF", "-DGSR_GAME_CODE_OPT=-Og",
        f"-DGSR_GAME_CODE_ROOT={tbs_corpus}", f"-DGSR_GENERATED_DIR={tbs_corpus / 'main'}",
        f"-DGSR_OVERLAY_ROOT={tbs_corpus}", f"-DGSR_STAMPS_DIR={tbs_corpus / 'stamps'}"])
    # Every build: the release settings may have changed since configuring.
    # The profiling switch too: the build folder keeps it between runs.
    run(["cmake", "-S", TBS, "-B", TBS_BUILD, *release_settings(version),
         f"-DGSR_LINUX_DEV_LAUNCHER={'ON' if profiling else 'OFF'}"],
        stdout=subprocess.DEVNULL)
    android_cmake(TLA, TLA_BUILD, [
        "-DTLA_SPLIT_GAME_CODE=ON", "-DTLA_ENABLE_LTO=OFF",
        "-DTLA_GAME_CODE_OPT=-Og", f"-DTLA_GENERATED_DIR={tla_corpus / 'main'}"])
    print("Compiling the launcher, engines and builders for the phone ...")
    tbs_targets = ["GoldenSunRecomp", "GoldenSunCollection", "gsr_builder", "gba_recompile"]
    tla_targets = ["TLARecomp", "tla_builder"]
    if check_games:
        tbs_targets.append("GoldenSunGame")
        tla_targets.append("TLAGame")
    run([ninja(), "-C", TBS_BUILD, *tbs_targets])
    run([ninja(), "-C", TLA_BUILD, *tla_targets])
    if check_games:
        audit_tail_jumps(TBS_BUILD / "CMakeFiles" / "gsr_game_objects.dir")
        audit_tail_jumps(TLA_BUILD / "CMakeFiles" / "tla_game_objects.dir")


def audit_tail_jumps(objects_dir: Path) -> None:
    """Every guest jump must be a host jump (scripts/tail_jump_audit.py)."""
    sys.path.insert(0, str(ROOT / "scripts"))
    import tail_jump_audit
    objdump = str(NDK_LLVM / "bin" / "llvm-objdump.exe")
    try:
        bad = tail_jump_audit.audit([objects_dir], objdump, "arm64")
    except (RuntimeError, subprocess.CalledProcessError) as error:
        fail(str(error))
    if not tail_jump_audit.report(bad):
        fail("some guest jumps compiled as calls; the game could run out of stack")


# ---- the files the app unpacks on the phone --------------------------------

def tbs_data(add) -> None:
    """The Broken Seal's builder/data, as scripts/make_release_linux.sh."""
    def language(source: Path, dest: str, extra: list[str]) -> None:
        for name in ["game_code_plan.txt", "main.toml", "overlay-registry.inc", *extra]:
            add(source / name, f"{dest}/{name}")
        for line in (source / "game_code_plan.txt").read_text(encoding="utf-8").splitlines():
            parts = line.split("#", 1)[0].split()
            if len(parts) >= 3 and parts[0] == "transient":
                add(source / parts[2], f"{dest}/{parts[2]}")
        for toml in sorted((source / "overlays").glob("*.toml")):
            add(toml, f"{dest}/overlays/{toml.name}")
    data = "games/The Broken Seal/builder/data"
    language(TBS / "config" / "usa", data, [])
    for code in later_revisions(TBS / "src" / "rom_revisions.inc"):
        language(TBS / "config" / code, f"{data}/{code}", ["overlay-sources.txt"])


def later_revisions(revision_list: Path) -> list[str]:
    """Every revision code after USA/Europe (languages, then patched ROMs)."""
    codes = re.findall(r'^GSR_ROM_REVISION\("([a-z]{2})"', revision_list.read_text(encoding="utf-8"), re.M)
    return codes[1:]


def tla_data(add) -> None:
    data = "games/The Lost Age/builder/data"
    add(TLA / "config" / "tla" / "main.toml", f"{data}/main.toml")
    for toml in sorted((TLA / "config" / "tla" / "reloc").glob("*.toml")):
        add(toml, f"{data}/reloc/{toml.name}")
    for code in later_revisions(TLA / "src" / "tla_rom_revisions.inc"):
        add(TLA / "config" / code / "main.toml", f"{data}/{code}/main.toml")
        for toml in sorted((TLA / "config" / code / "reloc").glob("*.toml")):
            add(toml, f"{data}/{code}/reloc/{toml.name}")


def payload(path: Path, llvm_build: Path) -> None:
    files: dict[str, Path] = {}

    def add(source: Path, name: str) -> None:
        if not source.is_file():
            fail(f"missing {source}")
        files[name] = source

    def add_tree(source: Path, name: str, skip=()) -> None:
        for f in sorted(source.rglob("*")):
            rel = f.relative_to(source).as_posix()
            if f.is_file() and not any(rel.startswith(s) for s in skip):
                add(f, f"{name}/{rel}")

    for asset in ("launcher_logo.png", "cartridge_broken_seal.png", "cartridge_lost_age.png"):
        add(ROOT / "assets" / asset, f"assets/{asset}")
    tbs_data(add)
    tla_data(add)
    for game in ("The Broken Seal", "The Lost Age"):
        # The overlay headers are for code the game builds in RAM while
        # playing (engine overlay_compile.cpp, compiled on the phone).
        for header in ("src/armv4t/runtime_arm.h", "src/armv4t/runtime_arm_types.h",
                       "src/runtime/overlay_runtime_arm.h", "src/runtime/overlay_abi.h"):
            add(ROOT / "engine" / header, f"games/{game}/builder/engine/include/{Path(header).name}")
    # The C/C++ library the game code compiles and links against (the NDK's,
    # for 64-bit ARM only) and Clang's own headers from the phone's Clang.
    sysroot = NDK_LLVM / "sysroot" / "usr"
    other_abis = ("arm-linux-androideabi", "i686-linux-android", "riscv64-linux-android",
                  "x86_64-linux-android")
    add_tree(sysroot / "include", "toolchain/sysroot/usr/include", skip=other_abis)
    libs = sysroot / "lib" / "aarch64-linux-android"
    add(libs / "libc++_shared.so", "toolchain/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so")
    for f in sorted((libs / str(MIN_SDK)).iterdir()):
        if f.suffix in (".o", ".so"):
            add(f, f"toolchain/sysroot/usr/lib/aarch64-linux-android/{MIN_SDK}/{f.name}")
    resource = next((llvm_build / "lib" / "clang").iterdir())
    add_tree(resource / "include", f"toolchain/lib/clang/{resource.name}/include")
    ndk_runtime = NDK_LLVM / "lib" / "clang" / "21" / "lib" / "linux"
    add(ndk_runtime / "libclang_rt.builtins-aarch64-android.a",
        f"toolchain/lib/clang/{resource.name}/lib/linux/libclang_rt.builtins-aarch64-android.a")
    add(ndk_runtime / "aarch64" / "libunwind.a",
        f"toolchain/lib/clang/{resource.name}/lib/linux/aarch64/libunwind.a")
    # Licences of what the app carries.
    add(ROOT / "engine" / "LICENSE", "licenses/ENGINE_LICENSE.txt")
    add(ROOT / "engine" / "THIRD_PARTY_ATTRIBUTION.md", "licenses/THIRD_PARTY_ATTRIBUTION.md")
    add(LLVM_SOURCE / "llvm" / "LICENSE.TXT", "licenses/LLVM_LICENSE.txt")
    add(SDL2_SOURCE / "LICENSE.txt", "licenses/SDL2_LICENSE.txt")
    add(NDK / "NOTICE", "licenses/ANDROID_NDK_NOTICE.txt")
    add(ROOT / "scripts" / "release" / "README.txt", "README.txt")
    add(ROOT / "scripts" / "release" / "MUSIC_README.txt", "Music/README.txt")
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for name, source in sorted(files.items()):
            z.write(source, name)
    print(f"    payload: {len(files)} files")


# ---- the APK -------------------------------------------------------------

def strip_into(source: Path, dest: Path) -> None:
    if not source.exists():
        fail(f"missing {source}")
    run([NDK_LLVM / "bin" / "llvm-strip.exe", "--strip-unneeded", "-o", dest, source])


def ensure_keystore() -> None:
    if KEYSTORE.exists():
        return
    KEYSTORE.parent.mkdir(parents=True, exist_ok=True)
    run([JDK / "bin" / "keytool.exe", "-genkeypair", "-keystore", KEYSTORE, "-alias", KEY_ALIAS,
         "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000",
         "-storepass", KEY_PASSWORD, "-keypass", KEY_PASSWORD,
         "-dname", "CN=Golden Sun Recompiled sideload"], env=tool_env())


def app_icon(res: Path) -> None:
    """The app icon: the launcher logo (assets/launcher_logo.png) on the
    launcher's own dark blue (collection_main.cpp), as an Android adaptive
    icon. The foreground is 108 dp at xxxhdpi (432 px); launchers show the
    middle 72 dp through their mask, so the logo is 270 px wide (about 68 dp)
    to keep its corners in for round masks."""
    from PIL import Image
    logo = Image.open(ROOT / "assets" / "launcher_logo.png").convert("RGBA")
    width = 270
    logo = logo.resize((width, round(logo.height * width / logo.width)), Image.LANCZOS)
    foreground = Image.new("RGBA", (432, 432), (0, 0, 0, 0))
    foreground.alpha_composite(logo, ((432 - logo.width) // 2, (432 - logo.height) // 2))
    (res / "drawable-xxxhdpi").mkdir(parents=True)
    foreground.save(res / "drawable-xxxhdpi" / "ic_launcher_foreground.png")
    (res / "values").mkdir()
    (res / "values" / "colors.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n<resources>\n'
        '    <color name="ic_launcher_background">#0A1620</color>\n</resources>\n',
        encoding="utf-8")
    (res / "mipmap-anydpi").mkdir()
    (res / "mipmap-anydpi" / "ic_launcher.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
        '    <background android:drawable="@color/ic_launcher_background" />\n'
        '    <foreground android:drawable="@drawable/ic_launcher_foreground" />\n'
        '</adaptive-icon>\n', encoding="utf-8")


def package(llvm: Path) -> Path:
    stage = TBS_BUILD / "apk"
    if stage.exists():
        shutil.rmtree(stage)
    (stage / "java").mkdir(parents=True)
    env = tool_env()
    print("Packing the APK ...")

    sources = [*sorted((HERE / "java").rglob("*.java")),
               *sorted((SDL2_SOURCE / "android-project" / "app" / "src" / "main" / "java").rglob("*.java"))]
    run([JDK / "bin" / "javac.exe", "-source", "8", "-target", "8", "-nowarn",
         "-Xlint:-options", "-encoding", "UTF-8", "-bootclasspath",
         os.pathsep.join([str(ANDROID_JAR), str(BUILD_TOOLS / "core-lambda-stubs.jar")]),
         "-d", stage / "java", *sources], env=env)
    classes = stage / "classes.jar"
    with zipfile.ZipFile(classes, "w") as jar:  # one jar: d8.bat's command line is short
        for cls in sorted((stage / "java").rglob("*.class")):
            jar.write(cls, cls.relative_to(stage / "java").as_posix())
    run([BUILD_TOOLS / "d8.bat", "--release", "--min-api", str(MIN_SDK),
         "--lib", ANDROID_JAR, "--output", stage, classes], env=env)

    manifest = (HERE / "AndroidManifest.xml.in").read_text(encoding="utf-8")
    (stage / "AndroidManifest.xml").write_text(
        manifest.replace("@PACKAGE@", PACKAGE).replace("@LABEL@", LABEL)
        .replace("@DEBUGGABLE@", "true" if DEBUGGABLE else "false"), encoding="utf-8")
    res = stage / "res"
    app_icon(res)
    compiled = stage / "res.zip"
    run([BUILD_TOOLS / "aapt2.exe", "compile", "--dir", res, "-o", compiled])
    base = stage / "base.apk"
    run([BUILD_TOOLS / "aapt2.exe", "link", "-o", base, "-I", ANDROID_JAR,
         "--manifest", stage / "AndroidManifest.xml",
         "--min-sdk-version", str(MIN_SDK), "--target-sdk-version", str(TARGET_SDK),
         "--version-code", str(int(time.time() // 60)),  # newer builds install over older
         "--version-name", APP_VERSION or time.strftime("%Y-%m-%d %H:%M"), compiled])

    libs = stage / "lib"
    libs.mkdir()
    recompiler = next((p for p in TBS_BUILD.rglob("gba_recompile") if p.is_file()), None)
    shipped = {
        "libmain.so": TBS_BUILD / "libmain.so",
        "libSDL2.so": TBS_BUILD / "gbarecomp_build" / "sdl2_build" / "libSDL2.so",
        "libc++_shared.so": NDK_LLVM / "sysroot" / "usr" / "lib" / "aarch64-linux-android" / "libc++_shared.so",
        "libgs_engine.so": TBS_BUILD / "libgs_engine.so",
        "libtla_engine.so": TLA_BUILD / "libtla_engine.so",
        # Programs, named lib*.so so Android unpacks them where they may run.
        "libgsr_builder.so": TBS_BUILD / "gsr_builder",
        "libtla_builder.so": TLA_BUILD / "tla_builder",
        "libgba_recompile.so": recompiler or TBS_BUILD / "gba_recompile",
        "libllvm.so": llvm,
    }
    for name, source in shipped.items():
        strip_into(source, libs / name)
    payload(stage / "payload.zip", llvm.parent.parent)

    unsigned = stage / "unsigned.apk"
    shutil.copy(base, unsigned)
    with zipfile.ZipFile(unsigned, "a", zipfile.ZIP_DEFLATED) as apk:
        apk.write(stage / "classes.dex", "classes.dex")
        apk.write(stage / "payload.zip", "assets/payload.zip", zipfile.ZIP_STORED)
        for lib in sorted(libs.iterdir()):
            apk.write(lib, f"lib/{ABI}/{lib.name}")
    aligned = stage / "aligned.apk"
    run([BUILD_TOOLS / "zipalign.exe", "-P", "16", "-f", "4", unsigned, aligned])
    ensure_keystore()
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    run([BUILD_TOOLS / "apksigner.bat", "sign", "--ks", KEYSTORE, "--ks-key-alias", KEY_ALIAS,
         "--ks-pass", f"pass:{KEY_PASSWORD}", "--key-pass", f"pass:{KEY_PASSWORD}",
         "--out", OUTPUT, aligned], env=env)
    run([BUILD_TOOLS / "apksigner.bat", "verify", OUTPUT], env=env)
    return OUTPUT


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check-games", action="store_true")
    parser.add_argument("--package-only", action="store_true")
    parser.add_argument("--check-tools", action="store_true", help="check Android prerequisites without building")
    parser.add_argument("--release-progress", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--version", help="release version the launcher reports (default: the last release's)")
    parser.add_argument("--release", action="store_true",
                        help="an APK to share: not open to adb's debugging (make_release.ps1 -Android)")
    parser.add_argument("--profiling", action="store_true",
                        help="developer launcher: cost probe, present cadence and frame timing CSVs in every session")
    args = parser.parse_args()
    if args.profiling and (args.release or args.package_only):
        fail("--profiling builds the launcher; it cannot be used with --release or --package-only")
    missing = missing_tools()
    if args.check_tools:
        for path in missing:
            print(f"Missing Android tool: {path}")
        if missing:
            sys.exit(1)
        return
    if missing:
        fail(f"missing Android tool: {missing[0]}")
    global DEBUGGABLE, APP_VERSION, OUTPUT
    if args.profiling:
        OUTPUT = PROFILING_OUTPUT
    if args.release:
        DEBUGGABLE = False
    if args.version and args.version != "dev":
        APP_VERSION = args.version
    if not args.package_only:
        no_other_build()
        llvm = build_phone_llvm()
        if args.release_progress:
            print("@release-stage Android: building the launcher and both games", flush=True)
        build_native(args.check_games, args.version, args.profiling)
    else:
        llvm = PHONE_LLVM_BUILD / "bin" / "llvm"
    if args.release_progress:
        print("@release-stage Android: packing and signing the APK", flush=True)
    apk = package(llvm)
    print(f"\nDone: {apk} ({apk.stat().st_size / (1 << 20):.0f} MB)")
    print("Copy it to the phone and open it to install.")


if __name__ == "__main__":
    main()

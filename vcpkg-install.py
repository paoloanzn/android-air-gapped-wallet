#!/usr/bin/env python3
"""Requires CMake, Ninja and a bootstrapped VCPKG_ROOT.

Examples:
    python3 vcpkg-install.py imgui
    python3 vcpkg-install.py 'imgui[android-binding]'
    python3 vcpkg-install.py fmt fmt fmt::fmt
"""

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys


def run(*args: str) -> None:
    subprocess.run(args, check=True)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Install a vcpkg port for Android and link it to airgap.",
        epilog="Example: python3 vcpkg-install.py imgui",
    )
    parser.add_argument("port", help="Port name, optionally with features, e.g. imgui[android-binding]")
    parser.add_argument("cmake", nargs="*", metavar="CMAKE_NAME", help="Optional CMake package followed by one or more targets")
    args = parser.parse_args()
    if len(args.cmake) == 1:
        parser.error("provide both a CMake package and at least one CMake target")
    if not os.environ.get("VCPKG_ROOT"):
        parser.error("export VCPKG_ROOT pointing to a bootstrapped vcpkg checkout")

    # Resolve before changing directories so relative environment paths still work.
    os.environ["VCPKG_ROOT"] = str(Path(os.environ["VCPKG_ROOT"]).resolve())
    if not os.environ.get("NDK"):
        # Keep the default in sync with native/setup-ndk.sh.
        sdk = os.environ.get("ANDROID_SDK_ROOT") or str(Path.home() / "Library/Android/sdk")
        os.environ["NDK"] = str(Path(sdk) / "ndk/27.1.12297006")
    os.environ["NDK"] = str(Path(os.environ["NDK"]).resolve())
    os.chdir(Path(__file__).resolve().parent)

    spec: str = args.port
    if not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*(?:\[[a-z0-9,-]+\])?", spec):
        sys.exit("Expected a vcpkg port, optionally with features: 'imgui[android-binding]'")
    port = spec.split("[")[0]
    overrides: list[str] = args.cmake
    if any(not re.fullmatch(r"[A-Za-z0-9_.:+-]+", value) for value in overrides):
        sys.exit("Invalid CMake package or target name")

    manifest = Path("vcpkg.json")
    cmakelists = Path("CMakeLists.txt")
    original_manifest = manifest.read_bytes()
    original_cmake = cmakelists.read_bytes()

    try:
        run(str(Path(os.environ["VCPKG_ROOT"]) / "vcpkg"), "add", "port", spec)
        # Manifest mode installs using the same NDK, ABI and triplet as the app.
        run("cmake", "--preset", "android")

        if overrides:
            package, *targets = overrides
        else:
            cache: dict[str, str] = {}
            for line in Path("build/CMakeCache.txt").read_text().splitlines():
                match = re.match(r"([^:#/][^:]*):[^=]+=(.*)", line)
                if match:
                    cache[match[1]] = match[2]
            installed = Path(cache["VCPKG_INSTALLED_DIR"])
            triplet = cache["VCPKG_TARGET_TRIPLET"]
            lists = list((installed / "vcpkg/info").glob(f"{port}_*_{triplet}.list"))
            if len(lists) != 1:
                raise RuntimeError(f"Cannot locate installed file list for {port}:{triplet}")

            packages: set[str] = set()
            libraries: set[str] = set()
            for relative in lists[0].read_text().splitlines():
                path = installed / relative
                if path.suffix != ".cmake" or not path.is_file():
                    continue
                config = re.fullmatch(r"(.+?)(?:Config|-config)\.cmake", path.name)
                if config:
                    packages.add(config[1])
                # Only literal imported libraries are safe to infer automatically.
                content = re.sub(r"#[^\n]*", "", path.read_text())
                libraries.update(re.findall(
                    r'add_library\s*\(\s*"?([A-Za-z0-9_.:+-]+)"?\s+'
                    r'(?:STATIC|SHARED|INTERFACE|UNKNOWN)\s+IMPORTED\b',
                    content, re.IGNORECASE,
                ))
            if len(packages) != 1 or len(libraries) != 1:
                raise RuntimeError(
                    f"CMake integration is ambiguous for {port}. "
                    f"Packages: {sorted(packages)}; targets: {sorted(libraries)}. "
                    "Retry with: python3 vcpkg-install.py PORT CMAKE_PACKAGE CMAKE_TARGET ..."
                )
            package = packages.pop()
            targets = sorted(libraries)

        begin = f"# BEGIN vcpkg-install: {port}"
        end = f"# END vcpkg-install: {port}"
        lines = [begin, f"find_package({package} REQUIRED)"]
        for target in targets:
            lines.extend([
                f"if(NOT TARGET {target})",
                f'    message(FATAL_ERROR "{package} did not define target {target}")',
                "endif()",
            ])
        lines.extend([f"target_link_libraries(airgap PRIVATE {' '.join(targets)})", end])
        block = "\n".join(lines)
        content = original_cmake.decode()
        pattern = re.escape(begin) + r"\n.*?" + re.escape(end)
        if begin in content:
            if len(re.findall(pattern, content, flags=re.DOTALL)) != 1:
                raise RuntimeError(f"Malformed managed CMake block for {port}")
            content = re.sub(pattern, lambda _: block, content, flags=re.DOTALL)
        else:
            content = content.rstrip() + "\n\n" + block + "\n"
        cmakelists.write_text(content)
        run("cmake", "--preset", "android")
    except (OSError, RuntimeError, KeyError, subprocess.CalledProcessError, KeyboardInterrupt) as error:
        manifest.write_bytes(original_manifest)
        cmakelists.write_bytes(original_cmake)
        sys.exit(f"Installation/integration failed; restored vcpkg.json and CMakeLists.txt.\n{error}")

    print(f"Installed {spec} and linked airgap to {', '.join(targets)}.")


if __name__ == "__main__":
    main()

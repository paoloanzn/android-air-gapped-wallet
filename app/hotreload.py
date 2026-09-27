#!/usr/bin/env python3
"""Rebuild native changes and open the on-device APK installer."""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import threading
import time

try:
    from watchdog.events import FileSystemEvent, FileSystemEventHandler
    from watchdog.observers import Observer
except ImportError:
    sys.exit("Install watchdog first: python3 -m pip install -r app/requirements-dev.txt")


ROOT = Path(__file__).resolve().parent.parent
PACKAGE = "com.paoloanzn.airgap"
ACTIVITY = f"{PACKAGE}/android.app.NativeActivity"
SOURCE_SUFFIXES = {".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".hxx", ".inl"}


class Changes(FileSystemEventHandler):
    def __init__(self) -> None:
        self.condition = threading.Condition()
        self.changed_at: float | None = None

    def on_any_event(self, event: FileSystemEvent) -> None:
        if event.is_directory or event.event_type not in {
            "created", "modified", "deleted", "moved"
        }:
            return
        paths = (os.fsdecode(event.src_path), os.fsdecode(event.dest_path))
        if any(Path(path).suffix.lower() in SOURCE_SUFFIXES for path in paths):
            with self.condition:
                self.changed_at = time.monotonic()
                self.condition.notify()

    def wait_for_save(self) -> None:
        with self.condition:
            while True:
                if self.changed_at is not None:
                    remaining = self.changed_at + 0.5 - time.monotonic()
                    if remaining <= 0:
                        self.changed_at = None
                        return
                    self.condition.wait(timeout=remaining)
                else:
                    self.condition.wait(timeout=0.5)


def adb(*args: str, capture: bool = False) -> str:
    return subprocess.run(
        ["adb", *args], check=True, text=True,
        stdout=subprocess.PIPE if capture else None,
    ).stdout or ""


def installed_version() -> str | None:
    details = adb("shell", "dumpsys", "package", PACKAGE, capture=True)
    match = re.search(r"^\s*lastUpdateTime=(.+)$", details, re.MULTILINE)
    return match.group(1).strip() if match else None


def open_installer() -> None:
    # Files grants the installer content-URI access that adb cannot grant.
    folder = "/sdcard/Download/airgap-hotreload"
    adb("shell", "mkdir", "-p", folder)
    adb("shell", "cp", "/sdcard/airgap.apk", f"{folder}/airgap.apk")
    adb(
        "shell", "am", "start", "-W", "-a", "android.intent.action.VIEW",
        "-d", "content://com.android.externalstorage.documents/document/"
        "primary%3ADownload%2Fairgap-hotreload",
        "-t", "vnd.android.document/directory",
    )
    # Xiaomi also restricts ADB input injection; use a real tap on the phone.
    print("Tap airgap.apk in Download/airgap-hotreload on the phone.", flush=True)


def reload_app() -> None:
    print("Building native changes...", flush=True)
    subprocess.run(["sh", "app/build.sh"], cwd=ROOT, check=True)
    previous = installed_version()
    open_installer()
    print("Confirm installation on the phone. Waiting up to 3 minutes...", flush=True)
    deadline = time.monotonic() + 180
    while time.monotonic() < deadline:
        current = installed_version()
        if current is not None and current != previous:
            adb("shell", "am", "force-stop", PACKAGE)
            adb("shell", "am", "start", "-W", "-n", ACTIVITY)
            print("App restarted. Watching for changes...", flush=True)
            return
        time.sleep(1)
    raise RuntimeError("Installation was not detected; save a source file to retry.")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", help="ADB device serial (or set ANDROID_SERIAL)")
    args = parser.parse_args()
    if args.serial:
        # Also selects the device for adb commands inside build.sh and CMake.
        os.environ["ANDROID_SERIAL"] = args.serial
    if not shutil.which("adb"):
        parser.error("adb must be available on PATH")
    changes = Changes()
    observer = Observer()
    observer.schedule(changes, str(ROOT / "native"), recursive=True)
    observer.start()
    print("Watching native/ for C/C++ source and header changes. Ctrl-C to stop.", flush=True)
    try:
        while True:
            changes.wait_for_save()
            try:
                reload_app()
            except (subprocess.CalledProcessError, OSError, RuntimeError) as error:
                print(f"Reload failed: {error}\nWatching for the next change...", file=sys.stderr)
    except KeyboardInterrupt:
        print("\nStopping watcher.")
    finally:
        observer.stop()
        observer.join()


if __name__ == "__main__":
    main()

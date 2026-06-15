#!/usr/bin/env python3
"""Build (optional) and package a self-contained HiveWE release zip.

Why this exists: releases were shipped missing the MSVC C++ runtime
(vcruntime140*.dll, msvcp140*.dll). They ran on dev machines (Visual Studio
installs the runtime into System32) but failed on a clean machine in the loader,
before main(), so nothing was even logged. This script stages the build output,
*asserts* every critical file is present (so a missing runtime fails the build
instead of the user's friend), and zips a flat, self-contained release.

Usage (from anywhere):
    python scripts/package_release.py                 # package current build -> HiveWE_v<ver>.zip
    python scripts/package_release.py --version 0.11.2
    python scripts/package_release.py --build         # cmake build Release first, then package
    python scripts/package_release.py --replace-old   # delete previous HiveWE_v*.zip after packaging

The zip is written to the repo root (matching the existing release convention)
and is git-ignored (*.zip).
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
# Multi-config build tree produced by `cmake --preset Release`.
BUILD_DIR = REPO_ROOT / "build" / "Release" / "Release"
DATA_SRC = REPO_ROOT / "data"
DEFAULT_VERSION = "0.11.2"

# Files copied from the build dir into the release. Executables + every runtime
# DLL the build placed next to them (Qt, StormLib, CascLib, MSVC runtime, ...).
EXES = ["HiveWE.exe", "HiveWE_cli.exe"]
# Qt plugin folders that must travel with the exe (qwindows.dll lives here).
PLUGIN_DIRS = ["platforms", "styles", "imageformats"]
# Build artefacts that must NOT ship.
EXCLUDE_NAMES = {
    "HiveWE_tests.exe",
    "hivewe.log",
    "hivewe_run.log",
    "data",  # the build tree's `data` is a symlink; we copy the real tree instead
}
EXCLUDE_SUFFIXES = {".lib", ".exp", ".pdb", ".ilk", ".manifest"}

# Hard requirement: if any of these is missing from the staged release, abort.
# This is the guard that stops "works on my machine" releases. The MSVC runtime
# entries are the exact DLLs whose absence broke the last release.
REQUIRED_FILES = [
    "HiveWE.exe",
    "vcruntime140.dll",
    "vcruntime140_1.dll",
    "msvcp140.dll",
    "msvcp140_atomic_wait.dll",
    "Qt6Core.dll",
    "Qt6Gui.dll",
    "Qt6Widgets.dll",
    "platforms/qwindows.dll",
]


def log(msg: str) -> None:
    print(f"[package] {msg}", flush=True)


def fail(msg: str) -> "None":
    print(f"[package][ERROR] {msg}", file=sys.stderr, flush=True)
    sys.exit(1)


def run_build() -> None:
    log("Building Release (cmake --build --preset Release)...")
    env = os.environ.copy()
    env.setdefault("VCPKG_ROOT", "C:/vcpkg")
    res = subprocess.run(
        ["cmake", "--build", "--preset", "Release"],
        cwd=REPO_ROOT,
        env=env,
    )
    if res.returncode != 0:
        fail("cmake build failed; aborting package.")


def find_dumpbin() -> Path | None:
    """Locate dumpbin.exe via vswhere, else PATH. Used for the optional, deeper
    dependency check. Absence is non-fatal."""
    vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) \
        / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    roots: list[Path] = []
    if vswhere.exists():
        try:
            out = subprocess.run(
                [str(vswhere), "-latest", "-property", "installationPath"],
                capture_output=True, text=True,
            ).stdout.strip()
            if out:
                roots.append(Path(out))
        except Exception:
            pass
    # Also honour an explicit VS root some setups use.
    env_vs = os.environ.get("VSINSTALLDIR")
    if env_vs:
        roots.append(Path(env_vs))
    for root in roots:
        hits = sorted((root / "VC" / "Tools" / "MSVC").glob("*/bin/Hostx64/x64/dumpbin.exe"))
        if hits:
            return hits[-1]
    found = shutil.which("dumpbin")
    return Path(found) if found else None


def imported_dlls(dumpbin: Path, binary: Path) -> list[str]:
    out = subprocess.run(
        [str(dumpbin), "/DEPENDENTS", str(binary)],
        capture_output=True, text=True,
    ).stdout
    dlls = []
    for line in out.splitlines():
        s = line.strip()
        if s.lower().endswith(".dll") and " " not in s:
            dlls.append(s)
    return dlls


def verify_dependencies(stage: Path, dumpbin: Path | None) -> None:
    """Best-effort: walk the exe's imports and confirm every non-system DLL is
    present in the staged release. Catches a forgotten dependency that the
    REQUIRED_FILES list doesn't explicitly name."""
    if dumpbin is None:
        log("dumpbin not found - skipping deep dependency check (REQUIRED_FILES still enforced).")
        return
    present = {p.name.lower() for p in stage.iterdir() if p.is_file()}
    # System DLLs provided by Windows itself; never bundled.
    def is_system(name: str) -> bool:
        n = name.lower()
        return (
            n.startswith("api-ms-win-")
            or n.startswith("ext-ms-")
            or n in {
                "kernel32.dll", "user32.dll", "shell32.dll", "gdi32.dll",
                "advapi32.dll", "ole32.dll", "oleaut32.dll", "ws2_32.dll",
                "imm32.dll", "opengl32.dll", "comdlg32.dll", "winmm.dll",
                "version.dll", "setupapi.dll", "dwmapi.dll", "uxtheme.dll",
                "ntdll.dll", "msvcrt.dll", "rpcrt4.dll", "crypt32.dll",
                "d3d9.dll", "dxgi.dll", "d3d11.dll", "userenv.dll",
                "bcrypt.dll", "wtsapi32.dll", "netapi32.dll", "secur32.dll",
            }
        )
    exe = stage / "HiveWE.exe"
    missing = sorted(
        d for d in imported_dlls(dumpbin, exe)
        if not is_system(d) and d.lower() not in present
    )
    if missing:
        fail("HiveWE.exe imports DLLs missing from the release: " + ", ".join(missing))
    log("Dependency check passed: all non-system imports are bundled.")


def stage_release(stage: Path) -> None:
    if not BUILD_DIR.exists():
        fail(f"Build output not found: {BUILD_DIR}\n  Run `cmake --build --preset Release` first, or pass --build.")

    # Executables.
    for exe in EXES:
        src = BUILD_DIR / exe
        if src.exists():
            shutil.copy2(src, stage / exe)
        elif exe == "HiveWE.exe":
            fail(f"Missing {exe} in build output.")
        else:
            log(f"(optional) {exe} not in build output - skipping.")

    # Every DLL the build placed next to the exe (Qt, Storm, Casc, MSVC runtime).
    for dll in BUILD_DIR.glob("*.dll"):
        shutil.copy2(dll, stage / dll.name)

    # Qt plugin folders.
    for pdir in PLUGIN_DIRS:
        src = BUILD_DIR / pdir
        if src.is_dir():
            shutil.copytree(src, stage / pdir, dirs_exist_ok=True,
                            ignore=shutil.ignore_patterns("*.pdb"))
        else:
            log(f"Plugin folder '{pdir}' not found in build output - skipping.")

    # Real data tree (the build tree's `data` is a symlink to this).
    if not DATA_SRC.is_dir():
        fail(f"data folder not found: {DATA_SRC}")
    shutil.copytree(DATA_SRC, stage / "data", dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns(".git*"))

    # Ship the user-facing requirements / troubleshooting notes.
    req = REPO_ROOT / "REQUIREMENTS.txt"
    if req.exists():
        shutil.copy2(req, stage / "REQUIREMENTS.txt")
    else:
        log("REQUIREMENTS.txt not found in repo root - skipping (consider adding it).")

    # Clean, machine-independent launcher (double-clicking the exe also works:
    # it pins its working dir to its own folder on startup).
    (stage / "run_hivewe.bat").write_text(
        "@echo off\r\n"
        "rem Launch HiveWE from its own folder.\r\n"
        "cd /d \"%~dp0\"\r\n"
        "start \"\" \"%~dp0HiveWE.exe\"\r\n",
        encoding="ascii",
    )

    # Drop excluded artefacts that the glob may have pulled in.
    for child in list(stage.iterdir()):
        if child.is_file() and (
            child.name in EXCLUDE_NAMES or child.suffix.lower() in EXCLUDE_SUFFIXES
        ):
            child.unlink()


def verify_required(stage: Path) -> None:
    missing = [rel for rel in REQUIRED_FILES if not (stage / rel).exists()]
    if missing:
        fail("Release is missing required files:\n  - " + "\n  - ".join(missing))
    data_files = sum(1 for _ in (stage / "data").rglob("*") if _.is_file())
    if data_files == 0:
        fail("Staged data/ folder is empty.")
    log(f"Required-file check passed ({data_files} data files bundled).")


def make_zip(stage: Path, version: str) -> Path:
    zip_path = REPO_ROOT / f"HiveWE_v{version}.zip"
    if zip_path.exists():
        zip_path.unlink()
    log(f"Writing {zip_path.name} ...")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for path in sorted(stage.rglob("*")):
            if path.is_file():
                z.write(path, path.relative_to(stage).as_posix())
    return zip_path


def main() -> None:
    ap = argparse.ArgumentParser(description="Package a self-contained HiveWE release zip.")
    ap.add_argument("--version", default=DEFAULT_VERSION, help=f"Release version (default {DEFAULT_VERSION}).")
    ap.add_argument("--build", action="store_true", help="Run `cmake --build --preset Release` first.")
    ap.add_argument("--replace-old", action="store_true",
                    help="Delete other HiveWE_v*.zip in the repo root after a successful package.")
    args = ap.parse_args()

    if args.build:
        run_build()

    dumpbin = find_dumpbin()
    with tempfile.TemporaryDirectory(prefix="hivewe_release_") as tmp:
        stage = Path(tmp)
        log(f"Staging release from {BUILD_DIR}")
        stage_release(stage)
        verify_required(stage)
        verify_dependencies(stage, dumpbin)
        zip_path = make_zip(stage, args.version)

    size_mb = zip_path.stat().st_size / (1024 * 1024)
    log(f"Done: {zip_path}  ({size_mb:.1f} MB)")

    if args.replace_old:
        for old in REPO_ROOT.glob("HiveWE_v*.zip"):
            if old.resolve() != zip_path.resolve():
                log(f"Removing previous release: {old.name}")
                old.unlink()


if __name__ == "__main__":
    main()

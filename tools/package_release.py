"""Package a clean committed checkout after the frozen offline smoke check."""
import argparse
import hashlib
from importlib.metadata import distribution
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VERSION = "1.1.0-rc.1"


def run(*arguments, cwd=ROOT):
    return subprocess.run(arguments, cwd=cwd, capture_output=True, check=True, text=True, encoding="utf-8").stdout


def copy_licenses(bundle):
    destination = bundle / "licenses"
    destination.mkdir(exist_ok=True)
    for name in ("PySide6", "PySide6_Essentials", "PySide6_Addons", "shiboken6", "pyserial"):
        metadata = distribution(name)
        for file in metadata.files or []:
            if any("license" in part.lower() or "copying" in part.lower() for part in file.parts):
                source = Path(metadata.locate_file(file))
                if source.is_file():
                    if Path(file).is_absolute() or ".." in file.parts:
                        continue
                    target = destination / name / Path(*file.parts)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(source, target)
    python_license = Path(sys.base_prefix) / "LICENSE.txt"
    if python_license.exists():
        shutil.copy2(python_license, destination / "PYTHON-LICENSE.txt")
    shutil.copy2(ROOT / "THIRD_PARTY_NOTICES.md", destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "dist/release")
    args = parser.parse_args()
    if run("git", "status", "--porcelain").strip():
        raise RuntimeError("Commit changes before packaging the release.")
    revision = run("git", "rev-parse", "HEAD").strip()
    bundle = ROOT / f"GUI/dist/TCallPager-{VERSION}-windows"
    smoke = subprocess.run([str(bundle / f"TCallPager-{VERSION}-selfcheck.exe"), "--self-check"],
                           cwd=bundle, capture_output=True, timeout=30)
    if smoke.returncode or b"SELF-CHECK PASSED" not in smoke.stdout:
        raise RuntimeError(f"Frozen check failed: {smoke.stdout!r} {smoke.stderr!r}")
    copy_licenses(bundle)
    args.output.mkdir(parents=True, exist_ok=True)
    desktop_zip = args.output / f"TCallPager-{VERSION}-windows.zip"
    with zipfile.ZipFile(desktop_zip, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for file in sorted(bundle.rglob("*")):
            if file.is_file():
                archive.write(file, Path(bundle.name) / file.relative_to(bundle))
    source_zip = args.output / f"TCall_Pager-{VERSION}-source.zip"
    files = run("git", "ls-files", "-z").split("\0")
    with zipfile.ZipFile(source_zip, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for name in sorted(file for file in files if file):
            archive.write(ROOT / name, "TCall_Pager/" + name)
        archive.writestr("TCall_Pager/REVISION.txt", revision + "\n")
    checksums = []
    for file in (desktop_zip, source_zip):
        with file.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        checksums.append(f"{digest}  {file.name}\n")
        print(file.name, file.stat().st_size)
    (args.output / "SHA256SUMS.txt").write_text("".join(checksums), encoding="ascii")
    print("Revision:", revision)


if __name__ == "__main__":
    main()

# Build from GUI/: python -m PyInstaller --clean --noconfirm gui.spec
from pathlib import Path
from importlib.util import find_spec

version = "1.1.0-rc.2"
name = f"TCallPager-{version}"
analysis = Analysis(
    ["launcher.py"], pathex=[str(Path(SPECPATH))], binaries=[], datas=[],
    hiddenimports=["serial.tools.list_ports", "serial.tools.list_ports_windows"],
    hookspath=[], hooksconfig={}, runtime_hooks=[],
    excludes=["PyQt6", "tkinter", "unittest"], noarchive=False,
)
# Qt uses the Windows ICU/API-set contracts. Do not ship an unrelated ICU
# distribution or local copies of operating-system API-set stubs beside it.
analysis.binaries = [entry for entry in analysis.binaries
    if not (Path(entry[0]).name.lower().startswith("api-ms-win-")
            or Path(entry[0]).name.lower() in {"ucrtbase.dll", "icuuc.dll"}
            or Path(entry[0]).name.lower().startswith("icudt"))]
# Ship one consistent MSVC runtime version from the Qt distribution instead
# of letting an older Python runtime DLL win dependency collection.
qt_directory = Path(find_spec("PySide6").origin).parent
runtime_names = {path.name.lower() for path in qt_directory.glob("*140*.dll")}
analysis.binaries = [entry for entry in analysis.binaries
                     if Path(entry[0]).name.lower() not in runtime_names]
analysis.binaries += [(path.name, str(path), "BINARY") for path in qt_directory.glob("*140*.dll")]
archive = PYZ(analysis.pure)
app = EXE(archive, analysis.scripts, [], exclude_binaries=True,
          name=name, console=False, debug=False, strip=False, upx=False,
          disable_windowed_traceback=False)
check = EXE(archive, analysis.scripts, [], exclude_binaries=True,
            name=name + "-selfcheck", console=True, debug=False, strip=False, upx=False)
bundle = COLLECT(app, check, analysis.binaries, analysis.datas,
                 strip=False, upx=False, name=name + "-windows")

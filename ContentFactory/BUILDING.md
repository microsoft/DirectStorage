# Build Content Factory

Build a Windows x64 portable folder containing the launcher, Python runtime, and all three encoders.

## Prerequisites

- Windows x64 with 64-bit Python.
- Visual Studio 2022 or Build Tools with MSVC and the Windows SDK.
- CMake on `PATH`, or supply its path with `--cmake`.
- An existing, complete GACL checkout supplied through `--gacl-root` or `GACL_ROOT`. Dependencies are not downloaded.
- PyInstaller installed in the Python environment used for packaging:

```powershell
python -m pip install -r .\ContentFactory\requirements-build.txt
```

## Build

From the DirectStorage repository root:

```powershell
python .\ContentFactory\build-package.py `
  --gacl-root C:\src\gacl `
  --build-directory .\ContentFactory\build `
  --output-directory ..\ContentFactory-package `
  --configuration Release
```

`--configuration` also accepts `RelWithDebInfo`, `MinSizeRel`, and `Debug`. Use `--help` to inspect arguments without building.

The output directory must be new, outside both checkouts, and separate from the build directory. Existing outputs are never deleted. Build artifacts can be reused; in-repository builds must stay under `ContentFactory/build/`, never inside the GACL checkout.

## Distribute

Copy the **entire output folder**, including `ContentFactory.exe`, `_internal/`, `bin/`, and `licenses/`. Users do not install Python or encoders separately. Usage is in `README.md`; the full JSON field guide is `CONFIGURATION.md`, with an editable `examples/request.json`.

The package has been built and smoke-tested locally. That does not establish clean-machine portability or unrestricted native correctness. Test on a separate machine before distribution.

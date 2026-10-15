# Building the portable ContentFactory package

This build entry point assembles a **Windows x64 folder distribution**, not a
single standalone executable. The folder contains `ContentFactory.exe`, the
Python runtime and modules collected by PyInstaller, and the three native tools
in `bin/`. Copy the **entire folder**, including `_internal/`, to the destination
machine. A separately installed Python interpreter is not required by that
packaged launcher.

## Build-machine prerequisites

- Windows x64 and a 64-bit Python interpreter.
- Visual Studio 2022 or its Build Tools, with the MSVC C++ toolchain and Windows SDK.
- CMake available by its explicit path or as the `cmake` build command.
- A complete, existing GACL dependency checkout, supplied as `--gacl-root` or
  through the `GACL_ROOT` environment variable. The build uses that checkout;
  it does not fetch or download dependencies.
- PyInstaller in the **same Python environment** used to run the build script.
  `requirements-build.txt` restricts it to `>=6.16,<7`.

The dependency-install command below is for a user-approved packaging setup.
Neither installation nor a native build is needed for source-only Python tests.

```powershell
python -m pip install -r .\ContentFactory\requirements-build.txt
```

## Build and package

Run from the public DirectStorage repository root after approving a native
build. Use an output directory **outside both source checkouts**, distinct from
the build directory, and not already present:

```powershell
python .\ContentFactory\build-package.py `
  --gacl-root C:\src\gacl `
  --build-directory .\ContentFactory\build `
  --output-directory ..\ContentFactory-package `
  --configuration Release
```

`--configuration` accepts `Release`, `RelWithDebInfo`, `MinSizeRel`, or `Debug`.
`--cmake` can supply an explicit CMake executable path. The build deliberately
uses `Visual Studio 17 2022` and `-A x64`; unsupported platforms/toolchains are
rejected. `--help` only prints arguments and does not configure or build anything.

The script:

1. Validates platform, build-tool availability, source files, required licenses,
   and directory safety before running native commands.
2. Configures the `ContentFactory` CMake project using the supplied GACL checkout.
3. Builds `zstd`, `GDeflateContentTool`, and `GACLContentTool` into
   `<build-directory>/native/bin/<configuration>/`.
4. Runs this interpreter's `python -m PyInstaller --onedir --name ContentFactory`
   on `ContentFactory/tools/run-content-factory.py`.
5. Assembles native executables and license files in a temporary sibling folder,
   then publishes the complete package by renaming it to the new output directory.

The native build selects the static MSVC runtime with CMP0091 and
`MultiThreaded$<$<CONFIG:Debug>:Debug>`. The packaging configuration enables the
zstd CLI with its static library and uses the existing GDeflate subtree with its
DirectStorage demo and tests disabled. Building does not run any native tool.

## Distribution layout

```text
ContentFactory-package/
  ContentFactory.exe
  _internal/                 # PyInstaller's bundled Python runtime and modules
  bin/
    zstd.exe
    GDeflateContentTool.exe
    GACLContentTool.exe
  licenses/                  # Required source/dependency licenses and notices
```

The package is a tool installation, not an input-data bundle. Input payloads, user-edited
JSON requests, reports, and generated output remain local files selected when
the launcher is run. The included request file is an editable example, not a captured user request. The package builder never copies an input directory or a
build directory into the distribution. The launcher must select the adjacent `bin/` tools using its bundled-tool
resolver; packaging does not resolve native tools from `PATH`.

## Output safety and rebuilds

The script **never removes an existing output directory**. To rebuild, select a
new output directory or move the old distribution yourself. Existing build
artifacts may be reused. A build directory within the public checkout must live
under `ContentFactory/build/`; it may not contain a source checkout or overlap
the output directory. The GACL checkout cannot be used as a build directory.

If a command fails, the build reports failure and does not publish an incomplete
package. Only the builder's private temporary staging directory is cleaned up.
If another process creates the destination during the build, its contents are
preserved and publication is refused.

## Run the packaged factory

```powershell
.\ContentFactory.exe --config C:\requests\content.json
```

The launcher finds `bin` next to its executable; users do not pass encoder paths. Relative input paths in a JSON request resolve from the request file's directory. Keep all processing settings in the JSON file; see the included README and example.

## Source-only verification

The packaging unit tests create fake outputs and mock subprocess calls. They do
not configure CMake, compile native code, execute native tools, install packages,
or run PyInstaller:

```powershell
python .\ContentFactory\build-package.py --help
python -m unittest discover -s .\ContentFactory\tests -p test_build_package.py
```

A passing mock suite verifies planning, command arguments, output safety, and
assembly logic. It is **not** proof that an MSVC build or PyInstaller distribution
has succeeded. An approved real build and a separate-machine smoke test are
still required before claiming portability has been validated.

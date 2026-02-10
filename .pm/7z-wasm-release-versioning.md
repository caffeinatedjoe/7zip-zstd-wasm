# WASM Release Versioning Guide

## Scheme
Use a composite, explicit tag that encodes the upstream 7-Zip ZS version, the
embedded Zstandard version, and a local WASM build iteration:

`v7zip-<7zip_ver>-zstd-<zstd_ver>-wasm.<build>`

Example:
`v7zip-25.01-zstd-1.5.7-wasm.1`

## Where the versions come from
- `7zip_ver`: `C/7zVersion.h` (`MY_VER_MAJOR.MY_VER_MINOR`)
- `zstd_ver`: `C/zstd/zstd.h` (`ZSTD_VERSION_MAJOR.ZSTD_VERSION_MINOR.ZSTD_VERSION_RELEASE`)

## Build iteration (`wasm.<build>`)
- Start at `1` for the first WASM artifact cut for a given 7zip/zstd base.
- Increment when you rebuild the WASM output for the same base versions.

## Folder naming
Place release artifacts under:

`releases/<version_tag>/`

## Required metadata
Each release folder must include a `build_info.json` file with:
- release tag
- 7-Zip and Zstandard versions + commit
- Emscripten version
- build flags
- build date (YYYY-MM-DD)

Include a snapshot of the build script as `build.sh` in the release folder
so the exact command line is preserved if `wasm/build.sh` changes later.

Current base versions in this repo (from source headers):
- 7-Zip ZS: `25.01`
- Zstandard: `1.5.7`

So the current folder name should be:
`releases/v7zip-25.01-zstd-1.5.7-wasm.1/`

## Renaming the current release folder
If files are not locked by a running server:

```powershell
Move-Item releases\version releases\v7zip-25.01-zstd-1.5.7-wasm.1
```

If you get an access denied error, stop any process serving the files (e.g. VSCode Live Server)
and retry the command.

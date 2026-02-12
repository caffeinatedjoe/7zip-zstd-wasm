# Building in Git Bash

Open Git Bash in the repository root (`/c/github/7zip-zstd-wasm`) and run:

```bash
./wasm/run-build.sh
```

That script now does the following automatically:

1. Uses the repo-local `emsdk`.
2. Installs + activates `latest` only if the toolchain is not present.
3. Sources `emsdk_env.sh`.
4. Runs `./wasm/build.sh`.

## Why this fixes the previous failure

Your previous environment had emsdk installed, but Git Bash still could not resolve bare `emcc`.
`wasm/build.sh` now falls back to `python emcc.py` (using `EMSDK_PYTHON`) and then `emcc.bat` via `cmd.exe`, so it no longer depends on `emcc` command lookup.

## One-time optional setup

If you still want permanent global activation for non-repo builds:

```bash
./emsdk/emsdk activate latest --permanent
```

This is optional for this repo because `./wasm/run-build.sh` is self-contained.

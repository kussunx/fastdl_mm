# FastDL Metamod

A standalone FastDL HTTP server for GoldSrc HLDS. Runs as a Metamod plugin and does not require AMX Mod X.

## Build

HLDS is 32-bit, so the plugin must be built for 32-bit.

Requires CMake and an SDK directory containing `cssdk/` and `metamod/`:

- `cssdk/` — headers from [ReGameDLL_CS](https://github.com/rehlds/ReGameDLL_CS)
  (`regamedll/`, which provides `common/`, `dlls/`, `engine/` and `public/`)
- `metamod/` — plugin headers from [Metamod-R](https://github.com/rehlds/Metamod-R)

Point `FASTDL_MM_SDK_ROOT` at the directory holding both.

### Windows

```powershell
cmake -S . -B build -A Win32 `
  -DFASTDL_MM_SDK_ROOT=C:\path\to\sdk
cmake --build build --config Release
```

Output:

```text
build/Release/fastdl_mm.dll
```

### Linux

Requires `g++-multilib` and `libc6-dev-i386`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DFASTDL_MM_SDK_ROOT=/path/to/sdk
cmake --build build -j4
```

Output:

```text
build/fastdl_mm_i386.so
```

## Install

Copy the plugin and configuration file:

```text
cstrike/addons/fastdl/dlls/fastdl_mm.dll
cstrike/addons/fastdl/dlls/fastdl_mm.cfg
```

Add the appropriate line to `cstrike/addons/metamod/plugins.ini`:

```text
win32 addons/fastdl/dlls/fastdl_mm.dll
linux addons/fastdl/dlls/fastdl_mm_i386.so
```

Configure the client download URL:

```text
sv_downloadurl "http://203.0.113.10:27015"
```

By default, `fastdl_port "0"` uses the same port number as HLDS. The game uses UDP and FastDL uses TCP, so they do not conflict.

The default root is the `cstrike` directory. URLs must therefore be mod-relative:

```text
/maps/example.bsp
```

## Main settings

```text
fastdl_enabled           "1"
fastdl_bind              "0.0.0.0"
fastdl_port              "0"
fastdl_root              "cstrike"
fastdl_serve_dirs        "sprites,sound,sounds,overviews,models,maps,gfx"
fastdl_serve_types       "bsp,nav,res,wad,mdl,spr,wav,mp3,bmp,tga,txt,htm,html,gz,bz2"
fastdl_max_file_mb       "250"
fastdl_threads           "4"
fastdl_log               "logs/fastdl/fastdl.log"
fastdl_log_age           "7"
```

Changes to `fastdl_mm.cfg` persist after restart. Use `fastdl_restart` to apply changed cvars without restarting HLDS.

## Commands

```text
fastdl_status
fastdl_restart
fastdl_unblock <ip>
```

## Notes

* Only `GET` and `HEAD` requests from the Steam downloader are served.
* Directory traversal and files outside the configured root are blocked.
* Files must match both `fastdl_serve_dirs` and `fastdl_serve_types`.
* The plugin cannot be unloaded while HLDS is running.
* Stop HLDS before replacing the plugin binary.

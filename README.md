# FastDL Metamod

FastDL Metamod is an HTTP file server embedded in a GoldSrc game server through
Metamod. This fork is maintained by Kussun for the ZPR Counter-Strike 1.6
framework and targets ReHLDS, ReGameDLL-CS, Metamod-r, and AMX Mod X 1.10.

The project originated at [bucketss/fastdl_mm](https://github.com/bucketss/fastdl_mm).
Its authorship and history remain credited upstream; fork changes are recorded by
normal Git authorship.

## Design and compatibility

The plugin runs libmicrohttpd workers inside the 32-bit ReHLDS process. File I/O,
bandwidth waits, logging, and compression work stay off the game thread. Defaults
therefore favor bounded memory and predictable server frame time over maximum HTTP
benchmark throughput:

- one HTTP worker and at most 64 connections;
- 64 KiB response buffers rather than libmicrohttpd's 1 MiB fd-response buffer;
- streamed 64-bit file reads with no whole-file allocation;
- a single low-priority compression worker and a size-bounded disk cache;
- lock-free transfer counters and bounded rolling throughput buckets.

ReHLDS is the primary runtime target. The plugin uses standard GoldSrc and
Metamod interfaces, so it has no mandatory ReHLDS or ReGameDLL runtime API
dependency.

## Build

HLDS and ReHLDS are 32-bit. A 64-bit plugin cannot load.

The SDK root must contain `cssdk/` and `metamod/`. `cssdk/` may be the
`regamedll/` directory from
[ReGameDLL-CS](https://github.com/rehlds/ReGameDLL_CS) or the corresponding
directories from the [Half-Life SDK](https://github.com/ValveSoftware/halflife).
Current [Metamod-r](https://github.com/rehlds/Metamod-R) headers are supported.

Windows x86:

```powershell
cmake -S . -B build -A Win32 `
  -DFASTDL_MM_SDK_ROOT=C:\path\to\sdk
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The plugin is written to `build/Release/fastdl_mm.dll`.

Linux i386 requires `g++-multilib` and `libc6-dev-i386`:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DFASTDL_MM_SDK_ROOT=/path/to/sdk
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

The plugin is written to `build/fastdl_mm_i386.so`.

## Install on ReHLDS

Copy the binary and configuration beside one another:

```text
cstrike/addons/fastdl/fastdl_mm.dll
cstrike/addons/fastdl/fastdl_mm.cfg
```

Add the platform entry to `cstrike/addons/metamod/plugins.ini`:

```text
win32 addons/fastdl/fastdl_mm.dll
linux addons/fastdl/fastdl_mm_i386.so
```

Open the configured port for TCP. GoldSrc game traffic uses UDP; FastDL uses
TCP. Both protocols may use the same numeric port without conflict. With
`fastdl_port "0"`, the plugin follows the game port number, commonly TCP 27015
beside UDP 27015. This exact arrangement has been validated with ReHLDS on UDP
27015 and FastDL on TCP 27015. Windows Firewall must allow TCP 27015, and an
Internet-facing server must forward TCP 27015 through its router/NAT separately
from the existing UDP rule.

`fastdl_bind "0.0.0.0"` means listen on every local IPv4 interface. Do not put a
public Internet address in `fastdl_bind` unless that address is actually assigned
to a local interface. The externally reachable address belongs in
`sv_downloadurl`, or in `fastdl_public_url` when automatic mode is enabled.

Set the public URL manually:

```text
sv_downloadurl "http://203.0.113.10:27015/"
```

The trailing slash is recommended. The plugin tolerates a resulting repeated
slash in asset paths.

Automatic mode is optional and disabled by default:

```text
fastdl_auto_downloadurl "1"
fastdl_public_url "http://203.0.113.10:27015/"
```

The URL is assigned only after the listener starts successfully. The previous
`sv_downloadurl` is restored on disable, failed restart, or plugin shutdown.
No external public-IP service is queried and NAT routing is never guessed.

For same-network testing, use the server machine's LAN address, for example:

```text
sv_downloadurl "http://192.168.1.42:27015/"
```

In the validated CS 1.6 Steam test, the client displayed a loopback URL such as
`http://127.0.0.1:27015/` as its primary download location but did not reliably
issue FastDL requests. The LAN address worked. This is an observed GoldSrc client
behavior, not a claim that loopback fails in every build or environment. External
players should use a public IP or, preferably, a hostname.

## Security model

Every request must pass canonical path containment, directory, extension, and
size checks. The opened descriptor is checked again against its canonical path,
closing the path-resolution/open race and rejecting symlink or junction escapes.
Traversal components, backslashes, colons, decoded question/fragment markers,
control characters, and excessive path depth or component length are rejected.
Request targets and relevant HTTP headers are length-bounded before parsing.

Subdirectory files must match both `fastdl_serve_dirs` and
`fastdl_serve_types`. Files directly below `fastdl_root` must match both
`fastdl_serve_root_types` and `fastdl_serve_types`.

The default root allowlist is deliberately narrow:

```text
fastdl_serve_root_types "wad"
```

This permits GoldSrc resources such as `cstrike/custom.wad` without exposing
`server.cfg`, `liblist.gam`, `motd.txt`, or unrelated files. Use an empty value
to restore deny-all behavior for root-level files.

`fastdl_steam_only "1"` filters on the Steam downloader User-Agent. This is a
traffic filter, not authentication or a security boundary. Disabling it permits
ordinary HTTP clients, but all path, type, size, connection, request, denial,
and bandwidth controls still apply.

Per-IP request history and bandwidth bucket maps are bounded. When those bounds
are exhausted, request limiting fails closed and excess bandwidth identities
share a restrictive overflow bucket instead of allocating unbounded state or
bypassing the configured rate.

## HTTP behavior

The server supports `GET`, `HEAD`, and one byte range:

```text
Range: bytes=START-END
Range: bytes=START-
Range: bytes=-SUFFIX
```

Valid ranges return `206 Partial Content` with `Accept-Ranges`, `Content-Range`,
and the exact `Content-Length`. Invalid, multiple, overflowing, and
unsatisfiable ranges return `416` with `Content-Range: bytes */TOTAL`.

File responses include metadata-based `ETag` and `Last-Modified` validators.
`If-None-Match` and `If-Modified-Since` can return `304 Not Modified`; no large
file is hashed on a request.

## Transfer metrics and bandwidth limits

Metrics count payload bytes supplied to libmicrohttpd. This closely
approximates network payload without touching socket internals and avoids
claiming the full asset size after an early disconnect. libmicrohttpd may have
one 64 KiB block buffered at abort, so abort totals can exceed bytes physically
received by at most approximately one response block.

`fastdl_stats` reports requests, active transfers, completed, aborted, timed
out and failed transfers, cumulative payload, recent ten-second throughput,
and gzip cache activity. The recent value is zero after the rolling window has
been idle long enough; it does not retain an old transfer rate indefinitely.

Bandwidth limits use decimal megabits per second; zero is unlimited:

```text
fastdl_bandwidth_max_mbps "80"
fastdl_bandwidth_ip_mbps  "12"
```

The per-IP controller is shared by all simultaneous connections from that IP,
so opening more asset requests does not multiply its allowance. Token
reservations are atomic; cancellation-aware condition-variable waits neither
spin nor sleep on the game thread. Small time quanta improve fairness and cap
shutdown latency.

Leave both rates at zero until the host's sustained upload capacity is known.
For a game server, a conservative global starting point is 70-80% of measured
upload capacity, leaving headroom for ReHLDS UDP traffic and operating-system
overhead. Set the per-IP value to a fraction of the global budget appropriate to
the expected player count (often 10-25%); it should not exceed the global value.
Measure during real downloads and reduce the limits if game latency or loss rises.

`fastdl_threads "1"` remains the compatible default. Local regression measurements
with four concurrent 128 KiB transfers found no material 1-versus-2 worker
difference under a 4 Mbps global or per-IP limit; both delivered about 4.5 Mbps
including the token bucket's bounded initial burst. Two workers improved an
unlimited loopback micro-test, but that result is disk- and scheduler-dependent.
Use two workers only when monitoring shows that concurrent slow downloads or
limiter waits serialize clients; each extra worker adds stack and connection
state inside the 32-bit ReHLDS process.

## Gzip response cache

HTTP gzip was selected because the current CS 1.6 downloader has been observed
sending `Accept-Encoding: gzip,identity,*;q=0`; see the captured headers in
[ValveSoftware/halflife issue 2605](https://github.com/ValveSoftware/halflife/issues/2605).
The official [Steam Fast Download documentation](https://help.steampowered.com/en/faqs/view/62E9-AEDF-6B35-B865)
documents exact logical asset URLs. The implementation therefore keeps the URL
unchanged and applies `Content-Encoding: gzip` only after explicit negotiation.

Automatic `.bz2` alternate requests were rejected because verified GoldSrc
client behavior was not found; that convention is associated with Source-engine
FastDL. Real-time per-client compression was rejected because it would create
unbounded CPU contention inside ReHLDS. Existing `.gz` and `.bz2` files remain
ordinary allowlisted assets when requested by their actual names.

Compression remains off in the compiled compatibility defaults. It is recommended
for the validated ZPR deployment:

```text
fastdl_gzip              "1"
fastdl_gzip_cache        "1"
fastdl_gzip_cache_path   "fastdl_cache"
fastdl_gzip_cache_max_mb "256"
```

The real CS 1.6 Steam/ReHLDS validation confirmed cold source responses, warm
gzip cache hits, `Content-Encoding: gzip`, Range fallback to identity, and
byte-identical SHA-256 after decompression. The observed session ended with 70
requests, 70 completed transfers, 20 gzip hits, 50 misses, and zero aborted,
timed-out, or failed transfers.

The cache path is resolved from the HLDS base directory and must be outside
`fastdl_root`. On a miss, the request immediately receives the original file and
one background worker prepares a gzip representation using fast compression.
Concurrent misses for the same source identity collapse into one job. The cache
key includes canonical path, size, high-resolution modification time, and file
identity; source changes therefore invalidate the old representation. Cache
writes are temporary and renamed only after completion. Old artifacts are
pruned with a bounded oldest-entry set and 10% size hysteresis, so a full cache
walk is not performed after every generated artifact. Stale `.skip` markers,
temporary files, orphaned metadata, and representations for changed, deleted, or
no-longer-served sources are cleaned at startup and explicit cache builds.

Use `fastdl_cache_build` to queue a background scan of currently permitted
assets. It walks only configured served directories and allowed root-level files,
not unrelated trees such as `addons`, `logs`, or `downloads`; every candidate
still passes `PathResolver` and secure descriptor checks. The command does not
scan or compress from `StartFrame`.

Only BSP, WAD, MDL, SPR, WAV, BMP, TGA, TXT, and RES files of at least 1 KiB are
considered. Results saving less than 5% are discarded. MP3, gzip, and bzip2 data
are skipped. Expected savings depend on content: text often saves 60-90%,
uncompressed WAV/BMP/TGA 40-90%, and BSP/WAD/MDL/SPR commonly 15-60%; already
compressed or media-heavy assets may save little. These are estimates, not a
substitute for measuring the ZPR asset set.

Range requests always use the uncompressed representation because byte offsets
refer to that representation. Gzip and identity representations have distinct
ETags and responses include `Vary: Accept-Encoding`.

## Configuration

The shipped `config/fastdl_mm.cfg` lists every setting and its compiled default.
Important safe defaults include:

```text
fastdl_enabled               "1"
fastdl_bind                  "0.0.0.0"
fastdl_port                  "0"
fastdl_root                  "cstrike"
fastdl_serve_root_types      "wad"
fastdl_max_file_mb           "250"
fastdl_threads               "1"
fastdl_max_connections       "64"
fastdl_max_connections_ip    "32"
fastdl_connection_timeout    "30"
fastdl_bandwidth_max_mbps    "0"
fastdl_bandwidth_ip_mbps     "0"
fastdl_steam_only            "1"
fastdl_gzip                  "0"
fastdl_auto_downloadurl      "0"
```

For the real ReHLDS/ZPR-tested baseline, start from
[`config/fastdl_mm.zpr.example.cfg`](config/fastdl_mm.zpr.example.cfg). It uses
TCP 27015 explicitly and enables gzip while leaving bandwidth unlimited until
the host uplink is measured. The example does not change the compiled defaults.

Older configuration files remain valid. Missing new settings retain compiled
defaults, and malformed or unknown assignments are reported and skipped rather
than making the entire configuration fatal. Run `fastdl_restart` after changing
CVARs.

## Commands

```text
fastdl_status
fastdl_stats
fastdl_restart
fastdl_cache_build
fastdl_unblock <ip>
```

`fastdl_status` shows listener, root and allowlists, worker/connection controls,
bandwidth, Steam filtering, compression, automatic URL, and logging state.
`fastdl_stats` contains the changing counters and cache telemetry so routine
status checks remain concise.

## Troubleshooting

- `start failed: libmicrohttpd could not bind or start`: confirm the TCP port is
  free and permitted by the host firewall. A working UDP game port does not
  prove the same TCP port is open.
- Clients pause before fallback: remove or correct a stale `sv_downloadurl`.
- `403`: inspect `fastdl_serve_dirs`, `fastdl_serve_types`, root types, Steam-only
  mode, request limits, and denial blocks with `fastdl_status`.
- Root WAD fails: confirm the WAD is under `fastdl_root` itself and `wad` appears
  in both extension lists.
- Gzip stays on cache miss: run `fastdl_cache_build`, inspect `fastdl_stats`, and
  verify the server account can write `fastdl_gzip_cache_path`.
- Asset has spaces: both encoded and Steam's legacy unencoded request targets
  are supported by the vendored request-line parser patch.

The listener is IPv4 and plain HTTP. TLS termination, multipart byte ranges, and
automatic public-address discovery are intentionally outside this embedded
server's scope.

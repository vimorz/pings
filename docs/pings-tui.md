# Pings TUI

Pings TUI is a terminal utility for monitoring several hosts with ICMP.
Use it to spot intermittent packet loss and compare response times across
hosts. A missing ICMP reply does not prove that a host or service is down.

## Quick start

With a Windows release archive, extract it and open an elevated PowerShell
in that directory:

```powershell
.\pings_tui.exe .\config.yml
```

On Linux, real checks require permission to open raw sockets (for example,
`CAP_NET_RAW`). Launch without arguments for an offline demo. In that mode
no packets are sent and no elevated privileges are needed.

## Configure your hosts

The path argument is relative to the current working directory. Use quotes
around paths containing spaces. Here is a starting configuration for testing
with local IPv4 and IPv6 loopback:

```yaml
defaults:
  reply_timeout_ms: 1000
  resolve_timeout_ms: 3000
  payload_size: 32
  address_family: ipv4

hosts:
  - name: Local IPv4
    host: 127.0.0.1
    enabled: true

  - name: Local IPv6
    host: "::1"
    enabled: true
    ping:
      address_family: ipv6
      reply_timeout_ms: 500
```

Set per-host overrides under `ping` using the same keys as in `defaults`.
Disabled hosts remain visible but are skipped. Restart the app to load
changes to the file. Use `any`, `ipv4` or `ipv6` to choose an address family.

## Controls

| Key | Action |
| --- | --- |
| Up / Down | Select a host and see its latest outcome and effective reply timeout |
| `m` | Switch between a single check and continuous monitoring while idle |
| `s` | Start the selected mode |
| `c` | Stop continuous monitoring; an active pass finishes before monitoring stops |
| `q` | Exit; an active pass finishes first |

Continuous mode runs one pass at a time, with a short pause between passes.
Progress counts completed attempts, including timeouts and errors. Once
the pass finishes, a result is added to each checked host's history.

Each host retains up to 60 results in memory, newest on the right. A narrow
terminal can show fewer cells. Green is a reply, red is a reply timeout,
yellow is another error, and gray means no measurement. The details panel
shows the selected host's latest status and, when available, a diagnostic
code. History is cleared on exit. The app does not yet have a database or
immediate cancellation of an active pass. The interface labels are in Russian.

## Build from source on Windows

From the repository root, with the tools and profiles listed in the
[project README](../README.md) installed:

```powershell
conan install . `
    --output-folder=out/examples `
    --build=missing `
    -pr:h=profiles/windows-msvc `
    -pr:b=default `
    -s:h build_type=Release `
    -o "&:with_config=True" `
    -o "&:build_apps=True"

cmake -S . -B out/examples/build `
    -G "Visual Studio 18 2026" -A x64 `
    -DCMAKE_TOOLCHAIN_FILE="$PWD/out/examples/build/generators/conan_toolchain.cmake" `
    -DPINGS_BUILD_CONFIG=ON `
    -DPINGS_BUILD_APPS=ON

cmake --build out/examples/build --config Release --target pings_tui
.\out\examples\build\Release\pings_tui.exe .\app\config.yml
```

If you don't have a Conan build profile yet, run `conan profile detect`
first. Use Release for the fullscreen UI: diagnostic logging from a Debug
build can interfere with terminal output.

## Build from source on Linux

From the repository root, use CMake 3.24+, Ninja, Conan 2 and GCC 14
(`gcc-14` and `g++-14` on PATH). These commands use the repository's
Linux x86-64 profile and follow the Linux CI build layout. If Conan has no
default build profile yet, run `conan profile detect` once.

```bash
export CC=gcc-14 CXX=g++-14

conan install . \
    --output-folder=out/linux-tui \
    --build=missing \
    -pr:h=profiles/linux-gcc \
    -pr:b=default \
    -s:h build_type=Release \
    -c:h tools.cmake.cmaketoolchain:generator=Ninja \
    -o '&:with_config=True' \
    -o '&:build_apps=True'

cmake -S . -B out/linux-tui/build/Release \
    -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$PWD/out/linux-tui/build/Release/generators/conan_toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DPINGS_BUILD_CONFIG=ON \
    -DPINGS_BUILD_APPS=ON

cmake --build out/linux-tui/build/Release --target pings_tui
```

Run the offline demo as a regular user:

```bash
./out/linux-tui/build/Release/pings_tui
```

For a real check, run with raw-socket permissions. On a local test machine,
one option is to grant the built binary only the capability it needs
(`setcap` must be installed):

```bash
sudo setcap cap_net_raw=ep ./out/linux-tui/build/Release/pings_tui
./out/linux-tui/build/Release/pings_tui ./app/config.yml
```

If a rebuild replaces the executable, check its capability with `getcap`
and reapply it if needed.

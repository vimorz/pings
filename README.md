# Pings

An asynchronous ICMP ping library for C++20, built with Boost.Asio.

The project is under active development. The public API may change before
the first stable release.

## Features

- IPv4 and IPv6 Echo requests.
- Single, batch and repeated ping operations.
- DNS resolution with separate resolve and reply timeouts.
- Explicit address-family selection: Any, IPv4 or IPv6.
- Cancellation handles and explicit shutdown.
- Repeat statistics: attempts, sent requests, received replies, loss and RTT.
- Optional YAML configuration with global defaults and per-host overrides.

## Modules

| CMake target | Purpose |
| --- | --- |
| `pings::core` | Ping API and ICMP transport |
| `pings::config` | YAML loading, validation and target mapping |

The core library does not depend on YAML or the terminal UI.
Example applications currently require the configuration module.

## Minimal example

```cpp
#include <iostream>

#include <pings/async_pinger.hpp>

int main()
{
    auto pinger = pings::AsyncPinger::create();

    pings::PingOptions options{};
    options.addressFamily = pings::AddressFamily::IPv4;

    [[maybe_unused]] auto handle = pinger->pingAsync(
        "127.0.0.1",
        options,
        [](const pings::PingResult &result)
        {
            std::cout << result.host << ": "
                      << pings::toString(result.status);

            if (result.latency)
            {
                std::cout << ", "
                          << result.latency->count() << " ms";
            }

            std::cout << '\n';
        });

    pinger->run();
}
```

`run()` processes asynchronous operations and blocks the calling thread.
Initiate operations on the same thread; concurrent use of an instance is
not currently supported.

The example uses raw sockets. Running ping operations requires appropriate
OS privileges: an elevated process on Windows, or root/CAP_NET_RAW on Linux.
The unit tests do not require raw-socket privileges.

IPv4 and IPv6 sockets are opened on demand when sending a request.
Creating an AsyncPinger does not open raw sockets. Socket open or bind
failures are reported asynchronously as SendError, with the system
error available in PingResult::error.

## Build requirements

- A C++20 compiler.
- CMake 3.24 or newer.
- Conan 2; the Windows build workflow has been verified with Conan 2.33.0.
- Python to run Conan.

Repository profiles currently select:

- Windows: MSVC 195, Visual Studio 18 2026, x86-64.
- Linux: GCC 14, libstdc++11 ABI, x86-64.

These profiles describe specific toolchains. Adjust them when using a
different compiler installation.

IPv4/IPv6 loopback and mixed batches have been manually tested on Windows
and Linux. External IPv6 success has not yet been verified in the current
test environment.

## Build the core on Windows

Run from the repository root with Conan available in the active environment.

```powershell
conan profile detect

conan install . `
    --output-folder=out/core `
    --build=missing `
    -pr:h=profiles/windows-msvc `
    -pr:b=default `
    -s:h build_type=Release `
    -o "&:with_config=True" `
    -o "&:build_apps=True"

cmake -S . -B out/core/build `
    -G "Visual Studio 18 2026" -A x64 `
    -DCMAKE_TOOLCHAIN_FILE="$PWD/out/core/build/generators/conan_toolchain.cmake" `
    -DPINGS_BUILD_CONFIG=OFF `
    -DPINGS_BUILD_APPS=OFF `
    -DBUILD_TESTING=ON

cmake --build out/core/build --config Release
ctest --test-dir out/core/build -C Release --output-on-failure --no-tests=error
```

If a Conan default profile already exists, reuse it instead of detecting
another one.

## Build options

| Conan option | CMake option | Default |
| --- | --- | --- |
| `with_config` | `PINGS_BUILD_CONFIG` | Enabled |
| `build_apps` | `PINGS_BUILD_APPS` | Enabled |

Conan options select dependencies. CMake options select build targets.
Keep them consistent when invoking CMake directly.

The core-only build registers eleven tests. Enabling configuration adds
two more tests.

## Address-family policy

- `Any`: use the first address returned by the system resolver.
- `IPv4`: accept IPv4 literals and resolve IPv4 addresses only.
- `IPv6`: accept IPv6 literals and resolve IPv6 addresses only.

A literal that conflicts with the selected family returns `InvalidOptions`.
There is no automatic retry with another address or family.

A DNS AAAA record does not guarantee that the local machine has working
IPv6 connectivity.

## Continuous integration

GitHub Actions builds and tests the project on Windows and Linux
with two configurations:

- Core only: YAML configuration and applications disabled, eleven tests.
- Full build: YAML configuration and applications enabled, thirteen tests.

CI runs deterministic unit tests without raw sockets or network access.
Conan dependency packages are cached between runs.

## Current limitations

- No stable installation/export package is provided yet.
- Successful stop is terminal for an instance; create another instance
  for new work after stopping.
- Repeat cancellation currently excludes the active unfinished attempt
  from the final summary.
- Repeat statistics cover recorded results, not all traffic over the
  lifetime of a series.
- macOS support has not been verified.
- CI currently covers Release builds only.
- The terminal UI is an experimental prototype and is not connected to ping operations yet.

## Examples

The `examples/` directory contains single, batch, repeated and
YAML-configured usage examples.

The console application reads `config.yml` from its current working
directory. Its build copies `app/config.yml` beside the executable.

`replyTimeout` is measured from the start of the asynchronous send.
A matching reply processed at or after that deadline is reported as
`Timeout`. The deadline is checked before converting the RTT to whole
milliseconds.

## License

Licensed under the [MIT License](LICENSE).

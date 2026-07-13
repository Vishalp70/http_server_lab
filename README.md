# Linux HTTP/1.1 Server Lab

A C++17 learning and measurement project that implements one small HTTP/1.1
server three ways (thread-per-connection, fixed thread pool, `epoll` event loop)
so the connection-handling models can be compared on correctness and measured
performance.

**Status: minimal HTTP/1.1 server with thread-per-connection, fixed thread-pool,
and level-triggered epoll modes.** `httpd` serves a small documented GET subset
over IPv4. This project is a study project and is not production-ready.

## Requirements

- Linux (Ubuntu 22.04/24.04 or WSL2 with one of them). CMake refuses to
  configure on other operating systems.
- A C++17 compiler (GCC or Clang), CMake, Git, `make`.
- Network access during the first configure: CMake downloads GoogleTest
  v1.15.2 from GitHub. For offline builds see "Offline GoogleTest" below.

Install on Ubuntu / WSL2:

```bash
sudo apt update
sudo apt install -y build-essential cmake git
# optional: sudo apt install -y ninja-build clang clang-format
```

CMake 3.16 or newer is declared as the minimum; the presets in
`CMakePresets.json` need CMake 3.21 or newer. The current milestone was built
and tested under Ubuntu 22.04 WSL2 with GCC 11.4.0 and CMake 3.31.9.

## Build, test, run (using presets)

```bash
git clone <your-repository-url> linux-http-server-lab
cd linux-http-server-lab

cmake --preset release            # configure   -> build/release
cmake --build --preset release -j # compile
ctest --preset release            # run all tests

./build/release/src/httpd --port 8080
```

| Preset       | Build type | Notes                                          |
|--------------|------------|------------------------------------------------|
| `debug`      | Debug      | Assertions on, `-g`                            |
| `release`    | Release    | `-O2 -DNDEBUG`; use this for benchmarks         |
| `asan-ubsan` | Debug      | AddressSanitizer + UndefinedBehaviorSanitizer  |
| `tsan`       | Debug      | ThreadSanitizer                                |

All presets enable `-Werror` (`SRV_WARNINGS_AS_ERRORS=ON`). Build trees go to
`build/<preset>/`.

Build trees contain absolute paths in `CMakeCache.txt`; do not copy them along
with the source when moving the project to another directory or machine. If a
copied project reports that its cache refers to a different source directory,
remove the copied `build/` directory and configure again from the new project
root:

```bash
rm -rf build
cmake --preset release
cmake --build --preset release -j
ctest --preset release
```

## Build without presets

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cd build && ctest --output-on-failure && cd ..
./build/src/httpd --help
```

Add `-G Ninja` to the first command if Ninja is installed. Without
`-DCMAKE_BUILD_TYPE`, Release is used.

## Running `httpd`

```
Usage: httpd [OPTIONS]

  -p, --port PORT   TCP port to listen on, 0-65535 (0 = ephemeral). Default: 8080
      --bind ADDRESS IPv4 address to bind (default: 127.0.0.1).
      --mode MODE    thread-per-connection, thread-pool, or epoll.
      --workers N    Pool workers, 1-64 (default: 4).
  -h, --help        Show this help and exit.
  -V, --version     Show version and exit.
```

`--port 9000` and `--port=9000` are both accepted. The port must be plain
decimal digits in the range 0 to 65535. Signs, spaces, hex, empty values and
out-of-range numbers are rejected. `--bind ADDRESS` and `--bind=ADDRESS` accept
an IPv4 address, which is validated when the listener is created. Unknown
options, positional arguments, and repeated `--port` or `--bind` options are
errors.

| Exit status | Meaning                                             |
|-------------|-----------------------------------------------------|
| 0           | Graceful shutdown, `--help`, or `--version`          |
| 2           | Invalid command line (message printed to stderr)    |
| 1           | Server setup or runtime error                       |

```bash
./build/release/src/httpd --version          # httpd 0.1.0
./build/release/src/httpd --mode thread-per-connection --bind 127.0.0.1 --port 9090
./build/release/src/httpd --mode thread-pool --workers 4 --port 9091
./build/release/src/httpd --mode epoll --port 9092
./build/release/src/httpd --port 70000; echo "exit=$?"   # error, exit=2
```

The server binds to loopback by default. `--bind ADDRESS` accepts an IPv4
address; `--port 0` requests an ephemeral port. On startup it prints the bound
endpoint as `READY address:port`. The server supports HTTP/1.1 request lines,
headers, and GET requests for `/`, `/health`, and `/hello`. It returns 400 for
malformed requests, 404 for unknown paths, 405 for other methods, and 431 when
the 8 KiB request limit is exceeded. Each response includes `Content-Length`
and `Connection: close`; persistent connections, request bodies, chunked
encoding, TLS, and full HTTP compliance are out of scope. Choose
`--mode thread-per-connection` (the default), `--mode thread-pool --workers N`
(1 to 64 workers, default 4), or `--mode epoll`. Thread-per-connection allows
up to 128 workers. The pool uses a bounded queue of 16 pending clients; when
full, new clients receive 503. Pool workers process whole connections, so a
slow client occupies one worker. Epoll uses level-triggered readiness, stores
bounded input/output state per client, and closes new clients once its
1024-connection limit is reached. `SIGINT` and `SIGTERM` request graceful
shutdown.

## Tests

`ctest` runs unit, smoke, and integration tests (41 tests in this milestone).
Coverage includes CLI and HTTP parsing, application routes, all three modes,
malformed and oversized requests, concurrent and repeated clients, pool queue
overload, partial requests, disconnects, and shutdown.

## Sanitizers

```bash
cmake --preset asan-ubsan && cmake --build --preset asan-ubsan -j && ctest --preset asan-ubsan
cmake --preset tsan       && cmake --build --preset tsan       -j && ctest --preset tsan
```

Without presets: `-DSRV_SANITIZER=asan-ubsan` or `-DSRV_SANITIZER=tsan`.
ASan and TSan cannot be combined. Sanitizer builds are for correctness
testing only and must never be used for benchmarks. Release and ASan/UBSan
each passed all 41 tests in the latest run. The TSan production build
compiled with tests disabled, but even running `httpd --help` aborted before
application startup with `unexpected memory mapping` on this WSL2 kernel. No
TSan runtime or test result is available for this machine.

## CMake options

| Option                    | Default   | Meaning                                              |
|---------------------------|-----------|------------------------------------------------------|
| `CMAKE_BUILD_TYPE`        | `Release` | `Debug` or `Release`                                 |
| `SRV_BUILD_TESTS`         | `ON`      | Build unit, smoke, and integration tests             |
| `SRV_WARNINGS_AS_ERRORS`  | `OFF`     | Add `-Werror` to the project's own targets           |
| `SRV_SANITIZER`           | `none`    | `none`, `asan-ubsan` or `tsan`                       |
| `SRV_USE_SYSTEM_GTEST`    | `OFF`     | Use an installed GoogleTest instead of fetching one  |

### Offline GoogleTest

```bash
sudo apt install -y libgtest-dev
cmake -S . -B build -DSRV_USE_SYSTEM_GTEST=ON
```

Or skip tests entirely with `-DSRV_BUILD_TESTS=OFF`. Do not combine the system
GoogleTest with `tsan`: the installed library is not TSan-instrumented, which
can produce false reports.

## Repository layout

```
CMakeLists.txt          project options, platform guard, subdirectories
CMakePresets.json       debug, release, asan-ubsan, tsan
cmake/                  ProjectOptions.cmake (warnings), Sanitizers.cmake
src/
  CMakeLists.txt        srv_cli, srv_http, srv_net, srv_epoll, and httpd
  app/                  cli.{hpp,cpp} (argument parsing), main.cpp
  common/               config.hpp, version.hpp.in
  http/                 http.{hpp,cpp} (bounded parser and shared routes)
  net/                  unique_fd.hpp, tcp_server.{hpp,cpp}, epoll_server.cpp
  server/               connection_pool.{hpp,cpp}
tests/
  CMakeLists.txt        GoogleTest setup, unit, smoke, and integration tests
  unit/                  cli_test.cpp, http_test.cpp
  integration/tcp_server_test.cpp
```

Platform note: command-line, HTTP, and configuration code is standard C++.
Linux networking APIs such as `accept4` and `epoll` are isolated under `src/net`.
CMake refuses to configure on non-Linux systems.

## Benchmarks and results

### Running the harness

Run the same warm-up and measured HTTP workload against each architecture:

```bash
cmake --preset release
cmake --build --preset release -j
python3 benchmark/run_benchmark.py \
  --concurrency 50 --warmup-seconds 2 --duration-seconds 5 --trials 3 \
  --output benchmark/results/my-run.json
```

The harness opens one TCP connection per `/health` request and records
successful requests per second, median/p95/p99 latency, failed requests and
HTTP status counts, sampled server CPU and RSS, request/response sizes,
concurrency, and machine/build metadata. CPU percentage is normalized to total
logical CPU capacity; CPU and RSS are sampled from `/proc` every 100 ms.
Latency percentiles include successful HTTP 200 responses only. The JSON file
contains every trial and its settings.

### Recorded run

This run was recorded on 2026-10-26 under Ubuntu 22.04 WSL2 on x86_64, kernel
`6.18.54.1-microsoft-standard-WSL2`, an Intel Core i7-13700HX (24 logical
CPUs; 7,798 MiB available to WSL), GCC 11.4.0, and CMake 3.31.9. It used the
Release preset, `/health`, 50 concurrent clients, a
2-second warm-up, and three 5-second trials. Each request was 47 bytes and each
response was 101 bytes including headers (3-byte body). Each result is the
median across the trials; CPU is median sampled host utilization and memory is
peak sampled server RSS.

| Mode | Successful req/s | Median ms | p95 ms | p99 ms | CPU % of host | Peak RSS MiB | Failed requests |
|---|---:|---:|---:|---:|---:|---:|---:|
| Thread per connection | 3,414.5 | 14.44 | 20.57 | 23.54 | 0.89 | 3.99 | 0 / 51,124 |
| Fixed thread pool (4 workers) | 3,207.4 | 14.81 | 20.20 | 22.83 | 0.42 | 3.62 | 1,954 / 50,199 |
| Level-triggered epoll | 3,405.1 | 14.45 | 20.69 | 23.65 | 0.44 | 3.57 | 0 / 48,941 |

All 1,954 thread-pool failures were HTTP 503 responses because the 16-client
pending queue filled at this concurrency, a 3.89% failure rate. Throughput
counts successful HTTP 200 responses; latency percentiles also exclude 503
responses. This is one WSL2 run, not a general performance claim. Results can change with host load,
WSL scheduling, worker count, and workload. Full trial data and metadata are in
[the recorded JSON result](benchmark/results/ubuntu22.04-wsl2-gcc11.4-2026-10-26.json).

### Resume bullets

- Implemented a C++17 HTTP server with thread-per-connection, bounded thread-pool, and level-triggered epoll modes using shared request handling and RAII socket ownership.
- Added a bounded HTTP/1.1 GET subset, parser and integration tests, and a benchmark harness reporting latency percentiles, throughput, CPU, memory, and overload responses.

# Linux HTTP/1.1 Server Lab

A C++17 learning and measurement project: one small HTTP/1.1 server implemented
three ways (thread-per-connection, fixed thread pool, `epoll` event loop) so the
connection-handling models can be compared on correctness and measured
performance.

**Status: Milestone 1 (build scaffold and command line only).**
The `httpd` executable currently parses and validates its command line and then
exits. It does not open a socket or serve HTTP yet. This project is a study
project and is not production-ready.

## Requirements

* Linux (Ubuntu 22.04/24.04 or WSL2 with one of them). CMake refuses to
configure on other operating systems.
* A C++17 compiler (GCC or Clang), CMake, Git, `make`.
* Network access during the first configure: CMake downloads GoogleTest
v1.15.2 from GitHub. For offline builds see "Offline GoogleTest" below.

Install on Ubuntu / WSL2:

```bash
sudo apt update
sudo apt install -y build-essential cmake git
# optional: sudo apt install -y ninja-build clang clang-format
```

CMake 3.16 or newer is declared as the minimum; the presets in
`CMakePresets.json` need CMake 3.21 or newer (Ubuntu 22.04 ships 3.22).
So far the project has only been built with GCC 13.3, Clang 18.1.3 and
CMake 3.28.3 on Ubuntu 24.04.

## Build, test, run (using presets)

```bash
git clone <your-repository-url> linux-http-server-lab
cd linux-http-server-lab

cmake --preset release            # configure   -> build/release
cmake --build --preset release -j # compile
ctest --preset release            # run all tests

./build/release/src/httpd --port 8080
```

|Preset|Build type|Notes|
|-|-|-|
|`debug`|Debug|Assertions on, `-g`|
|`release`|Release|`-O2 -DNDEBUG`; use this for benchmarks later|
|`asan-ubsan`|Debug|AddressSanitizer + UndefinedBehaviorSanitizer|
|`tsan`|Debug|ThreadSanitizer|

All presets enable `-Werror` (`SRV\_WARNINGS\_AS\_ERRORS=ON`). Build trees go to
`build/<preset>/`.

## Build without presets

```bash
cmake -S . -B build -DCMAKE\_BUILD\_TYPE=Release
cmake --build build -j
cd build \&\& ctest --output-on-failure \&\& cd ..
./build/src/httpd --help
```

Add `-G Ninja` to the first command if Ninja is installed. Without
`-DCMAKE\_BUILD\_TYPE`, Release is used.

## Running `httpd`

```
Usage: httpd \[OPTIONS]

  -p, --port PORT   TCP port to listen on, 0-65535 (0 = ephemeral). Default: 8080
  -h, --help        Show this help and exit.
  -V, --version     Show version and exit.
```

`--port 9000` and `--port=9000` are both accepted. The port must be plain
decimal digits in the range 0 to 65535. Signs, spaces, hex, empty values and
out-of-range numbers are rejected. Unknown options, positional arguments and a
repeated `--port` are errors too.

|Exit status|Meaning|
|-|-|
|0|Success, including `--help` and `--version`|
|2|Invalid command line (message printed to stderr)|

```bash
./build/release/src/httpd --version          # httpd 0.1.0
./build/release/src/httpd --port 9090        # configuration OK (port 9090)...
./build/release/src/httpd --port 70000; echo "exit=$?"   # error, exit=2
```

## Tests

`ctest` runs two groups:

* GoogleTest unit tests for the command-line parser (`tests/unit/`).
* Smoke tests that run the real `httpd` binary (`--help`, `--version`,
a valid port, an invalid port). These check the printed text; the exit
status 2 for invalid input is checked by the manual command above.

## Sanitizers

```bash
cmake --preset asan-ubsan \&\& cmake --build --preset asan-ubsan -j \&\& ctest --preset asan-ubsan
cmake --preset tsan       \&\& cmake --build --preset tsan       -j \&\& ctest --preset tsan
```

Without presets: `-DSRV\_SANITIZER=asan-ubsan` or `-DSRV\_SANITIZER=tsan`.
ASan and TSan cannot be combined. Sanitizer builds are for correctness
testing only and must never be used for benchmarks. If a TSan binary aborts at
startup with a memory-mapping error, that is a known incompatibility between
some kernels and the sanitizer runtime, not a project bug.

## CMake options

|Option|Default|Meaning|
|-|-|-|
|`CMAKE\_BUILD\_TYPE`|`Release`|`Debug` or `Release`|
|`SRV\_BUILD\_TESTS`|`ON`|Build unit and smoke tests (fetches GoogleTest)|
|`SRV\_WARNINGS\_AS\_ERRORS`|`OFF`|Add `-Werror` to the project's own targets|
|`SRV\_SANITIZER`|`none`|`none`, `asan-ubsan` or `tsan`|
|`SRV\_USE\_SYSTEM\_GTEST`|`OFF`|Use an installed GoogleTest instead of fetching one|

### Offline GoogleTest

```bash
sudo apt install -y libgtest-dev
cmake -S . -B build -DSRV\_USE\_SYSTEM\_GTEST=ON
```

Or skip tests entirely with `-DSRV\_BUILD\_TESTS=OFF`. Do not combine the system
GoogleTest with `tsan`: the installed library is not TSan-instrumented, which
can produce false reports.

## Repository layout

```
CMakeLists.txt          project options, platform guard, subdirectories
CMakePresets.json       debug, release, asan-ubsan, tsan
cmake/                  ProjectOptions.cmake (warnings), Sanitizers.cmake
src/
  CMakeLists.txt        srv\_cli library + httpd executable, version header
  app/                  cli.{hpp,cpp} (argument parsing), main.cpp
  common/               config.hpp, version.hpp.in
tests/
  CMakeLists.txt        GoogleTest setup, unit and smoke tests
  unit/cli\_test.cpp
```

Platform note: the code in `src/app` and `src/common` is portable standard
C++. Linux-specific code (`epoll`, `eventfd`, `accept4`) will be confined to
the `src/net` and `src/server` directories in later milestones, and CMake
already refuses to configure on non-Linux systems.

## Roadmap

Later milestones add the sans-I/O HTTP core, the three servers, the
conformance test suite, and the benchmark harness. Targets for a separate
client and for benchmarks are intentionally not created yet; they arrive with
the code they exercise.



•	Thread-per-connection is simplest and gives natural blocking code, but costs one stack and one scheduler entity per connection and hits OS limits before the others.

•	The fixed pool caps resource use but couples concurrency to worker count under keep-alive.

•	Single-threaded epoll avoids per-connection threads and locks, but one slow handler (for example a large /cpu request) blocks every connection on that loop. This is the reason /cpu exists and the reason E1 and E3 are listed.

•	The shared Session costs one small abstraction but removes the largest correctness risk (three divergent protocol implementations).

•	Level-triggered epoll costs some extra wakeups versus edge-triggered but is much harder to get wrong.




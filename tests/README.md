# Regression tests

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSOUNDUX_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

`config-regression` uses a separate configuration directory under the build directory.
It covers immediate hotkey/volume persistence, live sound/favorite indexes, older settings,
corrupt-config backups, failed writes, and default-device settings transitions.

`frontend-regression` requires Node.js and checks the packaged UI, including backend changes.
The compatibility patch is applied to copied assets, not to the upstream UI submodule.

`pipewire-regression` requires Python 3, PipeWire tools, WirePlumber 0.5+, and `dbus-run-session`.
It starts a private server with dummy microphones and a policy-only session manager: it does
not connect to the user's audio server or enumerate hardware. It checks repeated default-source
changes, restoration, external device selections, crash recovery, and shutdown cleanup.
When Xvfb is installed, it also checks real desktop close events and verifies that minimizing
to the tray does not trigger the shutdown watchdog.

Optional `SOUNDUX_TEST_TMPDIR` selects the directory for isolated integration-test files.

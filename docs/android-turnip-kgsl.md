# Android KGSL fork

This branch starts at upstream Mesa `e1f3f372c4a661cd0e71a0cdafc4d469d56ecf35`
(26.3.0-devel, 2026-09-19). Upstream remains
<https://gitlab.freedesktop.org/mesa/mesa>; this GitHub fork uses the
`chaotic-cx/mesa-mirror` mirror.

## Zero-timeout polling

KGSL WAITTIMESTAMP treats timeout 0 as an infinite wait. Vulkan timeline polling
and timeline garbage collection require nonblocking completion checks. Use
READTIMESTAMP_CTXTID with RETIRED for expired/zero deadlines; compare 32-bit
sequence numbers with wraparound. Positive remaining nanoseconds round up to
milliseconds, bounded by INT_MAX. Recompute deadlines after EINTR/EAGAIN and
return device-lost for unexpected ioctl failures rather than reporting timeout.
Completed timestamps still retire normally. No completion is fabricated and no
queue ordering or external synchronization is removed.

Run `python3 src/freedreno/vulkan/tests/kgsl_wait_test.py` with a host C++ compiler.
The test compiles the actual helpers with deterministic clock and ioctl inputs;
37 checks cover pending/completed/wrapped timestamps, expired and fractional
waits, infinite waits, interruption, and errors. The upstream helper fails the
positive sub-millisecond deadline case.

A bounded standalone Vulkan transfer/timeline test on Adreno 840 (`0x44050a31`)
with Linux 6.12 KGSL measured:

| Operation | Upstream | Patched |
|---|---:|---:|
| Counter query | 199.222 ms, value 1 | 0.007 ms, value 0 |
| Second queue submission | 191.680 ms | 0.020 ms |
| Zero-timeout wait | 195.911 ms, SUCCESS | 0.003 ms, TIMEOUT |

All three actual retirement waits completed and verified the requested timeline
value in each run. Patched finite waits still took about 192–197 ms. These are
synchronization correctness measurements, not game performance or broad GPU
conformance results. No ioctl repair interposer was enabled for the patched run.

## Reproduce the build

Requires Android NDK r29, Meson, Ninja, Python with Mako/PyYAML/packaging,
Bison 3.8 and Flex. On macOS, put Homebrew Bison/Flex before the system binaries
in PATH for this command (the system Bison is too old).

```sh
export ANDROID_NDK_HOME=/path/to/android-ndk
PATH="/opt/homebrew/opt/bison/bin:/opt/homebrew/opt/flex/bin:$PATH" \
  bin/build-android-turnip-kgsl.sh /absolute/path/to/build
```

Output is an AArch64 Android API 33 Vulkan HAL, not a ROM driver replacement.
The build dynamically depends on the NDK `libc++_shared.so` and Android platform
libraries; an app must supply a compatible C++ runtime and loader namespace.
Do not install the Android stub libraries into the device. Check SHA-256 and
ELF Build-ID of the exact library deployed for each device validation.

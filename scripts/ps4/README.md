# PlayStation 4 port

Experimental port for jailbroken PS4 consoles, built with [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain).

## Building

Requirements: OpenOrbis toolchain (LLVM 18 build), `clang-18`, `lld-18`, `python3`, `curl`.
`PkgTool.Core` from the toolchain needs OpenSSL 1.1 (`libssl1.1`) on modern distributions.

```
export OO_PS4_TOOLCHAIN=/path/to/OpenOrbis/PS4Toolchain
scripts/ps4/build.sh              # build/IV0000-XASH00001_00-XASH3DFWGSPS4000.pkg
scripts/ps4/build.sh 192.168.0.10 # same, and upload to the console FTP (GoldHEN, port 2121)
```

`scripts/ps4/apply-patches.sh` adds PS4 platform to `library_suffix` and `mainui` submodules,
until it's merged to their repositories.

## Running

* Game data goes to `/data/xash/valve` (or `/mnt/usbN/xash/valve` on USB storage).
* Game libraries built from [hlsdk-portable](https://github.com/FWGS/hlsdk-portable) go to
  `valve/cl_dlls/client_ps4_amd64.prx` and `valve/dlls/<name>_ps4_amd64.prx`.
* `extras.pk3` is copied from the package to `/data/xash/valve` on start.
* Extra command line arguments can be put into `/data/xash/xash3d.cmdline`,
  by default `-log -dev 2` is used.
* Output is written to `/data/xash/stdout.txt` and `/data/xash/engine.log`.

## Status

Half-Life runs: menu, gameplay, sound, DualShock 4, level changes, save/load, exit.

* Software renderer only: Piglet (system GLES2) can't compile shaders at runtime.
  Frame is rendered at configured resolution (1280x720 by default) and scaled to the screen.
* DualShock 4 is read through libScePad directly.
* Savegames store function offsets instead of names (no `dladdr`).

## Implementation notes

OpenOrbis toolchain needed several workarounds, all in `engine/platform/ps4/compat`
and linked into every image by `scripts/waifulib/ps4.py`:

* `ps4_crtlib.c` replaces toolchain `crtlib.o`, which never runs global constructors of modules.
* `ps4_malloc.c` replaces per-image musl heaps with one locked heap owned by `eboot.bin`,
  modules find it by a signature in its data segment.
* `ps4_cwd.c` emulates working directory (PS4 apps can't `chdir`, relative paths are rejected)
  and converts FreeBSD 9 `struct stat` returned by the kernel.
* Images must be linked with `ld.lld` of the same version as clang (18), newer lld picks
  musl's `lite_malloc`.

## Debugging

* `stdout.txt` survives crashes. Crashes print registers and stack as `module+offset`,
  resolve them with `scripts/ps4/symbolize.sh <console IP> [hlsdk build dir]`.
* `-ps4watchdog` in `xash3d.cmdline` dumps engine thread state if nothing is logged for 10 seconds.

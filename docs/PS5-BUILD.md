# PS5 cross-build status

Shelf cross-compiles its C++ UI, SDL event loop, read-only discovery adapter and exact-owned Home shortcut manager into a native PS5 executable. Its software renderer uses PS5 VideoOut and a direct-memory allocator. Stable v0.1.0 is the public production build for firmware 12.20; hardware acceptance remains bound to the exact executable and package manifest supplied with that release.

## Build without a system-wide installation

On Windows, these commands use an **existing** WSL distribution named `Ubuntu`. The scripts do not install WSL, run apt, use sudo, alter shell profiles, set global environment variables, or connect to a console. On x86-64 Ubuntu/Linux, the Python scripts can be invoked directly instead. A compatible host libc, binutils (`ar`/the native linker), `dpkg-deb`, and Python 3.10+ must already exist. A compiler and development headers need not be installed globally.

```sh
npm run build:ps5
npm run test:ps5-platform
npm run verify:ps5
npm run test:ps5-verifier
```

## Linked sources and relinking

`npm run prepare:ps5-sources` downloads and checks the complete sources for PS5 SDL 2.30.12, libiconv 1.17 and libsamplerate 0.2.2, plus their original PacBrew v0.40.2 build recipes. [Source pins](../tools/linked-sources.json) distinguish published recipe hashes from the SDL archive hash computed after matching its full Git tree. The linked SDL archive reports the same exact revision. The checker reads the complete compressed streams, validates member/link paths and reconstructs SDL's Git tree; it does not extract or execute upstream scripts.

After a verified app build, retain its own object/library inputs in a new snapshot and test the default relink:

```sh
npm run relink:ps5 -- --capture-inputs --inputs .cache/ps5/relink-inputs-my-build
```

The tool checks all three original static archives against the pinned PacBrew tarball and all SDK stubs against the pinned SDK ZIP. It preserves eighteen application objects, source fingerprints and hashes. It then rebuilds the converter and links those objects into a separate output directory. Without a replacement library, both the raw executable and its SELF must reproduce the reference hashes exactly. `--libiconv`, `--sdl` and `--libsamplerate` accept replacement PS5 static libraries; these deliberately may change the result. They do not recompile Shelf or deploy anything.

With reviewed source committed, `npm run prepare:ps5-source-kit -- --inputs .cache/ps5/relink-inputs-my-build` assembles the tracked source, notices, full library sources, original recipes and matching object/library snapshot. Its ZIP and every member are rechecked. `SOURCE-KIT.txt` documents relinking from the extracted folder and replacing libraries. First use can download the pinned public build dependencies. A cold relink from the extracted kit and full console/launch acceptance are still release gates; preparing the kit does not publish it.

The first build downloads the pinned target dependencies and an isolated Clang 18 prefix into ignored `.cache/ps5/`. Subsequent builds verify and reuse those archives. Nothing is deployed. The output is `.cache/ps5/build/eboot.elf`, `eboot.bin`, and `structural-check.json`; it is **not** a ready-to-install app folder. Do not copy the executable over another app.

The Ubuntu compiler packages are exact versions and SHA-256 values in [ubuntu-toolchain.lock.json](../native/ps5/ubuntu-toolchain.lock.json). Those pins were resolved from Ubuntu Noble's `main` and `universe` package indices, with their hashes checked against an InRelease signature verified using Ubuntu's installed archive keyring. `bootstrap_ps5_toolchain.py --resolve-lock` is an explicit maintainer action to refresh the lock; ordinary builds do not resolve new versions. Packages are extracted as data with `dpkg-deb`, without executing package scripts. The host's libc/loader are retained. A local linker-script overlay relocates the development-only `libc_nonshared.a` path, leaving the extracted package and OS files unchanged.

## Target dependencies and attribution

The [target dependency pins](../native/ps5/dependencies.json) identify:

- Public PS5 Payload SDK v0.42; SHA-256 checked against its GitHub release asset digest.
- PacBrew v0.40.2 static ports; SHA-256 checked against its GitHub release asset digest. Only its `target/user/homebrew` prefix is extracted. Shelf links SDL 2.30.12 (`SDL-2.30.12-ga47182a20`), libiconv 1.17 and libsamplerate 0.2.2. The Windows host uses a separate SDL 2.32.10 package.
- zlib 1.3.2, built locally from its pinned canonical source archive for the host packager.

The native startup, linker layout, module conversion, FSELF writer and clean-room runtime builder are imported from Jewel Case's pinned source, originally by BlackBearReloaded with converter/writer portions derived from SvenGDK's SharpProspero. Their original copyright and GPL notices are retained under [native/third_party/ps5-runtime](../native/third_party/ps5-runtime/). Its SOURCE.json records every imported file's byte count and SHA-256; the build checks them before compilation.

Shelf adapts Jewel Case's allocator, VideoOut and missing-import shims under [native/ps5/platform](../native/ps5/platform/). That directory records original paths/hashes and marks the adaptations. Changes add overflow checks, a bounded import report, a notification guard, the first back-buffer selection fix and joined display shutdown with direct-memory cleanup. A failed display close retains its scan-out memory for process teardown instead of unmapping active buffers. PS5 launch arguments cannot override Shelf's owned storage paths. The application uses no copied Jewel Case UI, assets, music, game database or embedded emulator.

The clean-room runtime is rebuilt twice from source. Both raw copies must match `8ee6e124993e1af26420cb455890fd002f5d6c7e78883c860ce45734e7d002bb`; both signed copies must match `e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036`. A failed match stops the build. It contains no downloaded proprietary runtime binary.

## What the checks establish

The host platform tests exercise the actual PS5 allocator/display source using fake console functions. They check allocation overflow, failed realloc preservation, five startup failure paths, the first displayed frame, alternating buffer selection, joined cleanup and memory retention after a failed display close. They do not simulate the PS5 graphics driver or establish its ABI compatibility.

The packager inspects the completed FSELF's container integrity. A separate Python verifier checks native ELF identity, executable entry, LOAD bounds, page alignment, absence of writable/executable LOAD segments, the expected system-module dependency set and the SELF wrapper. It also requires the exact public import contract, owned-tile marker and strong SystemService launch/self-close imports, while rejecting the private UI bridge, preview probes and protected-source diagnostics. Recorded artifact hashes reject changes after the container check. `test:ps5-verifier` damages or rebinds representative copies to prove these checks fail closed. Build inputs are fingerprinted; verification refuses an executable if those inputs have since changed. These checks do not establish hardware, firmware or physical controller compatibility. The regular Windows model/renderer tests remain separate.

## Release acceptance

Check the unofficial title identity for an installed collision. Assemble the owned app folder, artwork, complete licenses and corresponding source/relink instructions for all linked libraries, including LGPL libiconv and the precise PS5 SDL port revision. Validate discovery, shortcut ownership, launch contracts and transfer/readback hashes, then test display, DualSense input, saved settings, restart/reopen, bulk shortcut create/update/removal and supported routes on firmware 12.20. Publication remains gated on an acceptance record bound to the exact program and package manifest.

The build also compiles Shelf's original launcher using the ordinary SDK ELF-payload ABI, embeds it, and records the byte extent and SHA-256 in the executable check. It is a cooperative app launcher, not an exploit; the optional standard SDK runtime performs the in-memory permission initialization described below. No helper, firmware updater, network configuration tool or console deployment command runs as part of the cross-build. Matching SDK CRT/static-libc source and notices are an additional release requirement for this embedded helper. The SDK's LLVM libc++ headers carry the retained [Apache license and LLVM exceptions](../native/third_party/licenses/LLVM-libcxx.txt).

A separate [private UI test assembler](PS5-UI-TEST.md) can prepare a local folder and ZIP after the reviewed sources are committed. It records every file hash and never deploys or publishes anything. Private bridge builds are development candidates and are structurally rejected as stable release inputs; the normal public build keeps the owned-shortcut feature without that bridge.

The normal build uses the owner-approved, unmodified, hash-checked SDK helper CRT required for emulator launching. Its receipts record `runtimeMode: standard-sdk` and `permissionChanges: true`; building alone does not deploy or run it. `npm run build:ps5-restricted` explicitly stages the diagnostic restricted CRT, which cannot complete the running-app query on the owner's console and must not be installed as a launchable Shelf build.

## Preparing public assets

`npm run prepare:ps5-release -- --version v0.1.0 --acceptance /absolute/path/to/hardware-checks.json` prepares the seven stable-release downloads locally; it never publishes or contacts a console. The reviewed acceptance JSON must identify schema 1, firmware `12.20`, the exact `selfSHA256`, and actual owner results for every check named in `REQUIRED_CHECKS` in the tool. Missing or incomplete checks stop preparation before an output folder is created. These are human reports, not independently simulated hardware tests.

The tool requires a clean source tree, matching compiled inputs, complete console readback receipt and an exact cold-tested source/relink baseline. It preserves every tested app file except the installation text, retains the source kit's code/objects/libraries/upstream archives byte-for-byte, and updates only its publication instructions and generated metadata. It verifies all output ZIP members and writes size/SHA-256 records. The resulting public source ZIP still needs its own cold-relink check, and the final documentation needs console readback before publication. All artifacts remain under ignored `.cache/ps5/releases/`.

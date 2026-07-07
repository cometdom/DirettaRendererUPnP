# Changelog

## [2.5.8] - 2026-07-06

### Added
- **DoP (DSD over PCM) output mode** (`--dop` CLI flag, issue #80 requested by yama3kzh). For DACs that receive Diretta PCM but decode DoP natively (e.g., connected via I2S or S/PDIF passthrough at the target), `--dop` causes DSD streams to be transmitted over the Diretta PCM channel as standard 24-bit DoP frames instead of native Diretta DSD. Encoding: 2 DSD bytes per channel packed into a 24-bit PCM word with alternating 0x05/0xFA markers in the MSB byte (standard DoP v1.1 format). Sample rate mapping: DSD64→176.4kHz, DSD128→352.8kHz, DSD256→705.6kHz, DSD512→1.4MHz. Ring buffer operates in PCM mode (24-bit packed, silence=0x00); DoP marker state resets cleanly on each track start via `clear()`. No change to PCM or native DSD paths; the flag is a no-op when playing PCM. Format transition logic (DSD64→DSD128, DSD→PCM) uses the existing full close/reopen sequence since the underlying source format is still DSD.
- **`--dop-msb` flag** (issue #80). For DACs whose DoP decoder expects MSB-first DSD payload in 24-bit DoP frames (rather than the LSB-first default from DSF files), `--dop-msb` enables intra-byte bit reversal of each DSD byte before packing into DoP frames. The reversal uses a static 256-entry lookup table in `DirettaRingBuffer` — zero branches and zero heap allocations in the audio hot path. `--dop-msb` implies `--dop`. Verbose mode prints the first few DoP pushes with raw DSD bytes for diagnosis. Also accessible as `DOP=msb` in the config file and as a third option in the web UI DSD Output Mode selector.

### Fixed
- **DoP: PCM 0x00 silence before audio start causes DACs to miss DoP lock** (issue #80 — primary cause of "music + noise" reports). `getNewStream()` filled all silence periods (prefill, stabilisation, underrun) with `memset(0x00)` — raw PCM silence with no DoP markers. DACs that scan for the 0x05/0xFA alternation only at stream initialisation, not continuously, established a PCM stream during that silence and never switched to DoP mode; the subsequent DoP frames were played as regular PCM, producing wideband quantisation noise. Fix: `getNewStream()` now uses a `fillSilence()` helper that, when DoP mode is active, generates proper DoP silence frames — `[0x69, 0x69, marker]` per channel with alternating 0x05/0xFA markers — instead of raw 0x00. The DAC sees DoP markers from the very first frame and can lock before real audio starts. Marker state (`m_doPSilenceMarkerState`) persists across calls for a coherent marker stream; reset on each track open. PCM and native DSD silence paths are unchanged.
- **DoP: stabilisation silence count inflated 30× by wrong cycle-time formula** (issue #80 — secondary contributor). The stabilisation in `getNewStream()` computed cycle time as `efficientMTU / bytesPerSecond`. For DoP at 176.4kHz (1056-byte buffers, MTU=1500): 1497 / 1,058,400 × 1e6 = **1.4 µs** instead of ~1 ms, inflating the required buffer count to 70,000+ (capped at 3000) → ~3 s of silence instead of 100 ms. Fixed (commit `912a177`) to use `currentBytesPerBuffer`. Same fix applied to the DSD branch.
- **DoP: continuous "music + noise" caused by marker phase corruption from the 44.1k drift corrector** (issue #80 — confirmed root cause for both yama3kzh/SFORZATO and Dominique/HOLO-via-DDC-0). The 44.1k-family drift corrector in `getNewStream` accumulates 400 units/call (from 176400 % 1000) and fires when it reaches 1000, adding one extra frame to compensate for the non-integer 176.4 frames/ms rate. This produces a 177-frame (1062-byte) buffer approximately every 2.5 calls. 177 is **odd**: `fillSilence` generates 177 silence frames, leaving `m_doPSilenceMarkerState` flipped (net 1 flip = true). When real audio resumes from the ring, the ring's first frame uses `m_dopMarkerState = false` (0x05) while the last silence frame also emitted 0x05 → two consecutive 0x05 markers → DAC loses DoP sync → continuous noise. Two fixes together close all paths: (1) **`getNewStream` drift corrector**: in DoP mode, threshold changes from 1000 to 2000 and add changes from 1 to 2 frames, so it fires every ~5 calls and adds 178 frames (even). Average rate unchanged: 2 frames / 2000 units = 1 frame / 1000 units = 0.4 extra frames/call = 176.4. (2) **`pushDSDToDoP` even-frame guard** (`if (pcmFrames % 2 != 0) pcmFrames--;`, same commit `209365f`): ensures every push writes an even number of frames, keeping ring available-bytes always at even × 6. With fix (1) ensuring all buffer sizes are even and fix (2) ensuring ring content is even × 6, all silence/ring transition frame counts are even → `m_doPSilenceMarkerState` and `m_dopMarkerState` never diverge → perfect 0x05/0xFA alternation. Unit test `test_pushDSD_dop_marker_phase_invariant` (added for the push-side invariant) and the getNewStream drift fix are both required.
- **DSD / DoP stabilisation divide-by-zero** (issue #80). `configureRingDSD()` never stored `m_sampleRate` or `m_bytesPerSample` for DSD mode. When the first track is DSD, both were 0, causing a divide-by-zero in the DSD-branch cycle-time formula inside `getNewStream()`. Fixed by storing `byteRate * 8` and `1` respectively at the end of `configureRingDSD()`.
- **CPU affinity: cores silently rejected as invalid on AMD Ryzen with HT/SMT disabled** (reported by Kiran on a Ryzen 7730U running AudioLinux). `sysconf(_SC_NPROCESSORS_ONLN)` returns the *count* of online CPUs (8 on a Ryzen 7730U with HT off), which DRUP incorrectly used as the maximum valid CPU ID. With SMT disabled, Linux takes the odd-numbered logical CPUs offline and keeps the even-numbered ones active (0, 2, 4, ..., 14) — so cores 10, 12, 14 are perfectly valid but were rejected as `>= 8`. The validation silently cleared `config.cpuAudio/Decode/Other`, disabling DRUP's per-thread `pthread_setaffinity_np` calls entirely. Fix: new `getOnlineCpus()` helper reads `/sys/devices/system/cpu/online` and validates against actual set membership. Falls back to `0..N-1` if the file is unreadable. Same fix applied to slim2Diretta v1.4.10.
- **`install.sh`: FFmpeg check falsely reported `[MISSING] mov`** — `ffmpeg -demuxers` in FFmpeg 8.x lists the QuickTime demuxer as `mov,mp4,m4a,3gp,3g2,mj2` (alias group); the post-build check searched for `" mov "` (space on both sides) which never matches because the alias list is comma-separated. Fixed to `grep -qE " ${dem}[ ,]"`. The demuxer was always compiled in; only the check message was wrong.
- **`install.sh`: FFmpeg decode test failed on every minimal build** — the test used `-f lavfi -i "sine=..."` which requires the `lavfi` demuxer and `sine` source filter, neither of which is included in the minimal audio-only configure profile. Every minimal build would print a false-positive `[WARNING] FFmpeg decode test failed`. Replaced with a silent `s16le` byte-pipe that works with the minimal profile.
- **`install.sh`: WebUI profiles not refreshed after `--build` / option 3** — `build_renderer()` now detects whether the WebUI is already installed and copies the updated profile JSON files after each build.

## [2.5.7] - 2026-07-01

### Fixed
- **`install.sh`: FFmpeg 8.1+ fails to configure without `udp` in the protocol list** (issue #81, reported by sheviks). FFmpeg 8.1+ introduced a compile-time dependency on the `udp` protocol (likely via `hls`); configuring without it caused the build to fail. Added `udp` to both `get_ffmpeg_configure_opts` (legacy full build) and `get_ffmpeg_8_minimal_opts` (minimal build). No change to the DRUP binary itself.

### Changed
- **`install.sh`: FFmpeg menu overhauled** — FFmpeg 5.1.2 (2022, unmaintained) removed; FFmpeg 8.x bumped from 8.0.1 to 8.1.2; new option added for FFmpeg 7.1.1 minimal audio-only build (same configure as the 8.x minimal, recommended for Pi and low-RAM systems); RPM Fusion option description updated to clarify it installs the distro-maintained version with automatic security updates. Version numbers are now defined as named constants (`FFMPEG_7_VERSION`, `FFMPEG_8_VERSION`) at the top of the script — bumping a version is a one-line change. Final menu: (1) FFmpeg 7.1.1 full, (2) FFmpeg 7.1.1 minimal, (3) FFmpeg 8.1.2 minimal [default], (4) RPM Fusion / system packages.

## [2.5.6] - 2026-06-24

### Fixed
- **Boot hang: Target stuck in stale idle-mode when powered on before DRUP** (PR #79 by hoorna/Alfred). When a Diretta Target has been idle for more than a few minutes before DRUP starts, it can enter a stuck idle-mode: it accepts the SDK connection but never reaches a streaming-capable state — LEDs blink fast indefinitely and no UPnP renderer can claim it. The only escape was an external UPnP `AVTransport:Stop` command, which triggered the idle-release timer (~5 s), which in turn called `release()` and let the Target reset. Root cause is a state-machine bug in the Target firmware (confirmed independently of OS, SD card, predecessor client, and shutdown cleanliness). The previous warmup (`open()` → `stopPlayback()`, commit `0d1279b`) left the SDK connection open after boot; if Target was already stuck when we connected, the warmup just trapped us in the same state. The fix holds the SDK connection open for 6 seconds then calls `release()` cleanly, which is sufficient for Target to exit the stuck mode. The first real play then does a fresh cold connect. Trade-off: boot is ~6 s longer, and the first play pays a small cold-connect cost (~500 ms–2.5 s observed); both are well under the original ~5 s first-play glitch that motivated the warmup. All other playback paths (track changes, quick-resume, idle release timer, renderer switching) are unaffected.

## [2.5.5] - 2026-06-18

### Fixed
- **Build: hard `SIGILL` (invalid opcode) on Zen3/Zen2 "Ryzen 7000" mobile CPUs** (reported by Didier/ds21 on a Topton FU02 / Ryzen 7 7730U). The Makefile's Zen4 auto-detection used a model-name regex (`Ryzen.*(5|7|9).*7[0-9]{3}…`) to select the SDK library variant. AMD's mobile branding reuses the "Ryzen 7000" *number* for older silicon — the Ryzen 7 7730U is Barcelo (Zen3, no AVX-512), the Ryzen 5 7520U is Mendocino (Zen2) — so those chips matched the Zen4 rule and got the `x64-linux-15zen4` SDK lib plus `-march=znver4`, both of which emit AVX-512 instructions the CPU cannot execute. The process core-dumped with `status=4/ILL` the instant the SDK's `DIRETTA::Connection` constructor ran (stack trace `#0 _ZN7DIRETTA10ConnectionC2Ev`), before any audio or network activity, and systemd restart-looped it indefinitely (restart counter observed at 830). The fallback AVX-512 feature check that would have caught this was only consulted when the model-name regex returned 0, so it never ran for these parts. Fix: a genuine Zen4 always has AVX-512, so a hard guard now forces `IS_ZEN4=0` whenever `/proc/cpuinfo` lacks the `avx512` flag — such CPUs correctly fall through to the AVX2 (`x64-linux-15v3`) variant. Real Zen4 (7700X etc.), Intel AVX-512 (→ `v4`), and plain AVX2 boxes (→ `v3`) are all unaffected. Existing wizard/`install.sh` users on an affected CPU just need to `git pull` and re-run `./install.sh` (or rebuild once with `make ARCH_NAME=x64-linux-15v3`).

## [2.5.4] - 2026-06-18

### Fixed
- **Web UI: duplicate `KEY=VALUE` lines accumulating in `/etc/default/diretta-renderer` on every save** (PR #75 by hoorna/Alfred, found while investigating a `--cpu-other 0` pinning failure on a Raspberry Pi 4). `ShellVarConfig.save()` in `webui/config_parser.py` matched assignment lines with `^#?\s*([A-Z_][A-Z0-9_]*)=`, so a commented-out *example* line such as `#CPU_AUDIO=2` was treated as an active setting and rewritten as `CPU_AUDIO=2`; and with no `key not in written_keys` guard, every occurrence of a key was rewritten rather than just the first. Combined with `install.sh`'s migration `sed`, which used an unbounded `s|^#\?KEY=.*|KEY=val|` and therefore activated *every* `#KEY=...` example line in the freshly-copied config, a single web-UI save could leave three identical `CPU_AUDIO=...` lines — growing on each subsequent save (confirmed in the wild on Dominique's host: three `CPU_AUDIO=8` lines; harmless only because systemd `EnvironmentFile=` applies last-wins). The fix: (1) `config_parser.py` now matches only active (uncommented) assignments, updates the first occurrence per key, drops later active duplicates, and preserves commented example lines untouched; (2) `install.sh`'s migration `sed` is bounded to the first match per key with `0,/pattern/{s|...|}`; (3) new `webui/test_config_parser.py` adds stdlib-`unittest` regression coverage (commented-line preservation, duplicate collapse, idempotency, append-new-key, comment/blank preservation — 5/5 pass, no new deps). The shell wrappers already trimmed defensively per element, so this was a cosmetic-but-unbounded on-disk growth bug, not a functional one. Symmetric fix shipped in slim2Diretta v1.4.7.
- **CPU tuner: `--cpu-*` thread pinning failed with `EINVAL` when a flag referenced a core outside the isolated set** (PR #77; root cause analysis and Pi 4 testing by hoorna/Alfred, x86+SMT testing by Dominique). The `diretta-renderer-tuner.sh` (and `-nosmt`) generated systemd slice confined the service to `AllowedCPUs=${RENDERER_CPUS}` (the isolated renderer cores only). DRUP then pins its own threads via `--cpu-audio`/`--cpu-decode`/`--cpu-other` (#68): when any flag named a core *outside* that cpuset — typically a housekeeping core such as `CPU_OTHER=0` — the kernel rejected the `sched_setaffinity` call with `EINVAL`. A second problem compounded it: the tuner's `ExecStartPost=distribute-diretta-threads.sh` round-robin overwrote whatever pinning *did* land. The cgroup `AllowedCPUs` is static (baked at install) while `CPU_*` is dynamic (web-UI editable, then restart), so widening the slice to all cores at install time would permanently relax isolation — and on x86 pull the housekeeping core's SMT siblings into the cpuset — even for users who never set a `--cpu-*` flag. Fix: the tuner keeps the slice **strict** and drops the conflicting `ExecStartPost`; `start-renderer.sh` reconciles the cpuset at every start via `systemctl set-property --runtime`, to a deterministic target — the tuner-baked renderer cores plus exactly whatever `CPU_*` references (or the baked cores alone when no `CPU_*` is set). The base value is read from the slice unit file (`FragmentPath`), not from the live `AllowedCPUs` property, so a `--runtime` override left by a previous start (these persist across `systemctl restart`, only a reboot clears them) cannot leak: clearing `CPU_*` and restarting within one boot correctly restores strict isolation. Net result vs. the install-time-widening alternative (#76, superseded): strict isolation is preserved when no `--cpu-*` flag is used, no SMT-sibling leak on x86, and the cpuset always matches the live config.

## [2.5.3] - 2026-06-12

### Fixed
- **Web UI: comma-separated values are canonicalised at save time** (reported by Dominique while wiring `IRQ_INTERFACE` through the fedora-audiophile-setup stable-naming refactor on 2026-06-02). A user typing `eth-diretta, eth-lan` in the web UI used to persist that literal string — natural for free-text input but cosmetically inconsistent with the documented `enp1s0,enp2s0` form. The shell wrappers (`start-renderer.sh` and the same logic mirrored in `install.sh`) already trim defensively per-element (`tr -d ' '`), so this was not a functional bug; IRQ pinning worked regardless. The fix eliminates the cosmetic divergence on disk and protects any future consumer of `/etc/default/diretta-renderer` that reads the file without trimming. Applied to the five known comma-list fields: `IRQ_INTERFACE`, `IRQ_CPUS`, `CPU_AUDIO`, `CPU_DECODE`, `CPU_OTHER` (full profile); `CPU_AUDIO`, `CPU_DECODE`, `CPU_OTHER` (minimal profile — IRQ tuning belongs to the downstream distro on that flavour). Idempotent: values that are already canonical pass through unchanged, so configs only get rewritten when the user explicitly opens and saves the form. Symmetric fix shipped in slim2Diretta v1.4.2.

### Changed
- **Profile-driven normalization machinery in the web UI save handler** — setting JSON declarations gain an optional `"normalize"` field. Currently the only supported rule is `comma_list` (`re.sub(r'\s*,\s*', ',', value.strip())`), but the structure is in place for any future per-field input canonicalisation (e.g. lowercase MAC, trim whitespace, normalize boolean strings). Adding a future comma-list field to either profile is one line of JSON; no Python change needed.

## [2.5.2] - 2026-06-04

### Changed
- **`install.sh`: `--allowerasing` on FFmpeg `dnf install` and low-RAM warning before LTO compile**. Two small UX improvements; no functional change to the DRUP binary itself. (1) `install_ffmpeg_rpm_fusion` and `install_ffmpeg_system` now pass `--allowerasing` to `dnf install` so dnf can cleanly swap between `ffmpeg-free` / `ffmpeg-free-devel` (Fedora's default packages) and `ffmpeg` / `ffmpeg-devel` (RPM Fusion) when the user re-runs the installer with a different choice — previously the install failed on a "conflicting requests" error that the user had to resolve by hand. (2) A new `check_compile_ram()` helper is invoked at the start of `build_ffmpeg_8_minimal` and `build_ffmpeg_from_source`. The FFmpeg 8 LTO link stage can peak around 4–6 GB; on a 4 GB box without swap the linker was silently OOM-killed and the build ended on a vague error. The helper warns (does not block) when `MemTotal < 8 GB` and prints the exact commands to create / activate / remove a temporary swap file. We don't create one automatically, on purpose — audiophile setups run swap-less by design.

---

## [2.5.1] - 2026-05-24

### Fixed
- **Permanent hang on live radio stream stall via Roon proxy** (reported by hoorna/Alfred for Mother Earth Radio Classic via Roon): When Roon serves an internet radio station via its local HTTP proxy, the TCP connection stays alive (keepalives) even when the upstream CDN or station stops sending audio. `av_read_frame()` blocks indefinitely on such a connection — the existing 30-second FFmpeg I/O timeout does not fire because TCP keepalives prevent a socket-level timeout. The ring buffer empties, `getNewStream()` enters rebuffering mode, and the renderer never recovers: Diretta Link LEDs keep blinking, the Roon playing-time counter runs on, and no UPnP command has any effect. The fix registers an `AVIOInterruptCB` on the `AVFormatContext` before `avformat_open_input()`. In each `readSamples()` call, a per-call deadline (`now + 20 s`) is written to an atomic before `av_read_frame()` and cleared to zero after it returns. The callback checks the deadline on every FFmpeg I/O poll; when `now >= deadline` it returns 1, causing `av_read_frame()` to abort with `AVERROR_EXIT`. A new `m_readTimeout` flag (analogous to v2.4.5's `m_decodeError`) is set, and `process()` detects it before the `samplesRead == 0` guard and triggers an immediate clean stop: preload thread joined, next-track state cleared, `m_state = STOPPED`, `m_trackEndCallback()` fired. The UPnP controller (Roon) receives the correct STOPPED state, the playing-time counter freezes, and the Diretta target is fully released — identical observable behaviour to the v2.4.5 corrupt-packet fix. The interrupt callback is zero-cost during normal playback (deadline is 0, first branch returns immediately). The `triggerFatalStop` lambda in `process()` is shared between the decode-error and read-timeout cases, eliminating the duplicate 14-line teardown.

## [2.5.0] - 2026-05-23

### Added
- **`mlockall` at startup**: DRUP now calls `mlockall(MCL_CURRENT | MCL_FUTURE)` early in `main()`, just after the CPU-affinity validation block and before the main thread is pinned / any worker is spawned. All of the process's pages — code, heap, stack, and every page allocated thereafter — are locked into RAM for the lifetime of the binary. No page of DRUP can be swapped out, evicted from the page cache, or trigger a major/minor page fault that would otherwise stall the audio thread despite SCHED_FIFO + CPU pinning + isolcpus. This is the same memory-locking discipline JACK and PipeWire perform in RT mode, and it closes the last non-deterministic source of stalls (memory pressure / cache reclaim) for a CONFIG_PREEMPT_RT + isolated-CPU host. On `EPERM` (e.g. CLI run without privileges), a `LOG_WARN` is emitted and the binary continues; no behavioural change otherwise. The "Memory locked in RAM (mlockall MCL_CURRENT|MCL_FUTURE)" line is visible in the journal on every successful startup. RSS becomes a hard floor for the process — on this binary that's a few MiB and entirely negligible on any host running DRUP.

### Changed
- **`systemd/diretta-renderer.service`**: added `CAP_IPC_LOCK` to both `AmbientCapabilities` and `CapabilityBoundingSet`, and added `LimitMEMLOCK=infinity`. Without these, `mlockall` would fail with `EPERM` even though the unit runs as root — `CapabilityBoundingSet` strips any capability not listed (the user-supplied root identity does not grant capabilities that are bounded out), and `LimitMEMLOCK` is checked before `CAP_IPC_LOCK` allows it to be ignored. Comment block reorganised to explain each capability and the new resource-limit section. New installs (and any user copying `systemd/diretta-renderer.service` to `/etc/systemd/system/` over an existing unit) will pick this up automatically; users running an older locally-modified unit need to merge in these lines and `systemctl daemon-reload`.

## [2.4.5] - 2026-05-20

### Fixed
- **Lossy radio (AAC/MP3) white noise on 24-bit-limited DACs — S24 alignment** (companion to the v2.4.4 sink cap, reported by Laurent for France Musique AAC via JPLAY iOS on a TEAC UD-701N): the v2.4.4 cap fixed the sink negotiation (DRUP correctly asks for 24-bit) but the `s24Alignment` hint was still left as `Unknown` for lossy codecs — their decoder output is `AV_SAMPLE_FMT_FLTP` (float), which matches none of the three pre-existing branches (`PCM_S24LE/BE`, `FLAC/ALAC`, `sample_fmt == S32/S32P`). With the hint missing, `DirettaRingBuffer` had to auto-detect alignment on the first push and could pick `LsbAligned` on dynamic/silent content, producing white noise on 24-bit-only DACs. The resampler always converts a 24-bit lossy stream to `AV_SAMPLE_FMT_S32` (data in the upper 24 bits = MSB-aligned), so a 4th branch now marks such codecs as `MsbAligned` explicitly, using the same `AV_CODEC_PROP_LOSSY` codec-descriptor check as the v2.4.4 cap.
- **Renderer zombie state on corrupt PCM packet from radio stream**: A corrupt packet mid-stream caused `avcodec_receive_frame()` to return an error after some samples had already been decoded in the same `readSamples()` call. Because the error check was guarded by `samplesRead == 0`, it was silently skipped, leaving the decoder flagged as failed while the renderer kept running — producing silence and ignoring all subsequent UPnP commands. The fix moves the decode-error check before the `samplesRead == 0` guard so it fires regardless of partial reads. On detection, the preload thread is joined, next-track state (`m_nextDecoder`, `m_nextURI`, `m_nextMetadata`, `m_formatChangePending`) is cleared, and `m_state` is set to `STOPPED` before firing `m_trackEndCallback()` — mirroring the normal EOF teardown's state-then-callback ordering (intentionally without the end-of-track drain delay, since a corrupt packet is not a clean end), so the UPnP controller (including Roon) sees the correct state before transitioning. (PR #72 by hoorna/Alfred)

---

## [2.4.4] - 2026-05-16

### Fixed
- **Lossy radio streams (AAC/MP3) silent on 24-bit-limited DACs** (reported by Dominique for a friend's TEAC UD-701N on AudioLinux): FFmpeg decodes lossy codecs (AAC, MP3, Vorbis, Opus, AC-3, WMA…) into a float buffer (`FLT`/`FLTP`), which the bit-depth detection in `AudioEngine.cpp` mapped to 32-bit. That float is FFmpeg's internal calculation format, not a real 32-bit source — a 192 kbps AAC web radio (e.g. France Musique `francemusique-hifi.aac`) has far fewer than 16 effective bits. The bogus 32-bit value made `configureSinkPCM()` negotiate `FMT_PCM_SIGNED_32` with the sink; DACs that advertise 32-bit at the Diretta target level but are physically limited to 24-bit (e.g. TEAC UD-701N) then played silence or noise. Lossy codecs are now capped at 24-bit (transparent — their effective resolution is well below 24-bit, and every DAC accepts 24-bit). Lossless codecs (FLAC/ALAC/PCM) are identified via FFmpeg's codec descriptor props (`AV_CODEC_PROP_LOSSY` without `AV_CODEC_PROP_LOSSLESS`) and left untouched, so genuine 24/32-bit files still negotiate their real depth.

---

## [2.4.3] - 2026-05-11

### Changed
- **FFmpeg 8 minimal build: drop `--enable-small`, add `--enable-lto`** (Issue #70 reported by sheviks): The minimal FFmpeg 8.x configure flags in `install.sh` previously included `--enable-small`, which silently downgrades compiler optimization from `-O3` to `-Os` (GCC) / `-Oz` (Clang). With all the `--disable-everything` + selective `--enable-*` already trimming the build, `--enable-small` provided negligible size benefit while measurably hurting performance in the audio hot path (FLAC/AAC/PCM decoders, format conversions). Replaced with `--enable-lto` to align with the legacy/full FFmpeg build configuration and give the decoders the same `-O3 + LTO` treatment. Users who built FFmpeg via `install.sh` should recompile to benefit from the change.

---

## [2.4.2] - 2026-05-08

### Added
- **`--cpu-decode` option** (PR #68 by Daniel/Koala887): a third CPU-affinity granularity that pins the renderer's audio thread (HTTP receive + FFmpeg decode) to its own dedicated core, separate from the Diretta SDK worker (`--cpu-audio`) and from the lighter UPnP/position/main threads (`--cpu-other`). When `--cpu-decode` is set, the audio thread is also raised to `SCHED_FIFO` real-time priority (using `RT_PRIORITY`), since the dedicated core makes that safe. Falls back to `--cpu-other` when `--cpu-decode` is empty (no behavioural change for existing setups). Cross-core overlap warnings are emitted for all three combinations (audio/decode, audio/other, decode/other). Also exposed in the configuration file as `CPU_DECODE` and in the web UI (full and minimal profiles) under "CPU Affinity".

### Fixed
- **`ProtectKernelTunables=true` blocked IRQ affinity** (PR #68 by Daniel/Koala887): the systemd unit's `ProtectKernelTunables=true` directive prevented `start-renderer.sh` from writing to `/proc/irq/N/smp_affinity_list`, silently breaking the `IRQ_INTERFACE` / `IRQ_CPUS` feature shipped in v2.4.0. The directive is now commented out so the wrapper can apply the requested IRQ affinity. The other systemd hardening directives (ProtectKernelModules, ProtectKernelLogs, ProtectControlGroups, etc.) remain in place — only the kernel-tunables protection is relaxed, and only because the wrapper script genuinely needs to write to `/proc/irq/`.
- **Install script: stop service before replacing binary** (PR #69 by Daniel/Koala887): `install.sh` now detects whether `diretta-renderer.service` is currently running, stops it before copying the new binary into `/opt/diretta-renderer-upnp/`, and restarts it once the install completes. Previously, reinstalling on top of a running service silently failed because `cp` cannot overwrite a file held open by systemd, leaving the old binary in place until the next reboot.

---

## [2.4.1] - 2026-05-03

### Added
- **Minimal web UI profile** (`webui/profiles/diretta_renderer_minimal.json`): An alternative profile alongside the existing `diretta_renderer.json`, intended for downstream distributions that manage system-level tuning through their own framework (GentooPlayer, AudioLinux, etc.). The minimal profile drops everything that's wrapper-level system tuning — SMT toggle, NIC link tuning (`TARGET_INTERFACE` / `TARGET_SPEED` / `TARGET_DUPLEX`), IRQ affinity (`IRQ_INTERFACE` / `IRQ_CPUS`), and process priority shell vars except `RT_PRIORITY` (which is application-level via `--rt-priority` and remains exposed). It keeps everything that's strictly DirettaRendererUPnP application configuration: target, name, port, interface, gapless, minimal-UPnP, CPU affinity, buffer sizes, RT priority, and Diretta SDK options. Distributions can simply point their packaging at the `_minimal.json` profile instead of the default one. The full profile remains the default for self-install on a generic Linux distribution.
- **2.5 GbE option in `TARGET_SPEED`**: The "Advanced Network Settings" web UI dropdown now includes a `2500 Mbit (2.5 GbE)` choice alongside the existing 10 / 100 / 1000 options, for hosts equipped with 2.5 GbE NICs (Realtek RTL8125, Intel I225/I226, etc.). `ethtool` will refuse the value if the underlying NIC doesn't support it, and the launcher already logs a warning in that case — no functional change to the wrapper itself. The `.conf` comment is updated accordingly.
- **Minimal tarball release script** (`scripts/make-minimal-tarball.sh`): At release time, this script produces a `*-minimal.tar.gz` source archive from the current HEAD (or any tag passed as argument) where `webui/profiles/diretta_renderer.json` is the minimal profile content and `diretta_renderer_minimal.json` is removed. Intended to be uploaded as an additional asset on each GitHub Release alongside the standard tarball, so downstream distributors who ship by consuming the source archive (GentooPlayer, AudioLinux, etc.) can pick the minimal flavor without any packaging-side modification. By default the script also strips the " (Minimal)" suffix from `product_name` for a clean web UI label; set `STRIP_SUFFIX=0` to keep it.

---

## [2.4.0] - 2026-04-30

### Added
- **Target network link tuning** (PR #67 by Daniel/Koala887): New web UI section under "Advanced Network Settings" that forces the speed and duplex of the host NIC used to reach the Diretta target via `ethtool`. Some audiophile users report perceived sound-quality differences when constraining the link to a specific speed (typically 100 Mbit). Configurable via `TARGET_INTERFACE`, `TARGET_SPEED` (10 / 100 / 1000), and `TARGET_DUPLEX` (half / full) in the config file or web UI; leave `TARGET_INTERFACE` empty to keep the default behaviour. Requires the `ethtool` package, now added to the base dependency list installed by `install.sh` (dnf/apt/pacman). The launcher logs a clear warning and skips link tuning if `ethtool` is missing instead of failing silently. Web UI and `.conf` comments include a bandwidth-vs-format reminder so users don't accidentally pick a link speed too narrow for hi-res PCM or DSD (10 Mbit safe up to ~96 kHz PCM only; 100 Mbit comfortable through DSD256 but underruns from DSD512 onward; 1000 Mbit required for DSD1024).
- **IRQ affinity for the target NIC(s)**: New `IRQ_INTERFACE` / `IRQ_CPUS` config keys (also exposed in the web UI under "Advanced Network Settings") that pin all hardware interrupts of one or more NICs — including MSI-X queues — to a specific CPU list at service start. `IRQ_INTERFACE` accepts either a single name (e.g. `enp1s0`) or a comma-separated list (e.g. `enp1s0,enp2s0`) to cover hosts with separate NICs for the upstream source (LMS/Roon) and the Diretta target. Pairs naturally with `--cpu-audio` to keep network IRQ activity off the audio worker core, a known source of jitter on busy LANs. The launcher walks `/proc/interrupts`, applies the affinity to every IRQ matching any listed interface name, and logs a summary like `IRQ affinity for enp1s0,enp2s0 -> CPU(s) 0-5: 12 pinned, 2 skipped (managed/read-only)`. Kernel-managed IRQs that refuse runtime reassignment are reported as "skipped" without failure. Documented alongside an expanded section on `isolcpus=` kernel cmdline tuning in `docs/CONFIGURATION.md`.
- **SMT (Hyper-Threading) toggle at service start**: New `SMT` config key (also in the web UI under "CPU Affinity") accepting `on` / `off` / `forceoff` / empty (no change). The wrapper writes the chosen value to `/sys/devices/system/cpu/smt/control` before launching DRUP, so any subsequent `CPU_AUDIO` / `CPU_OTHER` pinning sees the right topology. Setting is system-wide and non-persistent across kernel reboots — the wrapper re-applies it on every service start. BIOS-level locks are detected and reported as a warning rather than a failure. The accompanying `.conf` and web UI text spell out the gotchas: the rest of the host shares this setting, and CPU lists referencing logical CPUs that disappear under SMT off must be reviewed.

---

## [2.3.0] - 2026-04-28

### Added
- **Multi-core CPU affinity** (`--cpu-audio`, `--cpu-other`): Both options now accept either a single core (e.g. `3`) or a comma-separated list (e.g. `3,4` or `6,7,8`). When multiple cores are specified, the kernel scheduler can move the thread within that set. Config file variables `CPU_AUDIO` and `CPU_OTHER` accept the same syntax. Single-core values remain fully compatible with previous versions. (Requested by Vlad)
- **Configurable buffers** (`--pcm-buffer-seconds`, `--pcm-remote-buffer-seconds`, `--dsd-buffer-seconds`, `--pcm-prefill-ms`, `--pcm-remote-prefill-ms`, `--dsd-prefill-ms`): All six buffer / prefill values are now exposable via CLI, config file (`PCM_BUFFER_SECONDS`, `PCM_REMOTE_BUFFER_SECONDS`, `DSD_BUFFER_SECONDS`, `PCM_PREFILL_MS`, `PCM_REMOTE_PREFILL_MS`, `DSD_PREFILL_MS`), and web UI under "Buffer Configuration (Advanced)". Leave empty to use defaults. Allows tuning latency vs stability for specific setups. (Requested by Vlad, previously planned on the roadmap)

### Fixed
- **Audirvana internet radio playback failure** (`Invalid sample_rate found in mime_type "audio/L16"`): Audirvana Studio relays internet radio streams as raw s16be PCM via Content-Type `audio/L16` but omits the mandatory `rate=` parameter (RFC 2586 violation). FFmpeg's `s16be` demuxer parses the HTTP Content-Type before applying user-supplied options, sees `audio/L16` without `rate=`, and returns `AVERROR_INVALIDDATA` — so simply forcing `sample_rate=44100` and `channels=2` had no effect. The fix detects Audirvana's specific URL pattern (`/audirvana/*.pcm`), opens the HTTP connection manually, and wraps it in a custom `AVIOContext` whose AVClass tree exposes no `mime_type` option. The demuxer's `av_opt_get(pb, "mime_type", AV_OPT_SEARCH_CHILDREN)` then returns NULL, the strict RFC 2586 check is skipped, and the demuxer falls through to the supplied 44100Hz/stereo defaults (RFC 3551 fallback). Channel layout is set via both `ch_layout=stereo` (FFmpeg ≥ 6.x) and the deprecated `channels=2` (older builds) since the PCM raw demuxer's default is "mono", which would otherwise cause stereo radio streams to play at half-speed. Also adds the `pcm_s16be` (and matching `pcm_s24be`/`pcm_s32be`) raw PCM demuxers to both FFmpeg build configurations in `install.sh`, since they were absent from the minimal build. Strictly scoped — has no effect on mp3/aac/ogg/flac internet radio (which already worked) or any other Audirvana flow (Qobuz/Tidal proxy, local files). **Users who built FFmpeg via `install.sh` need to recompile FFmpeg** to enable Audirvana raw PCM radio support. (Reported by grajaw)

---

## [2.2.3] - 2026-04-18

### Added
- **Web UI Stop button**: Added a Stop button alongside the existing Save & Restart and Restart Only buttons. Useful for users running DirettaRendererUPnP on their own Linux distributions to stop the service directly from the web UI — e.g., to release the Diretta target for another player or before maintenance. Includes a confirmation dialog.

### Fixed
- **CPU affinity: main and log drain threads not pinned**: When `--cpu-other` was set, the main thread and the log drain thread were not pinned to the specified core, allowing them to migrate to cores 0/1 and interfere with audio isolation. Now both are pinned to `cpuOther` alongside the other non-critical threads. (Reported by progman)

### Changed
- **Build system optimization** (PR #65 by sheviks): LDFLAGS now propagate `-O` and `-march` flags to the linker when LTO is enabled, ensuring whole-program analysis uses architecture-specific optimizations (AVX2/AVX-512/Zen4/NEON) instead of falling back to generic instructions. Also forces `lld` as the linker with Clang (`-fuse-ld=lld`) and unifies all C++ files to `-O3` (was `-O2`, while C files were already `-O3`). Applied to both the main binary and the FFmpeg minimal build in `install.sh`.

---

## [2.2.2] - 2026-04-11

### Added
- **Clang + LTO build support** (PR #64 by sheviks): The Makefile and `install.sh` now support building with Clang and Link-Time Optimization as an alternative to the default GCC build. Usage: `env LLVM=1 ./install.sh` or `make LLVM=1`. Clang+LTO may offer different performance and sound characteristics for users who compile from source. GCC remains the default.

### Fixed
- **32-bit 768kHz playlist advancement**: Streams served without Content-Length (e.g., slim2UPnP for high-rate PCM) would return `AVERROR(EIO)` at end-of-track instead of `AVERROR_EOF` because FFmpeg expected `UINT64_MAX` bytes but the stream closed mid-chunk. This prevented the playlist from advancing to the next track. Fix: if EIO occurs after successfully reading data (pos > 0), treat it as normal EOF. (Reported by abase)
- **Typo `clag++` → `clang++` in install.sh** (follow-up to PR #64): Small typo in the Clang support code that would have caused FFmpeg's configure step to fail with "compiler not found" when building with `LLVM=1`.

---

## [2.2.1] - 2026-04-11

### Changed
- **Larger PCM buffer for CDN resilience**: Increased remote streaming buffer to absorb Qobuz/Tidal CDN hiccups that affect most Diretta users. `PCM_REMOTE_BUFFER_SECONDS` raised from 1.0s to 3.0s (triple the buffer for CDN glitches), `PCM_REMOTE_PREFILL_MS` from 150ms to 500ms (larger initial buffer before playback). Added adaptive `REBUFFER_THRESHOLD_REMOTE_PCT` at 50% (vs 20% for local) — requires more data before resuming after an underrun to avoid stuttering cycles. For a 44.1/16/2 stream: buffer goes from 520KB to 1.5MB (3 seconds of audio), rebuffer threshold from ~200ms to ~1.5 seconds.

### Fixed
- **FFmpeg version detection in install.sh** (PR #63 by sheviks): The regex for detecting FFmpeg runtime version didn't handle the optional `n` prefix used by git-tagged builds (`ffmpeg version n8.1`), causing ABI compatibility checks to fail. Also added support for the new `version_major.h` header file introduced in recent FFmpeg releases, where major version macros were moved from `version.h` to a dedicated header. The script now searches both header variants for compatibility with legacy and modern FFmpeg installations.

---

## [2.2.0] - 2026-04-09

### Added
- **CPU affinity for audio thread isolation** (`--cpu-audio`, `--cpu-other`): Pin the Diretta worker thread and other threads (decode, UPnP, position) to dedicated CPU cores for reduced jitter and improved audio quality. When `--cpu-audio` is set, the SDK OCCUPIED flag is automatically enabled for hardware-level CPU pinning. Configurable via CLI, config file (`CPU_AUDIO`, `CPU_OTHER`), and web UI. Default: no pinning (current behavior preserved). (Requested by Daniel/Koala887)

### Fixed
- **Buffer underrun on long tracks from local UPnP sources**: FFmpeg HTTP buffer was 32KB with 10s timeout for local servers (slim2UPnP, JPLAY, etc.), causing underruns and premature track cutoff on long tracks (40+ minutes) when relaying Qobuz/Tidal streams. Now uses 256KB buffer and 30s timeout for all local servers. (Reported by Hoorna/Alfred, Dominique)
- **AIFF playback failure**: Added `aiff` demuxer and big-endian PCM decoders (`pcm_s16be`, `pcm_s24be`, `pcm_s32be`) to FFmpeg build configuration. Users who compiled FFmpeg via `install.sh` need to recompile for AIFF support. (Reported by Pascal)
- **CPU affinity core validation**: `--cpu-audio` and `--cpu-other` are now validated against the actual number of CPU cores on the system. Invalid core numbers are rejected with a warning and reset to no pinning. Also warns if both options are set to the same core (no isolation). (Suggested by Hoorna/Alfred)
- **Diretta worker thread not pinned to cpuAudio core**: The SDK's OCCUPIED mode with `cpuMain` doesn't reliably pin the worker thread on all platforms (confirmed on RPi 4). Now explicitly pins the worker thread via `pthread_setaffinity_np` in `startSyncWorker()`, in addition to the SDK parameter. (Reported by Hoorna/Alfred)
- **DSF files fail to play with MinimServer transcoding**: When MinimServer transcodes DSF to WAV (e.g., `stream.transcode=dsf:wav24;176`), the URL contains `.dsf` in the source path but ends with `.wav`. The format hint incorrectly forced FFmpeg's DSF demuxer on WAV data. Now checks only the last URL component's extension. (Reported by lithiumnk)

---

## [2.1.10] - 2026-04-06

### Fixed
- **AIFF playback failure** (`Invalid data found when processing input`): FFmpeg was compiled without the AIFF demuxer and big-endian PCM decoders. Added `aiff` demuxer and `pcm_s16be`, `pcm_s24be`, `pcm_s32be` decoders to both FFmpeg build configurations in `install.sh`. Users who compiled FFmpeg via `install.sh` (minimal configuration) need to recompile FFmpeg to enable AIFF support. (Reported by Pascal)

---

## [2.1.10] - 2026-04-06

### Changed
- **Config variable names aligned with CLI** (requested by Filippo/GentooPlayer): `RENDERER_NAME` → `NAME`, `NETWORK_INTERFACE` → `INTERFACE`, `MTU_OVERRIDE` → `MTU`. Enables simple automatic mapping (`KEY` → `--key`) for downstream integrations. Old names are still supported as fallback for backward compatibility.

---

## [2.1.9] - 2026-04-01

### Fixed
- **Cannot restart track from beginning while playing**: When a control point sends SetAVTransportURI with the same URI as the current track (to restart from beginning), the renderer incorrectly skipped the auto-stop ("Same URI already active") and then ignored the Play ("Already playing"). The track continued playing instead of restarting. Removed the same-URI shortcut — SetAVTransportURI now always performs auto-stop, allowing the track to reopen from the beginning.

---

## [2.1.8] - 2026-03-31

### Added
- **Minimal UPnP mode** (`--minimal-upnp`): Disables position thread polling and UPnP event notifications (LastChange NOTIFY) for reduced CPU overhead during playback. Improves audio quality (lower noise floor, more analog sound) by eliminating CPU wakeups during streaming. Recommended for JPlay iOS, LMS via slim2UPnP (fixes position bar drift), and Roon. Gapless playback, Play/Stop/Pause, and all audio functionality remain fully operational.

---

## [2.1.7] - 2026-03-29

### Fixed
- **UAPP GetPositionInfo response rejected by Cling parser**: The AVTransport SCPD declared only 5 output arguments for `GetPositionInfo` (Track, TrackDuration, TrackMetaData, TrackURI, RelTime) but the SOAP response returned 8 (including AbsTime, RelCount, AbsCount). Cling validates SOAP responses against the SCPD and silently rejects responses with undeclared arguments — causing UAPP to ignore position data entirely. Added the 3 missing arguments and their corresponding state variables (AbsoluteTimePosition, RelativeCounterPosition, AbsoluteCounterPosition) to the AVTransport SCPD.

---

## [2.1.6] - 2026-03-29

### Fixed
- **Service startup crash with IP-based NETWORK_INTERFACE**: `start-renderer.sh` passed `--bind-ip` when `NETWORK_INTERFACE` was an IP address (e.g., `192.168.1.32`), but the executable only accepts `--interface`. This caused `Unknown option: --bind-ip` and service failure on restart (reported by Pascal). `--interface` accepts both interface names and IP addresses via libupnp's `UpnpInit2`.

- **UAPP progress bar stuck**: The `Play` SOAP action handler executed the track-opening callback synchronously (FFmpeg init, DirettaSync open) before sending the HTTP 200 response, causing ~320ms latency. UAPP has a short internal timeout on PlayResponse and won't start its progress timer if the response is too slow. Fix: `onPlay` callback is now launched asynchronously so the HTTP 200 is returned immediately (< 50ms). Other control points (mConnect, BubbleUPnP, Audirvana) are unaffected.

---

## [2.1.5] - 2026-03-27

### Fixed

- **Silence on 16-bit and 24-bit content with some DACs**: `configureSinkPCM()` always tried 32-bit negotiation first, regardless of the source bit depth. DACs that report 32-bit support but are physically limited to 24-bit would produce silence or noise for 16-bit and 24-bit content. Now only offers 32-bit when the source is actually 32-bit. (Reported by PatrickW, matching fix from slim2diretta v1.2.2)

- **Worker thread join timeout in startSyncWorker**: Last remaining bare `m_workerThread.join()` in `startSyncWorker()` could block indefinitely if the SDK worker was unresponsive during format transitions. Now uses `joinWorkerWithTimeout(1000ms)` matching all other join sites. (Matching fix from slim2diretta v1.2.4, reported by Jeep972)

- **Extended stabilization on first Diretta target connect**: Added longer stabilization delay on initial SDK connection to prevent audio glitches at startup.

- **First-play glitch (~5s silence)**: Pre-connect Diretta pipeline at startup with default format (44100/24/2 PCM). The first real play now uses quick resume instead of cold connect, eliminating the silence gap reported with LMS (via slim2UPnP) and Roon.

- **White noise after track change with Audirvana** (by herisson-88): Anticipated preload opened a second AudioDecoder in parallel, causing FFmpeg to read up to 5MB (`probesize` default) from Audirvana's HTTP server concurrently with the active stream. Audirvana's embedded server doesn't handle concurrent reads well, corrupting the active stream data → permanent white noise ~4 seconds after track change. Fix: limit `probesize` to 32KB and `max_analyze_duration` to 0 for local servers. (PR #61)

- **UAPP position tracking still broken after v2.1.1 namespace fix**: The `u:` namespace prefix fix allowed UAPP's strict Cling parser to read the SOAP response envelope, but it then crashed parsing the time values. `RelTime`/`AbsTime` contained milliseconds (`00:00:01.407`) which strict parsers don't support. Now uses `HH:MM:SS` format without fractional seconds.

---

## [2.1.4] - 2026-03-16

### Fixed

- **Audirvana link-local stream misdetected as remote**: Audirvana Studio on some setups uses link-local addresses (`169.254.x.x`) for its HTTP audio server. These were not recognized as local servers, causing DirettaRendererUPnP to enable HTTP reconnection options (`reconnect=1`, `reconnect_streamed=1`) and larger remote buffers. Audirvana's local HTTP server doesn't support these options, leading to playback interruptions, white noise, and track advancement failures.

- **UPnP server startup failure on boot**: On systems without systemd network-online dependency (e.g., GentooPlayer with OpenRC), `UpnpInit2` could fail if the network interface wasn't ready yet. The UPnP initialization now retries every 2 seconds (with status logged every 5 seconds) until the network is available, matching the resilient target discovery behavior.

---

## [2.1.3] - 2026-03-15

### Fixed

- **Target retry loop not working**: v2.1.2 introduced resilient target discovery, but `DirettaRenderer::start()` had a pre-check (`verifyTargetAvailable()`) that exited immediately before the retry loop was reached. The retry now works as intended.

---

## [2.1.2] - 2026-03-15

### Added

- **Resilient target discovery**: When the Diretta target is not available at startup, the renderer now retries every 2 seconds (with status logged every 5 seconds) instead of exiting immediately. This is especially important on systems without systemd auto-restart (e.g., GentooPlayer with OpenRC). (Suggested by Filippo/GentooPlayer)

---

## [2.1.1] - 2026-03-10

### Fixed

- **UAPP (USB Audio Player Pro) SOAP response compatibility**: Added `u:` namespace prefix on SOAP action response root elements to match the format produced by libupnp's `UpnpMakeActionResponse`. Strict XML parsers like Cling (used by UAPP on Android) silently rejected our responses, causing GetPositionInfo callbacks to never fire — UAPP couldn't track position or advance to the next track. Lenient parsers (Audirvana, BubbleUPnP, mconnect) were unaffected.

- **Audirvana Studio format change crash**: Fixed race condition during rapid PCM format transitions (rate/bitdepth changes) that could cause crashes or hangs. Three interrelated fixes:
  - Timed worker thread join (1s timeout) prevents indefinite blocking when SDK is unresponsive
  - Lifecycle mutex (`m_lifecycleMutex`) prevents concurrent `open()`/`stopPlayback()`/`close()` calls from corrupting DirettaSync state
  - Interruptible `open()` via abort flag — when a stop is requested during a format transition, `open()` aborts early instead of completing a stale format change

- **High sample rate buffer underruns (>192kHz)**: Adaptive buffer sizing for sample rates above 192kHz (352.8kHz, 384kHz, 768kHz, 1536kHz). Source streams at ~1x real-time at these rates, leaving no margin with the previous 0.5s ring buffer. New behavior:
  - Ring buffer: 0.5s → 2.0s for rates >192kHz (takes precedence over remote 1.0s)
  - SDK prefill: 1000ms for rates >192kHz (vs 80-150ms)
  - MAX_BUFFER raised to 32MB (accommodates 1536kHz/32bit/2ch @ 2s)
  - No change for rates ≤192kHz (identical behavior to v2.1.0)

### Added

- **Build capabilities log at startup**: Displays architecture (x86_64/aarch64/arm) and SIMD support (AVX2/NEON/scalar) for easier remote diagnostics

---

## [2.1.0] - 2026-03-06

### ✨ New Features

**Web Configuration UI (diretta-webui):**
- Browser-based settings interface — no SSH needed to configure the renderer
- Accessible at `http://<ip>:8080` via a lightweight Python HTTP server
- Edit all renderer settings: target, port, gapless, verbose, network interface
- Advanced Diretta SDK settings: thread-mode, transfer-mode, cycle-time, info-cycle, target-profile-limit, MTU
- Save & Restart: applies settings and restarts the systemd service in one click
- Zero dependencies beyond Python 3 (stdlib only)
- Separate systemd service (`diretta-renderer-webui.service`) — transparent for audio quality
- Profile-based architecture: reusable for other Diretta projects (slim2diretta)
- Installable via `install.sh` option 6 or `./install.sh --webui`

**Configurable Process Priority (Nice/IOScheduling/SCHED_FIFO):**
- Process priority settings (`NICE_LEVEL`, `IO_SCHED_CLASS`, `IO_SCHED_PRIORITY`, `RT_PRIORITY`) now configurable via `/etc/default/diretta-renderer`
- `RT_PRIORITY` (1-99): SCHED_FIFO real-time priority for the audio worker thread (default: 50, was hardcoded)
- `--rt-priority <1-99>` CLI argument for direct control
- Removed hardcoded `Nice=-10` and `IOSchedulingClass=realtime` from the systemd service file
- Priority is applied by `start-renderer.sh` wrapper script via `nice` and `ionice` commands
- Adjustable through the web UI under the "Process Priority" group
- Defaults unchanged: nice -10, realtime I/O class, I/O priority 0, RT priority 50
- Same feature added to slim2diretta with `start-slim2diretta.sh` wrapper script

**Advanced Diretta SDK Settings Exposed via CLI:**
- `--thread-mode <mode>`: SDK thread mode bitmask (CRITICAL, NOSHORTSLEEP, SOCKETNOBLOCK, OCCUPIED, etc.)
- `--cycle-time <us>`: Max packet transmission cycle time in microseconds (disables auto-calculation)
- `--cycle-min-time <us>`: Min cycle time in microseconds (random mode only)
- `--info-cycle <us>`: Info packet cycle time (default: 100000µs = 100ms)
- `--transfer-mode <mode>`: Transfer mode (auto, varmax, varauto, fixauto, random)
- `--target-profile-limit <us>`: Target profile limit time (0=SelfProfile (stable), default: 0, >0=TargetProfile with auto-adaptation (experimental))
- `--mtu <bytes>`: MTU override (skip auto-detection)
- These options were available in v1.3.3 and have been reintroduced with the new DirettaSync architecture
- TargetProfile mode uses SDK `getProfileMaker()` for target-adaptive transmission profiles
- Refactored SDK `open()` calls into a single `openSDK()` helper to eliminate code duplication

**Configuration File Moved to `/etc/default/diretta-renderer`:**
- Config file relocated from `/opt/diretta-renderer-upnp/diretta-renderer.conf` to `/etc/default/diretta-renderer`
- Follows standard Linux convention (`/etc/default/` for service configuration)
- Fixes `Read-only file system` error on machines with read-only `/opt` partition
- Existing installations are automatically migrated: old config backed up, settings preserved
- Web UI can now save settings on all system configurations

**Automatic Configuration Migration on Upgrade:**
- When upgrading, `install.sh` automatically migrates settings from old location to `/etc/default/diretta-renderer`
- Old config is backed up as `diretta-renderer.conf.bak`
- User settings (TARGET, PORT, NETWORK_INTERFACE, etc.) are preserved and applied to the new file
- New options (SDK settings) appear with their default values, ready to customize
- Obsolete settings (e.g., `DROP_USER` from v2.0.5) are detected and reported

### 🐛 Bug Fixes

**UAPP (USB Audio Player Pro) Position Tracking Compatibility:**
- GetPositionInfo now returns real-time position with sub-second precision (`HH:MM:SS.FFF`)
- Previously returned `00:00:00` on first poll because position thread (1s update interval) hadn't updated yet
- UAPP polls only once and stopped tracking position when it received `00:00:00`
- Position is now computed directly from AudioEngine via callback, bypassing the cached value

**Audirvana Gapless Track Replay Fix (PR #60 by herisson-88):**
- Fixed race condition in `onSetURI` where split mutex lock allowed `onPlay` to read stale URI between auto-stop and URI update — Audirvana sends commands on separate HTTP connections, triggering the race consistently
- Rewrote `preloadNextTrack()` with thread-safe capture-validate-commit pattern: snapshot URI under lock, open decoder without lock, revalidate before commit
- Added stale preload detection: discards decoder when `m_nextURI` changes during loading
- Rejects same-URI `SetNextAVTransportURI` (Audirvana quirk that caused previous track replay)
- Added `onPlay` already-playing guard per UPnP AVTransport spec
- Syncs `DirettaRenderer::m_currentURI` during gapless transitions via `trackChangeCallback`

**Stop Action Uses stopPlayback() Instead of close() (fix by herisson-88):**
- Changed UPnP Stop handler from `close()` to `stopPlayback(false)` in DirettaSync
- Keeps SDK connection open for faster "quick resume" path on next Play
- Prevents intermittent white noise on hi-res track transitions caused by target (e.g., Holo Red) failing to resync after SDK reopen

**Auto-Detect libupnp Include Path:**
- Makefile now uses `pkg-config --cflags libupnp` to detect the correct include path
- Falls back to standard path detection if pkg-config is not available
- Fixes compilation on systems where libupnp headers are in non-standard locations (e.g., GentooPlayer on RPi4)

### 🗑️ Removed

**Privilege Drop (`--user` / `DROP_USER`) Removed:**
- Removed `--user` / `-u` command-line option and `DROP_USER` configuration setting
- Removed `PrivilegeDrop.h` module
- All users run dedicated audio machines where privilege isolation provides no benefit
- Running as root guarantees SCHED_FIFO real-time priority on worker threads — a bug in capability inheritance caused worker threads to lose SCHED_FIFO when dropping to an unprivileged user, resulting in degraded audio quality
- Systemd service simplified: removed `CAP_SETUID`/`CAP_SETGID` from `AmbientCapabilities` and `CapabilityBoundingSet`

---

## [2.0.4] - 2026-02-24

### ✨ New Features

**Centralized Log Level System:**
- New `LogLevel.h` header with 4 levels: ERROR, WARN, INFO, DEBUG
- `--quiet` (`-q`) option: show only warnings and errors (WARN level)
- `--verbose` continues to work as before (DEBUG level)
- Default level (INFO) produces the same output as v2.0.3
- All source files migrated from per-file `DEBUG_LOG` macros to unified `LOG_DEBUG`/`LOG_INFO`/`LOG_WARN`/`LOG_ERROR`
- `NOLOG` builds now only disable SDK internal logging (`DIRETTA_LOG`); application `LOG_*` macros remain active with runtime level control, so `--verbose` and `--quiet` work correctly in production builds

**Runtime Statistics via SIGUSR1:**
- Send `kill -USR1 <pid>` to dump live statistics to stdout
- Shows: playback state, current format, buffer fill level, MTU, stream/push/underrun counters
- Useful for monitoring production systems via systemd journal

**MS Mode Negotiation Logging (feature request by Alfred):**
- Verbose log now shows the MS mode negotiated with the Diretta Target
- From second track onwards: supported modes, requested mode, and negotiated mode
- First track: clear message that MS info becomes available after first connection
- Uses "negotiated" wording to clarify the mode is inferred from AUTO algorithm + target capabilities

**Rebuffering on Underrun (streaming resilience):**
- When the ring buffer empties during a network stall (e.g., Tidal/Qobuz streaming), small data bursts were immediately consumed, creating a rapid silence/audio alternation ("CD skip" effect)
- Now enters rebuffering mode on underrun: holds silence until the buffer refills to 20%
- Result: clean silence gap followed by smooth playback resumption instead of stuttering
- Rebuffering events logged at WARN level, visible in all builds (including `NOLOG=1` production builds and `--quiet` mode)

### ⚡ Performance

**Zero-Allocation Streaming Detection:**
- Replaced `std::string` + `std::transform` with POSIX `strcasestr()` for Qobuz/Tidal URL detection
- Eliminates heap allocation on every `openSource()` call

### 🐛 Bug Fixes

**FFmpeg DSD Streaming Error Handling:**
- Added handling for `AVERROR(ETIMEDOUT)`, `AVERROR(ECONNRESET)`, and `AVERROR_EXIT` in DSD read loop
- Generic fallback with `av_strerror()` for unexpected error codes
- Prevents silent hangs on network interruptions during DSD streaming

**Atomic Ordering Fix in RingAccessGuard:**
- Changed `fetch_add` from `memory_order_acquire` to `memory_order_acq_rel`
- Ensures the increment is visible to the reconfiguration thread on all architectures (ARM64)

**Stop Action Log Noise Reduction:**
- Redundant stop requests from control points now log at DEBUG level instead of INFO
- Actual stop actions still show a clear banner at INFO level
- Reduces log clutter when control points send multiple Stop actions (normal UPnP behavior)

### 🔧 Build & Configuration

**Production Build in install.sh:**
- `install.sh` now builds with `NOLOG=1` by default (disables SDK internal logging)
- Application-level logging (`--verbose`/`--quiet`) remains fully functional

**Updated systemd Configuration:**
- `diretta-renderer.conf`: documented `--quiet` option alongside `--verbose`
- `start-renderer.sh`: updated comments for log verbosity options

**Privilege Drop (`--user`):**
- New `--user, -u <name>` option to drop root privileges after network initialization
- Uses Linux-native `prctl(PR_SET_KEEPCAPS)` + `capset()` syscall — no libcap dependency
- Retains `CAP_NET_RAW`, `CAP_NET_ADMIN`, `CAP_SYS_NICE` capabilities after dropping to unprivileged user
- Non-fatal fallback: if `capset()` fails, logs a warning and continues with reduced capabilities

**Systemd Hardening:**
- 20+ security directives added to `diretta-renderer.service`
- Filesystem isolation: `ProtectSystem=strict`, `ProtectHome=true`, `PrivateTmp=true`
- Kernel protection: `ProtectKernelTunables`, `ProtectKernelModules`, `ProtectKernelLogs`
- Syscall filtering: blocks `@mount`, `@keyring`, `@debug`, `@module`, `@swap`, `@reboot`, `@obsolete`
- Dedicated `diretta` system user created by `install-systemd.sh`
- `CapabilityBoundingSet` limits to `CAP_NET_RAW CAP_NET_ADMIN CAP_SYS_NICE`

### 🏗️ ARM Architecture

**ARM NEON SIMD Format Conversions:**
- Hand-optimized NEON intrinsics for all PCM and DSD format conversions on ARM64
- PCM: `convert24BitPacked` (LSB/MSB), `convert16To32` using `vzip`/`vshrn`/`vmovn` intrinsics
- DSD: all 4 conversion modes (Passthrough, BitReverse, ByteSwap, BitReverseSwap) using `vzip1q_u32`/`vzip2q_u32` interleaving
- Bit reversal via `vqtbl1q_u8` LUT-based nibble swap, byte swap via `vrev32q_u8`
- Automatic detection via `DIRETTA_HAS_NEON` macro (`__aarch64__` + `__ARM_NEON`)
- Fallback to scalar code when NEON is not available

### 🧪 Testing

**Unit Test Suite (20 tests):**
- Comprehensive test suite for `DirettaRingBuffer` format conversions
- 3 memory infrastructure tests (memcpy correctness, timing variance, buffer alignment)
- 6 PCM conversion tests (24-bit pack LSB/MSB, 16→32, 16→24, single-sample edge cases)
- 5 DSD conversion tests (all 4 modes + small input scalar path)
- 4 ring buffer tests (wraparound, power-of-2 sizing, full buffer, empty pop)
- 2 integration tests (push24BitPacked→pop, pushDSDPlanarOptimized→pop)
- Run with `make test` — zero external dependencies

---

## [2.0.3] - 2026-02-15

### 🐛 Bug Fixes

**UPnP Event Deduplication (Audirvana compatibility):**
- Removed duplicate GENA events that caused progress bar hiccups on Audirvana
- Each UPnP action (Play, Pause, Stop, SetNextAVTransportURI) now sends exactly one `LastChange` event
- Root cause: action handlers called `sendAVTransportEvent()` redundantly — the DirettaRenderer callbacks already send the event via `notifyStateChange()`
- **Play**: Fixed by **herisson-88** ([PR #53](https://github.com/cometdom/DirettaRendererUPnP/pull/53))
- **SetAVTransportURI**: Removed duplicate `STOPPED` event during track-to-track transitions (auto-stop callback already sends it)
- **SetNextAVTransportURI**: Removed spurious event that triggered re-synchronization during gapless queueing
- **Pause**: Removed duplicate `PAUSED_PLAYBACK` event
- **Stop**: Removed duplicate `STOPPED` event
- Fixes progress bar stuttering/jumping in Audirvana and other control points that react to duplicate state notifications

**Format Change Preload Guard (squeeze2UPnP/LMS fix):**
- Prevented repeated preloading of the same next track during format changes (e.g., bit depth or sample rate change between tracks)
- The anticipated preload (from `SetNextAVTransportURI`) and EOF preload were not coordinated: when a format change was detected, the EOF path would re-open the same URL 2-3 additional times
- Added `m_formatChangePending` flag to skip redundant preloads once a format change transition is already scheduled
- Reduces unnecessary HTTP connections, especially beneficial for squeeze2UPnP/LMS setups that use ephemeral ports per track

**Adaptive Buffer for Remote Streaming (Qobuz/Tidal):**
- Ring buffer now adapts based on source type: 1.0s for internet streaming vs 0.5s for local playback
- Prefill increased to 150ms for remote sources (vs 80ms local) to absorb network latency
- Source type detection (local/remote) propagated from AudioEngine to DirettaSync via `isRemoteStream` flag
- Reduces underruns caused by CDN reconnections (e.g., Akamai dropping HTTP connections mid-stream)

**libupnp Callback Compatibility:**
- Fixed compilation error with libupnp <= 1.14.25 where `Upnp_FunPtr` Event parameter is `void*` instead of `const void*` (changed in 1.14.26)
- Compile-time detection of the callback signature using C++17 template type deduction
- Builds correctly with all libupnp 1.14.x versions without manual configuration

**Crash on Startup Failure (verbose mode):**
- Fixed `std::terminate()` crash when renderer fails to start with `--verbose` enabled
- Root cause: async log drain thread was not joined before `return 1`, causing `std::thread::~thread()` to call `std::terminate()` on a joinable thread
- Extracted cleanup into `shutdownAsyncLogging()` called at all exit paths (start failure, exception, signal handler, normal exit)
- Observed in the wild: `UpnpInit2 failed: -203` at boot → crash → systemd auto-restart

### ✅ Compatibility

**Audirvana (macOS/Windows):**
- Full compatibility confirmed with gapless PCM and DSD playback
- The "Universal Gapless" option in Audirvana is **no longer needed** and should be disabled
- DirettaRendererUPnP handles gapless transitions natively via `SetNextAVTransportURI`

---

## [2.0.2] - 2026-02-09

### ✨ New Features

**DSDIFF/DFF Native Playback (Audirvana DSD support):**
- Built-in DSDIFF container parser - FFmpeg has no DSDIFF demuxer, so DFF files were completely unplayable
- Uses FFmpeg's `avio` for HTTP I/O while parsing the DSDIFF container manually
- Parses FRM8 header, PROP chunk (sample rate, channels, compression type), and DSD data chunk
- Supports uncompressed DSD (rejects DST-compressed DSDIFF)
- Byte de-interleaves DFF data (L R L R...) to planar format ([all L][all R]) expected by the ring buffer
- DFF data stays MSB-first; DirettaRingBuffer handles bit conversion for the target
- Enables native DSD playback from Audirvana, which converts DSF files to DFF when streaming via UPnP
- Tested with DSD64 from Audirvana

**UPnP Event Notifications (progress bar fix):**
- Implemented proper UPnP GENA eventing with `UpnpNotify()` and `LastChange` XML
- Control points (Audirvana, BubbleUPnP, mConnect) now receive real-time notifications for:
  - Transport state changes (PLAYING, STOPPED, PAUSED)
  - Track changes (URI, duration, metadata)
  - Position updates
- Implemented `UpnpAcceptSubscription()` to send initial state on new subscriptions
- XML-escaped metadata values to prevent malformed events
- Fixes progress bar not updating on track transitions in control points

### ⚡ Performance

**Anticipated Gapless Preload (3-tier architecture fix):**
- HTTP connection for the next track now opens immediately when `SetNextAVTransportURI` is received
- Previously, the connection opened at EOF, causing buffer underruns (~0.2%) during the HTTP handshake
- Preload runs in a background thread while the current track plays, giving several seconds of headroom
- Fixes audio glitches during gapless transitions, especially on 3-tier Diretta setups (Host + Target on separate devices)
- Thanks to the user who reported this issue with Pi-5 Host/Target configuration

**Atomic Gapless Transition (Audirvana UI fix attempt):**
- Added `notifyGaplessTransition()` for atomic track data update + event sending
- Epoch counter prevents position thread from overwriting fresh track data with stale values
- Addresses race condition between 1-second position polling and track change callback

### 🐛 Bug Fixes

**Local vs Remote Server HTTP Options:**
- Detect local servers (192.168.x, 10.x, 172.x, localhost) and use simplified HTTP options
- Remote servers (Qobuz, Tidal) keep full reconnection/persistent options
- Fixes connection issues with local UPnP servers (Audirvana, JRiver) that don't support advanced HTTP features

**Streaming Proxy Detection (Qobuz/Tidal via local UPnP servers):**
- When a control point (e.g. Audirvana) proxies Qobuz/Tidal streams, the URL has a local IP (192.168.x.x) but the content comes from a remote streaming service
- The local/remote server detection now checks for streaming service names in the URL (case-insensitive)
- Proxied streams correctly use robust HTTP options (reconnect, http_persistent, ignore_eof) instead of simple local options
- Fixes potential connection drops when streaming Qobuz/Tidal through Audirvana or similar proxying control points
- Contributed by **herisson-88** ([PR #51](https://github.com/cometdom/DirettaRendererUPnP/pull/51))

**Premature Track Stop During Playlist Transitions:**
- Position reported to control points was ahead of DAC output by ~300ms (decoded vs played samples)
- Integer truncation caused `RelTime >= TrackDuration` before the track actually finished
- Some control points (mConnect) sent STOP prematurely, cutting audio on ~20% of transitions
- Fix: cap reported position to `duration - 1` while PLAYING; track end is signaled via `TransportState=STOPPED`
- Contributed by **herisson-88** ([PR #52](https://github.com/cometdom/DirettaRendererUPnP/pull/52))

**Ring Buffer Drain on Natural Track End:**
- `stopPlayback(true)` discarded ~75-150ms of buffered audio at end of track
- Now waits for ring buffer to drain below 1% before stopping (poll every 5ms, 2s timeout)
- Uses `stopPlayback(false)` for clean silence tail to DAC
- Contributed by **herisson-88** ([PR #52](https://github.com/cometdom/DirettaRendererUPnP/pull/52))

**Streaming Buffer Size for Remote Servers:**
- Increased FFmpeg HTTP buffer from 32KB to 512KB for remote streams (Qobuz, Tidal)
- Absorbs network jitter that caused intermittent micro-dropouts during streaming
- Local server buffer unchanged at 32KB (LAN is reliable)

---

## [2.0.1] - 2026-01-28

### 🐛 Bug Fixes

**24-bit Audio White Noise Fix (TEAC UD-701N and similar DACs):**
- Fixed white noise when playing 24-bit audio on DACs that only support 24-bit (not 32-bit)
- Root cause: S24 alignment hint was incorrectly set to LSB-aligned instead of MSB-aligned
- FFmpeg decodes 24-bit content into S32 format where audio data is in the upper 24 bits
- The v2.0.0 ring buffer extracted the wrong bytes (lower 24 bits including padding)
- Now correctly extracts bytes 1-3 (MSB-aligned) instead of bytes 0-2 (LSB-aligned)
- Affected: x86/x64 platforms with 24-bit-only DACs (ARM64 had a workaround)
- Thanks to the user who reported this issue with the TEAC UD-701N

---

## [2.0.0] - 2026-01-28

### 🚀 Complete Architecture Rewrite

Version 2.0.0 is a **complete rewrite** of DirettaRendererUPnP focused on low-latency and jitter reduction. It uses the Diretta SDK (by **Yu Harada**) at a lower level (`DIRETTA::Sync` instead of `DIRETTA::SyncBuffer`) for finer timing control, with core Diretta integration code contributed by **SwissMountainsBear** (ported from his MPD Diretta Output Plugin), and incorporating advanced optimizations from **leeeanh**.

**SDK Changes:**
- Inherits `DIRETTA::Sync` directly (pull model with `getNewStream()` callback)
- Requires SDK version 148 with application-managed memory
- Full control over buffer timing and format transitions

### ⚡ Performance Improvements

| Metric | v1.x | v2.0 | Improvement |
|--------|------|------|-------------|
| PCM buffer latency | ~1000ms | ~300ms | **70% reduction** |
| Time to first audio | ~50ms | ~30ms | **40% faster** |
| Jitter (DSD flow control) | ±2.5ms | ±50µs | **50× reduction** |
| Ring buffer operations | 10-20 cycles | 1 cycle | **10-20× faster** |
| 24-bit conversion | ~1 sample/cycle | ~8 samples/cycle | **8× faster** |
| DSD interleave | ~1 byte/cycle | ~32 bytes/cycle | **32× faster** |

**Key Optimizations:**
- Lock-free SPSC ring buffer with power-of-2 bitmask modulo
- Cache-line separated atomics (`alignas(64)`) to eliminate false sharing
- AVX2 SIMD format conversions (24-bit pack, 16→32 upsample, DSD interleave)
- Zero heap allocations in audio hot path (pre-allocated buffers)
- Condition variable flow control (500µs timeout vs 5ms blocking sleep)
- Worker thread SCHED_FIFO priority 50 for reduced scheduling jitter
- Generation counter caching (1 atomic load vs 5-6 per call)

### ✨ New Features

**PCM Bypass Mode:**
- Direct path for bit-perfect playback when formats match exactly
- Skips SwrContext for zero-processing audio path
- Log message: `[AudioDecoder] PCM BYPASS enabled - bit-perfect path`

**DSD Conversion Specialization:**
- 4 specialized functions selected at track open (no per-iteration branches):
  - `Passthrough` - Just interleave (fastest)
  - `BitReverseOnly` - Apply bit reversal
  - `ByteSwapOnly` - Endianness conversion
  - `BitReverseAndSwap` - Both operations

**Timestamped Logging:**
- All console output now includes `[HH:MM:SS.mmm]` timestamps
- Easier log analysis for diagnosing timing issues

**Enhanced Target Listing:**
- `--list-targets` shows output name, port numbers, SDK version, product ID

**Production Build:**
- `make NOLOG=1` completely removes all logging code for zero overhead

### 🐛 Bug Fixes

**High Sample Rate Stuttering Fix:**
- Fixed stuttering at >96kHz (192kHz, 352.8kHz, 384kHz)
- Root cause: `bytesPerBuffer` vs SDK cycle time mismatch (~4% data deficit)
- Solution: Synchronized buffer sizing with `DirettaCycleCalculator`

**MTU Overhead Fix (thanks to Hoorna):**
- Fixed stuttering on networks with MTU 1500 (standard Ethernet)
- Root cause: SDK's `m_effectiveMTU` already accounts for IP/UDP headers
- Original OVERHEAD=24 was too high, causing unnecessarily small packets
- Solution: Changed OVERHEAD from 24 to 3 (Diretta protocol overhead only)
- Tested: OVERHEAD=3 works at MTU 1500, OVERHEAD=2 causes stuttering

**16-bit Audio Segfault Fix (thanks to SwissMountainsBear):**
- Fixed crash when playing 16-bit audio on 24-bit-only sinks
- Root cause: Missing conversion path for 16-bit input to 24-bit sink
- Code calculated bytesPerFrame using sink's 3 bytes but input only had 2 bytes
- Solution: Added `push16To24()` and `convert16To24()` conversion functions

**AVX2 Detection Fix:**
- Fixed crash on older CPUs without AVX2 (Sandy Bridge, Ivy Bridge)
- Root cause: Code assumed all x86/x64 CPUs have AVX2
- Solution: Use compiler-defined `__AVX2__` macro for proper detection
- CPUs without AVX2 now correctly use scalar implementations

**S24 Detection Fix (ARM64 distortion):**
- Fixed audio distortion on 24-bit playback on ARM64 platforms (RPi4, RPi5, etc.)
- Root cause: FFmpeg on ARM64 outputs S24 samples in MSB-aligned format (byte 0 = padding)
- x86 FFmpeg outputs LSB-aligned format (byte 3 = padding)
- Solution: Force MSB-aligned extraction on ARM64 platforms
- Diagnostic: `[00 XX XX XX]` pattern = MSB (ARM), `[XX XX XX 00]` = LSB (x86)

**SDK 148 Track Change Fix:**
- Application-managed memory pattern for `getNewStream(diretta_stream&)`
- Persistent buffer with direct C structure field assignment
- Fixes segmentation faults during track changes

**DSD→PCM Transition Noise:**
- Full `close()` + 800ms delay + fresh `open()` for clean I2S target transitions
- Pre-transition silence buffers (rate-scaled) flush Diretta pipeline

**DSD Rate Change Noise:**
- All DSD rate changes now use full close/reopen (not just downgrades)
- Includes clock domain changes (44.1kHz ↔ 48kHz families)

**PCM Rate Change Noise:**
- PCM rate changes now use full close/reopen approach (200ms delay)
- Previously tried to send silence but playback was already stopped

**PCM 8fs Runtime Format Fix:**
- Runtime verification of frame format in bypass path
- Auto-fallback to resampler if format mismatch detected mid-stream

**FLAC Bypass Bug:**
- Compressed formats correctly excluded from bypass mode
- FLAC always decodes to planar format requiring SwrContext

**44.1kHz Family Drift Fix:**
- Bresenham-style accumulator for fractional frame tracking
- Eliminates gradual underruns from rounding errors

**DSD512 Zen3 Warmup:**
- MTU-aware stabilization buffer scaling
- Consistent warmup TIME regardless of MTU (400ms for DSD512)

**Playlist End Target Release:**
- `release()` function properly disconnects target when playlist ends
- Target can accept connections from other sources

**UPnP Stop Handling:**
- Diretta connection properly closed on UPnP Stop action

### 🔧 Tools & Scripts

**CPU Tuner Auto-Detection:**
- Tuner scripts now auto-detect CPU topology (AMD and Intel)
- Support for any number of cores with/without SMT
- New `detect` command to preview configuration before applying
- Dynamic allocation of housekeeping and renderer CPUs
- Tested with Ryzen 5/7/9 and Intel Core processors
- Clean handoff when switching renderers

### 📦 Installation

**New unified `install.sh` script:**
```bash
chmod +x install.sh
./install.sh
```

**Interactive menu options:**
1. Full installation (dependencies, FFmpeg, build, systemd)
2. Install dependencies only
3. Build only
4. Install systemd service only
5. Configure network only
6. Aggressive Fedora optimization (dedicated servers only)

**Command-line options:**
- `--full` - Full installation
- `--deps` - Dependencies only
- `--build` - Build only
- `--service, -s` - Install systemd service
- `--network, -n` - Configure network

### 🔧 Build System

**FFmpeg Version Detection:**
- Automatic header/library version mismatch detection
- Clear error if compile-time vs runtime versions differ
- Options: `FFMPEG_PATH`, `FFMPEG_LIB_PATH`, `FFMPEG_IGNORE_MISMATCH`

**Architecture Auto-Detection:**
- Automatically selects optimal SDK library variant
- x64: v2 (baseline), v3 (AVX2), v4 (AVX-512), zen4
- ARM64: Standard (4KB pages), k16 (16KB pages for Pi 5)

### 📚 Documentation

- Comprehensive `README.md` for v2.0
- `CLAUDE.md` project brief for contributors
- Technical documentation in `docs/`:
  - `PCM_FIFO_BYPASS_OPTIMIZATION.md`
  - `DSD_CONVERSION_OPTIMIZATION.md`
  - `DSD_BUFFER_OPTIMIZATION.md`
  - `SIMD_OPTIMIZATION_CHANGES.md`
  - `Timing_Variance_Optimization_Report.md`

### 🙏 Credits

- **Yu Harada** - Diretta SDK guidance and `DIRETTA::Sync` API recommendations

#### Key Contributors

- **SwissMountainsBear** - Ported and adapted the core Diretta integration code from his [MPD Diretta Output Plugin](https://github.com/swissmountainsbear/mpd-diretta-output-plugin). The `DIRETTA::Sync` architecture, `getNewStream()` callback implementation, same-format fast path, and buffer management patterns were directly contributed from his plugin. This project would not exist in its current form without his code contribution.

- **leeeanh** - Brilliant optimization strategies that made v2.0 a true low-latency solution:
  - Lock-free SPSC ring buffer design with atomic operations
  - Power-of-2 buffer sizing with bitmask modulo (10-20× faster than division)
  - Cache-line separation (`alignas(64)`) eliminating false sharing
  - Consumer hot path analysis leading to zero heap allocations
  - AVX2 SIMD batch conversion strategy (8-32× throughput improvement)
  - Condition variable flow control replacing blocking sleeps

---

## [1.3.3]

### 🐛 Bug Fixes

**Fixed:** Random playback failure when skipping tracks ("zapping")

Some users experienced an issue where skipping from one track to another would result in no audio playback, even though the progress bar in the UPnP control app continued to advance. Stopping and restarting playback would fix the issue.

**Root causes identified and fixed:**

1. **Play state notification without verification**
   - The UPnP controller was notified "PLAYING" even when the decoder failed to open
   - Now properly checks `AudioEngine::play()` return value before notifying
   - If playback fails, controller is notified "STOPPED" instead

2. **DAC stabilization delay skipped after Auto-STOP**
   - When changing tracks during playback, an "Auto-STOP" is triggered for JPlay iOS compatibility
   - The DAC stabilization delay timer (`lastStopTime`) was not updated during Auto-STOP
   - This could cause the next playback to start before the DAC was ready
   - Now properly records stop time in both manual Stop and Auto-STOP scenarios

**Impact:** More reliable track skipping, especially with rapid navigation through playlists.

---

## [1.3.2]

### 🐛 Bug Fixes

**Fixed:** DSD gapless playback on standard networks (MTU 1500)

If you experienced glitches between DSD tracks, this fixes it!
Works on any network equipment, no configuration needed.

---

## [1.3.1]

### 🐛 Bug Fixes

**Critical:** Fixed freeze after pause >10-20 seconds
- Root cause: Drainage state machine not reset on resume
- Solution: Reset m_isDraining and m_silenceCount flags
- Affects: GentooPlayer and other distributions

### ✨ New Features

**Timestamps:** Automatic [HH:MM:SS.mmm] on all log output
- Enables precise timing analysis
- Helps identify timeouts and race conditions
- Useful for debugging network issues

---

## [1.3.0] - 2026-01-11

### 🚀 NEW FEATURES

**Same-Format Fast Path (Thanks to SwissMountainsBear)**

Track transitions within the same audio format are now dramatically faster.

| Before | After | Improvement |
|--------|-------|-------------|
| 600-1200ms | <50ms | **24× faster** |

How it works:
- Connection kept alive between same-format tracks
- Smart buffer management (DSD: silence clearing, PCM: seek_front)
- Format changes still trigger full reconnection (safe behavior)

**Dynamic Cycle Time Calculation**

Network timing now adapts automatically to audio format characteristics:
- DSD64: ~23ms optimal cycle time (was 10ms fixed)
- PCM 44.1k: ~50ms optimal cycle time (was 10ms fixed)
- DSD512: ~5ms optimal cycle time (high throughput)

**Transfer Mode Option**

Added `--transfer-mode` option:
- **VarMax (default)**: Adaptive cycle timing
- **Fix**: Fixed cycle timing for precise control

```bash
# Fixed timing at 528 Hz
sudo ./DirettaRendererUPnP --target 1 --transfer-mode fix --cycle-time 1893
```

---

## [1.2.1] and earlier

See git history for previous versions.

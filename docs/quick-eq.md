# Quick EQ

The DSP and settings UI are ported from the working YTAudioEQ implementation in the local
uYouEnhanced project. Spotify uses Vitrine's existing output callback, with no compressed-audio
decoder and no additional import rebinding. Files live in `Shared/QuickEQ`; the small entry hooks
remain in their respective Native, Redesigned and App layers. No existing effect or player stage is
replaced. Preferences use `spotifyglass.quickEQ.configuration`, so Vitrine's reset/backup scope includes them.

## Audio path

`Shared/Player/SpeedPitch.x` identifies the active music AudioUnit. Its existing watcher prepares the
Quick EQ adapter when the unit starts, changes or receives a new format. `AudioEffects.x` calls it at
the end of the post-render notify, after the existing effects and before the visualization reader.
It processes actual output PCM, not the compressed Spotify stream or the audio-session device rate.
Sing, lossless and speed/pitch retain their existing stages.

The adapter accepts mono/stereo float32, signed16, signed32 or fixed 8.24, interleaved or planar, at
8–192 kHz. Unsupported layouts pass through and produce a diagnostic. Format and source epochs,
plus discontinuities in sample timestamps, reset filter history. Separate fixed storage holds each
output's state in a 32-slot cache. Idle slots are reclaimed as outputs are replaced; a long session
is not limited to 32 output addresses. A callback rechecks ownership after acquiring the slot so an
old callback cannot use a newly assigned output's history. Format and generation publish atomically.
The render path does not allocate, write preferences, send Objective-C messages or wait for a lock.

The bass shelf is broad and non-resonant. The ten octave-width peaking filters and bass share automatic
headroom and a stereo-linked **−1 dBFS sample-peak** limiter. Settings changes crossfade stable filter
banks over 20 ms. This bounds digital peaks, not intersample peaks or physical speaker distortion.
With Quick EQ off, samples pass unchanged after its fade; any separately enabled Audio Effects still
apply. Raising both EQs stacks their effects. The ten-band EQ reset leaves bass/settings alone; the
full Quick EQ reset turns its three switches off, restores bass to 80 Hz / +4 dB / 12 dB max, and flattens EQ.

## Build and checks

Run **Build IPA from your own Spotify IPA** (`.github/workflows/build-ipa.yml`) with a direct decrypted
Spotify IPA link. The result is under the run's **Artifacts → vitrine.ipa** by default. Extract the
artifact ZIP and sign the IPA as described in the README. The separate **Quick EQ checks** workflow
only runs checks; it does not produce an installable Spotify IPA.

`tweak/Makefile` compiles the C++ DSP/adapter and Objective-C++ panel into `spotifyglass.dylib` and stages
`SGQuickEQ.bundle` alongside it. The IPA pipeline verifies one injected tweak, a real dylib load command,
the EQ panel/code marker, and both localization files. This checks package contents, not runtime loading
or Spotify playback. Versioning continues through Vitrine's existing Release Please workflow.

Portable checks, with a C++17 compiler and Python 3:

```sh
bash harness/quick-eq/check.sh
python3 harness/quick-eq/packaging.py
python3 harness/quick-eq/make-flags.py
bash scripts/check-layers.sh
SANITIZE=address,undefined bash harness/quick-eq/check.sh
```

The DSP checks cover each band's response, bass frequency/gain limits, maximum combined boost, limiter,
44.1/48 kHz, mono/stereo PCM encodings, fades and mailbox concurrency. The actual production adapter is
compiled against a minimal Core Audio type shim and fed samples, including a measured 40 Hz/1 kHz bass
ratio, multiple outputs, source/format changes, invalid buffers and bit-exact settled bypass. Only its
format-query API is mocked; these checks cannot prove interception inside Spotify.

Regression checks also cover limiter release overshoot, the exact int32 samples after a fade ends
mid-buffer, settings changed while an unsupported route is selected, 160 consecutive output replacements,
2000 replacements concurrent with rendering, and preserving storage beyond the requested silent frames.
The compiler-flag probe expands the actual Makefile with empty stand-ins for the external Theos includes:
C++17 must apply to the C++ files without reaching Swift's Objective-C importer. It is not an iOS build.

Apple SDK checks (Mac/Xcode, `THEOS` set for the last two):

```sh
bash harness/quick-eq/check-apple.sh
bash harness/audio-effects/build-sim.sh
bash harness/lossless/handoff-build.sh
```

`check-apple.sh` compiles all production Quick EQ sources for arm64 with Clang modules enabled as in Theos,
and links with stand-ins for SGPage and SGPresentDark, checking C/Objective-C++ linkage. Framework imports
stay outside `extern "C"`; the shared `SGPresentDark` declaration uses `FOUNDATION_EXPORT` for C linkage.
With `THEOS` set, it also preprocesses and syntax-checks
the real native/player/settings/audio hooks and player menu. The existing audio integration harness links
the real Quick EQ adapter with a default-off mailbox. Its player, system-menu and Mod Settings UI
harnesses use explicit Quick EQ stubs. None of those stand-ins ship in the tweak.

The Apple check also runs `player-method-hook.m` on the Mac host against Apple's Objective-C runtime,
using an NSObject-based class hierarchy so class initialization matches the real views:
the player's Connect hit-area hook must preserve superclass/sibling methods and return the original
implementation without requiring Cydia Substrate.

Local validation on Windows/WSL covers the portable checks. Apple SDK compilation, UI behavior and
device playback have not been verified here. The Apple CI job builds the SDK and audio/handoff harnesses;
it does not run the simulator or a listening test.

## Device acceptance

- On a supported Spotify base version, test both looks. Open EQ from the player and Mod Settings;
  dismiss it back to the same player. Check portrait, landscape, large text and RTL; a crowded native
  header deliberately leaves the new button out instead of covering Spotify controls.
- Enable master and bass. The status must show **Active: Spotify PCM** with increasing processed counts.
  Compare 80 Hz / +4 dB and zero gain on the same recording; measure the spectrum as well as listening.
- Try every EQ band, lower max gain below the current gain, reset EQ, then reset Quick EQ. Relaunch and
  verify saved values. Verify the independent Audio Effects settings still work, together and alone.
- Test several tracks, seeking, pause/resume, downloaded tracks, screen lock/background, speaker,
  wired headphones and Bluetooth. Change routes during playback and verify counts/rate recover and
  no old filter tail plays. Check the limiter at maximum combined boost and switching continuity.
- Check lyrics, Connect, queue, volume, speed/pitch, Sing and lossless handoff remain usable. AirPlay
  and remote Spotify Connect processing are not acceptance requirements.
- If inactive, copy the panel's diagnostic report: reachability, callback/processed counts, actual PCM
  rate, output route and the retained failure stage/OSStatus distinguish UI availability from processing.

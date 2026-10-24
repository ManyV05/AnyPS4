# shadPS4 NGS2 (vendored)

Source: the `witch-fixes` branch of the user's shadPS4 fork, commit `354faa475753f53332a7d53a5feefbc98e1665f5`
(`src/core/libraries/ngs2/*`, verbatim) plus `externals/LibAtrac9/C/src` (MIT, see `externals/LibAtrac9/LICENSE`).

`shims/` forwards the few shadPS4 headers the module needs to the copies in `3rdparty/shadps4-gnm`
(`common/types.h`, `common/logging/log.h`, `core/libraries/libs.h`, `error_codes.h`, `fmt`) and
replaces the kernel headers: `PthreadMutex` is a member that the code never locks.

The module is built through `tools/stub_generator.py --vendored libSceNgs2.prx=3rdparty/shadps4-ngs2:libSceNgs2`:
the generator emits `extern "C"` forwarders for every `PS4_SYSV_ABI` function (declared from the
definitions in the `.cpp` files, since this module keeps no public header) and `nid_patcher` renames them.
`sceNgs2SystemRender` mixes into the buffer the title hands to `sceAudioOutOutputs`, which
`tools/orbis_impl/libSceAudioOut.cpp` plays through `waveOut`.

Local change: `ngs2_custom.h` closed two struct definitions with a variable of the same name (`} OrbisNgs2CustomChorusModuleOption;`, `} OrbisNgs2CustomVoicePeakMeterParam;`), which defines a global in every translation unit that includes the header; both now end with `};`.

Local change (phase 3.8): `sceNgs2SystemRender` reuses a voice's cached grain only when the voice is more than 64 ms ahead of the wall clock (the fork's window was 8 ms). `libSceAudioOut.prx` paces the title per chunk, so renders never run that far ahead and every render produces a fresh grain; with the 8 ms window, outputs released in bursts by the mixer repeated one chunk in four and the music crackled. The music volume floor the fork applies when the title asks for near silence is 0.4 instead of 0.6 (`ORBIS_NGS2_MUSIC_FLOOR` overrides it) so dialogue stays above the soundtrack.

Local changes (phase 4.1): `sceNgs2ParseWaveformData` parses raw `VAGp` files (the title's sound effects); mastering voices report PLAYING with real peak meters (`bus_peak`, the peak mixed into each bus on the last render) and the sampler state fills `decodedDataSize`, `userData` and `waveformData`; the music hold is scoped (`ShouldHoldMusic`: only a track faded out within three seconds with no other music playing, retired by the next music Play, hidden from the meters), with `ORBIS_NGS2_MUSIC_HOLD` and `ORBIS_NGS2_MUSIC_FAITHFUL` switches; `ORBIS_NGS2_TRACE` logs setups, volumes, matrices, events, state polls and per-voice output. See GNM_PLAN.md 4.4.10.

Phase 4.3 (24 October 2026): the fork's behaviour is the default again and the phase 4.1 and 3.8 changes sit behind switches: `ORBIS_NGS2_MUSIC_HOLD=fork|scoped|off` (default `fork`: Stop and Kill on music voices suppressed, retired by the next music Play, floor 0.6 under `ORBIS_NGS2_MUSIC_FLOOR`), `ORBIS_NGS2_MASTERING_METERS=1` (mastering voices PLAYING with meters; default: the fork's idle report), `ORBIS_NGS2_SAMPLER_EXT=1` (extended sampler state; default zero like the fork), `ORBIS_NGS2_GRAIN_WINDOW_MS` (default 8, the fork's window), `ORBIS_NGS2_VAG_PARSE=1` (parse VAGp files; off by default since phase 4.4, the fork's generic report is what the title gets). Phase 4.4 also loops music voices between the `smpl` loop points the RIFF parser finds (kept by data pointer, never reported to the title), decoding and discarding up to the loop start. See GNM_PLAN.md 4.4.12 and 4.4.13. Phase 4.5: diagnostics under `ORBIS_NGS2_TRACE` / `ORBIS_NGS2_DUMP`: per-voice decoded-PCM WAV dumps in `anyps4/audio_dumps`, a voice lifecycle tracker with per-rack active/limit counts (`Voice Leak Detected`), and a per-setup rate/pitch line (GNM_PLAN.md 4.4.14).

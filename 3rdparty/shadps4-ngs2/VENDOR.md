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

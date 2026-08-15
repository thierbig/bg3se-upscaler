# The MCM flicker under DLSS frame generation — investigation record

*Status as of 2026-08-14. This branch (`sync-upstream`) is the PureDark-compatible line:
stable with `NgxOverlayStage = 0`. The flicker is unfixed here, for the architectural
reason documented below. The successor approach lives on the `dlss-fg` branch.*

## Symptom

With PureDark's BG3 upscaler driving DLSS-G (frame generation), the extender's IMGUI
overlay (MCM menu) flickers: visible on some frames, absent on others. Present on every
extender build since the upscaler was introduced; tolerable but ugly.

## Root cause, measured

Diagnostic counters (`73ef388`, `71f332c`) established:

- Exactly **one** swapchain exists; no presents are skipped by the extender.
- The extender's present hook fires **once per UI frame** (1:1 over 1,665 frames) while
  DLSS-G runs at 4x. Generated frames are synthesized by Streamline **below** our hook,
  from buffers captured before the overlay is drawn, and presented directly.
- Therefore the menu exists on 1 of every N frames (N = FG multiplier). That is the
  flicker. **No present-time drawing can ever fix it.**

Confirmed by experiment: FG off ⇒ no flicker; flicker rate tracks the multiplier.

## The attempted fix and why it is a dead end

The composite approach: hook `NVSDK_NGX_VULKAN_EvaluateFeature` and draw the overlay
into the DLSS output image, upstream of FG capture. Implementing it surfaced and fixed
six real bugs (all latent in the original patch, unmasked one at a time):

| # | Bug | Evidence | Fix commit |
|---|-----|----------|-----------|
| 1 | NGX hook never installed: probed `sl.interposer.dll`/`nvngx_dlss.dll`/`nvngx.dll`, but the exports live in **`_nvngx.dll`** (driver store) and **`bg3.exe`** (statically linked); export is the non-`_C` variant | module scan log | `e3edfda` |
| 2 | ImGui pipeline built for swapchain format 44 (`B8G8R8A8_UNORM`), NGX output is format 122 (`B10G11R11_UFLOAT`); render-pass/pipeline format incompatibility ⇒ `DEVICE_LOST` | first composite crashed | `7bb708a` / `a77a355` |
| 3 | Overlay drawn twice per frame (NGX path + present path), lapping ImGui's vertex ring | reasoning + gate log | `7b6f92f` |
| 4 | Data race: NGX callback touched shared caches without `globalResourceLock_` | "built overlay pipeline" logged **twice** for one format | `e54257d` |
| 5 | Framebuffers cached by `VkImageView` handle; NGX recycles views, handles get reused ⇒ use-after-free | stage bisect: stage 1 clean, stage 2 faults | `8936f36` |
| 6 | Caches outlived backend teardown on swapchain recreate (alt-tab); then a `vkDeviceWaitIdle` fix inside the destroy hook was itself illegal (game threads still submitting) | alt-tab crash; then instant startup crash | `605930a` / `2055d75` / warmup `068c063` |

Debugging infrastructure that made this tractable (kept on this branch):

- **`NgxOverlayStage`** in `ScriptExtenderSettings.json`: `0` off, `1` barriers only,
  `2` + render pass, `3` full composite. Runtime bisect, no rebuild.
- One-shot INFO/ERR logging along the whole NGX path.

**The wall (stage 3):** with every lifetime/sync bug fixed, enabling the actual ImGui
draw produced whole-frame block corruption and a device loss. Mechanism: the draw is
recorded into the **middle of NGX's own command buffer**, and Vulkan has no way to save
and restore another recorder's state. Our pipeline/descriptor/vertex/viewport binds
clobber whatever Streamline records after `EvaluateFeature` returns; their subsequent
work executes under our state. Stage 2 (identical machinery minus the draw) is stable
through alt-tabs, isolating the fault to state pollution, not resources. **Injecting
draws into a foreign command buffer is unfixable by construction.**

## The decisive clue

Streamline's DLSS-G debug banner during the corruption run:

```
NVIDIA DLSSG v310.6.0 ... SL v2.11.0-rc1 - VK - 2560x1440 - 4x - Hudless: No - UIAlpha: No - UIR: N/A
```

`UIAlpha: No`: Streamline's `UIColorAndAlpha` buffer tag — the mechanism NVIDIA built
for compositing UI onto generated frames — is **unused** by PureDark's mod. Feed DLSS-G
a UI image with alpha and it composites the menu onto every generated frame itself; no
foreign command buffers involved.

## Where this goes

The `dlss-fg` branch pursues the extender driving Streamline directly (no
`upscaler.dll`): `slInit` + interposer chaining (already working here, `f1a3c16`),
depth/mvec/jitter harvested from the game's **native** DLSS SR NGX call (bg3.exe
creates FeatureID 1 itself), camera constants from BG3SE's mapped camera components,
and the overlay delivered via `UIColorAndAlpha`. Known risk concentrates in
`sl::Constants` (camera matrices) and Reflex marker placement — quality tuning, not
plumbing.

## Also fixed along the way (independent of the flicker)

- `f1a3c16`: `sl.interposer.dll` resolved at device creation (bare-name `LoadLibraryW`
  at init never found it in `mods\UpscalerBasePlugin\Streamline\`); without this the
  extender bypasses Streamline's swapchain proxy and FG cannot inject.
- CI pinned to `windows-2022` + `v143` (`206992c`): the vendored `External/` tree is a
  VS2022-era snapshot; `windows-latest` rolled to VS18 and broke ATL/protobuf/ZipLib.
- FG silently disabled for months: `mDLSSGEnabled = false` in `BG3Upscaler.ini` plus a
  missing `UpscalerBasePlugin` plugin binary (only the `Streamline\` folder was
  installed) plus expired Patreon auth. See BG3UpscalerProxy README troubleshooting.

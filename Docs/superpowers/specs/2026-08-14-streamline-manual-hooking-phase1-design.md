# Streamline manual hooking — phase 1 (plumbing) design

*Branch: `dlss-fg`. Date: 2026-08-14. Status: approved design, pre-implementation.*

## Context

The goal of the `dlss-fg` branch is DLSS frame generation driven entirely by the
extender — no PureDark, no third-party injector — ending (phase 3) with the IMGUI menu
composited onto generated frames via Streamline's `UIColorAndAlpha` tag, eliminating
the flicker documented in `docs/FLICKER-INVESTIGATION.md`.

The foundation spike established, on this machine:

- Streamline 2.12.0 initializes under the extender (`slInit ok`), with a matched,
  signed runtime we ship at `bin\mods\BG3SE-Streamline\` (interposer, common, dlss_g,
  reflex, pcl, nvngx_dlssg, NvLowLatencyVk — all one version, headers to match in
  `External/streamline/include`).
- Init must happen at the first `vkCreateInstance`, not extender startup: earlier,
  NVAPI reports driver version 0 and every plugin self-disables (adapter mask `0x0`);
  an early-loaded interposer also observes pre-instance loader calls and `slInit`
  refuses with result 24. One attempt only — retrying a failed `slInit` crashes.
- The plugins recognize the GPU (`sl.dlss_g` adapter mask `0x1`, driver 610.88
  detected, native VK optical-flow supported).
- **Dead end, proven:** routing the game's device creation through the interposer's
  `vkCreateDevice` wrapper crashes with an instruction fetch at address 0 inside
  `sl.common` during `initializePlugins`. Invariant across SL versions (2.10.3-mixed
  and 2.12.0-matched) and across reentry-guard designs (thread-local and process-wide
  atomic). The interposer's automatic Vulkan path is not usable from a detoured
  context in this game.

Streamline's own headers prescribe the alternative (`sl_helpers_vk.h`, at
`slSetVulkanInfo`): *"Only call this API if NOT using vkCreateDevice and
vkCreateInstance proxies provided by SL."* That is manual hooking, and it is
consistent with the observed behavior of the only working stack (PureDark's), whose
FG engages late (~95% of the load screen, at the swapchain recreate) rather than at
boot.

## Goal (phase 1 only)

Prove the manual-hooking device handoff safe, end to end, behind a gate:

- `slInit` with `eUseManualHooking`, requirements queried and logged
- the game's instance/device created by the *game's own* loader calls, extended
  additively with SL's requirements
- handles delivered via `slSetVulkanInfo`
- `DLSS-G supported` reported, game plays normally, zero crashes

Explicitly **not** phase 1: frame tokens, `slSetConstants`, buffer tags, Reflex/PCL
markers, `slDLSSGSetOptions`, FG activation (phase 2); UI tag (phase 3).

## Non-goals and constraints

- `dlss-fg` never loads `upscaler.dll`. PureDark exists only on `sync-upstream`.
- Gate off ⇒ byte-for-byte vanilla extender behavior: no SL module load, no routing,
  no logging beyond nothing.
- The game must always boot, whatever SL does. Any SL failure degrades to vanilla
  for the session with one `ERR` line.

## Design

### 1. Gating

`ScriptExtenderSettings.json` key `"StreamlineEnabled"`, default `false`, read via
the existing `ExtenderConfig` plumbing (`ConfigGet`, bool overload — same pattern as
`NgxOverlayStage`). Checked before any SL code path: module load, init, and both
create wrappers all short-circuit to original behavior when off.

### 2. Init flow (at first `vkCreateInstance`, once)

Per the spike's proven ordering, inside `vkCreateInstanceWrapped`:

1. `slInitAttempted_` latch (exists) — one attempt per process, success or failure.
2. `StreamlineManager::Load()` — `BG3SE-Streamline\sl.interposer.dll` by absolute
   path (`GetProcAddress` for the `sl*` API; the interposer's `vkCreate*` exports are
   no longer fetched or used).
3. `StreamlineManager::Init()` — `sl::Preferences` as in the spike (app id
   `0xE658703`, `RenderAPI::eVulkan`, verbose log callback, OTA off) **plus
   `PreferenceFlags::eUseManualHooking`**.
4. On success: `slGetFeatureRequirements(kFeatureDLSS_G / kFeatureReflex /
   kFeaturePCL)`; store and log each `FeatureRequirements` in full — instance/device
   extensions, Vulkan 1.2/1.3 feature names, `vkNumGraphicsQueuesRequired`,
   `vkNumComputeQueuesRequired`, `vkNumOpticalFlowQueuesRequired`.

All boot-window output continues through the buffered `Note()`/`SE-Streamline.log`
mechanism from the spike.

### 3. Create-info surgery (additive only)

**Instance** (`vkCreateInstanceWrapped`): union of `vkInstanceExtensions` across the
three features, minus extensions the game already requests, appended to a copy of the
game's `VkInstanceCreateInfo`. Original struct untouched; copy passed to `orig`.

**Device** (`vkCreateDeviceWrapped`):

- *Extensions*: same union/dedup/append as instance.
- *Features*: for the 1.2/1.3 feature names in requirements, build
  `VkPhysicalDeviceVulkan12Features` / `...13Features` via `sl_helpers_vk.h`'s
  `getVkPhysicalDeviceVulkan12Features`/`13`. If the game's `pNext` chain already
  contains the corresponding struct, OR the required booleans into it in place,
  restoring the saved values immediately after the `orig` call returns
  (mutate-and-restore: bounded to the call, single-threaded, invisible to the game);
  only if the struct is absent, prepend SL's copy at the head of the copied
  create-info's chain.
- *Queues*: SL needs extra queues (`graphics`, `compute`, `opticalFlow` counts). For
  each, find the family (graphics: the game's graphics family; compute: dedicated
  compute family if present; optical flow: family advertising
  `VK_QUEUE_OPTICAL_FLOW_BIT_NV`, only if `vkNumOpticalFlowQueuesRequired > 0`).
  Extend the copied `VkDeviceQueueCreateInfo` array: raise `queueCount` on the
  family's existing entry (or add an entry if the game doesn't use that family),
  clamped to the family's `queueCount` limit from
  `vkGetPhysicalDeviceQueueFamilyProperties`. Record the *index* of each SL queue
  (first index beyond the game's own count in that family) for the handoff. If a
  required queue cannot be allocated within the family limit, treat as failure
  (section 5).
- Priorities: appended queue entries use priority 1.0 buffers with correct
  `pQueuePriorities` sizing (the copy owns all memory it references).

The original create is invoked through `orig` (the Detours trampoline to the loader).
The interposer's wrapper functions are never called; the process-wide routing guard
and proxy plumbing from the spike are removed.

### 4. Handoff

After `orig` succeeds:

```
sl::VulkanInfo info{};            // kStructVersion3, per sl_helpers_vk.h 2.12.0
info.device / instance / physicalDevice = the created handles
info.graphicsQueueFamily / graphicsQueueIndex = recorded in §3
info.computeQueueFamily / computeQueueIndex   = recorded in §3
info.opticalFlowQueueFamily / opticalFlowQueueIndex = recorded in §3 (if required)
info.useNativeOpticalFlowMode = true if the native VK optical-flow family was used
slSetVulkanInfo(info)             // resolved via GetProcAddress
```

Result logged. Failure ⇒ section 5. On success, `LogFeatureSupport()` runs as in the
spike (`slIsFeatureSupported` per feature with versions).

### 5. Failure containment

Single policy, applied at every SL touchpoint (init, requirements, device creation
with extended info, handoff):

- If the **extended** `vkCreateDevice` fails: log the failing `VkResult` and the full
  list of what was added, retry exactly once with the game's *original* create-info,
  and permanently disable SL for the session.
- Any other SL failure: one `ERR` line with the result code, SL permanently disabled
  for the session, execution continues on the original path.
- No SL state is ever consulted again after disable; the wrappers reduce to
  pass-throughs.

### 6. Component layout

- `BG3Extender/Extender/Client/IMGUI/Streamline.h` — `StreamlineManager` grows:
  requirements storage, `InitManual()`, `HandOffDevice(...)`, `Disable(reason)`.
  (Stays header-only and in this directory for phase 1; relocation out of IMGUI/ is
  deferred until phase 2 makes the shape of the frame-data path clear.)
- `BG3Extender/Extender/Client/IMGUI/Vulkan.inl` — the two `*Wrapped` functions:
  gate check, surgery calls, handoff call. Interposer-routing code and
  `gSLRouteInFlight` deleted.
- `BG3Extender/Extender/Shared/ExtenderConfig.{h,inl}` — `StreamlineEnabled` key.
- No vcxproj changes (header-only additions).

### 7. Verification (phase-1 done criteria)

Three launches, each with `SE-Streamline.log` and crash-dump checks:

1. **Gate off** (default): `SE-Streamline.log` not written this session (delete
   before launch; confirm it does not reappear); game identical to vanilla; play +
   alt-tab; no dumps.
2. **Gate on**: log shows — manual-hooking `slInit ok`; three requirement sets;
   instance/device extension lists actually appended; `slSetVulkanInfo` ok;
   `DLSS-G supported`. Game reaches gameplay; no dumps.
3. **Gate on, stress**: full load into a save, several alt-tabs, quit from menu; no
   dumps, no device loss.

Any crash in (2)/(3) is triaged with the established tooling (dump parser, stack
walker, SE-Streamline.log) before any further code change.

## Risks

- **pNext merge correctness** is the riskiest code: BG3 chains many feature structs.
  Mitigations: additive-only policy, OR-ing into existing structs rather than
  double-chaining, and the §5 vanilla retry making any mistake non-fatal.
- **Queue limits**: if the game already saturates a family (graphics max 16, compute
  max 8 observed), SL's extra queue may not fit. §5 treats it as clean failure;
  phase 2 can revisit queue sharing if it ever occurs in practice.
- **`slSetVulkanInfo` sufficiency**: the manual path is documented but less traveled
  than the proxy path. If SL still needs hooks we haven't provided, phase-1's
  verification will show it as a clean failure (§5), not a crash — that evidence
  then feeds the phase-2 design rather than blocking the game.

## Phase map (for orientation)

1. **This spec** — safe device handoff, gate-off vanilla guarantee.
2. Frame data & activation — tokens, camera constants (BG3SE-mapped camera),
   depth/mvec tags from the game's native DLSS SR call, Reflex/PCL markers,
   `slDLSSGSetOptions`, late activation at the load-screen swapchain recreate.
3. UI on generated frames — render the IMGUI overlay to a dedicated image, tag as
   `UIColorAndAlpha`; flicker ends.

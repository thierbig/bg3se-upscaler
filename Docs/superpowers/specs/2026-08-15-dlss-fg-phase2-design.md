# DLSS Frame Generation — phase 2 (activation + frames + pacing) design

*Branch: `dlss-fg`. Date: 2026-08-15. Status: design, pre-plan. Builds on phase 1
(`2026-08-14-streamline-manual-hooking-phase1-design.md`), which is sealed at `b5d2b93f`.*

## Goal

Turn on DLSS Frame Generation, driven entirely by the extender — no PureDark. Phase 1
proved the extender initializes Streamline and hands it the device (`slSetVulkanInfo ok`,
`DLSS-G supported`). Phase 2 feeds Streamline the per-frame data DLSS-G needs and switches
it on, ending with generated frames delivered smoothly. Phase 3 (separate) then composites
the IMGUI menu onto generated frames via `UIColorAndAlpha`, killing the flicker.

## The key insight (why this is ours, not a PureDark clone)

DLSS-G's `sl::Constants` requires the camera matrices (mandatory, not optional;
`eFailCommonConstantsInvalid` is a named failure). NGX DLSS-*SR* — the call we hook — does
not carry those matrices (SR is screen-space). A generic injector (PureDark) must scrape
them from process memory. **We don't have to: BG3SE already maps the game's live
`ViewMatrix`/`ProjectionMatrix` and their inverses** (`GameDefinitions/Components/Camera.h:117-120`,
`glm::mat4`). So phase 2 is a hybrid by design, using each source for what it is
authoritative about:

| Data | Source |
|---|---|
| depth buffer, motion-vector buffer (the resources) | NGX parameter block (our `EvaluateFeature` hook) |
| jitter offset, mvec scale, reset | NGX parameter block |
| camera view/clip matrices, `clipToPrevClip`, pos/dirs/near/far/FOV/aspect | BG3SE camera component + glm + a prev-frame cache |

This is expected to be *more* correct than a memory-scraping injector, because the matrices
are the renderer's actual values, read from a mapped component rather than inferred.

## Sub-phase decomposition

Each sub-phase is its own plan + launch checkpoints. They land in order; 2a is the cheap
"does FG turn on at all" gate, analogous to phase 1's `slInit` gate.

### 2a — Activation (FG engages)

**Deliverable:** the DLSS-G watermark appears and `slDLSSGGetState` reports FG active, even
before constants/tags are perfect (frames may be wrong/absent — this only proves the pipe
is live).

- **Carry-in fix (blocking):** the device surgery creates an optical-flow queue and reports
  `useNativeOpticalFlowMode=true`, but never enables the `VkPhysicalDeviceOpticalFlowFeaturesNV.opticalFlow`
  feature. `vkCreateOpticalFlowSessionNV` (which DLSS-G calls) requires it. Add it to the
  device create-info via `sl::getVkPhysicalDeviceOpticalFlowNVFeatures` (`sl_helpers_vk.h:114`),
  chained additively like the 1.2/1.3 features.
- **Phase-1 cleanups (carry-in):** reorder `Streamline.h` includes so `vulkan/vulkan.h`
  precedes the SL headers (parked finding 6); drop the dead `sl_` member (parked).
- Switch the feature merge to `sl::getMergedSupportedVkPhysicalDeviceVulkanFeatures`
  (`sl_helpers_vk.h:128`) so an unsupported requested feature is masked out instead of
  failing the whole device create.
- **Frame token** each frame: `slGetNewFrameToken` (a monotonic frame index we own).
- **Viewport handle**: a single `sl::ViewportHandle{0}` for the game's one viewport.
- **Activation trigger:** call `slDLSSGSetOptions(viewport, {mode=eOn, numFramesToGenerate=N})`
  at the point FG can engage — the load-screen swapchain recreate (~95%), matching the
  observed working behavior. Read `mDLSSGFrames`-equivalent from a new settings key
  (`StreamlineFGFrames`, default 1 → 2x). Poll `slDLSSGGetState` and log the state +
  any `DLSSGStatus` failure flags.
- **Gating:** all under phase 1's `StreamlineEnabled`; a second key `StreamlineFGEnabled`
  (default false) gates activation specifically, so 2a can be toggled without touching the
  proven phase-1 plumbing.
- **Checkpoint:** gate on + FG on → watermark visible, `slDLSSGGetState` active, no device
  loss. Gate off → phase-1 behavior unchanged.

### 2b — Correct frames (the tuning phase)

**Deliverable:** generated frames are correct — no ghosting/smearing/inversion; motion looks
right under camera pan and character movement.

- **Harvest inputs** in the `EvaluateFeature` hook (extends the phase-flicker read that
  already pulled the NGX Output): read the input **depth** and **motion-vector** resources
  (`NVSDK_NGX_Parameter_GetVoidPointer` with the NGX input names — investigation task:
  confirm the exact param-name strings, standard NGX uses "Depth"/"MotionVectors"), plus the
  scalar **jitter offset**, **mvec scale**, and **reset** (`GetVoidPointer`/`GetF`/`GetI`).
- **Tag** depth and mvec via `slSetTagForFrame(frame, viewport, tags, n, cmdBuffer)` with a
  `sl::Resource{ eTex2d, VkImage, VkDeviceMemory, VkImageView, layout }` per buffer and the
  render extent. Lifecycle: volatile (valid only within the frame) with the command buffer
  supplied — must respect the tag fence (`DLSSGState::inputsProcessingCompletionFence`) noted
  in `sl_dlss_g.h:62,170` before reusing/destroying tagged resources.
- **Constants** from BG3SE's camera:
  - fetch the live camera component (investigation task: the ECS query that returns the
    active `CameraComponent` each frame),
  - `cameraViewToClip = ProjectionMatrix`, `clipToCameraView = InvProjectionMatrix`,
  - `clipToPrevClip = prevViewProj * InvViewProj_thisFrame`, `prevClipToClip` its inverse,
    where `viewProj = Projection * View`; cache this frame's `viewProj` for the next frame,
  - `cameraPos/Up/Right/Fwd` from `InvViewMatrix` columns; `cameraNear/Far/FOV/AspectRatio`
    from `ProjectionMatrix`,
  - `jitterOffset`, `mvecScale` from the NGX block; `depthInverted`, `cameraMotionIncluded`,
    `motionVectors3D`, `reset` set per BG3's conventions (investigation: BG3 uses reverse-Z?
    2D screen-space mvecs? — validated on screen),
  - `slSetConstants(consts, frame, viewport)`.
- **The risk lives here:** matrix convention mismatches (row/col major, NDC z-range, LH/RH,
  jitter sign) produce ghosting rather than crashes. Expect several launch/observe/adjust
  cycles. Mitigation: a debug settings key to dump the first frame's constants + a toggle to
  feed identity `clipToPrevClip` (which disables reprojection → FG degrades gracefully to a
  known-worse baseline, isolating matrix bugs from tag bugs).
- **Checkpoint:** FG on, moving camera and character, menu closed → generated frames are
  artifact-free to the eye; A/B fps roughly doubles at 2x.

### 2c — Pacing + lifecycle

**Deliverable:** smooth frame delivery and a clean shutdown.

- **Reflex/PCL markers** around the frame: `slReflexSleep(frame)` at frame start,
  `PCLHelper`/`ReflexHelper` markers (`SIMULATION_START/END`, `PRESENT_START/END`) via
  `slPCLSetMarker`/`slReflexSetMarker`, so DLSS-G paces generated vs real frames. Reflex is a
  hard DLSS-G dependency (`eFailReflexNotDetectedAtRuntime`).
- **Teardown ordering:** notify SL and free resources before device destruction —
  `slFreeResources(kFeatureDLSS_G, viewport)` in `vkDestroyDeviceHooked`, and `slShutdown`
  at backend teardown (phase 1 resolves `slShutdown_` but never calls it).
- **Multi-instance/device latch:** a `deviceHandedOff_` one-shot (like `slInitAttempted_`) so
  a second `vkCreateDevice` can't re-hand stale queue slots or double-`slSetVulkanInfo`
  (final-review carry-in).
- **Checkpoint:** extended play — no stutter beyond FG's nature, alt-tab and save-load clean,
  quit-from-menu leaves no error and no device-lost.

## Non-goals

- The IMGUI menu on generated frames (flicker kill) — phase 3, `UIColorAndAlpha`.
- HUD-less color tagging (`kBufferTypeHUDLessColor`) beyond what DLSS-G strictly needs.
- DLSS *super-resolution* control — the game keeps driving SR; we only add FG on top.

## Constraints (inherited from phase 1)

- Gate off ⇒ byte-for-byte vanilla. `StreamlineEnabled` off disables everything;
  `StreamlineFGEnabled` off keeps phase-1 plumbing but no FG.
- The game must always boot and must never be left worse than phase 1: any FG setup failure
  logs one ERR, disables FG for the session, and falls back to the phase-1 state (device
  handed over, SR intact, no FG) — never to a crash.
- All new per-frame work is on the render thread inside the existing hooks; no new threads.

## Open investigations (resolved as first plan tasks, not blockers)

1. The ECS query returning the live `CameraComponent` each frame (which entity owns it).
2. The exact NGX input param-name strings for depth / motion vectors / jitter / mvec scale /
   reset on this BG3 build.
3. BG3's depth and motion-vector conventions (reverse-Z? 2D vs 3D mvecs? jitter sign) — first
   validated by the 2b debug dump, then on screen.
4. `numFramesToGenerate` max from `slDLSSGGetState` vs the requested frame count.

## Phase-2a verification (2026-08-15)

- **Checkpoint 2a PASS.** Gate on: `slInit ok` → `slSetVulkanInfo ok` → `DLSS-G functions
  resolved` → **`DLSS-G ON - frames=1 max=3 status=0x0`** — DLSS Frame Generation activated by
  the extender alone (no PureDark), `numFramesToGenerateMax=3` (device supports up to 4x),
  no status-failure flags, no crash. This is the "FG turns on" gate.
- Note: `status=0x0` with no inputs yet supplied means no error was raised at activation;
  generated-frame correctness is 2b (depth/mvec tags + constants). Whether the DLSS-G
  watermark renders and whether frames are visually correct is deferred to 2b by design.

## Environment gotcha (phase 2b, 2026-08-15)

Initializing our Streamline (slInit) redirects the process's single NGX snippet search path to
our plugin folder. If that folder lacks the DLSS super-resolution model `nvngx_dlss.dll`, the
game's own DLSS detection fails and **BG3 removes DLSS from the Upscaling Type menu** (only
FSR/XeSS remain), which in turn means no DLSS-SR EvaluateFeature call and no depth/mvec for us.
Fix: place the game's own `bin\nvngx_dlss.dll` (version-matched) into `bin\mods\BG3SE-Streamline\`
alongside `nvngx_dlssg.dll`. (The `nvngx_dlss.dll doesn't exist in any of the search paths`
warning that remains afterward is Streamline's unused `sl.dlss` plugin, not the game's SR — benign.)

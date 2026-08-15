# DLSS-FG Phase 2b — Correct Frames Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Feed DLSS-G the per-frame data it needs so generated frames are *correct*: harvest depth/motion-vectors + jitter/mvec-scale/reset from the live NGX EvaluateFeature hook, build `sl::Constants` from a thread-safe snapshot of BG3SE's real camera matrices, tag the buffers, and set constants each frame.

**Architecture:** Phase 2a activated FG (`DLSS-G ON`, no inputs). The NGX EvaluateFeature hook already exists on this branch and fires during the game's DLSS-SR call (it currently reads only `"Output"`). Extend it to read the DLSS-SR *inputs* and drive `slSetTagForFrame` + `slSetConstants`. Camera data is snapshotted on the game thread (component reads are NOT render-thread-safe) and read by the render-thread hook.

**Tech Stack:** C++ (MSVC v143), Streamline 2.12.0, glm (vendored), existing `StreamlineManager` + NGX hook in `Vulkan.inl`.

**Spec:** `Docs/superpowers/specs/2026-08-15-dlss-fg-phase2-design.md` (sub-phase 2b). **Investigation (READ FIRST):** `Docs/superpowers/2026-08-15-phase2b-investigation.md` — has the exact struct paths, ECS access, NGX-read pattern, and SL signatures cited below.

## Global Constraints

- Branch `dlss-fg`, `C:\Dev\bg3se-upscaler`. Never load `upscaler.dll`.
- Gates: `StreamlineEnabled` + `StreamlineFGEnabled` (both default false). 2b work runs only when FG is enabled and `streamline_.Ready()`. Off ⇒ unchanged phase-2a behavior.
- Always-boot: any 2b failure (null resource, missing param, snapshot not ready) logs once via `Note()` and *skips that frame's tag/constants* — never crashes, never blocks the game's own DLSS-SR call or present. FG simply doesn't get fed that frame.
- The NGX overlay composite (`NgxOverlayStage`) is unrelated to 2b and stays off (0); 2b uses the hook only to READ inputs and TAG, never to composite. Do not enable or modify the overlay path.
- Render-thread safety: NO ECS/component reads inside the NGX hook or any Vulkan hook. Camera data crosses threads only via the Task-1 snapshot.
- Build+deploy (FOREGROUND, read result, ONE at a time; never background-and-return):
  `timeout 900 /mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe -NoProfile -ExecutionPolicy Bypass -File "C:\\Dev\\bg3se-upscaler\\build_and_deploy_extender.ps1" -SkipCodegen`
  Success = `DONE`, no `error C`. Deploy-only IOException = build PASS; note it.
- Launch checkpoints are controller+human; executor reads `bin\SE-Streamline.log` (delete before) and `bin\*.dmp`. Never claim a checkpoint passed without log evidence. The dev-plugin overlay is installed for live on-screen state.

---

### Task 1: Thread-safe camera snapshot

**Files:**
- Modify: `BG3Extender/Extender/Client/IMGUI/Streamline.h` (snapshot struct + setter/getter on `StreamlineManager`, or a small standalone holder)
- Modify: the client update path that runs on the game thread (investigation: `ScriptExtenderClient.cpp` client update / `OnUpdate`) to populate the snapshot each tick.

**Interfaces:**
- Produces: `struct CameraSnapshot { glm::mat4 view, invView, proj, invProj; glm::vec3 pos, up, right, fwd; float nearP, farP, fov, aspect; bool valid; };` and thread-safe `void StreamlineManager::SetCameraSnapshot(CameraSnapshot const&)` / `CameraSnapshot StreamlineManager::GetCameraSnapshot()` guarded by a mutex (double-buffer or plain mutex copy — a 4×mat4+struct copy under a lock is cheap once per frame).
- Consumes (game thread only): `cameraComponent->Controller->Camera.{ViewMatrix,InvViewMatrix,ProjectionMatrix,InvProjectionMatrix}` and `CameraController::{GetWorldTranslate(),GetWorldRotate(),FOV,NearPlane,FarPlane,AspectRatio}` per the investigation doc's Q1. The active camera via the ECS path in Q2 (`gExtender->GetClient().GetEntityHelpers().GetComponent<CameraComponent>(...)` — the exact handle/active-camera resolution is a sub-step; if a single active camera cannot be resolved cleanly, log and leave `valid=false` — Task 3 skips constants when invalid).

- [ ] **Step 1: Add the snapshot struct + accessors**

In `Streamline.h`, add the `CameraSnapshot` struct (above `StreamlineManager`) and, in the class, a `std::mutex cameraMutex_;`, `CameraSnapshot cameraSnapshot_;`, and:

```cpp
    void SetCameraSnapshot(CameraSnapshot const& s) { std::lock_guard _(cameraMutex_); cameraSnapshot_ = s; }
    CameraSnapshot GetCameraSnapshot() { std::lock_guard _(cameraMutex_); return cameraSnapshot_; }
```

`glm` types come from `<glm/glm.hpp>` (add the include; glm is vendored at `External/glm`).

- [ ] **Step 2: Populate on the game thread**

In the client game-thread update (investigation Q2 cites `ScriptExtenderClient.cpp:76-87` region for the update tick), when `StreamlineEnabled && StreamlineFGEnabled`, resolve the active `CameraComponent`, follow `->Controller`, and fill a `CameraSnapshot` from `Controller->Camera.*` matrices and `Controller` accessors; set `valid=true`; call `imgui_.GetVulkanBackend().Streamline().SetCameraSnapshot(snap)` (use whatever accessor exposes the `StreamlineManager`; if none, add a minimal getter). If the camera can't be resolved, set `valid=false`. Wrap in try/guards so a resolution failure never throws into the game loop.

- [ ] **Step 3: One-shot debug log (sanity)**

Add a one-shot `Note` (first valid snapshot) logging FOV, near, far, aspect, and `proj[0][0]`/`proj[1][1]` so the checkpoint can confirm the matrices are real (non-zero, sane) and not identity/garbage.

- [ ] **Step 4: Build** — run the build+deploy command; expect `DONE`.

- [ ] **Step 5: LAUNCH CHECKPOINT 2b-1 (controller + human)** — gate + FG on; reach gameplay; verify the log shows the one-shot snapshot line with sane values (FOV ~ tens of degrees, near/far positive, non-zero proj diagonal). No dumps. Gate off ⇒ no snapshot line. This proves the camera source works before it's wired to SL.

- [ ] **Step 6: Commit** — `feat(dlss-fg): thread-safe camera snapshot from BG3SE camera (game thread -> render thread)`; push origin dlss-fg.

---

### Task 2: Read DLSS-SR inputs from the NGX hook

**Files:**
- Modify: `BG3Extender/Extender/Client/IMGUI/Vulkan.inl` (the existing NGX EvaluateFeature hook `ngxEvaluateFeatureCHook`)

**Interfaces:**
- Consumes: the existing `NVSDK_NGX_Parameter_GetVoidPointer` resolution and `NVSDK_NGX_Resource_VK` interpretation already in the hook (investigation Q3). 
- Produces: per-call reads of the input **depth** and **motion-vector** `NVSDK_NGX_Resource_VK` (VkImage/VkImageView/format/width/height) and the scalar **jitter X/Y**, **mvec scale X/Y**, **reset** — stored in a small `NgxFrameInputs` struct on the backend for Task 3 to tag/convert. No tagging yet.

- [ ] **Step 1: Add NGX input param-name defines + scalar getters**

At the top of `Vulkan.inl` near the existing NGX defines, add (standard NGX DLSS param names; investigation Q3 confirms only "Output" is currently used):

```cpp
#define NGX_DLSS_Depth "Depth"
#define NGX_DLSS_MotionVectors "MotionVectors"
#define NGX_DLSS_Jitter_X "Jitter Offset X"
#define NGX_DLSS_Jitter_Y "Jitter Offset Y"
#define NGX_DLSS_MVScale_X "MV Scale X"
#define NGX_DLSS_MVScale_Y "MV Scale Y"
#define NGX_DLSS_Reset "Reset"
```

Add the matching `PFN_NVSDK_NGX_Parameter_GetF`/`GetUI`/`GetI` typedefs and resolve them next to `GetVoidPointer` (same module).

- [ ] **Step 2: Read the inputs in the hook**

In `ngxEvaluateFeatureCHook`, alongside the existing Output read, read the Depth and MotionVectors resources via `GetVoidPointer(params, NGX_DLSS_Depth/MotionVectors, &ptr)` → `NVSDK_NGX_Resource_VK*`, and the four scalars + reset via the F/UI getters. Store into a backend member `NgxFrameInputs ngxInputs_` (VkImage/VkImageView/format/extent for depth+mvec, float jitterX/Y, mvScaleX/Y, bool reset, bool valid). If any mandatory read fails, set `ngxInputs_.valid=false` and `Note` once.

- [ ] **Step 3: One-shot debug log** — first valid read logs depth extent+format and mvec extent+format and the jitter/mvscale values.

- [ ] **Step 4: Build** — expect `DONE`.

- [ ] **Step 5: LAUNCH CHECKPOINT 2b-2 (controller + human)** — FG on, reach gameplay: log shows depth + mvec resources with sane dimensions (≈ render resolution) and non-null handles; jitter small (sub-pixel), mvscale plausible. No dumps. This proves we can read the SR inputs before tagging.

- [ ] **Step 6: Commit** — `feat(dlss-fg): read DLSS-SR depth/mvec/jitter/mvscale inputs from NGX hook`; push.

---

### Task 3: Tag inputs + set constants (+ debug levers) — the tuning gate

**Files:**
- Modify: `BG3Extender/Extender/Client/IMGUI/Streamline.h` (`SubmitFrameData`), `Vulkan.inl` (call from the NGX hook)
- Modify: `ExtenderConfig.h`/`.inl` (two debug keys)

**Interfaces:**
- Consumes: `GetCameraSnapshot()` (Task 1), `ngxInputs_` (Task 2), SL sigs from investigation Q4 (`slGetNewFrameToken`, `slSetTagForFrame`, `slSetConstants`, `sl::Resource`, `sl::ResourceTag`, `sl::Constants`).
- Produces: `bool StreamlineManager::SubmitFrameData(VkCommandBuffer cmd, NgxFrameInputs const& in, uint32_t frameIndex)` — gets a frame token, tags depth (`kBufferTypeDepth`) + mvec (`kBufferTypeMotionVectors`) via `slSetTagForFrame`, builds `sl::Constants` from the camera snapshot + NGX scalars, calls `slSetConstants`. Fire-and-log.

- [ ] **Step 1: Debug settings keys**

`ExtenderConfig.h`/`.inl`: `bool StreamlineFGDebugConstants{ false };` (dump the first frame's full constants) and `bool StreamlineFGIdentityReproj{ false };` (feed identity `clipToPrevClip`/`prevClipToClip` — disables reprojection to isolate matrix bugs from tag bugs).

- [ ] **Step 2: Implement `SubmitFrameData`**

Build `sl::Resource` for depth and mvec from `ngxInputs_` (`sl::Resource{ sl::ResourceType::eTex2d, (void*)VkImage, (void*)VkDeviceMemory, (void*)VkImageView, currentLayout }` — note: NGX gives image+view; VkDeviceMemory may be null-acceptable for tagging read-only inputs, verify against the Resource ctor in the investigation doc; if memory is required and unavailable, log and skip). Build `sl::ResourceTag` (volatile lifecycle, extent = input extent) for each. `slGetNewFrameToken_(&token, &frameIndex)`; `slSetTagForFrame_(token, viewport{0}, tags, 2, cmd)`.

Build `sl::Constants`: `cameraViewToClip = snap.proj`, `clipToCameraView = snap.invProj`; `viewProj = proj*view`; `clipToPrevClip = prevViewProj_ * snap.invProj`-equivalent (cache `prevViewProj_`); `prevClipToClip` = inverse; `cameraPos/Up/Right/Fwd` from snapshot; `jitterOffset = {jitterX,jitterY}`; `mvecScale = {mvScaleX,mvScaleY}`; `cameraNear/Far/FOV/AspectRatio` from snapshot; `depthInverted`/`cameraMotionIncluded`/`motionVectors3D`/`reset` per BG3 convention (start: depthInverted per proj sign, mvec 2D, reset from NGX). If `StreamlineFGIdentityReproj`, set clipToPrevClip/prevClipToClip to identity. `slSetConstants_(consts, token, viewport{0})`. If `StreamlineFGDebugConstants`, `Note` the full matrices once.

glm→SL `float4x4` is row-major (`sl_consts.h`); convert explicitly (transpose if glm is column-major — glm default is column-major, so transpose). This conversion is a prime suspect for the tuning loop; make it one clearly-commented helper.

- [ ] **Step 3: Call from the NGX hook**

In `ngxEvaluateFeatureCHook`, after the Task-2 input read and BEFORE calling the original NGX EvaluateFeature (so tags/constants are set for the frame), if FG enabled + Ready + `ngxInputs_.valid` + snapshot valid: `streamline_.SubmitFrameData(InCmdList, ngxInputs_, frameNo_)`.

- [ ] **Step 4: Build** — expect `DONE`.

- [ ] **Step 5: LAUNCH CHECKPOINT 2b-3 — THE TUNING GATE (controller + human, iterative)**

FG on, reach gameplay, move camera and a character. Read BOTH the log (`slSetConstants`/`slSetTagForFrame` return eOk; DLSS-G status flags now clear of "missing constants/inputs"; `numFramesActuallyPresented` > rendered) AND the screen via the dev overlay (Hudless/UIAlpha/mode line). Human reports the VISUAL result:
- fps roughly doubles at frames=1 (2x)? 
- ghosting / smearing / trails behind moving objects?
- inverted or wrong-direction motion?
- UI/HUD artifacts?

This is iterative. Likely adjustments, each a small rebuild: matrix transpose on/off, `depthInverted` flip, mvec scale sign, jitter sign, `clipToPrevClip` order. Use `StreamlineFGIdentityReproj=true` first (should give stable-but-no-reprojection FG — if even that ghosts, the problem is tags/depth, not matrices; if identity is clean and real matrices ghost, the problem is the matrix convention). Iterate until frames are artifact-free to the eye.

- [ ] **Step 6: Commit** (after the gate is visually clean) — `feat(dlss-fg): tag depth/mvec + set constants from camera snapshot - FG generates correct frames`; push. Record the final matrix/convention settings in the spec's 2b verification section.

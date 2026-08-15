# DLSS-FG Phase 2a — Activation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn DLSS Frame Generation *on* — enable the optical-flow device feature, call `slDLSSGSetOptions(mode=eOn)` at the swapchain-create point, and confirm via `slDLSSGGetState` that DLSS-G is active — with correct-frame data (tags/constants) deferred to 2b.

**Architecture:** Builds on sealed phase-1 plumbing (`slSetVulkanInfo` handoff at device creation). Adds the one missing device feature DLSS-G's optical flow needs, two settings keys, and a one-shot FG activation from the existing `vkCreateSwapchainKHRHooked` post-hook. No per-frame work yet.

**Tech Stack:** C++ (MSVC v143), Streamline SDK 2.12.0 (`External/streamline/include`), existing `StreamlineManager` (`BG3Extender/Extender/Client/IMGUI/Streamline.h`) and Vulkan hooks (`Vulkan.inl`).

**Spec:** `Docs/superpowers/specs/2026-08-15-dlss-fg-phase2-design.md` (sub-phase 2a only)

## Global Constraints

- Branch: `dlss-fg` in `C:\Dev\bg3se-upscaler` (WSL `/mnt/c/Dev/bg3se-upscaler`). Never load `upscaler.dll`.
- Gates: phase-1 `StreamlineEnabled` (default false) governs ALL SL code. New `StreamlineFGEnabled` (default false) governs FG activation specifically — off ⇒ exact phase-1 behavior (device handed over, SR intact, no FG).
- Always-boot: any FG-setup failure logs one ERR via the existing `Note()`, disables FG for the session, and leaves the phase-1 state intact — never a crash, never worse than phase 1.
- Build+deploy (run FOREGROUND to completion, read the result; never background-and-return; one build at a time):
  `timeout 900 /mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe -NoProfile -ExecutionPolicy Bypass -File "C:\\Dev\\bg3se-upscaler\\build_and_deploy_extender.ps1" -SkipCodegen`
  Success = `DONE <timestamp>`, no `error C`. Deploy-only IOException (game running) still = build PASS; note it.
- Launch checkpoint is a controller+human step; the executor reads `/mnt/c/Games/Baldurs Gate 3/bin/SE-Streamline.log` (delete before launch) and `/mnt/c/Games/Baldurs Gate 3/bin/*.dmp`. Never claim a checkpoint passed without log evidence.
- Ruling recorded in the spec's plan scope: the `getMergedSupportedVkPhysicalDeviceVulkanFeatures` swap is DEFERRED to 2b (it rewrites proven phase-1 device surgery and is not required for activation on the verified GPU).

---

### Task 1: Enable the optical-flow device feature; phase-1 parked cleanups

**Files:**
- Modify: `BG3Extender/Extender/Client/IMGUI/Vulkan.inl` (device surgery in `vkCreateDeviceWrapped`; dead `sl_` member)
- Modify: `BG3Extender/Extender/Client/IMGUI/Streamline.h` (include order)

**Interfaces:**
- Consumes: `slQueueSlots_.opticalFlowNative` (bool, set in phase-1 device surgery), `sl::getVkPhysicalDeviceOpticalFlowNVFeatures` is NOT needed — build the struct directly.
- Produces: device created with `VkPhysicalDeviceOpticalFlowFeaturesNV.opticalFlow = VK_TRUE` chained when the native OFA queue was reserved. No new public symbols.

- [ ] **Step 1: Reorder Streamline.h includes (parked finding 6)**

In `Streamline.h`, the include block currently is `sl.h`, `sl_helpers_vk.h`, then `vulkan/vulkan.h`. `sl_helpers_vk.h` uses Vulkan types, so move `#include <vulkan/vulkan.h>` ABOVE `#include <External/streamline/include/sl.h>`:

```cpp
#include <vulkan/vulkan.h>
#include <External/streamline/include/sl.h>
#include <External/streamline/include/sl_helpers_vk.h>
```

- [ ] **Step 2: Drop the dead `sl_` member (parked)**

In `Vulkan.inl`, delete the member declaration `HMODULE sl_{ nullptr };` and its sole assignment `sl_ = streamline_.Module();` in `vkCreateInstanceWrapped` (grep `sl_` first to confirm exactly those two occurrences remain after phase 1's WARN-block deletion; if `streamline_.Module()` has no other caller and the result is unused, remove the assignment line entirely).

- [ ] **Step 3: Chain the optical-flow feature in device surgery**

In `vkCreateDeviceWrapped`, in the feature section (where `slF12`/`slF13` are built and prepended), after the 1.3 handling and before the `extended.pNext` prepend of slF12/slF13, add — only when the native OFA queue was reserved:

```cpp
            VkPhysicalDeviceOpticalFlowFeaturesNV slOFA{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV };
            bool wantOFA = (reqs.opticalFlowQueues > 0) && (slQueueSlots_.opticalFlowFamily != ~0u);
            if (wantOFA) slOFA.opticalFlow = VK_TRUE;
```

Then, alongside the existing `if (gameF12 == nullptr ...) prepend` lines, prepend the OFA struct when wanted (the game does not chain it):

```cpp
            if (wantOFA) { slOFA.pNext = const_cast<void*>(extended.pNext); extended.pNext = &slOFA; }
```

`slOFA` is a local of the same scope as `slF12`/`slF13`, alive through the `orig` call. It carries no game-owned bits, so it is NOT part of `restoreGameFeatures()` (it was never in the game's chain). `VkPhysicalDeviceOpticalFlowFeaturesNV` and its structure-type enum come from `<vulkan/vulkan.h>` (verified present, `vulkan_core.h:1014` / feature struct in the NV optical flow section).

- [ ] **Step 4: Build**

Run the build+deploy command. Expected `DONE`, no `error C`.

- [ ] **Step 5: Commit**

```bash
cd /mnt/c/Dev/bg3se-upscaler && git add -A && git commit -m "feat(dlss-fg): enable optical-flow device feature; phase-1 parked cleanups (include order, dead sl_)" && git push origin dlss-fg
```

---

### Task 2: FG settings keys + activation from swapchain creation

**Files:**
- Modify: `BG3Extender/Extender/Shared/ExtenderConfig.h`, `ExtenderConfig.inl` (two keys)
- Modify: `BG3Extender/Extender/Client/IMGUI/Streamline.h` (`ActivateFrameGen`, resolve DLSS-G procs)
- Modify: `BG3Extender/Extender/Client/IMGUI/Vulkan.inl` (call from `vkCreateSwapchainKHRHooked`)

**Interfaces:**
- Consumes: `gExtender->GetConfig().StreamlineFGEnabled` / `StreamlineFGFrames`; `streamline_.Ready()`.
- Produces: `bool StreamlineManager::ActivateFrameGen()` — idempotent, one-shot per successful activation; returns true once DLSS-G is set on. Resolves and stores `slDLSSGSetOptions_` / `slDLSSGGetState_` (via `slGetFeatureFunction` for the DLSS-G feature, or `GetProcAddress` fallback — see Step 3).

- [ ] **Step 1: Add settings keys**

`ExtenderConfig.h`, below `StreamlineEnabled`:

```cpp
    // dlss-fg phase 2: activate DLSS frame generation (requires StreamlineEnabled). Off = phase-1 plumbing only.
    bool StreamlineFGEnabled{ false };
    // Frames generated between rendered frames: 1=2x, 2=3x, 3=4x. Clamped to device max at runtime.
    uint32_t StreamlineFGFrames{ 1 };
```

`ExtenderConfig.inl`, beside the `StreamlineEnabled` parse:

```cpp
    ConfigGet(root, "StreamlineFGEnabled", config.StreamlineFGEnabled);
    ConfigGet(root, "StreamlineFGFrames", config.StreamlineFGFrames);
```

- [ ] **Step 2: Resolve the DLSS-G functions in `HandOffDevice`'s success path**

The DLSS-G entry points are feature functions, resolved via `slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", (void*&)fn)`. In `StreamlineManager`, add members and a resolver, called once right after `slSetVulkanInfo` succeeds (that is when feature functions become available):

```cpp
    PFun_slGetFeatureFunction* slGetFeatureFunction_{ nullptr };   // resolve in Load() alongside the others
    sl::Result (*slDLSSGSetOptions_)(const sl::ViewportHandle&, const sl::DLSSGOptions&){ nullptr };
    sl::Result (*slDLSSGGetState_)(const sl::ViewportHandle&, sl::DLSSGState&, const sl::DLSSGOptions*){ nullptr };

    void ResolveDLSSGFunctions()
    {
        if (slGetFeatureFunction_ == nullptr) { Note("SL: ERROR: slGetFeatureFunction unavailable; FG cannot resolve"); return; }
        void* setOpt = nullptr; void* getState = nullptr;
        slGetFeatureFunction_(sl::kFeatureDLSS_G, "slDLSSGSetOptions", setOpt);
        slGetFeatureFunction_(sl::kFeatureDLSS_G, "slDLSSGGetState", getState);
        slDLSSGSetOptions_ = reinterpret_cast<decltype(slDLSSGSetOptions_)>(setOpt);
        slDLSSGGetState_ = reinterpret_cast<decltype(slDLSSGGetState_)>(getState);
        Note("SL: DLSS-G functions resolved: setOptions=%p getState=%p", (void*)slDLSSGSetOptions_, (void*)slDLSSGGetState_);
    }
```

In `Load()`, resolve `slGetFeatureFunction_ = GetProc<PFun_slGetFeatureFunction>("slGetFeatureFunction");` (add to the required-export check only if you also want load to fail without it — do NOT add it there; a missing DLSS-G is a soft failure). In `HandOffDevice`, after the `slSetVulkanInfo` success log and before `return true;`, call `ResolveDLSSGFunctions();`.

- [ ] **Step 3: Implement `ActivateFrameGen`**

Add to `StreamlineManager`:

```cpp
    bool ActivateFrameGen()
    {
        if (fgActivated_) return true;
        if (!Ready() || slDLSSGSetOptions_ == nullptr || slDLSSGGetState_ == nullptr) return false;

        sl::ViewportHandle viewport{ 0 };

        uint32_t requested = gExtender->GetConfig().StreamlineFGFrames;
        sl::DLSSGState state{};
        if (slDLSSGGetState_(viewport, state, nullptr) == sl::Result::eOk && state.numFramesToGenerateMax > 0) {
            if (requested > state.numFramesToGenerateMax) {
                Note("SL: FG frames %u clamped to device max %u", requested, state.numFramesToGenerateMax);
                requested = state.numFramesToGenerateMax;
            }
        }

        sl::DLSSGOptions options{};
        options.mode = sl::DLSSGMode::eOn;
        options.numFramesToGenerate = requested;

        auto r = slDLSSGSetOptions_(viewport, options);
        if (r != sl::Result::eOk) {
            Note("SL: ERROR: slDLSSGSetOptions(eOn) failed: %d - FG disabled this session", (int)r);
            Disable("DLSS-G SetOptions failed");
            return false;
        }

        sl::DLSSGState post{};
        slDLSSGGetState_(viewport, post, &options);
        Note("SL: DLSS-G ON - frames=%u max=%u status=0x%x", requested, post.numFramesToGenerateMax, (unsigned)post.status);
        fgActivated_ = true;
        return true;
    }
```

Add member `bool fgActivated_{ false };`. `sl::DLSSGState`/`DLSSGOptions`/`DLSSGMode`/`ViewportHandle` come from the already-included `sl_dlss_g.h` (add `#include <External/streamline/include/sl_dlss_g.h>` below the other SL includes in `Streamline.h` if not transitively present).

- [ ] **Step 4: Trigger activation from the swapchain post-hook**

In `Vulkan.inl`, in `vkCreateSwapchainKHRHooked` (post-hook, after the swapchain is recorded), add at the end:

```cpp
        if (gExtender->GetConfig().StreamlineFGEnabled && streamline_.Ready()) {
            streamline_.ActivateFrameGen();
        }
```

(Idempotent via `fgActivated_`; safe to re-enter on later swapchain recreations — SetOptions may be called repeatedly, and the one-shot skips redundant calls. If a future need arises to re-arm on recreate, that is a 2c concern, not here.)

- [ ] **Step 5: Build**

Run the build+deploy command. Expected `DONE`.

- [ ] **Step 6: LAUNCH CHECKPOINT 2a (controller + human)**

Set `ScriptExtenderSettings.json`: `"StreamlineEnabled": true`, `"StreamlineFGEnabled": true`, `"StreamlineFGFrames": 1`. Delete `SE-Streamline.log`. Human launches, reaches gameplay (past the load screen so the swapchain is created), plays ~1 min, quits. Verify in the log:
- `DLSS-G functions resolved: setOptions=<nonzero> getState=<nonzero>`
- `DLSS-G ON - frames=1 max=<n> status=0x<...>`
- no crash dump.

Interpretation: `status` may report missing-input flags (depth/mvec/constants not yet supplied — that is 2b's job). The 2a gate is that SetOptions returns eOk, GetState reports a non-zero `numFramesToGenerateMax`, and the session is stable. The DLSS-G watermark may or may not render without inputs; its absence is NOT a 2a failure. A crash, a `SetOptions failed`, or `max=0` IS.

Also confirm gate-off path: set `"StreamlineFGEnabled": false`, delete log, relaunch briefly — log shows the phase-1 handoff but NO `DLSS-G ON` line; no dumps.

- [ ] **Step 7: Commit**

```bash
cd /mnt/c/Dev/bg3se-upscaler && git add -A && git commit -m "feat(dlss-fg): FG activation - settings keys, slDLSSGSetOptions(eOn) from swapchain hook, state readback" && git push origin dlss-fg
```

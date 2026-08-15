# Streamline Manual Hooking — Phase 1 (Plumbing) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Gated, crash-free Streamline manual-hooking device handoff: the game's Vulkan instance/device are created by the game's own loader calls, additively extended with SL's requirements, and handed to SL via `slSetVulkanInfo` — `DLSS-G supported` reported, game plays normally, gate-off is byte-for-byte vanilla.

**Architecture:** `slInit(eUseManualHooking)` at first `vkCreateInstance` (once, ever); `slGetFeatureRequirements` drives additive create-info surgery inside the existing Detours wrappers; the interposer's own `vkCreate*` wrapper path — proven fatal — is never invoked and its plumbing is deleted. Any SL failure degrades to vanilla for the session.

**Tech Stack:** C++ (MSVC v143), Detours (existing `WrappableFunction`), Streamline SDK 2.12.0 (headers vendored at `External/streamline/include`, runtime at `C:\Games\Baldurs Gate 3\bin\mods\BG3SE-Streamline\`).

**Spec:** `docs/superpowers/specs/2026-08-14-streamline-manual-hooking-phase1-design.md`

## Global Constraints

- Branch: `dlss-fg` in `C:\Dev\bg3se-upscaler` (WSL: `/mnt/c/Dev/bg3se-upscaler`). Never load `upscaler.dll` on this branch.
- Settings gate: `"StreamlineEnabled"`, default `false`. Off ⇒ no SL module load, no routing, no SL logging.
- The game must always boot; every SL failure ⇒ one `ERR` line + permanent session disable + vanilla path.
- Create-info changes are additive only; game-owned memory is never left modified (mutate-and-restore is permitted across the `orig` call only).
- Build+deploy command (from WSL, ~2 min):
  `timeout 900 /mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe -NoProfile -ExecutionPolicy Bypass -File "C:\\Dev\\bg3se-upscaler\\build_and_deploy_extender.ps1" -SkipCodegen`
  Expected: `DONE <timestamp>`; no `FAILED`, no `error C`. Deploy fails with IOException if the game is running — wait for it to close.
- Launch checkpoints require the human to start the game; the executor reads `/mnt/c/Games/Baldurs Gate 3/bin/SE-Streamline.log` (delete before launch) and checks `/mnt/c/Games/Baldurs Gate 3/bin/*.dmp` timestamps. Never claim a launch checkpoint passed without log evidence.
- Commit identity is already configured in the repo; commit after every task.

---

### Task 1: `StreamlineEnabled` config gate

**Files:**
- Modify: `BG3Extender/Extender/Shared/ExtenderConfig.h` (near `NgxOverlayStage`, ~line 60)
- Modify: `BG3Extender/Extender/Shared/ExtenderConfig.inl` (~line 14)
- Modify: `BG3Extender/Extender/Client/IMGUI/Streamline.h` (top of `Load()`)

**Interfaces:**
- Consumes: existing `ConfigGet(root, name, bool&)` overload; `gExtender->GetConfig()` (include `Extender/ScriptExtender.h` is already in `Vulkan.inl`; `Streamline.h` is included from it after that include).
- Produces: `config.StreamlineEnabled` (bool, default false); `StreamlineManager::Load()` returns `false` immediately when the gate is off, which short-circuits every downstream SL path (callers already treat `Load()==false` as "no SL").

- [ ] **Step 1: Add the config field**

In `ExtenderConfig.h`, directly below the `NgxOverlayStage` member:

```cpp
    // dlss-fg: master gate for the extender-driven Streamline integration. Off (default)
    // means not one line of SL code runs - no module load, no routing, vanilla behavior.
    bool StreamlineEnabled{ false };
```

- [ ] **Step 2: Parse it**

In `ExtenderConfig.inl`, next to the `NgxOverlayStage` line:

```cpp
    ConfigGet(root, "StreamlineEnabled", config.StreamlineEnabled);
```

- [ ] **Step 3: Gate the single choke point**

In `Streamline.h`, `Load()` currently begins:

```cpp
        if (module_ != nullptr) return true;
        gInstance = this;
```

Replace with:

```cpp
        if (module_ != nullptr) return true;
        if (!gExtender->GetConfig().StreamlineEnabled) return false;
        gInstance = this;
```

- [ ] **Step 4: Build**

Run the build+deploy command (Global Constraints). Expected: `DONE`, no errors.

- [ ] **Step 5: Commit**

```bash
cd /mnt/c/Dev/bg3se-upscaler && git add -A && git commit -m "feat(dlss-fg): StreamlineEnabled gate, default off" && git push origin dlss-fg
```

---

### Task 2: Strip interposer routing; manual-hooking init + requirements

**Files:**
- Modify: `BG3Extender/Extender/Client/IMGUI/Streamline.h`
- Modify: `BG3Extender/Extender/Client/IMGUI/Vulkan.inl`

**Interfaces:**
- Consumes: `PFun_slGetFeatureRequirements` (already GetProc'd as `slGetFeatureRequirements_`), `sl::FeatureRequirements` (`sl_core_types.h`), `sl::PreferenceFlags::eUseManualHooking`.
- Produces:
  - `struct OwnedRequirements { std::vector<std::string> instanceExtensions, deviceExtensions, features12, features13; uint32_t graphicsQueues{}, computeQueues{}, opticalFlowQueues{}; bool valid{}; };`
  - `OwnedRequirements const& StreamlineManager::Requirements() const` — merged union across DLSS-G/Reflex/PCL, populated by `Init()`.
  - `void StreamlineManager::Disable(char const* reason)` — logs one `ERR` via `Note`, sets `disabled_`; `Ready()` becomes `initialized_ && !disabled_`.
  - `Vulkan.inl` wrappers reduced to: gate → once-only load+init (instance wrapper) → `orig` → existing bookkeeping. No proxy calls, no routing guard.

- [ ] **Step 1: Delete interposer-proxy plumbing from `Streamline.h`**

Remove members `vkCreateInstanceProxy_`, `vkCreateDeviceProxy_`, their `GetProcAddress` lines in `Load()`, and the accessors `CreateInstanceProxy()` / `CreateDeviceProxy()`. In the `Load()` null-export check, drop the two proxy conditions:

```cpp
        if (!slInit_ || !slIsFeatureSupported_) {
```

- [ ] **Step 2: Manual-hooking flag + disable support + owned requirements**

In `Init()`, change the flags line to:

```cpp
        pref.flags = sl::PreferenceFlags::eDisableCLStateTracking
            | sl::PreferenceFlags::eUseManualHooking;
```

Add to the class (public, above `Ready()`):

```cpp
    struct OwnedRequirements
    {
        std::vector<std::string> instanceExtensions;
        std::vector<std::string> deviceExtensions;
        std::vector<std::string> features12;
        std::vector<std::string> features13;
        uint32_t graphicsQueues{ 0 };
        uint32_t computeQueues{ 0 };
        uint32_t opticalFlowQueues{ 0 };
        bool valid{ false };
    };

    OwnedRequirements const& Requirements() const { return requirements_; }

    void Disable(char const* reason)
    {
        if (disabled_) return;
        disabled_ = true;
        Note("SL: ERROR: disabled for this session: %s", reason);
    }
```

Change `Ready()` to `return initialized_ && !disabled_;` and add members `bool disabled_{ false }; OwnedRequirements requirements_;`.

- [ ] **Step 3: Gather requirements at the end of `Init()`**

Immediately after the `Note("SL: slInit ok ...")` line, before `return true;`:

```cpp
        static sl::Feature const wanted[] = { sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };
        auto addUnique = [](std::vector<std::string>& into, char const* value) {
            for (auto const& existing : into) if (existing == value) return;
            into.push_back(value);
        };
        for (auto feature : wanted) {
            sl::FeatureRequirements req{};
            auto reqResult = slGetFeatureRequirements_ ? slGetFeatureRequirements_(feature, req) : sl::Result::eErrorNotInitialized;
            if (reqResult != sl::Result::eOk) {
                Note("SL: WARN: slGetFeatureRequirements(%u) -> %d", feature, (int)reqResult);
                continue;
            }
            for (uint32_t i = 0; i < req.vkNumInstanceExtensions; i++) addUnique(requirements_.instanceExtensions, req.vkInstanceExtensions[i]);
            for (uint32_t i = 0; i < req.vkNumDeviceExtensions; i++) addUnique(requirements_.deviceExtensions, req.vkDeviceExtensions[i]);
            for (uint32_t i = 0; i < req.vkNumFeatures12; i++) addUnique(requirements_.features12, req.vkFeatures12[i]);
            for (uint32_t i = 0; i < req.vkNumFeatures13; i++) addUnique(requirements_.features13, req.vkFeatures13[i]);
            requirements_.graphicsQueues = std::max(requirements_.graphicsQueues, req.vkNumGraphicsQueuesRequired);
            requirements_.computeQueues = std::max(requirements_.computeQueues, req.vkNumComputeQueuesRequired);
            requirements_.opticalFlowQueues = std::max(requirements_.opticalFlowQueues, req.vkNumOpticalFlowQueuesRequired);
        }
        requirements_.valid = true;
        Note("SL: requirements: %u instance ext, %u device ext, %u feat12, %u feat13, queues g=%u c=%u ofa=%u",
            (unsigned)requirements_.instanceExtensions.size(), (unsigned)requirements_.deviceExtensions.size(),
            (unsigned)requirements_.features12.size(), (unsigned)requirements_.features13.size(),
            requirements_.graphicsQueues, requirements_.computeQueues, requirements_.opticalFlowQueues);
        for (auto const& e : requirements_.instanceExtensions) Note("SL:   instance ext: %s", e.c_str());
        for (auto const& e : requirements_.deviceExtensions) Note("SL:   device ext: %s", e.c_str());
        for (auto const& f : requirements_.features12) Note("SL:   feature12: %s", f.c_str());
        for (auto const& f : requirements_.features13) Note("SL:   feature13: %s", f.c_str());
```

Add `#include <algorithm>` next to the other standard includes in `Streamline.h`.

- [ ] **Step 4: Reduce the `Vulkan.inl` wrappers**

Delete the `gSLRouteInFlight` block (comment + `static std::atomic<bool>` declaration) and the `#include <atomic>`. Replace both wrapper bodies:

```cpp
    VkResult vkCreateInstanceWrapped(
        VkCreateInstanceHookType::BaseFuncType* orig,
        const VkInstanceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkInstance* pInstance)
    {
        // Load + init deferred to here (NVAPI is dead earlier and an early-loaded interposer
        // makes slInit refuse); attempted exactly once - retrying a failed slInit crashes.
        if (!slInitAttempted_) {
            slInitAttempted_ = true;
            if (streamline_.Load() && streamline_.Init()) {
                sl_ = streamline_.Module();
            }
        }

        auto result = orig(pCreateInfo, pAllocator, pInstance);
        vkCreateInstanceHooked(pCreateInfo, pAllocator, pInstance, result);
        return result;
    }

    VkResult vkCreateDeviceWrapped(
        VkCreateDeviceHookType::BaseFuncType* orig,
        VkPhysicalDevice physicalDevice,
        const VkDeviceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDevice* pDevice)
    {
        auto result = orig(physicalDevice, pCreateInfo, pAllocator, pDevice);
        vkCreateDeviceHooked(physicalDevice, pCreateInfo, pAllocator, pDevice, result);
        if (result == VK_SUCCESS) {
            streamline_.FlushBootLog();
            streamline_.LogFeatureSupport(physicalDevice);
        }
        return result;
    }
```

(The old `dlssgPresentFunction_` / `dlssgCreateSwapchainKHR_` fetches from `sl_` stay deleted with the routing code — `hookDevice()`'s `nextPresent`/`nextCreateSwapchain` fallbacks to the game functions handle their absence; leave those members null.) Also delete the now-unused `streamline_.FlushBootLog()` call sites elsewhere if duplicated (keep the one in `NewFrame`).

- [ ] **Step 5: Build**

Run the build+deploy command. Expected `DONE`.

- [ ] **Step 6: LAUNCH CHECKPOINT A (requires human)**

Set `"StreamlineEnabled": true` in `C:\Games\Baldurs Gate 3\bin\ScriptExtenderSettings.json`; delete `SE-Streamline.log`; human launches to main menu, quits. Verify:
- log contains `slInit ok`, `SL: requirements:` with nonzero instance/device ext counts, and the per-item lists
- no new `*.dmp` in `bin\`
- game reached the menu normally (SL is initialized but idle — no surgery, no handoff yet)

- [ ] **Step 7: Commit**

```bash
cd /mnt/c/Dev/bg3se-upscaler && git add -A && git commit -m "feat(dlss-fg): manual-hooking slInit + owned requirements; interposer routing removed" && git push origin dlss-fg
```

---

### Task 3: Instance create-info surgery

**Files:**
- Modify: `BG3Extender/Extender/Client/IMGUI/Vulkan.inl` (instance wrapper)

**Interfaces:**
- Consumes: `streamline_.Ready()`, `streamline_.Requirements().instanceExtensions`.
- Produces: instance created with SL's instance extensions appended (dedup, additive, copy-owned memory).

- [ ] **Step 1: Implement the merge inside `vkCreateInstanceWrapped`**

Replace the `auto result = orig(...)` line with:

```cpp
        VkResult result;
        auto const& reqs = streamline_.Requirements();
        if (streamline_.Ready() && reqs.valid && !reqs.instanceExtensions.empty()) {
            std::vector<char const*> extensions(
                pCreateInfo->ppEnabledExtensionNames,
                pCreateInfo->ppEnabledExtensionNames + pCreateInfo->enabledExtensionCount);
            for (auto const& wanted : reqs.instanceExtensions) {
                bool present = false;
                for (auto existing : extensions) {
                    if (wanted == existing) { present = true; break; }
                }
                if (!present) extensions.push_back(wanted.c_str());
            }
            VkInstanceCreateInfo extended = *pCreateInfo;
            extended.enabledExtensionCount = (uint32_t)extensions.size();
            extended.ppEnabledExtensionNames = extensions.data();
            INFO("SL: instance create extended with %u extension(s)",
                (unsigned)(extensions.size() - pCreateInfo->enabledExtensionCount));
            result = orig(&extended, pAllocator, pInstance);
            if (result != VK_SUCCESS) {
                streamline_.Disable("extended vkCreateInstance failed, retrying vanilla");
                result = orig(pCreateInfo, pAllocator, pInstance);
            }
        } else {
            result = orig(pCreateInfo, pAllocator, pInstance);
        }
```

- [ ] **Step 2: Build**

Run the build+deploy command. Expected `DONE`.

- [ ] **Step 3: Commit**

```bash
cd /mnt/c/Dev/bg3se-upscaler && git add -A && git commit -m "feat(dlss-fg): additive instance extension surgery with vanilla fallback" && git push origin dlss-fg
```

---

### Task 4: Device create-info surgery (extensions, features, queues)

**Files:**
- Modify: `BG3Extender/Extender/Client/IMGUI/Vulkan.inl` (device wrapper + one helper + members)
- Modify: `docs/superpowers/specs/2026-08-14-streamline-manual-hooking-phase1-design.md` (§3 amendment)

**Interfaces:**
- Consumes: `streamline_.Requirements()`, `sl::getVkPhysicalDeviceVulkan12Features` / `...13Features` (`sl_helpers_vk.h` — include it in `Streamline.h` below `sl.h`).
- Produces: `struct SLQueueSlots { uint32_t graphicsFamily{~0u}, graphicsIndex{}, computeFamily{~0u}, computeIndex{}, opticalFlowFamily{~0u}, opticalFlowIndex{}; bool opticalFlowNative{}; } slQueueSlots_;` — consumed by Task 5's handoff.

- [ ] **Step 1: Amend the spec's §3 feature-merge paragraph**

The spec's "clone the chain prefix" is unimplementable: pNext nodes are opaque structs of unknown size. Replace that sentence block with:

```
  `getVkPhysicalDeviceVulkan12Features`/`13`. If the game's `pNext` chain already
  contains the corresponding struct, OR the required booleans into it in place,
  restoring the saved values immediately after the `orig` call returns
  (mutate-and-restore: bounded to the call, single-threaded, invisible to the game);
  only if the struct is absent, prepend SL's copy at the head of the copied
  create-info's chain.
```

- [ ] **Step 2: Implement the device surgery**

Add member `SLQueueSlots slQueueSlots_;` (struct as in Interfaces) near `slInitAttempted_`. Replace the `auto result = orig(...)` line in `vkCreateDeviceWrapped` with:

```cpp
        VkResult result;
        auto const& reqs = streamline_.Requirements();
        if (streamline_.Ready() && reqs.valid) {
            // --- extensions (additive, dedup) ---
            std::vector<char const*> extensions(
                pCreateInfo->ppEnabledExtensionNames,
                pCreateInfo->ppEnabledExtensionNames + pCreateInfo->enabledExtensionCount);
            for (auto const& wanted : reqs.deviceExtensions) {
                bool present = false;
                for (auto existing : extensions) {
                    if (wanted == existing) { present = true; break; }
                }
                if (!present) extensions.push_back(wanted.c_str());
            }

            // --- features 1.2/1.3: mutate-and-restore or prepend ---
            auto slF12 = sl::getVkPhysicalDeviceVulkan12Features((uint32_t)reqs.features12.size(),
                [&] { static std::vector<char const*> v; v.clear(); for (auto const& f : reqs.features12) v.push_back(f.c_str()); return v.data(); }());
            auto slF13 = sl::getVkPhysicalDeviceVulkan13Features((uint32_t)reqs.features13.size(),
                [&] { static std::vector<char const*> v; v.clear(); for (auto const& f : reqs.features13) v.push_back(f.c_str()); return v.data(); }());

            VkPhysicalDeviceVulkan12Features* gameF12{ nullptr };
            VkPhysicalDeviceVulkan13Features* gameF13{ nullptr };
            for (auto node = (VkBaseOutStructure*)pCreateInfo->pNext; node; node = node->pNext) {
                if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) gameF12 = (VkPhysicalDeviceVulkan12Features*)node;
                if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) gameF13 = (VkPhysicalDeviceVulkan13Features*)node;
            }

            // OR VkBool32 payloads (skip sType+pNext header) into dst; returns saved copy.
            auto orFeatures = [](void* dst, void const* src, size_t size) {
                std::vector<uint8_t> saved((uint8_t*)dst, (uint8_t*)dst + size);
                auto* d = (uint32_t*)((uint8_t*)dst + offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge));
                auto* s = (uint32_t const*)((uint8_t const*)src + offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge));
                auto count = (size - offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge)) / sizeof(uint32_t);
                for (size_t i = 0; i < count; i++) d[i] |= s[i];
                return saved;
            };

            std::vector<uint8_t> savedF12, savedF13;
            if (gameF12 != nullptr) savedF12 = orFeatures(gameF12, &slF12, sizeof(slF12));
            if (gameF13 != nullptr) {
                std::vector<uint8_t> saved((uint8_t*)gameF13, (uint8_t*)gameF13 + sizeof(*gameF13));
                auto* d = (VkBool32*)&gameF13->robustImageAccess;
                auto* s = (VkBool32 const*)&slF13.robustImageAccess;
                auto count = (sizeof(*gameF13) - offsetof(VkPhysicalDeviceVulkan13Features, robustImageAccess)) / sizeof(VkBool32);
                for (size_t i = 0; i < count; i++) d[i] |= s[i];
                savedF13 = std::move(saved);
            }

            VkDeviceCreateInfo extended = *pCreateInfo;
            // Prepend structs the game does not chain (head insertion, no cloning).
            if (gameF12 == nullptr && !reqs.features12.empty()) { slF12.pNext = const_cast<void*>(extended.pNext); extended.pNext = &slF12; }
            if (gameF13 == nullptr && !reqs.features13.empty()) { slF13.pNext = const_cast<void*>(extended.pNext); extended.pNext = &slF13; }

            // --- queues: extend counts, record SL slots ---
            uint32_t familyCount{ 0 };
            vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, families.data());

            auto findFamily = [&](VkQueueFlags required, VkQueueFlags preferAbsent) -> uint32_t {
                uint32_t fallback = ~0u;
                for (uint32_t i = 0; i < familyCount; i++) {
                    if ((families[i].queueFlags & required) != required) continue;
                    if ((families[i].queueFlags & preferAbsent) == 0) return i;
                    if (fallback == ~0u) fallback = i;
                }
                return fallback;
            };

            std::vector<VkDeviceQueueCreateInfo> queues(
                pCreateInfo->pQueueCreateInfos,
                pCreateInfo->pQueueCreateInfos + pCreateInfo->queueCreateInfoCount);
            std::vector<std::vector<float>> priorityStorage;
            bool queueFailure = false;

            auto addQueues = [&](uint32_t family, uint32_t extra, uint32_t& outIndex) {
                if (extra == 0 || family == ~0u) { if (extra > 0) queueFailure = true; return; }
                for (auto& q : queues) {
                    if (q.queueFamilyIndex != family) continue;
                    if (q.queueCount + extra > families[family].queueCount) { queueFailure = true; return; }
                    outIndex = q.queueCount;
                    priorityStorage.emplace_back(q.queueCount + extra, 1.0f);
                    if (q.pQueuePriorities != nullptr)
                        std::copy(q.pQueuePriorities, q.pQueuePriorities + q.queueCount, priorityStorage.back().begin());
                    q.queueCount += extra;
                    q.pQueuePriorities = priorityStorage.back().data();
                    return;
                }
                if (extra > families[family].queueCount) { queueFailure = true; return; }
                outIndex = 0;
                priorityStorage.emplace_back(extra, 1.0f);
                queues.push_back(VkDeviceQueueCreateInfo{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                    nullptr, 0, family, extra, priorityStorage.back().data() });
            };

            slQueueSlots_ = {};
            slQueueSlots_.graphicsFamily = findFamily(VK_QUEUE_GRAPHICS_BIT, 0);
            addQueues(slQueueSlots_.graphicsFamily, reqs.graphicsQueues, slQueueSlots_.graphicsIndex);
            slQueueSlots_.computeFamily = findFamily(VK_QUEUE_COMPUTE_BIT, VK_QUEUE_GRAPHICS_BIT);
            addQueues(slQueueSlots_.computeFamily, reqs.computeQueues, slQueueSlots_.computeIndex);
            if (reqs.opticalFlowQueues > 0) {
                slQueueSlots_.opticalFlowFamily = findFamily(VK_QUEUE_OPTICAL_FLOW_BIT_NV, 0);
                slQueueSlots_.opticalFlowNative = slQueueSlots_.opticalFlowFamily != ~0u;
                addQueues(slQueueSlots_.opticalFlowFamily, reqs.opticalFlowQueues, slQueueSlots_.opticalFlowIndex);
            }

            extended.enabledExtensionCount = (uint32_t)extensions.size();
            extended.ppEnabledExtensionNames = extensions.data();
            extended.queueCreateInfoCount = (uint32_t)queues.size();
            extended.pQueueCreateInfos = queues.data();

            if (queueFailure) {
                streamline_.Disable("required SL queue does not fit family limits");
                result = orig(physicalDevice, pCreateInfo, pAllocator, pDevice);
            } else {
                INFO("SL: device create extended: +%u ext, queues g=%u@%u c=%u@%u ofa=%u@%u",
                    (unsigned)(extensions.size() - pCreateInfo->enabledExtensionCount),
                    slQueueSlots_.graphicsFamily, slQueueSlots_.graphicsIndex,
                    slQueueSlots_.computeFamily, slQueueSlots_.computeIndex,
                    slQueueSlots_.opticalFlowFamily, slQueueSlots_.opticalFlowIndex);
                result = orig(physicalDevice, &extended, pAllocator, pDevice);
                if (result != VK_SUCCESS) {
                    streamline_.Disable("extended vkCreateDevice failed, retrying vanilla");
                    result = orig(physicalDevice, pCreateInfo, pAllocator, pDevice);
                }
            }

            // Restore game-owned structs regardless of outcome.
            if (gameF12 != nullptr) std::copy(savedF12.begin(), savedF12.end(), (uint8_t*)gameF12);
            if (gameF13 != nullptr) std::copy(savedF13.begin(), savedF13.end(), (uint8_t*)gameF13);
        } else {
            result = orig(physicalDevice, pCreateInfo, pAllocator, pDevice);
        }
```

Add `#include <External/streamline/include/sl_helpers_vk.h>` in `Streamline.h` directly below the `sl.h` include.

If the vendored Vulkan headers predate VK_NV_optical_flow, add above the wrapper:

```cpp
#ifndef VK_QUEUE_OPTICAL_FLOW_BIT_NV
#define VK_QUEUE_OPTICAL_FLOW_BIT_NV 0x00000100
#endif
```

- [ ] **Step 3: Build**

Run the build+deploy command. Expected `DONE`. Fix compile errors only within this task's code.

- [ ] **Step 4: Commit**

```bash
cd /mnt/c/Dev/bg3se-upscaler && git add -A && git commit -m "feat(dlss-fg): additive device surgery - extensions, 1.2/1.3 features (mutate-and-restore), SL queues" && git push origin dlss-fg
```

---

### Task 5: Handoff + verification protocol

**Files:**
- Modify: `BG3Extender/Extender/Client/IMGUI/Streamline.h` (`HandOffDevice`)
- Modify: `BG3Extender/Extender/Client/IMGUI/Vulkan.inl` (call site)

**Interfaces:**
- Consumes: `slQueueSlots_` (Task 4), `sl::VulkanInfo` (kStructVersion3, `sl_helpers_vk.h`), `PFun_slSetVulkanInfo`.
- Produces: `bool StreamlineManager::HandOffDevice(VkInstance, VkPhysicalDevice, VkDevice, SLQueueSlots const&)` — phase 2 builds on a `Ready()`+handed-off SL.

- [ ] **Step 1: Resolve and store `slSetVulkanInfo`**

In `Load()`, with the other GetProcs: `slSetVulkanInfo_ = GetProc<PFun_slSetVulkanInfo>("slSetVulkanInfo");` and member `PFun_slSetVulkanInfo* slSetVulkanInfo_{ nullptr };`. Move (or duplicate) the `SLQueueSlots` struct definition into `Streamline.h` above the class so both files share one definition (delete the Vulkan.inl copy; `Vulkan.inl` includes `Streamline.h`).

- [ ] **Step 2: Implement `HandOffDevice`**

```cpp
    bool HandOffDevice(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, SLQueueSlots const& slots)
    {
        if (!Ready() || slSetVulkanInfo_ == nullptr) return false;

        sl::VulkanInfo info{};
        info.instance = instance;
        info.physicalDevice = physicalDevice;
        info.device = device;
        info.graphicsQueueFamily = slots.graphicsFamily;
        info.graphicsQueueIndex = slots.graphicsIndex;
        info.computeQueueFamily = slots.computeFamily;
        info.computeQueueIndex = slots.computeIndex;
        info.opticalFlowQueueFamily = slots.opticalFlowFamily == ~0u ? 0 : slots.opticalFlowFamily;
        info.opticalFlowQueueIndex = slots.opticalFlowIndex;
        info.useNativeOpticalFlowMode = slots.opticalFlowNative;

        auto result = slSetVulkanInfo_(info);
        if (result != sl::Result::eOk) {
            Disable("slSetVulkanInfo failed");
            Note("SL: ERROR: slSetVulkanInfo -> %d", (int)result);
            return false;
        }
        Note("SL: slSetVulkanInfo ok (g %u@%u, c %u@%u, ofa %u@%u native=%d)",
            slots.graphicsFamily, slots.graphicsIndex, slots.computeFamily, slots.computeIndex,
            slots.opticalFlowFamily, slots.opticalFlowIndex, slots.opticalFlowNative ? 1 : 0);
        return true;
    }
```

- [ ] **Step 3: Call it from the device wrapper**

In `vkCreateDeviceWrapped`, inside the existing `if (result == VK_SUCCESS)` block, before `LogFeatureSupport`:

```cpp
            if (streamline_.Ready()) {
                streamline_.HandOffDevice(instance_, physicalDevice, *pDevice, slQueueSlots_);
            }
```

- [ ] **Step 4: Build**

Run the build+deploy command. Expected `DONE`.

- [ ] **Step 5: LAUNCH CHECKPOINT B — gate off (requires human)**

Set `"StreamlineEnabled": false`; delete `SE-Streamline.log`; human launches, plays a minute, alt-tabs, quits. Verify: `SE-Streamline.log` was not recreated; no new dumps; behavior identical to vanilla.

- [ ] **Step 6: LAUNCH CHECKPOINT C — gate on (requires human)**

Set `"StreamlineEnabled": true`; delete `SE-Streamline.log`; human launches to gameplay. Verify in log: `slInit ok` → requirements lists → `SL: instance create extended` → `SL: device create extended` → `slSetVulkanInfo ok` → `DLSS-G supported`. No dumps.

- [ ] **Step 7: LAUNCH CHECKPOINT D — gate on, stress (requires human)**

Same settings; human loads a save fully, alt-tabs twice, plays several minutes, quits from menu. Verify: no dumps, no device loss, no SL `ERROR` lines after the handoff.

- [ ] **Step 8: Commit + record results**

```bash
cd /mnt/c/Dev/bg3se-upscaler && git add -A && git commit -m "feat(dlss-fg): slSetVulkanInfo handoff - phase 1 complete" && git push origin dlss-fg
```

Append checkpoint outcomes (pass/fail + log excerpts) to the spec under a `## Phase-1 verification results` heading, commit as `docs: phase-1 verification results`.

#pragma once

// NVIDIA Streamline integration, driven by the extender itself (no third-party injector).
//
// Streamline is initialized with eUseManualHooking; the game's own loader calls
// vkCreateInstance and vkCreateDevice extended with SL's requirements, and handles
// are handed over via slSetVulkanInfo. Feature plugins (sl.dlss_g.dll etc.) load
// from the same folder as the interposer.
//
// Spike scope: init + feature support reporting only. No frame data, no tags, no FG
// activation yet.

#include <vulkan/vulkan.h>
#include <External/streamline/include/sl.h>
#include <External/streamline/include/sl_helpers_vk.h>
#include <External/streamline/include/sl_dlss_g.h>
#include <Extender/ScriptExtender.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <mutex>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

BEGIN_NS(extui)

// Vulkan queue family/index slots SL's extended device create-info reserved for its own use,
// recorded by the device create-info surgery in Vulkan.inl and handed to slSetVulkanInfo once
// the real VkDevice exists.
struct SLQueueSlots
{
    uint32_t graphicsFamily{ ~0u };
    uint32_t graphicsIndex{};
    uint32_t computeFamily{ ~0u };
    uint32_t computeIndex{};
    uint32_t opticalFlowFamily{ ~0u };
    uint32_t opticalFlowIndex{};
    bool opticalFlowNative{};
};

// Thread-safe snapshot of the game's active camera, written once per tick on the game/update
// thread (ScriptExtender::OnUpdateGuarded, ScriptExtenderClient.cpp) and read later by the
// render thread (Vulkan/NGX hook). Component reads are NOT safe on the render thread - see
// Docs/superpowers/2026-08-15-phase2b-investigation.md Q2. When the active camera cannot be
// resolved, `valid` stays false and every other field is left at its identity/zero default;
// consumers must skip constants when `valid` is false rather than trust zeroed matrices.
struct CameraSnapshot
{
    glm::mat4 view{ 1.0f };
    glm::mat4 invView{ 1.0f };
    glm::mat4 proj{ 1.0f };
    glm::mat4 invProj{ 1.0f };
    glm::vec3 pos{ 0.0f };
    glm::vec3 up{ 0.0f };
    glm::vec3 right{ 0.0f };
    glm::vec3 fwd{ 0.0f };
    float nearP{ 0.0f };
    float farP{ 0.0f };
    float fov{ 0.0f };
    float aspect{ 0.0f };
    bool valid{ false };
};

// DLSS-SR input resources/scalars read from the NGX EvaluateFeature parameter block by
// VulkanBackend::readNgxFrameInputs() (Vulkan.inl) - render-thread-local, written and read only
// from inside the NGX hook (never touched by the game thread), so unlike CameraSnapshot above it
// needs no lock. Lives here (not nested inside VulkanBackend) purely so StreamlineManager::
// SubmitFrameData below can take it as an ordinary by-value/by-ref parameter without a
// forward-declaration dance across the two files.
struct NgxFrameInputs
{
    VkImage depthImage{ VK_NULL_HANDLE };
    VkImageView depthView{ VK_NULL_HANDLE };
    VkFormat depthFormat{ VK_FORMAT_UNDEFINED };
    uint32_t depthW{ 0 };
    uint32_t depthH{ 0 };
    VkImage mvecImage{ VK_NULL_HANDLE };
    VkImageView mvecView{ VK_NULL_HANDLE };
    VkFormat mvecFormat{ VK_FORMAT_UNDEFINED };
    uint32_t mvecW{ 0 };
    uint32_t mvecH{ 0 };
    float jitterX{ 0.0f };
    float jitterY{ 0.0f };
    float mvScaleX{ 0.0f };
    float mvScaleY{ 0.0f };
    uint32_t reset{ 0 };
    bool valid{ false };
};

class StreamlineManager
{
public:
    // The extender's console does not exist yet when Load()/Init() run at startup, and its
    // logger silently drops messages until it does. Everything is buffered and re-emitted by
    // FlushBootLog() once the console is alive (first device creation).
    void Note(char const* fmt, ...)
    {
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        _vsnprintf_s(buf, std::size(buf), _TRUNCATE, fmt, args);
        va_end(args);
        {
            std::lock_guard _(bootLogLock_);
            bootLog_.push_back(buf);
        }
        INFO("%s", buf);
        AppendToFile(buf);
    }

    // The console drops output until it exists and the runtime log opens even later, so the
    // boot sequence also goes to a file of our own next to the game binary. Truncated on the
    // first write of each session.
    void AppendToFile(char const* line)
    {
        if (logPath_.empty()) {
            wchar_t path[MAX_PATH]{};
            if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return;
            if (auto slash = wcsrchr(path, L'\\')) *slash = L'\0';
            logPath_ = std::wstring(path) + L"\\SE-Streamline.log";
        }

        std::lock_guard _(fileLock_);
        auto file = _wfsopen(logPath_.c_str(), fileTruncated_ ? L"ab" : L"wb", _SH_DENYNO);
        fileTruncated_ = true;
        if (file == nullptr) return;
        fwrite(line, 1, strlen(line), file);
        fwrite("\r\n", 1, 2, file);
        fclose(file);
    }

    void FlushBootLog()
    {
        // The logger silently drops output until the console exists; keep the buffer until
        // there is something real to print to. Called every frame until it succeeds.
        if (gCoreLibPlatformInterface.GlobalConsole == nullptr) return;

        std::lock_guard _(bootLogLock_);
        if (bootLog_.empty()) return;
        INFO("SL: --- buffered boot log (%u lines) ---", (unsigned)bootLog_.size());
        for (auto const& line : bootLog_) {
            INFO("%s", line.c_str());
        }
        bootLog_.clear();
    }

    bool Load()
    {
        if (module_ != nullptr) return true;
        if (!gExtender->GetConfig().StreamlineEnabled) return false;
        gInstance = this;

        // Prefer the Streamline runtime the upscaler package ships; the interposer finds
        // sl.common.dll and the feature plugins next to itself.
        // Our own matched Streamline runtime (SDK release, all components one version) has
        // priority; mixing the 2.10.3 interposer from the upscaler package with 2.11 OTA
        // plugins crashed with a null call inside the plugin/interposer handshake.
        wchar_t path[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, path, MAX_PATH) > 0) {
            if (auto slash = wcsrchr(path, L'\\')) *slash = L'\0';
            for (auto dir : { L"\\mods\\BG3SE-Streamline", L"\\mods\\UpscalerBasePlugin\\Streamline" }) {
                streamlineDir_ = std::wstring(path) + dir;
                auto interposer = streamlineDir_ + L"\\sl.interposer.dll";
                module_ = LoadLibraryW(interposer.c_str());
                if (module_ != nullptr) break;
            }
        }

        if (module_ == nullptr) {
            // Fall back to whatever the search path finds (e.g. a copy next to bin\)
            module_ = LoadLibraryW(L"sl.interposer.dll");
            streamlineDir_.clear();
        }

        if (module_ == nullptr) {
            Note("SL: ERROR: sl.interposer.dll not found; Streamline disabled");
            return false;
        }

        slInit_ = GetProc<PFun_slInit>("slInit");
        slShutdown_ = GetProc<PFun_slShutdown>("slShutdown");
        slIsFeatureSupported_ = GetProc<PFun_slIsFeatureSupported>("slIsFeatureSupported");
        slIsFeatureLoaded_ = GetProc<PFun_slIsFeatureLoaded>("slIsFeatureLoaded");
        slGetFeatureVersion_ = GetProc<PFun_slGetFeatureVersion>("slGetFeatureVersion");
        slGetFeatureRequirements_ = GetProc<PFun_slGetFeatureRequirements>("slGetFeatureRequirements");
        slSetVulkanInfo_ = GetProc<PFun_slSetVulkanInfo>("slSetVulkanInfo");
        slGetFeatureFunction_ = GetProc<PFun_slGetFeatureFunction>("slGetFeatureFunction");
        // Frame-data pipeline (Task 3): tag depth/mvec + push camera constants for DLSS-G.
        // Resolved best-effort here, like the others; SubmitFrameData logs once and no-ops if any
        // of these is missing rather than gating the whole plugin load on them.
        slGetNewFrameToken_ = GetProc<PFun_slGetNewFrameToken>("slGetNewFrameToken");
        slSetTagForFrame_ = GetProc<PFun_slSetTagForFrame>("slSetTagForFrame");
        slSetConstants_ = GetProc<PFun_slSetConstants>("slSetConstants");

        if (!slInit_ || !slIsFeatureSupported_) {
            Note("SL: ERROR: sl.interposer.dll is missing expected exports; Streamline disabled");
            module_ = nullptr;
            return false;
        }

        Note("SL: interposer loaded from %S", streamlineDir_.empty() ? L"(search path)" : streamlineDir_.c_str());
        return true;
    }

    bool Init()
    {
        if (initialized_) return true;
        if (module_ == nullptr) return false;

        static sl::Feature features[] = { sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };
        static const wchar_t* pluginPaths[1];

        sl::Preferences pref{};
        pref.showConsole = false;
        pref.logLevel = sl::LogLevel::eVerbose;
        pref.logMessageCallback = &LogCallback;
        // OTA off again, deliberately: we now ship a version-matched 2.12.0 runtime
        // (interposer + plugins + nvngx_dlssg from the SDK release), and the crash this
        // replaces came precisely from mixing the interposer with newer OTA plugins.
        pref.flags = sl::PreferenceFlags::eDisableCLStateTracking
            | sl::PreferenceFlags::eUseManualHooking
            // Required: we tag depth/mvec per-frame via slSetTagForFrame. Without this flag
            // slSetTagForFrame returns error 19 and DLSS-G gets no inputs (no frames generated).
            | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
        pref.featuresToLoad = features;
        pref.numFeaturesToLoad = (uint32_t)std::size(features);
        // 0xE658703: the app id family NVIDIA's driver/NGX on this machine already serves
        // (nvngx_config.txt lists app_E6587xx, and the OTA plugin cache is keyed 1B0_E658703 -
        // the id the working PureDark/nvapp stack ran under). The SDK sample id 231313132 has
        // no NGX min-spec data, which made every plugin fall back to stale defaults and
        // self-disable on this GPU.
        pref.applicationId = 0xE658703;
        pref.renderAPI = sl::RenderAPI::eVulkan;
        if (!streamlineDir_.empty()) {
            pluginPaths[0] = streamlineDir_.c_str();
            pref.pathsToPlugins = pluginPaths;
            pref.numPathsToPlugins = 1;
        }

        auto result = slInit_(pref, sl::kSDKVersion);
        if (result != sl::Result::eOk) {
            Note("SL: ERROR: slInit failed: %d (header SDK %u.%u.%u vs runtime on disk - see SL log lines above)",
                (int)result, SL_VERSION_MAJOR, SL_VERSION_MINOR, SL_VERSION_PATCH);
            return false;
        }

        Note("SL: slInit ok (SDK headers %u.%u.%u)", SL_VERSION_MAJOR, SL_VERSION_MINOR, SL_VERSION_PATCH);
        initialized_ = true;

        static sl::Feature const wanted[] = { sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };
        auto addUnique = [](std::vector<std::string>& into, char const* value) {
            for (auto const& existing : into) if (existing == value) return;
            into.push_back(value);
        };
        bool anyReqOk = false;
        for (auto feature : wanted) {
            sl::FeatureRequirements req{};
            auto reqResult = slGetFeatureRequirements_ ? slGetFeatureRequirements_(feature, req) : sl::Result::eErrorNotInitialized;
            if (reqResult != sl::Result::eOk) {
                Note("SL: WARN: slGetFeatureRequirements(%u) -> %d", feature, (int)reqResult);
                continue;
            }
            anyReqOk = true;
            for (uint32_t i = 0; i < req.vkNumInstanceExtensions; i++) addUnique(requirements_.instanceExtensions, req.vkInstanceExtensions[i]);
            for (uint32_t i = 0; i < req.vkNumDeviceExtensions; i++) addUnique(requirements_.deviceExtensions, req.vkDeviceExtensions[i]);
            for (uint32_t i = 0; i < req.vkNumFeatures12; i++) addUnique(requirements_.features12, req.vkFeatures12[i]);
            for (uint32_t i = 0; i < req.vkNumFeatures13; i++) addUnique(requirements_.features13, req.vkFeatures13[i]);
            requirements_.graphicsQueues = std::max(requirements_.graphicsQueues, req.vkNumGraphicsQueuesRequired);
            requirements_.computeQueues = std::max(requirements_.computeQueues, req.vkNumComputeQueuesRequired);
            requirements_.opticalFlowQueues = std::max(requirements_.opticalFlowQueues, req.vkNumOpticalFlowQueuesRequired);
        }
        requirements_.valid = anyReqOk;
        Note("SL: requirements: %u instance ext, %u device ext, %u feat12, %u feat13, queues g=%u c=%u ofa=%u",
            (unsigned)requirements_.instanceExtensions.size(), (unsigned)requirements_.deviceExtensions.size(),
            (unsigned)requirements_.features12.size(), (unsigned)requirements_.features13.size(),
            requirements_.graphicsQueues, requirements_.computeQueues, requirements_.opticalFlowQueues);
        for (auto const& e : requirements_.instanceExtensions) Note("SL:   instance ext: %s", e.c_str());
        for (auto const& e : requirements_.deviceExtensions) Note("SL:   device ext: %s", e.c_str());
        for (auto const& f : requirements_.features12) Note("SL:   feature12: %s", f.c_str());
        for (auto const& f : requirements_.features13) Note("SL:   feature13: %s", f.c_str());

        return true;
    }

    void LogFeatureSupport(VkPhysicalDevice physicalDevice)
    {
        if (!Ready() || featureSupportLogged_) return;
        featureSupportLogged_ = true;

        sl::AdapterInfo adapter{};
        adapter.vkPhysicalDevice = physicalDevice;

        struct { sl::Feature id; char const* name; } const features[] = {
            { sl::kFeatureDLSS_G, "DLSS-G" },
            { sl::kFeatureReflex, "Reflex" },
            { sl::kFeaturePCL, "PCL" },
        };

        for (auto const& feature : features) {
            auto result = slIsFeatureSupported_(feature.id, adapter);
            if (result == sl::Result::eOk) {
                sl::FeatureVersion version{};
                if (slGetFeatureVersion_ != nullptr && slGetFeatureVersion_(feature.id, version) == sl::Result::eOk) {
                    Note("SL: %s supported (SL %u.%u.%u, NGX %u.%u.%u)", feature.name,
                        version.versionSL.major, version.versionSL.minor, version.versionSL.build,
                        version.versionNGX.major, version.versionNGX.minor, version.versionNGX.build);
                } else {
                    Note("SL: %s supported", feature.name);
                }
            } else {
                Note("SL: ERROR: %s NOT supported: %d", feature.name, (int)result);
            }
        }
    }

    bool HandOffDevice(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, SLQueueSlots const& slots)
    {
        if (!Ready()) return false;
        if (slSetVulkanInfo_ == nullptr) {
            Disable("slSetVulkanInfo export missing");
            return false;
        }

        sl::VulkanInfo info{};
        info.instance = instance;
        info.physicalDevice = physicalDevice;
        info.device = device;
        info.graphicsQueueFamily = slots.graphicsFamily == ~0u ? 0 : slots.graphicsFamily;
        info.graphicsQueueIndex = slots.graphicsIndex;
        info.computeQueueFamily = slots.computeFamily == ~0u ? 0 : slots.computeFamily;
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
        ResolveDLSSGFunctions();
        return true;
    }

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

    bool Ready() const { return initialized_ && !disabled_; }
    HMODULE Module() const { return module_; }

    // Game thread only: called once per tick from ScriptExtender::OnUpdateGuarded with a
    // freshly-resolved snapshot (or a default-constructed, valid=false one if the active
    // camera couldn't be resolved that tick).
    void SetCameraSnapshot(CameraSnapshot const& s) { std::lock_guard _(cameraMutex_); cameraSnapshot_ = s; }

    // Safe to call from any thread, including the render/Vulkan/NGX hook - this is the whole
    // point of the snapshot (see CameraSnapshot's comment and the Q2 investigation doc).
    CameraSnapshot GetCameraSnapshot() { std::lock_guard _(cameraMutex_); return cameraSnapshot_; }

    // Render-thread only (called from ngxEvaluateFeatureCHook, Vulkan.inl). Assembles this
    // frame's DLSS-G inputs - tags depth+mvec via slSetTagForFrame, builds sl::Constants from the
    // camera snapshot + NGX scalars, and pushes them via slSetConstants. Always-boot: any failure
    // (missing export, bad token, tag/constants error) logs once via Note() and returns false -
    // never crashes, never blocks the caller's NGX EvaluateFeature call either way.
    bool SubmitFrameData(VkCommandBuffer cmd, NgxFrameInputs const& in, uint32_t frameIndex)
    {
        if (!Ready()) return false;

        if (!slGetNewFrameToken_ || !slSetTagForFrame_ || !slSetConstants_) {
            if (!frameDataFnsMissingLogged_) {
                frameDataFnsMissingLogged_ = true;
                Note("SL: ERROR: slGetNewFrameToken/slSetTagForFrame/slSetConstants not resolved "
                    "(sl.interposer.dll export missing?); FG frame data will never be submitted");
            }
            return false;
        }

        auto snapshot = GetCameraSnapshot();
        if (!snapshot.valid) {
            if (!frameDataSnapshotInvalidLogged_) {
                frameDataSnapshotInvalidLogged_ = true;
                Note("SL: WARN: camera snapshot not valid yet; skipping FG frame data this frame "
                    "(this is expected for the first few frames after boot)");
            }
            return false;
        }

        sl::FrameToken* token = nullptr;
        auto tokRes = slGetNewFrameToken_(token, &frameIndex);
        if (tokRes != sl::Result::eOk || token == nullptr) {
            if (!frameDataTokenFailLogged_) {
                frameDataTokenFailLogged_ = true;
                Note("SL: ERROR: slGetNewFrameToken failed: %d", (int)tokRes);
            }
            return false;
        }

        sl::ViewportHandle viewport{ 0 };

        // NGX hands us the depth/mvec VkImage + VkImageView, never the backing VkDeviceMemory -
        // there is no NGX parameter for it. Per the sl::Resource ctor doc comment (sl_core_types.h,
        // "Resource view, description etc. are MANDATORY only when using Vulkan"), memory is not
        // called out as mandatory there, so we pass null. UNVERIFIED against real hardware -
        // flagged for the human tuning pass: if slSetTagForFrame starts failing/rejecting once FG
        // is actually exercised, this is the first thing to check.
        if (!ngxMemoryNullNotedLogged_) {
            ngxMemoryNullNotedLogged_ = true;
            Note("SL: NOTE: tagging depth/mvec with VkDeviceMemory=null (NGX does not expose the "
                "backing memory handle) - revisit if DLSS-G rejects the tags");
        }

        // Layout assumption, also UNVERIFIED - NgxFrameInputs does not capture the resources'
        // current VkImageLayout (Task 2 only read image/view/format/extent). Depth uses the
        // depth-read-only layout, mvec (a plain R16G16 color image per the phase-2a checkpoint)
        // uses shader-read-only; both are the conventional "sampled, read-only input" layouts.
        // Prime tuning suspect alongside the matrix conventions below if tagging fails silently.
        sl::Resource depthRes(sl::ResourceType::eTex2d, (void*)in.depthImage, (void*)nullptr,
            (void*)in.depthView, (uint32_t)VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        sl::Resource mvecRes(sl::ResourceType::eTex2d, (void*)in.mvecImage, (void*)nullptr,
            (void*)in.mvecView, (uint32_t)VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        sl::Extent depthExtent{ 0, 0, in.depthW, in.depthH };
        sl::Extent mvecExtent{ 0, 0, in.mvecW, in.mvecH };

        sl::ResourceTag tags[] = {
            sl::ResourceTag(&depthRes, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &depthExtent),
            sl::ResourceTag(&mvecRes, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &mvecExtent),
        };

        auto tagRes = slSetTagForFrame_(*token, viewport, tags, (uint32_t)std::size(tags),
            reinterpret_cast<sl::CommandBuffer*>(cmd));
        if (tagRes != sl::Result::eOk) {
            if (!frameDataTagFailLogged_) {
                frameDataTagFailLogged_ = true;
                Note("SL: ERROR: slSetTagForFrame failed: %d", (int)tagRes);
            }
            // Fall through and still try slSetConstants - a partial submission surfaces more in
            // the log (and the debug overlay's status line) than bailing out entirely would.
        }

        // glm order: view then project (v_clip = proj * view * v_world), matching CameraSnapshot's
        // proj/view fields (Task 1).
        glm::mat4 viewProj = snapshot.proj * snapshot.view;
        bool identityReproj = gExtender->GetConfig().StreamlineFGIdentityReproj;

        sl::Constants consts{};
        consts.cameraViewToClip = ToSLMatrix(snapshot.proj);
        consts.clipToCameraView = ToSLMatrix(snapshot.invProj);

        if (identityReproj || !havePrevViewProj_) {
            // First frame ever (no previous view-proj to reproject from) or the debug lever is on:
            // identity disables reprojection so FG should still generate frames, just without
            // motion-compensated interpolation - a clean baseline for isolating tag/depth bugs
            // from matrix-convention bugs (see StreamlineFGIdentityReproj's comment).
            glm::mat4 id{ 1.0f };
            consts.clipToPrevClip = ToSLMatrix(id);
            consts.prevClipToClip = ToSLMatrix(id);
        } else {
            glm::mat4 clipToPrevClip = prevViewProj_ * glm::inverse(viewProj);
            consts.clipToPrevClip = ToSLMatrix(clipToPrevClip);
            consts.prevClipToClip = ToSLMatrix(glm::inverse(clipToPrevClip));
        }

        consts.cameraPinholeOffset = sl::float2(0.0f, 0.0f);   // no lens shift; SL warns if left invalid
        consts.jitterOffset = sl::float2(in.jitterX, in.jitterY);
        // Raw NGX values, already (-1,-1) per the phase-2a checkpoint (BG3 mvecs negated per axis).
        consts.mvecScale = sl::float2(in.mvScaleX, in.mvScaleY);
        consts.cameraPos = sl::float3(snapshot.pos.x, snapshot.pos.y, snapshot.pos.z);
        consts.cameraUp = sl::float3(snapshot.up.x, snapshot.up.y, snapshot.up.z);
        consts.cameraRight = sl::float3(snapshot.right.x, snapshot.right.y, snapshot.right.z);
        consts.cameraFwd = sl::float3(snapshot.fwd.x, snapshot.fwd.y, snapshot.fwd.z);
        consts.cameraNear = snapshot.nearP;
        consts.cameraFar = snapshot.farP;
        consts.cameraFOV = snapshot.fov;              // radians - confirmed by the phase-2a checkpoint (~0.84 = 48deg); SL's header documents cameraFOV as radians too
        consts.cameraAspectRatio = snapshot.aspect;

        // Start conservative per the brief; proj[2][2]'s sign is the usual reverse-Z tell but
        // hasn't been cross-checked against a captured BG3 projection matrix here - flip during
        // tuning if depth-driven artifacts (haloing at depth discontinuities) show up.
        consts.depthInverted = sl::Boolean::eFalse;
        consts.cameraMotionIncluded = sl::Boolean::eTrue;
        consts.motionVectors3D = sl::Boolean::eFalse;      // BG3 mvecs are 2D screen-space (R16G16_SFLOAT, phase-2a checkpoint)
        consts.motionVectorsJittered = sl::Boolean::eFalse;
        consts.reset = in.reset != 0 ? sl::Boolean::eTrue : sl::Boolean::eFalse;

        auto constRes = slSetConstants_(consts, *token, viewport);
        if (constRes != sl::Result::eOk) {
            if (!frameDataConstantsFailLogged_) {
                frameDataConstantsFailLogged_ = true;
                Note("SL: ERROR: slSetConstants failed: %d", (int)constRes);
            }
        }

        if (gExtender->GetConfig().StreamlineFGDebugConstants && !debugConstantsDumped_) {
            debugConstantsDumped_ = true;
            DumpConstants(consts, snapshot, in);
        }

        prevViewProj_ = viewProj;
        havePrevViewProj_ = true;

        return tagRes == sl::Result::eOk && constRes == sl::Result::eOk;
    }

    // The extender only ever creates one live StreamlineManager (VulkanBackend::streamline_,
    // Vulkan.inl). gInstance is set as soon as Load() gets past the StreamlineEnabled gate, so
    // this is a convenient global accessor for game-thread code (ScriptExtenderClient.cpp) that
    // does not otherwise have a handle to the active VulkanBackend/IMGUIManager. Returns nullptr
    // if Streamline was never loaded (e.g. StreamlineEnabled=false, or DX11 backend).
    static StreamlineManager* Get() { return gInstance; }

private:
    template <class T>
    T* GetProc(char const* name)
    {
        return reinterpret_cast<T*>(GetProcAddress(module_, name));
    }

    static void LogCallback(sl::LogType type, char const* msg)
    {
        // SL terminates its messages with a newline; the console adds its own.
        auto len = msg ? strlen(msg) : 0;
        if (len > 0 && msg[len - 1] == '\n') len--;
        char const* prefix = type == sl::LogType::eError ? "SL: ERROR: "
            : type == sl::LogType::eWarn ? "SL: WARN: " : "SL: ";
        if (gInstance != nullptr) {
            gInstance->Note("%s%.*s", prefix, (int)len, msg);
        } else {
            INFO("%s%.*s", prefix, (int)len, msg);
        }
    }

    // glm is column-major (M[col][row]); sl::float4x4 is row-major (row[4], sl_consts.h says so
    // explicitly) - built explicitly element-by-element here, one clearly-commented spot, rather
    // than any bulk memcpy/reinterpret, precisely so the convention can't silently drift. This is
    // the prime suspect if FG tuning shows garbled/swept-wrong-way matrices: the fix to try first
    // is removing this transpose (i.e. copying rows straight across) rather than adding another.
    static sl::float4x4 ToSLMatrix(glm::mat4 const& m)
    {
        sl::float4x4 out{};
        for (int r = 0; r < 4; r++) {
            out.row[r] = sl::float4(m[0][r], m[1][r], m[2][r], m[3][r]);
        }
        return out;
    }

    // One-shot, gated by StreamlineFGDebugConstants - dumps every sl::Constants field so the
    // human tuning pass can eyeball the actual numbers going to Streamline against what the
    // in-game camera is doing.
    void DumpConstants(sl::Constants const& c, CameraSnapshot const& snap, NgxFrameInputs const& in)
    {
        auto dumpMat = [this](char const* name, sl::float4x4 const& m) {
            Note("SL: FG DEBUG %s row0=(%.4f,%.4f,%.4f,%.4f)", name, m.row[0].x, m.row[0].y, m.row[0].z, m.row[0].w);
            Note("SL: FG DEBUG %s row1=(%.4f,%.4f,%.4f,%.4f)", name, m.row[1].x, m.row[1].y, m.row[1].z, m.row[1].w);
            Note("SL: FG DEBUG %s row2=(%.4f,%.4f,%.4f,%.4f)", name, m.row[2].x, m.row[2].y, m.row[2].z, m.row[2].w);
            Note("SL: FG DEBUG %s row3=(%.4f,%.4f,%.4f,%.4f)", name, m.row[3].x, m.row[3].y, m.row[3].z, m.row[3].w);
        };
        Note("SL: FG DEBUG --- one-shot constants dump ---");
        dumpMat("cameraViewToClip", c.cameraViewToClip);
        dumpMat("clipToCameraView", c.clipToCameraView);
        dumpMat("clipToPrevClip", c.clipToPrevClip);
        dumpMat("prevClipToClip", c.prevClipToClip);
        Note("SL: FG DEBUG jitter=(%.4f,%.4f) mvecScale=(%.4f,%.4f)",
            c.jitterOffset.x, c.jitterOffset.y, c.mvecScale.x, c.mvecScale.y);
        Note("SL: FG DEBUG cameraPos=(%.3f,%.3f,%.3f) up=(%.3f,%.3f,%.3f) right=(%.3f,%.3f,%.3f) fwd=(%.3f,%.3f,%.3f)",
            c.cameraPos.x, c.cameraPos.y, c.cameraPos.z, c.cameraUp.x, c.cameraUp.y, c.cameraUp.z,
            c.cameraRight.x, c.cameraRight.y, c.cameraRight.z, c.cameraFwd.x, c.cameraFwd.y, c.cameraFwd.z);
        Note("SL: FG DEBUG near=%.3f far=%.3f fov=%.4f aspect=%.4f depthInverted=%d cameraMotionIncluded=%d motionVectors3D=%d reset=%d",
            c.cameraNear, c.cameraFar, c.cameraFOV, c.cameraAspectRatio,
            (int)c.depthInverted, (int)c.cameraMotionIncluded, (int)c.motionVectors3D, (int)c.reset);
        Note("SL: FG DEBUG depth %ux%u mvec %ux%u (raw NGX extents this frame)", in.depthW, in.depthH, in.mvecW, in.mvecH);
    }

    static inline StreamlineManager* gInstance{ nullptr };

    std::mutex cameraMutex_;
    CameraSnapshot cameraSnapshot_;

    HMODULE module_{ nullptr };
    std::mutex bootLogLock_;
    std::mutex fileLock_;
    std::vector<std::string> bootLog_;
    std::wstring logPath_;
    bool fileTruncated_{ false };
    std::wstring streamlineDir_;
    bool initialized_{ false };
    bool featureSupportLogged_{ false };
    bool disabled_{ false };
    bool fgActivated_{ false };
    OwnedRequirements requirements_;

    PFun_slInit* slInit_{ nullptr };
    PFun_slShutdown* slShutdown_{ nullptr };
    PFun_slIsFeatureSupported* slIsFeatureSupported_{ nullptr };
    PFun_slIsFeatureLoaded* slIsFeatureLoaded_{ nullptr };
    PFun_slGetFeatureVersion* slGetFeatureVersion_{ nullptr };
    PFun_slGetFeatureRequirements* slGetFeatureRequirements_{ nullptr };
    PFun_slSetVulkanInfo* slSetVulkanInfo_{ nullptr };
    PFun_slGetFeatureFunction* slGetFeatureFunction_{ nullptr };
    sl::Result (*slDLSSGSetOptions_)(const sl::ViewportHandle&, const sl::DLSSGOptions&){ nullptr };
    sl::Result (*slDLSSGGetState_)(const sl::ViewportHandle&, sl::DLSSGState&, const sl::DLSSGOptions*){ nullptr };

    // Frame-data pipeline (Task 3).
    PFun_slGetNewFrameToken* slGetNewFrameToken_{ nullptr };
    PFun_slSetTagForFrame* slSetTagForFrame_{ nullptr };
    PFun_slSetConstants* slSetConstants_{ nullptr };
    // Render-thread-only (SubmitFrameData is only ever called from the NGX hook) - no lock needed,
    // same reasoning as NgxFrameInputs above.
    glm::mat4 prevViewProj_{ 1.0f };
    bool havePrevViewProj_{ false };
    // One-shot log latches, all render-thread-only.
    bool frameDataFnsMissingLogged_{ false };
    bool frameDataSnapshotInvalidLogged_{ false };
    bool frameDataTokenFailLogged_{ false };
    bool frameDataTagFailLogged_{ false };
    bool frameDataConstantsFailLogged_{ false };
    bool ngxMemoryNullNotedLogged_{ false };
    bool debugConstantsDumped_{ false };
};

END_NS()

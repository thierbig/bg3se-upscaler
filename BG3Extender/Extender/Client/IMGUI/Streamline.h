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

#include <External/streamline/include/sl.h>
#include <External/streamline/include/sl_helpers_vk.h>
#include <vulkan/vulkan.h>
#include <Extender/ScriptExtender.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <mutex>
#include <string>
#include <vector>

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
            | sl::PreferenceFlags::eUseManualHooking;
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

    static inline StreamlineManager* gInstance{ nullptr };

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
    OwnedRequirements requirements_;

    PFun_slInit* slInit_{ nullptr };
    PFun_slShutdown* slShutdown_{ nullptr };
    PFun_slIsFeatureSupported* slIsFeatureSupported_{ nullptr };
    PFun_slIsFeatureLoaded* slIsFeatureLoaded_{ nullptr };
    PFun_slGetFeatureVersion* slGetFeatureVersion_{ nullptr };
    PFun_slGetFeatureRequirements* slGetFeatureRequirements_{ nullptr };
    PFun_slSetVulkanInfo* slSetVulkanInfo_{ nullptr };
};

END_NS()

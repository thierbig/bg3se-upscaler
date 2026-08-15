#pragma once

// NVIDIA Streamline integration, driven by the extender itself (no third-party injector).
//
// The extender loads sl.interposer.dll, initializes Streamline, and routes the game's
// Vulkan object creation through the interposer so Streamline can add the device
// extensions and queues its features need. Feature plugins (sl.dlss_g.dll etc.) load
// from the same folder as the interposer.
//
// Spike scope: init + feature support reporting only. No frame data, no tags, no FG
// activation yet.

#include <External/streamline/include/sl.h>
#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <mutex>
#include <string>
#include <vector>

BEGIN_NS(extui)

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
        gInstance = this;

        // Prefer the Streamline runtime the upscaler package ships; the interposer finds
        // sl.common.dll and the feature plugins next to itself.
        wchar_t path[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, path, MAX_PATH) > 0) {
            if (auto slash = wcsrchr(path, L'\\')) *slash = L'\0';
            streamlineDir_ = std::wstring(path) + L"\\mods\\UpscalerBasePlugin\\Streamline";
            auto interposer = streamlineDir_ + L"\\sl.interposer.dll";
            module_ = LoadLibraryW(interposer.c_str());
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

        vkCreateInstanceProxy_ = reinterpret_cast<PFN_vkCreateInstance>(GetProcAddress(module_, "vkCreateInstance"));
        vkCreateDeviceProxy_ = reinterpret_cast<PFN_vkCreateDevice>(GetProcAddress(module_, "vkCreateDevice"));

        if (!slInit_ || !slIsFeatureSupported_ || !vkCreateInstanceProxy_ || !vkCreateDeviceProxy_) {
            Note("SL: ERROR: sl.interposer.dll is missing expected exports; Streamline disabled");
            module_ = nullptr;
            return false;
        }

        Note("SL: interposer loaded from %s", streamlineDir_.empty() ? "search path" : "UpscalerBasePlugin\\Streamline");
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
        // OTA must stay on: the on-disk Streamline plugins (2.10.3) predate this GPU
        // architecture and self-disable on it, while NVIDIA's OTA cache serves 2.11.0 plugins
        // that support it - which is also how the PureDark setup actually ran. Disabling OTA
        // "for determinism" made slInit fail with no loadable plugins.
        pref.flags = sl::PreferenceFlags::eDisableCLStateTracking
            | sl::PreferenceFlags::eAllowOTA
            | sl::PreferenceFlags::eLoadDownloadedPlugins;
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
        return true;
    }

    void LogFeatureSupport(VkPhysicalDevice physicalDevice)
    {
        if (!initialized_ || featureSupportLogged_) return;
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

    bool Ready() const { return initialized_; }
    HMODULE Module() const { return module_; }
    PFN_vkCreateInstance CreateInstanceProxy() const { return initialized_ ? vkCreateInstanceProxy_ : nullptr; }
    PFN_vkCreateDevice CreateDeviceProxy() const { return initialized_ ? vkCreateDeviceProxy_ : nullptr; }

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

    PFun_slInit* slInit_{ nullptr };
    PFun_slShutdown* slShutdown_{ nullptr };
    PFun_slIsFeatureSupported* slIsFeatureSupported_{ nullptr };
    PFun_slIsFeatureLoaded* slIsFeatureLoaded_{ nullptr };
    PFun_slGetFeatureVersion* slGetFeatureVersion_{ nullptr };
    PFun_slGetFeatureRequirements* slGetFeatureRequirements_{ nullptr };

    PFN_vkCreateInstance vkCreateInstanceProxy_{ nullptr };
    PFN_vkCreateDevice vkCreateDeviceProxy_{ nullptr };
};

END_NS()

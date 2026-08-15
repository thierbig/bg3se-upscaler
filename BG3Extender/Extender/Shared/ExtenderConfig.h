#pragma once

#include <cstdint>
#include <string>

BEGIN_SE()

struct ProfilerThreshold
{
    inline constexpr ProfilerThreshold(uint32_t error)
        : Error(error)
    {}

    uint32_t Error{ 0 };
};

struct ExtenderConfig
{
#if defined(OSI_EXTENSION_BUILD)
    bool CreateConsole{ false };
    bool DefaultToClientConsole{ false };
    bool EnableDebugger{ false };
    bool EnableLuaDebugger{ false };
#else
    bool CreateConsole{ true };
    bool DefaultToClientConsole{ false };
    bool EnableDebugger{ true };
    bool EnableLuaDebugger{ true };
#endif

    bool ExtendStory{ false };
    bool LogFailedCompile{ true };
    bool EnableLogging{ false };
    bool LogCompile{ false };
    bool LogRuntime{ false };
    bool SendCrashReports{ true };
    bool ForceCrashReporting{ false };
    bool EnableAchievements{ true };
    bool DisableLauncher{ false };
    bool DisableStoryPatching{ false };
    bool DisableStoryCompilation{ true };
    bool InsanityCheck{ false };
    bool LocalMessagePassing{ true };
    bool Optick{ false };
    bool MergeVirtualTextures{ false };

#if defined(OSI_EXTENSION_BUILD)
#if defined(_DEBUG)
    bool DeveloperMode{ true };
#else
    bool DeveloperMode{ false };
#endif // defined(_DEBUG)
#else
    bool DeveloperMode{ true };
#endif // defined(OSI_EXTENSION_BUILD)

    bool ClearOnReset{ true };
    // DIAGNOSTIC (temporary): how much of the NGX overlay composite to run.
    //   0 = off, 1 = layout barriers only, 2 = + render pass, 3 = + ImGui draw (default)
    uint32_t NgxOverlayStage{ 3 };
    // dlss-fg: master gate for the extender-driven Streamline integration. Off (default)
    // means not one line of SL code runs - no module load, no routing, vanilla behavior.
    bool StreamlineEnabled{ false };
    // dlss-fg phase 2: activate DLSS frame generation (requires StreamlineEnabled). Off = phase-1 plumbing only.
    bool StreamlineFGEnabled{ false };
    // Frames generated between rendered frames: 1=2x, 2=3x, 3=4x. Clamped to device max at runtime.
    uint32_t StreamlineFGFrames{ 1 };
    // dlss-fg phase 2b tuning lever: one-shot Note() dump of the full sl::Constants (all matrices
    // + scalars) the first time SubmitFrameData runs. Off by default - verbose.
    bool StreamlineFGDebugConstants{ false };
    // dlss-fg phase 2b tuning lever: force clipToPrevClip/prevClipToClip to identity, disabling
    // reprojection. Used to isolate matrix-convention bugs (ghosting with real matrices but not
    // with identity) from tag/depth bugs (ghosting either way).
    bool StreamlineFGIdentityReproj{ false };
    uint32_t DebuggerPort{ 9999 };
    uint32_t LuaDebuggerPort{ 9998 };
    uint32_t DebugFlags{ 0 };
    std::wstring LogDirectory;
    std::wstring LuaBuiltinResourceDirectory;
    std::string CustomProfile;

    // Lua profiler configuration
    bool EnablePerfMessages{ true };
    ProfilerThreshold ProfilerLoadThreshold{ 50000 };
    ProfilerThreshold ProfilerLoadCallbackThreshold{ 50000 };
    ProfilerThreshold ProfilerCallbackThreshold{ 5000 };
    ProfilerThreshold ProfilerClientCallbackThreshold{ 2000 };
};

inline bool ProfilerShouldReport(uint64_t took, ExtenderConfig const& config, ProfilerThreshold const& threshold)
{
    return config.EnablePerfMessages
        && took >= threshold.Error;
}

#define PERF_REPORT(type, took, ...) WARN(##__VA_ARGS__)
#define PERF_SHOULD_REPORT(type, took) ProfilerShouldReport((took), gExtender->GetConfig(), gExtender->GetConfig().Profiler##type##Threshold)

END_SE()

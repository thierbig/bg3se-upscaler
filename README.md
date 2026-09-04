# Norbyte's Baldur's Gate 3 Script Extender (fork, archived)

> ## IMPORTANT: this fork is archived. For DLSS Frame Generation on Vulkan, use bg3fgvk.
>
> This repository was a fork of the Script Extender modified for the PureDark upscaler
> compatibility experiments behind [BG3UpscalerProxy](https://github.com/thierbig/BG3UpscalerProxy).
> That approach is superseded by **[bg3fgvk](https://github.com/thierbig/bg3fgvk)**
> ([Nexus Mods](https://www.nexusmods.com/baldursgate3/mods/24804)): free, open-source DLSS Frame
> Generation (x2 / x3 / x4) for Baldur's Gate 3 on Vulkan that works with **Norbyte's stock Script
> Extender and MCM**, so no modified extender is needed any more.
>
> For the Script Extender itself, use the upstream project: [Norbyte/bg3se](https://github.com/Norbyte/bg3se).
> Everything below is the upstream README, kept for reference.

[Downloads available here](https://github.com/Norbyte/bg3se/releases)

The Script Extender adds Lua/Osiris scripting support to the game.
[API Documentation](https://github.com/Norbyte/bg3se/blob/master/Docs/API.md)

### Configuration

The following configuration variables can be set in the `ScriptExtenderSettings.json` file:

| Variable | Type | Default | Description |
|--|--|--|--|
| CreateConsole | Boolean | false | Creates a console window that logs extender internals. Mainly useful for debugging. |
| EnableLogging | Boolean | false | Enable logging of Osiris activity (rule evaluation, queries, etc.) to a log file. |
| LogRuntime | Boolean | false | Log extender console and script output to a log file. |
| LogCompile | Boolean | false | Log Osiris story compilation to a log file. |
| LogFailedCompile | Boolean | true | Log errors during Osiris story compilation to a log file. |
| LogDirectory | String | `My Documents\OsirisLogs` | Directory where the generated Osiris logs will be stored. |
| EnableExtensions | Boolean | true | Make the Osiris extension functionality available ingame or in the editor. |
| SendCrashReports | Boolean | true | Upload minidumps to the crash report collection server after a game crash. |
| ~~DumpNetworkStrings~~ | Boolean | Not implemented yet | Dumps the NetworkFixedString table to `LogDirectory`. Mainly useful for debugging desync issues. |
| DeveloperMode | Boolean | false | Enables various debug functionality for development purposes. |
| DisableModValidation | Boolean | true | Disable module hashing when loading modules. |
| EnableAchievements | Boolean | true | Re-enable achievements for modded games. |
| EnableDebugger | Boolean | false | Enables the Osiris debugger interface |
| DebuggerPort | Integer | 9999 | Port number the Osiris debugger will listen on |
| EnableLuaDebugger | Boolean | false | Enables the Lua debugger interface |
| LuaDebuggerPort | Integer | 9998 | Port number the Lua debugger will listen on  |

### Build Instructions

Download the latest [external dependencies from here](https://nb-stor.s3.eu-central-1.amazonaws.com/bg3-legacy/External.7z) and extract them to the `External/` folder.

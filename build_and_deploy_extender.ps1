param(
    [string]$WorkspaceFolder = $PSScriptRoot,
    [string]$GameBinFolder = "C:\Games\Baldurs Gate 3\bin",
    [string]$GameLocalFolder = "C:\Users\thier\AppData\Local\BG3ScriptExtender\ScriptExtender\32.0.0.0_0feee35b7443d799a48ab0015e5c7cd08c1f16e1fbeebc8bcb19edcf75b3623b",
    # The updater DLL in the game bin folder is part of the working install; only
    # redeploy it when explicitly asked.
    [switch]$DeployUpdater,
    # Skip the generate-* steps when no property map / proto inputs changed.
    [switch]$SkipCodegen
)

$ErrorActionPreference = 'Stop'

# Local VS2022 ships the v143 toolset only; the project files declare v145 (VS18),
# so pin v143 exactly like CI does.
$MSBuild = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
$Toolset = "/p:PlatformToolset=v143"

function Invoke-Step([string]$Name, [scriptblock]$Body) {
    Write-Host "== $Name" -ForegroundColor Cyan
    & $Body
    if ($LASTEXITCODE -ne 0) { Write-Error "$Name FAILED"; exit 1 }
    Write-Host "== $Name OK" -ForegroundColor Green
}

if (-not $SkipCodegen) {
    Invoke-Step "generate-property-maps" { & "${WorkspaceFolder}\generate-property-maps.bat" }
    Invoke-Step "generate-proto"         { & "${WorkspaceFolder}\generate-proto.bat" }
}

Invoke-Step "NuGet restore" { & "${WorkspaceFolder}\nuget.exe" restore "${WorkspaceFolder}\BG3Tools.sln" -Verbosity quiet }

Invoke-Step "ZipLib (Release x64)" {
    & $MSBuild "${WorkspaceFolder}\External\ziplib\ZipLib.sln" "/p:Configuration=Release" "/p:Platform=x64" $Toolset /t:ZipLib /m /nologo /verbosity:quiet /consoleloggerparameters:summary
}

Invoke-Step "SymbolTableGenerator (Release x64)" {
    & $MSBuild "${WorkspaceFolder}\SymbolTableGenerator\SymbolTableGenerator.vcxproj" "/p:Configuration=Release" "/p:Platform=x64" $Toolset /t:Build /m /nologo /verbosity:quiet /consoleloggerparameters:summary
}

# Incremental on purpose: no /t:Clean. Delete x64\ for a full rebuild.
Invoke-Step "BG3Tools.sln (Release x64)" {
    & $MSBuild "${WorkspaceFolder}\BG3Tools.sln" "/p:Configuration=Release" "/p:Platform=x64" $Toolset /t:Build /m /nologo /verbosity:quiet /consoleloggerparameters:summary
}

Invoke-Step "Lua.bundle" {
    & "${WorkspaceFolder}\x64\Release\ResourceBundler.exe" "${WorkspaceFolder}\BG3Extender\LuaScripts" "${WorkspaceFolder}\BG3Extender\Lua.bundle"
}

Invoke-Step "BG3Extender (Game Release x64)" {
    & $MSBuild "${WorkspaceFolder}\BG3Tools.sln" "/p:Configuration=Game Release" "/p:Platform=x64" $Toolset "/p:PreBuildEventUseInBuild=false" /t:BG3Extender /m /nologo /verbosity:quiet /consoleloggerparameters:summary
}

Invoke-Step "Symbol table" {
    & "${WorkspaceFolder}\x64\Release\SymbolTableGenerator.exe" "${WorkspaceFolder}\x64\Game Release\BG3ScriptExtender.pdb" "${WorkspaceFolder}\BG3Extender\GameHooks\BG3ScriptExtender.symtab"
}

if ($DeployUpdater) {
    Invoke-Step "Deploy updater" {
        Copy-Item -Path "${WorkspaceFolder}\x64\Release\BG3Updater.dll" -Destination (Join-Path $GameBinFolder "ScriptExtender.dll") -Force
    }
}

Invoke-Step "Deploy extender DLL + symtab" {
    Copy-Item -Path "${WorkspaceFolder}\x64\Game Release\BG3ScriptExtender.dll" -Destination (Join-Path $GameLocalFolder "BG3ScriptExtender.dll") -Force
    Copy-Item -Path "${WorkspaceFolder}\BG3Extender\GameHooks\BG3ScriptExtender.symtab" -Destination (Join-Path $GameLocalFolder "BG3ScriptExtender.symtab") -Force
    $global:LASTEXITCODE = 0
}

Write-Host ("DONE {0}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss")) -ForegroundColor Green

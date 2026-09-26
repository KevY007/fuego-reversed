#Requires -Version 5.1
<#
.SYNOPSIS
    One-shot setup: fetches and builds everything Fuego Reversed depends on, then
    (by default) builds bin\FuegoReversed.asi. Safe to re-run; incremental.

.DESCRIPTION
    Fuego Reversed links gta-reversed (the GTA:SA reimplementation, pinned as the
    external/gta-reversed submodule) as its game layer. Upstream only builds as an
    .asi, so this script:
      1. checks the toolchain: git, Visual Studio 2026 with the C++ x86 toolset
         (v145), Python 3 + Conan 2 (installed with pip if missing);
      2. syncs the submodules (external/gta-reversed, docs/reference/samp-packet-list.wiki);
      3. installs gta-reversed's Conan packages (Release, x86, static CRT) into a
         project-local Conan home (external/.conan2) -- nothing global is touched;
      4. configures + builds upstream gta-reversed UNMODIFIED (Release, /Z7);
      5. archives its object files, minus the three entry-point TUs, into
         external/generated/gta_reversed.lib;
      6. writes external/generated/gta_deps.props: the exact include dirs, defines
         and link inputs upstream compiled with, so our bridge TU (src/gta/gtahost.cpp)
         and our link match the library exactly (one imgui/spdlog/json for everyone);
      7. builds FuegoReversed.sln (Release|Win32) unless -DepsOnly.

.PARAMETER Clean
    Delete gta-reversed's build tree and the generated outputs first (full rebuild).
.PARAMETER DepsOnly
    Stop after generating the dependency outputs; don't build the ASI.
.PARAMETER ConanHome
    Conan cache location. Defaults to external/.conan2 (project-local).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1
.EXAMPLE
    scripts\bootstrap.cmd -Clean
#>
[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$DepsOnly,
    [string]$ConanHome
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$Root   = Split-Path -Parent $PSScriptRoot
$GtaDir = Join-Path $Root 'external\gta-reversed'
$GenDir = Join-Path $Root 'external\generated'
if (-not $ConanHome) { $ConanHome = Join-Path $Root 'external\.conan2' }

# Upstream TUs that define process entry points / app-loop globals. We coexist with
# the running game, so these stay out of the archive (src/gta/gtastubs.cpp supplies
# the few symbols the rest of the library references from them).
$ExcludedObjects = @('dllmain.obj', 'WinMain.obj', 'WinPlatform.obj')

function Step([string]$msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Info([string]$msg) { Write-Host "    $msg" }
function Fail([string]$msg) { Write-Host "ERROR: $msg" -ForegroundColor Red; exit 1 }

# Run a native tool; its stderr is progress output (git/conan), not an error. Fails
# the script on a non-zero exit code.
function Invoke-Native([string]$exe, [string[]]$arguments) {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $exe @arguments 2>&1 | ForEach-Object {
            if ($_ -is [System.Management.Automation.ErrorRecord]) { $_.Exception.Message } else { "$_" }
        } | Out-Host
    }
    finally { $ErrorActionPreference = $prev }
    if ($LASTEXITCODE -ne 0) { Fail "'$exe $($arguments -join ' ')' failed (exit code $LASTEXITCODE)." }
}

# ---------------------------------------------------------------------------------
Step 'Checking toolchain'

if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Fail 'git is not on PATH. Install Git for Windows.' }

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { Fail 'Visual Studio 2026 is required (vswhere.exe not found).' }
$vsPath = & $vswhere -latest -products * -version '[18.0,19.0)' `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { Fail 'Visual Studio 2026 with the "Desktop development with C++" workload is required.' }
$vsPath = @($vsPath)[0]
$toolsVer = (Get-Content (Join-Path $vsPath 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt') -Raw).Trim()
$libExe   = Join-Path $vsPath "VC\Tools\MSVC\$toolsVer\bin\Hostx64\x86\lib.exe"
$msbuild  = Join-Path $vsPath 'MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path $libExe))  { Fail "MSVC x86 librarian not found at $libExe." }
if (-not (Test-Path $msbuild)) { Fail "MSBuild not found at $msbuild." }
Info "Visual Studio: $vsPath (MSVC $toolsVer)"

# CMake >= 4.2 must be on PATH for the Conan packages built from source (upstream's own
# configure uses Conan's pinned CMake). Fall back to the copy bundled with Visual Studio.
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    $vsCMake = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake'
    if (-not (Test-Path (Join-Path $vsCMake 'CMake\bin\cmake.exe'))) {
        Fail 'CMake >= 4.2 is required: install it (cmake.org) or add the "C++ CMake tools for Windows" VS component.'
    }
    $env:PATH = (Join-Path $vsCMake 'CMake\bin') + ';' + (Join-Path $vsCMake 'Ninja') + ';' + $env:PATH
}
$cmakeVer = [version](((& cmake --version) | Select-Object -First 1) -replace '^cmake version (\d+\.\d+\.\d+).*$', '$1')
if ($cmakeVer -lt [version]'4.2.0') { Fail "CMake >= 4.2 is required (found $cmakeVer on PATH)." }
Info "CMake $cmakeVer ($((Get-Command cmake).Source))"

# Conan 2: prefer conan on PATH, else run it through a Python that has it, else pip-install it.
$script:ConanExe  = $null
$script:ConanArgs = @()
if (Get-Command conan -ErrorAction SilentlyContinue) {
    $script:ConanExe = 'conan'
} else {
    $pythons = @()
    if (Get-Command py -ErrorAction SilentlyContinue)     { $pythons += ,@('py', '-3') }
    if (Get-Command python -ErrorAction SilentlyContinue) { $pythons += ,@('python') }
    if ($pythons.Count -eq 0) { Fail 'Python 3 is required (for Conan). Install it from python.org with "Add to PATH".' }
    foreach ($p in $pythons) {
        $pyArgs = @($p | Select-Object -Skip 1)
        $prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
        & $p[0] @pyArgs -c 'import conans' 2>$null | Out-Null
        $ok = ($LASTEXITCODE -eq 0); $ErrorActionPreference = $prev
        if ($ok) { $script:ConanExe = $p[0]; $script:ConanArgs = $pyArgs + @('-m', 'conans.conan'); break }
    }
    if (-not $script:ConanExe) {
        $p = $pythons[0]; $pyArgs = @($p | Select-Object -Skip 1)
        Info 'Conan not found -- installing it with pip (user site).'
        Invoke-Native $p[0] ($pyArgs + @('-m', 'pip', 'install', '--user', '--upgrade', 'conan>=2.13,<3'))
        $script:ConanExe = $p[0]; $script:ConanArgs = $pyArgs + @('-m', 'conans.conan')
    }
}
function Invoke-Conan([string[]]$arguments) { Invoke-Native $script:ConanExe ($script:ConanArgs + $arguments) }
# The same invocation as a cmd.exe command string (for the generated .cmd steps).
$ConanCmd = (@($script:ConanExe) + $script:ConanArgs) -join ' '

# Run a generated batch file from $cwd (cmd.exe handles its quoting + redirection,
# which Windows PowerShell 5.1 mangles for native tools).
function Invoke-Batch([string]$name, [string[]]$lines, [string]$cwd) {
    $bat = Join-Path $GenDir $name
    (@('@echo off') + $lines) | Set-Content -Encoding ASCII $bat
    Push-Location $cwd
    try { Invoke-Native 'cmd.exe' @('/d', '/c', $bat) } finally { Pop-Location }
}

$env:CONAN_HOME = $ConanHome
$prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
$conanVer = (& $script:ConanExe @($script:ConanArgs + @('--version')) 2>&1 | Out-String).Trim()
$ErrorActionPreference = $prev
if ($conanVer -notmatch 'Conan version 2\.') { Fail "Conan 2.x is required (got: $conanVer)." }
Info "$conanVer (CONAN_HOME=$ConanHome)"

# ---------------------------------------------------------------------------------
Step 'Syncing submodules'
Invoke-Native git @('-C', $Root, 'submodule', 'update', '--init', '--recursive')
if (-not (Test-Path (Join-Path $GtaDir 'conanfile.py'))) { Fail "external/gta-reversed is empty -- submodule checkout failed." }
$prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
$gtaCommit = (& git -C $GtaDir rev-parse HEAD 2>$null | Out-String).Trim()
$ErrorActionPreference = $prev
Info "gta-reversed @ $gtaCommit"

if ($Clean) {
    Step 'Cleaning previous gta-reversed build + generated outputs'
    foreach ($d in @((Join-Path $GtaDir 'build'), (Join-Path $GtaDir 'bin'), $GenDir)) {
        if (Test-Path $d) { Remove-Item -Recurse -Force $d; Info "removed $d" }
    }
}

# ---------------------------------------------------------------------------------
Step 'Installing gta-reversed Conan dependencies (Release | x86 | static CRT)'
New-Item -ItemType Directory -Force -Path $GenDir | Out-Null
if (-not (Test-Path (Join-Path $ConanHome 'profiles\default'))) { Invoke-Conan @('profile', 'detect', '--force') }
$graphJson = Join-Path $GenDir 'conan_graph.json'
$conanLog  = Join-Path $GenDir 'conan_install.log'
# stdout = the JSON dependency graph (--format=json); progress goes to stderr -> log.
# tracy/*:enable=False -- Conan's Tracy defaults to TRACY_ENABLE, which would make the
# library (and so our ASI) start the Tracy profiler on load: a TCP listener on :8086 +
# LAN UDP broadcasts. Profiling is upstream dev tooling; with it off the zone macros
# compile to nothing and no Tracy code is linked. Game code is unaffected.
Invoke-Batch 'conan_install.cmd' @(
    ("$ConanCmd install . --build=missing --profile:host conan/profiles/windows-msvc.txt " +
     "-s:h build_type=Release -o:h `"tracy/*:enable=False`" " +
     "--format=json > `"$graphJson`" 2> `"$conanLog`""),
    "if errorlevel 1 (powershell -NoProfile -Command `"Get-Content -Tail 40 '$conanLog'`" & exit /b 1)"
) $GtaDir

# ---------------------------------------------------------------------------------
Step 'Building gta-reversed (upstream, unmodified; Release)'
$conanBuildBat = Join-Path $GtaDir 'build\generators\conanbuild.bat'
if (-not (Test-Path $conanBuildBat)) { Fail "Conan did not generate $conanBuildBat." }
# conanbuild.bat puts Conan's pinned CMake on PATH. /Z7 (no Edit&Continue) is what an
# optimised static archive needs; everything else is upstream's own configuration.
Invoke-Batch 'build_gta_reversed.cmd' @(
    "call `"$conanBuildBat`"",
    'cmake --preset conan-default -DGTASA_NO_EDIT_AND_CONTINUE=ON',
    'if errorlevel 1 exit /b 1',
    'cmake --build --preset conan-release --target gta_reversed --parallel',
    'if errorlevel 1 exit /b 1'
) $GtaDir

# ---------------------------------------------------------------------------------
Step 'Archiving gta-reversed objects into external\generated\gta_reversed.lib'
$vcxproj = Join-Path $GtaDir 'build\source\gta_reversed.vcxproj'
$objDir  = Join-Path $GtaDir 'build\source\gta_reversed.dir\Release'
if (-not (Test-Path $vcxproj)) { Fail "Upstream project not found at $vcxproj." }
if (-not (Test-Path $objDir))  { Fail "Upstream object dir not found at $objDir." }
$objs = @(Get-ChildItem $objDir -Recurse -Filter *.obj | Where-Object { $ExcludedObjects -notcontains $_.Name })
if ($objs.Count -lt 500) { Fail "Only $($objs.Count) objects under $objDir -- the upstream build looks incomplete." }
$rsp = Join-Path $GenDir 'gta_reversed_objs.rsp'
$objs | ForEach-Object { '"' + $_.FullName + '"' } | Set-Content -Encoding ASCII $rsp
$outLib = Join-Path $GenDir 'gta_reversed.lib'
Invoke-Native $libExe @('/NOLOGO', '/MACHINE:X86', "/OUT:$outLib", "@$rsp")
Info "$($objs.Count) objects -> $outLib ($([math]::Round((Get-Item $outLib).Length / 1MB)) MB)"

# ---------------------------------------------------------------------------------
Step 'Generating external\generated\gta_deps.props'
[xml]$proj = Get-Content $vcxproj
$ns = New-Object System.Xml.XmlNamespaceManager($proj.NameTable)
$ns.AddNamespace('m', 'http://schemas.microsoft.com/developer/msbuild/2003')
$idg = $proj.SelectSingleNode("//m:ItemDefinitionGroup[contains(@Condition, 'Release|Win32')]", $ns)
if (-not $idg) { Fail 'Release|Win32 settings not found in the upstream project.' }

function Split-MsbuildList([string]$s, [string[]]$drop) {
    if (-not $s) { return @() }
    return @($s -split ';' | ForEach-Object { $_.Trim() } |
        Where-Object { $_ -and ($_ -notmatch '^%\(') -and ($drop -notcontains $_) })
}
function Get-NodeText($parent, [string]$name) {
    $n = $parent.SelectSingleNode("m:$name", $ns)
    if ($n) { return $n.InnerText } else { return '' }
}

$cl    = $idg.SelectSingleNode('m:ClCompile', $ns)
$link  = $idg.SelectSingleNode('m:Link', $ns)
$incs  = @(Split-MsbuildList (Get-NodeText $cl 'AdditionalIncludeDirectories') @())
$incs += @(Split-MsbuildList (Get-NodeText $cl 'ExternalIncludePath') @())
# CMake passes imported (SYSTEM) include dirs as /external:I in AdditionalOptions.
$opts = Get-NodeText $cl 'AdditionalOptions'
foreach ($m in [regex]::Matches($opts, '/external:I\s*(?:"([^"]+)"|(\S+))')) {
    if ($m.Groups[1].Success) { $incs += $m.Groups[1].Value } else { $incs += $m.Groups[2].Value }
}
$incs = @($incs | Select-Object -Unique)

# Our project sets its own platform/config defines; everything else is upstream's.
$defs = Split-MsbuildList (Get-NodeText $cl 'PreprocessorDefinitions') @('WIN32', '_WINDOWS', 'NDEBUG')
$defs = @($defs | Where-Object { $_ -notmatch '^CMAKE_INTDIR=' -and $_ -notmatch '_EXPORTS$' }) # CMake module-target artifacts

# Language mode + the flags that change how headers compile (charset, preprocessor,
# conformance). Warning/debug-info/parallelism flags are ours to choose.
$langStd = Get-NodeText $cl 'LanguageStandard'
# Upstream's own header-warning suppressions (e.g. 4251 from NOTSA_EXPORT_VTABLE classes).
$noWarn = @(Split-MsbuildList (Get-NodeText $cl 'DisableSpecificWarnings') @())
$keep = @($opts -split '\s+' | Where-Object { $_ -match '^/(utf-8|bigobj|Zc:\S+|permissive-?|source-charset:\S+|execution-charset:\S+)$' } |
    Select-Object -Unique)

$libs = @(Split-MsbuildList (Get-NodeText $link 'AdditionalDependencies') @())
$libs = @($outLib) + $libs

# The imgui package (the ONE imgui the whole ASI uses -- our menu included).
$graph = Get-Content $graphJson -Raw | ConvertFrom-Json
$imguiFolder = $null
foreach ($p in $graph.graph.nodes.PSObject.Properties) {
    if ($p.Value.ref -like 'imgui/*') { $imguiFolder = $p.Value.package_folder; break }
}
if (-not $imguiFolder -or -not (Test-Path (Join-Path $imguiFolder 'res\bindings\imgui_impl_dx9.cpp'))) {
    Fail 'Could not locate the imgui Conan package (with res\bindings) from the Conan graph.'
}

function Xml([string]$s) { return [System.Security.SecurityElement]::Escape($s) }
$props = @"
<?xml version="1.0" encoding="utf-8"?>
<!--
  GENERATED by scripts\bootstrap.ps1: do not edit, do not commit (machine-specific).
  Mirrors how upstream gta-reversed compiled + links, so src\gta\gtahost.cpp and our
  link see exactly the same headers, defines and libraries as gta_reversed.lib.
-->
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup>
    <GtaDepsCommit>$gtaCommit</GtaDepsCommit>
    <GtaReversedIncludes>$(Xml ($incs -join ';'))</GtaReversedIncludes>
    <GtaReversedDefines>$(Xml ($defs -join ';'))</GtaReversedDefines>
    <GtaLanguageStandard>$(Xml $langStd)</GtaLanguageStandard>
    <GtaCompileOptions>$(Xml ($keep -join ' '))</GtaCompileOptions>
    <GtaDisableWarnings>$(Xml ($noWarn -join ';'))</GtaDisableWarnings>
    <GtaLinkDeps>$(Xml ($libs -join ';'))</GtaLinkDeps>
    <ImguiInclude>$(Xml (Join-Path $imguiFolder 'include'))</ImguiInclude>
    <ImguiBindings>$(Xml (Join-Path $imguiFolder 'res\bindings'))</ImguiBindings>
  </PropertyGroup>
</Project>
"@
Set-Content -Encoding UTF8 -Path (Join-Path $GenDir 'gta_deps.props') -Value $props
Info "$($incs.Count) include dirs, $($defs.Count) defines, $($libs.Count) link inputs"

# ---------------------------------------------------------------------------------
if ($DepsOnly) { Step 'Done (dependencies only).'; exit 0 }

Step 'Building FuegoReversed (Release | Win32)'
Invoke-Native $msbuild @((Join-Path $Root 'FuegoReversed.sln'), '/p:Configuration=Release', '/p:Platform=Win32',
    '/m', '/nologo', '/v:minimal')
Step "Done -> $(Join-Path $Root 'bin\FuegoReversed.asi')"

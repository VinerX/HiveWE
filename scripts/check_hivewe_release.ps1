[CmdletBinding()]
param(
    [string]$TargetDir,
    [int]$ProbeSeconds = 8,
    [switch]$NoLaunch
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)

$script:repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

function Resolve-ReleaseDir {
    param([string]$Requested)

    if ($Requested) {
        return (Resolve-Path $Requested).Path
    }

    $candidates = @(
        (Join-Path $script:repoRoot "build\Release\Release"),
        (Join-Path $script:repoRoot "build\Release"),
        $script:repoRoot
    )

    foreach ($candidate in $candidates) {
        if ((Test-Path $candidate) -and (Test-Path (Join-Path $candidate "HiveWE.exe"))) {
            return (Resolve-Path $candidate).Path
        }
    }

    throw "Не удалось автоматически найти папку релиза. Передайте -TargetDir <путь-к-папке-с-HiveWE.exe>."
}

function Add-Result {
    param(
        [System.Collections.Generic.List[string]]$Lines,
        [string]$Status,
        [string]$Message
    )

    $prefix = switch ($Status) {
        "OK" { "[OK]" }
        "WARN" { "[WARN]" }
        "FAIL" { "[FAIL]" }
        default { "[INFO]" }
    }
    $line = "$prefix $Message"
    $Lines.Add($line) | Out-Null
    Write-Host $line
}

function Get-LastLines {
    param(
        [string]$Path,
        [int]$Count = 40
    )

    if (-not (Test-Path $Path)) {
        return @()
    }

    return @(Get-Content $Path -Tail $Count)
}

function Try-GetWarcraftDirectory {
    $regPaths = @(
        "HKCU:\Software\Blizzard Entertainment\Warcraft III",
        "HKLM:\Software\Blizzard Entertainment\Warcraft III",
        "HKLM:\Software\WOW6432Node\Blizzard Entertainment\Warcraft III"
    )

    foreach ($regPath in $regPaths) {
        if (-not (Test-Path $regPath)) {
            continue
        }

        foreach ($name in @("InstallPathX", "InstallPath", "Path")) {
            try {
                $value = (Get-ItemProperty -Path $regPath -Name $name -ErrorAction Stop).$name
                if ($value) {
                    return [string]$value
                }
            } catch {
            }
        }
    }

    foreach ($path in @("C:\Program Files\Warcraft III", "C:\Program Files (x86)\Warcraft III")) {
        if (Test-Path $path) {
            return $path
        }
    }

    return $null
}

function Analyze-Log {
    param([string[]]$Lines)

    $analysis = [ordered]@{
        OpenGLLine = $null
        FatalLine = $null
        QtPluginHint = $null
        WarcraftHint = $null
    }

    foreach ($line in $Lines) {
        if (-not $analysis.OpenGLLine -and $line -match "OpenGL") {
            $analysis.OpenGLLine = $line
        }
        if (-not $analysis.FatalLine -and $line -match "\[FATAL\]|\[ERROR\]") {
            $analysis.FatalLine = $line
        }
        if (-not $analysis.QtPluginHint -and $line -match "platform plugin|qwindows|could not find the Qt platform plugin") {
            $analysis.QtPluginHint = $line
        }
        if (-not $analysis.WarcraftHint -and $line -match "Warcraft III|CASC|Required file not found") {
            $analysis.WarcraftHint = $line
        }
    }

    return $analysis
}

function Try-GetCimData {
    param(
        [string]$ClassName
    )

    try {
        return @(Get-CimInstance $ClassName -ErrorAction Stop)
    } catch {
        return @()
    }
}

$releaseDir = Resolve-ReleaseDir -Requested $TargetDir
$reportLines = [System.Collections.Generic.List[string]]::new()
$timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
$reportPath = Join-Path $releaseDir "hivewe_diagnose.txt"
$exePath = Join-Path $releaseDir "HiveWE.exe"
$logPath = Join-Path $releaseDir "hivewe.log"

Add-Result -Lines $reportLines -Status INFO -Message "HiveWE release diagnostic started at $timestamp"
Add-Result -Lines $reportLines -Status INFO -Message "Release dir: $releaseDir"

$requiredPaths = @(
    "HiveWE.exe",
    "Qt6Core.dll",
    "Qt6Gui.dll",
    "Qt6Widgets.dll",
    "vcruntime140.dll",
    "vcruntime140_1.dll",
    "msvcp140.dll",
    "msvcp140_atomic_wait.dll",
    "platforms\qwindows.dll",
    "styles\qmodernwindowsstyle.dll",
    "imageformats\qjpeg.dll",
    "data\themes\Dark.qss",
    "data\shaders\terrain.vert"
)

$missing = @()
foreach ($relativePath in $requiredPaths) {
    $fullPath = Join-Path $releaseDir $relativePath
    if (Test-Path $fullPath) {
        Add-Result -Lines $reportLines -Status OK -Message "Found $relativePath"
    } else {
        Add-Result -Lines $reportLines -Status FAIL -Message "Missing $relativePath"
        $missing += $relativePath
    }
}

$osInfo = Try-GetCimData -ClassName "Win32_OperatingSystem" | Select-Object -First 1
$osCaption = if ($osInfo) { [string]$osInfo.Caption } else { [System.Runtime.InteropServices.RuntimeInformation]::OSDescription }
$osVersion = if ($osInfo) { [string]$osInfo.Version } else { [Environment]::OSVersion.VersionString }
$is64Bit = [Environment]::Is64BitOperatingSystem
if ($is64Bit) {
    Add-Result -Lines $reportLines -Status OK -Message "Windows: $osCaption ($osVersion), 64-bit"
} else {
    Add-Result -Lines $reportLines -Status FAIL -Message "Windows is not 64-bit: $osCaption ($osVersion)"
}

$videoControllers = @(Try-GetCimData -ClassName "Win32_VideoController" | Sort-Object Name -Unique)
if ($videoControllers.Count -gt 0) {
    foreach ($gpu in $videoControllers) {
        $driver = if ($gpu.DriverVersion) { " driver $($gpu.DriverVersion)" } else { "" }
        Add-Result -Lines $reportLines -Status INFO -Message "GPU: $($gpu.Name)$driver"
    }
} else {
    Add-Result -Lines $reportLines -Status WARN -Message "Could not enumerate Win32_VideoController (CIM unavailable or access denied)"
}

$sessionName = [Environment]::GetEnvironmentVariable("SESSIONNAME")
if ($sessionName -and $sessionName.StartsWith("RDP", [System.StringComparison]::OrdinalIgnoreCase)) {
    Add-Result -Lines $reportLines -Status WARN -Message "Running inside Remote Desktop session ($sessionName). OpenGL is often capped or unavailable over RDP."
} else {
    $sessionLabel = if ($sessionName) { $sessionName } else { "local/console" }
    Add-Result -Lines $reportLines -Status OK -Message "Session: $sessionLabel"
}

$warcraftDir = Try-GetWarcraftDirectory
if ($warcraftDir) {
    Add-Result -Lines $reportLines -Status INFO -Message "Warcraft III candidate: $warcraftDir"
    $warcraftExe = Join-Path $warcraftDir "x86_64\Warcraft III.exe"
    if (Test-Path $warcraftExe) {
        Add-Result -Lines $reportLines -Status OK -Message "Found Warcraft III executable: $warcraftExe"
    } else {
        Add-Result -Lines $reportLines -Status WARN -Message "Warcraft III folder found, but x86_64\Warcraft III.exe is missing there"
    }
} else {
    Add-Result -Lines $reportLines -Status WARN -Message "Warcraft III install was not found in registry/default paths"
}

if ($missing.Count -gt 0) {
    Add-Result -Lines $reportLines -Status FAIL -Message "Release folder is incomplete. If HiveWE does not create hivewe.log, this is the primary suspect."
}

$preLogExists = Test-Path $logPath
$preLogStamp = if ($preLogExists) { (Get-Item $logPath).LastWriteTimeUtc } else { $null }

if (-not $NoLaunch -and (Test-Path $exePath)) {
    Add-Result -Lines $reportLines -Status INFO -Message "Launching HiveWE.exe for up to $ProbeSeconds seconds to collect startup diagnostics..."

    $process = Start-Process -FilePath $exePath -WorkingDirectory $releaseDir -PassThru
    $timedOut = $false

    try {
        Wait-Process -Id $process.Id -Timeout $ProbeSeconds -ErrorAction Stop
    } catch {
        $timedOut = $true
    }

    if ($timedOut) {
        Add-Result -Lines $reportLines -Status OK -Message "HiveWE.exe stayed alive for more than $ProbeSeconds seconds. That usually means the loader and early startup succeeded."
        try {
            Stop-Process -Id $process.Id -Force -ErrorAction Stop
            $process.WaitForExit()
            Add-Result -Lines $reportLines -Status INFO -Message "Stopped the probe process after the timeout."
        } catch {
            Add-Result -Lines $reportLines -Status WARN -Message "Could not stop probe process cleanly: $($_.Exception.Message)"
        }
    } else {
        $exitCode = $process.ExitCode
        if ($exitCode -eq 0) {
            Add-Result -Lines $reportLines -Status WARN -Message "HiveWE.exe exited quickly with code 0. This often means it closed itself early after a prerequisite check or dialog."
        } else {
            Add-Result -Lines $reportLines -Status FAIL -Message "HiveWE.exe exited quickly with code $exitCode"
        }
    }
}

$postLogExists = Test-Path $logPath
if ($postLogExists) {
    $postItem = Get-Item $logPath
    if (-not $preLogExists) {
        Add-Result -Lines $reportLines -Status OK -Message "hivewe.log was created: $logPath"
    } elseif ($preLogStamp -and $postItem.LastWriteTimeUtc -gt $preLogStamp) {
        Add-Result -Lines $reportLines -Status OK -Message "hivewe.log was updated during the probe"
    } else {
        Add-Result -Lines $reportLines -Status WARN -Message "hivewe.log exists, but its timestamp did not change during the probe"
    }

    $tailLines = Get-LastLines -Path $logPath -Count 40
    $analysis = Analyze-Log -Lines $tailLines

    if ($analysis.OpenGLLine) {
        Add-Result -Lines $reportLines -Status OK -Message "OpenGL line: $($analysis.OpenGLLine)"
    } else {
        Add-Result -Lines $reportLines -Status WARN -Message "No OpenGL line found in hivewe.log"
        if ($analysis.WarcraftHint) {
            Add-Result -Lines $reportLines -Status WARN -Message "OpenGL was probably not reached because startup stopped earlier on Warcraft III data detection."
        }
    }

    if ($analysis.FatalLine) {
        Add-Result -Lines $reportLines -Status WARN -Message "Last fatal/error hint: $($analysis.FatalLine)"
    }
    if ($analysis.QtPluginHint) {
        Add-Result -Lines $reportLines -Status WARN -Message "Qt plugin hint: $($analysis.QtPluginHint)"
    }
    if ($analysis.WarcraftHint) {
        Add-Result -Lines $reportLines -Status WARN -Message "Warcraft hint: $($analysis.WarcraftHint)"
    }

    $reportLines.Add("") | Out-Null
    $reportLines.Add("---- hivewe.log tail ----") | Out-Null
    foreach ($line in $tailLines) {
        $reportLines.Add($line) | Out-Null
    }
} else {
    Add-Result -Lines $reportLines -Status FAIL -Message "hivewe.log does not exist after the probe. That strongly suggests a failure before main() (missing DLL, broken Qt plugin dependency, unsupported OS, etc.)."
}

$reportLines.Add("") | Out-Null
$reportLines.Add("---- summary ----") | Out-Null
if ($missing.Count -gt 0) {
    $reportLines.Add("Release is missing required files: $($missing -join ', ')") | Out-Null
}
if (-not $postLogExists) {
    $reportLines.Add("Because hivewe.log was never created, focus on the loader level: bundled DLLs, plugin folders, Windows version, archive extraction, and launching from the full folder rather than a lone exe.") | Out-Null
}

$reportLines | Set-Content -Path $reportPath -Encoding UTF8
Write-Host ""
Write-Host "Report saved to $reportPath"

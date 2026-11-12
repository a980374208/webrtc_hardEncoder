<#
Run from an extracted validation package containing this script and both EXEs.
No credentials, network access, service changes, or installations are needed.
The smoke test must successfully use QSV; a software fallback is a failure.
#>
[CmdletBinding()]
param(
    [switch]$SkipUnitTests,
    [string]$OutputDirectory,
    [ValidateRange(1, 600)]
    [int]$TimeoutSeconds = 120
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-UtcTimestamp {
    [DateTime]::UtcNow.ToString('o')
}

function ConvertTo-NativeArgument {
    param([string]$Argument)
    # Start-Process joins ArgumentList on Windows; use C-runtime quoting to
    # preserve spaces, embedded quotes, and trailing backslashes in arguments.
    $escaped = [regex]::Replace($Argument, '(\\*)"', '$1$1\"')
    $escaped = [regex]::Replace($escaped, '(\\+)$', '$1$1')
    '"' + $escaped + '"'
}

function Invoke-ValidationProcess {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Executable,
        [string[]]$Arguments = @()
    )

    $stdoutPath = Join-Path $script:resultDirectory ($Name + '.stdout.txt')
    $stderrPath = Join-Path $script:resultDirectory ($Name + '.stderr.txt')
    $logPath = Join-Path $script:resultDirectory ($Name + '.log')
    $result = [ordered]@{
        name = $Name
        executable = $Executable
        sha256 = $null
        arguments = @($Arguments)
        status = 'FAIL'
        startedAtUtc = Get-UtcTimestamp
        finishedAtUtc = $null
        durationSeconds = $null
        timeoutSeconds = $TimeoutSeconds
        timedOut = $false
        terminationSucceeded = $null
        exitCode = $null
        error = $null
        stdoutFile = [IO.Path]::GetFileName($stdoutPath)
        stderrFile = [IO.Path]::GetFileName($stderrPath)
        logFile = [IO.Path]::GetFileName($logPath)
    }
    $stopwatch = [Diagnostics.Stopwatch]::StartNew()
    $childProcess = $null
    try {
        $executablePath = Join-Path $PSScriptRoot $Executable
        if (-not (Test-Path -LiteralPath $executablePath -PathType Leaf)) {
            throw "Missing executable: $Executable"
        }
        $result.sha256 = (Get-FileHash -LiteralPath $executablePath -Algorithm SHA256).Hash
        $startParameters = @{
            FilePath = $executablePath
            WorkingDirectory = $PSScriptRoot
            PassThru = $true
            WindowStyle = 'Hidden'
            RedirectStandardOutput = $stdoutPath
            RedirectStandardError = $stderrPath
        }
        if ($Arguments.Count -gt 0) {
            $startParameters.ArgumentList = ($Arguments | ForEach-Object {
                ConvertTo-NativeArgument $_
            }) -join ' '
        }
        $childProcess = Start-Process @startParameters
        # Cache the native handle so Windows PowerShell can retain ExitCode
        # even when a short-lived process exits before the later refresh.
        $null = $childProcess.Handle
        if (-not $childProcess.WaitForExit($TimeoutSeconds * 1000)) {
            $result.timedOut = $true
            $result.error = "Process exceeded $TimeoutSeconds seconds."
            try {
                # Kill only the child created above, never processes by name.
                $childProcess.Kill()
                $result.terminationSucceeded = $childProcess.WaitForExit(10000)
            } catch {
                $childProcess.Refresh()
                $result.terminationSucceeded = $childProcess.HasExited
                if (-not $result.terminationSucceeded) {
                    $result.error += ' Could not terminate the child process.'
                }
            }
        }
        $childProcess.Refresh()
        if ($childProcess.HasExited) {
            $result.exitCode = $childProcess.ExitCode
        }
        if (-not $result.timedOut -and $result.exitCode -eq 0) {
            $result.status = 'PASS'
        } elseif (-not $result.timedOut) {
            $result.error = "Process exited with code $($result.exitCode)."
        }
    } catch {
        $result.error = $_.Exception.Message
        if ($null -ne $childProcess) {
            try {
                $childProcess.Refresh()
                if (-not $childProcess.HasExited) {
                    $childProcess.Kill()
                    $result.terminationSucceeded = $childProcess.WaitForExit(10000)
                }
                if ($childProcess.HasExited) {
                    $result.exitCode = $childProcess.ExitCode
                }
            } catch {
                $result.terminationSucceeded = $false
                $result.error += ' Could not terminate the child process.'
            }
        }
    } finally {
        $stopwatch.Stop()
        $result.finishedAtUtc = Get-UtcTimestamp
        $result.durationSeconds = [Math]::Round($stopwatch.Elapsed.TotalSeconds, 3)
        foreach ($path in @($stdoutPath, $stderrPath)) {
            if (-not (Test-Path -LiteralPath $path)) {
                Set-Content -LiteralPath $path -Value '' -Encoding UTF8
            }
        }
        $stdout = Get-Content -LiteralPath $stdoutPath -Raw -Encoding UTF8
        $stderr = Get-Content -LiteralPath $stderrPath -Raw -Encoding UTF8
        $log = @(
            "name=$Name"
            "status=$($result.status)"
            "startedAtUtc=$($result.startedAtUtc)"
            "finishedAtUtc=$($result.finishedAtUtc)"
            "durationSeconds=$($result.durationSeconds)"
            "exitCode=$($result.exitCode)"
            "timedOut=$($result.timedOut)"
            "error=$($result.error)"
            ''
            '--- stdout ---'
            $stdout
            '--- stderr ---'
            $stderr
        ) -join [Environment]::NewLine
        Set-Content -LiteralPath $logPath -Value $log -Encoding UTF8
        if ($null -ne $childProcess) {
            $childProcess.Dispose()
        }
    }
    [pscustomobject]$result
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $PSScriptRoot (
        'qsv-h264-validation-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
} elseif (-not [IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $PSScriptRoot $OutputDirectory
}
$script:resultDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if ($script:resultDirectory.StartsWith('\\')) {
    throw 'OutputDirectory must be a local path.'
}
# Refuse to append to earlier evidence; every invocation gets a fresh directory.
if (Test-Path -LiteralPath $script:resultDirectory) {
    throw 'OutputDirectory already exists. Choose a new directory.'
}
New-Item -ItemType Directory -Path $script:resultDirectory | Out-Null

$report = [ordered]@{
    schemaVersion = 1
    scope = 'QSV H.264 local encode/RTP/decode validation'
    startedAtUtc = Get-UtcTimestamp
    finishedAtUtc = $null
    overallStatus = 'FAIL'
    operatingSystem = [Environment]::OSVersion.VersionString
    is64BitOperatingSystem = [Environment]::Is64BitOperatingSystem
    is64BitProcess = [Environment]::Is64BitProcess
    powershellVersion = $PSVersionTable.PSVersion.ToString()
    runnerSha256 = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
    frozenInputs = @()
    evidenceBindingErrors = @()
    gpuInformation = @()
    metadataErrors = @()
    unitTests = $null
    qsvSmoke = $null
    crashDiagnostics = [ordered]@{
        status = 'NO_DUMP'
        directory = '.'
        dumps = @()
        errors = @()
    }
    networkAndRtcpPli = 'NOT_RUN'
}
foreach ($name in @('package-manifest.json', 'build-inputs.json')) {
    $sourcePath = Join-Path $PSScriptRoot $name
    if (Test-Path -LiteralPath $sourcePath -PathType Leaf) {
        try {
            $sourceHash = (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash
            $destinationPath = Join-Path $script:resultDirectory $name
            Copy-Item -LiteralPath $sourcePath -Destination $destinationPath
            $copiedHash = (Get-FileHash -LiteralPath $destinationPath -Algorithm SHA256).Hash
            if ($copiedHash -ne $sourceHash) { throw "Frozen input copy differs: $name" }
            $report.frozenInputs += [ordered]@{ file = $name; sha256 = $copiedHash }
        } catch {
            $report.evidenceBindingErrors += $_.Exception.Message
        }
    }
}
try {
    $report.gpuInformation = @(Get-CimInstance -ClassName Win32_VideoController -OperationTimeoutSec 15 | ForEach-Object {
        [ordered]@{
            name = $_.Name
            vendor = $_.AdapterCompatibility
            videoProcessor = $_.VideoProcessor
            driverVersion = $_.DriverVersion
            driverDateUtc = if ($null -ne $_.DriverDate) {
                $_.DriverDate.ToUniversalTime().ToString('o')
            } else { $null }
        }
    })
} catch {
    # Driver metadata is diagnostic; the required hardware smoke is the gate.
    $report.metadataErrors = @($_.Exception.Message)
}

if ($SkipUnitTests) {
    $report.unitTests = [ordered]@{
        name = 'backend_video_encoder_unittests'
        executable = 'backend_video_encoder_unittests.exe'
        status = 'NOT_RUN'
        reason = 'Skipped explicitly with -SkipUnitTests.'
        sha256 = $null
    }
    $unitExecutablePath = Join-Path $PSScriptRoot 'backend_video_encoder_unittests.exe'
    if (Test-Path -LiteralPath $unitExecutablePath -PathType Leaf) {
        try {
            $report.unitTests.sha256 = (
                Get-FileHash -LiteralPath $unitExecutablePath -Algorithm SHA256).Hash
        } catch {
            $report.metadataErrors += 'Could not hash the skipped unit-test executable.'
        }
    }
} else {
    $report.unitTests = Invoke-ValidationProcess `
        -Name 'backend_video_encoder_unittests' `
        -Executable 'backend_video_encoder_unittests.exe' `
        -Arguments @('--gtest_color=no')
}
$report.qsvSmoke = Invoke-ValidationProcess `
    -Name 'qsv_h264_smoke' `
    -Executable 'qsv_h264_smoke.exe' `
    -Arguments @('--require-qsv', '--diagnostic-dir', $script:resultDirectory)

try {
    $report.crashDiagnostics.dumps = @(Get-ChildItem -LiteralPath $script:resultDirectory -File -Filter '*.dmp' | ForEach-Object {
        [ordered]@{
            file = $_.Name
            bytes = $_.Length
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
        }
    })
    if ($report.crashDiagnostics.dumps.Count -gt 0) {
        $report.crashDiagnostics.status = if (@($report.crashDiagnostics.dumps | Where-Object { $_.bytes -eq 0 }).Count -gt 0) {
            'EMPTY_DUMP'
        } else { 'CAPTURED' }
    }
} catch {
    $report.crashDiagnostics.status = 'EVIDENCE_ERROR'
    $report.crashDiagnostics.errors += $_.Exception.Message
    $report.evidenceBindingErrors += 'Could not record diagnostic dump hashes.'
}

if ($report.qsvSmoke.status -eq 'FAIL' -or $report.unitTests.status -eq 'FAIL') {
    $report.overallStatus = 'FAIL'
} elseif ($report.unitTests.status -eq 'NOT_RUN' -or $report.evidenceBindingErrors.Count -gt 0) {
    $report.overallStatus = 'INCOMPLETE'
} else {
    $report.overallStatus = 'PASS'
}
$report.finishedAtUtc = Get-UtcTimestamp
$resultPath = Join-Path $script:resultDirectory 'result.json'
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $resultPath -Encoding UTF8
Write-Host "Validation: $($report.overallStatus)"
Write-Host "Evidence: $resultPath"
if ($report.overallStatus -eq 'PASS') {
    exit 0
}
exit 1

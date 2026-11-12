<#
Run from a validation package with both smoke EXEs and the unit-test EXE.
The independent FFmpeg CLI is a reference-decoder tool, not a product dependency.
Use -FfmpegPath, bundled reference-ffmpeg/ffmpeg.exe, or FFmpeg from PATH.
Requires the real x265 H.265 backend/factory and an OpenH264 software regression.
No ReplayFixture is accepted. X265_PASS is a local software media result;
hardware is NOT_APPLICABLE, and network/RTCP PLI and stability are NOT_RUN.
#>
[CmdletBinding()]
param(
    [switch]$SkipUnitTests,
    [string]$FfmpegPath,
    [string]$OutputDirectory,
    [ValidateRange(1, 600)][int]$TimeoutSeconds = 120
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-UtcTimestamp { [DateTime]::UtcNow.ToString('o') }

# Start-Process joins ArgumentList into a command line on Windows. Preserve
# spaces and quotes using the Windows C-runtime argument quoting rules.
function ConvertTo-NativeArgument {
    param([string]$Argument)
    $escaped = [regex]::Replace($Argument, '(\\*)"', '$1$1\"')
    $escaped = [regex]::Replace($escaped, '(\\+)$', '$1$1')
    '"' + $escaped + '"'
}

function Invoke-ValidationProcess {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Executable,
        [string[]]$Arguments = @(),
        [string]$WorkingDirectory = $PSScriptRoot
    )
    $stdoutPath = Join-Path $script:resultDirectory ($Name + '.stdout.txt')
    $stderrPath = Join-Path $script:resultDirectory ($Name + '.stderr.txt')
    $result = [ordered]@{
        name = $Name; executable = $Executable; sha256 = $null
        arguments = @($Arguments); status = 'FAIL'
        startedAtUtc = Get-UtcTimestamp; finishedAtUtc = $null
        durationSeconds = $null; timeoutSeconds = $TimeoutSeconds
        timedOut = $false; terminationSucceeded = $null; exitCode = $null
        error = $null; stdoutFile = [IO.Path]::GetFileName($stdoutPath)
        stderrFile = [IO.Path]::GetFileName($stderrPath)
    }
    $stopwatch = [Diagnostics.Stopwatch]::StartNew()
    $childProcess = $null
    try {
        $executablePath = if ([IO.Path]::IsPathRooted($Executable)) {
            $Executable
        } else { Join-Path $PSScriptRoot $Executable }
        if (-not (Test-Path -LiteralPath $executablePath -PathType Leaf)) {
            throw "Missing executable: $Executable"
        }
        $result.sha256 = (Get-FileHash -LiteralPath $executablePath -Algorithm SHA256).Hash
        $startParameters = @{
            FilePath = $executablePath; WorkingDirectory = $WorkingDirectory
            PassThru = $true; WindowStyle = 'Hidden'
            RedirectStandardOutput = $stdoutPath; RedirectStandardError = $stderrPath
        }
        if ($Arguments.Count -gt 0) {
            $startParameters.ArgumentList = ($Arguments | ForEach-Object {
                ConvertTo-NativeArgument $_
            }) -join ' '
        }
        $childProcess = Start-Process @startParameters
        $null = $childProcess.Handle
        if (-not $childProcess.WaitForExit($TimeoutSeconds * 1000)) {
            $result.timedOut = $true
            $result.error = "Process exceeded $TimeoutSeconds seconds."
            try {
                # Terminate only the child created by this invocation.
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
        if ($childProcess.HasExited) { $result.exitCode = $childProcess.ExitCode }
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
        $log = @(
            "name=$Name", "status=$($result.status)", "exitCode=$($result.exitCode)",
            "timedOut=$($result.timedOut)", "error=$($result.error)",
            '--- stdout ---', (Get-Content -LiteralPath $stdoutPath -Raw -Encoding UTF8),
            '--- stderr ---', (Get-Content -LiteralPath $stderrPath -Raw -Encoding UTF8)
        ) -join [Environment]::NewLine
        Set-Content -LiteralPath (Join-Path $script:resultDirectory ($Name + '.log')) `
            -Value $log -Encoding UTF8
        if ($null -ne $childProcess) { $childProcess.Dispose() }
    }
    [pscustomobject]$result
}

function Resolve-ReferenceFfmpeg {
    if (-not [string]::IsNullOrWhiteSpace($FfmpegPath)) {
        return (Resolve-Path -LiteralPath $FfmpegPath -ErrorAction Stop).ProviderPath
    }
    foreach ($relative in @('reference-ffmpeg\ffmpeg.exe', 'ffmpeg.exe')) {
        $candidate = Join-Path $PSScriptRoot $relative
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return [IO.Path]::GetFullPath($candidate)
        }
    }
    (Get-Command ffmpeg.exe -CommandType Application -ErrorAction Stop).Source
}

function Assert-Gate {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Test-ReferenceDecode {
    param(
        [string]$Name, [string]$StreamFile, [string]$InputFile,
        [int]$ExpectedFrames, [int]$Width, [int]$Height
    )
    $result = [ordered]@{
        name = $Name; status = 'FAIL'; error = $null
        expectedFrames = $ExpectedFrames; decodedFrames = 0
        dimensions = "$($Width)x$Height"; minimumPsnrDb = $null
        decode = $null; psnr = $null
    }
    try {
        # Each invocation starts a fresh software HEVC decoder process. Decode
        # all frames (no -frames:v cap that could conceal extra output).
        $decodedName = $Name + '.decoded.i420'
        $hashName = $Name + '.decoded.framehash'
        $statsName = $Name + '.psnr.txt'
        $result.decode = Invoke-ValidationProcess -Name ($Name + '-decode') `
            -Executable $script:referenceFfmpeg -WorkingDirectory $script:artifactDirectory `
            -Arguments @('-hide_banner', '-nostdin', '-y', '-xerror',
                '-err_detect', 'explode', '-hwaccel', 'none', '-c:v', 'hevc',
                '-f', 'hevc', '-i', $StreamFile,
                '-map', '0:v:0', '-c:v', 'rawvideo', '-pix_fmt', 'yuv420p',
                '-fps_mode', 'passthrough', '-f', 'rawvideo', $decodedName,
                '-map', '0:v:0', '-c:v', 'rawvideo', '-pix_fmt', 'yuv420p',
                '-fps_mode', 'passthrough', '-f', 'framehash', '-hash', 'SHA256', $hashName)
        Assert-Gate ($result.decode.status -eq 'PASS') 'Reference HEVC decoder failed.'
        $frameBytes = [long]$Width * $Height * 3 / 2
        $decodedPath = Join-Path $script:artifactDirectory $decodedName
        $inputPath = Join-Path $script:artifactDirectory $InputFile
        Assert-Gate ((Get-Item -LiteralPath $decodedPath).Length -eq $frameBytes * $ExpectedFrames) `
            'Decoded I420 size does not contain exactly the expected frames.'
        Assert-Gate ((Get-Item -LiteralPath $inputPath).Length -eq $frameBytes * $ExpectedFrames) `
            'Reference I420 size does not contain exactly the expected frames.'
        $hashLines = @(Get-Content -LiteralPath (Join-Path $script:artifactDirectory $hashName))
        $dimensionLines = @($hashLines | Where-Object { $_ -match '^#dimensions\s+0:\s*(\d+)x(\d+)\s*$' })
        Assert-Gate ($dimensionLines.Count -eq 1) 'Missing decoder dimensions.'
        $null = $dimensionLines[0] -match '^#dimensions\s+0:\s*(\d+)x(\d+)\s*$'
        Assert-Gate ([int]$Matches[1] -eq $Width -and [int]$Matches[2] -eq $Height) `
            'Reference decoder dimensions differ from the input.'
        $frameLines = @($hashLines | Where-Object { $_ -match '^\s*0\s*,' })
        $result.decodedFrames = $frameLines.Count
        Assert-Gate ($result.decodedFrames -eq $ExpectedFrames) 'Reference decoder frame count mismatch.'

        # Raw inputs share an artificial 30 Hz clock only for frame-by-frame
        # comparison. Original RTP timestamps are separately checked/exported.
        $result.psnr = Invoke-ValidationProcess -Name ($Name + '-psnr') `
            -Executable $script:referenceFfmpeg -WorkingDirectory $script:artifactDirectory `
            -Arguments @('-hide_banner', '-nostdin', '-y', '-xerror',
                '-f', 'rawvideo', '-pixel_format', 'yuv420p', '-video_size', "$($Width)x$Height",
                '-framerate', '30', '-i', $InputFile,
                '-f', 'rawvideo', '-pixel_format', 'yuv420p', '-video_size', "$($Width)x$Height",
                '-framerate', '30', '-i', $decodedName,
                '-filter_complex', "[0:v][1:v]psnr=stats_file=$($statsName):shortest=1",
                '-f', 'null', '-')
        Assert-Gate ($result.psnr.status -eq 'PASS') 'Frame-content PSNR process failed.'
        $stats = @(Get-Content -LiteralPath (Join-Path $script:artifactDirectory $statsName))
        Assert-Gate ($stats.Count -eq $ExpectedFrames) 'PSNR did not compare every decoded frame.'
        $minimum = [double]::PositiveInfinity
        for ($i = 0; $i -lt $stats.Count; ++$i) {
            Assert-Gate ($stats[$i] -match '^n:(\d+)\s+.*\bpsnr_avg:(\S+)') 'Invalid PSNR frame record.'
            Assert-Gate ([int]$Matches[1] -eq $i + 1) 'PSNR frame order mismatch.'
            $psnr = if ($Matches[2] -eq 'inf') { [double]::PositiveInfinity } else {
                [double]::Parse($Matches[2], [Globalization.CultureInfo]::InvariantCulture)
            }
            Assert-Gate (-not [double]::IsNaN($psnr) -and $psnr -ge 28.0) `
                "Decoded frame $i PSNR is below 28 dB."
            $minimum = [Math]::Min($minimum, $psnr)
        }
        $result.minimumPsnrDb = if ([double]::IsPositiveInfinity($minimum)) { 'inf' } else { $minimum }
        $result.status = 'PASS'
    } catch {
        $result.error = $_.Exception.Message
    }
    [pscustomobject]$result
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $PSScriptRoot (
        'x265-validation-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
} elseif (-not [IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $PSScriptRoot $OutputDirectory
}
$script:resultDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if ($script:resultDirectory.StartsWith('\\')) { throw 'OutputDirectory must be a local path.' }
if (Test-Path -LiteralPath $script:resultDirectory) {
    throw 'OutputDirectory already exists. Choose a fresh evidence directory.'
}
New-Item -ItemType Directory -Path $script:resultDirectory | Out-Null
$script:artifactDirectory = Join-Path $script:resultDirectory 'h265-artifacts'
New-Item -ItemType Directory -Path $script:artifactDirectory | Out-Null

$report = [ordered]@{
    schemaVersion = 1; scope = 'x265 H.265 software encode/RTP/reference-decode and OpenH264 regression'
    startedAtUtc = Get-UtcTimestamp; finishedAtUtc = $null; overallStatus = 'FAIL'
    operatingSystem = [Environment]::OSVersion.VersionString
    is64BitOperatingSystem = [Environment]::Is64BitOperatingSystem
    is64BitProcess = [Environment]::Is64BitProcess
    powershellVersion = $PSVersionTable.PSVersion.ToString()
    gpuInformation = @(); metadataErrors = @(); unitTests = $null
    h264Regression = $null; h265Smoke = $null
    referenceDecoder = [ordered]@{
        status = 'NOT_RUN'; error = $null; toolPath = $null; version = $null
        stream = $null; independentKeys = @(); builtinFactory = $null
        productH265Decoder = 'NOT_IMPLEMENTED'
    }
    hardwareStatus = 'NOT_APPLICABLE'; softwareEncoderStatus = 'NOT_RUN'; localBuildGate = $false
    runnerSha256 = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
    packageVersion = $null; frozenInputs = @(); evidenceBindingErrors = @()
    networkAndRtcpPli = 'NOT_RUN'; artifactHashes = @()
}
try {
    foreach ($name in @('package-manifest.json', 'build-inputs.json')) {
        $sourcePath = Join-Path $PSScriptRoot $name
        if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
            throw "Missing bound package input: $name"
        }
        $sourceHash = (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash
        $destinationPath = Join-Path $script:resultDirectory $name
        Copy-Item -LiteralPath $sourcePath -Destination $destinationPath
        $copiedHash = (Get-FileHash -LiteralPath $destinationPath -Algorithm SHA256).Hash
        Assert-Gate ($sourceHash -eq $copiedHash) "Frozen input copy differs: $name"
        $report.frozenInputs += [ordered]@{ file = $name; sha256 = $copiedHash }
    }
    $packageManifestPath = Join-Path $PSScriptRoot 'package-manifest.json'
    if (Test-Path -LiteralPath $packageManifestPath -PathType Leaf) {
        $packageManifest = Get-Content -LiteralPath $packageManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
        $supportedVersions = @('20261006-v1')
        $expectedScope = 'x265 H265 Main software encode/RTP/independent FFmpeg HEVC decode and OpenH264 regression'
        Assert-Gate ($packageManifest.packageVersion -in $supportedVersions -and
            $packageManifest.scope -eq $expectedScope) 'Unexpected x265 package manifest.'
        $report.packageVersion = $packageManifest.packageVersion
        Assert-Gate ($packageManifest.packageStage -in @('LOCAL_BUILD_GATE', 'FROZEN_PACKAGE')) `
            'Unexpected package stage.'
        $report.localBuildGate = $packageManifest.packageStage -eq 'LOCAL_BUILD_GATE'
        $packageRoot = [IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\') + '\'
        $listedFiles = @()
        foreach ($item in $packageManifest.files) {
            Assert-Gate (-not [IO.Path]::IsPathRooted($item.file)) 'Rooted manifest file path.'
            $filePath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot $item.file))
            Assert-Gate ($filePath.StartsWith($packageRoot, [StringComparison]::OrdinalIgnoreCase)) `
                'Manifest file path leaves the package.'
            Assert-Gate (Test-Path -LiteralPath $filePath -PathType Leaf) "Missing package file: $($item.file)"
            Assert-Gate ((Get-Item -LiteralPath $filePath).Length -eq $item.bytes -and
                (Get-FileHash -LiteralPath $filePath -Algorithm SHA256).Hash -eq $item.sha256) `
                "Package bytes/hash differ: $($item.file)"
            $listedFiles += $item.file
        }
        Assert-Gate (@($listedFiles | Select-Object -Unique).Count -eq $listedFiles.Count) `
            'Duplicate package file record.'
        foreach ($name in @('backend_video_encoder_unittests.exe', 'nvenc_h264_smoke.exe',
            'x265_h265_smoke.exe', 'run_x265_validation.ps1', 'reference-ffmpeg/ffmpeg.exe', 'build-inputs.json')) {
            Assert-Gate ($name -in $listedFiles) "Required package file is not hash-bound: $name"
        }
        Assert-Gate ((Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'build-inputs.json') -Algorithm SHA256).Hash -eq
            $packageManifest.buildInputSnapshotSha256) 'Package does not bind the copied input snapshot.'
        $buildInputs = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'build-inputs.json') `
            -Raw -Encoding UTF8 | ConvertFrom-Json
        Assert-Gate ($buildInputs.packageVersion -eq $packageManifest.packageVersion) `
            'Package/input snapshot version mismatch.'
    }
} catch { $report.evidenceBindingErrors += $_.Exception.Message }
try {
    $report.gpuInformation = @(Get-CimInstance -ClassName Win32_VideoController -OperationTimeoutSec 15 | ForEach-Object {
        [ordered]@{
            name = $_.Name; vendor = $_.AdapterCompatibility
            videoProcessor = $_.VideoProcessor; driverVersion = $_.DriverVersion
            driverDateUtc = if ($null -ne $_.DriverDate) { $_.DriverDate.ToUniversalTime().ToString('o') } else { $null }
        }
    })
} catch { $report.metadataErrors += $_.Exception.Message }

if ($report.evidenceBindingErrors.Count -eq 0) {
if ($SkipUnitTests) {
    $report.unitTests = [ordered]@{ status = 'NOT_RUN'; reason = 'Explicit -SkipUnitTests.'; sha256 = $null }
    $unitPath = Join-Path $PSScriptRoot 'backend_video_encoder_unittests.exe'
    if (Test-Path -LiteralPath $unitPath -PathType Leaf) {
        $report.unitTests.sha256 = (Get-FileHash -LiteralPath $unitPath -Algorithm SHA256).Hash
    }
} else {
    # Require the complete suite even if the caller configured GoogleTest
    # filtering, repetition or sharding in its shell environment.
    Remove-Item Env:GTEST_TOTAL_SHARDS, Env:GTEST_SHARD_INDEX -ErrorAction SilentlyContinue
    $report.unitTests = Invoke-ValidationProcess -Name 'backend_video_encoder_unittests' `
        -Executable 'backend_video_encoder_unittests.exe' `
        -Arguments @('--gtest_color=no', '--gtest_filter=*', '--gtest_repeat=1', '--gtest_shuffle=0')
}
$report.h264Regression = Invoke-ValidationProcess -Name 'openh264_regression' `
    -Executable 'nvenc_h264_smoke.exe' -Arguments @('--software')
$report.h265Smoke = Invoke-ValidationProcess -Name 'x265_h265_smoke' `
    -Executable 'x265_h265_smoke.exe' -Arguments @('--require-x265', '--output-dir', $script:artifactDirectory)

if ($report.h265Smoke.status -eq 'PASS') {
    try {
        $script:referenceFfmpeg = Resolve-ReferenceFfmpeg
        $report.referenceDecoder.toolPath = $script:referenceFfmpeg
        $referenceRecord = @($packageManifest.files | Where-Object { $_.file -eq 'reference-ffmpeg/ffmpeg.exe' })
        Assert-Gate ($referenceRecord.Count -eq 1 -and
            (Get-Item -LiteralPath $script:referenceFfmpeg).Length -eq $referenceRecord[0].bytes -and
            (Get-FileHash -LiteralPath $script:referenceFfmpeg -Algorithm SHA256).Hash -eq $referenceRecord[0].sha256) `
            'Selected reference FFmpeg differs from the hash-bound decoder.'
        $report.referenceDecoder.version = Invoke-ValidationProcess -Name 'reference-ffmpeg-version' `
            -Executable $script:referenceFfmpeg -Arguments @('-version')
        Assert-Gate ($report.referenceDecoder.version.status -eq 'PASS') 'Reference FFmpeg is unavailable.'
        $manifest = Get-Content -LiteralPath (Join-Path $script:artifactDirectory 'manifest.json') `
            -Raw -Encoding UTF8 | ConvertFrom-Json
        $expectedHardware = $false
        $expectedImplementation = 'x265'
        Assert-Gate ($manifest.codec -eq 'H265' -and $manifest.hardware -eq $expectedHardware -and
            $manifest.implementation -eq $expectedImplementation -and $manifest.frameCount -eq 60 -and
            $manifest.sessionCount -eq 2 -and $manifest.width -eq 640 -and $manifest.height -eq 360) `
            'Unexpected real x265 software export manifest.'
        $frames = @($manifest.frames)
        Assert-Gate ($frames.Count -eq 60 -and $manifest.fuPackets -gt 0) 'Missing frame records or RTP FU coverage.'
        for ($i = 0; $i -lt 60; ++$i) {
            $frame = $frames[$i]
            Assert-Gate ($frame.index -eq $i -and $frame.session -eq [Math]::Floor($i / 30) -and
                $frame.sessionIndex -eq $i % 30 -and $frame.frameId -eq $frame.rtpTimestamp) `
                'Encoded/RTP frame ID or sequence differs from the input.'
            if ($i -gt 0) {
                $sessionIndex = $i % 30
                $expectedDelta = if ($sessionIndex -eq 20) { 24000 } elseif ($sessionIndex -ge 15) { 6000 } else { 3000 }
                Assert-Gate (($frame.rtpTimestamp - $frames[$i - 1].rtpTimestamp) -eq $expectedDelta) `
                    'RTP timestamps do not match encode/rate-change/pause order.'
            }
        }
        $keys = @($frames | Where-Object { $_.requiredKey })
        Assert-Gate ($keys.Count -eq 6 -and $manifest.requiredKeyCount -eq 6) 'Missing required IDR records.'
        foreach ($key in $keys) {
            Assert-Gate ($key.keyFrame -eq $true -and ($key.sessionIndex -in @(0, 10, 20))) `
                'Required session/request/resume frame is not IDR.'
        }
        $report.referenceDecoder.stream = Test-ReferenceDecode -Name 'h265-stream' `
            -StreamFile $manifest.streamFile -InputFile $manifest.inputFile -ExpectedFrames 60 -Width 640 -Height 360
        foreach ($key in $keys) {
            $report.referenceDecoder.independentKeys += Test-ReferenceDecode -Name ('h265-key-' + $key.index) `
                -StreamFile $key.keyStreamFile -InputFile $key.keyInputFile -ExpectedFrames 1 -Width 640 -Height 360
        }
        $decodeGates = @($report.referenceDecoder.stream) + @($report.referenceDecoder.independentKeys)
        Assert-Gate ($manifest.validationMode -eq 'SOFTWARE_ENCODER_REQUIRED' -and
            $manifest.builtinFactory.hardware -eq $false -and
            $manifest.builtinFactory.implementation -eq 'x265' -and
            $manifest.builtinFactory.streamFile -eq 'builtin-factory.hevc' -and
            $manifest.builtinFactory.inputFile -eq 'builtin-factory.i420') `
            'Builtin factory did not select the real x265 software encoder.'
        $report.referenceDecoder.builtinFactory = Test-ReferenceDecode -Name 'h265-builtin-factory' `
            -StreamFile $manifest.builtinFactory.streamFile -InputFile $manifest.builtinFactory.inputFile `
            -ExpectedFrames 1 -Width 640 -Height 360
        $decodeGates += @($report.referenceDecoder.builtinFactory)
        Assert-Gate (@($decodeGates | Where-Object { $_.status -ne 'PASS' }).Count -eq 0) `
            'A required reference-decode/content gate failed.'
        $report.referenceDecoder.status = 'PASS'
    } catch {
        $report.referenceDecoder.status = 'FAIL'
        $report.referenceDecoder.error = $_.Exception.Message
    }
} else {
    $report.referenceDecoder.error = 'Real x265 H.265 smoke did not successfully export the required RTP evidence.'
}
} else {
    $report.unitTests = [ordered]@{ status = 'NOT_RUN'; reason = 'Frozen input binding failed.' }
    $report.h264Regression = [ordered]@{ status = 'NOT_RUN'; reason = 'Frozen input binding failed.' }
    $report.h265Smoke = [ordered]@{ status = 'NOT_RUN'; reason = 'Frozen input binding failed.' }
}

$statuses = @($report.unitTests.status, $report.h264Regression.status,
    $report.h265Smoke.status, $report.referenceDecoder.status)
if ($statuses -contains 'FAIL' -or $report.evidenceBindingErrors.Count -gt 0) { $report.overallStatus = 'FAIL' }
elseif (@($statuses | Where-Object { $_ -ne 'PASS' }).Count -gt 0) { $report.overallStatus = 'INCOMPLETE' }
else { $report.overallStatus = 'X265_PASS'; $report.softwareEncoderStatus = 'PASS' }
try {
    $report.artifactHashes = @(Get-ChildItem -LiteralPath $script:artifactDirectory -File | ForEach-Object {
        [ordered]@{ file = $_.Name; bytes = $_.Length; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
    })
} catch {
    $report.metadataErrors += $_.Exception.Message
    $report.overallStatus = 'FAIL'
}
if ($report.overallStatus -ne 'X265_PASS') {
    $report.softwareEncoderStatus = if ($report.h265Smoke.status -eq 'NOT_RUN') { 'NOT_RUN' } else { 'FAIL' }
}
$report.finishedAtUtc = Get-UtcTimestamp
$resultPath = Join-Path $script:resultDirectory 'result.json'
$report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $resultPath -Encoding UTF8
Write-Host "Validation: $($report.overallStatus)"
Write-Host "Evidence: $resultPath"
if ($report.overallStatus -eq 'X265_PASS') { exit 0 }
exit 1

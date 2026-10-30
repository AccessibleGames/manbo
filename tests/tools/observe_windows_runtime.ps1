param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [ValidateRange(1, 120)][int]$IdleSeconds = 60,
    [switch]$SingleCore,
    [ValidatePattern('^[A-Za-z0-9_.*:-]+$')][string]$Filter = 'Idle.Quiescent',
    [ValidateRange(1, 100)][int]$Repeat = 1,
    [string]$OutputDirectory = "$PSScriptRoot\..\..\build\windows\observations"
)

$ErrorActionPreference = 'Stop'
$executablePath = (Resolve-Path -LiteralPath $Executable).Path
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$outputPath = (Resolve-Path -LiteralPath $OutputDirectory).Path
$start = New-Object System.Diagnostics.ProcessStartInfo
$start.FileName = $executablePath
$start.Arguments = "--gtest_filter=$Filter --gtest_repeat=$Repeat"
$start.UseShellExecute = $false
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$start.CreateNoWindow = $true
$start.EnvironmentVariables['MANBO_IDLE_SECONDS'] = [string]$IdleSeconds
$process = New-Object System.Diagnostics.Process
$process.StartInfo = $start
$launcher = [System.Diagnostics.Process]::GetCurrentProcess()
$originalAffinity = $launcher.ProcessorAffinity
$wall = [System.Diagnostics.Stopwatch]::StartNew()
try {
    if ($SingleCore) {
        # Pin before creation: the child inherits one CPU before any test starts.
        $available = $originalAffinity.ToInt64()
        $oneCpu = $available -band (-$available)
        $launcher.ProcessorAffinity = [IntPtr]$oneCpu
    }
    if (-not $process.Start()) { throw 'Could not start runtime observation.' }
    $effectiveAffinity = $process.ProcessorAffinity.ToInt64()
    if ($SingleCore -and $effectiveAffinity -ne $oneCpu) {
        $process.Kill()
        throw 'The child did not inherit the requested single-CPU affinity.'
    }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
}
finally {
    if ($SingleCore) { $launcher.ProcessorAffinity = $originalAffinity }
    $launcher.Dispose()
}
$timeoutMs = [Math]::Max($IdleSeconds + 30, $Repeat * 5) * 1000
if (-not $process.WaitForExit($timeoutMs)) {
    $process.Kill()
    throw "Runtime observation exceeded ${timeoutMs}ms."
}
$wall.Stop()
$cpuMs = $process.TotalProcessorTime.TotalMilliseconds
$stdout = $stdoutTask.GetAwaiter().GetResult()
$stderr = $stderrTask.GetAwaiter().GetResult()
$exitCode = $process.ExitCode
$process.Dispose()
$stdout | Set-Content -LiteralPath "$outputPath\stdout.txt" -Encoding UTF8
$stderr | Set-Content -LiteralPath "$outputPath\stderr.txt" -Encoding UTF8
$report = [ordered]@{
    executable = $executablePath
    filter = $Filter
    repeats = $Repeat
    requested_idle_seconds = $IdleSeconds
    single_core = [bool]$SingleCore
    affinity_mask_hex = $effectiveAffinity.ToString('X')
    single_core_from_creation = [bool]$SingleCore
    exit_code = $exitCode
    wall_ms = $wall.Elapsed.TotalMilliseconds
    total_process_cpu_ms = $cpuMs
    average_one_core_cpu_percent = 100 * $cpuMs / $wall.Elapsed.TotalMilliseconds
    idle_evidence = @($stdout -split '\r?\n' | Where-Object { $_ -like 'IDLE_EVIDENCE *' })
    limitation = 'CPU includes test startup/shutdown. Counters measure application activity, not ETW scheduler wakeups or a real terminal session.'
}
$report | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath "$outputPath\report.json" -Encoding UTF8
$report | ConvertTo-Json -Depth 4 | Write-Output
if ($exitCode -ne 0) { Write-Output $stdout; Write-Error $stderr; exit $exitCode }

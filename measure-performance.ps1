param([ValidateRange(5,3600)][int]$Seconds=60)
$ErrorActionPreference='Stop'
$matches=@(Get-Process -Name NightLight -ErrorAction Stop)
if($matches.Count -ne 1){throw 'Run exactly one NightLight instance.'}
$p=$matches[0]
$idToSample=$p.Id
$startCpu=$p.TotalProcessorTime.TotalSeconds
$clock=[Diagnostics.Stopwatch]::StartNew()
$rows=for($i=0;$i -le $Seconds;$i++){
    if($i -gt 0){Start-Sleep -Seconds 1}
    $p=Get-Process -Id $idToSample -ErrorAction Stop
    [pscustomobject]@{
        WorkingSetMiB=$p.WorkingSet64/1MB
        PrivateMiB=$p.PrivateMemorySize64/1MB
        Handles=$p.HandleCount
        Threads=$p.Threads.Count
        CpuSeconds=$p.TotalProcessorTime.TotalSeconds
    }
}
$clock.Stop()
$last=$rows[-1]
$cpu=$last.CpuSeconds-$startCpu
[pscustomobject]@{
    DurationSeconds=[math]::Round($clock.Elapsed.TotalSeconds,2)
    CpuSeconds=[math]::Round($cpu,4)
    CpuPercentOfOneCore=[math]::Round(100*$cpu/$clock.Elapsed.TotalSeconds,4)
    CpuPercentOfMachine=[math]::Round(100*$cpu/($clock.Elapsed.TotalSeconds*[Environment]::ProcessorCount),4)
    AverageWorkingSetMiB=[math]::Round(($rows|Measure-Object WorkingSetMiB -Average).Average,2)
    PeakWorkingSetMiB=[math]::Round(($rows|Measure-Object WorkingSetMiB -Maximum).Maximum,2)
    AveragePrivateMiB=[math]::Round(($rows|Measure-Object PrivateMiB -Average).Average,2)
    PeakPrivateMiB=[math]::Round(($rows|Measure-Object PrivateMiB -Maximum).Maximum,2)
    FirstHandles=$rows[0].Handles
    LastHandles=$last.Handles
    FirstThreads=$rows[0].Threads
    LastThreads=$last.Threads
}|Format-List

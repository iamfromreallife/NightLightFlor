param([ValidateRange(5,3600)][int]$Seconds=60)
$ErrorActionPreference='Stop'
# Sample GDI/USER resources separately from kernel handles. Add-Type runs
# in the measuring PowerShell process, never inside NightLight.
if(-not ('NightLightPerf.GuiResources' -as [type])) {
    Add-Type -Namespace NightLightPerf -Name GuiResources -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern uint GetGuiResources(System.IntPtr hProcess, uint uiFlags);
'@
}
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
        GdiHandles=[NightLightPerf.GuiResources]::GetGuiResources($p.Handle,0)
        UserHandles=[NightLightPerf.GuiResources]::GetGuiResources($p.Handle,1)
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
    PeakHandles=($rows|Measure-Object Handles -Maximum).Maximum
    FirstGdiHandles=$rows[0].GdiHandles
    LastGdiHandles=$last.GdiHandles
    PeakGdiHandles=($rows|Measure-Object GdiHandles -Maximum).Maximum
    FirstUserHandles=$rows[0].UserHandles
    LastUserHandles=$last.UserHandles
    PeakUserHandles=($rows|Measure-Object UserHandles -Maximum).Maximum
    FirstThreads=$rows[0].Threads
    LastThreads=$last.Threads
}|Format-List

# Prints the game's binaries\x64 folder: next to this script, then every Steam library.
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$cands = @($here, (Join-Path $here '..'), (Join-Path $here 'binaries\x64'))
try {
    $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction Stop).SteamPath
    $libs = @($steam)
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path $vdf) {
        foreach ($m in (Select-String -Path $vdf -Pattern '"path"\s+"(.+?)"')) { $libs += $m.Matches[0].Groups[1].Value -replace '\\\\', '\' }
    }
    foreach ($l in $libs) { $cands += Join-Path $l 'steamapps\common\Call to Arms - Gates of Hell\binaries\x64' }
} catch {}
$cands += 'C:\Program Files (x86)\Steam\steamapps\common\Call to Arms - Gates of Hell\binaries\x64'
foreach ($c in $cands) {
    if (Test-Path (Join-Path $c 'call_to_arms.exe')) { (Resolve-Path $c).Path; exit 0 }
}
exit 1

<#
.SYNOPSIS
  Holt ein statisches ffmpeg-Build und legt nur ffmpeg.exe neben CapView ab.

.DESCRIPTION
  Genau die Schritte, die der Knopf in den CapView-Einstellungen ausfuehrt:
  Archiv herunterladen, pruefen, eine einzige Datei herausziehen, Archiv
  loeschen, Ergebnis verifizieren.

  Bewusst ein *statisches* Build: nur dann laeuft die einzelne ffmpeg.exe ohne
  begleitende DLLs. Bei einem Shared-Build muesste der ganze bin-Ordner mit.

  Heruntergeladen wird vom Upstream, nicht von uns weiterverteilt. Das ist die
  lizenzrechtlich entspannte Variante: die ueblichen Windows-Builds enthalten
  x264 und x265 und sind damit GPL, und wer sie selbst weitergibt, schuldet den
  Quellcode dazu.

.PARAMETER Target
  Zielordner. Standard: der Programm-Ordner neben diesem Skript, Unterordner
  "ffmpeg".

.PARAMETER Force
  Ueberschreibt eine vorhandene ffmpeg.exe.
#>

[CmdletBinding()]
param(
    [string]$Target,
    [switch]$Force,
    # Nur nachsehen, ob es eine neuere Version gibt, und nichts herunterladen.
    [switch]$CheckOnly
)

$ErrorActionPreference = 'Stop'

# gyan.dev "release-essentials": statisch, von ffmpeg.org verlinkt, stabile URL.
#
# Gegen die Alternative BtbN win64-gpl gemessen: 106 statt 163 MB Archiv, 98
# statt 139 MB entpackt, und beide enthalten dieselben Encoder. In essentials
# sind h264/hevc/av1 fuer NVENC, QuickSync und AMF einkompiliert, dazu x264 und
# x265. "full" bringt nur weitere externe Bibliotheken, die CapView nicht nutzt.
$Url = 'https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip'

if (-not $Target) {
    $root = Split-Path -Parent $PSScriptRoot
    $Target = Join-Path $root 'Programm\ffmpeg'
}

$exePath = Join-Path $Target 'ffmpeg.exe'

# Die stabile URL ist ein 303 auf das versionierte Paket. Der Dateiname darin
# ist die einzige Stelle, an der die Remote-Version steht -- gyan hat keinen
# eigenen Endpunkt dafuer.
function Get-RemoteVersion {
    try {
        $null = Invoke-WebRequest -Uri $Url -Method Head -UseBasicParsing -MaximumRedirection 0 -ErrorAction Stop
    } catch {
        $loc = $_.Exception.Response.Headers.Location
        if ($loc -and $loc.ToString() -match 'ffmpeg-([0-9][0-9.]*)-') { return $Matches[1] }
    }
    return $null
}

function Get-LocalVersion {
    if (-not (Test-Path $exePath)) { return $null }
    $out = & $exePath -hide_banner -version 2>&1
    if ($LASTEXITCODE -ne 0) { return $null }
    if (@($out)[0] -match 'ffmpeg version ([0-9][0-9.]*)') { return $Matches[1] }
    return $null
}

if ($CheckOnly) {
    $local = Get-LocalVersion
    $remote = Get-RemoteVersion
    Write-Host ("Lokal : {0}" -f $(if ($local) { $local } else { "nicht installiert" }))
    Write-Host ("Remote: {0}" -f $(if ($remote) { $remote } else { "nicht ermittelbar" }))
    if ($local -and $remote) {
        if ([version]$remote -gt [version]$local) { Write-Host "-> Update verfuegbar." }
        else { Write-Host "-> Aktuell." }
    }
    exit 0
}

if ((Test-Path $exePath) -and -not $Force) {
    Write-Host "Schon vorhanden: $exePath"
    Write-Host "Mit -Force ueberschreiben, mit -CheckOnly auf Updates pruefen."
    exit 0
}

Write-Host "=== 1/5  Quelle ==="
Write-Host "  $Url"
Write-Host "  Ziel: $exePath"

New-Item -ItemType Directory -Force -Path $Target | Out-Null
$archive = Join-Path ([System.IO.Path]::GetTempPath()) 'capview_ffmpeg.zip'

Write-Host "`n=== 2/5  Herunterladen ==="
# Fortschrittsanzeige aus: Invoke-WebRequest wird damit um ein Vielfaches
# langsamer, weil es pro Block das Fenster neu zeichnet.
$oldProgress = $ProgressPreference
$ProgressPreference = 'SilentlyContinue'
$sw = [System.Diagnostics.Stopwatch]::StartNew()
try {
    Invoke-WebRequest -Uri $Url -OutFile $archive -UseBasicParsing
} finally {
    $ProgressPreference = $oldProgress
}
$sw.Stop()
$sizeMb = (Get-Item $archive).Length / 1MB
Write-Host ("  {0:N1} MB in {1:N1} s" -f $sizeMb, $sw.Elapsed.TotalSeconds)

Write-Host "`n=== 3/5  Archiv pruefen ==="
# Echte Pruefsumme, nicht nur die ZIP-Kennung: gyan veroeffentlicht eine SHA-256
# neben dem Archiv. Das faengt einen abgebrochenen oder verfaelschten Download
# ab. Es ersetzt keine Signatur -- die Pruefsumme kommt vom selben Server wie
# die Datei, schuetzt also gegen Uebertragungsfehler, nicht gegen einen
# kompromittierten Server. Eine Authenticode-Signatur haben diese Builds nicht.
$expected = $null
try {
    $expected = (Invoke-WebRequest -Uri ($Url + '.sha256') -UseBasicParsing).Content.Trim().Split()[0]
} catch {
    Write-Host "  Warnung: Pruefsumme nicht abrufbar, pruefe nur die ZIP-Kennung."
}

if ($expected) {
    $actual = (Get-FileHash -Path $archive -Algorithm SHA256).Hash.ToLower()
    if ($actual -ne $expected.ToLower()) {
        Remove-Item $archive -Force
        throw "SHA-256 stimmt nicht ueberein.`n  erwartet: $expected`n  bekommen: $actual"
    }
    Write-Host "  SHA-256 stimmt ($($expected.Substring(0,16))...)"
} else {
    $head = [System.IO.File]::ReadAllBytes($archive)[0..1]
    if ($head[0] -ne 0x50 -or $head[1] -ne 0x4B) {
        Remove-Item $archive -Force
        throw "Die geladene Datei ist kein ZIP-Archiv."
    }
    Write-Host "  ZIP-Kennung ok"
}

Write-Host "`n=== 4/5  Nur ffmpeg.exe herausziehen ==="
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($archive)
try {
    # Der Eintrag liegt in einem Unterordner, dessen Name die Version enthaelt.
    $entry = $zip.Entries | Where-Object { $_.FullName -like '*bin/ffmpeg.exe' } | Select-Object -First 1
    if (-not $entry) { throw "ffmpeg.exe im Archiv nicht gefunden." }
    Write-Host ("  {0}  ({1:N1} MB entpackt)" -f $entry.FullName, ($entry.Length / 1MB))
    [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $exePath, $true)
} finally {
    $zip.Dispose()
}

Write-Host "`n=== 5/5  Aufraeumen und pruefen ==="
Remove-Item $archive -Force
Write-Host "  Archiv geloescht"

# Erst vollstaendig einsammeln, dann die erste Zeile nehmen: ein
# "Select-Object -First 1" direkt in der Pipeline bricht das native Kommando ab
# und macht $LASTEXITCODE unbrauchbar.
$output = & $exePath -hide_banner -version 2>&1
if ($LASTEXITCODE -ne 0) { throw "ffmpeg.exe laesst sich nicht ausfuehren." }
Write-Host "  $(@($output)[0])"
Write-Host ("  {0:N1} MB auf der Platte" -f ((Get-Item $exePath).Length / 1MB))
Write-Host "`nFertig. CapView findet das jetzt von allein."

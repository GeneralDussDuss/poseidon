param(
  [string]$Port = 'COM16',
  [int]$Baud = 115200,
  [int]$Seconds = 8
)
$p = New-Object System.IO.Ports.SerialPort $Port,$Baud,'None',8,'One'
$p.DtrEnable = $false
$p.RtsEnable = $false
$p.ReadTimeout = 500
try { $p.Open() } catch { Write-Host "open failed: $_"; exit 1 }
$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline) {
  try { Write-Host -NoNewline $p.ReadExisting() } catch {}
  Start-Sleep -Milliseconds 100
}
$p.Close()

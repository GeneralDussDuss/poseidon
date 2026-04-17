$port = New-Object System.IO.Ports.SerialPort 'COM17', 115200, 'None', 8, 'One'
$port.ReadTimeout = 200
$port.DtrEnable = $false
$port.RtsEnable = $false
$port.Open()
$log = "C:\Users\D\poseidon\_c5_capture.log"
"" | Out-File $log
try {
    $end = (Get-Date).AddSeconds(45)
    while ((Get-Date) -lt $end) {
        try {
            $line = $port.ReadLine()
            $line | Out-File -Append $log
            Write-Output $line
        } catch { Start-Sleep -Milliseconds 30 }
    }
} finally { $port.Close() }

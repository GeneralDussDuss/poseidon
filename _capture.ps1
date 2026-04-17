$port = New-Object System.IO.Ports.SerialPort 'COM16', 115200, 'None', 8, 'One'
$port.ReadTimeout = 400
$port.Open()
$log = "C:\Users\D\poseidon\_capture.log"
"" | Out-File $log
try {
    $end = (Get-Date).AddSeconds(60)
    while ((Get-Date) -lt $end) {
        try {
            $line = $port.ReadLine()
            $line | Out-File -Append $log
            Write-Output $line
        } catch { Start-Sleep -Milliseconds 30 }
    }
} finally { $port.Close() }

Get-CimInstance -ClassName Win32_PnPEntity |
  Where-Object { $_.Caption -match 'COM\d+' } |
  ForEach-Object { $_.Caption }

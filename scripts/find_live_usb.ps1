Get-PnpDevice -Status OK |
  Where-Object { $_.FriendlyName -match 'JTAG|ESP32|USB Serial|Cardputer|CP210|CH340|Silicon Labs' } |
  Format-Table FriendlyName, Class, Status -Auto

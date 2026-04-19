$devs = Get-PnpDevice -Status OK | Where-Object { $_.FriendlyName -match 'ESP|JTAG|Serial|COM|CDC|USB' }
$devs | Format-Table FriendlyName, Status -Auto

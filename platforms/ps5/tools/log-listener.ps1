param([int]$Port = 18200, [string]$Out = "$PSScriptRoot\logs\ps5-live.log")
# Receives a development build's log stream (build with MELEE_PS5_LOG_HOST set
# to this PC's address and allow TCP $Port through the firewall). Each
# connection starts a new section.
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Any, $Port)
$listener.Start()
while ($true) {
  $client = $listener.AcceptTcpClient()
  Add-Content $Out ("===== connection " + (Get-Date -Format s) + " from " + $client.Client.RemoteEndPoint)
  $reader = [IO.StreamReader]::new($client.GetStream())
  try { while (($line = $reader.ReadLine()) -ne $null) { Add-Content $Out $line } } catch {}
  $client.Close()
}

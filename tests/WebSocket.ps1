param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$Compiler,
    [Parameter(Mandatory)][string]$WorkDirectory
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $WorkDirectory | Out-Null
$Probe = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
$Probe.Start()
$Port = $Probe.LocalEndpoint.Port
$Probe.Stop()
$Server = Start-Process -FilePath $Executable -ArgumentList @('--serve', '--port', "$Port") -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput "$WorkDirectory/server.log" -RedirectStandardError "$WorkDirectory/server.error.log"
$Connections = [Collections.Generic.List[IDisposable]]::new()
$Checks = 0

function Check([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    $script:Checks++
}

function Connect-Bridge {
    $Socket = [Net.WebSockets.ClientWebSocket]::new()
    $script:Connections.Add($Socket)
    $Deadline = [Threading.CancellationTokenSource]::new(5000)
    try { $null = $Socket.ConnectAsync([Uri]"ws://127.0.0.1:$Port/decompile", $Deadline.Token).GetAwaiter().GetResult() }
    finally { $Deadline.Dispose() }
    return $Socket
}

function Send-Text($Socket, [string]$Text, [bool]$Final = $true) {
    $Bytes = [Text.Encoding]::UTF8.GetBytes($Text)
    $Deadline = [Threading.CancellationTokenSource]::new(10000)
    try { $null = $Socket.SendAsync([ArraySegment[byte]]::new($Bytes), [Net.WebSockets.WebSocketMessageType]::Text, $Final, $Deadline.Token).GetAwaiter().GetResult() }
    finally { $Deadline.Dispose() }
}

function Receive-Text($Socket) {
    $Buffer = [byte[]]::new(16384)
    $Output = [IO.MemoryStream]::new()
    $Deadline = [Threading.CancellationTokenSource]::new(10000)
    try {
        do {
            $Result = $Socket.ReceiveAsync([ArraySegment[byte]]::new($Buffer), $Deadline.Token).GetAwaiter().GetResult()
            if ($Result.MessageType -ne [Net.WebSockets.WebSocketMessageType]::Text) { throw 'Expected a text response' }
            $Output.Write($Buffer, 0, $Result.Count)
        } while (-not $Result.EndOfMessage)
        return [Text.Encoding]::UTF8.GetString($Output.ToArray())
    } finally { $Deadline.Dispose(); $Output.Dispose() }
}

function Fixture([string]$Name, [string]$Source) {
    $Path = Join-Path $WorkDirectory $Name
    [IO.File]::WriteAllText("$Path.luau", $Source)
    & $Compiler "$Path.luau" "$Path.luac"
    if ($LASTEXITCODE) { throw 'Fixture compilation failed' }
    & $Executable "$Path.luac" -o "$Path.expected.luau"
    if ($LASTEXITCODE) { throw 'CLI decompilation failed' }
    return @{
        Hex = [BitConverter]::ToString([IO.File]::ReadAllBytes("$Path.luac")).Replace('-', '')
        Source = [IO.File]::ReadAllText("$Path.expected.luau")
    }
}

function Open-Raw {
    $Client = [Net.Sockets.TcpClient]::new()
    $script:Connections.Add($Client)
    $Client.ReceiveTimeout = 3000
    $Client.SendTimeout = 3000
    $Client.Connect('127.0.0.1', $Port)
    $Stream = $Client.GetStream()
    $Request = "GET /decompile HTTP/1.1`r`nHost: 127.0.0.1:$Port`r`nUpgrade: websocket`r`nConnection: keep-alive, Upgrade`r`nSec-WebSocket-Version: 13`r`nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==`r`nOrigin: ws://127.0.0.1:$Port`r`n`r`n"
    $Bytes = [Text.Encoding]::ASCII.GetBytes($Request)
    $Stream.Write($Bytes, 0, $Bytes.Length)
    $Header = ''
    while (-not $Header.EndsWith("`r`n`r`n")) {
        $Byte = $Stream.ReadByte()
        if ($Byte -lt 0) { throw 'Handshake disconnected' }
        $Header += [char]$Byte
    }
    Check ($Header.Contains('Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=')) 'RFC 6455 handshake hash differs'
    return $Stream
}

try {
    $Ready = $false
    for ($Attempt = 0; $Attempt -lt 40; $Attempt++) {
        if ($Server.HasExited) { throw 'WebSocket server exited during startup' }
        if ((Test-Path "$WorkDirectory/server.log") -and (Get-Content "$WorkDirectory/server.log" -Raw) -match 'listening') { $Ready = $true; break }
        Start-Sleep -Milliseconds 50
    }
    Check $Ready 'Server did not become ready'
    $Small = Fixture 'small' 'return 37, "hello", nil'
    $Large = Fixture 'large' ('return "' + ('a' * 70000) + '"')
    $Socket = Connect-Bridge
    Send-Text $Socket ("1`n" + $Small.Hex)
    Check ((Receive-Text $Socket) -ceq ("1`nok`n" + $Small.Source)) 'WebSocket output differs from CLI output'
    Send-Text $Socket '2' $false
    Send-Text $Socket ("`n" + $Small.Hex.Substring(0, 7)) $false
    Send-Text $Socket $Small.Hex.Substring(7)
    Check ((Receive-Text $Socket) -ceq ("2`nok`n" + $Small.Source)) 'Fragmented request failed'
    Send-Text $Socket "3`nzz"
    Check ((Receive-Text $Socket).StartsWith("3`nerror`n")) 'Invalid hex should return a request error'
    Send-Text $Socket "4`n00"
    Check ((Receive-Text $Socket).StartsWith("4`nerror`n")) 'Invalid bytecode should return a request error'
    Send-Text $Socket ("5`n" + $Large.Hex)
    Check ((Receive-Text $Socket) -ceq ("5`nok`n" + $Large.Source)) '64-bit frame length or large response failed'
    $Other = Connect-Bridge
    Send-Text $Socket ("6`n" + $Small.Hex)
    Send-Text $Other ("6`n" + $Large.Hex)
    Check ((Receive-Text $Socket) -ceq ("6`nok`n" + $Small.Source)) 'First simultaneous client failed'
    Check ((Receive-Text $Other) -ceq ("6`nok`n" + $Large.Source)) 'Second simultaneous client failed'
    $Socket.Dispose()
    $Other.Dispose()
    $Again = Connect-Bridge
    Send-Text $Again ("7`n" + $Small.Hex)
    Check ((Receive-Text $Again) -ceq ("7`nok`n" + $Small.Source)) 'Reconnect failed'
    $Again.Dispose()

    $Raw = Open-Raw
    [byte[]]$Ping = @(0x89, 0x81, 1, 2, 3, 4, (120 -bxor 1))
    $Raw.Write($Ping, 0, $Ping.Length)
    Check ($Raw.ReadByte() -eq 0x8a -and $Raw.ReadByte() -eq 1 -and $Raw.ReadByte() -eq 120) 'Ping/pong failed'
    [byte[]]$Close = @(0x88, 0x80, 1, 2, 3, 4)
    $Raw.Write($Close, 0, $Close.Length)
    Check ($Raw.ReadByte() -eq 0x88) 'Close handshake failed'
    $Raw.Dispose()

    $Raw = Open-Raw
    [byte[]]$Unmasked = @(0x81, 0)
    $Raw.Write($Unmasked, 0, $Unmasked.Length)
    Check ($Raw.ReadByte() -eq 0x88) 'Unmasked client frame was not rejected'
    $Raw.Dispose()
    $Raw = Open-Raw
    [byte[]]$Oversize = @(0x81, 0xff, 0, 0, 0, 1, 0, 0, 0, 0)
    $Raw.Write($Oversize, 0, $Oversize.Length)
    Check ($Raw.ReadByte() -eq 0x88) 'Oversized frame was not rejected before allocation'
    Write-Output "$Checks WebSocket checks passed"
} finally {
    foreach ($Connection in $Connections) { $Connection.Dispose() }
    if (-not $Server.HasExited) { Stop-Process -Id $Server.Id }
    $Server.Dispose()
}

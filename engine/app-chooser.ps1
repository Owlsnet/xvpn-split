<#
.SYNOPSIS
    Shows which applications use the VPN tunnel and which use the real network, and moves
    applications between the two sides.

.DESCRIPTION
    A full-tunnel VPN such as X-VPN routes all traffic through its tunnel, so every
    application is IN the VPN unless it is explicitly taken OUT. An application is taken OUT
    by sending its traffic through the loopback proxy app-bypass.ps1, whose outbound sockets
    are bound to the physical adapter (IP_UNICAST_IF).

    The script keeps the chosen side of each application in apps.config.json, starts and
    stops the bypass proxy, and reports the live state of every application by grouping the
    established TCP connections of running processes by executable:

        connected to 127.0.0.1:<proxy port>        OUT proxy
        local address = physical adapter address   OUT real
        local address = tunnel adapter address     IN vpn
        both OUT and IN connections                MIXED (typically during a switch)

    Chromium-based browsers only apply --proxy-server at process start, so switching a
    running browser restarts it with or without the flag. The profiles that were open are
    reopened with --profile-directory and --restore-last-session. If an enabled extension
    controls the browser's proxy setting, it overrides the flag; in that case the OUT browser
    runs from a separate clean profile under %LOCALAPPDATA%\xvpn\<browser>-out, and the normal
    profile is used again once no extension blocks it. The OUT window is started with
    --window-name "OUT - real network".

    Other applications cannot be given a proxy by the script and must be configured to use
    127.0.0.1:<port> themselves.

    Without an action parameter, an interactive menu is shown.

.PARAMETER Menu
    Shows the interactive menu. The menu is also shown when no other action parameter is
    given.

.PARAMETER List
    Prints the application table (saved mode and live state) and exits.

.PARAMETER StartProxy
    Starts the bypass proxy (app-bypass.ps1) detached on the proxy port.

.PARAMETER StopProxy
    Stops the bypass proxy if it was started by this script.

.PARAMETER ProxyStatus
    Reports whether the bypass proxy is running and shows its most recent requests.

.PARAMETER Probe
    Runs app-bypass.ps1 -Test on a temporary port (the proxy port + 7).

.PARAMETER Add
    Saves an application with the mode given by -Mode. Accepts an application name, part of
    a name or path from the table, or the full path of an executable.

.PARAMETER Set
    Changes the saved mode of an application shown by -List. The running application is not
    changed.

.PARAMETER Send
    Saves the mode of an application and applies it immediately. A running Chromium browser
    is restarted on the new side.

.PARAMETER Remove
    Removes an application from apps.config.json.

.PARAMETER Launch
    Starts an application on its saved side.

.PARAMETER Mode
    The side used by -Add, -Set and -Send: Vpn (IN, the default) or Real (OUT).

.PARAMETER Port
    Loopback port of the bypass proxy. Defaults to ProxyPort in apps.config.json, or 49500.

.PARAMETER Underlay
    Interface alias of the physical adapter. Defaults to the first connected adapter with an
    IPv4 default gateway that is not a VPN or virtual adapter.

.PARAMETER UserDataDir
    Chromium --user-data-dir to store with the application for -Add, -Set and -Send. With
    -Send, the saved value is kept when this is omitted.

.EXAMPLE
    .\app-chooser.ps1
    Opens the interactive menu.

.EXAMPLE
    .\app-chooser.ps1 -List
    Prints the application table once.

.EXAMPLE
    .\app-chooser.ps1 -Send Chrome -Mode Real
    Saves Chrome as OUT and restarts it through the bypass proxy.

.EXAMPLE
    .\app-chooser.ps1 -Send Chrome -Mode Vpn
    Saves Chrome as IN and restarts it without the proxy.

.EXAMPLE
    .\app-chooser.ps1 -Add 'C:\Program Files\Vendor\App\app.exe' -Mode Real
    Saves an application by its full path without starting it.

.EXAMPLE
    .\app-chooser.ps1 -StartProxy -Port 49501
    Starts the bypass proxy on a non-default port.

.NOTES
    No installation or administrator rights are required. Files written next to the script:

        apps.config.json        saved applications, modes and proxy port
        _bypass.pid             process id, port and start time of the bypass proxy
        _bypass.log             transcript of the bypass proxy
        _bypass.requests.log    per-request log of the bypass proxy

    Browser profile folders created on demand:

        %LOCALAPPDATA%\xvpn\<browser>-out   separate OUT profile, used when an extension
                                            overrides the proxy or when chosen in the menu

    Output lines starting with "[ OK ]", "[WARN]" and "[FAIL]" and the final summary line
    are parsed by xvpn-ui.exe; keep their format stable.
#>
[CmdletBinding()]
param(
    [switch] $Menu,
    [switch] $List,
    [switch] $StartProxy,
    [switch] $StopProxy,
    [switch] $ProxyStatus,
    [switch] $Probe,
    [string] $Add = '',
    [string] $Set = '',
    [string] $Send = '',
    [string] $Remove = '',
    [string] $Launch = '',
    [ValidateSet('Vpn', 'Real')][string] $Mode = 'Vpn',
    [int] $Port = 0,
    [string] $Underlay = '',
    [string] $UserDataDir = ''
)

$ErrorActionPreference = 'Continue'
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$cfgPath   = Join-Path $scriptDir 'apps.config.json'
$pidPath   = Join-Path $scriptDir '_bypass.pid'
$logPath   = Join-Path $scriptDir '_bypass.log'
$reqLogPath = Join-Path $scriptDir '_bypass.requests.log'
$bypassScript = Join-Path $scriptDir 'app-bypass.ps1'
$DefaultPort  = 49500

$script:nOk = 0; $script:nWarn = 0; $script:nBad = 0
function Line {
    param([string]$State, [string]$Text)
    if ($State -eq 'OK')       { Write-Host ('  [ OK ]  ' + $Text) -ForegroundColor Green;  $script:nOk++ }
    elseif ($State -eq 'WARN') { Write-Host ('  [WARN]  ' + $Text) -ForegroundColor Yellow; $script:nWarn++ }
    elseif ($State -eq 'FAIL') { Write-Host ('  [FAIL]  ' + $Text) -ForegroundColor Red;    $script:nBad++ }
    else                       { Write-Host ('          ' + $Text) -ForegroundColor Gray }
}

function Get-Config {
    if (-not (Test-Path $cfgPath)) {
        return [pscustomobject]@{ UpdatedAt = ''; ProxyPort = $DefaultPort; Apps = @() }
    }
    try {
        $c = Get-Content $cfgPath -Raw | ConvertFrom-Json
        if (-not $c.PSObject.Properties['ProxyPort'] -or -not $c.ProxyPort) { $c | Add-Member -NotePropertyName ProxyPort -NotePropertyValue $DefaultPort -Force }
        if (-not $c.PSObject.Properties['Apps']) { $c | Add-Member -NotePropertyName Apps -NotePropertyValue @() -Force }
        return $c
    } catch {
        Line WARN ('apps.config.json could not be read (' + $_.Exception.Message + ') - starting from an empty list')
        return [pscustomobject]@{ UpdatedAt = ''; ProxyPort = $DefaultPort; Apps = @() }
    }
}

function Save-Config {
    param($Cfg)
    $Cfg.UpdatedAt = (Get-Date).ToString('s')
    $Cfg | ConvertTo-Json -Depth 6 | Set-Content -Path $cfgPath -Encoding UTF8
}
function Get-Physical {
    param([string]$Alias = '')
    # The choice matters twice: outbound sockets are pinned to this adapter, and its address
    # is what marks a connection as being on the real network. A virtual adapter (ZeroTier
    # One, Tailscale, Hyper-V, VMware, ...) usually carries a default route of its own
    # without providing internet access, so real hardware is preferred and the name filter is
    # only a fallback for adapters whose kind cannot be read.
    $ExcludeAlias = 'VPN|Loopback|Host-Only|Virtual|TAP|Kernel Debug|Wi-Fi Direct|WAN Miniport|6to4|Teredo|IP-HTTPS'
    $list = @()
    foreach ($cfg in @(Get-NetIPConfiguration -ErrorAction SilentlyContinue)) {
        if ($null -eq $cfg.IPv4Address) { continue }
        if ($cfg.NetAdapter.Status -ne 'Up') { continue }
        if (-not $Alias -and $cfg.InterfaceAlias -match $ExcludeAlias) { continue }
        if ($Alias -and $cfg.InterfaceAlias -ne $Alias) { continue }
        $def = @(Get-NetRoute -InterfaceIndex $cfg.InterfaceIndex -AddressFamily IPv4 -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue)
        if ($null -eq $cfg.IPv4DefaultGateway -and $def.Count -eq 0) { continue }
        $gw = '0.0.0.0'
        if ($null -ne $cfg.IPv4DefaultGateway) { $gw = $cfg.IPv4DefaultGateway[0].NextHop }
        $na = $cfg.NetAdapter
        $hw = $true
        if ($na.PSObject.Properties['HardwareInterface']) { $hw = ($na.HardwareInterface -eq $true) }
        elseif ($na.PSObject.Properties['Virtual'])       { $hw = ($na.Virtual -ne $true) }
        $metric = [int]::MaxValue
        if ($def.Count -gt 0) { $metric = [int](@($def | Sort-Object RouteMetric)[0].RouteMetric) }
        $list += [pscustomobject]@{
            Alias    = $cfg.InterfaceAlias
            IfIndex  = [int]$cfg.InterfaceIndex
            Ip       = $cfg.IPv4Address[0].IPAddress
            Gateway  = $gw
            Metric   = $metric
            Hardware = $hw
        }
    }
    return @($list | Sort-Object @{ Expression = { -not $_.Hardware } }, Metric, IfIndex) | Select-Object -First 1
}

function Get-Tunnel {
    param([int]$UnderlayIfIndex = -1, [string]$Alias = 'X-VPN')
    # Use the interface of the route actually chosen for a public address instead of matching
    # X-VPN's route prefixes, which differ between connections.
    $idx = 0
    $fr = @(Find-NetRoute -RemoteIPAddress '1.1.1.1' -ErrorAction SilentlyContinue)
    if ($fr.Count -ge 2) {
        $ri = [int]$fr[$fr.Count - 1].InterfaceIndex
        if ($ri -ne 0 -and $ri -ne $UnderlayIfIndex) { $idx = $ri }
    }
    if ($idx -eq 0) {
        $c = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object {
            $_.InterfaceIndex -ne 1 -and $_.InterfaceIndex -ne $UnderlayIfIndex -and
            ($_.InterfaceAlias -eq $Alias -or $_.InterfaceAlias -match 'VPN|TUN|WIREGUARD|WG')
        } | Select-Object -First 1
        if ($c) { $idx = [int]$c.InterfaceIndex }
    }
    if ($idx -eq 0) { return $null }
    $ipa = Get-NetIPAddress -InterfaceIndex $idx -AddressFamily IPv4 -ErrorAction SilentlyContinue | Select-Object -First 1
    $ipi = Get-NetIPInterface -InterfaceIndex $idx -AddressFamily IPv4 -ErrorAction SilentlyContinue
    [pscustomobject]@{ IfIndex = $idx; Alias = $ipi.InterfaceAlias; Ip = $ipa.IPAddress }
}

function Get-ProcPaths {
    $map = @{}
    foreach ($p in @(Get-Process -ErrorAction SilentlyContinue)) {
        try { if ($p.Path) { $map[[int]$p.Id] = $p.Path } } catch { }
    }
    return $map
}

function Get-LiveMap {
    param($Phys, $Tunnel, [int]$ProxyPort = 0)
    # Groups established TCP connections by executable path:
    #   remote 127.0.0.1:<proxy port>        -> uses the bypass proxy       (OUT)
    #   local address = physical adapter IP  -> leaves over the real NIC    (OUT)
    #   local address = tunnel adapter IP    -> inside the VPN              (IN)
    $live = @{}
    foreach ($c in @(Get-NetTCPConnection -State Established -ErrorAction SilentlyContinue)) {
        $proc = [int]$c.OwningProcess
        if ($proc -le 0) { continue }
        if (-not $script:ProcPaths.ContainsKey($proc)) { continue }
        $key = $script:ProcPaths[$proc]
        if (-not $live.ContainsKey($key)) { $live[$key] = [pscustomobject]@{ Real = 0; Vpn = 0; Proxy = 0; Other = 0; Pids = 0 } }
        if ($script:SeenPids -notcontains $proc) { $script:SeenPids += $proc; $live[$key].Pids++ }
        $la = [string]$c.LocalAddress
        $toProxy = ($ProxyPort -gt 0 -and [string]$c.RemoteAddress -eq '127.0.0.1' -and [int]$c.RemotePort -eq $ProxyPort)
        if ($toProxy)                                { $live[$key].Proxy++ }
        elseif ($Phys -and $la -eq $Phys.Ip)         { $live[$key].Real++ }
        elseif ($Tunnel -and $la -eq $Tunnel.Ip)     { $live[$key].Vpn++ }
        else                                         { $live[$key].Other++ }
    }
    return $live
}

function Get-KnownPaths {
    $out = @()
    $literal = @(
        'C:\Program Files\Google\Chrome\Application\chrome.exe',
        'C:\Program Files (x86)\Google\Chrome\Application\chrome.exe',
        'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe',
        'C:\Program Files\BraveSoftware\Brave-Browser\Application\brave.exe',
        'C:\Program Files\Mozilla Firefox\firefox.exe',
        "$env:APPDATA\Telegram Desktop\Telegram.exe",
        "$env:LOCALAPPDATA\Programs\Opera\opera.exe",
        'C:\Program Files\VideoLAN\VLC\vlc.exe',
        "$env:APPDATA\Spotify\Spotify.exe"
    )
    foreach ($p in $literal) { if (Test-Path $p) { $out += $p } }
    # Discord installs into a versioned subfolder
    $d = Get-ChildItem (Join-Path $env:LOCALAPPDATA 'Discord') -Filter 'Discord.exe' -Recurse -Depth 2 -ErrorAction SilentlyContinue |
         Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($d) { $out += $d.FullName }
    return @($out | Select-Object -Unique)
}
function Get-Rows {
    param($Cfg, $Live)
    $rows = @{}
    foreach ($a in @($Cfg.Apps)) {
        if (-not $a) { continue }
        $rows[[string]$a.Path] = [pscustomobject]@{
            Name = [string]$a.Name; Path = [string]$a.Path; Mode = [string]$a.Mode
            Config = $true; UserDataDir = [string]$a.UserDataDir
        }
    }
    foreach ($k in @($Live.Keys)) {
        if (-not $rows.ContainsKey([string]$k)) {
            $rows[[string]$k] = [pscustomobject]@{
                Name = [System.IO.Path]::GetFileNameWithoutExtension($k); Path = [string]$k
                Mode = 'Vpn'; Config = $false; UserDataDir = ''
            }
        }
    }
    foreach ($p in @(Get-KnownPaths)) {
        if (-not $rows.ContainsKey($p)) {
            $rows[$p] = [pscustomobject]@{
                Name = [System.IO.Path]::GetFileNameWithoutExtension($p); Path = $p
                Mode = 'Vpn'; Config = $false; UserDataDir = ''
            }
        }
    }
    return @($rows.Values | Sort-Object -Property @{Expression = { $_.Config }; Descending = $true }, @{Expression = { $_.Name } })
}

function Get-LiveText {
    param($Live, [string]$Path)
    if (-not $Live.ContainsKey($Path)) { return 'idle' }
    $l = $Live[$Path]
    $out = $l.Real + $l.Proxy
    if ($out -gt 0 -and $l.Vpn -gt 0) { return 'MIXED' }
    if ($l.Proxy -gt 0 -and $l.Real -eq 0) { return 'OUT proxy' }
    if ($l.Real -gt 0)                { return 'OUT real' }
    if ($l.Vpn -gt 0)                 { return 'IN vpn' }
    return 'idle'
}

function Show-Apps {
    param($Cfg, $Phys, $Tunnel, $Live, [switch]$Numbered)
    $rows = Get-Rows -Cfg $Cfg -Live $Live
    Write-Host ''
    $tunTxt = 'no tunnel up'
    if ($Tunnel) { $tunTxt = 'tunnel ' + $Tunnel.Alias + ' ' + $Tunnel.Ip }
    $nicTxt = 'no NIC'
    if ($Phys) { $nicTxt = 'real ' + $Phys.Alias + ' ' + $Phys.Ip }
    Write-Host (' apps  -  mode: IN = keep in X-VPN, OUT = send to the real network   (' + $tunTxt + ' / ' + $nicTxt + ')') -ForegroundColor White
    Write-Host ('   #  app                  mode    live        path') -ForegroundColor DarkGray
    $i = 0
    foreach ($r in $rows) {
        $i++
        $mColor = 'DarkGray'
        if ($r.Mode -eq 'Real') { $mColor = 'Yellow' }
        $lt = Get-LiveText -Live $Live -Path $r.Path
        $lc = 'DarkGray'
        if ($lt -like 'OUT*') { $lc = 'Yellow' }
        elseif ($lt -like 'IN*') { $lc = 'Green' }
        elseif ($lt -eq 'MIXED') { $lc = 'Magenta' }
        $prefix = '    '
        if ($Numbered) { $prefix = ('  {0,2}. ' -f $i) }
        $nm = $r.Name
        if ($nm.Length -gt 24) { $nm = $nm.Substring(0, 23) + '~' }
        Write-Host ($prefix + $nm.PadRight(25)) -NoNewline
        if ($r.Mode -eq 'Real') { Write-Host 'OUT     ' -NoNewline -ForegroundColor Yellow } else { Write-Host 'IN      ' -NoNewline -ForegroundColor DarkGray }
        Write-Host ($lt.PadRight(12)) -NoNewline -ForegroundColor $lc
        $p = $r.Path
        if ($p.Length -gt 58) { $p = '...' + $p.Substring($p.Length - 55) }
        Write-Host $p -ForegroundColor DarkGray
    }
    Write-Host ''
    Line Info 'mode = the saved choice   live = what the app''s sockets are currently doing'
    Line Info 'everything is IN the VPN by default; OUT means it was started through the loopback proxy'
    Line Info 'live OUT proxy = it is connected to the bypass proxy;  OUT real = its own socket used your NIC'
    Line Info 'live "OUT real" can also be a connection opened BEFORE the VPN came up - it keeps its old path until it reconnects'
    return $rows
}

function Find-AppRow {
    param($Rows, [string]$Needle)
    $n = $Needle.Trim()
    if ($n -eq '') { return $null }
    # exact path, then exact name, then substring on either
    $hit = @($Rows | Where-Object { $_.Path -eq $n })
    if ($hit.Count -eq 0) { $hit = @($Rows | Where-Object { $_.Name -eq $n }) }
    if ($hit.Count -eq 0) { $hit = @($Rows | Where-Object { $_.Name -like ('*' + $n + '*') -or $_.Path -like ('*' + $n + '*') }) }
    if ($hit.Count -eq 0) { return $null }
    return $hit[0]
}

function Set-AppMode {
    param($Cfg, [string]$Path, [string]$Name, [string]$NewMode, [string]$DataDir)
    $apps = New-Object System.Collections.ArrayList
    foreach ($a in @($Cfg.Apps)) { if ([string]$a.Path -ne $Path) { [void]$apps.Add($a) } }
    [void]$apps.Add([pscustomobject]@{ Name = $Name; Path = $Path; Mode = $NewMode; UserDataDir = [string]$DataDir })
    $Cfg.Apps = @($apps.ToArray())
    Save-Config -Cfg $Cfg
    Line OK ('saved: ' + $Name + '  ->  ' + $NewMode + '   (' + (Split-Path -Leaf $cfgPath) + ')')
    if ($NewMode -eq 'Real') { Line Info 'launch it from here (or with app-bypass.ps1) to keep it on the real network' }
}

function Remove-App {
    param($Cfg, [string]$Path)
    $apps = New-Object System.Collections.ArrayList
    foreach ($a in @($Cfg.Apps)) { if ([string]$a.Path -ne $Path) { [void]$apps.Add($a) } }
    $Cfg.Apps = @($apps.ToArray())
    Save-Config -Cfg $Cfg
    Line OK ('removed from the list: ' + $Path)
}
function Get-ProxyState {
    if (-not (Test-Path $pidPath)) { return $null }
    $j = $null
    try { $j = Get-Content $pidPath -Raw | ConvertFrom-Json } catch { return $null }
    if (-not $j) { return $null }
    $procId = [int]$j.ProcessId
    if (-not (Get-Process -Id $procId -ErrorAction SilentlyContinue)) { return $null }
    $cmd = ''
    $ci = Get-CimInstance Win32_Process -Filter ('ProcessId=' + $procId) -ErrorAction SilentlyContinue
    if ($ci) { $cmd = [string]$ci.CommandLine }
    if ($cmd -notmatch 'app-bypass') { return $null }   # PID may have been reused by an unrelated process
    return [pscustomobject]@{ ProcessId = $procId; Port = [int]$j.Port; StartedAt = [string]$j.StartedAt }
}

function Start-Proxy {
    param([int]$UsePort)
    $st = Get-ProxyState
    if ($st) { return $st }
    if (-not (Test-Path $bypassScript)) { Line FAIL 'app-bypass.ps1 is not next to this script'; return $null }
    $occupied = @(Get-NetTCPConnection -State Listen -LocalPort $UsePort -ErrorAction SilentlyContinue)
    if ($occupied.Count -gt 0) {
        $owner = [int]$occupied[0].OwningProcess
        Line WARN ('port ' + $UsePort + ' is already being listened on (pid ' + $owner + ')')
        Line Info 'that is probably an app-bypass.ps1 you started yourself - OUT apps can use it as it is'
        Line Info ('nothing new was started; if it is not app-bypass.ps1, pick another port:  -Port ' + ($UsePort + 1))
        return [pscustomobject]@{ ProcessId = $owner; Port = $UsePort; StartedAt = 'already running' }
    }
    Remove-Item $logPath -ErrorAction SilentlyContinue
    Remove-Item $reqLogPath -ErrorAction SilentlyContinue
    # Start the proxy fully detached through WMI (Win32_Process.Create). A Start-Process child
    # inherits this console's handles, so a caller reading this script's output (such as the
    # GUI) would keep waiting on the pipes for as long as the proxy runs. -Log writes the
    # transcript; -RequestLog receives the per-request lines, which are written from C# and
    # cannot be captured by a transcript.
    $cmdLine = 'powershell.exe -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File "' + $bypassScript + '" -Port ' + $UsePort + ' -Log "' + $logPath + '" -RequestLog "' + $reqLogPath + '"'
    $r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = $cmdLine } -ErrorAction SilentlyContinue
    if (-not $r -or $r.ReturnValue -ne 0) {
        Line FAIL ('could not start app-bypass.ps1 (Win32_Process.Create returned ' + $(if ($r) { $r.ReturnValue } else { 'nothing' }) + ')')
        return $null
    }
    $procId = [int]$r.ProcessId
    # app-bypass.ps1 compiles its C# proxy at startup, which takes a few seconds
    $deadline = (Get-Date).AddSeconds(25)
    $listening = $false
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 800
        if (@(Get-NetTCPConnection -State Listen -LocalPort $UsePort -ErrorAction SilentlyContinue).Count -gt 0) { $listening = $true; break }
        if (-not (Get-Process -Id $procId -ErrorAction SilentlyContinue)) { break }
    }
    if (-not $listening) {
        Line FAIL ('the proxy did not come up on 127.0.0.1:' + $UsePort + ' - see ' + (Split-Path -Leaf $logPath))
        return $null
    }
    [pscustomobject]@{ ProcessId = $procId; Port = $UsePort; StartedAt = (Get-Date).ToString('s') } |
        ConvertTo-Json | Set-Content -Path $pidPath -Encoding UTF8
    Line OK ('bypass proxy started on 127.0.0.1:' + $UsePort + '  (pid ' + $procId + ', log ' + (Split-Path -Leaf $logPath) + ')')
    return [pscustomobject]@{ ProcessId = $procId; Port = $UsePort; StartedAt = '' }
}

function Stop-Proxy {
    param([int]$UsePort = 0)
    if ($UsePort -le 0) { $UsePort = $DefaultPort }
    $st = Get-ProxyState
    if (-not $st) {
        Remove-Item $pidPath -ErrorAction SilentlyContinue
        Line Info 'no bypass proxy started from here is running'
        $occ = @(Get-NetTCPConnection -State Listen -LocalPort $UsePort -ErrorAction SilentlyContinue)
        if ($occ.Count -gt 0) { Line Info ('port ' + $UsePort + ' is still in use by pid ' + [int]$occ[0].OwningProcess + ' - that one was started outside this script; close it yourself') }
        return
    }
    Stop-Process -Id $st.ProcessId -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 600
    Remove-Item $pidPath -ErrorAction SilentlyContinue
    Line OK ('stopped the bypass proxy (pid ' + $st.ProcessId + ', port ' + $st.Port + ')')
    Line Info 'apps that used it go back to the VPN as soon as they reconnect'
}

function Test-ChromiumLike {
    param([string]$Path)
    $n = [System.IO.Path]::GetFileNameWithoutExtension($Path).ToLowerInvariant()
    return @('chrome', 'msedge', 'brave', 'chromium', 'vivaldi', 'opera') -contains $n
}

# --------------------------------------------- Chromium browsers: switch sides ---
# A running Chromium browser hands every new launch to its existing process, and that process
# keeps the proxy configuration it was started with. Moving a browser to the other side
# therefore requires a restart: end the browser process, wait until it is gone, and start it
# again with or without --proxy-server, plus --restore-last-session so the tabs come back.

function Get-BrowserMain {
    # Main browser process(es) for this executable and user data dir (no --type= children)
    param([string]$Path, [string]$DataDir)
    $procs = @(Get-CimInstance Win32_Process -Filter ("Name='" + [System.IO.Path]::GetFileName($Path) + "'") -ErrorAction SilentlyContinue |
        Where-Object { $_.ExecutablePath -and $_.ExecutablePath -ieq $Path -and $_.CommandLine -notmatch '--type=' })
    return @($procs | Where-Object {
        $m = [regex]::Match([string]$_.CommandLine, '--user-data-dir="?([^"]+?)"?(\s--|$)')
        $dir = if ($m.Success) { $m.Groups[1].Value.Trim() } else { '' }
        if ($DataDir) { $dir -ieq $DataDir } else { $dir -eq '' }
    })
}

function Get-BrowserSide {
    # OUT when the running browser was started with the bypass proxy, IN otherwise
    param([string]$Path, [string]$DataDir, [int]$UsePort)
    $main = @(Get-BrowserMain -Path $Path -DataDir $DataDir)
    if ($main.Count -eq 0) { return 'closed' }
    if ([string]$main[0].CommandLine -match ('--proxy-server=\S*127\.0\.0\.1:' + $UsePort)) { return 'OUT' }
    return 'IN'
}

function Get-BrowserUserData {
    # The browser's --user-data-dir, or the vendor default location
    param([string]$Path, [string]$DataDir)
    if ($DataDir) { return $DataDir }
    $n = [System.IO.Path]::GetFileNameWithoutExtension($Path).ToLowerInvariant()
    $map = @{
        'chrome'   = 'Google\Chrome\User Data'
        'msedge'   = 'Microsoft\Edge\User Data'
        'brave'    = 'BraveSoftware\Brave-Browser\User Data'
        'chromium' = 'Chromium\User Data'
        'vivaldi'  = 'Vivaldi\User Data'
    }
    if (-not $map.ContainsKey($n)) { return '' }
    return (Join-Path $env:LOCALAPPDATA $map[$n])
}

function Get-OpenProfiles {
    # Profile folders that are currently open. Must be read before the browser is closed,
    # because Chrome clears this list on shutdown. Without --profile-directory a restart
    # would stop at the profile picker.
    param([string]$Path, [string]$DataDir)
    $ud = Get-BrowserUserData -Path $Path -DataDir $DataDir
    if (-not $ud) { return @() }
    $ls = $null
    try { $ls = Get-Content (Join-Path $ud 'Local State') -Raw -ErrorAction Stop | ConvertFrom-Json } catch { return @() }
    $open = @($ls.profile.last_active_profiles | Where-Object { $_ })
    if ($open.Count -eq 0 -and $ls.profile.last_used) { $open = @([string]$ls.profile.last_used) }
    return $open
}

function Get-ProxyExtensions {
    # Enabled extensions that control the proxy setting in these profiles. Extension-set
    # preferences take precedence over --proxy-server, so while one of them is enabled
    # (typically a VPN or proxy extension) the browser ignores the bypass proxy and stays IN.
    param([string]$Path, [string]$DataDir, [string[]]$Profiles)
    $ud = Get-BrowserUserData -Path $Path -DataDir $DataDir
    if (-not $ud) { return @() }
    $hits = @()
    foreach ($pd in $Profiles) {
        $dir = Join-Path $ud $pd
        foreach ($f in 'Secure Preferences', 'Preferences') {
            $j = $null
            try { $j = Get-Content (Join-Path $dir $f) -Raw -ErrorAction Stop | ConvertFrom-Json } catch { continue }
            $st = $j.extensions.settings
            if (-not $st) { continue }
            foreach ($p in $st.PSObject.Properties) {
                $e = $p.Value
                if (-not $e.preferences -or -not $e.preferences.proxy) { continue }
                if (@($e.disable_reasons | Where-Object { $_ }).Count -gt 0) { continue }
                $name = [string]$e.manifest.name
                if (-not $name -or $name -like '__MSG_*') {
                    $mf = Get-ChildItem (Join-Path $dir ('Extensions\' + $p.Name)) -Recurse -Filter manifest.json -ErrorAction SilentlyContinue | Select-Object -First 1
                    if ($mf) { try { $name = [string](Get-Content $mf.FullName -Raw | ConvertFrom-Json).name } catch { } }
                }
                if (-not $name -or $name -like '__MSG_*') { $name = $p.Name }
                $hits += [pscustomobject]@{ Profile = $pd; Id = $p.Name; Name = $name }
            }
        }
    }
    return @($hits | Sort-Object Profile, Id -Unique)
}

function Test-BrowserReallyOut {
    # The proxy flag alone does not make a browser OUT: an extension that controls the proxy
    # setting overrides it. Reports FAIL and returns $false when such an extension is enabled
    # in the profiles in use.
    param($Row, [string[]]$Profiles)
    $chk = @($Profiles | Where-Object { $_ })
    if ($chk.Count -eq 0) { $chk = @(Get-OpenProfiles -Path $Row.Path -DataDir $Row.UserDataDir) }
    if ($chk.Count -eq 0) { $chk = @('Default') }
    $blockers = @(Get-ProxyExtensions -Path $Row.Path -DataDir $Row.UserDataDir -Profiles $chk)
    if ($blockers.Count -eq 0) { return $true }
    foreach ($b in $blockers) {
        Line FAIL ($Row.Name + ' runs with the proxy, but the extension "' + $b.Name + '" (profile ' + $b.Profile + ') controls its proxy setting and overrides it - it is still IN')
    }
    Line Info ('fix: in ' + $Row.Name + ' open  chrome://extensions  and switch that extension off, then run this again')
    Line Info 'or give the OUT browser its own clean profile:  -Send <app> -Mode Real -UserDataDir <folder>'
    return $false
}

function Stop-Browser {
    param([string]$Path, [string]$DataDir, [string]$Name, [switch]$Closing)
    $main = @(Get-BrowserMain -Path $Path -DataDir $DataDir)
    if ($main.Count -eq 0) { return $true }
    if ($Closing) { Line Info ('closing ' + $Name) }
    else          { Line Info ('restarting ' + $Name + ' on the other side - it reopens your tabs') }
    if ($Closing) {
        # Closing only (the separate OUT profile): request a normal close first so the browser
        # shuts down cleanly and does not show "Restore pages?" on its next start.
        foreach ($m in $main) { & taskkill.exe /PID ([int]$m.ProcessId) 2>&1 | Out-Null }
        $deadline = (Get-Date).AddSeconds(6)
        while ((Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 300
            if (@(Get-BrowserMain -Path $Path -DataDir $DataDir).Count -eq 0) { break }
        }
        $main = @(Get-BrowserMain -Path $Path -DataDir $DataDir)
    }
    # Restarting: end the browser with its windows still open. A normal close (WM_CLOSE) does not
    # work here: if the browser keeps running in the background after its windows close, it
    # records them as intentionally closed and --restore-last-session has nothing to restore.
    # Ending the process leaves the session files listing every open tab, as after a crash.
    foreach ($m in $main) { & taskkill.exe /F /T /PID ([int]$m.ProcessId) 2>&1 | Out-Null }
    $deadline = (Get-Date).AddSeconds(8)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 300
        if (@(Get-BrowserMain -Path $Path -DataDir $DataDir).Count -eq 0) { break }
    }
    if (@(Get-BrowserMain -Path $Path -DataDir $DataDir).Count -gt 0) {
        Line FAIL ('could not close ' + $Name + ' - close it yourself and try again')
        return $false
    }
    # renderer/gpu children can outlive the browser for a moment and hold the profile lock
    Start-Sleep -Milliseconds 700
    return $true
}

function Get-DefaultOutProfile {
    param([string]$Path)
    $n = [System.IO.Path]::GetFileNameWithoutExtension($Path).ToLowerInvariant()
    return (Join-Path $env:LOCALAPPDATA ('xvpn\' + $n + '-out'))
}

function Switch-Browser {
    param($Row, [int]$UsePort)
    $want = if ($Row.Mode -eq 'Real') { 'OUT' } else { 'IN' }

    # The separate OUT profile is selected automatically only when an extension blocks the
    # normal profile. Once nothing blocks it, switch back to the normal profile (logins, tabs).
    $auto = Get-DefaultOutProfile -Path $Row.Path
    if ($Row.UserDataDir -and $Row.UserDataDir -ieq $auto) {
        $prof = @(Get-OpenProfiles -Path $Row.Path -DataDir '')
        if ($prof.Count -eq 0) { $prof = @('Default') }
        if (@(Get-ProxyExtensions -Path $Row.Path -DataDir '' -Profiles $prof).Count -eq 0) {
            Line Info ('nothing overrides the proxy in your normal ' + $Row.Name + ' any more - using your normal profile again')
            if (@(Get-BrowserMain -Path $Row.Path -DataDir $auto).Count -gt 0) {
                if (-not (Stop-Browser -Path $Row.Path -DataDir $auto -Name ('the separate OUT ' + $Row.Name) -Closing)) { return }
            }
            $Row.UserDataDir = ''
            $c = Get-Config
            Set-AppMode -Cfg $c -Path $Row.Path -Name $Row.Name -NewMode $Row.Mode -DataDir ''
        }
    }

    # An extension that controls the proxy setting in the normal profile (typically a VPN or
    # proxy extension) takes precedence over --proxy-server, so restarting that profile cannot
    # make it OUT. The OUT browser then gets its own clean profile and runs next to the normal one.
    if ($want -eq 'OUT' -and -not $Row.UserDataDir) {
        $prof = @(Get-OpenProfiles -Path $Row.Path -DataDir '')
        if ($prof.Count -eq 0) { $prof = @('Default') }
        $blk = @(Get-ProxyExtensions -Path $Row.Path -DataDir '' -Profiles $prof)
        if ($blk.Count -gt 0) {
            $Row.UserDataDir = Get-DefaultOutProfile -Path $Row.Path
            Line Info ('"' + $blk[0].Name + '" controls the proxy in your normal ' + $Row.Name + ', so OUT uses a separate clean profile: ' + $Row.UserDataDir)
            Line Info ('your normal ' + $Row.Name + ' keeps running IN the VPN next to it (sign in to the OUT one once, it remembers)')
            $c = Get-Config
            Set-AppMode -Cfg $c -Path $Row.Path -Name $Row.Name -NewMode 'Real' -DataDir $Row.UserDataDir
        }
    }

    # Separate-profile mode: the normal profile is the IN browser, so IN means closing the OUT one
    if ($want -eq 'IN' -and $Row.UserDataDir) {
        if (@(Get-BrowserMain -Path $Row.Path -DataDir $Row.UserDataDir).Count -gt 0) {
            if (-not (Stop-Browser -Path $Row.Path -DataDir $Row.UserDataDir -Name ('the OUT ' + $Row.Name) -Closing)) { return }
        }
        $normal = Get-BrowserSide -Path $Row.Path -DataDir '' -UsePort $UsePort
        if ($normal -eq 'closed') { Start-Process -FilePath $Row.Path | Out-Null }
        Line OK ($Row.Name + ' is IN - the OUT window is closed and your normal ' + $Row.Name + ' uses the VPN')
        if ($normal -eq 'OUT') {
            Line Info ('note: your normal ' + $Row.Name + ' was started with the proxy flag earlier; it stays IN only while an extension overrides that - restart it once to clear the flag')
        }
        return
    }

    $side = Get-BrowserSide -Path $Row.Path -DataDir $Row.UserDataDir -UsePort $UsePort
    $bargs = @()
    if ($want -eq 'OUT') {
        $st = Start-Proxy -UsePort $UsePort
        if (-not $st) { Line FAIL ($Row.Name + ' stays where it is - the proxy is not available'); return }
        $bargs += @('--proxy-server=http://127.0.0.1:' + $st.Port, '--disable-quic')
    }
    if ($Row.UserDataDir) { $bargs += @('--user-data-dir=' + $Row.UserDataDir, '--no-first-run', '--no-default-browser-check') }
    $bargs += '--hide-crash-restore-bubble'
    $outTitle = 'OUT - real network'
    if ($want -eq 'OUT') {
        # A fixed window title (instead of the page title) distinguishes the OUT window from
        # the normal (VPN) one in the title bar and on the taskbar.
        $bargs += ('--window-name="' + $outTitle + '"')
    }

    $profiles = @()
    if ($side -eq $want) {
        # Already on the requested side: open a window in that browser. With a separate profile,
        # a plain launch would open in the normal (VPN) browser instead.
        if ($Row.UserDataDir) {
            $open = @('--user-data-dir=' + $Row.UserDataDir)
            if ($want -eq 'OUT') { $open += @('--new-window', ('--window-name="' + $outTitle + '"')) }
            Start-Process -FilePath $Row.Path -ArgumentList $open | Out-Null
        }
        if ($want -eq 'OUT' -and -not (Test-BrowserReallyOut -Row $Row -Profiles $profiles)) { return }
        if ($Row.UserDataDir) {
            Line OK ($Row.Name + ' is already ' + $want + ' - opened a window in it')
            if ($want -eq 'OUT') { Line Info ('the OUT window is the one titled "' + $outTitle + '" - every other ' + $Row.Name + ' window stays in the VPN') }
        } else {
            Line OK ($Row.Name + ' is already ' + $want + ' - nothing to change')
        }
        return
    }
    if ($side -ne 'closed') {
        $profiles = @(Get-OpenProfiles -Path $Row.Path -DataDir $Row.UserDataDir)
        if (-not (Stop-Browser -Path $Row.Path -DataDir $Row.UserDataDir -Name $Row.Name)) { return }
        $bargs += '--restore-last-session'
    }
    if ($profiles.Count -eq 0) {
        if ($bargs.Count -gt 0) { Start-Process -FilePath $Row.Path -ArgumentList $bargs | Out-Null }
        else                    { Start-Process -FilePath $Row.Path | Out-Null }
    } else {
        # One launch per previously open profile: the first starts the browser with the flags,
        # the others join that process and restore their own windows.
        $first = $true
        foreach ($pd in $profiles) {
            Start-Process -FilePath $Row.Path -ArgumentList ($bargs + ('--profile-directory="' + $pd + '"')) | Out-Null
            if ($first) { Start-Sleep -Milliseconds 1500; $first = $false }
        }
        Line Info ('reopened profile(s): ' + ($profiles -join ', '))
    }

    # Determine the resulting side from the running process's command line
    $now = 'closed'
    $deadline = (Get-Date).AddSeconds(10)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
        $now = Get-BrowserSide -Path $Row.Path -DataDir $Row.UserDataDir -UsePort $UsePort
        if ($now -ne 'closed') { break }
    }
    if ($now -eq $want -and $want -eq 'OUT' -and -not (Test-BrowserReallyOut -Row $Row -Profiles $profiles)) {
        # Started with the proxy flag but overridden by an extension; reported by Test-BrowserReallyOut
    } elseif ($now -eq $want) {
        if ($want -eq 'OUT') {
            Line OK ($Row.Name + ' is OUT - it runs through 127.0.0.1:' + $UsePort + ' on the real network; every other app stays in the VPN')
            if ($Row.UserDataDir) { Line Info ('the OUT window is the one titled "' + $outTitle + '" - your normal ' + $Row.Name + ' windows stay in the VPN') }
        }
        else { Line OK ($Row.Name + ' is back IN the VPN') }
    } elseif ($now -eq 'closed') {
        Line FAIL ($Row.Name + ' did not start - launch it yourself; mode is saved as ' + $want)
    } else {
        Line FAIL ($Row.Name + ' came up ' + $now + ' instead of ' + $want + ' - close it completely and run this again')
    }
}

function Start-AppInMode {
    param($Row, [int]$UsePort, [switch]$Quiet)
    if (Test-ChromiumLike -Path $Row.Path) { Switch-Browser -Row $Row -UsePort $UsePort; return }
    if ($Row.Mode -ne 'Real') {
        Start-Process -FilePath $Row.Path | Out-Null
        Line OK ('launched ' + $Row.Name + ' normally - it stays IN the VPN')
        return
    }
    $st = Start-Proxy -UsePort $UsePort
    if (-not $st) { return }
    Start-Process -FilePath $Row.Path | Out-Null
    Line WARN ($Row.Name + ' is not a Chromium browser, so it cannot be given the proxy by a flag')
    Line Info ('set its own proxy to 127.0.0.1:' + $st.Port + '; until you do, it stays IN the VPN')
    Line Info 'apps that have no proxy setting at all: use .\split-tunnel.ps1 -Exclude <ip|host> -Apply'
}

function Show-ProxyStatus {
    $st = Get-ProxyState
    if ($st) {
        Line OK ('bypass proxy running: 127.0.0.1:' + $st.Port + '  pid ' + $st.ProcessId + '  since ' + $st.StartedAt)
        $src = $reqLogPath
        if (-not (Test-Path $src)) { $src = $logPath }
        if (Test-Path $src) {
            $last = @(Get-Content $src -ErrorAction SilentlyContinue | Where-Object { $_ -match 'CONNECT|GET |POST |FAILED' } | Select-Object -Last 5)
            if ($last.Count -eq 0) { Line Info 'no requests through it yet' }
            foreach ($l in $last) { Line Info ('  ' + $l.Trim()) }
        }
    } else {
        Line Info 'no bypass proxy running - apps marked OUT will not be able to leave until you start it'
    }
}
function Select-AppRow {
    param($Rows, [string]$Prompt)
    $sel = Read-Host $Prompt
    if (-not $sel) { return $null }
    $n = $sel.Trim()
    if ($n -match '^\d+$') {
        $i = [int]$n
        if ($i -ge 1 -and $i -le $Rows.Count) { return $Rows[$i - 1] }
        Line WARN ('there is no entry number ' + $i)
        return $null
    }
    $r = Find-AppRow -Rows $Rows -Needle $n
    if (-not $r) { Line WARN ('nothing matches "' + $n + '" - use a number from the list, a name, or a full path') }
    return $r
}

function Invoke-Probe {
    param([int]$UsePort)
    if (-not (Test-Path $bypassScript)) { Line FAIL 'app-bypass.ps1 is not next to this script'; return }
    $testPort = $UsePort + 7
    Line Info ('running app-bypass.ps1 -Test on port ' + $testPort + ' (it uses its own temporary proxy)')
    & powershell -NoProfile -ExecutionPolicy Bypass -File $bypassScript -Test -Port $testPort -NoLog
}

function Set-ModeInteractive {
    param($Cfg, $Rows, [string]$NewMode, [int]$UsePort)
    $row = Select-AppRow -Rows $Rows -Prompt '  which app (number, name or path)'
    if (-not $row) { return }
    $dd = $row.UserDataDir
    if ($NewMode -eq 'Real' -and (Test-ChromiumLike -Path $row.Path)) {
        $ans = Read-Host ('  give ' + $row.Name + ' its own profile folder so it can run next to your normal one? (y/N)')
        if ($ans -and $ans.Trim().ToLowerInvariant().StartsWith('y')) {
            $dd = Get-DefaultOutProfile -Path $row.Path
            Line Info ('separate profile: ' + $dd)
            Line Info 'first start is a fresh browser profile (no bookmarks/logins)'
        } else {
            $dd = ''
            Line Info 'uses your normal profile - a running browser is restarted on the new side'
        }
    }
    Set-AppMode -Cfg $Cfg -Path $row.Path -Name $row.Name -NewMode $NewMode -DataDir $dd
    # A running browser only changes side on restart, so apply the new mode immediately
    if ((Test-ChromiumLike -Path $row.Path) -and (Get-BrowserSide -Path $row.Path -DataDir $dd -UsePort $UsePort) -ne 'closed') {
        $row.Mode = $NewMode; $row.UserDataDir = $dd
        Switch-Browser -Row $row -UsePort $UsePort
    }
}

function Invoke-Menu {
    param([int]$UsePort)
    $cfg = Get-Config
    while ($true) {
        $phys = Get-Physical -Alias $Underlay
        $tIdx = -1
        if ($phys) { $tIdx = $phys.IfIndex }
        $tunnel = Get-Tunnel -UnderlayIfIndex $tIdx
        $script:ProcPaths = Get-ProcPaths
        $script:SeenPids = @()
        $live = Get-LiveMap -Phys $phys -Tunnel $tunnel -ProxyPort $UsePort
        $rows = Show-Apps -Cfg $cfg -Phys $phys -Tunnel $tunnel -Live $live -Numbered
        $pst = Get-ProxyState
        if ($pst) { Write-Host ('  proxy: running on 127.0.0.1:' + $pst.Port + ' (pid ' + $pst.ProcessId + ')') -ForegroundColor DarkGray }
        else      { Write-Host '  proxy: not running - it starts by itself when you launch an OUT app' -ForegroundColor DarkGray }
        Write-Host ''
        Write-Host '  1) refresh' -ForegroundColor White
        Write-Host '  2) keep an app IN the VPN          (mode = IN)' -ForegroundColor White
        Write-Host '  3) send an app OUT to the real net (mode = OUT)' -ForegroundColor White
        Write-Host '  4) launch an app in its mode' -ForegroundColor White
        Write-Host '  5) start / stop the bypass proxy' -ForegroundColor White
        Write-Host '  6) probe: two paths, two addresses' -ForegroundColor White
        Write-Host '  7) forget an app' -ForegroundColor White
        Write-Host '  0) quit' -ForegroundColor White
        $choice = (Read-Host '  choice').Trim()
        Write-Host ''
        if ($choice -eq '0') { return }
        elseif ($choice -eq '1')                 { }
        elseif ($choice -eq '2')                 { Set-ModeInteractive -Cfg $cfg -Rows $rows -NewMode 'Vpn' -UsePort $UsePort }
        elseif ($choice -eq '3')                 { Set-ModeInteractive -Cfg $cfg -Rows $rows -NewMode 'Real' -UsePort $UsePort }
        elseif ($choice -eq '4') {
            $row = Select-AppRow -Rows $rows -Prompt '  which app to launch (number, name or path)'
            if ($row) { Start-AppInMode -Row $row -UsePort $UsePort }
        }
        elseif ($choice -eq '5') {
            if (Get-ProxyState) { Stop-Proxy } else { $null = Start-Proxy -UsePort $UsePort }
        }
        elseif ($choice -eq '6')                 { Invoke-Probe -UsePort $UsePort }
        elseif ($choice -eq '7') {
            $row = Select-AppRow -Rows $rows -Prompt '  which entry to forget'
            if ($row) {
                if ($row.Config) { Remove-App -Cfg $cfg -Path $row.Path }
                else { Line Info ($row.Name + ' is not in your list yet - nothing to forget (mode changes are what get saved)') }
            }
        }
        else { Line WARN ('unknown choice: ' + $choice) }
        $cfg = Get-Config
        Write-Host ''
    }
}
# ================================================================ main ======
Write-Host ''
Write-Host '=====================================================================' -ForegroundColor Cyan
Write-Host ' app-chooser.ps1  -  what is inside the VPN, what is outside, and how to switch' -ForegroundColor Cyan
Write-Host ' nothing installed, no admin needed, everything here is reversible' -ForegroundColor Cyan
Write-Host '=====================================================================' -ForegroundColor Cyan

$cfg = Get-Config
$UsePort = $Port
if ($UsePort -le 0) { $UsePort = [int]$cfg.ProxyPort }
if ($UsePort -le 0) { $UsePort = $DefaultPort }

$phys = Get-Physical -Alias $Underlay
$tIdx = -1
if ($phys) { $tIdx = $phys.IfIndex }
$tunnel = Get-Tunnel -UnderlayIfIndex $tIdx
Line Info ('machine : ' + $env:COMPUTERNAME + '   user ' + $env:USERNAME)
if ($phys)   { Line OK ('real    : ' + $phys.Alias + ' ' + $phys.Ip + ' via ' + $phys.Gateway) }
else         { Line WARN 'no physical adapter with a default gateway - pass -Underlay "<alias>"' }
if ($tunnel) { Line OK ('tunnel  : ' + $tunnel.Alias + ' ' + $tunnel.Ip + '   (everything is inside it by default)') }
else         { Line WARN 'X-VPN is not connected - with no tunnel, everything is already "out"' }
Line Info ('config  : ' + (Split-Path -Leaf $cfgPath) + '   (' + @($cfg.Apps).Count + ' app(s) remembered)')

$script:ProcPaths = Get-ProcPaths
$script:SeenPids = @()
$live = Get-LiveMap -Phys $phys -Tunnel $tunnel -ProxyPort $UsePort
$rows = @(Get-Rows -Cfg $cfg -Live $live)

$didWork = $false
if ($Probe)       { $didWork = $true; Invoke-Probe -UsePort $UsePort }
if ($StartProxy)  { $didWork = $true; $null = Start-Proxy -UsePort $UsePort }
if ($StopProxy)   { $didWork = $true; Stop-Proxy -UsePort $UsePort }
if ($ProxyStatus) { $didWork = $true; Show-ProxyStatus }
if ($List)        { $didWork = $true; $null = Show-Apps -Cfg $cfg -Phys $phys -Tunnel $tunnel -Live $live }

if ($Add) {
    $didWork = $true
    $row = Find-AppRow -Rows $rows -Needle $Add
    if (-not $row -and (Test-Path $Add)) {
        $rp = (Resolve-Path $Add).Path
        $row = [pscustomobject]@{ Name = [System.IO.Path]::GetFileNameWithoutExtension($rp); Path = $rp; Mode = 'Vpn'; Config = $false; UserDataDir = '' }
    }
    if (-not $row) { Line FAIL ('nothing matches "' + $Add + '" - run -List, or pass the full exe path') }
    else           { Set-AppMode -Cfg $cfg -Path $row.Path -Name $row.Name -NewMode $Mode -DataDir $UserDataDir }
}

if ($Set) {
    $didWork = $true
    $row = Find-AppRow -Rows $rows -Needle $Set
    if (-not $row) { Line FAIL ('nothing matches "' + $Set + '" - run -List first') }
    else           { Set-AppMode -Cfg $cfg -Path $row.Path -Name $row.Name -NewMode $Mode -DataDir $UserDataDir }
}

if ($Send) {
    # Save the mode and apply it immediately (a running browser is restarted on the new side)
    $didWork = $true
    $row = Find-AppRow -Rows $rows -Needle $Send
    if (-not $row) { Line FAIL ('nothing matches "' + $Send + '" - run -List first') }
    else {
        $dd = if ($UserDataDir) { $UserDataDir } else { [string]$row.UserDataDir }
        Set-AppMode -Cfg $cfg -Path $row.Path -Name $row.Name -NewMode $Mode -DataDir $dd
        $row.Mode = $Mode; $row.UserDataDir = $dd
        Start-AppInMode -Row $row -UsePort $UsePort
    }
}

if ($Remove) {
    $didWork = $true
    $row = Find-AppRow -Rows $rows -Needle $Remove
    if (-not $row -or -not $row.Config) { Line WARN ('"' + $Remove + '" is not in your saved list') }
    else                                { Remove-App -Cfg $cfg -Path $row.Path }
}

if ($Launch) {
    $didWork = $true
    $row = Find-AppRow -Rows $rows -Needle $Launch
    if (-not $row) { Line FAIL ('nothing matches "' + $Launch + '" - run -List first') }
    else           { Start-AppInMode -Row $row -UsePort $UsePort }
}

if (-not $didWork) { Invoke-Menu -UsePort $UsePort }

Write-Host ''
Write-Host ('summary: ' + $script:nOk + ' ok, ' + $script:nWarn + ' warnings, ' + $script:nBad + ' failures') -ForegroundColor Cyan
Write-Host ('files written by this script: ' + (Split-Path -Leaf $cfgPath) + ', ' + (Split-Path -Leaf $pidPath) + ', ' + (Split-Path -Leaf $logPath) + ' - nothing else') -ForegroundColor DarkGray
Write-Host ''






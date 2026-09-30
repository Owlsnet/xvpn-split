<#
.SYNOPSIS
    Destination-based split tunneling for X-VPN and other full-tunnel VPNs on Windows.

.DESCRIPTION
    A full-tunnel VPN on Windows typically captures all IPv4 traffic by installing
    several broad routes on its tunnel interface (for example 0.0.0.0/1 and
    128.0.0.0/1) that together cover the whole address space. X-VPN is a Windows
    VPN plugin app, so its tunnel is enforced this way, through the route table.

    Windows selects routes by longest-prefix match, so a more specific route out
    the physical adapter takes precedence over those broad routes. This script
    adds such exception routes (/32 for a single address, or any CIDR prefix) for
    the destinations given with -Exclude or -DomainFile. All other traffic keeps
    using the VPN.

    Changes are limited and reversible:
    - Only IPv4 route entries are changed. They are added with
      -PolicyStore ActiveStore, so they are not persistent and a reboot removes
      them.
    - No firewall rules, DNS settings, registry keys, services, drivers or VPN
      files are modified.
    - Before the first change, the IPv4 route table is saved to
      routes-before.json. Every route that is added is recorded (prefix,
      interface, gateway, metric) in split-tunnel.state.json. Both files are
      written next to the script.
    - -Undo removes only routes that match an entry in the state file, including
      its interface, gateway and metric. A route that existed before the script
      ran is never removed.
    - -DryRun shows the plan without changing anything.

    Excluded traffic leaves through the physical adapter with the machine's real
    public IP address, which is visible to the local network and to the
    destination. That is the purpose of split tunneling, and also its main risk.

    Exceptions are per destination (IPv4 address, CIDR prefix or hostname), not
    per process; per-process splitting would require a WFP callout driver.
    Hostnames are resolved when the script starts and, with -KeepAlive, again
    every -RefreshSeconds.

    If the VPN blocks traffic outside the tunnel (a kill switch, typically a
    firewall filter on the physical adapter), destination exceptions cannot work
    while that protection is enabled. -Probe detects this with a temporary
    canary route.

    Without a mode switch the script prints a read-only report: network
    topology, which interface wins the route lookup, the current public IP and
    what the script has installed. -Probe, -Apply and -Undo require an elevated
    shell; the report and -Status do not.

.PARAMETER Exclude
    Comma-separated list of destinations to route outside the VPN. Each entry is
    an IPv4 address, an IPv4 CIDR prefix or a hostname. Used with -Apply.

.PARAMETER DomainFile
    Path to a text file with further destinations, one per line, in the same
    formats as -Exclude. Text after '#' is ignored. Used with -Apply.

.PARAMETER ProbeIp
    IPv4 address used by -Probe as the canary destination. It must serve
    /cdn-cgi/trace over HTTPS on port 443 so the public IP can be read. If a
    comma-separated list is given, the first valid address is used. Defaults to
    1.1.1.1.

.PARAMETER Probe
    Adds a temporary /32 route for -ProbeIp out the physical adapter, compares
    the public IP before and after, and reports whether traffic outside the
    tunnel is delivered or blocked. The route is always removed afterwards.
    Requires elevation.

.PARAMETER Apply
    Installs exception routes for the destinations from -Exclude and
    -DomainFile, then shows which interface wins the route lookup for each one.
    Requires elevation.

.PARAMETER Undo
    Removes every exception route recorded in the state file and deletes the
    state file. Requires elevation.

.PARAMETER Status
    Lists the entries in the state file and whether each route is currently
    installed. Read-only.

.PARAMETER Prune
    With -Apply, and on every -KeepAlive cycle, removes routes for hostname
    entries that no longer resolve to the address they were installed for.

.PARAMETER DryRun
    With -Apply, shows which routes would be added without changing anything.

.PARAMETER KeepAlive
    With -Apply, keeps running after installation. Every -RefreshSeconds,
    hostnames are resolved again and missing exception routes are re-added.
    Ctrl+C stops the loop and leaves the exceptions installed; use -Undo to
    remove them.

.PARAMETER NoDoh
    Resolves hostnames with the system resolver only. By default hostnames are
    resolved over DNS-over-HTTPS (Cloudflare, then Google), with the system
    resolver as a fallback.

.PARAMETER Underlay
    Interface alias of the physical adapter that exceptions are routed through.
    By default the first connected adapter with an IPv4 default gateway that is
    not a VPN or virtual adapter is used.

.PARAMETER Tunnel
    Interface alias of the VPN tunnel. The tunnel is normally identified from
    the route lookup for a public address; this alias is used as a fallback.
    Defaults to 'X-VPN'.

.PARAMETER RouteMetric
    Route metric for the exception routes. Defaults to 1.

.PARAMETER RefreshSeconds
    Interval of the -KeepAlive loop in seconds. Values below 15 are treated as
    15. Defaults to 300.

.PARAMETER RunSeconds
    With -Apply, runs the keep-alive loop for this many seconds and then exits.
    A value greater than 0 starts the loop even without -KeepAlive. Defaults to
    0 (no time limit).

.PARAMETER Log
    Path of a transcript file that receives all output. Useful in an elevated
    window that closes as soon as the script ends.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\split-tunnel.ps1

    Prints the read-only report and changes nothing.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\split-tunnel.ps1 -Probe

    From an elevated shell, checks whether traffic outside the tunnel is
    delivered or blocked. The canary route is removed again afterwards.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\split-tunnel.ps1 -Exclude 1.1.1.1,192.0.2.0/24 -Apply -DryRun

    Shows the routes that would be added for one address and one /24 prefix.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\split-tunnel.ps1 -Exclude example.com -Apply -KeepAlive

    Routes the addresses of example.com outside the VPN and keeps the
    exceptions current until Ctrl+C is pressed.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\split-tunnel.ps1 -Undo

    From an elevated shell, removes every route the script added.

.NOTES
    Targets Windows PowerShell 5.1 and the built-in NetTCPIP cmdlets
    (Get-NetRoute, New-NetRoute, Find-NetRoute). Only IPv4 routes are added, and
    hostnames are resolved to A records only.

    routes-before.json is written once, as a reference; it is never used to
    restore the table. -Undo relies solely on split-tunnel.state.json. -Probe
    records its canary route in the state file while it exists, so -Undo can
    still remove it if a probe is interrupted.
#>
[CmdletBinding()]
param(
    [string] $Exclude        = '',
    [string] $DomainFile     = '',
    [string] $ProbeIp        = '1.1.1.1',
    [switch] $Probe,
    [switch] $Apply,
    [switch] $Undo,
    [switch] $Status,
    [switch] $Prune,
    [switch] $DryRun,
    [switch] $KeepAlive,
    [switch] $NoDoh,
    [string] $Underlay       = '',
    [string] $Tunnel         = 'X-VPN',
    [int]    $RouteMetric    = 1,
    [int]    $RefreshSeconds = 300,
    [int]    $RunSeconds     = 0,
    [string] $Log            = ''
)

$ErrorActionPreference = 'Continue'
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$statePath = Join-Path $scriptDir 'split-tunnel.state.json'
$snapPath  = Join-Path $scriptDir 'routes-before.json'

$script:nOk = 0; $script:nWarn = 0; $script:nBad = 0
function Line {
    param([string]$State, [string]$Text)
    if ($State -eq 'OK')       { Write-Host ('  [ OK ]  ' + $Text) -ForegroundColor Green;     $script:nOk++ }
    elseif ($State -eq 'WARN') { Write-Host ('  [WARN]  ' + $Text) -ForegroundColor Yellow;    $script:nWarn++ }
    elseif ($State -eq 'FAIL') { Write-Host ('  [FAIL]  ' + $Text) -ForegroundColor Red;       $script:nBad++ }
    else                       { Write-Host ('          ' + $Text) -ForegroundColor Gray }
}

function Get-Elevated {
    return ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-NetworkPrefix {
    param([string]$Ip, [int]$PrefixLength)
    $b = [System.Net.IPAddress]::Parse($Ip).GetAddressBytes()
    $net = New-Object 'byte[]' 4
    for ($i = 0; $i -lt 4; $i++) {
        $left = $PrefixLength - ($i * 8)
        if ($left -ge 8)     { $m = 255 }
        elseif ($left -le 0) { $m = 0 }
        else                 { $m = 256 - [int][math]::Pow(2, 8 - $left) }
        $net[$i] = [byte]($b[$i] -band $m)
    }
    return ('{0}/{1}' -f (New-Object System.Net.IPAddress -ArgumentList (, $net)).ToString(), $PrefixLength)
}

function Get-Underlay {
    param([string]$Alias = '')
    # Exception routes are installed against this adapter and its gateway, so a wrong choice
    # sends the traffic nowhere. Virtual adapters (ZeroTier One, Tailscale, Hyper-V, VMware,
    # ...) commonly install a default route of their own without providing internet access,
    # so real hardware is preferred and the name filter is only a fallback.
    $ExcludeAlias = 'VPN|Loopback|Host-Only|Virtual|TAP|Kernel Debug|Wi-Fi Direct|WAN Miniport|6to4|Teredo|IP-HTTPS'
    $list = @()
    foreach ($cfg in @(Get-NetIPConfiguration -ErrorAction SilentlyContinue)) {
        if ($null -eq $cfg.IPv4Address -or $null -eq $cfg.IPv4DefaultGateway) { continue }
        if ($cfg.NetAdapter.Status -ne 'Up') { continue }
        if (-not $Alias -and $cfg.InterfaceAlias -match $ExcludeAlias) { continue }
        if ($Alias -and $cfg.InterfaceAlias -ne $Alias) { continue }
        $def = @(Get-NetRoute -InterfaceIndex $cfg.InterfaceIndex -AddressFamily IPv4 -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue)
        $na = $cfg.NetAdapter
        $hw = $true
        if ($na.PSObject.Properties['HardwareInterface']) { $hw = ($na.HardwareInterface -eq $true) }
        elseif ($na.PSObject.Properties['Virtual'])       { $hw = ($na.Virtual -ne $true) }
        $metric = [int]::MaxValue
        if ($def.Count -gt 0) { $metric = [int](@($def | Sort-Object RouteMetric)[0].RouteMetric) }
        $list += [pscustomobject]@{ Cfg = $cfg; Metric = $metric; Hardware = $hw }
    }
    $pick = @($list | Sort-Object @{ Expression = { -not $_.Hardware } }, Metric, @{ Expression = { [int]$_.Cfg.InterfaceIndex } }) | Select-Object -First 1
    if (-not $pick) { return $null }
    $cfg  = $pick.Cfg
    $ip   = $cfg.IPv4Address[0].IPAddress
    $plen = [int]$cfg.IPv4Address[0].PrefixLength
    $ipi  = Get-NetIPInterface -InterfaceIndex $cfg.InterfaceIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue
    [pscustomobject]@{
        Alias    = $cfg.InterfaceAlias
        IfIndex  = [int]$cfg.InterfaceIndex
        Ip       = $ip
        Prefix   = $plen
        Cidr     = (Get-NetworkPrefix -Ip $ip -PrefixLength $plen)
        Gateway  = $cfg.IPv4DefaultGateway[0].NextHop
        Metric   = [int]$ipi.InterfaceMetric
        Hardware = $pick.Hardware
    }
}
function Get-TunnelInfo {
    param([string]$Alias = 'X-VPN')
    # Do not infer the tunnel from the shape of its route set: the prefixes a VPN
    # installs to cover IPv4 differ between connections (a few /1-/3 routes, or many
    # smaller fragments). Ask the OS which interface wins the lookup for a public
    # address instead.
    $underlay = Get-Underlay -Alias $Underlay
    $underlayIdx = -1
    if ($underlay) { $underlayIdx = $underlay.IfIndex }
    $idx = 0
    $fr = @(Find-NetRoute -RemoteIPAddress '1.1.1.1' -ErrorAction SilentlyContinue)
    if ($fr.Count -ge 2) {
        $ri = [int]$fr[$fr.Count - 1].InterfaceIndex
        if ($ri -ne 0 -and $ri -ne $underlayIdx) { $idx = $ri }
    }
    if ($idx -eq 0) {
        $c = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object {
            $_.InterfaceIndex -ne 1 -and $_.InterfaceIndex -ne $underlayIdx -and
            ($_.InterfaceAlias -eq $Alias -or $_.InterfaceAlias -match 'VPN|TUN|WIREGUARD|WG')
        } | Select-Object -First 1
        if ($c) { $idx = [int]$c.InterfaceIndex }
    }
    if ($idx -eq 0) { return $null }
    $ipi = Get-NetIPInterface -InterfaceIndex $idx -AddressFamily IPv4 -ErrorAction SilentlyContinue
    $ip  = Get-NetIPAddress -InterfaceIndex $idx -AddressFamily IPv4 -ErrorAction SilentlyContinue | Select-Object -First 1
    $cov = @(Get-NetRoute -InterfaceIndex $idx -AddressFamily IPv4 -ErrorAction SilentlyContinue |
             Where-Object { $_.DestinationPrefix -notmatch '/32$' -and $_.NextHop -eq '0.0.0.0' } |
             Select-Object -ExpandProperty DestinationPrefix -Unique)
    $short = @($cov | Sort-Object { [int](($_ -split '/')[1]) } | Select-Object -First 6)
    [pscustomobject]@{
        IfIndex        = $idx
        Alias          = $ipi.InterfaceAlias
        Ip             = $ip.IPAddress
        Mtu            = $ipi.NlMtu
        Metric         = [int]$ipi.InterfaceMetric
        HalfRoutes     = $short
        CoveredCount   = $cov.Count
        RouteCount     = @(Get-NetRoute -InterfaceIndex $idx -AddressFamily IPv4 -ErrorAction SilentlyContinue).Count
        VisibleAsAdapter = [bool](@(Get-NetAdapter -IncludeHidden -ErrorAction SilentlyContinue | Where-Object { $_.ifIndex -eq $idx }).Count)
    }
}

function Get-ChosenRoute {
    param([string]$RemoteIp)
    try {
        $fr = @(Find-NetRoute -RemoteIPAddress $RemoteIp -ErrorAction Stop)
        if ($fr.Count -lt 2) { return $null }
        $src = $fr[0]; $rt = $fr[$fr.Count - 1]
        [pscustomobject]@{
            SourceIp = $src.IPAddress
            Amount   = $fr.Count
            Dest     = $rt.DestinationPrefix
            NextHop  = $rt.NextHop
            IfIndex  = [int]$rt.InterfaceIndex
            IfAlias  = $rt.InterfaceAlias
            Metric   = [int]$rt.RouteMetric + [int]$rt.InterfaceMetric
        }
    } catch { return $null }
}

function Get-Egress {
    param([string]$Ip = '1.1.1.1', [int]$TimeoutMs = 12000)
    # Use a new socket on every call. Invoke-RestMethod in Windows PowerShell 5.1 reuses
    # pooled connections, so a request could keep using an existing connection through
    # the tunnel after a route change. A new connection forces a fresh route lookup.
    $client = $null; $ssl = $null; $local = ''
    try {
        $client = New-Object System.Net.Sockets.TcpClient
        $ar = $client.BeginConnect($Ip, 443, $null, $null)
        if (-not $ar.AsyncWaitHandle.WaitOne($TimeoutMs)) { return [pscustomobject]@{ Ip = ''; LocalAddress = ''; Error = 'connect timed out' } }
        $client.EndConnect($ar)
        $local = $client.Client.LocalEndPoint.Address.ToString()
        $cb  = [System.Net.Security.RemoteCertificateValidationCallback]{ param($s, $c, $ch, $e) return $true }
        $ssl = New-Object System.Net.Security.SslStream($client.GetStream(), $false, $cb)
        $ssl.AuthenticateAsClient($Ip)
        $req = 'GET /cdn-cgi/trace HTTP/1.1' + "`r`n" + 'Host: ' + $Ip + "`r`n" + 'Connection: close' + "`r`n" + 'User-Agent: split-tunnel.ps1' + "`r`n" + 'Accept: */*' + "`r`n`r`n"
        $b = [Text.Encoding]::ASCII.GetBytes($req)
        $ssl.Write($b, 0, $b.Length); $ssl.Flush()
        $body = (New-Object System.IO.StreamReader($ssl)).ReadToEnd()
        $m = [regex]::Match($body, '(?m)^ip=(\S+)')
        if ($m.Success) { return [pscustomobject]@{ Ip = $m.Groups[1].Value; LocalAddress = $local; Error = '' } }
        return [pscustomobject]@{ Ip = ''; LocalAddress = $local; Error = 'no ip= line in the trace' }
    } catch {
        return [pscustomobject]@{ Ip = ''; LocalAddress = $local; Error = $_.Exception.Message }
    } finally {
        if ($ssl) { $ssl.Dispose() }
        if ($client) { $client.Close() }
    }
}

function Resolve-Ipv4 {
    param([string]$Name, [switch]$ForceSystem)
    if (-not $ForceSystem -and -not $NoDoh) {
        $urls = @(
            ('https://1.1.1.1/dns-query?name={0}&type=A&ct=application/dns-json' -f $Name),
            ('https://dns.google/resolve?name={0}&type=A' -f $Name)
        )
        foreach ($u in $urls) {
            try {
                $j = Invoke-RestMethod -Uri $u -TimeoutSec 12 -Headers @{ accept = 'application/dns-json' } -UseBasicParsing
                $ips = @($j.Answer | Where-Object { $_.type -eq 1 } | ForEach-Object { $_.data } |
                         Where-Object { $_ -match '^\d{1,3}(\.\d{1,3}){3}$' } | Select-Object -Unique)
                if ($ips.Count -gt 0) { return $ips }
            } catch { }
        }
    }
    try {
        return @(Resolve-DnsName -Name $Name -Type A -ErrorAction Stop |
                 Where-Object { $_.IPAddress -match '^\d{1,3}(\.\d{1,3}){3}$' } |
                 ForEach-Object { $_.IPAddress } | Select-Object -Unique)
    } catch { return @() }
}

function Get-PrefixList {
    param([string]$Csv = '', [string]$File = '')
    $items = @()
    if ($Csv.Trim().Length -gt 0) { $items += @($Csv.Split(',') | ForEach-Object { $_.Trim() } | Where-Object { $_ }) }
    if ($File.Trim().Length -gt 0 -and (Test-Path $File)) {
        $items += @(Get-Content $File | ForEach-Object { ($_ -split '#')[0].Trim() } | Where-Object { $_ })
    }
    $out = @()
    foreach ($t in $items) {
        if ($t -match '^(\d{1,3}(\.\d{1,3}){3})(/(\d{1,2}))?$') {
            $plen = if ($Matches[4]) { [int]$Matches[4] } else { 32 }
            $out += [pscustomobject]@{ Prefix = (Get-NetworkPrefix -Ip $Matches[1] -PrefixLength $plen); Source = 'literal:' + $t }
        } else {
            $ips = @(Resolve-Ipv4 -Name $t)
            if ($ips.Count -eq 0) {
                Line WARN ('cannot resolve ' + $t + ' - skipped')
            } else {
                foreach ($ip in $ips) {
                    $out += [pscustomobject]@{ Prefix = (Get-NetworkPrefix -Ip $ip -PrefixLength 32); Source = ('host:' + $t) }
                }
            }
        }
    }
    return @($out | Sort-Object Prefix -Unique)
}
function Get-SplitState {
    if (-not (Test-Path $statePath)) { return $null }
    try { return (Get-Content $statePath -Raw | ConvertFrom-Json) } catch { return $null }
}

function Save-SplitState {
    param($Underlay, $Tunnel, [object[]]$Entries)
    $obj = [pscustomobject]@{
        UpdatedAt       = (Get-Date).ToString('s')
        Machine         = $env:COMPUTERNAME
        UnderlayAlias   = $Underlay.Alias
        UnderlayIfIndex = $Underlay.IfIndex
        UnderlayIp      = $Underlay.Ip
        Gateway         = $Underlay.Gateway
        TunnelAlias     = $Tunnel.Alias
        RouteMetric     = $RouteMetric
        Prefixes        = @($Entries)
    }
    $obj | ConvertTo-Json -Depth 6 | Set-Content -Path $statePath -Encoding UTF8
}

function Save-RouteSnapshot {
    if (Test-Path $snapPath) { return $false }
    $routes = @(Get-NetRoute -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Select-Object DestinationPrefix, NextHop, InterfaceIndex, InterfaceAlias, RouteMetric, InterfaceMetric, Protocol, PolicyStore)
    $obj = [pscustomobject]@{
        TakenAt = (Get-Date).ToString('s')
        Machine = $env:COMPUTERNAME
        Note    = 'IPv4 route table before split-tunnel.ps1 changed anything. Informational only.'
        Routes  = $routes
    }
    $obj | ConvertTo-Json -Depth 5 | Set-Content -Path $snapPath -Encoding UTF8
    return $true
}

function Find-OwnRoute {
    param([string]$Prefix, $Underlay, [int]$Metric = -1)
    $hits = @(Get-NetRoute -AddressFamily IPv4 -DestinationPrefix $Prefix -ErrorAction SilentlyContinue |
        Where-Object {
            $_.InterfaceIndex -eq $Underlay.IfIndex -and
            ($_.NextHop -eq $Underlay.Gateway -or $_.NextHop -eq '' -or $_.NextHop -eq '0.0.0.0') -and
            ($Metric -lt 0 -or [int]$_.RouteMetric -eq $Metric)
        })
    return $hits
}

function Add-ExceptionRoute {
    param([string]$Prefix, $Underlay)
    $own = @(Find-OwnRoute -Prefix $Prefix -Underlay $Underlay)
    if ($own.Count -gt 0) {
        # A route for this prefix already leads out the physical adapter. Leave it as
        # is: it was either added by an earlier run, or it predates this script, in
        # which case -Undo must not remove it either.
        return [pscustomobject]@{ Status = 'kept'; Preexisting = $true }
    }
    if ($DryRun) { return [pscustomobject]@{ Status = 'dry'; Preexisting = $false } }
    New-NetRoute -AddressFamily IPv4 -DestinationPrefix $Prefix -InterfaceIndex $Underlay.IfIndex `
        -NextHop $Underlay.Gateway -RouteMetric $RouteMetric -PolicyStore ActiveStore -ErrorAction Stop | Out-Null
    return [pscustomobject]@{ Status = 'added'; Preexisting = $false }
}

function Remove-ExceptionRoute {
    param([string]$Prefix, $Underlay, [int]$Metric = 0)
    if ($Metric -le 0) { $Metric = $RouteMetric }
    # Match the metric as well as interface and gateway, so that a route for the same
    # prefix created by something else is not removed.
    $own = @(Find-OwnRoute -Prefix $Prefix -Underlay $Underlay -Metric $Metric)
    if ($DryRun -or $own.Count -eq 0) { return $own.Count }
    foreach ($r in $own) { $r | Remove-NetRoute -Confirm:$false -ErrorAction SilentlyContinue }
    return $own.Count
}

function Add-StateEntry {
    param($Underlay, $Tunnel, $Entry)
    $st = Get-SplitState
    $list = @()
    if ($st) { $list = @(@($st.Prefixes) | Where-Object { $_.Prefix -ne $Entry.Prefix }) }
    $list = @($list) + @($Entry)
    Save-SplitState -Underlay $Underlay -Tunnel $Tunnel -Entries $list
}

function Remove-StateEntry {
    param([string]$Prefix)
    $st = Get-SplitState
    if (-not $st) { return }
    $list = @(@($st.Prefixes) | Where-Object { $_.Prefix -ne $Prefix })
    if ($list.Count -eq 0) { Remove-Item $statePath -ErrorAction SilentlyContinue; return }
    $u = [pscustomobject]@{ Alias = $st.UnderlayAlias; IfIndex = [int]$st.UnderlayIfIndex; Ip = $st.UnderlayIp; Gateway = $st.Gateway }
    Save-SplitState -Underlay $u -Tunnel ([pscustomobject]@{ Alias = $st.TunnelAlias }) -Entries $list
}

function Invoke-Undo {
    Write-Host ''
    Write-Host 'UNDO  -  removing every exception this script added' -ForegroundColor White
    $st = Get-SplitState
    if (-not $st) {
        Line Info ('nothing recorded in ' + (Split-Path -Leaf $statePath) + ' - this script has not added anything')
        return
    }
    $underlay = Get-Underlay -Alias $st.UnderlayAlias
    if (-not $underlay) {
        $underlay = [pscustomobject]@{ Alias = $st.UnderlayAlias; IfIndex = [int]$st.UnderlayIfIndex; Gateway = $st.Gateway; Ip = $st.UnderlayIp }
        Line WARN ('adapter ' + $st.UnderlayAlias + ' is not up right now - matching on the recorded interface/gateway anyway')
    }
    $kept = @(); $removed = 0
    foreach ($e in @($st.Prefixes)) {
        if ($e.Preexisting) { $kept += $e; Line Info ($e.Prefix + '  left alone (it existed before this script ran)'); continue }
        $m = [int]$st.RouteMetric
        if ($e.PSObject.Properties['Metric']) { if ([int]$e.Metric -gt 0) { $m = [int]$e.Metric } }
        $n = Remove-ExceptionRoute -Prefix $e.Prefix -Underlay $underlay -Metric $m
        if ($n -gt 0) { $removed += $n; Line OK ($(if ($DryRun) { 'would remove ' } else { 'removed ' }) + $e.Prefix + '  (was ' + $e.Source + ')') }
        else {
            Line Info ('not in the table - nothing to remove: ' + $e.Prefix)
            Line Info ('  to confirm: Get-NetRoute -DestinationPrefix ' + $e.Prefix)
        }
    }
    # a dry run only counts, so the state file must keep tracking the routes
    if ($DryRun) {
        Write-Host ''
        Line OK ('dry run - ' + $removed + ' route(s) would be removed; nothing was changed')
        return
    }
    if ($kept.Count -gt 0) { Save-SplitState -Underlay $underlay -Tunnel ([pscustomobject]@{ Alias = $st.TunnelAlias }) -Entries $kept }
    else { Remove-Item $statePath -ErrorAction SilentlyContinue }
    Write-Host ''
    Line OK ('undo done - ' + $removed + ' route(s) removed, state cleared')
    Line Info ('the read-only snapshot ' + (Split-Path -Leaf $snapPath) + ' is kept for reference and can be deleted any time')
    Line Info 'route entries were non-persistent (ActiveStore), so a reboot would have cleared them too'
}

function Show-State {
    Write-Host ''
    Write-Host 'STATUS  -  what this script currently has installed' -ForegroundColor White
    $st = Get-SplitState
    if (-not $st) { Line Info 'no state file - nothing installed by this script'; return }
    Line Info ('state written ' + $st.UpdatedAt + '   underlay ' + $st.UnderlayAlias + ' (' + $st.UnderlayIp + ' via ' + $st.Gateway + ')   tunnel ' + $st.TunnelAlias)
    $underlay = Get-Underlay -Alias $st.UnderlayAlias
    if (-not $underlay) {
        $underlay = [pscustomobject]@{ Alias = $st.UnderlayAlias; IfIndex = [int]$st.UnderlayIfIndex; Gateway = $st.Gateway; Ip = $st.UnderlayIp }
    }
    foreach ($e in @($st.Prefixes)) {
        $live = @(Find-OwnRoute -Prefix $e.Prefix -Underlay $underlay -Metric ([int]$st.RouteMetric))
        $state = if ($live.Count -gt 0) { 'installed' } elseif ($e.Preexisting) { 'was already there' } else { 'MISSING (re-apply or -Undo)' }
        Line Info ($e.Prefix.PadRight(20) + ' ' + $state.PadRight(24) + ' ' + $e.Source)
    }
}
function Test-TcpPort {
    param([string]$Ip, [int]$Port = 443, [int]$TimeoutMs = 6000)
    $client = $null
    try {
        $client = New-Object System.Net.Sockets.TcpClient
        $ar = $client.BeginConnect($Ip, $Port, $null, $null)
        if ($ar.AsyncWaitHandle.WaitOne($TimeoutMs)) { $client.EndConnect($ar); return $true }
        return $false
    } catch { return $false }
    finally { if ($client) { $client.Close() } }
}

function Invoke-Probe {
    Write-Host ''
    Write-Host 'PROBE  -  does traffic really escape the tunnel, or is it blocked?' -ForegroundColor White
    if (-not (Get-Elevated)) {
        Line FAIL 'needs an elevated shell (adding a route requires admin)'
        Line Info ('run:  Start-Process powershell -Verb RunAs -ArgumentList ''-ExecutionPolicy Bypass -File "' + $PSCommandPath + '" -Probe''')
        return
    }
    $underlay = Get-Underlay -Alias $Underlay
    if (-not $underlay) { Line FAIL 'no physical adapter with a default gateway found - pass -Underlay "<alias>"'; return }
    $tunnel = Get-TunnelInfo -Alias $Tunnel
    if (-not $tunnel) {
        Line FAIL 'X-VPN is not connected: no 0.0.0.0/1-style tunnel routes exist, so there is nothing to escape'
        Line Info 'connect X-VPN first, then run:  .\split-tunnel.ps1 -Probe'
        return
    }
    Line OK ('underlay : ' + $underlay.Alias + '  ' + $underlay.Ip + '  gateway ' + $underlay.Gateway + '  if ' + $underlay.IfIndex)
    Line OK ('tunnel   : ' + $tunnel.Alias + '  ' + $tunnel.Ip + '  if ' + $tunnel.IfIndex + '  (' + $tunnel.RouteCount + ' routes)')

    $ips = @($ProbeIp.Split(',') | ForEach-Object { $_.Trim() } | Where-Object { $_ -match '^\d{1,3}(\.\d{1,3}){3}$' })
    if ($ips.Count -eq 0) { Line FAIL 'no usable IP in -ProbeIp'; return }
    $ip = $ips[0]

    Write-Host ''
    Line Info ('step 1 - egress while everything still goes through the VPN (https://' + $ip + '/cdn-cgi/trace)')
    $before = Get-Egress -Ip $ip
    if ($before.Ip) { Line OK ('  through the VPN you appear as ' + $before.Ip + '   (socket source ' + $before.LocalAddress + ')') }
    else            { Line WARN ('  no answer through the VPN: ' + $before.Error + ' - carrying on') }

    $added = $false
    try {
        Line Info ('step 2 - temporarily routing ' + $ip + '/32 out ' + $underlay.Alias + ' (removed again at the end, always)')
        $res = Add-ExceptionRoute -Prefix ($ip + '/32') -Underlay $underlay
        $added = ($res.Status -eq 'added' -or $res.Status -eq 'kept')
        if ($res.Status -eq 'added')      { Line OK ('  route added: ' + $ip + '/32 -> ' + $underlay.Gateway + ' on if ' + $underlay.IfIndex + ' metric ' + $RouteMetric) }
        elseif ($res.Status -eq 'kept')   { Line OK ('  a ' + $ip + '/32 route was already in place (leftover from before?) - it will be removed at the end as well') }
        else                              { Line WARN ('  route not added (' + $res.Status + ')') }
        if ($added -and -not $DryRun) {
            # Record the canary so -Undo can still remove it if this process is killed
            # before the cleanup below runs.
            Add-StateEntry -Underlay $underlay -Tunnel $tunnel -Entry ([pscustomobject]@{
                Prefix = ($ip + '/32'); Source = 'probe:canary'; Preexisting = $false; Metric = $RouteMetric })
        }

        $chosen = Get-ChosenRoute -RemoteIp $ip
        if ($chosen) {
            Line Info ('step 3 - what the OS now picks for ' + $ip)
            Line Info ('  source ' + $chosen.SourceIp + '   via ' + $chosen.IfAlias + '   nexthop ' + $chosen.NextHop + '   metric ' + $chosen.Metric)
        }
        Line Info 'step 4 - raw TCP 443 handshake out that path'
        $tcpOk = Test-TcpPort -Ip $ip
        if ($tcpOk) { Line OK '  tcp 443 handshake OK' } else { Line WARN '  tcp 443 handshake did not complete' }
        Line Info 'step 5 - egress again, now with the exception in place (fresh socket)'
        $after = Get-Egress -Ip $ip
        if ($after.Ip) { Line OK ('  now you appear as ' + $after.Ip + '   (socket source ' + $after.LocalAddress + ')') }
        else           { Line WARN ('  no answer: ' + $after.Error) }

        Write-Host ''
        if ($after.Ip -and $before.Ip -and $after.Ip -ne $before.Ip) {
            Line OK ('SPLIT TUNNELING WORKS - that traffic skipped the VPN: you appear as ' + $after.Ip + ' instead of ' + $before.Ip)
            Line Info ('the socket left from ' + $after.LocalAddress + ' (' + $underlay.Alias + ') instead of ' + $before.LocalAddress + ' (the tunnel)')
            Line Info 'no leak-protection filter is blocking the physical path, so -Apply will work'
        } elseif ($after.Ip -and -not $before.Ip) {
            Line OK ('traffic answered as ' + $after.Ip + ' - exception in place; no VPN baseline to compare')
        } elseif ($after.Ip -and $before.Ip -and $after.Ip -eq $before.Ip) {
            Line FAIL ('NO EFFECT - still leaving through the VPN as ' + $after.Ip + '; the route did not win')
            Line Info ('socket source was ' + $after.LocalAddress + ' - compare with the underlay ' + $underlay.Ip)
            Line Info 'check: is X-VPN re-adding its own route for this prefix? does -Underlay name the right adapter?'
        } elseif (-not $after.Ip -and -not $tcpOk) {
            Line FAIL 'BLOCKED - no answer and no TCP handshake once the traffic left the tunnel'
            Line Info 'the VPN blocks traffic outside the tunnel (a "kill switch", e.g. a firewall filter on the physical adapter).'
            Line Info 'destination exceptions cannot work while that protection is enabled.'
        } else {
            Line WARN 'inconclusive - handshake partly worked but the HTTPS trace failed; try another -ProbeIp (e.g. 8.8.8.8 or 9.9.9.9)'
        }
    } finally {
        # The canary is removed even if a step above failed, so a probe never leaves
        # a route behind.
        if ($added) {
            $null = Remove-ExceptionRoute -Prefix ($ip + '/32') -Underlay $underlay -Metric $RouteMetric
            if (-not $DryRun) { Remove-StateEntry -Prefix ($ip + '/32') }
            $back = Get-ChosenRoute -RemoteIp $ip
            $via = if ($back) { $back.IfAlias } else { 'unknown' }
            Write-Host ''
            Line OK ('cleanup - the ' + $ip + '/32 exception was removed; ' + $ip + ' now leaves via ' + $via + ' again')
            Line Info ('if a probe is ever interrupted, the canary is recorded, so .\split-tunnel.ps1 -Undo still clears it')
        } else {
            Line Info 'cleanup - nothing had to be removed'
        }
    }
}
function Sync-Exceptions {
    param($Underlay, [switch]$Quiet)
    $prefixes = Get-PrefixList -Csv $Exclude -File $DomainFile
    $changed = 0
    $entries = @()
    # A route counts as owned by this script only if the script added it. A route that
    # was already in the table when first seen is marked Preexisting and never removed.
    $prevMap = @{}
    $prev = Get-SplitState
    if ($prev) { foreach ($e in @($prev.Prefixes)) { $prevMap[$e.Prefix] = [bool]$e.Preexisting } }
    foreach ($p in $prefixes) {
        try { $r = Add-ExceptionRoute -Prefix $p.Prefix -Underlay $Underlay }
        catch { if (-not $Quiet) { Line FAIL ($p.Prefix + '  ' + $_.Exception.Message) }; continue }
        $already = [bool]$r.Preexisting
        $ours = $false
        if ($already) {
            if ($prevMap.ContainsKey($p.Prefix)) { $ours = -not $prevMap[$p.Prefix] }
            else                                 { $ours = $false }
        } else {
            $ours = $true
        }
        $pre = -not $ours
        if ($r.Status -eq 'added') {
            $changed++
            if (-not $Quiet) { Line OK ('added    ' + $p.Prefix.PadRight(20) + ' via ' + $Underlay.Alias + '   ' + $p.Source) }
        } elseif (-not $Quiet) {
            if ($pre) { Line Info ('present  ' + $p.Prefix.PadRight(20) + ' already routed out ' + $Underlay.Alias + ' before this script ran - left alone') }
            else      { Line Info ('present  ' + $p.Prefix.PadRight(20) + ' via ' + $Underlay.Alias + '   ' + $p.Source) }
        }
        $metricUsed = $RouteMetric
        if ($ours) {
            $live = @(Find-OwnRoute -Prefix $p.Prefix -Underlay $Underlay)
            if ($live.Count -gt 0) { $metricUsed = [int]$live[0].RouteMetric }
        }
        $entries += [pscustomobject]@{ Prefix = $p.Prefix; Source = $p.Source; Preexisting = $pre; Metric = $metricUsed }
    }
    return [pscustomobject]@{ Entries = @($entries); Changed = $changed; Count = @($prefixes).Count }
}

function Invoke-Apply {
    Write-Host ''
    Write-Host 'APPLY  -  installing split-tunnel exceptions' -ForegroundColor White
    if (-not (Get-Elevated)) {
        Line FAIL 'needs an elevated shell (adding a route requires admin)'
        Line Info ('run:  Start-Process powershell -Verb RunAs -ArgumentList ''-ExecutionPolicy Bypass -File "' + $PSCommandPath + '" -Exclude <list> -Apply''')
        return
    }
    $underlay = Get-Underlay -Alias $Underlay
    if (-not $underlay) { Line FAIL 'no physical adapter with a default gateway found - pass -Underlay "<alias>"'; return }
    $tunnel = Get-TunnelInfo -Alias $Tunnel
    Line OK ('underlay : ' + $underlay.Alias + '  ' + $underlay.Ip + '  gateway ' + $underlay.Gateway + '  if ' + $underlay.IfIndex)
    if ($tunnel) { Line OK ('tunnel   : ' + $tunnel.Alias + '  ' + $tunnel.Ip + '  if ' + $tunnel.IfIndex + '  (' + $tunnel.RouteCount + ' routes)') }
    else         { Line WARN 'no VPN tunnel routes right now - installing anyway (the exceptions are harmless without a VPN)' }
    Line Info ('metric   : ' + $RouteMetric + '  (VPN routes use 256 + the interface metric, so an equal prefix from this script wins)')
    if ($DryRun) { Line WARN 'DRY RUN - nothing on this machine is changed' }

    $preview = @(Get-PrefixList -Csv $Exclude -File $DomainFile)
    if ($preview.Count -eq 0) { Line FAIL 'nothing to install: pass -Exclude <ip|cidr|host,...> and/or -DomainFile <path>'; return }
    Line Info ($preview.Count + ' destination(s) will go out ' + $underlay.Alias + ' instead of the VPN')

    if (-not $DryRun) {
        if (Save-RouteSnapshot) { Line OK ('route table snapshot saved: ' + (Split-Path -Leaf $snapPath) + '  (read-only reference)') }
        else                    { Line Info ('snapshot ' + (Split-Path -Leaf $snapPath) + ' already exists - untouched') }
    }

    Write-Host ''
    $sync = Sync-Exceptions -Underlay $underlay
    $entries = @($sync.Entries)

    if ($Prune -and -not $DryRun) { $null = Remove-StaleHostEntries -Underlay $underlay -Keep $entries }

    if (-not $DryRun) { Save-SplitState -Underlay $underlay -Tunnel $tunnel -Entries $entries }

    Write-Host ''
    Write-Host 'verification  -  who wins now (Find-NetRoute)' -ForegroundColor White
    $winner = 0
    foreach ($p in $entries) {
        $ipOnly = ($p.Prefix -split '/')[0]
        $c = Get-ChosenRoute -RemoteIp $ipOnly
        $via = if ($c) { $c.IfAlias } else { 'unknown' }
        if ($c -and $c.IfIndex -eq $underlay.IfIndex) {
            $winner++
            Line OK ($p.Prefix.PadRight(20) + ' -> ' + $via + '  source ' + $c.SourceIp + '  ' + $p.Source)
        } else {
            Line WARN ($p.Prefix.PadRight(20) + ' -> still ' + $via + '  ' + $p.Source)
        }
    }
    Write-Host ''
    Line Info ($winner + ' of ' + @($entries).Count + ' exception(s) are winning the route lookup')
    Line WARN 'these destinations now LEAVE FROM YOUR REAL IP over the local network, not from the VPN'
    Line Info 'undo any time:  .\split-tunnel.ps1 -Undo      (or just reboot)'
    Line Info 'see what is installed:  .\split-tunnel.ps1 -Status'
}
function Remove-StaleHostEntries {
    param($Underlay, [object[]]$Keep)
    $old = Get-SplitState
    if (-not $old) { return 0 }
    $keepSet = @($Keep | ForEach-Object { $_.Prefix })
    $n = 0
    foreach ($e in @($old.Prefixes)) {
        if ($e.Source -like 'host:*' -and -not $e.Preexisting -and $keepSet -notcontains $e.Prefix) {
            $m = [int]$old.RouteMetric
            if ($e.PSObject.Properties['Metric']) { if ([int]$e.Metric -gt 0) { $m = [int]$e.Metric } }
            $r = Remove-ExceptionRoute -Prefix $e.Prefix -Underlay $Underlay -Metric $m
            if ($r -gt 0) { $n += $r; Line Info ('pruned   ' + $e.Prefix.PadRight(20) + ' ' + $e.Source + ' no longer resolves there') }
        }
    }
    return $n
}

function Invoke-KeepAlive {
    param($Underlay, [int]$Seconds, [int]$Refresh)
    if ($Refresh -lt 15) { $Refresh = 15 }
    $until = if ($Seconds -gt 0) { (Get-Date).AddSeconds($Seconds) } else { $null }
    Write-Host ''
    Write-Host 'KEEP-ALIVE  -  re-asserting exceptions and re-resolving hostnames' -ForegroundColor White
    if ($until) { Line Info ('will stop by itself at ' + $until.ToString('HH:mm:ss') + '   (Ctrl+C stops it earlier)') }
    else        { Line Info 'runs until Ctrl+C' }
    Line Info ('cycle every ' + $Refresh + 's: routes are re-added if X-VPN removed them, hostnames are resolved again')
    try {
        while ($true) {
            if ($until -and (Get-Date) -ge $until) { Line Info 'run time reached - stopping the keep-alive loop'; break }
            Start-Sleep -Seconds $Refresh
            if ($until -and (Get-Date) -ge $until) { Line Info 'run time reached - stopping the keep-alive loop'; break }
            $sync = Sync-Exceptions -Underlay $Underlay -Quiet
            $entries = @($sync.Entries)
            if ($Prune) { $null = Remove-StaleHostEntries -Underlay $Underlay -Keep $entries }
            $tunnel = Get-TunnelInfo -Alias $Tunnel
            Save-SplitState -Underlay $Underlay -Tunnel $tunnel -Entries $entries
            Write-Host ('  ' + (Get-Date).ToString('HH:mm:ss') + '  ' + $entries.Count + ' exception(s) enforced, ' + $sync.Changed + ' (re)added') -ForegroundColor DarkGray
        }
    } finally {
        Write-Host ''
        Line Info 'keep-alive stopped - the exceptions stay installed'
        Line Info 'remove them with:  .\split-tunnel.ps1 -Undo     (or reboot)'
    }
}
function Show-Report {
    Write-Host ''
    Write-Host '1) topology  -  what the VPN does to your routing' -ForegroundColor White
    $underlay = Get-Underlay -Alias $Underlay
    if ($underlay) {
        Line OK ('physical : ' + $underlay.Alias + '  ' + $underlay.Ip + '/' + $underlay.Prefix + '  gateway ' + $underlay.Gateway + '  if ' + $underlay.IfIndex + '  metric ' + $underlay.Metric)
        Line Info ('local net: ' + $underlay.Cidr + '  -> exceptions will follow this path')
    } else {
        Line FAIL 'no physical adapter with a default gateway found - pass -Underlay "<alias>"'
    }

    $tunnel = Get-TunnelInfo -Alias $Tunnel
    if ($tunnel) {
        Line OK ('tunnel   : ' + $tunnel.Alias + '  ' + $tunnel.Ip + '  if ' + $tunnel.IfIndex + '  mtu ' + $tunnel.Mtu + '  metric ' + $tunnel.Metric + '  ' + $tunnel.RouteCount + ' routes')
        $hij = (($tunnel.HalfRoutes | Sort-Object) -join '  ')
        $extra = $tunnel.CoveredCount - @($tunnel.HalfRoutes).Count
        if ($extra -gt 0) { $hij += ('   (+' + $extra + ' more)') }
        Line Info ('hijack   : ' + $hij + '   -> together these cover all of IPv4')
        Line Info ('note     : the exact prefix set changes between connections, so this script never pattern-matches it')
        if (-not $tunnel.VisibleAsAdapter) {
            Line Info 'the tunnel has no visible network adapter - X-VPN is a Windows VPN-plugin app'
            Line Info '(manifest: vpnClient background task + networkingVpnProvider capability), so Windows'
            Line Info 'does the tunneling through the route table, which is what lets route exceptions work.'
        }
    } else {
        Line WARN 'no full-tunnel half-routes found - is X-VPN connected?'
    }

    Write-Host ''
    Write-Host '2) egress right now' -ForegroundColor White
    $eg = Get-Egress
    if ($eg.Ip -and $tunnel)  { Line OK   ('public IP: ' + $eg.Ip + '   (the VPN''s address - traffic is being tunnelled)') }
    elseif ($eg.Ip)           { Line WARN ('public IP: ' + $eg.Ip + '   (your REAL address - no VPN tunnel is up right now)') }
    else                      { Line WARN ('could not read the egress IP: ' + $eg.Error) }
    if ($eg.LocalAddress)     { Line Info ('that reading was taken on a fresh socket, source ' + $eg.LocalAddress) }

    Write-Host ''
    Write-Host '3) installed by this script' -ForegroundColor White
    $st = Get-SplitState
    if (-not $st) {
        Line Info 'nothing - no state file, this script has not changed your routing'
    } else {
        $u2 = Get-Underlay -Alias $st.UnderlayAlias
        if (-not $u2) { $u2 = [pscustomobject]@{ Alias = $st.UnderlayAlias; IfIndex = [int]$st.UnderlayIfIndex; Gateway = $st.Gateway; Ip = $st.UnderlayIp } }
        Line Info ('state from ' + $st.UpdatedAt + '  underlay ' + $st.UnderlayAlias + '  (' + @($st.Prefixes).Count + ' entries)')
        foreach ($e in @($st.Prefixes)) {
            $live = @(Find-OwnRoute -Prefix $e.Prefix -Underlay $u2 -Metric ([int]$st.RouteMetric))
            $stateTxt = if ($live.Count -gt 0) { 'installed' } elseif ($e.Preexisting) { 'already there before' } else { 'MISSING' }
            Line Info ('  ' + $e.Prefix.PadRight(20) + ' ' + $stateTxt.PadRight(22) + ' ' + $e.Source)
        }
    }

    Write-Host ''
    Write-Host '4) who wins these lookups now (Find-NetRoute)' -ForegroundColor White
    foreach ($ip in @('1.1.1.1', '8.8.8.8')) {
        $c = Get-ChosenRoute -RemoteIp $ip
        if ($c) { Line Info ($ip.PadRight(16) + ' via ' + $c.IfAlias.PadRight(22) + ' source ' + $c.SourceIp + '   metric ' + $c.Metric) }
        else    { Line WARN ($ip + '  route lookup failed') }
    }

    Write-Host ''
    Write-Host '5) next steps' -ForegroundColor White
    Line Info 'a) prove it works, safely:   .\split-tunnel.ps1 -Probe        (elevated, removes itself again)'
    Line Info 'b) preview an exclusion:     .\split-tunnel.ps1 -Exclude 1.1.1.1 -Apply -DryRun'
    Line Info 'c) install one for real:     .\split-tunnel.ps1 -Exclude 1.1.1.1,example.com -Apply'
    Line Info 'd) see or remove it:         .\split-tunnel.ps1 -Status   /   .\split-tunnel.ps1 -Undo'
    Line Info 'Only route entries are ever changed, and -Undo removes exactly what this script added.'
}
# ================================================================ main ======
if ($Log) {
    try { Start-Transcript -Path $Log -Force | Out-Null }
    catch { Write-Host ('  [WARN]  could not start the transcript: ' + $_.Exception.Message) -ForegroundColor Yellow }
}
Write-Host ''
Write-Host '==================================================================' -ForegroundColor Cyan
Write-Host ' split-tunnel.ps1  -  destination-based split tunneling for X-VPN'   -ForegroundColor Cyan
Write-Host ' routes only: no driver, no firewall rule, nothing persisted'         -ForegroundColor Cyan
Write-Host '==================================================================' -ForegroundColor Cyan

Line Info ('machine  : ' + $env:COMPUTERNAME + '   user ' + $env:USERNAME + '   elevated shell: ' + (Get-Elevated))
Line Info ('state    : ' + $statePath)

$didWork = $false
if ($Undo)   { Invoke-Undo;  $didWork = $true }
if ($Status) { Show-State;   $didWork = $true }
if ($Probe)  { Invoke-Probe; $didWork = $true }
if ($Apply)  { Invoke-Apply; $didWork = $true }
if (-not $didWork) { Show-Report }

if (($Exclude -or $DomainFile) -and -not $Apply) {
    Write-Host ''
    Line WARN '-Exclude / -DomainFile were given without -Apply, so nothing was changed'
    Line Info 'install them with -Apply, or preview with -Apply -DryRun'
}

if ($Apply -and -not $DryRun -and ($KeepAlive -or $RunSeconds -gt 0)) {
    $u = Get-Underlay -Alias $Underlay
    if ($u) { Invoke-KeepAlive -Underlay $u -Seconds $RunSeconds -Refresh $RefreshSeconds }
}

Write-Host ''
Write-Host ('summary: ' + $script:nOk + ' ok, ' + $script:nWarn + ' warnings, ' + $script:nBad + ' failures') -ForegroundColor Cyan
Write-Host 'remember: excluded traffic leaves from this machine''s real IP. -Undo reverses everything.' -ForegroundColor DarkGray
Write-Host ''
if ($Log) { try { Stop-Transcript | Out-Null } catch { } }

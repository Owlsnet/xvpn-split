<#
.SYNOPSIS
    Lets devices on the Windows Mobile Hotspot use the VPN, through a proxy.

.DESCRIPTION
    The Windows Mobile Hotspot shares the physical adapter, so devices on it do
    not use the VPN. Sharing the tunnel itself with Internet Connection Sharing
    is not possible for VPN-plugin clients such as X-VPN: the tunnel has no
    network adapter, and the sharing service refuses it or crashes.

    -Start runs an HTTP/HTTPS proxy for the hotspot instead. Devices on the
    hotspot set it as their Wi-Fi proxy (the hotspot address, normally
    192.168.137.1, and the port shown). Every outbound socket of the proxy is
    pinned to the VPN tunnel with the socket option IP_UNICAST_IF, so their
    browsing and app traffic leaves through the VPN. When the VPN is not
    connected, the proxy refuses connections instead of using the real network.
    The tunnel is looked up again every few seconds, so a VPN reconnect is
    followed.

    Changes are limited and reversible:
    - The hotspot is started if it is off, and its name and password are set
      when -Ssid or -Passphrase is given.
    - One inbound firewall rule ("xvpn hotspot proxy") allows the proxy port
      from the hotspot subnet only. The proxy also turns away every other
      address itself.
    - What -Start did is recorded in hotspot.state.json. -Stop ends the proxy,
      removes the firewall rule and stops the hotspot again if -Start started it.

    Only TCP through the proxy is covered. Traffic a device sends without the
    proxy (most games, calls, other UDP) is handled by the hotspot as usual and
    is not guaranteed to use the VPN.

    -Start and -Stop require an elevated shell (the firewall rule). Without a
    mode switch the script prints a read-only status report.

.PARAMETER Start
    Starts the hotspot if it is off, opens the firewall for the proxy and starts
    the proxy in the background. Requires elevation.

.PARAMETER Stop
    Stops the proxy, removes the firewall rule and stops the hotspot if -Start
    started it. Requires elevation.

.PARAMETER Status
    Prints the read-only status report. This is also the default.

.PARAMETER Serve
    Runs the proxy in the current window until -Stop. Used by -Start.

.PARAMETER Ssid
    With -Start, sets the network name of the hotspot.

.PARAMETER Passphrase
    With -Start, sets the hotspot password (8 to 63 characters).

.PARAMETER Port
    TCP port of the proxy. By default the first free port from 8080 to 8089.

.PARAMETER Underlay
    Interface alias of the physical adapter that carries the hotspot. By default
    the first connected adapter with an IPv4 default gateway that is not a VPN
    or virtual adapter is used.

.PARAMETER Tunnel
    Interface alias of the VPN tunnel. The tunnel is normally identified from
    the route lookup for a public address; this alias is used as a fallback.
    Defaults to 'X-VPN'.

.PARAMETER OutFile
    Also appends every output line to this file, as it is written. The xvpn
    window reads the output of elevated runs from it.

.PARAMETER Log
    Path of a transcript file that receives all output.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\hotspot.ps1

    Prints the status report and changes nothing.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\hotspot.ps1 -Start -Ssid "My hotspot" -Passphrase "a long password"

    From an elevated shell, starts the hotspot with that name and password and
    starts the proxy.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\hotspot.ps1 -Stop

    From an elevated shell, undoes -Start.

.NOTES
    Targets Windows PowerShell 5.1. The hotspot is controlled through the WinRT
    NetworkOperatorTetheringManager. The proxy connects to destinations over
    IPv4 only and resolves names with the system resolver, which uses the VPN's
    DNS while the VPN is connected.

    Files next to the script: hotspot.state.json (what -Start changed),
    hotspot.status.json (live status for the xvpn window), hotspot.serve.pid
    (the proxy) and _hotspot-serve.log (its output).
#>
[CmdletBinding()]
param(
    [switch] $Start,
    [switch] $Stop,
    [switch] $Status,
    [switch] $Serve,
    [string] $Ssid       = '',
    [string] $Passphrase = '',
    [int]    $Port       = 0,
    [string] $Underlay   = '',
    [string] $Tunnel     = 'X-VPN',
    [string] $OutFile    = '',
    [string] $Log        = ''
)

$ErrorActionPreference = 'Continue'
$scriptPath = $MyInvocation.MyCommand.Path
$scriptDir  = Split-Path -Parent $scriptPath
$statePath  = Join-Path $scriptDir 'hotspot.state.json'
$statusPath = Join-Path $scriptDir 'hotspot.status.json'
$servePid   = Join-Path $scriptDir 'hotspot.serve.pid'
$serveLog   = Join-Path $scriptDir '_hotspot-serve.log'
$ruleName   = 'xvpn-hotspot-proxy'

# Write-Host, plus a copy in -OutFile. An elevated run cannot hand its output to the
# unelevated xvpn window through a pipe, so the window reads this file instead.
function Say {
    param([string]$Text = '', [ConsoleColor]$Color = 'Gray')
    Write-Host $Text -ForegroundColor $Color
    if ($OutFile) { try { [IO.File]::AppendAllText($OutFile, $Text + "`r`n") } catch { } }
}

$script:nOk = 0; $script:nWarn = 0; $script:nBad = 0
function Line {
    param([string]$State, [string]$Text)
    if ($State -eq 'OK')       { Say ('  [ OK ]  ' + $Text) Green;  $script:nOk++ }
    elseif ($State -eq 'WARN') { Say ('  [WARN]  ' + $Text) Yellow; $script:nWarn++ }
    elseif ($State -eq 'FAIL') { Say ('  [FAIL]  ' + $Text) Red;    $script:nBad++ }
    else                       { Say ('          ' + $Text) Gray }
}

function Get-Elevated {
    return ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# ------------------------------------------------------------- topology ---
function Get-UnderlayAdapter {
    param([string]$Alias = '')
    # Virtual adapters (ZeroTier, Hyper-V, VMware, ...) often own a default route without
    # providing internet access, so real hardware is preferred and the name filter is only
    # a fallback.
    $ExcludeAlias = 'VPN|Loopback|Host-Only|Virtual|TAP|Kernel Debug|Wi-Fi Direct|WAN Miniport|6to4|Teredo|IP-HTTPS'
    $list = @()
    foreach ($cfg in @(Get-NetIPConfiguration -ErrorAction SilentlyContinue)) {
        if ($null -eq $cfg.IPv4Address -or $null -eq $cfg.IPv4DefaultGateway) { continue }
        if ($cfg.NetAdapter.Status -ne 'Up') { continue }
        if (-not $Alias -and $cfg.InterfaceAlias -match $ExcludeAlias) { continue }
        if ($Alias -and $cfg.InterfaceAlias -ne $Alias) { continue }
        $na = $cfg.NetAdapter
        $hw = $true
        if ($na.PSObject.Properties['HardwareInterface']) { $hw = ($na.HardwareInterface -eq $true) }
        elseif ($na.PSObject.Properties['Virtual'])       { $hw = ($na.Virtual -ne $true) }
        $def = @(Get-NetRoute -InterfaceIndex $cfg.InterfaceIndex -AddressFamily IPv4 -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue)
        $metric = [int]::MaxValue
        if ($def.Count -gt 0) { $metric = [int](@($def | Sort-Object RouteMetric)[0].RouteMetric) }
        $list += [pscustomobject]@{ Cfg = $cfg; Metric = $metric; Hardware = $hw }
    }
    $pick = @($list | Sort-Object @{ Expression = { -not $_.Hardware } }, Metric, @{ Expression = { [int]$_.Cfg.InterfaceIndex } }) | Select-Object -First 1
    if (-not $pick) { return $null }
    [pscustomobject]@{
        Alias   = $pick.Cfg.InterfaceAlias
        IfIndex = [int]$pick.Cfg.InterfaceIndex
        Ip      = $pick.Cfg.IPv4Address[0].IPAddress
        Guid    = [string]$pick.Cfg.NetAdapter.InterfaceGuid
    }
}

function Get-TunnelAdapter {
    param([string]$Alias = 'X-VPN', $Underlay)
    # The tunnel is whichever interface wins the route lookup for a public address when that
    # is not the physical adapter. Matching names is only a fallback, and a pin to an
    # interface that is down fails rather than leaking.
    $underlayIdx = -1
    if ($Underlay) { $underlayIdx = $Underlay.IfIndex }
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
    if (-not $ipi -or -not $ip) { return $null }
    [pscustomobject]@{ Alias = $ipi.InterfaceAlias; IfIndex = $idx; Ip = $ip.IPAddress }
}

function Get-HotspotAdapter {
    # the hotspot runs on a "Microsoft Wi-Fi Direct Virtual Adapter"; Windows gives the one
    # in use 192.168.137.1
    $cands = @(Get-NetAdapter -IncludeHidden -ErrorAction SilentlyContinue |
               Where-Object { $_.InterfaceDescription -like '*Wi-Fi Direct Virtual Adapter*' -and $_.Status -eq 'Up' })
    foreach ($a in $cands) {
        $ip = Get-NetIPAddress -InterfaceIndex $a.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($ip -and $ip.IPAddress -notlike '169.254.*') {
            return [pscustomobject]@{ Alias = $a.Name; IfIndex = [int]$a.ifIndex; Ip = $ip.IPAddress; Prefix = [int]$ip.PrefixLength }
        }
    }
    return $null
}

# The hotspot runs on the same Wi-Fi card as the PC's own connection, so it shares that
# channel: every byte crosses the air twice. On a crowded 2.4 GHz channel that is slow, which
# the window points out. Empty for Ethernet.
function Get-WifiBand {
    param([string]$Alias)
    if (-not $Alias) { return '' }
    $name = ''
    foreach ($l in @(netsh wlan show interfaces 2>$null)) {
        if ($l -match '^\s*Name\s*:\s*(.+?)\s*$') { $name = $Matches[1] }
        elseif ($name -eq $Alias -and $l -match '^\s*Band\s*:\s*(.+?)\s*$') { return $Matches[1] }
    }
    return ''
}

function Get-SubnetMask {
    param([int]$PrefixLength)
    $m = [uint32]0
    for ($i = 0; $i -lt $PrefixLength; $i++) { $m = $m -bor ([uint32]1 -shl (31 - $i)) }
    $b = [BitConverter]::GetBytes($m); [Array]::Reverse($b)
    return (New-Object System.Net.IPAddress -ArgumentList (, $b)).ToString()
}

# ---------------------------------------------------------------- WinRT ---
$script:winrt = $false
function Initialize-WinRt {
    if ($script:winrt) { return $true }
    try {
        Add-Type -AssemblyName System.Runtime.WindowsRuntime
        $null = [Windows.Networking.Connectivity.NetworkInformation, Windows.Networking.Connectivity, ContentType = WindowsRuntime]
        $null = [Windows.Networking.NetworkOperators.NetworkOperatorTetheringManager, Windows.Networking.NetworkOperators, ContentType = WindowsRuntime]
        $script:asTaskOp = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
            $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
        } | Select-Object -First 1
        $script:asTaskAction = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
            $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncAction'
        } | Select-Object -First 1
        $script:winrt = $true
    } catch {
        Line FAIL ('cannot load the Windows networking API: ' + $_.Exception.Message)
    }
    return $script:winrt
}

# Waits for a WinRT call and throws its real error. Task.Wait reports a failure only as
# "One or more errors occurred", which says nothing.
function Wait-Task {
    param($Task, [int]$TimeoutMs)
    $done = $false
    try { $done = $Task.Wait($TimeoutMs) } catch { $done = $true }
    if (-not $done) { throw 'Windows did not answer in time' }
    if ($Task.Exception) {
        $e = $Task.Exception
        while ($e.InnerException) { $e = $e.InnerException }
        throw ($e.Message.Trim() + (' (0x{0:X8})' -f $e.HResult))
    }
}

function Wait-Operation {
    param($Operation, [Type]$ResultType, [int]$TimeoutMs = 30000)
    $task = $script:asTaskOp.MakeGenericMethod($ResultType).Invoke($null, @($Operation))
    Wait-Task -Task $task -TimeoutMs $TimeoutMs
    return $task.Result
}

function Wait-Action {
    param($Action, [int]$TimeoutMs = 30000)
    $task = $script:asTaskAction.Invoke($null, @($Action))
    Wait-Task -Task $task -TimeoutMs $TimeoutMs
}

# The hotspot service can get stuck half-way ("InTransition"); then reading its settings
# hangs for good. Give it a moment to settle, and report a stuck service instead of hanging.
function Get-HotspotStateSettled {
    param($Manager, [int]$Seconds = 15)
    $until = (Get-Date).AddSeconds($Seconds)
    while ($true) {
        $st = 'Unknown'
        try { $st = [string]$Manager.TetheringOperationalState } catch { }
        if ($st -ne 'InTransition' -or (Get-Date) -ge $until) { return $st }
        Start-Sleep -Milliseconds 500
    }
}

function Test-HotspotService {
    $svc = Get-Service -Name icssvc -ErrorAction SilentlyContinue
    if ($svc -and $svc.Status -in 'StopPending', 'StartPending') {
        Line FAIL ('the Windows Mobile Hotspot service is stuck (' + $svc.Status + ') - restart Windows to clear it')
        return $false
    }
    return $true
}

function Get-TetheringManager {
    param($Underlay, [switch]$Quiet)
    if (-not $Underlay -or -not (Initialize-WinRt)) { return $null }
    # Windows keeps a profile for every known network on an adapter; the connected one is
    # the one with connectivity
    $want = [guid]$Underlay.Guid
    $profile = $null
    foreach ($p in [Windows.Networking.Connectivity.NetworkInformation]::GetConnectionProfiles()) {
        if (-not $p.NetworkAdapter -or $p.NetworkAdapter.NetworkAdapterId -ne $want) { continue }
        if ([int]$p.GetNetworkConnectivityLevel() -gt 0) { $profile = $p; break }
        if (-not $profile) { $profile = $p }
    }
    if (-not $profile) { return $null }
    try {
        return [Windows.Networking.NetworkOperators.NetworkOperatorTetheringManager]::CreateFromConnectionProfile($profile)
    } catch {
        if (-not $Quiet) { Line FAIL ('the Mobile Hotspot is not available on ' + $Underlay.Alias + ': ' + $_.Exception.Message) }
        return $null
    }
}

function Start-Hotspot {
    param($Manager)
    $r = Wait-Operation -Operation $Manager.StartTetheringAsync() -ResultType ([Windows.Networking.NetworkOperators.NetworkOperatorTetheringOperationResult])
    if ([string]$r.Status -ne 'Success') {
        $msg = [string]$r.Status
        if ($r.AdditionalErrorMessage) { $msg += ' - ' + $r.AdditionalErrorMessage }
        throw $msg
    }
}

function Stop-Hotspot {
    param($Manager)
    $r = Wait-Operation -Operation $Manager.StopTetheringAsync() -ResultType ([Windows.Networking.NetworkOperators.NetworkOperatorTetheringOperationResult])
    if ([string]$r.Status -ne 'Success') { throw ([string]$r.Status) }
}

# ---------------------------------------------------------------- proxy ---
$proxyCs = @'
using System;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public class HotspotProxy
{
    [DllImport("Ws2_32.dll", SetLastError = true)]
    private static extern int setsockopt(IntPtr s, int level, int optname, ref int optval, int optlen);

    // Interface outbound sockets are pinned to: the VPN tunnel, refreshed by the script.
    // 0 means the VPN is not connected, and then every request is refused: falling back to
    // the normal route would send the device's traffic out of the real network.
    public static volatile int TunnelIfIndex = 0;
    public static uint AllowedNet = 0, AllowedMask = 0;   // the hotspot subnet, host byte order
    public static long Requests = 0, Refused = 0, Failures = 0, Active = 0;
    public static string LogPath = "";

    private static TcpListener listener;
    private static volatile bool running = false;

    public static string Start(int port)
    {
        try
        {
            // every address, so a hotspot restart does not take the listener down; clients
            // outside the hotspot subnet are turned away in Serve
            listener = new TcpListener(IPAddress.Any, port);
            listener.Start();
            running = true;
            Thread t = new Thread(new ThreadStart(AcceptLoop));
            t.IsBackground = true;
            t.Start();
            return null;
        }
        catch (Exception ex) { return ex.Message; }
    }

    public static void SetAllowed(string ip, int prefixLength)
    {
        uint mask = prefixLength <= 0 ? 0u : (prefixLength >= 32 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> prefixLength));
        AllowedMask = mask;
        AllowedNet = ToHost(IPAddress.Parse(ip)) & mask;
    }

    public static void Stop()
    {
        running = false;
        try { if (listener != null) listener.Stop(); } catch (Exception) { }
    }

    public static bool PortFree(int port)
    {
        TcpListener l = null;
        try { l = new TcpListener(IPAddress.Any, port); l.Start(); return true; }
        catch (Exception) { return false; }
        finally { if (l != null) { try { l.Stop(); } catch (Exception) { } } }
    }

    private static uint ToHost(IPAddress a)
    {
        byte[] b = a.GetAddressBytes();
        return ((uint)b[0] << 24) | ((uint)b[1] << 16) | ((uint)b[2] << 8) | b[3];
    }

    private static void AcceptLoop()
    {
        while (running)
        {
            TcpClient client = null;
            try { client = listener.AcceptTcpClient(); }
            catch (Exception) { if (!running) return; continue; }
            TcpClient c = client;
            Thread t = new Thread(delegate() { Serve(c); });
            t.IsBackground = true;
            t.Start();
        }
    }

    private static void Serve(TcpClient client)
    {
        Interlocked.Increment(ref Active);
        try
        {
            IPEndPoint rep = client.Client.RemoteEndPoint as IPEndPoint;
            bool allowed = rep != null && rep.AddressFamily == AddressFamily.InterNetwork &&
                           (ToHost(rep.Address) & AllowedMask) == (AllowedNet & AllowedMask);
            if (!allowed) { return; }
            Handle(client);
        }
        catch (Exception ex) { Interlocked.Increment(ref Failures); Log("  ! " + ex.Message); }
        finally
        {
            try { client.Close(); } catch (Exception) { }
            Interlocked.Decrement(ref Active);
        }
    }

    private static Socket ThroughTunnel(string host, int port)
    {
        int idx = TunnelIfIndex;
        if (idx <= 0) { throw new InvalidOperationException("the VPN is not connected"); }
        IPAddress[] ips;
        IPAddress parsed;
        if (IPAddress.TryParse(host, out parsed)) { ips = new IPAddress[] { parsed }; }
        else { ips = Dns.GetHostAddresses(host); }
        Exception last = null;
        for (int i = 0; i < ips.Length; i++)
        {
            if (ips[i].AddressFamily != AddressFamily.InterNetwork) { continue; }
            Socket s = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
            // IP_UNICAST_IF restricts the route lookup for this socket to the tunnel; for IPv4
            // the index is in network byte order. If the pin cannot be set, do not connect.
            int v = IPAddress.HostToNetworkOrder(idx);
            if (setsockopt(s.Handle, 0, 31, ref v, 4) != 0) { s.Close(); throw new InvalidOperationException("cannot pin the connection to the VPN"); }
            // Nagle would hold back the small packets of a TLS handshake until the previous one
            // is acknowledged, a stall of up to 200 ms each time over the VPN's long round trip
            s.NoDelay = true;
            try { s.Connect(ips[i], port); return s; }
            catch (Exception ex) { last = ex; try { s.Close(); } catch (Exception) { } }
        }
        if (last != null) { throw last; }
        throw new Exception("no IPv4 address for " + host);
    }

    private static void Refuse(NetworkStream cs, string why)
    {
        Interlocked.Increment(ref Refused);
        string body = "xvpn: " + why + "\r\n";
        WriteAscii(cs, "HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain\r\nContent-Length: " + body.Length +
                       "\r\nConnection: close\r\n\r\n" + body);
    }

    private static readonly object logLock = new object();

    private static void Log(string msg)
    {
        if (LogPath == null || LogPath.Length == 0) { return; }
        lock (logLock)
        {
            try { System.IO.File.AppendAllText(LogPath, DateTime.Now.ToString("HH:mm:ss") + "  " + msg + Environment.NewLine); }
            catch (Exception) { }
        }
    }

    private static void Handle(TcpClient client)
    {
        client.NoDelay = true;
        NetworkStream cs = client.GetStream();
        string head = ReadHeaders(cs);
        if (head == null) { return; }
        string[] lines = head.Split(new string[] { "\r\n" }, StringSplitOptions.None);
        string[] first = lines[0].Split(' ');
        if (first.Length < 3) { WriteAscii(cs, "HTTP/1.1 400 Bad Request\r\n\r\n"); return; }
        string method = first[0].ToUpperInvariant();
        string target = first[1];

        if (method == "CONNECT")
        {
            string chost; int cport;
            if (!SplitHostPort(target, 443, out chost, out cport)) { WriteAscii(cs, "HTTP/1.1 400 Bad Request\r\n\r\n"); return; }
            Socket up;
            try { up = ThroughTunnel(chost, cport); }
            catch (InvalidOperationException ex) { Refuse(cs, ex.Message); return; }
            catch (Exception ex)
            {
                Interlocked.Increment(ref Failures);
                Log("CONNECT " + chost + ":" + cport + "  FAILED - " + ex.Message);
                WriteAscii(cs, "HTTP/1.1 502 Bad Gateway\r\n\r\n");
                return;
            }
            WriteAscii(cs, "HTTP/1.1 200 Connection Established\r\n\r\n");
            Interlocked.Increment(ref Requests);
            Pump(cs, up);
            return;
        }

        Uri u = null;
        if (!Uri.TryCreate(target, UriKind.Absolute, out u)) { WriteAscii(cs, "HTTP/1.1 400 Bad Request\r\n\r\n"); return; }
        int p2 = u.Port > 0 ? u.Port : 80;
        Socket up2;
        try { up2 = ThroughTunnel(u.Host, p2); }
        catch (InvalidOperationException ex) { Refuse(cs, ex.Message); return; }
        catch (Exception ex)
        {
            Interlocked.Increment(ref Failures);
            Log(method + " " + u.Host + "  FAILED - " + ex.Message);
            WriteAscii(cs, "HTTP/1.1 502 Bad Gateway\r\n\r\n");
            return;
        }
        StringBuilder req = new StringBuilder();
        req.Append(method + " " + u.PathAndQuery + " HTTP/1.1\r\n");
        for (int i = 1; i < lines.Length; i++)
        {
            if (lines[i].Length == 0) { continue; }
            string low = lines[i].ToLowerInvariant();
            if (low.StartsWith("proxy-connection:") || low.StartsWith("connection:") || low.StartsWith("proxy-authorization:")) { continue; }
            req.Append(lines[i] + "\r\n");
        }
        req.Append("Connection: close\r\n\r\n");
        up2.Send(Encoding.ASCII.GetBytes(req.ToString()));
        Interlocked.Increment(ref Requests);
        Pump(cs, up2);
    }

    private static void Pump(NetworkStream cs, Socket up)
    {
        NetworkStream upNs = new NetworkStream(up, false);
        Thread t = new Thread(delegate()
        {
            byte[] b = new byte[32768];
            try { int n; while ((n = cs.Read(b, 0, b.Length)) > 0) { up.Send(b, 0, n, SocketFlags.None); } }
            catch (Exception) { }
            finally { try { up.Shutdown(SocketShutdown.Send); } catch (Exception) { } }
        });
        t.IsBackground = true;
        t.Start();
        try
        {
            byte[] b = new byte[32768];
            int n;
            while ((n = upNs.Read(b, 0, b.Length)) > 0) { cs.Write(b, 0, n); }
            cs.Flush();
        }
        catch (Exception) { }
        finally
        {
            try { cs.Close(); } catch (Exception) { }
            try { upNs.Close(); } catch (Exception) { }
            try { up.Close(); } catch (Exception) { }
        }
    }

    private static string ReadHeaders(NetworkStream cs)
    {
        System.IO.MemoryStream ms = new System.IO.MemoryStream();
        int state = 0;
        while (true)
        {
            int b = cs.ReadByte();
            if (b < 0) { break; }
            ms.WriteByte((byte)b);
            if ((state == 0 || state == 2) && b == 13) { state++; }
            else if ((state == 1 || state == 3) && b == 10) { state++; }
            else { state = 0; }
            if (state == 4) { break; }
            if (ms.Length > 65536) { break; }
        }
        if (ms.Length == 0) { return null; }
        return Encoding.ASCII.GetString(ms.ToArray());
    }

    private static void WriteAscii(NetworkStream cs, string s)
    {
        byte[] b = Encoding.ASCII.GetBytes(s);
        cs.Write(b, 0, b.Length);
        cs.Flush();
    }

    private static bool SplitHostPort(string s, int defPort, out string host, out int port)
    {
        host = ""; port = defPort;
        if (s == null) { return false; }
        int i = s.LastIndexOf(':');
        if (i < 0) { host = s.Trim(); return host.Length > 0; }
        host = s.Substring(0, i).Trim();
        int p;
        if (!int.TryParse(s.Substring(i + 1).Trim(), out p)) { return false; }
        port = p;
        return host.Length > 0 && port > 0 && port < 65536;
    }
}
'@
function Initialize-Proxy {
    if (-not ('HotspotProxy' -as [type])) { Add-Type -TypeDefinition $proxyCs -Language CSharp }
}

# ---------------------------------------------------------- state files ---
function Get-HotspotState {
    if (-not (Test-Path $statePath)) { return $null }
    try { return (Get-Content $statePath -Raw | ConvertFrom-Json) } catch { return $null }
}

function Save-HotspotState {
    param([bool]$Started, [int]$ProxyPort, [string]$ProxyHost, [string]$Subnet)
    [pscustomobject]@{
        UpdatedAt      = (Get-Date).ToString('s')
        Machine        = $env:COMPUTERNAME
        Mode           = 'proxy'
        StartedHotspot = $Started
        ProxyHost      = $ProxyHost
        ProxyPort      = $ProxyPort
        Subnet         = $Subnet
        FirewallRule   = $ruleName
    } | ConvertTo-Json | Set-Content -Path $statePath -Encoding UTF8
}

# hotspot.status.json is what the xvpn window shows: it cannot ask Windows itself
function Save-HotspotStatus {
    param($Manager, [bool]$Serving, [bool]$On, [int]$Clients, [string]$TunnelAlias = '', [string]$ProxyHost = '', [int]$ProxyPort = 0, [string]$Problem = '', [string]$Band = '')
    $on = $On; $clients = $Clients; $max = 0; $name = ''
    if ($Manager) {
        try {
            $st = [string]$Manager.TetheringOperationalState
            $max = [int]$Manager.MaxClientCount
            # reading the settings of a hotspot that is switching hangs
            if ($st -eq 'On' -or $st -eq 'Off') { $name = $Manager.GetCurrentAccessPointConfiguration().Ssid }
        } catch { }
    }
    $req = 0; $active = 0; $refused = 0
    if ('HotspotProxy' -as [type]) { $req = [HotspotProxy]::Requests; $active = [HotspotProxy]::Active; $refused = [HotspotProxy]::Refused }
    $obj = [pscustomobject]@{
        UpdatedAt   = (Get-Date).ToString('s')
        HotspotOn   = $on
        Shared      = ($Serving -and $on -and [bool]$TunnelAlias)
        TunnelAlias = $TunnelAlias
        Ssid        = $name
        Clients     = $clients
        MaxClients  = $max
        ProxyHost   = $ProxyHost
        ProxyPort   = $ProxyPort
        Requests    = $req
        Active      = $active
        Refused     = $refused
        Problem     = $Problem
        UplinkBand  = $Band
    }
    try { $obj | ConvertTo-Json | Set-Content -Path $statusPath -Encoding UTF8 } catch { }
}

function Get-ServeProcess {
    if (-not (Test-Path $servePid)) { return $null }
    try { $id = [int]((Get-Content $servePid -Raw | ConvertFrom-Json).ProcessId) } catch { return $null }
    $p = Get-Process -Id $id -ErrorAction SilentlyContinue
    if ($p -and $p.ProcessName -eq 'powershell') { return $p }
    return $null
}

function Stop-ServeProcess {
    $p = Get-ServeProcess
    if ($p) {
        try { Stop-Process -Id $p.Id -Force -ErrorAction Stop; Line OK ('proxy stopped (pid ' + $p.Id + ')') }
        catch { Line WARN ('cannot stop the proxy: ' + $_.Exception.Message) }
    }
    Remove-Item -Path $servePid -Force -ErrorAction SilentlyContinue
}

function Remove-ProxyRule {
    Get-NetFirewallRule -Name $ruleName -ErrorAction SilentlyContinue | Remove-NetFirewallRule -ErrorAction SilentlyContinue
}

# ---------------------------------------------------------------- modes ---
function Invoke-Start {
    Say ''
    Say 'START  -  hotspot devices use the VPN through a proxy' White
    if (-not (Get-Elevated)) { Line FAIL '-Start needs an elevated shell (Terminal (Admin))'; return $false }
    if ($Passphrase -and ($Passphrase.Length -lt 8 -or $Passphrase.Length -gt 63)) {
        Line FAIL 'the hotspot password must be 8 to 63 characters'
        return $false
    }
    $underlay = Get-UnderlayAdapter -Alias $Underlay
    if (-not $underlay) { Line FAIL 'no Wi-Fi or Ethernet connection with internet access found'; return $false }
    Line OK ('physical : ' + $underlay.Alias + '  ' + $underlay.Ip)
    $tunnel = Get-TunnelAdapter -Alias $Tunnel -Underlay $underlay
    if ($tunnel) { Line OK ('tunnel   : ' + $tunnel.Alias + '  ' + $tunnel.Ip + '  if ' + $tunnel.IfIndex) }
    else         { Line WARN 'the VPN is not connected - the proxy waits for it and refuses connections until then' }

    if (-not (Test-HotspotService)) { return $false }
    $tm = Get-TetheringManager -Underlay $underlay
    if (-not $tm) { Line FAIL ('the Mobile Hotspot cannot run on ' + $underlay.Alias); return $false }
    $hs = Get-HotspotStateSettled -Manager $tm
    if ($hs -eq 'InTransition') {
        Line FAIL 'the Windows Mobile Hotspot is stuck switching on or off - restart Windows to clear it'
        return $false
    }
    $state = Get-HotspotState
    $started = $false
    if ($state) { $started = [bool]$state.StartedHotspot }
    if ($Ssid -or $Passphrase) {
        try {
            $ap = $tm.GetCurrentAccessPointConfiguration()
            $change = ($Ssid -and $ap.Ssid -cne $Ssid) -or ($Passphrase -and $ap.Passphrase -cne $Passphrase)
            if ($change) {
                # the hotspot takes a new name or password only while it is off
                if ($hs -eq 'On') { Stop-Hotspot -Manager $tm; $null = Get-HotspotStateSettled -Manager $tm }
                if ($Ssid) { $ap.Ssid = $Ssid }
                if ($Passphrase) { $ap.Passphrase = $Passphrase }
                Wait-Action -Action $tm.ConfigureAccessPointAsync($ap)
                $what = 'name "' + $ap.Ssid + '"'
                if ($Passphrase) { $what += ', password set' }
                Line OK ('hotspot  : ' + $what)
            }
        } catch {
            Line FAIL ('cannot change the hotspot name or password: ' + $_.Exception.Message)
            return $false
        }
    }
    if ((Get-HotspotStateSettled -Manager $tm) -ne 'On') {
        try { Start-Hotspot -Manager $tm; $started = $true; Line OK ('hotspot  : started on ' + $underlay.Alias) }
        catch { Line FAIL ('cannot start the Mobile Hotspot: ' + $_.Exception.Message); return $false }
    } else {
        Line OK ('hotspot  : already on, ' + $tm.ClientCount + ' of ' + $tm.MaxClientCount + ' device(s) connected')
    }
    $hotspot = $null
    for ($i = 0; $i -lt 20 -and -not $hotspot; $i++) {
        $hotspot = Get-HotspotAdapter
        if (-not $hotspot) { Start-Sleep -Milliseconds 500 }
    }
    if (-not $hotspot) { Line FAIL 'the hotspot is on but its network adapter did not get an address'; return $false }
    $mask = Get-SubnetMask -PrefixLength $hotspot.Prefix
    $subnet = $hotspot.Ip + '/' + $hotspot.Prefix

    Stop-ServeProcess
    Initialize-Proxy
    $usePort = $Port
    if ($usePort -le 0) {
        foreach ($c in 8080..8089) { if ([HotspotProxy]::PortFree($c)) { $usePort = $c; break } }
    }
    if ($usePort -le 0) { Line FAIL 'no free port between 8080 and 8089 - pass -Port <number>'; return $false }

    try {
        Remove-ProxyRule
        $remote = $hotspot.Ip + '/' + $mask
        New-NetFirewallRule -Name $ruleName -DisplayName 'xvpn hotspot proxy' -Description 'Lets devices on the Mobile Hotspot reach the xvpn proxy. Removed by hotspot.ps1 -Stop.' `
            -Direction Inbound -Action Allow -Protocol TCP -LocalPort $usePort -RemoteAddress $remote -Profile Any -ErrorAction Stop | Out-Null
        Line OK ('firewall : port ' + $usePort + ' open to the hotspot (' + $subnet + ') only')
    } catch {
        Line FAIL ('cannot add the firewall rule: ' + $_.Exception.Message)
        return $false
    }
    Save-HotspotState -Started $started -ProxyPort $usePort -ProxyHost $hotspot.Ip -Subnet $subnet

    $serveArgs = '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + $scriptPath + '" -Serve -Port ' + $usePort +
                 ' -Tunnel "' + $Tunnel + '" -OutFile "' + $serveLog + '"'
    if ($Underlay) { $serveArgs += ' -Underlay "' + $Underlay + '"' }
    try {
        Remove-Item -Path $serveLog, $statusPath -Force -ErrorAction SilentlyContinue
        $p = Start-Process -FilePath (Join-Path $PSHOME 'powershell.exe') -ArgumentList $serveArgs -WindowStyle Hidden -PassThru -ErrorAction Stop
    } catch {
        Line FAIL ('cannot start the proxy: ' + $_.Exception.Message)
        return $false
    }
    # wait for the proxy to report that it listens
    $ready = $false
    for ($i = 0; $i -lt 40 -and -not $ready; $i++) {
        Start-Sleep -Milliseconds 250
        if ($p.HasExited) { break }
        if (Test-Path $statusPath) { $ready = $true }
    }
    if (-not $ready) {
        Line FAIL 'the proxy did not start'
        if (Test-Path $serveLog) { foreach ($l in (Get-Content $serveLog | Select-Object -Last 5)) { Line Info $l.Trim() } }
        return $false
    }
    Line OK ('proxy    : ' + $hotspot.Ip + ':' + $usePort + '   (pid ' + $p.Id + ')')
    Say ''
    Line Info ('on each device: join the hotspot, then set its Wi-Fi proxy to Manual,')
    Line Info ('server ' + $hotspot.Ip + ', port ' + $usePort)
    return $true
}

function Invoke-Stop {
    Say ''
    Say 'STOP  -  hotspot devices stop using the VPN' White
    if (-not (Get-Elevated)) { Line FAIL '-Stop needs an elevated shell (Terminal (Admin))'; return }
    $state = Get-HotspotState
    # the proxy loop ends by itself once the state file is gone; stopping it as well is quicker
    Remove-Item -Path $statePath -Force -ErrorAction SilentlyContinue
    Stop-ServeProcess
    Remove-ProxyRule
    Line OK 'firewall rule removed'
    if ($state -and $state.StartedHotspot) {
        $tm = Get-TetheringManager -Underlay (Get-UnderlayAdapter -Alias $Underlay)
        if ($tm -and [string]$tm.TetheringOperationalState -eq 'On') {
            try { Stop-Hotspot -Manager $tm; Line OK 'hotspot turned off (it was off before you started)' }
            catch { Line WARN ('cannot stop the Mobile Hotspot: ' + $_.Exception.Message) }
        }
    } elseif ($state) {
        Line Info 'the hotspot stays on, as it was before you started'
    }
    Remove-Item -Path $statusPath -Force -ErrorAction SilentlyContinue
    Line OK 'stopped'
}

# The proxy itself. Keeps the tunnel pin and the status file current until -Stop removes
# the state file (or this process is ended).
function Invoke-Serve {
    $state = Get-HotspotState
    if (-not $state) { Line FAIL 'no state file - start with -Start'; return }
    [pscustomobject]@{ ProcessId = $PID; StartedAt = (Get-Date).ToString('s') } | ConvertTo-Json | Set-Content -Path $servePid -Encoding UTF8
    Initialize-Proxy
    $listenPort = $Port
    if ($listenPort -le 0) { $listenPort = [int]$state.ProxyPort }
    $proxyHost = [string]$state.ProxyHost
    $parts = ([string]$state.Subnet).Split('/')
    [HotspotProxy]::SetAllowed($parts[0], [int]$parts[1])
    [HotspotProxy]::LogPath = $serveLog
    $err = [HotspotProxy]::Start($listenPort)
    if ($err) { Line FAIL ('cannot listen on port ' + $listenPort + ': ' + $err); return }
    Line OK ('listening on port ' + $listenPort + ' for ' + $state.Subnet)

    $underlay = $null; $tm = $null; $nextTopo = Get-Date
    $lastTunnel = -1; $lastHotspot = $null
    try {
        while (Test-Path $statePath) {
            if ((Get-Date) -ge $nextTopo) {
                $underlay = Get-UnderlayAdapter -Alias $Underlay
                $tm = Get-TetheringManager -Underlay $underlay -Quiet
                $band = Get-WifiBand -Alias $underlay.Alias
                $nextTopo = (Get-Date).AddSeconds(30)
            }
            $tunnel = Get-TunnelAdapter -Alias $Tunnel -Underlay $underlay
            $idx = 0
            if ($tunnel) { $idx = $tunnel.IfIndex }
            [HotspotProxy]::TunnelIfIndex = $idx
            $stamp = (Get-Date).ToString('HH:mm:ss')
            if ($idx -ne $lastTunnel) {
                if ($idx -gt 0) { Line OK ($stamp + '  through ' + $tunnel.Alias + ' (if ' + $idx + ')') }
                else            { Line WARN ($stamp + '  the VPN is not connected - refusing connections until it is') }
                $lastTunnel = $idx
            }
            # The tethering API's state flickers and its client count stays at 0, so the hotspot
            # adapter is asked instead: it is on when it is up with its address, and every device
            # on it has an entry in its neighbor table.
            $hsAdapter = Get-HotspotAdapter
            $on = $null -ne $hsAdapter
            $clients = 0
            if ($on) {
                $clients = @(Get-NetNeighbor -InterfaceIndex $hsAdapter.IfIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object {
                    $_.State -notin 'Unreachable', 'Incomplete' -and $_.LinkLayerAddress -notmatch '^(FF-FF-FF-FF-FF-FF|01-00-5E|00-00-00-00-00-00)' -and
                    $_.IPAddress -ne $hsAdapter.Ip }).Count
            }
            if ($on -ne $lastHotspot) {
                if ($on) { Line OK ($stamp + '  the hotspot is on') } else { Line WARN ($stamp + '  the hotspot is off') }
                $lastHotspot = $on
            }
            $problem = ''
            if (-not $on)        { $problem = 'the hotspot is off' }
            elseif ($idx -eq 0)  { $problem = 'waiting for the VPN to connect' }
            $alias = ''
            if ($tunnel) { $alias = $tunnel.Alias }
            Save-HotspotStatus -Manager $tm -Serving $true -On $on -Clients $clients -TunnelAlias $alias -ProxyHost $proxyHost -ProxyPort $listenPort -Problem $problem -Band $band
            Start-Sleep -Seconds 3
        }
    } finally {
        [HotspotProxy]::Stop()
        $own = $false
        try { $own = ([int]((Get-Content $servePid -Raw | ConvertFrom-Json).ProcessId) -eq $PID) } catch { }
        if ($own) { Remove-Item -Path $servePid -Force -ErrorAction SilentlyContinue }
        Line Info 'proxy stopped'
    }
}

function Show-Report {
    Say ''
    Say '1) network' White
    $underlay = Get-UnderlayAdapter -Alias $Underlay
    if ($underlay) { Line OK ('physical : ' + $underlay.Alias + '  ' + $underlay.Ip) }
    else           { Line FAIL 'no Wi-Fi or Ethernet connection with internet access found' }
    $tunnel = Get-TunnelAdapter -Alias $Tunnel -Underlay $underlay
    if ($tunnel) { Line OK ('tunnel   : ' + $tunnel.Alias + '  ' + $tunnel.Ip + '  if ' + $tunnel.IfIndex) }
    else         { Line WARN 'the VPN is not connected' }

    Say ''
    Say '2) mobile hotspot' White
    $tm = $null
    $svcOk = Test-HotspotService
    if ($svcOk) { $tm = Get-TetheringManager -Underlay $underlay }
    if (-not $svcOk) {
        # already reported
    } elseif ($tm) {
        $st = Get-HotspotStateSettled -Manager $tm -Seconds 5
        if ($st -eq 'InTransition') {
            Line WARN 'hotspot  : stuck switching on or off - restart Windows to clear it'
        } else {
            $ap = $tm.GetCurrentAccessPointConfiguration()
            if ($st -eq 'On') { Line OK ('hotspot  : on   "' + $ap.Ssid + '"   ' + $tm.ClientCount + ' of ' + $tm.MaxClientCount + ' device(s) connected') }
            else              { Line Info ('hotspot  : ' + $st.ToLower() + '   "' + $ap.Ssid + '"') }
        }
    } elseif ($underlay) {
        Line WARN ('the Mobile Hotspot is not available on ' + $underlay.Alias)
    }

    Say ''
    Say '3) proxy' White
    $state = Get-HotspotState
    $p = Get-ServeProcess
    if ($state -and $p) {
        Line OK ('proxy    : ' + $state.ProxyHost + ':' + $state.ProxyPort + '   (pid ' + $p.Id + ')')
        if (Test-Path $statusPath) {
            try {
                $s = Get-Content $statusPath -Raw | ConvertFrom-Json
                Line Info ('traffic  : ' + $s.Requests + ' connection(s) so far, ' + $s.Active + ' open, ' + $s.Refused + ' refused while the VPN was down')
            } catch { }
        }
        Line Info ('devices  : set the Wi-Fi proxy to Manual, server ' + $state.ProxyHost + ', port ' + $state.ProxyPort)
    } elseif ($state) {
        Line WARN 'the proxy is not running - start it again'
    } else {
        Line Info 'not started'
    }
}

# ================================================================ main ======
if ($Log) {
    try { Start-Transcript -Path $Log -Force | Out-Null }
    catch { Say ('  [WARN]  could not start the transcript: ' + $_.Exception.Message) Yellow }
}
if (-not $Serve) {
    Say ''
    Say '==================================================================' Cyan
    Say ' hotspot.ps1  -  devices on the Mobile Hotspot use the VPN'           Cyan
    Say ' through a proxy pinned to the VPN tunnel'                            Cyan
    Say '==================================================================' Cyan
    Line Info ('machine  : ' + $env:COMPUTERNAME + '   user ' + $env:USERNAME + '   elevated shell: ' + (Get-Elevated))
}

$exitCode = 0
if ($Start -and $Stop) {
    Line FAIL '-Start and -Stop cannot be combined'
    $exitCode = 1
} elseif ($Stop) {
    Invoke-Stop
} elseif ($Start) {
    if (-not (Invoke-Start)) { $exitCode = 1 }
} elseif ($Serve) {
    Invoke-Serve
} else {
    Show-Report
}

if (-not $Serve) {
    Say ''
    Say ('summary: ' + $script:nOk + ' ok, ' + $script:nWarn + ' warnings, ' + $script:nBad + ' failures') Cyan
    Say ''
}
if ($Log) { try { Stop-Transcript | Out-Null } catch { } }
exit $exitCode

<#
.SYNOPSIS
    Routes selected applications around a full-tunnel VPN through a loopback proxy.

.DESCRIPTION
    Windows has no per-process routing for user-mode code: the route table is
    shared by all processes, so a full-tunnel VPN captures all traffic, and route
    exceptions (see split-tunnel.ps1) can only be made per destination.

    This script starts an HTTP/HTTPS CONNECT proxy on 127.0.0.1. Every outbound
    socket the proxy opens is pinned to the physical network adapter with the
    socket option IP_UNICAST_IF, which restricts the route lookup for that socket
    to the given interface. An application configured to use the proxy therefore
    reaches the network through the physical adapter, while all other processes
    keep using the VPN tunnel.

    Optionally, a Chromium-based browser is launched with --proxy-server pointing
    at the proxy. The script makes no persistent changes to the system (no route
    entries, firewall rules, registry keys or drivers) and does not require
    administrator rights. Stopping the proxy only closes the loopback port.

.PARAMETER Port
    Loopback TCP port the proxy listens on. The port must be free. Default: 49500.

.PARAMETER Launch
    Browser to start with the proxy configured: None, Chrome, Edge or Brave. The
    browser is started from its default installation path. Default: None.

.PARAMETER ExePath
    Full path of an executable to start instead of the -Launch browser. It is
    given the same Chromium switches (--proxy-server, --disable-quic), so it must
    be a Chromium-based browser or otherwise accept those switches.

.PARAMETER Test
    Runs a self-test and exits. The public address reported by
    https://1.1.1.1/cdn-cgi/trace is fetched once over an ordinary socket and
    once through the proxy, and the two results are compared.

.PARAMETER RunSeconds
    Stops the proxy after the given number of seconds. 0 runs until Ctrl+C.
    Default: 0.

.PARAMETER NoLog
    Suppresses the per-request log lines, both on the console and in the
    request log file.

.PARAMETER Underlay
    Interface alias of the physical adapter that outbound sockets are pinned to.
    By default the first connected adapter with an IPv4 default gateway that is
    not a VPN or virtual adapter is used.

.PARAMETER UserDataDir
    Profile directory passed to the browser with --user-data-dir. A separate
    profile starts a separate browser instance, so the proxy switch takes effect
    even while the regular profile is already open.

.PARAMETER Log
    Writes a transcript of the console output to this file. Unless -RequestLog is
    given, per-request lines are written to "<Log>.requests".

.PARAMETER RequestLog
    File that per-request log lines are appended to.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\app-bypass.ps1 -Test

    Starts the proxy, compares the public address seen with and without it, and
    exits.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\app-bypass.ps1 -Launch Chrome

    Starts the proxy and launches Chrome through it. The proxy runs until Ctrl+C.

.EXAMPLE
    .\app-bypass.ps1 -Launch Edge -UserDataDir "$env:LOCALAPPDATA\xvpn\edge-out" -RunSeconds 3600

    Launches Edge with a separate profile through the proxy and stops the proxy
    after one hour.

.EXAMPLE
    .\app-bypass.ps1 -Port 49501 -Log "$env:TEMP\app-bypass.log" -RequestLog "$env:TEMP\app-bypass.requests.log"

    Runs only the proxy, without launching an application. This is how
    xvpn-ui.exe and app-chooser.ps1 start the script in the background.

.NOTES
    Only applications that support a proxy setting can be routed this way. For
    applications that ignore proxies (many games, installers and voice clients),
    use per-destination exceptions with split-tunnel.ps1 instead.

    Do not set the system-wide Windows proxy to this address: that would move
    every application off the VPN.

    The proxy connects to destinations over IPv4 only.
#>
[CmdletBinding()]
param(
    [int] $Port = 49500,
    [ValidateSet('None', 'Chrome', 'Edge', 'Brave')][string] $Launch = 'None',
    [string] $ExePath = '',
    [switch] $Test,
    [int] $RunSeconds = 0,
    [switch] $NoLog,
    [string] $Underlay = '',
    [string] $UserDataDir = '',
    [string] $Log = '',
    [string] $RequestLog = ''
)

$ErrorActionPreference = 'Continue'

$script:nOk = 0; $script:nWarn = 0; $script:nBad = 0
function Line {
    param([string]$State, [string]$Text)
    if ($State -eq 'OK')       { Write-Host ('  [ OK ]  ' + $Text) -ForegroundColor Green;  $script:nOk++ }
    elseif ($State -eq 'WARN') { Write-Host ('  [WARN]  ' + $Text) -ForegroundColor Yellow; $script:nWarn++ }
    elseif ($State -eq 'FAIL') { Write-Host ('  [FAIL]  ' + $Text) -ForegroundColor Red;    $script:nBad++ }
    else                       { Write-Host ('          ' + $Text) -ForegroundColor Gray }
}

function Get-Physical {
    param([string]$Alias = '')
    $ExcludeAlias = 'VPN|Loopback|Host-Only|Virtual|TAP|Kernel Debug|Wi-Fi Direct|WAN Miniport|6to4|Teredo|IP-HTTPS'
    $cfgs = Get-NetIPConfiguration -ErrorAction SilentlyContinue | Where-Object {
        $_.IPv4DefaultGateway -ne $null -and $_.NetAdapter.Status -eq 'Up' -and $_.InterfaceAlias -notmatch $ExcludeAlias
    }
    if ($Alias) { $cfgs = @($cfgs | Where-Object { $_.InterfaceAlias -eq $Alias }) }
    $cfg = $cfgs | Select-Object -First 1
    if (-not $cfg) { return $null }
    $def = @(Get-NetRoute -InterfaceIndex $cfg.InterfaceIndex -AddressFamily IPv4 -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue).Count
    [pscustomobject]@{
        Alias   = $cfg.InterfaceAlias
        IfIndex = [int]$cfg.InterfaceIndex
        Ip      = $cfg.IPv4Address[0].IPAddress
        Gateway = $cfg.IPv4DefaultGateway[0].NextHop
        HasDefaultRoute = ($def -gt 0)
    }
}

function Get-Tunnel {
    param([int]$UnderlayIfIndex = -1, [string]$Alias = 'X-VPN')
    # The set of covering routes a VPN client installs varies between connections
    # (0.0.0.0/1 + 128.0.0.0/1, or a finer split such as 0.0.0.0/4, 16.0.0.0/5, ...),
    # so prefixes are not matched. Instead the OS is asked which interface wins the
    # route lookup for a public address; if it is not the physical adapter, it is
    # taken to be the tunnel.
    $idx = 0
    $fr = @(Find-NetRoute -RemoteIPAddress '1.1.1.1' -ErrorAction SilentlyContinue)
    if ($fr.Count -ge 2) {
        $ri = [int]$fr[$fr.Count - 1].InterfaceIndex
        if ($ri -ne 0 -and ($UnderlayIfIndex -lt 0 -or $ri -ne $UnderlayIfIndex)) { $idx = $ri }
    }
    if ($idx -eq 0) {
        $c = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object {
            $_.InterfaceIndex -ne 1 -and $_.InterfaceIndex -ne $UnderlayIfIndex -and
            ($_.InterfaceAlias -eq $Alias -or $_.InterfaceAlias -match 'VPN|TUN|WIREGUARD|WG')
        } | Select-Object -First 1
        if ($c) { $idx = [int]$c.InterfaceIndex }
    }
    if ($idx -eq 0) { return $null }
    $ipi = Get-NetIPInterface -InterfaceIndex $idx -AddressFamily IPv4 -ErrorAction SilentlyContinue
    $ipa = Get-NetIPAddress -InterfaceIndex $idx -AddressFamily IPv4 -ErrorAction SilentlyContinue | Select-Object -First 1
    [pscustomobject]@{
        IfIndex  = $idx
        Alias    = $ipi.InterfaceAlias
        Ip       = $ipa.IPAddress
        Mtu      = $ipi.NlMtu
        Covered  = @(Get-NetRoute -InterfaceIndex $idx -AddressFamily IPv4 -ErrorAction SilentlyContinue |
                     Where-Object { $_.DestinationPrefix -notmatch '/32$' } |
                     Select-Object -ExpandProperty DestinationPrefix -Unique).Count
    }
}
$cs = @'
using System;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public class AppBypass
{
    [DllImport("Ws2_32.dll", SetLastError = true)]
    private static extern int setsockopt(IntPtr s, int level, int optname, ref int optval, int optlen);

    public static int  IfIndex  = 0;     // interface index outbound sockets are pinned to
    public static bool Verbose  = true;  // log one line per request
    public static string LogPath = "";   // optional file that request lines are appended to
    public static long Requests = 0;
    public static long Tunnels  = 0;
    public static long Failures = 0;

    private static TcpListener listener;
    private static bool running = false;

    public static string Start(int port)
    {
        try
        {
            listener = new TcpListener(IPAddress.Loopback, port);
            listener.Start();
            running = true;
            Thread t = new Thread(new ThreadStart(AcceptLoop));
            t.IsBackground = true;
            t.Start();
            return null;
        }
        catch (Exception ex) { return ex.Message; }
    }

    public static void Stop()
    {
        running = false;
        try { if (listener != null) listener.Stop(); } catch (Exception) { }
    }

    public static string Stats()
    {
        return Requests + " request(s), " + Tunnels + " CONNECT tunnel(s), " + Failures + " failure(s)";
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
        try { Handle(client); }
        catch (Exception ex) { Failures++; Log("  ! " + ex.Message); }
        finally { try { client.Close(); } catch (Exception) { } }
    }

    private static Socket Pinned(string host, int port)
    {
        IPAddress[] ips;
        IPAddress parsed;
        if (IPAddress.TryParse(host, out parsed)) { ips = new IPAddress[] { parsed }; }
        else { ips = Dns.GetHostAddresses(host); }
        Exception last = null;
        for (int i = 0; i < ips.Length; i++)
        {
            if (ips[i].AddressFamily != AddressFamily.InterNetwork) { continue; }
            Socket s = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
            if (IfIndex > 0)
            {
                // IP_UNICAST_IF restricts the route lookup for this socket to one
                // interface. For IPv4 the index must be in network byte order.
                int v = IPAddress.HostToNetworkOrder(IfIndex);
                setsockopt(s.Handle, 0, 31, ref v, 4);   // IPPROTO_IP, IP_UNICAST_IF
            }
            try { s.Connect(ips[i], port); return s; }
            catch (Exception ex) { last = ex; try { s.Close(); } catch (Exception) { } }
        }
        if (last != null) { throw last; }
        throw new Exception("no IPv4 address for " + host);
    }

    private static readonly object logLock = new object();

    private static void Log(string msg)
    {
        if (!Verbose) { return; }
        string line = DateTime.Now.ToString("HH:mm:ss") + "  " + msg;
        Console.WriteLine(line);
        if (LogPath != null && LogPath.Length > 0)
        {
            lock (logLock)
            {
                try { System.IO.File.AppendAllText(LogPath, line + Environment.NewLine); }
                catch (Exception) { }
            }
        }
    }
    private static void Handle(TcpClient client)
    {
        client.NoDelay = true;
        NetworkStream cs = client.GetStream();
        string head = ReadHeaders(cs);
        if (head == null) { return; }
        string[] lines = head.Split(new string[] { "\r\n" }, StringSplitOptions.None);
        if (lines.Length == 0) { return; }
        string[] first = lines[0].Split(' ');
        if (first.Length < 3) { WriteAscii(cs, "HTTP/1.1 400 Bad Request\r\n\r\n"); return; }
        string method = first[0].ToUpperInvariant();
        string target = first[1];

        if (method == "CONNECT")
        {
            string chost; int cport;
            if (!SplitHostPort(target, 443, out chost, out cport)) { WriteAscii(cs, "HTTP/1.1 400 Bad Request\r\n\r\n"); return; }
            Socket up;
            try { up = Pinned(chost, cport); }
            catch (Exception ex)
            {
                Failures++;
                Log("CONNECT " + chost + ":" + cport + "  FAILED - " + ex.Message);
                WriteAscii(cs, "HTTP/1.1 502 Bad Gateway\r\n\r\n");
                return;
            }
            WriteAscii(cs, "HTTP/1.1 200 Connection Established\r\n\r\n");
            Requests++; Tunnels++;
            Log("CONNECT " + chost + ":" + cport + "  ->  " + up.LocalEndPoint.ToString() + " -> " + up.RemoteEndPoint.ToString());
            Pump(cs, up);
            return;
        }

        Uri u = null;
        if (!Uri.TryCreate(target, UriKind.Absolute, out u)) { WriteAscii(cs, "HTTP/1.1 400 Bad Request\r\n\r\n"); return; }
        int p2 = u.Port > 0 ? u.Port : 80;
        Socket up2;
        try { up2 = Pinned(u.Host, p2); }
        catch (Exception ex)
        {
            Failures++;
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
            if (low.StartsWith("proxy-connection:")) { continue; }
            if (low.StartsWith("connection:")) { continue; }
            if (low.StartsWith("proxy-authorization:")) { continue; }
            req.Append(lines[i] + "\r\n");
        }
        req.Append("Connection: close\r\n\r\n");
        up2.Send(Encoding.ASCII.GetBytes(req.ToString()));
        Requests++;
        Log(method + " " + u.Host + u.PathAndQuery + "  ->  " + up2.LocalEndPoint.ToString());
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
# ========================================================== self-test =======
# Both helpers fetch https://1.1.1.1/cdn-cgi/trace, which reports the public
# address the request arrived from. Certificate validation is skipped because
# only that address is of interest, not the identity of the server.
function Get-TraceDirect {
    param([string]$Ip = '1.1.1.1', [int]$TimeoutMs = 12000)
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
        $b = [Text.Encoding]::ASCII.GetBytes('GET /cdn-cgi/trace HTTP/1.1' + "`r`n" + 'Host: ' + $Ip + "`r`n" + 'Connection: close' + "`r`n`r`n")
        $ssl.Write($b, 0, $b.Length); $ssl.Flush()
        $body = (New-Object System.IO.StreamReader($ssl)).ReadToEnd()
        $m = [regex]::Match($body, '(?m)^ip=(\S+)')
        $v = ''
        if ($m.Success) { $v = $m.Groups[1].Value }
        return [pscustomobject]@{ Ip = $v; LocalAddress = $local; Error = '' }
    } catch {
        return [pscustomobject]@{ Ip = ''; LocalAddress = $local; Error = $_.Exception.Message }
    } finally {
        if ($ssl) { $ssl.Dispose() }
        if ($client) { $client.Close() }
    }
}

function Get-TraceViaProxy {
    param([int]$ProxyPort, [string]$Ip = '1.1.1.1', [int]$TimeoutMs = 15000)
    $client = $null; $ssl = $null
    try {
        $client = New-Object System.Net.Sockets.TcpClient
        $ar = $client.BeginConnect('127.0.0.1', $ProxyPort, $null, $null)
        if (-not $ar.AsyncWaitHandle.WaitOne(5000)) { return [pscustomobject]@{ Ip = ''; Error = 'the proxy did not accept a connection' } }
        $client.EndConnect($ar)
        $ns = $client.GetStream()
        $hb = [Text.Encoding]::ASCII.GetBytes('CONNECT ' + $Ip + ':443 HTTP/1.1' + "`r`n" + 'Host: ' + $Ip + ':443' + "`r`n`r`n")
        $ns.Write($hb, 0, $hb.Length); $ns.Flush()
        $sb = New-Object System.Text.StringBuilder
        while ($true) {
            $byte = $ns.ReadByte()
            if ($byte -lt 0) { break }
            [void]$sb.Append([char]$byte)
            if ($sb.ToString().EndsWith("`r`n`r`n") -or $sb.Length -gt 4096) { break }
        }
        $reply = $sb.ToString()
        if ($reply -notmatch '200') {
            $firstLine = ($reply -split "`r`n")[0]
            return [pscustomobject]@{ Ip = ''; Error = ('the proxy said: ' + $firstLine) }
        }
        $cb  = [System.Net.Security.RemoteCertificateValidationCallback]{ param($s, $c, $ch, $e) return $true }
        $ssl = New-Object System.Net.Security.SslStream($ns, $false, $cb)
        $ssl.AuthenticateAsClient($Ip)
        $b = [Text.Encoding]::ASCII.GetBytes('GET /cdn-cgi/trace HTTP/1.1' + "`r`n" + 'Host: ' + $Ip + "`r`n" + 'Connection: close' + "`r`n`r`n")
        $ssl.Write($b, 0, $b.Length); $ssl.Flush()
        $body = (New-Object System.IO.StreamReader($ssl)).ReadToEnd()
        $m = [regex]::Match($body, '(?m)^ip=(\S+)')
        $v = ''
        if ($m.Success) { $v = $m.Groups[1].Value }
        return [pscustomobject]@{ Ip = $v; Error = '' }
    } catch {
        return [pscustomobject]@{ Ip = ''; Error = $_.Exception.Message }
    } finally {
        if ($ssl) { $ssl.Dispose() }
        if ($client) { $client.Close() }
    }
}
# ================================================================ main ======
if ($Log) {
    try { Start-Transcript -Path $Log -Force | Out-Null }
    catch { Write-Host ('  [WARN]  could not start the transcript: ' + $_.Exception.Message) -ForegroundColor Yellow }
}
Write-Host ''
Write-Host '==================================================================' -ForegroundColor Cyan
Write-Host ' app-bypass.ps1  -  one app on the real network, all the rest on the VPN' -ForegroundColor Cyan
Write-Host ' loopback proxy only: no route, no firewall, no driver, no admin'          -ForegroundColor Cyan
Write-Host '==================================================================' -ForegroundColor Cyan

Line Info ('machine  : ' + $env:COMPUTERNAME + '   user ' + $env:USERNAME)

$phys = Get-Physical -Alias $Underlay
if (-not $phys) { Line FAIL 'no physical adapter with a default gateway found - pass -Underlay "<alias>"'; exit 1 }
$tunnel = Get-Tunnel -UnderlayIfIndex $phys.IfIndex
Line OK ('physical : ' + $phys.Alias + '  ' + $phys.Ip + '  gateway ' + $phys.Gateway + '  if ' + $phys.IfIndex)
if ($tunnel) { Line OK ('tunnel   : ' + $tunnel.Alias + '  ' + $tunnel.Ip + '  if ' + $tunnel.IfIndex + '  (' + $tunnel.Covered + ' covering routes - anything not launched from here keeps using it)') }
else         { Line WARN 'no VPN tunnel detected - the proxy still works, but there is nothing to bypass' }
if (-not $phys.HasDefaultRoute) {
    Line WARN ('adapter ' + $phys.Alias + ' has no 0.0.0.0/0 route of its own - pinning sockets to it would fail')
}

Add-Type -TypeDefinition $cs -Language CSharp
[AppBypass]::IfIndex = $phys.IfIndex
[AppBypass]::Verbose = (-not $NoLog)
# Start-Transcript does not capture console output written by the compiled proxy,
# so when the script runs detached, request lines need a file of their own.
if (-not $RequestLog -and $Log) { $RequestLog = $Log + '.requests' }
if ($RequestLog) { $RequestLog = [System.IO.Path]::GetFullPath($RequestLog); [AppBypass]::LogPath = $RequestLog }

$err = [AppBypass]::Start($Port)
if ($err) {
    Line FAIL ('cannot listen on 127.0.0.1:' + $Port + ' - ' + $err)
    Line Info ('another port may be free:  .\app-bypass.ps1 -Port ' + ($Port + 1))
    exit 1
}
Line OK ('proxy    : 127.0.0.1:' + $Port + '   outbound sockets pinned to if ' + $phys.IfIndex + ' (' + $phys.Alias + ' -> ' + $phys.Gateway + ')')

if ($Test) {
    Write-Host ''
    Write-Host 'self-test  -  the same target over two paths' -ForegroundColor White
    $direct = Get-TraceDirect
    if ($direct.Ip) { Line OK ('normal socket    : ' + $direct.Ip + '   (source ' + $direct.LocalAddress + ')') }
    else            { Line WARN ('normal socket    : no answer - ' + $direct.Error) }
    $via = Get-TraceViaProxy -ProxyPort $Port
    if ($via.Ip)    { Line OK ('via this proxy   : ' + $via.Ip) }
    else            { Line FAIL ('via this proxy   : no answer - ' + $via.Error) }
    Write-Host ''
    if ($via.Ip -and $direct.Ip -and $via.Ip -ne $direct.Ip) {
        Line OK 'BYPASS WORKS - an app pointed at this proxy leaves on the real network while everything else stays in the VPN'
        Line Info 'now use it:  .\app-bypass.ps1 -Launch Chrome'
    } elseif ($via.Ip -and $via.Ip -eq $direct.Ip) {
        Line WARN 'both paths report the same address - either no VPN is up, or the pin did not take effect'
    } else {
        Line FAIL 'the proxy path failed - nothing on the machine was changed'
    }
    [AppBypass]::Stop()
    Write-Host ''
    Line Info ('proxy stopped - ' + [AppBypass]::Stats())
    exit 0
}
# ---------------------------------------------------------------- launch ---
$targetExe = $ExePath
if (-not $targetExe) {
    if     ($Launch -eq 'Chrome') { $targetExe = 'C:\Program Files\Google\Chrome\Application\chrome.exe' }
    elseif ($Launch -eq 'Edge')   { $targetExe = 'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe' }
    elseif ($Launch -eq 'Brave')  { $targetExe = 'C:\Program Files\BraveSoftware\Brave-Browser\Application\brave.exe' }
}
$proxyUrl = 'http://127.0.0.1:' + $Port
if ($targetExe) {
    if (-not (Test-Path $targetExe)) {
        Line FAIL ('not found: ' + $targetExe)
        Line Info 'pass -ExePath "<full path to the app>" instead'
    } else {
        # The proxy relays TCP only; without --disable-quic the browser could still
        # send HTTP/3 over UDP outside the proxy, and that traffic would use the VPN.
        $bargs = @('--proxy-server=' + $proxyUrl, '--disable-quic')
        if ($UserDataDir) {
            $bargs += ('--user-data-dir=' + $UserDataDir)
            $bargs += '--no-first-run'
            $bargs += '--no-default-browser-check'
        }
        # A running Chromium instance absorbs new launches of the same profile into
        # itself and ignores their command-line switches, including --proxy-server.
        $pname = [System.IO.Path]::GetFileNameWithoutExtension($targetExe)
        $running = @(Get-Process -Name $pname -ErrorAction SilentlyContinue)
        if ($running.Count -gt 0 -and -not $UserDataDir) {
            Line WARN ($pname + ' is already running (' + $running.Count + ' process(es)) - the flag is ignored')
            Line Info 'close every window of that browser first, or add -UserDataDir "<folder>"'
        }
        Start-Process -FilePath $targetExe -ArgumentList $bargs | Out-Null
        Line OK ('launched ' + $pname + ' with ' + $proxyUrl)
        Line Info 'this browser uses the real network; all other apps are unaffected and stay on the VPN'
    }
}

Write-Host ''
Write-Host 'other apps' -ForegroundColor White
Line Info ('point the app''s own proxy setting at 127.0.0.1:' + $Port)
Line Info 'games, voice clients and most installers ignore proxies - for those use per-destination'
Line Info 'exceptions instead:  .\split-tunnel.ps1 -Exclude <ip|host> -Apply'
Line Info 'do NOT set the Windows-wide proxy for this: it would move every other app off the VPN too'

Write-Host ''
Write-Host 'proxy running' -ForegroundColor White
if ($RunSeconds -gt 0) { Line Info ('stops by itself after ' + $RunSeconds + 's   (Ctrl+C stops it sooner)') }
else                   { Line Info 'Ctrl+C to stop' }
Line Info 'stopping only closes the port; the app falls back to the VPN by itself'

$until = $null
if ($RunSeconds -gt 0) { $until = (Get-Date).AddSeconds($RunSeconds) }
$beat = (Get-Date).AddSeconds(30)
try {
    while ($true) {
        if ($until -and (Get-Date) -ge $until) { Write-Host ''; Line Info 'run time reached - stopping'; break }
        Start-Sleep -Seconds 2
        if ((Get-Date) -ge $beat) {
            $beat = (Get-Date).AddSeconds(30)
            Write-Host ('  ' + (Get-Date).ToString('HH:mm:ss') + '  ' + [AppBypass]::Stats()) -ForegroundColor DarkGray
        }
    }
} finally {
    [AppBypass]::Stop()
    Write-Host ''
    Line OK ('proxy stopped - ' + [AppBypass]::Stats())
    Line Info 'nothing was changed on this machine: no route entry, no firewall rule, no registry key, no driver'
}

Write-Host ''
Write-Host ('summary: ' + $script:nOk + ' ok, ' + $script:nWarn + ' warnings, ' + $script:nBad + ' failures') -ForegroundColor Cyan
Write-Host ''
if ($Log) { try { Stop-Transcript | Out-Null } catch { } }





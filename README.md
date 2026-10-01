# xvpn

Split tunneling for full-tunnel VPNs on Windows.

Some VPN clients, X-VPN among them, send all traffic through the tunnel and offer no
way to leave individual apps out. xvpn adds that: choose an app, send it OUT, and
it uses your normal internet connection while every other app stays in the VPN.

It is a single portable exe. There is no installer, no driver and no service, and
it needs no administrator rights for per-app splitting.

## Features

- **Per-app split tunneling.** Send a browser OUT (real network) or keep it IN
  (VPN) with one click. A running browser is restarted on the new side and its tabs
  are restored.
- **Live view.** For every app with connections, xvpn shows whether they currently
  go through the tunnel, the real network or the bypass proxy.
- **Destination exceptions.** Send traffic for chosen IP addresses, networks or
  hostnames past the VPN for every app, using temporary routes that a reboot or Undo
  removes. This needs administrator rights.
- **Status at a glance.** VPN tunnel, physical adapter, gateway and the public IP
  address your traffic currently leaves from.

## Requirements

- Windows 10 or Windows 11, 64-bit
- Windows PowerShell 5.1, which ships with Windows
- A VPN that routes everything through its tunnel, for example X-VPN. xvpn detects
  the tunnel automatically and does not modify the VPN client.

## Getting started

1. Download `xvpn-ui.exe` from the Releases page and put it anywhere.
2. Start it. Windows SmartScreen may warn about an unrecognised app because the exe
   is not code-signed; choose "More info" and then "Run anyway".
3. Connect your VPN.
4. Open the **Apps** tab, select your browser and click **Send OUT**.

To go back, select the browser again and click **Keep IN**.

## How it works

Windows has no per-process routing for normal programs, so xvpn uses two techniques.

**Per-app: bypass proxy.** xvpn runs a small HTTP/HTTPS proxy on `127.0.0.1`. Its
outgoing connections are bound to the physical network adapter with the socket
option `IP_UNICAST_IF`, so they skip the VPN's routes. Names are resolved by asking that
adapter's own DNS servers, also over a pinned socket, so addresses that exist only on your
real network still resolve while the VPN is up. An app that uses the proxy
leaves on the real network; everything else is unaffected. Chromium-based browsers
(Chrome, Edge, Brave) are started with `--proxy-server` pointing at it. Other apps
can use it if they have a proxy setting: HTTP proxy `127.0.0.1`, port `49500`.

The adapter the proxy leaves on is chosen automatically: a real network card is preferred
and the choice is confirmed with a test connection before the proxy starts, so a virtual
adapter (ZeroTier, Hyper-V, VMware, ...) is not used as the way out. To set it by hand:
`app-bypass.ps1 -Underlay "<adapter name>"` (`Get-NetAdapter` lists the names).

**Per-destination: routes.** A full-tunnel VPN covers the internet with a few broad
routes. A more specific route through the physical adapter wins by longest-prefix
match, so traffic to that destination bypasses the tunnel for every app. The
routes are added to the active store only (not persistent), recorded in a state
file, and removed by Undo, which only touches routes it added.

The work is done by three PowerShell scripts that are embedded in the exe and
unpacked to `%LOCALAPPDATA%\xvpn` on start:

| Script | Purpose |
|---|---|
| `app-chooser.ps1` | IN/OUT choices, switching browsers, starting and stopping the proxy |
| `app-bypass.ps1` | The loopback bypass proxy |
| `split-tunnel.ps1` | Destination exceptions (routes) |

They can also be run on their own; each has built-in help, for example
`Get-Help "$env:LOCALAPPDATA\xvpn\app-chooser.ps1" -Full`.

## Using it

### Apps

The list shows every app with open connections, plus the installed browsers.

- **MODE** is what you chose: IN (VPN) or OUT (real network).
- **LIVE** is what the app's connections are doing right now: `IN`, `OUT proxy`,
  `OUT real`, `MIXED` (both) or `idle`.

Sending a browser OUT or IN restarts it, because a browser only reads its proxy
setting at start. Open tabs are reopened; unsaved form input on a page is lost.

If a browser extension controls the proxy setting (VPN and proxy extensions do),
it overrides the proxy xvpn sets. In that case xvpn opens a separate browser window
with its own clean profile, titled "OUT - real network", and your normal browser
stays in the VPN. Turn that extension off in `chrome://extensions` to use your
normal profile instead; xvpn switches back to it by itself.

### Proxy

Shows whether the bypass proxy is running, with buttons to start, stop and test it.
Test compares your public IP address with and without the proxy.

### Exceptions

Shows how many destination exceptions are installed. Installing and removing them
needs administrator rights, so the tab gives ready-to-copy commands to run in an
elevated PowerShell window (Terminal (Admin)):

```powershell
powershell -ExecutionPolicy Bypass -File "$env:LOCALAPPDATA\xvpn\split-tunnel.ps1" -Exclude 1.1.1.1,example.com -Apply
powershell -ExecutionPolicy Bypass -File "$env:LOCALAPPDATA\xvpn\split-tunnel.ps1" -Undo
```

Add `-DryRun` to preview a change without making it.

## Limitations

- Per-app splitting works for apps that support an HTTP proxy. Chromium-based
  browsers are handled automatically. Apps without proxy support (most games,
  voice chat, many installers) stay in the VPN; use destination exceptions for them.
- The bypass proxy handles TCP (HTTP and HTTPS). QUIC is turned off for browsers
  that are sent OUT so they fall back to TCP; other UDP traffic stays in the VPN.
- IPv4 only.
- If the VPN blocks traffic outside the tunnel (a kill switch), neither technique
  can work while that is on. The proxy test and `split-tunnel.ps1 -Probe` detect this.
- After a restart of Windows, the proxy is not running. Send the app OUT again. While the
  proxy is down, a browser that was sent OUT cannot load anything and reports
  `ERR_TUNNEL_CONNECTION_FAILED` for every address; the Apps tab shows a warning and a
  Start proxy button in that case.
- Where Group Policy forces a PowerShell execution policy, the embedded scripts may
  be blocked.
- A destination exception given as a hostname is resolved with the system resolver. With
  the VPN up that resolver answers from the tunnel's DNS, so a name that exists only on your
  own network has to be given as an address instead. The bypass proxy has no such limit: it
  asks the real adapter's own DNS servers.

## Files and removal

xvpn writes only to `%LOCALAPPDATA%\xvpn`:

| File | Content |
|---|---|
| `*.ps1` | The unpacked engine scripts |
| `apps.config.json` | Your IN/OUT choices |
| `_bypass.pid`, `_bypass*.log` | Proxy process id and logs |
| `split-tunnel.state.json`, `routes-before.json` | Installed exceptions and a route snapshot |
| `chrome-out\` (and similar) | Separate browser profile, only if one was needed |

To remove xvpn, send your apps IN, run Undo if you installed exceptions, close the
window, then delete the exe and `%LOCALAPPDATA%\xvpn`.

## Building from source

Requires Visual Studio 2022 or the Build Tools for Visual Studio 2022 with the
"Desktop development with C++" workload.

```bat
build.cmd
```

The result is `build\xvpn-ui.exe`. `build.cmd` finds MSVC with `vswhere`; set
`VCVARS` to the path of `vcvars64.bat` to use a specific toolchain.

Project layout:

```
src/            C++ source (Win32, Direct3D 11, Dear ImGui)
engine/         PowerShell engine scripts, embedded into the exe
assets/         Icon
third_party/    Dear ImGui 1.92.9b (Win32 and DX11 backends only)
build.cmd       Build script
```

## Privacy and responsible use

Traffic you send OUT leaves from your real IP address and is visible to your local
network, your internet provider and the sites you visit, exactly as without a VPN.
That is the purpose of split tunneling, and also its risk.

Only use xvpn on networks and devices you are allowed to configure, and follow the
rules of your network and the laws that apply to you.

## Disclaimer

xvpn is an independent project. It is not affiliated with, endorsed by or supported
by X-VPN or its publisher, or by any other VPN provider. X-VPN and other product
names are trademarks of their respective owners and are used only to describe
compatibility.

The software is provided "as is", without warranty of any kind. See [LICENSE](LICENSE).

## License

MIT, see [LICENSE](LICENSE).

Dear ImGui is copyright (c) Omar Cornut and licensed under the MIT license, see
[third_party/imgui/LICENSE.txt](third_party/imgui/LICENSE.txt). The Segoe UI,
Segoe Fluent Icons and Cascadia Mono fonts are loaded from Windows at runtime and
are not distributed with xvpn.

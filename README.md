# BUSMUST INCA J2534 Support Package

[简体中文](README.zh-CN.md)

For illustrated Chinese instructions, open the [BUSMUST INCA connection guide](先看这里_BUSMUST设备接入INCA指南.pdf).

Connect local BUSMUST CAN / CAN FD ports to a Windows J2534 client. The package
provides J2534 04.04 entry points, a configuration dialog and its matching BMAPI
runtime. Install the normal BUSMUST USB driver separately. An INCA installation
and the licences for the INCA features you use remain your responsibility.

## Download

Download **`busmust-inca-support-package-v1.0.0.1-windows.zip`** from
[GitHub Releases](https://github.com/busmust/busmust-inca-support-package/releases/latest).
This is the ready-to-install package for Windows x86/x64; the automatically
created **Source code** archives are for developers and do not contain the runtime.
The package includes the illustrated Chinese guide, both READMEs and runtime licences.
Source mirror: [Gitee](https://gitee.com/busmust/busmust-inca-support-package).

## Install once

1. Extract the complete customer ZIP to a local directory. Fully close INCA and its TgtSvr.
2. Double-click `Install.cmd` and accept the Windows administrator prompt.
   No connected hardware is required for installation.
3. Start INCA using its original shortcut. Add a J2534 system and select
   **BUSMUST - BMJ2534**. Keep the API version at **04.04**.
4. Use INCA's external device configuration button to open
   **BUSMUST J2534 Device Configuration**. Use **Auto** for exactly one available
   physical CAN/CAN FD channel, or **Manual** to select one complete channel name,
   for example `BM-USB-XL4(10356) CH2`.
5. Choose **Off** or **120 Ω** termination to match your physical bus. **Save**
   closes the dialog. The selection takes effect the next time the hardware is opened.

You can also open the configuration dialog from the Windows Start menu under
**BUSMUST**. Do not enable an additional terminator if the bus is already correctly
terminated. Mode and termination are saved per Windows user; Manual also saves
the exact UID/physical port.

The installation is stored in `%ProgramW6432%\BUSMUST\INCA-J2534`. The downloaded
ZIP and extracted directory can be deleted after a successful installation. No
INCA executable, ETAS DLL, ETAS global setting or system PATH is changed.

## New devices, multiple devices and channel changes

Installation registers one generic provider in each registry architecture view.
It does not register a DLL per device or channel. After connecting a new device,
open the configuration dialog, select its port, save and reinitialise
INCA. Reinstallation is unnecessary for models supported by the installed BMAPI
and USB driver. A later new hardware model can require a driver/BMAPI update.
The channel list follows BMAPI's complete names and ordering without splitting
devices and ports into linked lists. It refreshes on Windows device notifications;
use **Refresh** if needed. Software and selected-channel firmware versions share
one row. A missing saved channel is marked unavailable and cannot be saved; it
is never silently replaced. Selecting a valid channel clears that state.
The dialog uses BMAPI names
and stable UID/port identities, not USB enumeration order. LIN, remote and virtual
channels are excluded. The interface follows INCA's saved Chinese or English
language preference; other languages use English.

Without configuration, the default is **Auto with termination Off**. Every Open
resolves Auto once: exactly one available local physical CAN/CAN FD channel is
used; zero fails as disconnected and more than one requires Manual selection.
One four-channel device counts as four; two single-channel devices count as two.
Auto never saves the resolved UID, so after Close you can replace single-channel
device A with B and open again without configuration. Auto can save its own
Off/120-ohm choice. An active connection never hot-switches to a replacement.

**Manual** saves one exact UID/port and fails if it is absent, even if another
unique device is available. Only Save of a different Manual choice or a return to
Auto changes that binding. Old exact-UID settings are read as Manual; corrupt
settings fail rather than silently enabling Auto. An explicit client selector
overrides the user's default mode. Stop/reinitialise hardware before switching.
Different Windows users configure their own selection.

Configure the port before initialising hardware. When already connected, release
INCA's hardware connection before opening Config; occupied channels may otherwise
be absent. The following sequence was verified on INCA 7.4.3 without exiting INCA:

1. Stop measurement, save the experiment and return to the database window (`Ctrl+F7` from hardware configuration).
2. Once, create an empty workspace with `Edit → Add → Workspace`; add no hardware and reuse it later.
3. Select that empty workspace, activate it with `Ctrl+K`, confirm closing the old editors, then **open its hardware configuration with `Ctrl+F3`**.
4. Open Config from the BUSMUST Start-menu folder, select the channel, change settings and save.
5. Return to the original workspace, use `Ctrl+K`, `Ctrl+F3`, then `F3` to initialise and run its experiment.

Stopping measurement or merely activating the empty workspace does not complete
the release. The included Chinese PDF illustrates the sequence on page 4.
Installation, updates and removal still require normal exit of INCA and other clients.

This is a per-user default for subsequent connections, not a separate binding
stored in each INCA workspace. Set nominal/data bit rates and other communication
parameters in INCA's hardware configuration; the external dialog does not write
those workspace parameters.

The supported range is **INCA 7.X.X**. The verified environment records are listed
separately below; an unlisted combination is not claimed as tested.

| INCA version | Verified operating system |
|---|---|
| 7.3.7 | Windows 10 |
| 7.4.3 | Windows 10 |
| 7.4.3 | Windows 11 |

This package does not claim native ES582 compatibility or simultaneous physical
ports in one INCA experiment. A standard client can select an exact UID/port
explicitly; its own concurrency limits still apply. Validation results must refer
to the exact candidate's evidence, not merely to this README.

## Features and boundaries

* Classic CAN and CAN FD monitoring; ISO-TP for Classic CAN and CAN FD.
* INCA XCP and UDS use the application's corresponding features and licences.
  UDS in ODX-LINK requires the appropriate ETAS licence and a consistent project.
* CAN FD extensions include SAE `FD_CAN_PS` / `FD_ISO15765_PS` and GM
  `CAN_FD_PS` / `ISO15765_FD_PS`. GM refers to General Motors' extension namespace;
  it is not a new BUSMUST company or product. This package does not include SAE
  or GM standards publications or claim their certification.
* Raw CAN FD uses legal payload sizes up to 64 bytes. ISO-TP handles segmented
  requests and responses within the declared J2534 message-buffer limit.
* A Classic-only physical port rejects FD protocols. Unsupported protocols,
  voltages and optional services return errors rather than simulated success.
* Default sampling is **75% CAN / nominal** and **80% CAN FD data**. Under
  Sampling point, choose Custom for selected channel to enter integer percentages
  (1..100). Default disables the inputs. Classic CAN uses only CAN / nominal;
  data applies only to CAN FD and is disabled for Classic-only hardware.
* Saved custom values bind to that exact Manual channel and are read at the next
  hardware Open, without changing an active connection. CAN starts with this
  value and still accepts later INCA / standard `BIT_SAMPLE_POINT` changes.
  CAN FD uses both Config values. Existing FD-only custom records retain their
  scope until explicitly saved again in Config.
* Hardware rejection or readback outside tolerance returns an error and restores
  the previous settings; failed recovery requires Close/Open. INCA 7.4.3's original
  FD sample-point display is not synchronised with Config and does not confirm
  applied hardware values. For troubleshooting, `CONFIG_APPLIED` contains requested
  and actual values; `CONFIG_REJECTED` records failures. Routine configuration
  does not require reading logs. FD `BIT_SAMPLE_POINT` remains unsupported;
  no private parameter is presented as a standard one.
* Raw CAN/CAN FD periodic and ordinary writes share arrival order. Each periodic message has at
  most one pending job. Overload skips missed cycles instead of accumulating or
  catching up an unbounded backlog, so the actual interval can increase.
* Bus-Off recovery is bounded per port. Persistent failure requires reconnecting
  that channel; it does not endlessly resend requests or close other ports.
* On firmware that rejects directional TX clearing, the provider cancels its
  software queue locally. A frame already submitted to old hardware can remain;
  a successful software clear is not proof of physical withdrawal of that frame.

## Remove, update and troubleshooting

Close INCA and any J2534 clients. Use **Uninstall BUSMUST J2534** in the Start menu
or the installed `Uninstall.cmd`. Only this installation's provider registrations,
known shortcuts and unchanged owned files are removed. Modified/extra files are
preserved and reported. Keep that report if removal is incomplete. An update
refuses to overwrite modified or unowned files; preserve them before resolving
that conflict. An interrupted installation can be retried with the same package.
Double-click the uninstall wrapper as a standalone console; after confirmation it
closes that command process. For automation or an existing command session, use
the PowerShell management command below instead of calling the batch wrapper.

If INCA cannot open the port, close other applications holding it, reconnect the
intended device, refresh the dialog and confirm its model/SN, channel and bus parameters.
Saving a selection does not initialise an already open INCA connection. A licence
warning is an INCA feature issue; it does not grant or remove a BM device licence.
Inspect INCA's monitor log after a test and retain new warnings/errors.

Advanced administrators may use Windows PowerShell 5.1 or PowerShell 7:

```powershell
.\Manage-SupportPackage.ps1 -Action Plan -InstallRoot 'D:\Tools\BUSMUST-J2534'
.\Manage-SupportPackage.ps1 -Action Install -InstallRoot 'D:\Tools\BUSMUST-J2534'
.\Manage-SupportPackage.ps1 -Action Uninstall -InstallRoot 'D:\Tools\BUSMUST-J2534'
```

Use a dedicated directory. Reparse targets, parent traversal and foreign
installation ownership are rejected. Administrators should use the same root
for updates. Installing into another root does not take ownership of an existing
provider from the first root.

## Field diagnostics

By default, diagnostics are written under the current user's
`%LOCALAPPDATA%\BUSMUST\J2534\Logs`. Retain matching `BMJ2534-*.jsonl`, `.1`
and `.2` files with the INCA version, monitor log and reproduction steps.
Records identify the runtime/BMAPI hashes, UID/port, model/SN/firmware, applied
configuration, connection/close results, errors and low-frequency counters.
They do not dump message payloads, but can include user paths and device IDs.

A background writer uses a 128-record queue and 128-record/second budget.
Files rotate at 4 MiB with two backups; the latest eight sessions are normally
retained without deleting active files. Overflow is summarized. At most two
writers may be active/retiring; rapid reopen attempts may have only a skipped
count. These bounded diagnostics are not a complete capture: process failure,
I/O failure, rotation and throttling can lose records. Normal close requests an
asynchronous drain without waiting for disk. An unwritable directory does not
change communication results or redirect logs into the INCA installation.

Support may disable logging with DWORD `FileLogEnabled=0` in the 64-bit view of
`HKCU\Software\BUSMUST\J2534`; delete it or set it to 1 to restore the default.
Changes apply on the next device open. This changes only our own configuration.
Stopping INCA measurement is not necessarily a hardware Disconnect/Close;
closing our configuration dialog does not close an active channel.

## Source, licences and verification

[LICENSE](LICENSE) is Apache-2.0 for BUSMUST-authored source, scripts and documents.
[NOTICE](NOTICE) and [THIRD_PARTY.md](THIRD_PARTY.md) identify retained upstream terms.
The supplied `BMAPI.dll` retains its own SDK licence under `licenses/BMAPI/`.
ETAS software, licences and third-party standards are
not redistributed. No ETAS binary modification is required.

Developers: `Build-Native.ps1` consumes external BMAPI headers. The independent
human-maintained authority is `native/J2534/BMJ2534.Version.props`; no build or
dependency automatically increments it. PE and J2534 DLL versions use that value;
actual firmware, API `04.04` and BMAPI Runtime are reported separately.
See [public regression](tests/README.md) for the independently buildable mock tests.
`Build-SupportPackage.ps1` assembles a candidate
from matching runtime and legal inputs and writes `payload.json` SHA-256 hashes.
Product version selection, signing and release publication remain release-owner
steps. `Export-PublicSource.ps1` exports only the explicit public-source allowlist;
it excludes internal workflow records, evidence, binaries and account data.

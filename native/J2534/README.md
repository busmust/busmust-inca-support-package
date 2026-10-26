# BUSMUST Windows J2534 implementation

Build the three Release Win32/x64 projects through the root `Build-Native.ps1`.
Supply external maintained BMAPI SDK headers.
The repository does not contain a BMAPI implementation or modify ETAS binaries.
`BMJ2534.Version.props` sets the PE and J2534 DLL product versions.
Firmware, API and BMAPI Runtime versions are reported separately.

Each architecture's runtime consists of the core (`BMJ25332.dll` or
`BMJ2534.dll`), generic `BMJ2534Endpoint.dll`, `BMJ2534Config.exe`, and adjacent
matching `BMAPI.dll`. The backend resolves that companion by absolute path.
The core exposes standard one-byte packed J2534 04.04 structures. The x64 endpoint
adapts the verified INCA backend's outer pointer lists; it does not alter message
or element layout, ETAS global packing, or the standard core ABI.

Register only through the customer installer, which passes its persisted
installation ownership ID. Registry views contain one `BUSMUST - BMJ2534` key
each. `ConfigApplication` points to the installed configuration program. Device
selection is one atomic versioned Auto/Manual record per Windows user, including
termination and the exact Manual UID/port. Explicit selectors take priority.
Auto resolves a unique eligible physical channel on every Open and never stores
its UID; an active connection cannot hot-switch. Zero/many candidates fail.
Manual absence never substitutes; corrupt records return an error.
See `../../tests/README.md` for public mock regression.

`BMJ2534Config.exe --enumerate-json` is read-only discovery over the same growing
BMAPI enumeration buffer and local CAN/CAN FD capability filter. It opens no
channel, changes no bus configuration and writes no selection. The normal dialog
provides one complete channel list in BMAPI order, device-notification refresh
with a manual fallback, firmware version readback, Off/120-ohm termination,
sampling-point settings, Save and Cancel. Names follow BMAPI; full UID/port
identities bind saved selections. Custom sampling binds to the selected Manual
channel: CAN uses the nominal value; CAN FD uses both nominal and data values.
Defaults are 75% nominal and 80% data. Chinese
and English resources follow the saved ETAS language preference without changing
it. Nominal/data rates are configured in the INCA
workspace; this dialog does not implement workspace parameter synchronization.

The implementation supports raw Classic/FD and Classic/FD ISO-TP, filters,
periodic messages, bounded RX/TX queues and the declared SAE/GM FD namespaces.
Unsupported services return errors. Standard HS_CAN_TERMINATION uses 0/3; GM's
separate parameter uses 0/1. A failed native control query is distinct from valid
Bus-Off status. Recovery uses BM_RecoverBusOff once per fault episode/port and
requires positive transmit confirmation before rearming. Persistent failure
rejects new writes until reconnect. Old-firmware ILLOPERATION from directional
clear permits local cancellation; other errors propagate. Disconnect still runs
full clear and CAN-off, even when directional TX clear is unsupported.

See the root README files for the customer workflow and boundaries, and
../../THIRD_PARTY.md plus third_party/ for retained upstream sources and terms.

The private diagnostic sink is independent of TraceEnabled/OutputDebugString.
Live devices retain a session; its last owner requests a bounded asynchronous
drain, with a module reference held only by that writer. Producer calls do not
write files or wait for its queue lock. Default polling and payloads are omitted;
channel health appears every 30 seconds and on normal stop. See the root README
for path, retention, rate limits, privacy and diagnostic visibility limitations.
The writer uses a 128-record queue and a 128-record/second budget; overflow is
summarized. At most two writers may be active or retiring. Logs rotate at 4 MiB
with two backups and normally retain the latest eight sessions without deleting
active files. Unwritable storage does not change communication results.

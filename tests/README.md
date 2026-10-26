# Public native regression

This is a software test double and adapter regression, not BMAPI's implementation
and not a substitute for hardware, INCA or firmware acceptance. It contains no
USB calls. The probe refuses a DLL without the explicit mock control exports
before loading the provider. Its registry writes use a process-local HKCU
override into a temporary test key; the real user's selection is not changed.
Diagnostic files remain under the chosen output directory.

Prerequisites: Windows, PowerShell 5.1 or later, Visual Studio MSBuild with the
v140_xp C++ tools and Windows SDK, and matching external BMAPI SDK headers
(`bmapi.h`, `bm_usb_def.h`). A BMAPI binary, USB driver, hardware, INCA, Python,
private credentials and internal evidence are not required.

From the exported source root:

```powershell
.\tests\Run-Regression.ps1 -BMAPIIncludeDirectory C:\SDK\BMAPI\include -OutputDirectory C:\Build\BMJ2534-regression
```

Choose an empty directory outside the checkout. `-MSBuildPath` is optional.
The script builds Release Win32/x64 core, endpoint and Config, then a test-only
`BMAPI.dll` and native probe per architecture. It runs both probes with no visible
console, retains build/behavior logs and fails on the first violated assertion.
Do not copy the test double into an installed product directory or customer ZIP.

Coverage: persistent Auto/Manual and legacy/corrupt selections, zero/unique/many
physical channels, virtual/remote/LIN exclusion, replacement on Close/Open only,
explicit override, concurrent Open, standard termination 0/3, bounded Bus-Off
recovery, old directional-clear fallback versus real errors, disconnect cleanup,
large/reordered device inventories, all 14 core/endpoint exports, PE versions,
actual mock firmware / independent product / 04.04 API version fields.
Config UI, log retention/performance and real transport have separate acceptance.

BMJ2534's human-maintained authority is
`native/J2534/BMJ2534.Version.props`. Tests read it; they never increment it.

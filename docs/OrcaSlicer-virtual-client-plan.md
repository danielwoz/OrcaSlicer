# OrcaSlicer virtual-printer client — plan

## Goal

Add support to OrcaSlicer for **connecting to virtual Bambu printers**
exposed by a Bambu Bridge running elsewhere on the LAN.

A virtual printer is a `FFFF…`-prefixed serial-number facade the bridge
mints in front of a real Bambu printer it owns. From OrcaSlicer's
perspective the virtual printer behaves like a LAN-mode Bambu printer:
SSDP discovery, MQTT over TLS, FTPS uploads, RTSP live-view, vtun
storage tunnel.

## Hard requirement

**Zero dependency on `libbambu_networking.so` (the proprietary network
plugin) for the virtual-printer flow.** OrcaSlicer must be able to
discover, connect to, and operate a virtual printer with the plugin
absent. Real (non-virtual) Bambu printers continue to need the plugin
unchanged — the virtual path is a parallel route that bypasses it.

## What's *not* in scope

- `--bridge-only` mode. The bridge runs in BambuStudio-bridge (or
  the future stand-alone daemon); not in OrcaSlicer.
- The `src/bambu_bridge/` static library. Lives only in
  BambuStudio-bridge.
- `BridgeApp`, `BridgeStorageBackend`, `BridgeOnly*`,
  `NetworkAgentPluginAdapter`. All server-side.
- The full BambuStudio `MediaUrlBuilder` URL ladder — OrcaSlicer's
  ladder is simpler; we add a thin variant for FFFF dev-ids only.

## What's in scope

### New slicer-side source (ports from `BambuStudio-bridge bridge-necessary`)

- `src/slic3r/Utils/VirtualMqttClient.{cpp,hpp}` — TLS MQTT 3.1.1
  client for FFFF dev-ids. `verify=false` so the bridge's self-signed
  cert is accepted. One session per virtual dev-id with its own I/O
  thread; reconnect on socket loss.
- `src/slic3r/Utils/VirtualMqttCli.cpp` — dispatch wrapper used by
  `NetworkAgent` for `send_message_to_printer` of FFFF dev-ids.
- `src/slic3r/Utils/VirtualFtpsClient.{cpp,hpp}` — FTPS upload client
  for FFFF dev-ids (G-code uploads land here instead of in the plugin).
- `src/slic3r/Utils/VirtualLanPrinterStore.{cpp,hpp}` — persists the
  discovered FFFF entries between launches so the user doesn't
  re-discover after every restart.
- `src/slic3r/GUI/Printer/VirtualBambuTunnel.{cpp,hpp}` — TLS
  `Bambu_Tunnel*` look-alike for FFFF dev-ids, used by
  `PrinterFileSystem` for storage tunnel.
- `src/slic3r/GUI/Printer/MediaUrlBuilder.{cpp,hpp}` — thin URL helper
  that returns the LAN `bambu:///local/<lan_ip>:<port>` URL for FFFF
  dev-ids (skips OrcaSlicer's TUTK / Agora ladder which doesn't apply).

### Slicer-side touchpoints (edits to upstream files)

- `src/slic3r/Utils/NetworkAgent.{cpp,hpp}`: add `is_virtual_dev_id`
  (FFFF-prefix check). In `connect_printer` /
  `send_message_to_printer` / `disconnect_printer`, branch on the
  predicate — virtual → `VirtualMqttClient`, real → plugin (existing
  path). Plugin path is a no-op when the plugin isn't loaded — no
  change to error semantics, virtual path is fully independent.
- `src/slic3r/GUI/DeviceManager.cpp`: in `parse_json`, skip
  `net.info[].ip` overwrite of `dev_ip` for virtual dev-ids (the
  cloud-pushed real-printer IP would clobber the bridge's IP we want
  to keep).
- `src/slic3r/GUI/Printer/PrinterFileSystem.{cpp,h}`: instantiate
  `VirtualBambuTunnel` instead of `Bambu_Tunnel*` when the dev-id is
  FFFF.
- `src/slic3r/GUI/MediaFilePanel.cpp` + `MediaPlayCtrl.cpp`: short
  branch that calls `MediaUrlBuilder` for FFFF dev-ids.

### **New** (was not in BambuStudio-bridge): slicer-side SSDP listener

- `src/slic3r/Utils/VirtualSsdpDiscovery.{cpp,hpp}` — `boost::asio`
  UDP listener on the SSDP port (`239.255.255.250:1900`). Filters
  for `urn:bambulab-com:device:3dprinter:1` services and `USN: FFFF…`
  identifiers. Parses model/name/version. Hands discovered entries
  to `DeviceManager` (additions to `localMachineList`) on the wx main
  thread via `CallAfter`.
- Idiom modeled after `src/slic3r/Utils/Bonjour.cpp` (mDNS) which is
  already cross-platform via Asio.
- M-SEARCH on startup + periodic re-probe (every 60 s) to handle
  printers that come online late or whose initial NOTIFY missed.
- Persists discoveries via `VirtualLanPrinterStore` so the second
  launch hydrates before the listener catches them.

### CMake

- Add the new sources unconditionally (no BAMBU_BRIDGE define). This
  is a normal OrcaSlicer feature, gated only by runtime detection
  (FFFF prefix on dev-id).
- No new linker deps — `boost::asio` and OpenSSL are already linked.
- No new external libraries.

## Architecture diagram

```
                       LAN
   ┌─────────────────────────────────────────┐
   │                                         │
   │     BambuStudio-bridge --bridge-only    │  (separate machine /
   │     ↳ libbambu_networking.so loaded     │   container; talks to
   │     ↳ talks to real Bambu printers      │   real printers via cloud
   │     ↳ advertises FFFF… via SSDP         │   + plugin)
   │     ↳ MQTT/FTPS/RTSP/vtun on port-base+N│
   │                                         │
   │                  ▲                      │
   │                  │                      │
   │     SSDP (UDP/1900)                     │
   │                  │                      │
   │     ┌────────────┴────────────────┐    │
   │     │  OrcaSlicer (NO PLUGIN)      │   │
   │     │                              │   │
   │     │  VirtualSsdpDiscovery        │   │
   │     │     ↳ FFFF entries           │   │
   │     │       into DeviceManager     │   │
   │     │                              │   │
   │     │  NetworkAgent                │   │
   │     │     ↳ is_virtual_dev_id?     │   │
   │     │        ├ yes → VirtualMqttClient  │
   │     │        └ no  → libbambu_networking│  (absent → no-op)
   │     │                              │   │
   │     │  PrinterFileSystem           │   │
   │     │     ↳ FFFF → VirtualBambuTunnel  │
   │     │                              │   │
   │     │  MediaFilePanel/MediaPlayCtrl│   │
   │     │     ↳ FFFF → MediaUrlBuilder │   │
   │     └──────────────────────────────┘   │
   │                                         │
   └─────────────────────────────────────────┘
```

## Validation

1. Vanilla OrcaSlicer build succeeds — no regressions on the existing
   plugin path. (Tested with plugin present.)
2. With `libbambu_networking.so` removed from `~/.config/OrcaSlicer/`,
   OrcaSlicer launches, finds FFFF entries via SSDP, connects, sees
   push_status, opens storage tunnel, uploads G-code, plays
   live-view.
3. The existing `tests/e2e/bridge_integration.py` script (in
   BambuStudio-bridge) still passes 7/7 against a bridge that
   OrcaSlicer is consuming concurrently — the bridge has no idea
   which client is which.

## Phases

| # | Work | Status |
| - | --- | --- |
| A | Plan doc (this file) | ☑ done |
| B | Copy client-side sources from BambuStudio-bridge | ☐ |
| C | Implement `VirtualSsdpDiscovery` | ☐ |
| D | Touchpoints in `NetworkAgent` / `DeviceManager` / `PrinterFileSystem` / `MediaFilePanel` / `MediaPlayCtrl` | ☐ |
| E | CMake glue | ☐ |
| F | Build clean | ☐ |
| G | Runtime test against external bridge | ☐ |
| H | Runtime test with `libbambu_networking.so` removed | ☐ |

## Notes

- The bridge-side server-side experimental work lives at
  `refs/archive/orcaslicer-bridge-server-side` (commit `16234717`) for
  reference. Not on any branch.
- `docs/OrcaSlicer-bridge-plan.md` and `OrcaSlicer-bridge-audit.md`
  were from the bridge-server experiment and are now obsolete; the
  hard reset removed them.

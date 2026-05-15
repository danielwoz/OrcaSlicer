#ifndef SLIC3R_GUI_PRINTER_MEDIA_URL_BUILDER_HPP
#define SLIC3R_GUI_PRINTER_MEDIA_URL_BUILDER_HPP

// Tiny URL helper for virtual (FFFF-prefixed) Bambu printers served by a
// Bambu Bridge. For real printers, the existing MediaFilePanel /
// MediaPlayCtrl URL logic still applies; we only branch off into this
// helper when the dev_id is virtual.
//
// Virtual storage URLs use a custom scheme (`bambu:///virtual/...`)
// that PrinterFileSystem's dispatcher (see VirtualBambuTunnel.cpp +
// PrinterFileSystem::StaticBambuLib::get) routes to a TLS connection
// against the bridge's vtun server. The bridge auto-handles the
// `bambu:///local/...` live-view URL the same way a real LAN-mode
// Bambu printer would.

#include <string>

namespace Slic3r {

class MachineObject;

namespace GUI {

// Build `bambu:///virtual/<bridge_ip>:<vtun_port>?dev_id=...&access_code=...`
// for the Storage tab. Returns empty string when `mo` is null or has
// no LAN IP yet (caller should wait for SSDP to populate it).
std::string build_virtual_storage_url(MachineObject* mo);

// Build `bambu:///local/<bridge_ip>.?port=6000&user=bblp&passwd=...`
// for live view. Same gating as the storage URL.
std::string build_virtual_live_url(MachineObject* mo);

} // namespace GUI
} // namespace Slic3r

#endif // SLIC3R_GUI_PRINTER_MEDIA_URL_BUILDER_HPP

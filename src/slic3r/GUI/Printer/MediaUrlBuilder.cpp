// Thin URL helper for virtual (FFFF-prefix) Bambu printers. See header
// for design notes. Anything beyond FFFF is handled by MediaFilePanel
// / MediaPlayCtrl using their existing logic.

#include "MediaUrlBuilder.hpp"

#include "../DeviceManager.hpp"     // full definition of MachineObject
#include "../DeviceCore/DevManager.h"
#include "../../Utils/NetworkAgent.hpp"
#include "../../Utils/bambu_virtual_client/VirtualLanPrinterStore.hpp"

#include <string>

namespace Slic3r {
namespace GUI {

namespace {
// Bridge's per-device port bases. Keep in sync with BridgeAppConfig
// defaults in BambuStudio-bridge (mqtt=8883, rtsp=38322, vtun=39998).
// Multi-process bridge gives each child the same base + offset 0 (one
// device per child), but the bridge-wide base values are constant.
constexpr uint16_t kBridgeMqttPort = 8883;
constexpr uint16_t kBridgeRtspPort = 38322;
constexpr uint16_t kBridgeVtunPort = 39998;
} // namespace

std::string build_virtual_storage_url(MachineObject* mo) {
    if (!mo) return {};
    const std::string dev_id = mo->get_dev_id();
    if (!NetworkAgent::is_virtual_dev_id(dev_id)) return {};
    const std::string lan_ip = mo->get_dev_ip();
    if (lan_ip.empty()) return {};
    const std::string passwd = mo->get_access_code();
    return "bambu:///virtual/" + lan_ip + ":" +
           std::to_string(kBridgeVtunPort) +
           "?dev_id=" + dev_id +
           "&access_code=" + passwd;
}

std::string build_virtual_live_url(MachineObject* mo) {
    if (!mo) return {};
    const std::string dev_id = mo->get_dev_id();
    if (!NetworkAgent::is_virtual_dev_id(dev_id)) return {};
    const std::string lan_ip = mo->get_dev_ip();
    if (lan_ip.empty()) return {};
    const std::string passwd = mo->get_access_code();

    // Pre-bridge code returned a `bambu:///local/<ip>.?port=6000…` URL —
    // that's the proprietary BambuPipe local-stream protocol on port
    // 6000, which the bridge doesn't speak. The slicer's MediaPlayCtrl
    // load thread bails inside 10 ms when the connect fails.
    //
    // Bridge serves the LAN camera over RTSPS instead, on rtsp_port (per
    // device, multi-process: kBridgeRtspPort + (mqtt_port - kBridgeMqttPort)).
    // The `bambu:///rtsps___user:pw@host/streaming/live/1?proto=rtsps`
    // scheme is the slicer's pre-existing handler for LAN-mode RTSPS,
    // so we can reuse it verbatim — see MediaPlayCtrl.cpp:321.
    uint16_t rtsp_port = kBridgeRtspPort; // safe fallback: child #0
    Slic3r::VirtualLanPrinterStore store;
    for (const auto& e : store.load()) {
        if (e.dev_id == dev_id && e.mqtt_port != 0) {
            const int delta = int(e.mqtt_port) - int(kBridgeMqttPort);
            rtsp_port = static_cast<uint16_t>(int(kBridgeRtspPort) + delta);
            break;
        }
    }
    return "bambu:///rtsps___bblp:" + passwd + "@" + lan_ip + ":" +
           std::to_string(rtsp_port) +
           "/streaming/live/1?proto=rtsps";
}

} // namespace GUI
} // namespace Slic3r

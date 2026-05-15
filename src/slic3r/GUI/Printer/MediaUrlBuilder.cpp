// Thin URL helper for virtual (FFFF-prefix) Bambu printers. See header
// for design notes. Anything beyond FFFF is handled by MediaFilePanel
// / MediaPlayCtrl using their existing logic.

#include "MediaUrlBuilder.hpp"

#include "../DeviceManager.hpp"     // full definition of MachineObject
#include "../DeviceCore/DevManager.h"
#include "../../Utils/NetworkAgent.hpp"

#include <string>

namespace Slic3r {
namespace GUI {

namespace {
// Bridge's per-device vtun port. Keep in sync with BridgeAppConfig
// in BambuStudio-bridge.
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
    return "bambu:///local/" + lan_ip + ".?port=6000&user=bblp&passwd=" + passwd +
           "&device=" + dev_id;
}

} // namespace GUI
} // namespace Slic3r

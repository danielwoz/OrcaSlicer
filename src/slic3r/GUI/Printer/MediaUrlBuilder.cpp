// Thin URL helper for virtual (FFFF-prefix) Bambu printers. See header
// for design notes. Anything beyond FFFF is handled by MediaFilePanel
// / MediaPlayCtrl using their existing logic.

#include "MediaUrlBuilder.hpp"

#include "../DeviceManager.hpp"     // full definition of MachineObject
#include "../DeviceCore/DevManager.h"
#include "../../Utils/NetworkAgent.hpp"
#include "../../Utils/bambu_virtual_client/StructuredLog.hpp"          // BBL_LOG
#include "../../Utils/bambu_virtual_client/VirtualLanPrinterStore.hpp"
#include "../../Utils/bambu_virtual_client/VirtualSsdpDiscovery.hpp"  // per-printer port resolver

#include <boost/asio.hpp>
#include <chrono>
#include <cstring>
#include <string>

namespace Slic3r {
namespace GUI {

namespace {
// Bridge's per-device port bases (keep in sync with BridgeAppConfig in
// BambuStudio-bridge: mqtt=8883, rtsp=38322, vtun=39998). Each printer
// gets base+index; multi-process gives each child base+offset. So the
// camera RTSP port is rtsp_base + (mqtt_port - mqtt_base).
constexpr uint16_t kBridgeMqttPort = 8883;
constexpr uint16_t kBridgeRtspPort = 38322;
constexpr uint16_t kBridgeVtunPort = 39998;

// Detect whether the bridge serves the camera as plain RTSP or RTSPS (the
// bridge chooses via BAMBU_BRIDGE_RTSP_TLS). We probe the port directly so
// the slicer needs no matching flag: open the socket, send a plaintext RTSP
// OPTIONS, and look at the reply. A plain-RTSP server answers "RTSP/..."; a
// TLS server treats the plaintext as a bad handshake (alert/close/no reply),
// so anything that isn't an "RTSP/" reply means rtsps. Short, bounded
// timeouts; defaults to plain rtsp on any error.
const char* probe_rtsp_scheme(const std::string& host, uint16_t port) {
    namespace ba = boost::asio;
    using namespace std::chrono;
    const char* scheme = "rtsp";   // safe default
    try {
        ba::io_context io;
        ba::ip::tcp::socket sock(io);
        boost::system::error_code cec = ba::error::would_block;
        sock.async_connect(
            ba::ip::tcp::endpoint(ba::ip::make_address(host), port),
            [&](const boost::system::error_code& e) { cec = e; });
        io.run_for(milliseconds(500));
        if (cec || !sock.is_open()) return nullptr; // Signal failure

        const std::string req =
            "OPTIONS rtsp://" + host + "/streaming/live/1 RTSP/1.0\r\nCSeq: 1\r\n\r\n";
        boost::system::error_code wec;
        ba::write(sock, ba::buffer(req), wec);

        io.restart();
        char buf[16] = {0};
        std::size_t got = 0;
        sock.async_read_some(ba::buffer(buf),
            [&](const boost::system::error_code& e, std::size_t n) { if (!e) got = n; });
        io.run_for(milliseconds(500));

        // If we read RTSP/<x>.<y> back the server is plain RTSP. If we
        // read anything else (including nothing within the 500ms read
        // window — a slow bridge accept/respond chain is the common
        // case right after a child bounce) we used to pick "rtsps" as
        // the fallback. That's the wrong default for our bridge: it
        // serves PLAIN RTSP by default (`rdev.tls = false` in
        // BridgeApp.cpp; only flipped by BAMBU_BRIDGE_RTSP_TLS=1). A TLS
        // ServerHello starts with 0x16 0x03 — sniff for that to detect
        // the rare TLS case; otherwise default to plain rtsp so the
        // slow-response case doesn't silently break video with an
        // un-decryptable TLS handshake against a plain socket.
        const bool looks_like_tls =
            (got >= 2 && static_cast<unsigned char>(buf[0]) == 0x16
                      && static_cast<unsigned char>(buf[1]) == 0x03);
        scheme = looks_like_tls ? "rtsps" : "rtsp";
        boost::system::error_code ig;
        sock.close(ig);
    } catch (...) {
        return nullptr;
    }
    return scheme;
}
} // namespace

std::string build_virtual_storage_url(MachineObject* mo) {
    if (!mo) return {};
    const std::string dev_id = mo->get_dev_id();
    if (!NetworkAgent::is_virtual_dev_id(dev_id)) return {};
    const std::string lan_ip = mo->get_dev_ip();
    if (lan_ip.empty()) return {};
    const std::string passwd = mo->get_access_code();
    // Per-printer vtun port (vtun_base + index), resolved the same way as
    // MQTT/FTPS/RTSP — NOT hardcoded 39998, which would send every printer's
    // storage to the first printer's (A1's) tunnel.
    const uint16_t vtun_port =
        Slic3r::VirtualSsdpDiscovery::port_for(dev_id, kBridgeVtunPort, lan_ip);
    return "bambu:///virtual/" + lan_ip + ":" +
           std::to_string(vtun_port) +
           "?dev_id=" + dev_id +
           "&access_code=" + passwd;
}

std::string build_virtual_live_url(MachineObject* mo) {
    if (!mo) return {};
    const std::string dev_id = mo->get_dev_id();
    if (!NetworkAgent::is_virtual_dev_id(dev_id)) return {};
    std::string lan_ip = mo->get_dev_ip();
    if (lan_ip.empty()) return {};
    // Standard PLAIN RTSP served by the bridge's own C++ RtspServer (the
    // relay lives in the bridge codebase — server/RtspServer.cpp — not an
    // external process). It re-packetises the printer's camera as standard
    // RTP (H.264/RFC6184 or MJPEG/RFC2435), which wxMediaCtrl2's GStreamer
    // rtspsrc backend plays directly — no libBambuSource, no bambu:///.
    // Per-device port (rtsp_base + index) via the shared resolver — live SSDP
    // cache -> persisted store -> unicast probe of the bridge — matching
    // MQTT/FTPS/vtun. (Was store-only, which silently defaulted to the A1's
    // 38322 whenever the store hadn't captured this dev's mqtt_port yet.)
    uint16_t rtsp_port =
        Slic3r::VirtualSsdpDiscovery::port_for(dev_id, kBridgeRtspPort, lan_ip);
    // Transport (rtsp vs rtsps) is detected by probing the bridge — no
    // slicer-side flag needed; the bridge's BAMBU_BRIDGE_RTSP_TLS is the
    // single source of truth.
    const char* scheme = probe_rtsp_scheme(lan_ip, rtsp_port);

    if (scheme == nullptr) {
        // Connection failed. The IP address likely changed and our cache is stale.
        // Force a multicast rediscovery of the bridge.
        BBL_LOG("media-url", "probe_failed_triggering_rediscovery")
            .str("dev_id", dev_id)
            .str("stale_ip", lan_ip);
        
        Slic3r::VirtualSsdpDiscovery::invalidate_ip(dev_id);
        auto found = Slic3r::VirtualSsdpDiscovery::rediscover_bridge(dev_id);
        if (!found.ip.empty()) {
            lan_ip = found.ip;
            mo->set_dev_ip(lan_ip);
            rtsp_port = Slic3r::VirtualSsdpDiscovery::port_for(dev_id, kBridgeRtspPort, lan_ip);
            scheme = probe_rtsp_scheme(lan_ip, rtsp_port);
        }
        if (scheme == nullptr) scheme = "rtsp"; // Final fallback
    }
#ifdef _WIN32
    // Windows: a plain rtsp(s):// URL would be handed to wxMediaCtrl2's native
    // backend = Windows Media Player, which CANNOT play RTSP (it sends
    // OPTIONS/DESCRIBE then hangs at "Loading…" forever). Real printers avoid
    // this by using the bambu:/// scheme, which wxMediaCtrl2 routes to the
    // registered BambuSource.dll DirectShow filter (live555 RTSP client) — the
    // one client that handles RTP-over-TCP on Windows. Point that same client
    // at the bridge's relay so virtual cameras play exactly like real ones.
    // (Linux keeps the plain rtsp:// form below, played by GStreamer rtspsrc.)
    const std::string passwd = mo->get_access_code();
    const std::string esc_scheme =
        std::string(scheme) + "___";   // bambu:/// encodes "://" as "___"
    const std::string url =
        "bambu:///" + esc_scheme + "bblp:" + passwd + "@" + lan_ip + ":" +
        std::to_string(rtsp_port) + "/streaming/live/1" +
        "?proto=" + scheme +
        "&device=" + dev_id;
#else
    const std::string url = std::string(scheme) + "://" + lan_ip + ":" +
                            std::to_string(rtsp_port) + "/streaming/live/1";
#endif
    BBL_LOG("media-url", "virtual_live_url_built")
        .str("dev_id",    dev_id)
        .str("lan_ip",    lan_ip)
        .num("rtsp_port", rtsp_port)
        .str("scheme",    scheme)
        .str("url",       url);
    return url;
}

} // namespace GUI
} // namespace Slic3r

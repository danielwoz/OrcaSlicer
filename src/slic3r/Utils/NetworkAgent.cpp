#include <stdio.h>
#include <stdlib.h>
#include <atomic>
#include <chrono>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>
#include <algorithm>

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>
#include "libslic3r/Utils.hpp"
#include "NetworkAgent.hpp"
#include "BBLNetworkPlugin.hpp"
#include "bambu_virtual_client/VirtualMqttClient.hpp"
#include "bambu_virtual_client/VirtualFtpsClient.hpp"
#include "bambu_virtual_client/VirtualLanPrinterStore.hpp"
#include "bambu_virtual_client/VirtualSsdpDiscovery.hpp"

namespace Slic3r {

namespace {

template<typename Fn>
int invoke_on_all_cloud_agents(const std::map<std::string, std::shared_ptr<ICloudServiceAgent>>& cloud_agents, Fn&& fn)
{
    if (cloud_agents.empty()) {
        return -1;
    }

    int result = 0;
    for (const auto& cloud_agent_pair : cloud_agents) {
        const int ret = fn(*cloud_agent_pair.second);
        if (result == 0 && ret != 0) {
            result = ret;
        }
    }

    return result;
}

} // namespace

bool NetworkAgent::use_legacy_network = true;

// ============================================================================
// Static methods - delegate to BBLNetworkPlugin
// ============================================================================

std::string NetworkAgent::get_libpath_in_current_directory(std::string library_name)
{
    return BBLNetworkPlugin::get_libpath_in_current_directory(library_name);
}

std::string NetworkAgent::get_versioned_library_path(const std::string& version)
{
    return BBLNetworkPlugin::get_versioned_library_path(version);
}

bool NetworkAgent::versioned_library_exists(const std::string& version) { return BBLNetworkPlugin::versioned_library_exists(version); }

bool NetworkAgent::legacy_library_exists() { return BBLNetworkPlugin::legacy_library_exists(); }

void NetworkAgent::remove_legacy_library() { BBLNetworkPlugin::remove_legacy_library(); }

std::vector<std::string> NetworkAgent::scan_plugin_versions() { return BBLNetworkPlugin::scan_plugin_versions(); }

int NetworkAgent::initialize_network_module(bool using_backup, const std::string& version)
{
    return BBLNetworkPlugin::instance().initialize(using_backup, version);
}

int NetworkAgent::unload_network_module() { return BBLNetworkPlugin::instance().unload(); }

bool NetworkAgent::is_network_module_loaded() { return BBLNetworkPlugin::instance().is_loaded(); }

bool NetworkAgent::has_virtual_printer() {
    try {
        return !Slic3r::VirtualLanPrinterStore().load().empty();
    } catch (...) {
        return false;
    }
}

#if defined(_MSC_VER) || defined(_WIN32)
HMODULE NetworkAgent::get_bambu_source_entry() { return BBLNetworkPlugin::instance().get_bambu_source_entry(); }
#else
void* NetworkAgent::get_bambu_source_entry() { return BBLNetworkPlugin::instance().get_bambu_source_entry(); }
#endif

std::string NetworkAgent::get_version() { return BBLNetworkPlugin::instance().get_version(); }

void* NetworkAgent::get_network_function(const char* name) { return BBLNetworkPlugin::instance().get_network_function(name); }

NetworkLibraryLoadError NetworkAgent::get_load_error() { return BBLNetworkPlugin::instance().get_load_error(); }

void NetworkAgent::clear_load_error() { BBLNetworkPlugin::instance().clear_load_error(); }

void NetworkAgent::set_load_error(const std::string& message, const std::string& technical_details, const std::string& attempted_path)
{
    BBLNetworkPlugin::instance().set_load_error(message, technical_details, attempted_path);
}

// ============================================================================
// Constructors
// ============================================================================

NetworkAgent::NetworkAgent(std::shared_ptr<ICloudServiceAgent> cloud_agent, std::shared_ptr<IPrinterAgent> printer_agent)
    : m_printer_agent(std::move(printer_agent))
{
    if (!cloud_agent) {
        BOOST_LOG_TRIVIAL(warning) << "Null cloud agent provided, skipping agent initialization";
        return;
    }
    if (cloud_agent->get_id().empty()) {
        BOOST_LOG_TRIVIAL(warning) << "Invalid cloud agent with empty ID provided, skipping agent initialization";
        return;
    }
    m_cloud_agents.emplace(cloud_agent->get_id(), std::move(cloud_agent));

    // The bridge advertises one MQTT port per virtual printer. The
    // slicer's MachineObject only knows `dev_ip`; the port comes from
    // SSDP LOCATION headers persisted by VirtualLanPrinterStore. The
    // VirtualMqttClient calls this resolver each connect_printer.
    // Falls back to 8883 inside the client if 0 is returned (no entry
    // or stale store).
    Slic3r::VirtualMqttClient::instance().set_port_resolver(
        [](const std::string& dev_id) -> uint16_t {
            if (!is_virtual_dev_id(dev_id)) return 0;
            // Shared chain: live SSDP cache -> persisted store -> unicast probe
            // (the store's lan_ip seeds the probe). 0 -> client falls back to 8883.
            return Slic3r::VirtualSsdpDiscovery::resolve_mqtt_port(dev_id);
        });
}

NetworkAgent::~NetworkAgent()
{
    // Note: We don't destroy the agent here anymore since it's managed by BBLNetworkPlugin singleton
    // The singleton manages the agent lifecycle
}

void NetworkAgent::add_cloud_agent(const std::string& provider, std::shared_ptr<ICloudServiceAgent> agent)
{
    if (agent) {
        m_cloud_agents[provider] = std::move(agent);
    }
}

void NetworkAgent::set_printer_agent(std::shared_ptr<IPrinterAgent> printer_agent)
{
    if (!printer_agent) {
        return;
    }

    // Disconnect all callbacks from the old agent
    auto old_printer_agent = m_printer_agent;

    m_printer_agent    = std::move(printer_agent);
    m_printer_agent_id = m_printer_agent->get_agent_info().id;

    // Disconnect the old agent's connections/threads.
    if (old_printer_agent && old_printer_agent != m_printer_agent) {
        old_printer_agent->disconnect_printer();
        apply_printer_callbacks(old_printer_agent, {});
    }

    apply_printer_callbacks(m_printer_agent, m_printer_callbacks);
}

void* NetworkAgent::get_network_agent() { return BBLNetworkPlugin::instance().get_agent(); }

void NetworkAgent::apply_printer_callbacks(const std::shared_ptr<IPrinterAgent>& printer_agent, const PrinterCallbacks& callbacks)
{
    if (!printer_agent) {
        return;
    }

    printer_agent->set_on_ssdp_msg_fn(callbacks.on_ssdp_msg_fn);
    printer_agent->set_on_printer_connected_fn(callbacks.on_printer_connected_fn);
    printer_agent->set_on_subscribe_failure_fn(callbacks.on_subscribe_failure_fn);
    printer_agent->set_on_message_fn(callbacks.on_message_fn);
    printer_agent->set_on_user_message_fn(callbacks.on_user_message_fn);
    printer_agent->set_on_local_connect_fn(callbacks.on_local_connect_fn);
    printer_agent->set_on_local_message_fn(callbacks.on_local_message_fn);
    printer_agent->set_queue_on_main_fn(callbacks.queue_on_main_fn);
    printer_agent->set_server_callback(callbacks.on_server_err_fn);
}

std::shared_ptr<ICloudServiceAgent> NetworkAgent::get_cloud_agent(const std::string& provider) const
{
    const auto& key = (provider.empty() || provider == ORCA_CLOUD_PROVIDER) ? ORCA_CLOUD_PROVIDER : provider;
    auto it = m_cloud_agents.find(key);
    return it != m_cloud_agents.end() ? it->second : nullptr;
}

// ============================================================================
// Shared agent methods
// ============================================================================

int NetworkAgent::set_queue_on_main_fn(QueueOnMainFn fn, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    m_printer_callbacks.queue_on_main_fn = fn;

    int ret = -1;
    if (cloud_agent)
        ret = cloud_agent->set_queue_on_main_fn(fn);
    if (m_printer_agent)
        m_printer_agent->set_queue_on_main_fn(fn);
    return ret;
}

// ============================================================================
// Cloud agent methods
// ============================================================================

int NetworkAgent::init_log()
{
    return invoke_on_all_cloud_agents(m_cloud_agents, [](ICloudServiceAgent& cloud_agent) { return cloud_agent.init_log(); });
}

int NetworkAgent::set_config_dir(std::string config_dir)
{
    return invoke_on_all_cloud_agents(m_cloud_agents,
                                      [&config_dir](ICloudServiceAgent& cloud_agent) { return cloud_agent.set_config_dir(config_dir); });
}

int NetworkAgent::set_cert_file(std::string folder, std::string filename)
{
    return invoke_on_all_cloud_agents(m_cloud_agents, [&folder, &filename](ICloudServiceAgent& cloud_agent) {
        return cloud_agent.set_cert_file(folder, filename);
    });
}

int NetworkAgent::set_country_code(std::string country_code)
{
    return invoke_on_all_cloud_agents(m_cloud_agents, [&country_code](ICloudServiceAgent& cloud_agent) {
        return cloud_agent.set_country_code(country_code);
    });
}

int NetworkAgent::start()
{
    return invoke_on_all_cloud_agents(m_cloud_agents, [](ICloudServiceAgent& cloud_agent) { return cloud_agent.start(); });
}

int NetworkAgent::set_on_server_connected_fn(AppOnServerConnectedFn fn)
{
    return invoke_on_all_cloud_agents(m_cloud_agents,
                                      [fn](ICloudServiceAgent& cloud_agent) { return cloud_agent.set_on_server_connected_fn(fn); });
}

int NetworkAgent::set_on_http_error_fn(AppOnHttpErrorFn fn)
{
    return invoke_on_all_cloud_agents(m_cloud_agents,
                                      [fn](ICloudServiceAgent& cloud_agent) { return cloud_agent.set_on_http_error_fn(fn); });
}

int NetworkAgent::set_get_country_code_fn(GetCountryCodeFn fn)
{
    return invoke_on_all_cloud_agents(m_cloud_agents,
                                      [fn](ICloudServiceAgent& cloud_agent) { return cloud_agent.set_get_country_code_fn(fn); });
}

int NetworkAgent::change_user(std::string user_info, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->change_user(std::move(user_info));
    return -1;
}

bool NetworkAgent::is_user_login(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->is_user_login();
    return false;
}

int NetworkAgent::user_logout(bool request, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->user_logout(request);
    return -1;
}

std::string NetworkAgent::get_user_id(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_user_id();
    return "";
}

std::string NetworkAgent::get_user_name(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_user_name();
    return "";
}

std::string NetworkAgent::get_user_avatar(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_user_avatar();
    return "";
}

std::string NetworkAgent::get_user_nickname(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_user_nickname();
    return "";
}

std::string NetworkAgent::build_login_cmd(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->build_login_cmd();
    return "";
}

std::string NetworkAgent::build_logout_cmd(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->build_logout_cmd();
    return "";
}

std::string NetworkAgent::build_login_info(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->build_login_info();
    return "";
}

std::string NetworkAgent::get_cloud_service_host(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_cloud_service_host();
    return "";
}

std::string NetworkAgent::get_cloud_login_url(const std::string& language, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_cloud_login_url(language);
    return "";
}

int NetworkAgent::connect_server()
{
    return invoke_on_all_cloud_agents(m_cloud_agents, [](ICloudServiceAgent& cloud_agent) { return cloud_agent.connect_server(); });
}

bool NetworkAgent::is_server_connected(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->is_server_connected();
    return false;
}

int NetworkAgent::refresh_connection(const std::string& provider)
{
    if(provider.empty())
        return invoke_on_all_cloud_agents(m_cloud_agents, [](ICloudServiceAgent& cloud_agent) { return cloud_agent.refresh_connection(); });
    else {
        const auto cloud_agent = get_cloud_agent(provider);
        if (cloud_agent)
            return cloud_agent->refresh_connection();
        return -1;
    }
     
}

void NetworkAgent::enable_multi_machine(bool enable, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        cloud_agent->enable_multi_machine(enable);
}

int NetworkAgent::get_user_presets(std::map<std::string, std::map<std::string, std::string>>* user_presets, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_user_presets(user_presets);
    return -1;
}

std::string NetworkAgent::request_setting_id(std::string                         name,
                                             std::map<std::string, std::string>* values_map,
                                             unsigned int*                       http_code,
                                             const std::string&                  provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->request_setting_id(std::move(name), values_map, http_code);
    return "";
}

int NetworkAgent::put_setting(std::string                         setting_id,
                              std::string                         name,
                              std::map<std::string, std::string>* values_map,
                              unsigned int*                       http_code,
                              const std::string&                  provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->put_setting(std::move(setting_id), std::move(name), values_map, http_code);
    return -1;
}

int NetworkAgent::get_setting_list(std::string bundle_version, ProgressFn pro_fn, WasCancelledFn cancel_fn, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_setting_list(std::move(bundle_version), pro_fn, cancel_fn);
    return -1;
}

int NetworkAgent::get_setting_list2(
    std::string bundle_version, CheckFn chk_fn, ProgressFn pro_fn, WasCancelledFn cancel_fn, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_setting_list2(std::move(bundle_version), chk_fn, pro_fn, cancel_fn);
    return -1;
}

int NetworkAgent::delete_setting(std::string setting_id, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->delete_setting(std::move(setting_id));
    return -1;
}

int NetworkAgent::get_my_message(int type, int after, int limit, unsigned int* http_code, std::string* http_body, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_my_message(type, after, limit, http_code, http_body);
    return -1;
}

int NetworkAgent::check_user_task_report(int* task_id, bool* printable, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->check_user_task_report(task_id, printable);
    return -1;
}

int NetworkAgent::get_user_print_info(unsigned int* http_code, std::string* http_body, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_user_print_info(http_code, http_body);
    return -1;
}

int NetworkAgent::get_user_tasks(TaskQueryParams params, std::string* http_body, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_user_tasks(params, http_body);
    return -1;
}

int NetworkAgent::get_printer_firmware(std::string dev_id, unsigned* http_code, std::string* http_body, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_printer_firmware(std::move(dev_id), http_code, http_body);
    return -1;
}

int NetworkAgent::get_task_plate_index(std::string task_id, int* plate_index, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_task_plate_index(std::move(task_id), plate_index);
    return -1;
}

int NetworkAgent::get_user_info(int* identifier, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_user_info(identifier);
    return -1;
}

int NetworkAgent::get_subtask_info(
    std::string subtask_id, std::string* task_json, unsigned int* http_code, std::string* http_body, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_subtask_info(std::move(subtask_id), task_json, http_code, http_body);
    return -1;
}

int NetworkAgent::get_slice_info(
    std::string project_id, std::string profile_id, int plate_index, std::string* slice_json, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_slice_info(std::move(project_id), std::move(profile_id), plate_index, slice_json);
    return -1;
}

int NetworkAgent::query_bind_status(std::vector<std::string> query_list,
                                    unsigned int*            http_code,
                                    std::string*             http_body,
                                    const std::string&       provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->query_bind_status(std::move(query_list), http_code, http_body);
    return -1;
}

int NetworkAgent::modify_printer_name(std::string dev_id, std::string dev_name, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->modify_printer_name(std::move(dev_id), std::move(dev_name));
    return -1;
}

int NetworkAgent::get_camera_url(std::string dev_id, std::function<void(std::string)> callback, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_camera_url(std::move(dev_id), std::move(callback));
    return -1;
}

int NetworkAgent::get_design_staffpick(int offset, int limit, std::function<void(std::string)> callback, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_design_staffpick(offset, limit, std::move(callback));
    return -1;
}

int NetworkAgent::start_publish(
    PublishParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, std::string* out, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->start_publish(params, update_fn, cancel_fn, out);
    return -1;
}

int NetworkAgent::get_model_publish_url(std::string* url, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_model_publish_url(url);
    return -1;
}

int NetworkAgent::get_subtask(BBLModelTask* task, OnGetSubTaskFn getsub_fn, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_subtask(task, getsub_fn);
    return -1;
}

int NetworkAgent::get_model_mall_home_url(std::string* url, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_model_mall_home_url(url);
    return -1;
}

int NetworkAgent::get_model_mall_detail_url(std::string* url, std::string id, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_model_mall_detail_url(url, std::move(id));
    return -1;
}

int NetworkAgent::get_my_profile(std::string token, unsigned int* http_code, std::string* http_body, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_my_profile(std::move(token), http_code, http_body);
    return -1;
}

int NetworkAgent::get_my_token(std::string ticket, unsigned int* http_code, std::string* http_body, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_my_token(std::move(ticket), http_code, http_body);
    return -1;
}

int NetworkAgent::track_enable(bool enable, const std::string& provider)
{
    this->enable_track     = enable;
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->track_enable(enable);
    return -1;
}

int NetworkAgent::track_remove_files(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->track_remove_files();
    return -1;
}

int NetworkAgent::track_event(std::string evt_key, std::string content, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->track_event(std::move(evt_key), std::move(content));
    return -1;
}

int NetworkAgent::track_header(std::string header, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->track_header(std::move(header));
    return -1;
}

int NetworkAgent::track_update_property(std::string name, std::string value, std::string type, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->track_update_property(std::move(name), std::move(value), std::move(type));
    return -1;
}

int NetworkAgent::track_get_property(std::string name, std::string& value, std::string type, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->track_get_property(std::move(name), value, std::move(type));
    return -1;
}

int NetworkAgent::put_model_mall_rating(int                      design_id,
                                        int                      score,
                                        std::string              content,
                                        std::vector<std::string> images,
                                        unsigned int&            http_code,
                                        std::string&             http_error,
                                        const std::string&       provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->put_model_mall_rating(design_id, score, std::move(content), std::move(images), http_code, http_error);
    return -1;
}

int NetworkAgent::get_oss_config(
    std::string& config, std::string country_code, unsigned int& http_code, std::string& http_error, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_oss_config(config, std::move(country_code), http_code, http_error);
    return -1;
}

int NetworkAgent::put_rating_picture_oss(std::string&       config,
                                         std::string&       pic_oss_path,
                                         std::string        model_id,
                                         int                profile_id,
                                         unsigned int&      http_code,
                                         std::string&       http_error,
                                         const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->put_rating_picture_oss(config, pic_oss_path, std::move(model_id), profile_id, http_code, http_error);
    return -1;
}

int NetworkAgent::get_model_mall_rating_result(
    int job_id, std::string& rating_result, unsigned int& http_code, std::string& http_error, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_model_mall_rating_result(job_id, rating_result, http_code, http_error);
    return -1;
}

int NetworkAgent::get_mw_user_preference(std::function<void(std::string)> callback, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_mw_user_preference(std::move(callback));
    return -1;
}

int NetworkAgent::get_mw_user_4ulist(int seed, int limit, std::function<void(std::string)> callback, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_mw_user_4ulist(seed, limit, std::move(callback));
    return -1;
}

// ============================================================================
// Printer agent methods
// ============================================================================

int NetworkAgent::set_on_ssdp_msg_fn(OnMsgArrivedFn fn)
{
    m_printer_callbacks.on_ssdp_msg_fn = fn;
    if (m_printer_agent)
        return m_printer_agent->set_on_ssdp_msg_fn(fn);
    return -1;
}

int NetworkAgent::set_on_printer_connected_fn(OnPrinterConnectedFn fn)
{
    m_printer_callbacks.on_printer_connected_fn = fn;
    if (m_printer_agent)
        return m_printer_agent->set_on_printer_connected_fn(fn);
    return -1;
}

int NetworkAgent::set_on_subscribe_failure_fn(GetSubscribeFailureFn fn)
{
    m_printer_callbacks.on_subscribe_failure_fn = fn;
    if (m_printer_agent)
        return m_printer_agent->set_on_subscribe_failure_fn(fn);
    return -1;
}

int NetworkAgent::set_on_message_fn(OnMessageFn fn)
{
    m_printer_callbacks.on_message_fn = fn;
    // No need to wire `on_message` into VirtualMqttClient — the
    // virtual path uses set_on_local_*_fn (LAN-only). Keep it captured
    // for the plugin path only.
    if (m_printer_agent)
        return m_printer_agent->set_on_message_fn(fn);
    return -1;
}

int NetworkAgent::set_on_user_message_fn(OnMessageFn fn)
{
    m_printer_callbacks.on_user_message_fn = fn;
    if (m_printer_agent)
        return m_printer_agent->set_on_user_message_fn(fn);
    return -1;
}

int NetworkAgent::set_on_local_connect_fn(OnLocalConnectedFn fn)
{
    m_printer_callbacks.on_local_connect_fn = fn;
    // Capture so VirtualMqttClient can fire it for FFFF dev-ids when
    // their TLS+MQTT session reaches CONNACK. Wrap to filter virtual
    // dev-ids out of the plugin's own callback — the plugin's SSDP
    // scanner will otherwise fail the bridge's cert and report
    // state=Failed for our virtual entries, yanking them out of the
    // UI a few seconds after we add them.
    if (m_printer_agent) {
        OnLocalConnectedFn wrapped =
            [fn](int state, std::string dev_id, std::string msg) {
                if (is_virtual_dev_id(dev_id)) return;
                if (fn) fn(state, dev_id, msg);
            };
        Slic3r::VirtualMqttClient::instance().set_on_local_connect(fn);
        return m_printer_agent->set_on_local_connect_fn(std::move(wrapped));
    }
    // Plugin absent: still install the callback on VirtualMqttClient so
    // the virtual flow works.
    Slic3r::VirtualMqttClient::instance().set_on_local_connect(fn);
    return -1;
}

int NetworkAgent::set_on_local_message_fn(OnMessageFn fn)
{
    m_printer_callbacks.on_local_message_fn = fn;
    // Same shape as set_on_local_connect_fn: filter plugin-originated
    // messages for virtual dev-ids out (VirtualMqttClient owns those),
    // and install the user's raw callback on VirtualMqttClient so
    // device/<dev_id>/report frames flow through.
    if (m_printer_agent) {
        OnMessageFn wrapped =
            [fn](std::string dev_id, std::string msg) {
                if (is_virtual_dev_id(dev_id)) return;
                if (fn) fn(dev_id, msg);
            };
        Slic3r::VirtualMqttClient::instance().set_on_message(fn);
        return m_printer_agent->set_on_local_message_fn(std::move(wrapped));
    }
    Slic3r::VirtualMqttClient::instance().set_on_message(fn);
    return -1;
}

int NetworkAgent::set_server_callback(OnServerErrFn fn)
{
    m_printer_callbacks.on_server_err_fn = fn;
    if (m_printer_agent)
        return m_printer_agent->set_server_callback(fn);
    return -1;
}

int NetworkAgent::send_message(std::string dev_id, std::string json_str, int qos, int flag)
{
    if (m_printer_agent)
        return m_printer_agent->send_message(dev_id, json_str, qos, flag);
    return -1;
}

int NetworkAgent::connect_printer(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl)
{
    if (is_virtual_dev_id(dev_id)) {
        auto& vc = Slic3r::VirtualMqttClient::instance();
        // Make sure VirtualMqttClient knows the captured callbacks even
        // if set_on_local_*_fn was called before NetworkAgent learned
        // about a printer_agent.
        if (m_printer_callbacks.on_local_connect_fn)
            vc.set_on_local_connect(m_printer_callbacks.on_local_connect_fn);
        if (m_printer_callbacks.on_local_message_fn)
            vc.set_on_message(m_printer_callbacks.on_local_message_fn);
        int rc = vc.connect_printer(dev_id, dev_ip, /*access_code=*/password);
        if (rc == 0) m_current_local_dev_id = dev_id;
        return rc;
    }
    if (m_printer_agent) {
        int rc = m_printer_agent->connect_printer(dev_id, dev_ip, username, password, use_ssl);
        if (rc == 0) m_current_local_dev_id = dev_id;
        return rc;
    }
    return -1;
}

int NetworkAgent::disconnect_printer()
{
    // The dev-id-less disconnect API mirrors the plugin's one-session-
    // at-a-time model. Route based on whatever connect_printer most
    // recently registered.
    if (is_virtual_dev_id(m_current_local_dev_id)) {
        const std::string id = m_current_local_dev_id;
        m_current_local_dev_id.clear();
        return Slic3r::VirtualMqttClient::instance().disconnect_printer(id);
    }
    if (m_printer_agent) {
        int rc = m_printer_agent->disconnect_printer();
        if (rc == 0) m_current_local_dev_id.clear();
        return rc;
    }
    return -1;
}

int NetworkAgent::send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag)
{
    if (is_virtual_dev_id(dev_id)) {
        return Slic3r::VirtualMqttClient::instance()
            .send_message(dev_id, json_str, qos);
    }
    if (m_printer_agent)
        return m_printer_agent->send_message_to_printer(dev_id, json_str, qos, flag);
    return -1;
}

int NetworkAgent::check_cert()
{
    if (m_printer_agent)
        return m_printer_agent->check_cert();
    return -1;
}

void NetworkAgent::install_device_cert(std::string dev_id, bool lan_only)
{
    if (m_printer_agent)
        m_printer_agent->install_device_cert(dev_id, lan_only);
}

bool NetworkAgent::start_discovery(bool start, bool sending)
{
    if (m_printer_agent)
        return m_printer_agent->start_discovery(start, sending);
    return false;
}

int NetworkAgent::ping_bind(std::string ping_code)
{
    if (m_printer_agent)
        return m_printer_agent->ping_bind(ping_code);
    return -1;
}

int NetworkAgent::bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect)
{
    if (m_printer_agent)
        return m_printer_agent->bind_detect(dev_ip, sec_link, detect);
    return -1;
}

int NetworkAgent::bind(
    std::string dev_ip, std::string dev_id, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn)
{
    if (m_printer_agent)
        return m_printer_agent->bind(dev_ip, dev_id, sec_link, timezone, improved, update_fn);
    return -1;
}

int NetworkAgent::unbind(std::string dev_id)
{
    if (m_printer_agent)
        return m_printer_agent->unbind(dev_id);
    return -1;
}

std::string NetworkAgent::get_user_selected_machine()
{
    if (m_printer_agent)
        return m_printer_agent->get_user_selected_machine();
    return "";
}

int NetworkAgent::set_user_selected_machine(std::string dev_id)
{
    if (m_printer_agent)
        return m_printer_agent->set_user_selected_machine(dev_id);
    return -1;
}

int NetworkAgent::start_subscribe(std::string module)
{
    if (m_printer_agent)
        return m_printer_agent->start_subscribe(std::move(module));
    return -1;
}

int NetworkAgent::stop_subscribe(std::string module)
{
    if (m_printer_agent)
        return m_printer_agent->stop_subscribe(std::move(module));
    return -1;
}

int NetworkAgent::add_subscribe(std::vector<std::string> dev_list)
{
    if (m_printer_agent)
        return m_printer_agent->add_subscribe(std::move(dev_list));
    return -1;
}

int NetworkAgent::del_subscribe(std::vector<std::string> dev_list)
{
    if (m_printer_agent)
        return m_printer_agent->del_subscribe(std::move(dev_list));
    return -1;
}

namespace {

// Mirror of BambuStudio OSS
// `bambu_net_oss/core/LocalPrintOrchestrator.cpp::compose_remote_path`.
// SHARED source of truth for both the FTPS STOR remote name and the
// `print.param` field of the slicer's `gcode_file` MQTT command. These
// two MUST be identical character-for-character — the bridge's MqttBroker
// print interceptor keys its spool lookup on the basename of
// `print.param`. Drift between the two sites is exactly the bug we hit
// in BBS-bridge and tracked down by factoring this function out; same
// invariant applies here.
//
// `project_name` is intentionally NOT involved: it rides separately on
// the MQTT command as `print.project_name` and the printer uses it for
// the UI label; the filesystem path stays under the slicer's own naming
// convention (the dotted temp `.NNNNNN.0.3mf` BambuStudio / OrcaSlicer
// writes), matching what stock direct prints do.
static std::string compose_remote_path(const PrintParams& p) {
    std::string folder = p.ftp_folder.empty() ? std::string("/") : p.ftp_folder;
    if (folder.empty() || folder.back() != '/') folder += '/';

    std::string fname = p.dst_file;
    if (fname.empty()) fname = p.ftp_file;
    if (fname.empty()) {
        try {
            fname = boost::filesystem::path(p.filename).filename().string();
        } catch (...) {}
    }
    if (fname.empty()) fname = "lan_print.3mf";
    if (!fname.empty() && fname.front() == '/') fname.erase(0, 1);
    return folder + fname;
}

// Virtual-printer (FFFF) FTPS upload. Per-printer FTPS port (ftps_base+index)
// via the shared resolver so a multi-printer send hits THIS printer's FTPS
// server instead of always landing on the first printer's 39990 (otherwise
// the slicer re-prompts for IP/access code).
int virtual_ftps_upload_(const PrintParams& params,
                         OnUpdateStatusFn  update_fn,
                         WasCancelledFn    cancel_fn) {
    BOOST_LOG_TRIVIAL(info)
        << "[ORCA-TRACE] virtual_ftps_upload_ entered "
        << " dev_id=" << params.dev_id
        << " dev_ip=" << params.dev_ip
        << " ftp_folder=" << params.ftp_folder
        << " ftp_file=" << params.ftp_file
        << " dst_file=" << params.dst_file
        << " filename=" << params.filename
        << " username=" << params.username
        << " pass_len=" << params.password.size();
    Slic3r::virtual_ftps::UploadParams up;
    up.host        = params.dev_ip;
    up.port        = Slic3r::VirtualSsdpDiscovery::port_for(params.dev_id, 39990, params.dev_ip);
    BOOST_LOG_TRIVIAL(info)
        << "[ORCA-TRACE] virtual_ftps_upload_ resolved "
        << " host=" << up.host << " port=" << up.port;
    up.user        = params.username.empty() ? std::string("bblp") : params.username;
    up.pass        = params.password;
    up.local_path  = params.filename;
    // Single source of truth shared with the gcode_file MQTT command
    // emitted from virtual_lan_print_. See compose_remote_path() above.
    {
        const std::string composed = compose_remote_path(params);
        const auto slash = composed.find_last_of('/');
        up.remote_name = (slash == std::string::npos) ? composed
                                                      : composed.substr(slash + 1);
    }
    Slic3r::virtual_ftps::ProgressFn  prog = nullptr;
    Slic3r::virtual_ftps::CancelledFn canc = nullptr;
    if (update_fn) prog = [update_fn](int pct, std::string msg){ update_fn(pct, 0, msg); };
    if (cancel_fn) canc = [cancel_fn]() -> bool { return cancel_fn(); };
    BOOST_LOG_TRIVIAL(info) << "virtual_ftps_upload: dev=" << params.dev_id
                            << " ftps_port=" << up.port
                            << " remote=" << up.remote_name;
    return Slic3r::virtual_ftps::upload(up, prog, canc);
}

// Virtual-printer (FFFF) LAN PRINT: upload + send the gcode_file MQTT command
// telling the printer "print this file from your SD". Mirrors the OSS
// BambuStudio LocalPrintOrchestrator exactly:
//   {"print":{"command":"gcode_file","param":"<remote_path>","sequence_id":"<n>"}}
// The MQTT publish goes through the FFFF branch of NetworkAgent::send_message
// (-> VirtualMqttClient -> bridge broker -> printer, signed).
int virtual_lan_print_(const PrintParams& params,
                       OnUpdateStatusFn  update_fn,
                       WasCancelledFn    cancel_fn) {
    // (Plates start at 1: slicer writes plate_1.gcode, slice_info.config
    // index=1, bridge passes through, printer opens plate_1.gcode. An
    // earlier `virtual_print_normalise_plate_to_zero` renamed
    // plate_<N>.* → plate_0.* but did NOT update slice_info.config,
    // creating a mismatch the printer rejected. Removed in both slicers
    // and bridge; see memory `project_orca_3mf_filament_settings`.)

    int rc = virtual_ftps_upload_(params, update_fn, cancel_fn);
    if (rc != 0) {
        BOOST_LOG_TRIVIAL(warning) << "virtual_lan_print: FTPS upload failed rc=" << rc;
        return rc;
    }
    // Single source of truth shared with virtual_ftps_upload_'s STOR
    // remote_name. Both ends MUST agree or the printer can't find the
    // file the bridge stored. See compose_remote_path() above.
    const std::string remote_path = compose_remote_path(params);
    static std::atomic<uint64_t> s_seq{1};
    const std::string seq = std::to_string(s_seq.fetch_add(1));

    // Build the gcode_file payload to mirror what BambuStudio's
    // proprietary plugin's `start_local_print` produces — captured in
    // docs/plugin-trace/H2D-lan.yaml (2026-05-30, the successful BBS LAN
    // print). The bare 3-field payload (BBS OSS reference) works for
    // some calibration files but not for AMS-equipped object prints,
    // because the printer has no way to know which AMS slot to draw
    // from. Adding the AMS map + bed-type + cali toggles brings the
    // wire shape into line with what the H2D firmware sees in cloud-
    // relay project_file ACKs.
    nlohmann::json j;
    j["print"]["command"]     = "gcode_file";
    j["print"]["param"]       = remote_path;
    j["print"]["sequence_id"] = seq;
    // AMS routing. Source-of-truth example: "[3,-1,-1,-1,-1,-1,-1,-1]"
    // (8-entry int array, -1 = unmapped). When PrintParams holds the
    // stringified form, embed verbatim; the printer parses the same
    // shape from project_file too.
    auto emit_json_or_string = [&](const char* key, const std::string& s) {
        if (s.empty()) return;
        try { j["print"][key] = nlohmann::json::parse(s); }
        catch (...) { j["print"][key] = s; }
    };
    emit_json_or_string("ams_mapping",      params.ams_mapping);
    emit_json_or_string("ams_mapping2",     params.ams_mapping2);
    emit_json_or_string("ams_mapping_info", params.ams_mapping_info);
    emit_json_or_string("nozzles_info",     params.nozzles_info);
    emit_json_or_string("nozzle_mapping",   params.nozzle_mapping);
    if (!params.task_bed_type.empty())
        j["print"]["task_bed_type"]    = params.task_bed_type;
    j["print"]["use_ams"]              = params.task_use_ams;
    j["print"]["task_use_ams"]         = params.task_use_ams;
    j["print"]["bed_leveling"]         = params.task_bed_leveling;
    j["print"]["flow_cali"]            = params.task_flow_cali;
    j["print"]["vibration_cali"]       = params.task_vibration_cali;
    j["print"]["layer_inspect"]        = params.task_layer_inspect;
    j["print"]["timelapse"]            = params.task_record_timelapse;
    j["print"]["auto_bed_leveling"]    = params.auto_bed_leveling;
    j["print"]["auto_flow_cali"]       = params.auto_flow_cali;
    j["print"]["auto_offset_cali"]     = params.auto_offset_cali;
    if (params.plate_index > 0)
        j["print"]["plate_idx"]        = std::to_string(params.plate_index);
    if (params.origin_profile_id > 0)
        j["print"]["profile_id"]       = std::to_string(params.origin_profile_id);
    if (!params.origin_model_id.empty())
        j["print"]["model_id"]         = params.origin_model_id;
    if (!params.project_name.empty())
        j["print"]["project_name"]     = params.project_name;
    if (!params.task_name.empty())
        j["print"]["task_name"]        = params.task_name;
    // try_emmc_print isn't observed as an MQTT field in any trace; it's a
    // slicer-side flag the plugin uses to pick the upload transport. Keep
    // out of the wire payload.

    const std::string cmd = j.dump();
    BOOST_LOG_TRIVIAL(info) << "virtual_lan_print: dev=" << params.dev_id
                            << " gcode_file " << remote_path
                            << " bytes=" << cmd.size()
                            << " ams_mapping=" << params.ams_mapping
                            << " use_ams=" << int(params.task_use_ams);
    int pubrc = Slic3r::VirtualMqttClient::instance().send_message(params.dev_id, cmd, /*qos=*/0);
    if (pubrc != 0) {
        BOOST_LOG_TRIVIAL(warning) << "virtual_lan_print: MQTT send rc=" << pubrc;
        return -1;
    }

    // Stock OrcaSlicer's PrintJob fires a leading verify_job probe just
    // like BambuStudio does (see PrintJob.cpp:222) — but for FFFF dev_ids
    // we now gate that out in PrintJob, so this code path should never
    // be reached with project_name=="verify_job" again. Keep the safety
    // skip anyway so an older PrintJob mid-upgrade can't deadlock the
    // wait loop below.
    if (params.project_name == "verify_job") {
        BOOST_LOG_TRIVIAL(info)
            << "virtual_lan_print: probe (project_name=verify_job) — "
               "skipping bridge-dispatch wait";
        return 0;
    }

    // Wait for the bridge to finish dispatching this print to the real
    // printer before returning. Without this, Orca's PrintJob considers
    // the print "sent" the moment the local-FTPS-to-bridge upload
    // finishes (~ instant on LAN) and immediately closes its progress
    // dialog while the bridge is still uploading to the printer and
    // waiting for the print to actually start (10–20 s after).
    //
    // The bridge writes a JSON status file at
    //   /tmp/bridge-progress/<FFFF dev_id>.json
    // and updates it on each plugin update_fn callback. We poll the
    // file every 250 ms, mirror the (stage, code, info) into the
    // caller's update_fn so Orca's "Sending…" dialog shows progress,
    // and return only when the bridge marks the upload `done` (rc=0)
    // or `failed`. Bounded by a 120-second timeout so a hung bridge
    // doesn't lock up Orca's print dialog forever.
    {
        const std::string progress_path =
            "/tmp/bridge-progress/" + params.dev_id + ".json";
        using namespace std::chrono;
        const auto deadline = steady_clock::now() + seconds(120);
        std::string last_phase;
        int         last_stage = -1, last_code = -1;
        std::string last_info;
        while (true) {
            if (cancel_fn && cancel_fn()) {
                BOOST_LOG_TRIVIAL(info)
                    << "virtual_lan_print: cancelled while waiting for "
                       "bridge dispatch";
                return -2;
            }
            if (steady_clock::now() > deadline) {
                BOOST_LOG_TRIVIAL(warning)
                    << "virtual_lan_print: bridge dispatch wait timed out "
                       "after 120s — returning success anyway so Orca "
                       "doesn't strand the user";
                return 0;
            }
            std::ifstream f(progress_path);
            if (f) {
                std::stringstream ss; ss << f.rdbuf();
                try {
                    auto j2 = nlohmann::json::parse(ss.str());
                    int stage = j2.value("stage", -1);
                    int code  = j2.value("code",  -1);
                    std::string info  = j2.value("info",  std::string{});
                    std::string phase = j2.value("phase", std::string{});
                    if (stage != last_stage || code != last_code
                        || info != last_info || phase != last_phase) {
                        if (update_fn) update_fn(stage, code, info);
                        BOOST_LOG_TRIVIAL(info)
                            << "virtual_lan_print: bridge progress phase="
                            << phase << " stage=" << stage
                            << " code=" << code << " info=" << info;
                        last_stage = stage; last_code = code;
                        last_info = info;   last_phase = phase;
                    }
                    if (phase == "done") {
                        BOOST_LOG_TRIVIAL(info)
                            << "virtual_lan_print: bridge dispatch done; "
                               "returning success";
                        return 0;
                    }
                    if (phase == "failed") {
                        BOOST_LOG_TRIVIAL(warning)
                            << "virtual_lan_print: bridge dispatch failed; "
                               "info=" << info;
                        return code != 0 ? code : -1;
                    }
                } catch (...) {
                    // Half-written file or stale; just retry.
                }
            }
            std::this_thread::sleep_for(milliseconds(250));
        }
    }
}

} // namespace

int NetworkAgent::start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    if (is_virtual_dev_id(params.dev_id)) {
        BOOST_LOG_TRIVIAL(info) << "[ORCA-TRACE] NetworkAgent print-entry "
            << " virtual_branch=true dev_id=" << params.dev_id
            << " dev_ip=" << params.dev_ip
            << " ftp_folder=" << params.ftp_folder
            << " ftp_file=" << params.ftp_file
            << " dst_file=" << params.dst_file
            << " filename=" << params.filename
            << " pass_len=" << params.password.size();
        return virtual_lan_print_(params, update_fn, cancel_fn);
    }
    BOOST_LOG_TRIVIAL(info) << "[ORCA-TRACE] NetworkAgent print-entry "
        << " virtual_branch=false dev_id=" << params.dev_id;
    if (m_printer_agent)
        return m_printer_agent->start_print(params, update_fn, cancel_fn, wait_fn);
    return -1;
}

int NetworkAgent::start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    if (is_virtual_dev_id(params.dev_id)) {
        BOOST_LOG_TRIVIAL(info) << "[ORCA-TRACE] NetworkAgent print-entry "
            << " virtual_branch=true dev_id=" << params.dev_id
            << " dev_ip=" << params.dev_ip
            << " ftp_folder=" << params.ftp_folder
            << " ftp_file=" << params.ftp_file
            << " dst_file=" << params.dst_file
            << " filename=" << params.filename
            << " pass_len=" << params.password.size();
        return virtual_lan_print_(params, update_fn, cancel_fn);
    }
    BOOST_LOG_TRIVIAL(info) << "[ORCA-TRACE] NetworkAgent print-entry "
        << " virtual_branch=false dev_id=" << params.dev_id;
    if (m_printer_agent)
        return m_printer_agent->start_local_print_with_record(params, update_fn, cancel_fn, wait_fn);
    return -1;
}

int NetworkAgent::start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    if (is_virtual_dev_id(params.dev_id))
        return virtual_ftps_upload_(params, update_fn, cancel_fn);
    if (m_printer_agent)
        return m_printer_agent->start_send_gcode_to_sdcard(params, update_fn, cancel_fn, wait_fn);
    return -1;
}

int NetworkAgent::start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    if (is_virtual_dev_id(params.dev_id)) {
        BOOST_LOG_TRIVIAL(info) << "[ORCA-TRACE] NetworkAgent print-entry "
            << " virtual_branch=true dev_id=" << params.dev_id
            << " dev_ip=" << params.dev_ip
            << " ftp_folder=" << params.ftp_folder
            << " ftp_file=" << params.ftp_file
            << " dst_file=" << params.dst_file
            << " filename=" << params.filename
            << " pass_len=" << params.password.size();
        return virtual_lan_print_(params, update_fn, cancel_fn);
    }
    BOOST_LOG_TRIVIAL(info) << "[ORCA-TRACE] NetworkAgent print-entry "
        << " virtual_branch=false dev_id=" << params.dev_id;
    if (m_printer_agent)
        return m_printer_agent->start_local_print(params, update_fn, cancel_fn);
    return -1;
}

int NetworkAgent::start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    if (m_printer_agent)
        return m_printer_agent->start_sdcard_print(params, update_fn, cancel_fn);
    return -1;
}

FilamentSyncMode NetworkAgent::get_filament_sync_mode() const
{
    if (m_printer_agent)
        return m_printer_agent->get_filament_sync_mode();
    return FilamentSyncMode::none;
}

bool NetworkAgent::fetch_filament_info(std::string dev_id)
{
    if (m_printer_agent) {
        return m_printer_agent->fetch_filament_info(dev_id);
    }
    return false;
}

int NetworkAgent::request_bind_ticket(std::string* ticket)
{
    if (m_printer_agent)
        return m_printer_agent->request_bind_ticket(ticket);
    return -1;
}

} // namespace Slic3r

#include "protocol.h"

#include "yaml_parser.h"

#include <cstdio>
#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {

static AppConfig app_config;

static bool parse_field_type(const std::string& text, FieldType& type) {
    if (text == "char")   { type = FIELD_CHAR;   return true; }
    if (text == "uint8")  { type = FIELD_UINT8;  return true; }
    if (text == "uint16") { type = FIELD_UINT16; return true; }
    if (text == "uint32") { type = FIELD_UINT32; return true; }
    if (text == "uint64") { type = FIELD_UINT64; return true; }
    if (text == "int16")  { type = FIELD_INT16;  return true; }
    if (text == "int32")  { type = FIELD_INT32;  return true; }
    if (text == "int64")  { type = FIELD_INT64;  return true; }
    if (text == "string") { type = FIELD_STRING; return true; }
    if (text == "binary") { type = FIELD_BINARY; return true; }
    return false;
}

static bool parse_message_spec(char msg_type,
                               const json& obj,
                               MessageSpec& message) {
    if (!obj.contains("fields") || !obj["fields"].is_array()) {
        std::printf("Spec message '%c' missing fields array\n", msg_type);
        return false;
    }

    message = MessageSpec();
    message.msg_type = msg_type;
    message.name = obj.value("name", "");

    uint32_t offset = 0;
    const json& fields = obj["fields"];
    for (size_t i = 0; i < fields.size(); i++) {
        const json& field_json = fields[i];

        FieldSpec field;
        field.name = field_json.value("name", "");
        field.size = (uint32_t)field_json.value("size", 0);
        field.offset = offset;

        std::string type_name = field_json.value("type", "");
        if (!parse_field_type(type_name, field.type)) {
            std::printf("Spec message '%c' field '%s' has unknown type '%s'\n",
                        msg_type, field.name.c_str(), type_name.c_str());
            return false;
        }

        offset += field.size;
        message.fields.push_back(field);
    }

    message.total_length = offset;
    return true;
}

static bool load_message_section(const json& section,
                                 std::unordered_map<char, MessageSpec>& specs) {
    if (!section.is_object()) {
        std::printf("Spec section is not an object\n");
        return false;
    }

    specs.clear();
    specs.reserve(section.size());

    for (json::const_iterator it = section.begin(); it != section.end(); ++it) {
        const std::string& msg_key = it.key();
        if (msg_key.size() != 1) {
            std::printf("Spec message key '%s' must be one character\n",
                        msg_key.c_str());
            return false;
        }

        MessageSpec message;
        char msg_type = msg_key[0];
        if (!parse_message_spec(msg_type, it.value(), message)) {
            return false;
        }

        specs[msg_type] = message;
    }

    return true;
}

static void rebuild_spec_index(AppConfig& cfg) {
    cfg.outbound_spec_by_type.fill(nullptr);
    cfg.inbound_spec_by_type.fill(nullptr);

    for (std::unordered_map<char, MessageSpec>::const_iterator it =
             cfg.outbound_specs.begin();
         it != cfg.outbound_specs.end(); ++it) {
        cfg.outbound_spec_by_type[(unsigned char)it->first] = &it->second;
    }

    for (std::unordered_map<char, MessageSpec>::const_iterator it =
             cfg.inbound_specs.begin();
         it != cfg.inbound_specs.end(); ++it) {
        cfg.inbound_spec_by_type[(unsigned char)it->first] = &it->second;
    }
}

static bool load_message_specs(const std::string& spec_path, AppConfig& cfg) {
    std::ifstream file(spec_path.c_str());
    if (!file) {
        std::printf("Failed to open spec: %s\n", spec_path.c_str());
        return false;
    }

    json root;
    try {
        file >> root;
    } catch (const std::exception& ex) {
        std::printf("Failed to parse spec %s: %s\n",
                    spec_path.c_str(), ex.what());
        return false;
    }

    cfg.outbound_specs.clear();
    cfg.inbound_specs.clear();

    bool has_directional = root.contains("outbound") || root.contains("inbound");
    if (has_directional) {
        if (root.contains("outbound") &&
            !load_message_section(root["outbound"], cfg.outbound_specs)) {
            return false;
        }
        if (root.contains("inbound") &&
            !load_message_section(root["inbound"], cfg.inbound_specs)) {
            return false;
        }
    } else if (!load_message_section(root, cfg.outbound_specs)) {
        return false;
    }

    rebuild_spec_index(cfg);
    return true;
}

static std::string resolve_path(const char* config_path,
                                const std::string& relative_path) {
    if (relative_path.empty()) return "";
    if (relative_path[0] == '/') return relative_path;

    std::string config_file = config_path;
    size_t last_slash = config_file.find_last_of('/');
    if (last_slash == std::string::npos) return relative_path;

    return config_file.substr(0, last_slash) + "/" + relative_path;
}

static bool find_session(const YamlConfig& yaml,
                         const std::string& sessions_prefix,
                         const std::string& session_key,
                         SessionConfig& session) {
    for (int index = 0; index < 100; index++) {
        char index_text[16];
        std::snprintf(index_text, sizeof(index_text), "%d", index);
        std::string item_prefix = sessions_prefix + "." + index_text;

        std::string item_key = yaml.get(item_prefix + ".key");
        if (item_key.empty()) {
            break;
        }

        if (item_key != session_key) {
            continue;
        }

        session.key = session_key;
        session.server_ip = yaml.get(item_prefix + ".server_ip");
        session.server_port =
            (uint16_t)yaml.get_int(item_prefix + ".server_port");
        session.username = yaml.get(item_prefix + ".username");
        session.password = yaml.get(item_prefix + ".password");
        return true;
    }

    return false;
}

static void build_field_index(AppConfig& cfg) {
    cfg.security_field_offset = -1;
    cfg.security_field_size = 0;
    cfg.order_number_field_offset = -1;
    cfg.order_number_field_size = 0;

    for (std::unordered_map<char, MessageSpec>::const_iterator it =
             cfg.outbound_specs.begin();
         it != cfg.outbound_specs.end(); ++it) {
        const std::vector<FieldSpec>& fields = it->second.fields;

        for (size_t i = 0; i < fields.size(); i++) {
            const FieldSpec& field = fields[i];

            if (cfg.security_field_offset < 0 &&
                (field.name == "SecurityId" || field.name == "OrderbookId")) {
                cfg.security_field_offset = (int)field.offset;
                cfg.security_field_size = (int)field.size;
            }

            if (cfg.order_number_field_offset < 0 &&
                field.name == "OrderNumber") {
                cfg.order_number_field_offset = (int)field.offset;
                cfg.order_number_field_size = (int)field.size;
            }
        }
    }
}

} // namespace

AppConfig::AppConfig()
    : security_field_offset(-1),
      security_field_size(0),
      order_number_field_offset(-1),
      order_number_field_size(0) {
    outbound_spec_by_type.fill(nullptr);
    inbound_spec_by_type.fill(nullptr);
}

bool load_config(const char* config_path,
                 const std::string& mode,
                 const std::string& session_key) {
    YamlConfig yaml;
    if (!parse_yaml(config_path, yaml)) {
        std::printf("Failed to parse config: %s\n", config_path);
        return false;
    }

    std::string protocol_prefix = "protocols." + mode;
    std::string spec_path = yaml.get(protocol_prefix + ".protocol_spec");
    if (spec_path.empty()) {
        std::printf("Protocol '%s' not found or missing protocol_spec\n",
                    mode.c_str());
        return false;
    }

    ProtocolConfig protocol;
    protocol.name = mode;
    protocol.protocol_spec = resolve_path(config_path, spec_path);
    protocol.heartbeat_interval_sec =
        yaml.get_int(protocol_prefix + ".heartbeat_interval_sec", 15);
    protocol.max_reconnect_attempts =
        yaml.get_int(protocol_prefix + ".max_reconnect_attempts", 10);
    protocol.reconnect_delay_sec =
        yaml.get_int(protocol_prefix + ".reconnect_delay_sec", 5);

    SessionConfig session;
    if (!find_session(yaml, protocol_prefix + ".sessions",
                      session_key, session)) {
        std::printf("Session '%s' not found in protocol '%s'\n",
                    session_key.c_str(), mode.c_str());
        return false;
    }

    if (session.server_ip.empty()) {
        std::printf("Session '%s' missing server_ip\n", session_key.c_str());
        return false;
    }

    if (session.server_port == 0) {
        std::printf("Session '%s' missing server_port\n", session_key.c_str());
        return false;
    }

    AppConfig loaded;
    loaded.protocol = protocol;
    loaded.session = session;

    if (!load_message_specs(loaded.protocol.protocol_spec, loaded)) {
        return false;
    }

    build_field_index(loaded);
    app_config = loaded;
    rebuild_spec_index(app_config);
    return true;
}

const AppConfig& config() {
    return app_config;
}

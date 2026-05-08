#include "run_modes.h"

#include "protocol.h"
#include "decoder.h"
#include "scenario.h"
#include "soup_session.h"
#include "soupbintcp.h"
#include "tcp_socket.h"
#include "token_store.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr int OUCH_IDLE_TIMEOUT_MS = 1000;
constexpr int OUCH_POLL_INTERVAL_MS = 100;

struct OuchState {
    Clock::time_point last_send;
    Clock::time_point last_data;
    uint32_t max_token;
    uint64_t accepted_live;
    uint64_t accepted_other;
    uint64_t rejected;
    uint64_t terminal;
    bool ended;
    bool no_response_warning_printed;

    OuchState()
        : last_send(Clock::now()),
          last_data(last_send),
          max_token(0),
          accepted_live(0),
          accepted_other(0),
          rejected(0),
          terminal(0),
          ended(false),
          no_response_warning_printed(false) {
    }
};

static int heartbeat_interval_ms(const ProtocolConfig& protocol) {
    return protocol_heartbeat_ms(protocol, 1000);
}

static int elapsed_ms(Clock::time_point since) {
    return (int)std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - since).count();
}

static bool is_token_field(const FieldSpec& field) {
    if (field.type != FIELD_UINT32) {
        return false;
    }

    static const char suffix[] = "Token";
    static const size_t suffix_len = sizeof(suffix) - 1;
    if (field.name.size() < suffix_len) {
        return false;
    }

    return field.name.compare(field.name.size() - suffix_len,
                              suffix_len, suffix) == 0;
}

static const FieldSpec* find_field(const MessageSpec& spec, const char* name) {
    for (size_t i = 0; i < spec.fields.size(); i++) {
        if (spec.fields[i].name == name) {
            return &spec.fields[i];
        }
    }
    return 0;
}

static bool field_fits(const FieldSpec* field, uint16_t payload_length) {
    return field && field->offset + field->size <= payload_length;
}

static bool read_u32_field(const MessageSpec& spec,
                           const uint8_t* payload,
                           uint16_t payload_length,
                           const char* name,
                           uint32_t& out) {
    const FieldSpec* field = find_field(spec, name);
    if (!field_fits(field, payload_length) || field->type != FIELD_UINT32) {
        return false;
    }

    out = read_u32_be(payload + field->offset);
    return true;
}

static bool read_u64_field(const MessageSpec& spec,
                           const uint8_t* payload,
                           uint16_t payload_length,
                           const char* name,
                           uint64_t& out) {
    const FieldSpec* field = find_field(spec, name);
    if (!field_fits(field, payload_length) || field->type != FIELD_UINT64) {
        return false;
    }

    out = read_u64_be(payload + field->offset);
    return true;
}

static bool read_char_field(const MessageSpec& spec,
                            const uint8_t* payload,
                            uint16_t payload_length,
                            const char* name,
                            char& out) {
    const FieldSpec* field = find_field(spec, name);
    if (!field_fits(field, payload_length) || field->type != FIELD_CHAR) {
        return false;
    }

    out = (char)payload[field->offset];
    return true;
}

static void update_max_token(const uint8_t* payload,
                             uint16_t payload_length,
                             const AppConfig& cfg,
                             uint32_t& max_token) {
    if (!payload || payload_length == 0) {
        return;
    }

    char msg_type = (char)payload[0];
    const MessageSpec* spec = cfg.outbound_spec_by_type[(unsigned char)msg_type];
    if (!spec) {
        return;
    }

    for (size_t i = 0; i < spec->fields.size(); i++) {
        const FieldSpec& field = spec->fields[i];
        if (!is_token_field(field) || field.offset + field.size > payload_length) {
            continue;
        }

        uint32_t token = read_u32_be(payload + field.offset);
        if (token > max_token) {
            max_token = token;
        }
    }
}

static void print_token(const char* label, bool ok, uint32_t token) {
    if (ok) {
        std::printf("%s=%u", label, token);
    } else {
        std::printf("%s=?", label);
    }
}

static void note_ouch_response(const uint8_t* payload,
                               uint16_t payload_length,
                               const AppConfig& cfg,
                               OuchState& state) {
    if (!payload || payload_length == 0) {
        return;
    }

    char msg_type = (char)payload[0];
    const MessageSpec* spec = cfg.outbound_spec_by_type[(unsigned char)msg_type];
    if (!spec) {
        return;
    }

    uint32_t token = 0;
    bool has_token = read_u32_field(*spec, payload, payload_length,
                                    "OrderToken", token);

    if (msg_type == 'A') {
        char order_state = '?';
        uint64_t order_number = 0;
        bool has_state = read_char_field(*spec, payload, payload_length,
                                         "OrderState", order_state);
        bool has_order_number = read_u64_field(*spec, payload, payload_length,
                                               "OrderNumber", order_number);

        if (has_state && order_state == 'L') {
            state.accepted_live++;
            if (state.accepted_live == 1) {
                std::printf("OUCH live order confirmed: ");
                print_token("token", has_token, token);
                if (has_order_number) {
                    std::printf(", order_number=%llu",
                                (unsigned long long)order_number);
                }
                std::printf("\n");
            }
            return;
        }

        state.accepted_other++;
        std::printf("OUCH order accepted but not live: ");
        print_token("token", has_token, token);
        std::printf(", order_state=%c\n", has_state ? order_state : '?');
        return;
    }

    if (msg_type == 'J') {
        char reason = '?';
        bool has_reason = read_char_field(*spec, payload, payload_length,
                                          "OrderRejectedReason", reason);

        state.rejected++;
        std::printf("OUCH order rejected: ");
        print_token("token", has_token, token);
        std::printf(", reason=%c\n", has_reason ? reason : '?');
        return;
    }

    if (msg_type == 'C' || msg_type == 'D') {
        state.terminal++;
        std::printf("OUCH order canceled: ");
        print_token("token", has_token, token);
        std::printf(", type=%c\n", msg_type);
        return;
    }

    if (msg_type == 'E') {
        state.terminal++;
        std::printf("OUCH order executed: ");
        print_token("token", has_token, token);
        std::printf("\n");
    }
}

static void maybe_warn_no_order_response(const AppArgs& args,
                                         OuchState& state) {
    if (!args.listen || state.no_response_warning_printed) {
        return;
    }
    if (state.accepted_live || state.accepted_other ||
        state.rejected || state.terminal) {
        return;
    }
    if (elapsed_ms(state.last_send) < OUCH_IDLE_TIMEOUT_MS) {
        return;
    }

    state.no_response_warning_printed = true;
    std::printf("OUCH listen is active, but no order response arrived yet. "
                "Check token sequence with --sync-tokens -v and confirm "
                "the scenario orderbook/group matches the book you inspect.\n");
}

static bool maybe_send_heartbeat(TcpSocket& sock,
                                 OuchState& state,
                                 int heartbeat_ms) {
    if (elapsed_ms(state.last_send) < heartbeat_ms) {
        return true;
    }

    if (!send_heartbeat(sock)) {
        return false;
    }

    state.last_send = Clock::now();
    return true;
}

static bool handle_ouch_packet(const SoupPacket& packet,
                               const AppConfig& cfg,
                               const AppArgs& args,
                               OuchState& state) {
    const uint8_t* payload = packet.payload.data();
    uint16_t payload_length = (uint16_t)packet.payload.size();

    if (packet.packet_type == SOUP_SEQUENCED_DATA) {
        if (payload_length == 0) {
            return true;
        }

        state.last_data = Clock::now();
        update_max_token(payload, payload_length, cfg, state.max_token);

        char prefix[64];
        std::snprintf(prefix, sizeof(prefix),
                      "<< (%u, 'S'", (unsigned)packet.packet_length);
        decode_ouch_message(payload, payload_length, cfg,
                            std::string(prefix), args.verbose);
        note_ouch_response(payload, payload_length, cfg, state);
        return true;
    }

    if (packet.packet_type == SOUP_SERVER_HEARTBEAT) {
        if (args.verbose) {
            std::printf("<< (%u, 'H')\n", (unsigned)packet.packet_length);
        }
        return true;
    }

    if (packet.packet_type == SOUP_END_OF_SESSION) {
        std::printf("<< (%u, 'Z')\n", (unsigned)packet.packet_length);
        state.ended = true;
        return true;
    }

    if (packet.packet_type == SOUP_DEBUG) {
        if (args.verbose && payload_length > 0) {
            std::printf("<< (%u, '+', '%.*s')\n",
                        (unsigned)packet.packet_length,
                        (int)payload_length, (const char*)payload);
        }
        return true;
    }

    return true;
}

static bool poll_ouch(TcpSocket& sock,
                      const AppConfig& cfg,
                      const ProtocolConfig& protocol,
                      const AppArgs& args,
                      OuchState& state,
                      int timeout_ms,
                      bool& got_packet) {
    got_packet = false;

    SoupPacket packet;
    if (!poll_packet(sock, timeout_ms, packet, got_packet)) {
        return false;
    }

    if (!maybe_send_heartbeat(sock, state, heartbeat_interval_ms(protocol))) {
        return false;
    }

    if (!got_packet) {
        return true;
    }

    return handle_ouch_packet(packet, cfg, args, state);
}

static bool wait_until(TcpSocket& sock,
                       const AppConfig& cfg,
                       const ProtocolConfig& protocol,
                       const AppArgs& args,
                       OuchState& state,
                       Clock::time_point send_time) {
    while (!state.ended && Clock::now() < send_time) {
        int remaining_ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
            send_time - Clock::now()).count();
        int timeout_ms = std::max(1, std::min(OUCH_POLL_INTERVAL_MS, remaining_ms));

        bool got_packet = false;
        if (!poll_ouch(sock, cfg, protocol, args, state,
                       timeout_ms, got_packet)) {
            return false;
        }
    }

    return !state.ended;
}

static bool sync_tokens_from_server(TcpSocket& sock,
                                    const AppConfig& cfg,
                                    const ProtocolConfig& protocol,
                                    const SessionConfig& session,
                                    const AppArgs& args,
                                    OuchState& state) {
    std::printf("Syncing order tokens from OUCH sequenced responses...\n");

    state.last_data = Clock::now();
    while (!state.ended && elapsed_ms(state.last_data) < OUCH_IDLE_TIMEOUT_MS) {
        bool got_packet = false;
        if (!poll_ouch(sock, cfg, protocol, args, state,
                       OUCH_POLL_INTERVAL_MS, got_packet)) {
            return false;
        }
    }

    if (state.ended) {
        return false;
    }

    if (state.max_token == 0) {
        std::printf("No OUCH order token found during sync\n");
        return true;
    }

    return save_token_floor(session.username, state.max_token);
}

static bool reserve_tokens(const SessionConfig& session,
                           uint32_t token_count,
                           uint64_t order_count,
                           bool unlimited,
                           uint32_t& token_base) {
    token_base = 0;

    if (token_count == 0 || unlimited) {
        return true;
    }

    if (order_count > std::numeric_limits<uint32_t>::max() / token_count) {
        std::printf("Token reservation too large (orders=%llu, tokens/order=%u)\n",
                    (unsigned long long)order_count, token_count);
        return false;
    }

    uint32_t total_tokens = (uint32_t)(order_count * token_count);
    return next_tokens(session.username, total_tokens, token_base);
}

static bool prepare_batch(const SessionConfig& session,
                          const std::vector<Message>& templates,
                          uint32_t token_count,
                          bool unlimited,
                          uint32_t reserved_token_base,
                          uint64_t batch_index,
                          std::vector<Message>& batch) {
    batch = templates;

    if (token_count == 0) {
        return true;
    }

    uint32_t token_base = 0;
    if (unlimited) {
        if (!next_tokens(session.username, token_count, token_base)) {
            return false;
        }
    } else {
        uint64_t batch_base =
            (uint64_t)reserved_token_base + batch_index * (uint64_t)token_count;
        if (batch_base > std::numeric_limits<uint32_t>::max()) {
            std::printf("Token base overflow at batch %llu\n",
                        (unsigned long long)batch_index);
            return false;
        }
        token_base = (uint32_t)batch_base;
    }

    assign_tokens(batch, token_base);
    return true;
}

static bool send_ouch_message(TcpSocket& sock,
                              const Message& message,
                              const AppConfig& cfg,
                              const AppArgs& args,
                              OuchState& state,
                              uint64_t message_number) {
    const std::vector<uint8_t>& bytes = message.bytes;

    if (!sock.send_bytes(bytes.data(), (int)bytes.size())) {
        std::printf("Send failed on OUCH message %llu\n",
                    (unsigned long long)message_number);
        return false;
    }

    state.last_send = Clock::now();

    uint16_t packet_length = read_u16_be(bytes.data());
    char prefix[64];
    std::snprintf(prefix, sizeof(prefix),
                  ">> (%u, 'U'", (unsigned)packet_length);

    const uint8_t* ouch_payload = &bytes[3];
    uint16_t ouch_length = (uint16_t)(bytes.size() - 3);
    decode_ouch_message(ouch_payload, ouch_length, cfg,
                        std::string(prefix), args.verbose, true);
    return true;
}

static bool send_batches(TcpSocket& sock,
                         const AppConfig& cfg,
                         const ProtocolConfig& protocol,
                         const SessionConfig& session,
                         const AppArgs& args,
                         const std::vector<Message>& templates,
                         uint32_t token_count,
                         OuchState& state) {
    bool unlimited = args.order_count == 0;
    uint32_t reserved_token_base = 0;
    if (!reserve_tokens(session, token_count, args.order_count,
                        unlimited, reserved_token_base)) {
        return false;
    }

    uint64_t batch_index = 0;
    uint64_t sent_messages = 0;

    std::chrono::microseconds send_interval(0);
    if (args.send_rate > 0) {
        uint64_t micros = 1000000ULL / args.send_rate;
        if (micros == 0) {
            micros = 1;
        }
        send_interval = std::chrono::microseconds(micros);
    }

    Clock::time_point next_send_time = Clock::now();

    while (!state.ended && (unlimited || batch_index < args.order_count)) {
        std::vector<Message> batch;
        if (!prepare_batch(session, templates, token_count, unlimited,
                           reserved_token_base, batch_index, batch)) {
            return false;
        }

        for (size_t i = 0; i < batch.size(); i++) {
            if (args.send_rate > 0 &&
                !wait_until(sock, cfg, protocol, args, state, next_send_time)) {
                return false;
            }

            sent_messages++;
            if (!send_ouch_message(sock, batch[i], cfg, args,
                                   state, sent_messages)) {
                return false;
            }

            if (args.send_rate > 0) {
                next_send_time += send_interval;
            }
        }

        batch_index++;
    }

    return !state.ended;
}

static int receive_after_send(TcpSocket& sock,
                              const AppConfig& cfg,
                              const ProtocolConfig& protocol,
                              const AppArgs& args,
                              OuchState& state) {
    while (!state.ended) {
        if (!args.listen) {
            Clock::time_point last_activity =
                state.last_data > state.last_send ? state.last_data : state.last_send;
            if (elapsed_ms(last_activity) >= OUCH_IDLE_TIMEOUT_MS) {
                send_logout(sock);
                sock.close();
                return 0;
            }
        }

        bool got_packet = false;
        if (!poll_ouch(sock, cfg, protocol, args, state,
                       OUCH_POLL_INTERVAL_MS, got_packet)) {
            break;
        }

        maybe_warn_no_order_response(args, state);
    }

    sock.close();
    return state.ended ? 0 : 1;
}

} // namespace

int run_ouch(const AppArgs& args, Filter& filter) {
    (void)filter;

    const AppConfig& cfg = config();
    const ProtocolConfig& protocol = cfg.protocol;
    const SessionConfig& session = cfg.session;

    std::string scenario_path = args.scenario_file;
    if (scenario_path.empty()) {
        scenario_path = "scenarios/ouch_message.txt";
    }

    std::vector<Message> templates;
    uint32_t token_count = 0;
    if (!load_scenario(scenario_path, cfg, templates, token_count)) {
        return 1;
    }

    TcpSocket sock;
    std::string session_id;
    uint64_t current_seq = 0;

    uint64_t login_seq = args.has_start_seq
        ? args.start_seq
        : (args.sync_tokens ? 1 : 0);

    if (!connect_and_login(sock, session, login_seq,
                           session_id, current_seq, true)) {
        return 1;
    }

    OuchState state;

    if (args.sync_tokens &&
        !sync_tokens_from_server(sock, cfg, protocol, session, args, state)) {
        sock.close();
        return 1;
    }

    if (!send_batches(sock, cfg, protocol, session, args,
                      templates, token_count, state)) {
        sock.close();
        return state.ended ? 0 : 1;
    }

    return receive_after_send(sock, cfg, protocol, args, state);
}

#include "run_modes.h"

#include "protocol.h"
#include "decoder.h"
#include "soup_session.h"
#include "soupbintcp.h"
#include "tcp_socket.h"

#include <cstdio>

int run_glimpse(const AppArgs& args, Filter& filter) {
    const AppConfig& cfg = config();
    const ProtocolConfig& protocol = cfg.protocol;
    const SessionConfig& session = cfg.session;

    TcpSocket sock;
    std::string session_id;
    uint64_t current_seq = 0;
    uint64_t decoded_count = 0;

    if (!connect_and_login(sock, session, 1, session_id, current_seq)) {
        return 1;
    }

    SoupSession session_runner(sock, protocol, 15000);

    while (true) {
        SoupPacket packet;
        bool got_packet = false;
        if (!session_runner.poll(packet, got_packet)) {
            break;
        }
        if (!got_packet) {
            continue;
        }

        const uint8_t* payload = packet.payload.data();
        uint16_t payload_length = (uint16_t)packet.payload.size();

        if (packet.packet_type == SOUP_SEQUENCED_DATA) {
            if (payload_length == 0) {
                continue;
            }

            if ((char)payload[0] == 'G') {
                uint64_t realtime_next_sequence = 0;
                int sequence_offset = (payload_length >= 17) ? 9 : 1;
                if (sequence_offset + 8 <= payload_length) {
                    realtime_next_sequence = read_u64_be(payload + sequence_offset);
                }

                std::printf(">> {%u, 'S', 'G', %llu}\n",
                            (unsigned)packet.packet_length,
                            (unsigned long long)realtime_next_sequence);
                sock.close();
                return 0;
            }

            decoded_count++;

            if (!filter.passes(payload, payload_length, cfg)) {
                continue;
            }

            char prefix[64];
            std::snprintf(prefix, sizeof(prefix),
                          ">> {%u, 'S'", (unsigned)packet.packet_length);

            decode_itch_message(payload, payload_length, cfg,
                                std::string(prefix), args.verbose);

            if (args.max_messages != 0 &&
                decoded_count >= args.max_messages) {
                sock.close();
                return 0;
            }
            continue;
        }

        if (packet.packet_type == SOUP_SERVER_HEARTBEAT) {
            if (args.verbose) {
                std::printf(">> {%u, 'H'}\n", (unsigned)packet.packet_length);
            }

            if (!session_runner.send_heartbeat_if_due()) {
                break;
            }
            continue;
        }

        if (packet.packet_type == SOUP_END_OF_SESSION) {
            sock.close();
            return 0;
        }
    }

    sock.close();
    return 1;
}

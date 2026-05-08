#include "run_modes.h"

#include "protocol.h"
#include "decoder.h"
#include "soup_session.h"
#include "soupbintcp.h"
#include "tcp_socket.h"

#include <cstdio>
#include <unistd.h>

int run_itch(const AppArgs& args, Filter& filter) {
    const AppConfig& cfg = config();
    const ProtocolConfig& protocol = cfg.protocol;
    const SessionConfig& session = cfg.session;

    uint64_t login_seq = args.has_start_seq ? args.start_seq : 1;
    std::string session_id;
    uint64_t current_seq = 0;
    uint64_t decoded_count = 0;

    int reconnect_delay_sec = protocol.reconnect_delay_sec;
    if (reconnect_delay_sec <= 0) {
        reconnect_delay_sec = 5;
    }

    int reconnect_attempt = 0;

    while (true) {
        TcpSocket sock;

        if (!connect_and_login(sock, session, login_seq, session_id, current_seq)) {
            reconnect_attempt++;
            if (protocol.max_reconnect_attempts > 0 &&
                reconnect_attempt >= protocol.max_reconnect_attempts) {
                return 1;
            }
            ::sleep((unsigned)reconnect_delay_sec);
            continue;
        }

        reconnect_attempt = 0;

        SoupSession session_runner(sock, protocol, 15000);
        bool needs_reconnect = false;

        while (true) {
            SoupPacket packet;
            bool got_packet = false;
            if (!session_runner.poll(packet, got_packet)) {
                needs_reconnect = true;
                break;
            }
            if (!got_packet) {
                continue;
            }

            const uint8_t* payload = packet.payload.data();
            uint16_t payload_length = (uint16_t)packet.payload.size();

            if (packet.packet_type == SOUP_SEQUENCED_DATA) {
                current_seq++;

                if (payload_length == 0) {
                    continue;
                }

                decoded_count++;

                if (!filter.passes(payload, payload_length, cfg)) {
                    continue;
                }

                char prefix[128];
                std::snprintf(prefix, sizeof(prefix),
                              ">> {'%.*s', %llu",
                              (int)session_id.size(), session_id.c_str(),
                              (unsigned long long)current_seq);

                decode_itch_message(payload, payload_length, cfg,
                                    std::string(prefix), args.verbose);

                if (args.max_messages != 0 &&
                    decoded_count >= args.max_messages) {
                    send_logout(sock);
                    sock.close();
                    return 0;
                }
                continue;
            }

            if (packet.packet_type == SOUP_SERVER_HEARTBEAT) {
                if (args.verbose) {
                    std::printf(">> {%u, '0'}\n",
                                (unsigned)packet.packet_length);
                }

                if (!session_runner.send_heartbeat_if_due()) {
                    needs_reconnect = true;
                    break;
                }
                continue;
            }

            if (packet.packet_type == SOUP_END_OF_SESSION) {
                std::printf(">> {'%.*s', %llu, 'Z'}\n",
                            (int)session_id.size(), session_id.c_str(),
                            (unsigned long long)current_seq);
                sock.close();
                return 0;
            }

            if (packet.packet_type == SOUP_DEBUG) {
                if (args.verbose && payload_length > 0) {
                    std::printf(">> {%u, '+', '%.*s'}\n",
                                (unsigned)packet.packet_length,
                                (int)payload_length, (const char*)payload);
                }
                continue;
            }
        }

        sock.close();

        if (!needs_reconnect) {
            return 0;
        }

        login_seq = current_seq;
        reconnect_attempt++;

        if (protocol.max_reconnect_attempts > 0 &&
            reconnect_attempt >= protocol.max_reconnect_attempts) {
            return 1;
        }

        ::sleep((unsigned)reconnect_delay_sec);
    }
}

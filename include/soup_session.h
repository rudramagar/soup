#ifndef SOUP_SESSION_H
#define SOUP_SESSION_H

#include <ctime>
#include <cstdint>
#include <string>
#include <vector>

#include "protocol.h"
#include "tcp_socket.h"

// Buffer constants used by
// all protocol's
// receive loop.
constexpr int RECV_BUF_CAPACITY    = 64 * 1024;
constexpr int SOCKET_RECV_BUF_SIZE = 4 * 1024 * 1024;

struct SoupPacket {
    uint16_t packet_length;
    char packet_type;
    std::vector<uint8_t> payload;

    SoupPacket() : packet_length(0), packet_type(0) {}
};

// Big-endian
// integer readers or writers.
uint16_t read_u16_be(const uint8_t* src);
uint32_t read_u32_be(const uint8_t* src);
uint64_t read_u64_be(const uint8_t* src);
void write_u16_be(uint8_t* dst, uint16_t value);
void write_u32_be(uint8_t* dst, uint32_t value);
void write_u64_be(uint8_t* dst, uint64_t value);

// SoupBinTCP framed msg
// connect and login
bool connect_and_login(TcpSocket& sock,
                       const SessionConfig& session,
                       uint64_t requested_sequence,
                       std::string& session_id,
                       uint64_t& sequence_number,
                       bool is_ouch = false);

// send HB packet (1-byte payload)
bool send_heartbeat(TcpSocket& sock);

// send a logout request packet (1-byte payload)
bool send_logout(TcpSocket& sock);

// Read one SoupBinTCP packet.
bool recv_packet(TcpSocket& sock, SoupPacket& packet);

int protocol_heartbeat_ms(const ProtocolConfig& protocol, int default_ms);

bool poll_packet(TcpSocket& sock,
                 int timeout_ms,
                 SoupPacket& packet,
                 bool& got_packet);

class SoupSession {
public:
    SoupSession(TcpSocket& sock,
                const ProtocolConfig& protocol,
                int default_heartbeat_ms);

    bool poll(SoupPacket& packet, bool& got_packet);
    bool send_heartbeat_if_due();

private:
    TcpSocket& sock_;
    int heartbeat_interval_ms_;
    int server_timeout_sec_;
    time_t last_send_time_;
    time_t last_recv_time_;
};

// Read and discard
bool drain_payload(TcpSocket& sock,
                   uint8_t* scratch_buf,
                   int scratch_buf_capacity,
                   int bytes_to_drain);

#endif

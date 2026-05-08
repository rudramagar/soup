#ifndef FILTER_H
#define FILTER_H

#include <cstdint>
#include <string>
#include <vector>
#include "protocol.h"

inline uint64_t filter_read_u64_be(const uint8_t* src) {
    return ((uint64_t)src[0] << 56) | ((uint64_t)src[1] << 48) |
           ((uint64_t)src[2] << 40) | ((uint64_t)src[3] << 32) |
           ((uint64_t)src[4] << 24) | ((uint64_t)src[5] << 16) |
           ((uint64_t)src[6] << 8)  | ((uint64_t)src[7]);
}

struct Filter {
    bool has_type_filter = false;
    bool type_allowed[256] = {};

    bool has_security_filter = false;
    std::vector<std::string> securities;

    bool has_order_number_filter = false;
    std::vector<uint64_t> order_numbers;

    void add_type(char type) {
        has_type_filter = true;
        type_allowed[(unsigned char)type] = true;
    }

    void add_security(const std::string& code) {
        has_security_filter = true;
        securities.push_back(code);
    }

    void add_order_number(uint64_t num) {
        has_order_number_filter = true;
        order_numbers.push_back(num);
    }

    bool passes(const uint8_t* msg, uint16_t msg_len,
                const AppConfig& cfg) const {
        if (!msg || msg_len == 0) {
            return false;
        }

        if (has_type_filter && !type_allowed[(unsigned char)msg[0]]) {
            return false;
        }

        if (has_security_filter && cfg.security_field_offset >= 0) {
            int offset = cfg.security_field_offset;
            int size = cfg.security_field_size;

            if (offset + size <= (int)msg_len) {
                std::string value((const char*)(msg + offset), (size_t)size);
                size_t end = value.find_last_not_of(' ');
                if (end != std::string::npos) {
                    value = value.substr(0, end + 1);
                }

                bool match = false;
                for (size_t i = 0; i < securities.size(); i++) {
                    if (value == securities[i]) {
                        match = true;
                        break;
                    }
                }
                if (!match) {
                    return false;
                }
            }
        }

        if (has_order_number_filter && cfg.order_number_field_offset >= 0) {
            int offset = cfg.order_number_field_offset;
            int size = cfg.order_number_field_size;

            if (offset + size <= (int)msg_len && size == 8) {
                uint64_t value = filter_read_u64_be(msg + offset);
                bool match = false;
                for (size_t i = 0; i < order_numbers.size(); i++) {
                    if (value == order_numbers[i]) {
                        match = true;
                        break;
                    }
                }
                if (!match) {
                    return false;
                }
            }
        }

        return true;
    }
};

#endif

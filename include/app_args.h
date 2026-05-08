#ifndef APP_ARGS_H
#define APP_ARGS_H

#include <cstdint>
#include <string>

struct AppArgs {
    std::string mode;
    std::string session_key;
    std::string scenario_file;

    bool has_start_seq;
    uint64_t start_seq;
    uint64_t max_messages;
    bool verbose;

    uint64_t order_count;
    uint32_t send_rate;
    bool listen;
    bool sync_tokens;

    AppArgs()
        : has_start_seq(false),
          start_seq(0),
          max_messages(0),
          verbose(false),
          order_count(1),
          send_rate(0),
          listen(false),
          sync_tokens(false) {
    }
};

#endif

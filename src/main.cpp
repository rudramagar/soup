#include "app_args.h"
#include "filter.h"
#include "protocol.h"
#include "run_modes.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <getopt.h>

static void usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s --mode <protocol> -u <session_key> [options]\n\n"
        "Required:\n"
        "  --mode <protocol>   protocols (itch, glimpse, ouch)\n"
        "  -u <session_key>    sessions\n\n"
        "Options:\n"
        "  -s <seq>            start from sequence number\n"
        "  -n <count>          stop after N messages\n"
        "  -v                  verbose mode\n"
        "  --type <X>          filter by message type (repeatable)\n"
        "  --security <code>   filter by SecurityId/OrderbookId (repeatable)\n"
        "  --ordernum <num>    filter by OrderNumber (repeatable)\n"
        "  --scenario <path>   OUCH scenario file (.ouch, .txt, or no suffix)\n"
        "  --order-count <N>   OUCH: send scenario N times (0 = forever)\n"
        "  --rate <N>          OUCH: max outbound messages per second\n"
        "  --listen            OUCH: stay connected after sending\n"
        "  --sync-tokens       OUCH: replay responses and save max token before sending\n"
        "  -h                  show help\n",
        prog);
}

static bool parse_u64_arg(const char* text, uint64_t& out) {
    if (!text || std::strlen(text) == 0) {
        return false;
    }

    char* end = 0;
    unsigned long long value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0') {
        return false;
    }

    out = (uint64_t)value;
    return true;
}

static int run_app(const AppArgs& args, Filter& filter) {
    const char* config_path = "config/config.yaml";
    if (!load_config(config_path, args.mode, args.session_key)) {
        return 1;
    }

    if (args.mode == "itch") {
        return run_itch(args, filter);
    }

    if (args.mode == "glimpse") {
        return run_glimpse(args, filter);
    }

    if (args.mode == "ouch") {
        return run_ouch(args, filter);
    }

    std::printf("Unknown mode: %s\n", args.mode.c_str());
    return 1;
}

int main(int argc, char** argv) {
    // Line-buffered stdout/stderr
    std::setvbuf(stdout, 0, _IOLBF, 0);
    std::setvbuf(stderr, 0, _IOLBF, 0);

    AppArgs args;
    Filter filter;

    static struct option long_options[] = {
        {"mode",     required_argument, 0, 1001},
        {"type",     required_argument, 0, 1002},
        {"security", required_argument, 0, 1003},
        {"ordernum", required_argument, 0, 1004},
        {"scenario", required_argument, 0, 1005},
        {"order-count", required_argument, 0, 1006},
        {"rate", required_argument, 0, 1007},
        {"listen", no_argument, 0, 1008},
        {"sync-tokens", no_argument, 0, 1009},
        {0, 0, 0, 0}
    };

    int opt;
    int long_index = 0;

    while ((opt = getopt_long(argc, argv, "u:s:n:vh", long_options, &long_index)) != -1) {
        switch (opt) {

        case 1001:
            args.mode = optarg;
            break;

        case 1002:
            if (!optarg || std::strlen(optarg) != 1) {
                std::fprintf(stderr, "Invalid --type (expect single char): %s\n",
                        optarg ? optarg : "(null)");
                usage(argv[0]);
                return 1;
            }
            filter.add_type(optarg[0]);
            break;

        case 1003:
            if (!optarg || std::strlen(optarg) == 0) {
                std::fprintf(stderr, "Invalid --security\n");
                usage(argv[0]);
                return 1;
            }
            filter.add_security(optarg);
            break;

        case 1004: {
            uint64_t value = 0;
            if (!parse_u64_arg(optarg, value)) {
                std::fprintf(stderr, "Invalid --ordernum: %s\n", optarg);
                usage(argv[0]);
                return 1;
            }
            filter.add_order_number(value);
            break;
        }

        case 1005:
            if (!optarg || std::strlen(optarg) == 0) {
                std::fprintf(stderr, "Invalid --scenario\n");
                usage(argv[0]);
                return 1;
            }
            args.scenario_file = optarg;
            break;

        case 1006: {
            uint64_t value = 0;
            if (!parse_u64_arg(optarg, value)) {
                std::fprintf(stderr, "Invalid --order-count: %s\n", optarg);
                usage(argv[0]);
                return 1;
            }
            args.order_count = value;
            break;
        }

        case 1007: {
            uint64_t value = 0;
            if (!parse_u64_arg(optarg, value) || value > 0xFFFFFFFFULL) {
                std::fprintf(stderr, "Invalid --rate: %s\n", optarg);
                usage(argv[0]);
                return 1;
            }
            args.send_rate = (uint32_t)value;
            break;
        }

        case 1008:
            args.listen = true;
            break;

        case 1009:
            args.sync_tokens = true;
            break;

        case 'u':
            args.session_key = optarg;
            break;

        case 's': {
            uint64_t value = 0;
            if (!parse_u64_arg(optarg, value)) {
                std::fprintf(stderr, "Invalid -s: %s\n", optarg);
                usage(argv[0]);
                return 1;
            }
            args.has_start_seq = true;
            args.start_seq = value;
            break;
        }

        case 'n': {
            uint64_t value = 0;
            if (!parse_u64_arg(optarg, value)) {
                std::fprintf(stderr, "Invalid -n: %s\n", optarg);
                usage(argv[0]);
                return 1;
            }
            args.max_messages = value;
            break;
        }

        case 'v':
            args.verbose = true;
            break;

        case 'h':
            usage(argv[0]);
            return 0;

        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (args.mode.empty()) {
        std::fprintf(stderr, "Error: --mode is required\n\n");
        usage(argv[0]);
        return 1;
    }

    if (args.session_key.empty()) {
        std::fprintf(stderr, "Error: -u is required\n\n");
        usage(argv[0]);
        return 1;
    }

    return run_app(args, filter);
}

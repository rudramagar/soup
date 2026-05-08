#ifndef TOKEN_STORE_H
#define TOKEN_STORE_H

#include <cstdint>
#include <string>

bool next_tokens(const std::string& username,
                 uint32_t count,
                 uint32_t& out_base);

bool save_token_floor(const std::string& username, uint32_t min_value);

#endif

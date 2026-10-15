#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace puse::core {

bool ReleasePartyMon(std::vector<uint8_t> &buffer, int party_index, std::string *error = nullptr);

bool DepositPartyMon(
    std::vector<uint8_t> &buffer,
    std::vector<uint8_t> &pc_stream,
    int party_index,
    int box,
    int slot,
    bool swap_if_occupied,
    std::string *error = nullptr
);

bool WithdrawPcMon(
    std::vector<uint8_t> &buffer,
    std::vector<uint8_t> &pc_stream,
    int box,
    int slot,
    int swap_party_index,
    std::string *error = nullptr
);

bool InsertPartyMon(
    std::vector<uint8_t> &buffer,
    uint16_t species_id,
    int level,
    const std::string &nickname,
    uint32_t otid,
    const std::string &ot_name,
    const std::unordered_map<int, std::string> &species_db,
    std::string *error = nullptr
);

} // namespace puse::core

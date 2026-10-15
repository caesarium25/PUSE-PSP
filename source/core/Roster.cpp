#include <puse/core/Roster.hpp>

#include <puse/core/Binary.hpp>
#include <puse/core/GameProfile.hpp>
#include <puse/core/Party.hpp>
#include <puse/core/Pc.hpp>
#include <puse/core/SaveSections.hpp>

#include <algorithm>
#include <array>
#include <cstring>

namespace puse::core {
namespace {

constexpr size_t kPartyCountOff = profile::kStoragePartyCountOffset;
constexpr size_t kPartyStart = profile::kStoragePartyStart;
constexpr size_t kPartySize = profile::kStoragePartyRecordSize;
constexpr size_t kPcSize = profile::kStoragePcRecordSize;

bool FindTrainer(std::vector<uint8_t> &buffer, SaveSection *section, std::string *error) {
    for (const auto &candidate : ActiveUnboundSections(buffer)) {
        if (candidate.section_id == profile::kStoragePartySectionId) {
            *section = candidate;
            return true;
        }
    }
    if (error) *error = "trainer section not found";
    return false;
}

size_t PcOffset(const int box, const int slot) {
    return static_cast<size_t>(((box - 1) * kPcBoxSlotCount + (slot - 1)) * static_cast<int>(kPcSize));
}

bool ValidPcSlot(const std::vector<uint8_t> &stream, const int box, const int slot) {
    if (box < 1 || box > PcStreamBoxCount() || slot < 1 || slot > kPcBoxSlotCount) return false;
    return PcOffset(box, slot) + kPcSize <= stream.size();
}

bool PcOccupied(const uint8_t *pc) {
    return ReadU16Le(pc, profile::kCodecsPcFieldsSpecies) != 0;
}

std::array<uint16_t, 4> UnpackMoves(const uint8_t *pc) {
    uint64_t packed = 0;
    for (int i = 0; i < 5; ++i) {
        packed |= static_cast<uint64_t>(pc[profile::kCodecsPcFieldsMoveIds + static_cast<size_t>(i)]) << (i * 8);
    }
    std::array<uint16_t, 4> moves{};
    for (int i = 0; i < 4; ++i) moves[static_cast<size_t>(i)] = static_cast<uint16_t>((packed >> (i * 10)) & 0x3FFU);
    return moves;
}

void PackMoves(uint8_t *pc, const std::array<uint16_t, 4> &moves) {
    uint64_t packed = 0;
    for (int i = 0; i < 4; ++i) packed |= (static_cast<uint64_t>(moves[static_cast<size_t>(i)] & 0x3FFU) << (i * 10));
    for (int i = 0; i < 5; ++i) pc[profile::kCodecsPcFieldsMoveIds + static_cast<size_t>(i)] = static_cast<uint8_t>((packed >> (i * 8)) & 0xFFU);
}

void PcToParty(const uint8_t *pc, uint8_t *party) {
    std::fill(party, party + kPartySize, 0);
    std::memcpy(party + 0, pc + 0, 28);

    uint8_t *growth = party + profile::kCodecsPartyFieldsDataStart;
    uint8_t *attacks = growth + 12;
    uint8_t *evs = growth + 24;
    uint8_t *misc = growth + 36;

    WriteU16Le(growth, 0, ReadU16Le(pc, profile::kCodecsPcFieldsSpecies));
    WriteU16Le(growth, 2, ReadU16Le(pc, profile::kCodecsPcFieldsItem));
    WriteU32Le(growth, 4, ReadU32Le(pc, profile::kCodecsPcFieldsExp));
    growth[8] = pc[profile::kCodecsPcFieldsPpUps];
    growth[9] = pc[profile::kCodecsPcFieldsHappiness];
    growth[10] = pc[profile::kCodecsPcFieldsBall];

    const auto moves = UnpackMoves(pc);
    for (size_t i = 0; i < moves.size(); ++i) {
        WriteU16Le(attacks, i * 2, moves[i]);
        const int pp_ups = (growth[8] >> (i * 2)) & 3;
        attacks[8 + i] = static_cast<uint8_t>(std::clamp(CalcMaxMovePp(moves[i], pp_ups), 0, 255));
    }
    std::memcpy(evs, pc + profile::kCodecsPcFieldsEvs, 6);
    WriteU32Le(misc, 4, ReadU32Le(pc, profile::kCodecsPcFieldsIvs));

    const uint16_t species = ReadU16Le(growth, 0);
    int growth_rate = GetSpeciesGrowthRate(species);
    if (growth_rate < 0) growth_rate = 0;
    const int level = CalcLevelFromExp(growth_rate, ReadU32Le(growth, 4));
    party[profile::kCodecsPartyFieldsLevelVisual] = static_cast<uint8_t>(std::clamp(level, 1, 100));

    std::array<uint8_t, 6> ivs{};
    const uint32_t packed_ivs = ReadU32Le(misc, 4);
    for (size_t i = 0; i < ivs.size(); ++i) ivs[i] = static_cast<uint8_t>((packed_ivs >> (i * 5)) & 31U);
    std::array<uint8_t, 6> party_evs{};
    std::copy(evs, evs + 6, party_evs.begin());
    const auto preview = CalculateBattlePreview(species, level, static_cast<uint8_t>(ReadU32Le(party, 0) % 25U), ivs, party_evs);
    if (preview.available) {
        WriteU16Le(party, profile::kCodecsPartyFieldsCurrHp, preview.stats[0]);
        WriteU16Le(party, profile::kCodecsPartyFieldsMaxHp, preview.stats[0]);
        WriteU16Le(party, profile::kCodecsPartyFieldsAtk, preview.stats[1]);
        WriteU16Le(party, profile::kCodecsPartyFieldsDef, preview.stats[2]);
        WriteU16Le(party, profile::kCodecsPartyFieldsSpe, preview.stats[3]);
        WriteU16Le(party, profile::kCodecsPartyFieldsSpa, preview.stats[4]);
        WriteU16Le(party, profile::kCodecsPartyFieldsSpd, preview.stats[5]);
    }
    WriteU16Le(party, profile::kCodecsPartyFieldsChecksum, 0);
}

void PartyToPc(const uint8_t *party, uint8_t *pc) {
    std::fill(pc, pc + kPcSize, 0);
    std::memcpy(pc + 0, party + 0, 28);
    const uint8_t *growth = party + profile::kCodecsPartyFieldsDataStart;
    const uint8_t *attacks = growth + 12;
    const uint8_t *evs = growth + 24;
    const uint8_t *misc = growth + 36;

    WriteU16Le(pc, profile::kCodecsPcFieldsSpecies, ReadU16Le(growth, 0));
    WriteU16Le(pc, profile::kCodecsPcFieldsItem, ReadU16Le(growth, 2));
    WriteU32Le(pc, profile::kCodecsPcFieldsExp, ReadU32Le(growth, 4));
    pc[profile::kCodecsPcFieldsPpUps] = growth[8];
    pc[profile::kCodecsPcFieldsHappiness] = growth[9];
    pc[profile::kCodecsPcFieldsBall] = growth[10];
    std::array<uint16_t, 4> moves{};
    for (size_t i = 0; i < moves.size(); ++i) moves[i] = ReadU16Le(attacks, i * 2);
    PackMoves(pc, moves);
    std::memcpy(pc + profile::kCodecsPcFieldsEvs, evs, 6);
    WriteU32Le(pc, profile::kCodecsPcFieldsIvs, ReadU32Le(misc, 4));
}

void ClearAndShiftParty(uint8_t *trainer, const int index, const int count) {
    if (index < count - 1) {
        std::memmove(trainer + kPartyStart + static_cast<size_t>(index) * kPartySize,
            trainer + kPartyStart + static_cast<size_t>(index + 1) * kPartySize,
            static_cast<size_t>(count - index - 1) * kPartySize);
    }
    std::fill(trainer + kPartyStart + static_cast<size_t>(count - 1) * kPartySize,
        trainer + kPartyStart + static_cast<size_t>(count) * kPartySize, 0);
    WriteU32Le(trainer, kPartyCountOff, static_cast<uint32_t>(count - 1));
}

bool FinalizeTrainer(std::vector<uint8_t> &buffer, const SaveSection &section, std::string *error) {
    uint8_t *trainer = buffer.data() + section.offset;
    const int count = std::clamp(static_cast<int>(ReadU32Le(trainer, kPartyCountOff)), 0, 6);
    for (int i = 0; i < count; ++i) {
        WriteU16Le(trainer + kPartyStart + static_cast<size_t>(i) * kPartySize,
            profile::kCodecsPartyFieldsChecksum, 0);
    }
    if (!RecalculateSectionChecksum(buffer, section.offset)) {
        if (error) *error = "could not update trainer section checksum";
        return false;
    }
    return true;
}

} // namespace

bool ReleasePartyMon(std::vector<uint8_t> &buffer, const int party_index, std::string *error) {
    SaveSection section{};
    if (!FindTrainer(buffer, &section, error)) return false;
    uint8_t *trainer = buffer.data() + section.offset;
    const int count = std::clamp(static_cast<int>(ReadU32Le(trainer, kPartyCountOff)), 0, 6);
    if (count <= 1) {
        if (error) *error = "the last party Pokemon cannot be released";
        return false;
    }
    if (party_index < 0 || party_index >= count) {
        if (error) *error = "party index out of range";
        return false;
    }
    ClearAndShiftParty(trainer, party_index, count);
    return FinalizeTrainer(buffer, section, error);
}

bool DepositPartyMon(std::vector<uint8_t> &buffer, std::vector<uint8_t> &pc_stream,
    const int party_index, const int box, const int slot, const bool swap_if_occupied, std::string *error) {
    if (!ValidPcSlot(pc_stream, box, slot)) {
        if (error) *error = "PC destination is out of range";
        return false;
    }
    SaveSection section{};
    if (!FindTrainer(buffer, &section, error)) return false;
    uint8_t *trainer = buffer.data() + section.offset;
    const int count = std::clamp(static_cast<int>(ReadU32Le(trainer, kPartyCountOff)), 0, 6);
    if (party_index < 0 || party_index >= count) {
        if (error) *error = "party index out of range";
        return false;
    }
    uint8_t *party = trainer + kPartyStart + static_cast<size_t>(party_index) * kPartySize;
    uint8_t *pc = pc_stream.data() + PcOffset(box, slot);
    const bool occupied = PcOccupied(pc);
    if (occupied && !swap_if_occupied) {
        if (error) *error = "PC destination is occupied";
        return false;
    }
    std::array<uint8_t, kPcSize> outgoing{};
    PartyToPc(party, outgoing.data());
    if (occupied) {
        std::array<uint8_t, kPartySize> incoming{};
        PcToParty(pc, incoming.data());
        std::copy(incoming.begin(), incoming.end(), party);
    } else {
        if (count <= 1) {
            if (error) *error = "the last party Pokemon cannot be deposited";
            return false;
        }
        ClearAndShiftParty(trainer, party_index, count);
    }
    std::copy(outgoing.begin(), outgoing.end(), pc);
    return FinalizeTrainer(buffer, section, error);
}

bool WithdrawPcMon(std::vector<uint8_t> &buffer, std::vector<uint8_t> &pc_stream,
    const int box, const int slot, const int swap_party_index, std::string *error) {
    if (!ValidPcSlot(pc_stream, box, slot)) {
        if (error) *error = "PC source is out of range";
        return false;
    }
    uint8_t *pc = pc_stream.data() + PcOffset(box, slot);
    if (!PcOccupied(pc)) {
        if (error) *error = "PC source is empty";
        return false;
    }
    SaveSection section{};
    if (!FindTrainer(buffer, &section, error)) return false;
    uint8_t *trainer = buffer.data() + section.offset;
    const int count = std::clamp(static_cast<int>(ReadU32Le(trainer, kPartyCountOff)), 0, 6);
    std::array<uint8_t, kPartySize> incoming{};
    PcToParty(pc, incoming.data());

    if (swap_party_index >= 0) {
        if (swap_party_index >= count) {
            if (error) *error = "party swap index out of range";
            return false;
        }
        uint8_t *party = trainer + kPartyStart + static_cast<size_t>(swap_party_index) * kPartySize;
        std::array<uint8_t, kPcSize> outgoing{};
        PartyToPc(party, outgoing.data());
        std::copy(incoming.begin(), incoming.end(), party);
        std::copy(outgoing.begin(), outgoing.end(), pc);
    } else {
        if (count >= 6) {
            if (error) *error = "party is full; choose a party Pokemon to swap";
            return false;
        }
        uint8_t *party = trainer + kPartyStart + static_cast<size_t>(count) * kPartySize;
        std::copy(incoming.begin(), incoming.end(), party);
        std::fill(pc, pc + kPcSize, 0);
        WriteU32Le(trainer, kPartyCountOff, static_cast<uint32_t>(count + 1));
    }
    return FinalizeTrainer(buffer, section, error);
}

bool InsertPartyMon(std::vector<uint8_t> &buffer, const uint16_t species_id, const int level,
    const std::string &nickname, const uint32_t otid, const std::string &ot_name,
    const std::unordered_map<int, std::string> &species_db, std::string *error) {
    std::vector<uint8_t> temporary(static_cast<size_t>(PcStreamBoxCount()) * kPcBoxSlotCount * kPcSize, 0);
    if (!InsertPcMon(temporary, 1, 1, species_id, level, nickname, otid, ot_name, species_db, error)) return false;
    return WithdrawPcMon(buffer, temporary, 1, 1, -1, error);
}

} // namespace puse::core

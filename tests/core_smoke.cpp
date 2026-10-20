#include <puse/core/Bag.hpp>
#include <puse/core/GameProgress.hpp>
#include <puse/core/Money.hpp>
#include <puse/core/Party.hpp>
#include <puse/core/Pc.hpp>
#include <puse/core/PokedexFlags.hpp>
#include <puse/core/Rtc.hpp>
#include <puse/core/Roster.hpp>
#include <puse/core/SaveHealth.hpp>
#include <puse/core/SaveSession.hpp>
#include <puse/io/DataLoader.hpp>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void Require(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
    std::cout << "PASS: " << message << '\n';
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "Usage: core_smoke <Unbound.sav>\n";
        return 2;
    }

    puse::core::SaveSession session;
    std::string error;
    Require(session.LoadFromFile(argv[1], &error, "unbound-2.1.1.1"), "load and recognize Unbound save: " + error);
    Require(session.Buffer().size() == 0x20000 || session.Buffer().size() == 0x20010, "supported save size");
    Require(puse::core::EnsurePartyStaticDataLoaded(&error), "load party catalogs: " + error);
    Require(puse::core::EnsureBagDataLoaded(&error), "load bag catalogs: " + error);

    const auto species = puse::io::LoadIdNameFile(
        puse::io::ResolveAssetPath(puse::core::profile::CatalogAsset("pokemon.txt")));
    Require(!species.empty(), "load species names");
    const auto aliases = puse::io::LoadSpeciesFormAliases(puse::io::ResolveAssetPath("data/species_form_aliases.json"));
    const auto wiki_learnsets = puse::io::LoadSpeciesLearnsets(puse::io::ResolveAssetPath("data/wiki_species_learnsets.json"));
    const auto move_details = puse::io::LoadMoveDetails(puse::io::ResolveAssetPath("data/move_details.txt"));
    const auto wiki_abilities = puse::io::LoadWikiAbilities(puse::io::ResolveAssetPath("data/wiki_species_abilities.txt"));
    Require(!aliases.empty(), "load regional/form aliases");
    Require(!wiki_learnsets.empty(), "load Unbound Wiki compatibility data");
    Require(!move_details.empty(), "load Unbound Wiki move details");
    Require(!wiki_abilities.empty(), "load Unbound Wiki species abilities");

    const auto party = puse::core::ParseParty(session.Buffer(), species);
    Require(!party.empty() && party.size() <= 6, "parse party");

    const auto stream = puse::core::BuildPcStream(session.Buffer(), &error);
    Require(stream.size() == static_cast<size_t>(puse::core::PcStreamBoxCount()) *
        puse::core::kPcBoxSlotCount * puse::core::kPcMonSize, "map complete PC stream: " + error);

    uint32_t money = 0;
    uint16_t bp = 0;
    Require(puse::core::ReadMoney(session.Buffer(), &money, &error), "read money: " + error);
    Require(puse::core::ReadBp(session.Buffer(), &bp, &error), "read Battle Points: " + error);

    puse::core::GameProgressSnapshot progress{};
    Require(puse::core::BuildGameProgressSnapshot(session.Buffer(), "normal", &progress, &error),
        "build game-progress snapshot: " + error);

    auto changed = session.Buffer();
    Require(puse::core::WriteMoney(changed, money, &error), "round-trip money mutation: " + error);
    Require(puse::core::UpdatePartyHappiness(changed, party.front().index, party.front().happiness, &error),
        "round-trip party mutation: " + error);
    Require(puse::core::CommitPartySectionChecksums(changed, &error), "commit party checksums: " + error);

    auto changed_stream = stream;
    bool pc_mutation_tested = false;
    for (int box = 1; box <= puse::core::PcStreamBoxCount() && !pc_mutation_tested; ++box) {
        const auto mons = puse::core::ParsePcBox(changed_stream, box, species);
        if (mons.empty()) continue;
        Require(puse::core::UpdatePcMonHappiness(changed_stream, box, mons.front().slot,
            mons.front().happiness, &error), "round-trip PC mutation: " + error);
        pc_mutation_tested = true;
    }
    Require(pc_mutation_tested, "locate an occupied PC slot");

    bool roster_swap_tested = false;
    for (int box = 1; box <= puse::core::PcStreamBoxCount() && !roster_swap_tested; ++box) {
        const auto mons = puse::core::ParsePcBox(changed_stream, box, species);
        if (mons.empty()) continue;
        auto roster_save = changed;
        auto roster_pc = changed_stream;
        const auto before_party = puse::core::ParseParty(roster_save, species);
        const uint16_t party_species = before_party.front().species_id;
        const uint16_t pc_species = mons.front().species_id;
        Require(puse::core::DepositPartyMon(roster_save, roster_pc, before_party.front().index,
            box, mons.front().slot, true, &error), "swap Party Pokemon into occupied PC slot: " + error);
        const auto swapped_party = puse::core::ParseParty(roster_save, species);
        const auto swapped_box = puse::core::ParsePcBox(roster_pc, box, species);
        const auto swapped_pc = std::find_if(swapped_box.begin(), swapped_box.end(), [&](const auto &mon) { return mon.slot == mons.front().slot; });
        const auto swapped_party_mon = std::find_if(swapped_party.begin(), swapped_party.end(), [&](const auto &entry) {
            return entry.index == before_party.front().index;
        });
        Require(swapped_party_mon != swapped_party.end() && swapped_party_mon->species_id == pc_species,
            "PC Pokemon expands into a valid Party record (expected " + std::to_string(pc_species) +
            ", got " + (swapped_party_mon == swapped_party.end() ? std::string("missing") : std::to_string(swapped_party_mon->species_id)) + ")");
        Require(swapped_pc != swapped_box.end() && swapped_pc->species_id == party_species,
            "Party Pokemon compacts into a valid PC record");
        Require(puse::core::WithdrawPcMon(roster_save, roster_pc, box, mons.front().slot,
            before_party.front().index, &error), "swap PC Pokemon back into Party: " + error);
        roster_swap_tested = true;
    }
    Require(roster_swap_tested, "exercise Party/PC transfer conversion");

    {
        auto insertion_stream = stream;
        bool inserted = false;
        puse::core::TrainerIdentity trainer{};
        Require(puse::core::ReadTrainerIdentity(changed, &trainer), "read trainer for PC creation");
        for (int box = 1; box <= puse::core::PcStreamBoxCount() && !inserted; ++box) {
            const auto mons = puse::core::ParsePcBox(insertion_stream, box, species);
            for (int slot = 1; slot <= puse::core::kPcBoxSlotCount; ++slot) {
                if (std::any_of(mons.begin(), mons.end(), [slot](const auto &mon) { return mon.slot == slot; })) continue;
                Require(puse::core::InsertPcMon(insertion_stream, box, slot, 1, 5, "", trainer.trainer_id,
                    trainer.name, species, &error), "create PC Pokemon: " + error);
                const auto after = puse::core::ParsePcBox(insertion_stream, box, species);
                Require(std::any_of(after.begin(), after.end(), [slot](const auto &mon) {
                    return mon.slot == slot && mon.species_id == 1 && mon.level == 5;
                }), "PC creation produces a readable record");
                inserted = true;
                break;
            }
        }
        Require(inserted, "locate an empty PC slot for creation");
    }

    Require(puse::core::CalcMaxMovePp(89, 3) >= puse::core::CalcMaxMovePp(89, 0),
        "PP Max calculation does not reduce move PP");

    Require(puse::core::ClampLegalEvTarget({252, 252, 0, 0, 0, 0}, 2, 252) == 6,
        "EV Max uses only the six points remaining under 510");
    Require(puse::core::ClampLegalEvTarget({252, 152, 100, 0, 0, 0}, 2, 252) == 106,
        "EV Max includes the selected stat's existing points");
    Require(puse::core::ClampLegalEvTarget({252, 252, 10, 0, 0, 0}, 2, 11) == 10,
        "over-limit EV data cannot be increased");
    Require(puse::core::ClampLegalEvTarget({252, 252, 10, 0, 0, 0}, 2, 9) == 9,
        "over-limit EV data can be reduced incrementally");

    {
        auto created_save = changed;
        const auto existing_party = puse::core::ParseParty(created_save, species);
        if (existing_party.size() < 6) {
            puse::core::TrainerIdentity trainer{};
            Require(puse::core::ReadTrainerIdentity(created_save, &trainer), "read trainer for Party creation");
            Require(puse::core::InsertPartyMon(created_save, 1, 5, "", trainer.trainer_id,
                trainer.name, species, &error), "create Party Pokemon: " + error);
            Require(puse::core::ParseParty(created_save, species).size() == existing_party.size() + 1,
                "Party creation increments team count");
            Require(puse::core::ReleasePartyMon(created_save, static_cast<int>(existing_party.size()), &error),
                "release a created Party Pokemon: " + error);
            Require(puse::core::ParseParty(created_save, species).size() == existing_party.size(),
                "Party creation/release round trip restores team count");
        }
        if (existing_party.size() > 1) {
            auto released_save = changed;
            Require(puse::core::ReleasePartyMon(released_save, 0, &error), "release Party Pokemon: " + error);
            Require(puse::core::ParseParty(released_save, species).size() == existing_party.size() - 1,
                "Party release compacts the party");
        }
        if (existing_party.size() == 1) {
            auto protected_save = changed;
            Require(!puse::core::ReleasePartyMon(protected_save, 0, &error),
                "protect the last Party Pokemon from release");
        }
    }
    error.clear();
    Require(puse::core::CommitPcStream(changed, changed_stream, &error), "commit PC stream: " + error);

    const auto pockets = puse::core::ResolveQuickPockets(changed);
    bool bag_mutation_tested = false;
    for (const auto &entry : pockets) {
        if (!entry.second.ready) continue;
        const auto slots = puse::core::MapPocketFromAnchor(changed, entry.second.anchor_offset);
        for (const auto &slot : slots) {
            if (slot.item_id == 0) continue;
            Require(puse::core::WriteSlot(changed, slot.offset, slot.item_id, slot.qty,
                slot.encoding_swapped), "round-trip bag slot mutation");
            bag_mutation_tested = true;
            break;
        }
        if (bag_mutation_tested) break;
    }
    Require(bag_mutation_tested, "locate an occupied bag slot");
    error.clear();
    Require(puse::core::CommitBagSectorChecksums(changed, &error), "commit bag checksums: " + error);

    const auto dex = puse::core::GetPokedexFlags(changed, party.front().species_id);
    if (dex.trackable) {
        Require(puse::core::SetPokedexFlag(changed, party.front().species_id, "seen", dex.seen),
            "round-trip Pokedex mutation");
    }

    const auto health = puse::core::BuildSaveHealthReport(session.Buffer(), changed);
    Require(health.section_count >= 28, "save-health report covers both generations");

    puse::core::RtcTimeFixerResult rtc{};
    if (puse::core::ReenableTimeFixer(session.Buffer(), &rtc, &error)) {
        Require(rtc.bytes.size() == session.Buffer().size(), "RTC recovery preserves save length");
    } else {
        std::cout << "INFO: RTC Time Fixer reset unavailable for this save: " << error << '\n';
    }

    std::cout << "SUMMARY: party=" << party.size()
              << " boxes=" << puse::core::PcStreamBoxCount()
              << " money=" << money
              << " bp=" << bp << '\n';
    return 0;
}

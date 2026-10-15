#include <puse/core/Bag.hpp>
#include <puse/core/GameProgress.hpp>
#include <puse/core/Money.hpp>
#include <puse/core/Party.hpp>
#include <puse/core/Pc.hpp>
#include <puse/core/PokedexFlags.hpp>
#include <puse/core/Rtc.hpp>
#include <puse/core/SaveHealth.hpp>
#include <puse/core/SaveSession.hpp>
#include <puse/io/DataLoader.hpp>

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

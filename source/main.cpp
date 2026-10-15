#include <pspctrl.h>
#include <pspdebug.h>
#include <pspkernel.h>
#include <psppower.h>

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

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <optional>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

PSP_MODULE_INFO("PUSE-PSP", PSP_MODULE_USER, 0, 1);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

namespace {

constexpr uint32_t kSaveBodySize = 0x20000;
constexpr uint32_t kSaveRtcSize = 0x20010;
constexpr int kVisibleRows = 23;

bool g_running = true;

int ExitCallback(int, int, void *) {
    g_running = false;
    sceKernelExitGame();
    return 0;
}

int CallbackThread(SceSize, void *) {
    const int callback = sceKernelCreateCallback("exit_callback", ExitCallback, nullptr);
    sceKernelRegisterExitCallback(callback);
    sceKernelSleepThreadCB();
    return 0;
}

void SetupCallbacks() {
    const int thread = sceKernelCreateThread("callback_thread", CallbackThread, 0x11, 0xFA0, 0, nullptr);
    if (thread >= 0) sceKernelStartThread(thread, 0, nullptr);
}

std::string Truncate(const std::string &value, size_t width) {
    if (value.size() <= width) return value;
    if (width < 4) return value.substr(0, width);
    return value.substr(0, width - 3) + "...";
}

void DrawHeader(const std::string &title, const std::string &subtitle = "") {
    pspDebugScreenClear();
    pspDebugScreenSetTextColor(0x00FFFFFF);
    pspDebugScreenPrintf("PUSE-PSP 0.1\n");
    pspDebugScreenSetTextColor(0x0000FFFF);
    pspDebugScreenPrintf("%s\n", Truncate(title, 59).c_str());
    pspDebugScreenSetTextColor(0x00808080);
    pspDebugScreenPrintf("------------------------------------------------------------\n");
    if (!subtitle.empty()) {
        pspDebugScreenSetTextColor(0x00C0C0C0);
        pspDebugScreenPrintf("%s\n", Truncate(subtitle, 59).c_str());
    }
    pspDebugScreenSetTextColor(0x00FFFFFF);
}

uint32_t WaitButton() {
    SceCtrlData pad{};
    do {
        sceCtrlPeekBufferPositive(&pad, 1);
        sceKernelDelayThread(10000);
    } while (g_running && pad.Buttons != 0);
    do {
        sceCtrlPeekBufferPositive(&pad, 1);
        sceKernelDelayThread(10000);
    } while (g_running && pad.Buttons == 0);
    return pad.Buttons;
}

void Message(const std::string &title, const std::vector<std::string> &lines) {
    DrawHeader(title);
    for (const auto &line : lines) pspDebugScreenPrintf("%s\n", Truncate(line, 59).c_str());
    pspDebugScreenPrintf("\n[X/O] Continue");
    WaitButton();
}

bool Confirm(const std::string &title, const std::string &question) {
    DrawHeader(title);
    pspDebugScreenPrintf("%s\n\n", Truncate(question, 59).c_str());
    pspDebugScreenPrintf("[X] Yes    [O] No\n");
    while (g_running) {
        const uint32_t key = WaitButton();
        if (key & PSP_CTRL_CROSS) return true;
        if (key & PSP_CTRL_CIRCLE) return false;
    }
    return false;
}

int Menu(const std::string &title, const std::vector<std::string> &items,
         const std::string &subtitle = "", int initial = 0) {
    if (items.empty()) {
        Message(title, {"No entries available."});
        return -1;
    }
    int selected = std::max(0, std::min(initial, static_cast<int>(items.size()) - 1));
    while (g_running) {
        DrawHeader(title, subtitle);
        int first = selected - kVisibleRows / 2;
        first = std::max(0, std::min(first, std::max(0, static_cast<int>(items.size()) - kVisibleRows)));
        const int last = std::min(static_cast<int>(items.size()), first + kVisibleRows);
        for (int i = first; i < last; ++i) {
            if (i == selected) {
                pspDebugScreenSetTextColor(0x0000FF00);
                pspDebugScreenPrintf("> %-57s\n", Truncate(items[i], 57).c_str());
            } else {
                pspDebugScreenSetTextColor(0x00FFFFFF);
                pspDebugScreenPrintf("  %-57s\n", Truncate(items[i], 57).c_str());
            }
        }
        pspDebugScreenSetTextColor(0x00808080);
        pspDebugScreenPrintf("\n[X] Select  [O] Back  [Up/Down] Move\n");

        const uint32_t key = WaitButton();
        if (key & PSP_CTRL_UP) selected = (selected + static_cast<int>(items.size()) - 1) % items.size();
        if (key & PSP_CTRL_DOWN) selected = (selected + 1) % items.size();
        if (key & PSP_CTRL_LTRIGGER) selected = std::max(0, selected - kVisibleRows);
        if (key & PSP_CTRL_RTRIGGER) selected = std::min(static_cast<int>(items.size()) - 1, selected + kVisibleRows);
        if (key & PSP_CTRL_CROSS) return selected;
        if (key & PSP_CTRL_CIRCLE) return -1;
    }
    return -1;
}

bool EditNumber(const std::string &title, long current, long minimum, long maximum, long *out) {
    long value = std::max(minimum, std::min(maximum, current));
    long step = 1;
    while (g_running) {
        DrawHeader(title);
        pspDebugScreenPrintf("Value: %ld\nRange: %ld - %ld\nStep:  %ld\n\n", value, minimum, maximum, step);
        pspDebugScreenPrintf("Left/Right: change value\nUp/Down: change by 10 x step\nL/R: change step (1, 10, 100, 1000)\n\n[X] Apply  [O] Cancel\n");
        const uint32_t key = WaitButton();
        if (key & PSP_CTRL_LEFT) value = std::max(minimum, value - step);
        if (key & PSP_CTRL_RIGHT) value = std::min(maximum, value + step);
        if (key & PSP_CTRL_UP) value = std::min(maximum, value + step * 10);
        if (key & PSP_CTRL_DOWN) value = std::max(minimum, value - step * 10);
        if (key & PSP_CTRL_LTRIGGER) step = std::max(1L, step / 10);
        if (key & PSP_CTRL_RTRIGGER) step = std::min(1000L, step * 10);
        if (key & PSP_CTRL_CROSS) {
            *out = value;
            return true;
        }
        if (key & PSP_CTRL_CIRCLE) return false;
    }
    return false;
}

bool EditText(const std::string &title, const std::string &current, size_t max_length, std::string *out) {
    static const std::string chars = " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-.'";
    std::string value = current.substr(0, max_length);
    if (value.empty()) value = " ";
    size_t cursor = 0;
    while (g_running) {
        DrawHeader(title);
        pspDebugScreenPrintf("Text: [%s]\n      ", value.c_str());
        for (size_t i = 0; i < cursor + 1; ++i) pspDebugScreenPrintf(" ");
        pspDebugScreenPrintf("^\n\n");
        pspDebugScreenPrintf("Left/Right: cursor   Up/Down: character\n");
        pspDebugScreenPrintf("Triangle: append    Square: delete\n");
        pspDebugScreenPrintf("[X] Apply  [O] Cancel\n");
        const uint32_t key = WaitButton();
        if (key & PSP_CTRL_LEFT) cursor = cursor == 0 ? value.size() - 1 : cursor - 1;
        if (key & PSP_CTRL_RIGHT) cursor = (cursor + 1) % value.size();
        if (key & (PSP_CTRL_UP | PSP_CTRL_DOWN)) {
            const auto found = chars.find(value[cursor]);
            size_t index = found == std::string::npos ? 0 : found;
            if (key & PSP_CTRL_UP) index = (index + 1) % chars.size();
            else index = (index + chars.size() - 1) % chars.size();
            value[cursor] = chars[index];
        }
        if ((key & PSP_CTRL_TRIANGLE) && value.size() < max_length) {
            value.push_back(' ');
            cursor = value.size() - 1;
        }
        if ((key & PSP_CTRL_SQUARE) && !value.empty()) {
            value.erase(cursor, 1);
            if (value.empty()) value = " ";
            cursor = std::min(cursor, value.size() - 1);
        }
        if (key & PSP_CTRL_CROSS) {
            while (!value.empty() && value.back() == ' ') value.pop_back();
            *out = value;
            return true;
        }
        if (key & PSP_CTRL_CIRCLE) return false;
    }
    return false;
}

bool IsDirectory(const std::string &path) {
    struct stat info{};
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool IsSaveFile(const std::string &path) {
    struct stat info{};
    if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) return false;
    if (static_cast<uint32_t>(info.st_size) != kSaveBodySize && static_cast<uint32_t>(info.st_size) != kSaveRtcSize) return false;
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.size() >= 4 && (lower.rfind(".sav") == lower.size() - 4 || lower.rfind(".srm") == lower.size() - 4);
}

void WalkSaves(const std::string &root, int depth, std::set<std::string> *paths) {
    if (depth < 0 || !IsDirectory(root)) return;
    DIR *directory = opendir(root.c_str());
    if (directory == nullptr) return;
    while (dirent *entry = readdir(directory)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        const std::string full = root + (root.back() == '/' ? "" : "/") + name;
        if (IsDirectory(full)) WalkSaves(full, depth - 1, paths);
        else if (IsSaveFile(full)) paths->insert(full);
    }
    closedir(directory);
}

std::vector<std::string> FindSaves() {
    static const char *kRoots[] = {
        "ms0:/PSP/GAME/TempGBA",
        "ms0:/PSP/GAME",
        "ms0:/GBA",
        "ms0:/roms",
        "ef0:/PSP/GAME/TempGBA",
        "ef0:/PSP/GAME",
    };
    std::set<std::string> found;
    for (const char *root : kRoots) WalkSaves(root, 4, &found);
    return {found.begin(), found.end()};
}

bool CopyFile(const std::string &source, const std::string &destination) {
    FILE *in = std::fopen(source.c_str(), "rb");
    if (in == nullptr) return false;
    FILE *out = std::fopen(destination.c_str(), "wb");
    if (out == nullptr) {
        std::fclose(in);
        return false;
    }
    std::array<uint8_t, 16384> buffer{};
    bool ok = true;
    while (true) {
        const size_t count = std::fread(buffer.data(), 1, buffer.size(), in);
        if (count > 0 && std::fwrite(buffer.data(), 1, count, out) != count) {
            ok = false;
            break;
        }
        if (count < buffer.size()) {
            if (std::ferror(in)) ok = false;
            break;
        }
    }
    std::fclose(out);
    std::fclose(in);
    return ok;
}

bool ReadFile(const std::string &path, std::vector<uint8_t> *bytes) {
    FILE *fp = std::fopen(path.c_str(), "rb");
    if (fp == nullptr) return false;
    std::fseek(fp, 0, SEEK_END);
    const long size = std::ftell(fp);
    std::rewind(fp);
    if (size <= 0) {
        std::fclose(fp);
        return false;
    }
    bytes->assign(static_cast<size_t>(size), 0);
    const size_t read = std::fread(bytes->data(), 1, bytes->size(), fp);
    std::fclose(fp);
    return read == bytes->size();
}

std::string NameFor(const std::unordered_map<int, std::string> &db, int id) {
    const auto found = db.find(id);
    return found == db.end() ? std::to_string(id) : found->second;
}

class App {
  public:
    void Run();

  private:
    puse::core::SaveSession session_;
    std::unordered_map<int, std::string> species_;
    std::unordered_map<int, std::string> items_;
    std::unordered_map<int, std::string> moves_;
    std::vector<uint8_t> pc_stream_;
    std::string save_path_;
    bool dirty_ = false;
    bool legit_mode_ = true;

    bool SelectAndLoadSave();
    bool LoadSave(const std::string &path);
    bool LoadAssets();
    bool SaveWithBackup();
    void MainMenu();
    void PartyMenu();
    void EditPartyMon(int slot);
    void EditStats(bool ivs, int slot, puse::core::PartyEntry entry);
    void EditPartyMoves(int slot, puse::core::PartyEntry entry);
    void PcMenu();
    void EditPcMon(int box, int slot);
    void EditPcStats(bool ivs, int box, int slot, puse::core::PcMon mon);
    void EditPcMoves(int box, int slot, puse::core::PcMon mon);
    void BagMenu();
    void MoneyMenu();
    void PokedexMenu();
    void RtcMenu();
    void GameProgressScreen();
    void SaveHealthScreen();
    void AboutScreen();
    bool CommitPc();
    void MarkPartyChanged();
};

bool App::LoadAssets() {
    std::string error;
    if (!puse::core::EnsurePartyStaticDataLoaded(&error)) {
        Message("Static data error", {error, "Keep the data folder beside EBOOT.PBP."});
        return false;
    }
    const std::string species_path = puse::io::ResolveAssetPath(puse::core::profile::CatalogAsset("pokemon.txt"));
    const std::string items_path = puse::io::ResolveAssetPath(puse::core::profile::CatalogAsset("items.txt"));
    const std::string moves_path = puse::io::ResolveAssetPath(puse::core::profile::CatalogAsset("moves.txt"));
    species_ = puse::io::LoadIdNameFile(species_path);
    items_ = puse::io::LoadIdNameFile(items_path);
    moves_ = puse::io::LoadIdNameFile(moves_path);
    if (species_.empty() || items_.empty() || moves_.empty()) {
        Message("Static data error", {"One or more catalogs could not be loaded.", "Keep the data folder beside EBOOT.PBP."});
        return false;
    }
    return true;
}

bool App::LoadSave(const std::string &path) {
    std::string error;
    session_.Clear();
    if (!session_.LoadFromFile(path, &error, "unbound-2.1.1.1")) {
        Message("Cannot load save", {path, error});
        return false;
    }
    save_path_ = path;
    if (!LoadAssets()) return false;
    pc_stream_ = puse::core::BuildPcStream(session_.Buffer(), &error);
    if (pc_stream_.empty()) Message("PC warning", {"PC storage could not be mapped.", error});
    dirty_ = false;
    return true;
}

bool App::SelectAndLoadSave() {
    DrawHeader("Searching Memory Stick");
    pspDebugScreenPrintf("Looking for 128 KiB .sav and .srm files...\n");
    const auto saves = FindSaves();
    if (saves.empty()) {
        Message("No save found", {
            "No compatible .sav or .srm was found.",
            "Place Unbound.sav under:",
            "ms0:/PSP/GAME/TempGBA/",
            "or ms0:/PSP/GAME/PUSE-PSP/"
        });
        return false;
    }
    std::vector<std::string> labels;
    for (const auto &path : saves) labels.push_back(path);
    const int selected = Menu("Choose save", labels, std::to_string(saves.size()) + " compatible files found");
    return selected >= 0 && LoadSave(saves[selected]);
}

bool App::SaveWithBackup() {
    if (!session_.IsLoaded()) return false;
    puse::core::RefreshPartyMonChecksums(session_.MutableBuffer());
    std::string error;
    if (!pc_stream_.empty() && !puse::core::CommitPcStream(session_.MutableBuffer(), pc_stream_, &error)) {
        Message("Save failed", {"PC commit failed", error});
        return false;
    }
    const std::string backup = save_path_ + ".puse.bak";
    if (!CopyFile(save_path_, backup)) {
        Message("Save failed", {"Could not create backup:", backup});
        return false;
    }
    if (!session_.ExportToFile(save_path_, &error)) {
        CopyFile(backup, save_path_);
        Message("Save failed", {error, "The backup was restored."});
        return false;
    }
    const std::string path = save_path_;
    if (!session_.LoadFromFile(path, &error, "unbound-2.1.1.1")) {
        Message("Saved with reload warning", {"Save written and backup created.", "Restart PUSE-PSP before more edits."});
        dirty_ = false;
        return true;
    }
    pc_stream_ = puse::core::BuildPcStream(session_.Buffer(), &error);
    dirty_ = false;
    Message("Save complete", {"Written:", save_path_, "Backup:", backup});
    return true;
}

void App::MarkPartyChanged() {
    puse::core::CommitPartySectionChecksums(session_.MutableBuffer());
    dirty_ = true;
}

void App::PartyMenu() {
    int selected = 0;
    while (g_running) {
        const auto party = puse::core::ParseParty(session_.Buffer(), species_);
        std::vector<std::string> rows(6, "(empty)");
        for (const auto &mon : party) {
            rows[mon.index] = std::to_string(mon.index + 1) + ". " + mon.species_name + " Lv." + std::to_string(mon.level) +
                (mon.is_shiny ? " [shiny]" : "");
        }
        selected = Menu("Party", rows, "Edit complete party data", selected);
        if (selected < 0) return;
        const auto found = std::find_if(party.begin(), party.end(), [selected](const auto &mon) { return mon.index == selected; });
        if (found == party.end()) Message("Empty party slot", {"Party insertion is not exposed by PUSE core."});
        else EditPartyMon(selected);
    }
}

void App::EditStats(bool ivs, int slot, puse::core::PartyEntry entry) {
    static const char *kNames[] = {"HP", "Attack", "Defense", "Speed", "Sp. Attack", "Sp. Defense"};
    auto values = ivs ? entry.ivs : entry.evs;
    while (g_running) {
        std::vector<std::string> rows;
        for (int i = 0; i < 6; ++i) rows.push_back(std::string(kNames[i]) + ": " + std::to_string(values[i]));
        const int choice = Menu(ivs ? "Edit IVs" : "Edit EVs", rows, legit_mode_ ? "Legit limits enabled" : "Raw per-stat limits");
        if (choice < 0) return;
        long value = values[choice];
        const long max_value = ivs ? 31 : 252;
        if (!EditNumber(rows[choice], value, 0, max_value, &value)) continue;
        values[choice] = static_cast<uint8_t>(value);
        if (!ivs && legit_mode_) {
            int total = 0;
            for (uint8_t stat : values) total += stat;
            if (total > 510) {
                Message("EV limit", {"Total EVs cannot exceed 510 in Legit Mode."});
                values[choice] = ivs ? entry.ivs[choice] : entry.evs[choice];
                continue;
            }
        }
        std::string error;
        const bool ok = ivs
            ? puse::core::UpdatePartyIvs(session_.MutableBuffer(), slot, values, &error)
            : puse::core::UpdatePartyEvs(session_.MutableBuffer(), slot, values, &error);
        if (ok) MarkPartyChanged();
        else Message("Edit failed", {error});
    }
}

void App::EditPartyMoves(int slot, puse::core::PartyEntry entry) {
    auto move_ids = entry.move_ids;
    while (g_running) {
        std::vector<std::string> rows;
        for (int i = 0; i < 4; ++i) rows.push_back(std::to_string(i + 1) + ". " + NameFor(moves_, move_ids[i]) + " [" + std::to_string(move_ids[i]) + "]");
        const int choice = Menu("Edit moves", rows, "Enter native Unbound move IDs");
        if (choice < 0) return;
        long value = move_ids[choice];
        if (!EditNumber(rows[choice], value, 0, 2047, &value)) continue;
        move_ids[choice] = static_cast<uint16_t>(value);
        std::string error;
        if (puse::core::UpdatePartyMoves(session_.MutableBuffer(), slot, move_ids, std::nullopt, std::nullopt, &error)) MarkPartyChanged();
        else Message("Edit failed", {error});
    }
}

void App::EditPartyMon(int slot) {
    while (g_running) {
        const auto party = puse::core::ParseParty(session_.Buffer(), species_);
        const auto found = std::find_if(party.begin(), party.end(), [slot](const auto &mon) { return mon.index == slot; });
        if (found == party.end()) return;
        const auto entry = *found;
        std::vector<std::string> rows = {
            "Nickname: " + entry.nickname,
            "OT name: " + entry.ot_name,
            "Species: " + entry.species_name + " [" + std::to_string(entry.species_id) + "]",
            "Level: " + std::to_string(entry.level),
            "Held item: " + NameFor(items_, entry.item_id) + " [" + std::to_string(entry.item_id) + "]",
            "Happiness: " + std::to_string(entry.happiness),
            "Nature: " + entry.nature_name + " [" + std::to_string(entry.nature_id) + "]",
            std::string("Shiny: ") + (entry.is_shiny ? "Yes" : "No"),
            "Gender: " + entry.gender,
            "Ability slot: " + std::to_string(entry.current_ability_index),
            "IVs...",
            "EVs...",
            "Moves...",
        };
        const int choice = Menu(entry.species_name, rows, "Party slot " + std::to_string(slot + 1));
        if (choice < 0) return;
        std::string error;
        bool ok = false;
        long value = 0;
        std::string text;
        switch (choice) {
            case 0:
                if (EditText("Nickname", entry.nickname, 10, &text)) ok = puse::core::UpdatePartyNickname(session_.MutableBuffer(), slot, text, &error);
                break;
            case 1:
                if (EditText("Original Trainer", entry.ot_name, 7, &text)) ok = puse::core::UpdatePartyOtName(session_.MutableBuffer(), slot, text, &error);
                break;
            case 2:
                value = entry.species_id;
                if (EditNumber("Species ID", value, 1, puse::core::profile::MaxSpeciesId(), &value)) ok = puse::core::UpdatePartySpecies(session_.MutableBuffer(), slot, value, &error);
                break;
            case 3: {
                value = entry.level;
                puse::core::PartyLevelResult result{};
                if (EditNumber("Level", value, 1, 100, &value)) ok = puse::core::UpdatePartyLevel(session_.MutableBuffer(), slot, value, std::nullopt, &result, &error);
                break;
            }
            case 4:
                value = entry.item_id;
                if (EditNumber("Held item ID", value, 0, 4095, &value)) ok = puse::core::UpdatePartyItem(session_.MutableBuffer(), slot, value, &error);
                break;
            case 5:
                value = entry.happiness;
                if (EditNumber("Happiness", value, 0, 255, &value)) ok = puse::core::UpdatePartyHappiness(session_.MutableBuffer(), slot, value, &error);
                break;
            case 6:
                value = entry.nature_id;
                if (EditNumber("Nature ID", value, 0, 24, &value)) ok = puse::core::UpdatePartyNature(session_.MutableBuffer(), slot, value, &error);
                break;
            case 7:
                ok = puse::core::UpdatePartyIdentity(session_.MutableBuffer(), slot, std::optional<bool>(!entry.is_shiny), std::nullopt, &error);
                break;
            case 8:
                if (!entry.gender_editable) {
                    Message("Gender", {"This species has a fixed or genderless identity."});
                    continue;
                } else {
                    const int gender = Menu("Gender", {"Male", "Female"}, "PID-compatible identity edit");
                    if (gender >= 0) ok = puse::core::UpdatePartyIdentity(session_.MutableBuffer(), slot, std::nullopt,
                        std::optional<std::string>(gender == 0 ? "male" : "female"), &error);
                }
                break;
            case 9:
                value = entry.current_ability_index;
                if (EditNumber("Ability slot", value, 0, 2, &value)) ok = puse::core::UpdatePartyAbilitySwitch(session_.MutableBuffer(), slot, value, &error);
                break;
            case 10: EditStats(true, slot, entry); continue;
            case 11: EditStats(false, slot, entry); continue;
            case 12: EditPartyMoves(slot, entry); continue;
        }
        if (ok) MarkPartyChanged();
        else if (!error.empty()) Message("Edit failed", {error});
    }
}

bool App::CommitPc() {
    std::string error;
    if (!puse::core::CommitPcStream(session_.MutableBuffer(), pc_stream_, &error)) {
        Message("PC commit failed", {error});
        return false;
    }
    dirty_ = true;
    return true;
}

void App::EditPcStats(bool ivs, int box, int slot, puse::core::PcMon mon) {
    static const char *kNames[] = {"HP", "Attack", "Defense", "Speed", "Sp. Attack", "Sp. Defense"};
    auto values = ivs ? mon.ivs : mon.evs;
    while (g_running) {
        std::vector<std::string> rows;
        for (int i = 0; i < 6; ++i) rows.push_back(std::string(kNames[i]) + ": " + std::to_string(values[i]));
        const int choice = Menu(ivs ? "Edit PC IVs" : "Edit PC EVs", rows,
            legit_mode_ ? "Legit limits enabled" : "Raw per-stat limits");
        if (choice < 0) return;
        long value = values[choice];
        if (!EditNumber(rows[choice], value, 0, ivs ? 31 : 252, &value)) continue;
        const uint8_t old = values[choice];
        values[choice] = static_cast<uint8_t>(value);
        if (!ivs && legit_mode_) {
            int total = 0;
            for (uint8_t stat : values) total += stat;
            if (total > 510) {
                values[choice] = old;
                Message("EV limit", {"Total EVs cannot exceed 510 in Legit Mode."});
                continue;
            }
        }
        std::string error;
        const bool ok = ivs
            ? puse::core::UpdatePcMonIvs(pc_stream_, box, slot, values, &error)
            : puse::core::UpdatePcMonEvs(pc_stream_, box, slot, values, &error);
        if (ok) CommitPc();
        else Message("Edit failed", {error});
    }
}

void App::EditPcMoves(int box, int slot, puse::core::PcMon mon) {
    auto move_ids = mon.move_ids;
    auto pp_ups = mon.move_pp_ups;
    while (g_running) {
        std::vector<std::string> rows;
        for (int i = 0; i < 4; ++i) {
            rows.push_back(std::to_string(i + 1) + ". " + NameFor(moves_, move_ids[i]) +
                " [" + std::to_string(move_ids[i]) + "] PP Ups " + std::to_string(pp_ups[i]));
        }
        const int choice = Menu("Edit PC moves", rows, "Enter native Unbound move IDs");
        if (choice < 0) return;
        const int action = Menu("Move slot " + std::to_string(choice + 1), {"Change move ID", "Change PP Ups"});
        if (action < 0) continue;
        long value = action == 0 ? move_ids[choice] : pp_ups[choice];
        if (!EditNumber(action == 0 ? "Move ID" : "PP Ups", value, 0, action == 0 ? 1023 : 3, &value)) continue;
        if (action == 0) move_ids[choice] = static_cast<uint16_t>(value);
        else pp_ups[choice] = static_cast<uint8_t>(value);
        std::string error;
        if (puse::core::UpdatePcMonMoves(pc_stream_, box, slot, move_ids, &pp_ups, &error)) CommitPc();
        else Message("Edit failed", {error});
    }
}

void App::EditPcMon(int box, int slot) {
    while (g_running) {
        const auto mons = puse::core::ParsePcBox(pc_stream_, box, species_);
        const auto found = std::find_if(mons.begin(), mons.end(), [slot](const auto &mon) { return mon.slot == slot; });
        if (found == mons.end()) return;
        const auto mon = *found;
        std::vector<std::string> rows = {
            "Nickname: " + mon.nickname,
            "OT name: " + mon.ot_name,
            "Species: " + mon.species_name + " [" + std::to_string(mon.species_id) + "]",
            "Level: " + std::to_string(mon.level),
            "Held item: " + NameFor(items_, mon.item_id) + " [" + std::to_string(mon.item_id) + "]",
            "Happiness: " + std::to_string(mon.happiness),
            "Nature ID: " + std::to_string(mon.nature_id),
            std::string("Shiny: ") + (mon.is_shiny ? "Yes" : "No"),
            "Ability slot: " + std::to_string(mon.current_ability_index),
            "IVs...",
            "EVs...",
            "Moves...",
            "Release Pokemon",
        };
        const int choice = Menu(mon.species_name, rows, "Box " + std::to_string(box) + " Slot " + std::to_string(slot));
        if (choice < 0) return;
        std::string error;
        bool ok = false;
        long value = 0;
        std::string text;
        switch (choice) {
            case 0:
                if (EditText("Nickname", mon.nickname, 10, &text)) ok = puse::core::UpdatePcMonNickname(pc_stream_, box, slot, text, &error);
                break;
            case 1:
                if (EditText("Original Trainer", mon.ot_name, 7, &text)) ok = puse::core::UpdatePcMonOtName(pc_stream_, box, slot, text, &error);
                break;
            case 2:
                value = mon.species_id;
                if (EditNumber("Species ID", value, 1, puse::core::profile::MaxSpeciesId(), &value)) ok = puse::core::UpdatePcMonSpecies(pc_stream_, box, slot, value, &error);
                break;
            case 3:
                value = mon.level;
                if (EditNumber("Level", value, 1, 100, &value)) ok = puse::core::UpdatePcMonLevel(pc_stream_, box, slot, value, &error);
                break;
            case 4:
                value = mon.item_id;
                if (EditNumber("Held item ID", value, 0, 4095, &value)) ok = puse::core::UpdatePcMonItem(pc_stream_, box, slot, value, &error);
                break;
            case 5:
                value = mon.happiness;
                if (EditNumber("Happiness", value, 0, 255, &value)) ok = puse::core::UpdatePcMonHappiness(pc_stream_, box, slot, value, &error);
                break;
            case 6:
                value = mon.nature_id;
                if (EditNumber("Nature ID", value, 0, 24, &value)) ok = puse::core::UpdatePcMonNature(pc_stream_, box, slot, value, &error);
                break;
            case 7: ok = puse::core::UpdatePcMonShiny(pc_stream_, box, slot, !mon.is_shiny, &error); break;
            case 8:
                value = mon.current_ability_index;
                if (EditNumber("Ability slot", value, 0, 2, &value)) ok = puse::core::UpdatePcMonAbilitySwitch(pc_stream_, box, slot, value, &error);
                break;
            case 9: EditPcStats(true, box, slot, mon); continue;
            case 10: EditPcStats(false, box, slot, mon); continue;
            case 11: EditPcMoves(box, slot, mon); continue;
            case 12:
                if (Confirm("Release Pokemon", "Permanently clear this PC slot?")) {
                    ok = puse::core::ReleasePcMon(pc_stream_, box, slot, &error);
                    if (ok) CommitPc();
                    return;
                }
                break;
        }
        if (ok) CommitPc();
        else if (!error.empty()) Message("Edit failed", {error});
    }
}

void App::PcMenu() {
    int box = 1;
    int selected = 0;
    while (g_running) {
        const auto mons = puse::core::ParsePcBox(pc_stream_, box, species_);
        std::vector<std::string> rows(puse::core::kPcBoxSlotCount);
        for (int slot = 1; slot <= puse::core::kPcBoxSlotCount; ++slot) rows[slot - 1] = std::to_string(slot) + ". (empty)";
        for (const auto &mon : mons) rows[mon.slot - 1] = std::to_string(mon.slot) + ". " + mon.species_name + " Lv." + std::to_string(mon.level);
        selected = Menu("PC Box " + std::to_string(box), rows,
            "Select slot; use Box Change entry after slot 30", selected);
        if (selected < 0) {
            std::vector<std::string> boxes;
            for (int i = 1; i <= puse::core::PcStreamBoxCount(); ++i) boxes.push_back("Box " + std::to_string(i));
            const int next = Menu("Choose PC box", boxes, "Current: " + std::to_string(box), box - 1);
            if (next < 0) return;
            box = next + 1;
            selected = 0;
            continue;
        }
        const int slot = selected + 1;
        const auto found = std::find_if(mons.begin(), mons.end(), [slot](const auto &mon) { return mon.slot == slot; });
        if (found != mons.end()) {
            EditPcMon(box, slot);
            continue;
        }
        if (!Confirm("Empty PC slot", "Insert a new Pokemon here?")) continue;
        long species = 1;
        long level = 5;
        if (!EditNumber("Species ID", species, 1, puse::core::profile::MaxSpeciesId(), &species)) continue;
        if (!EditNumber("Level", level, 1, 100, &level)) continue;
        puse::core::TrainerIdentity trainer{};
        puse::core::ReadTrainerIdentity(session_.Buffer(), &trainer);
        std::string error;
        if (puse::core::InsertPcMon(pc_stream_, box, slot, species, level, "", trainer.trainer_id, trainer.name, species_, &error)) CommitPc();
        else Message("Insert failed", {error});
    }
}

void App::BagMenu() {
    std::string error;
    if (!puse::core::EnsureBagDataLoaded(&error)) {
        Message("Bag data error", {error});
        return;
    }
    static const char *kTypes[] = {"main", "ball", "key", "tm", "berry"};
    static const char *kLabels[] = {"Items", "Poke Balls", "Key Items", "TMs/HMs", "Berries"};
    while (g_running) {
        const auto pockets = puse::core::ResolveQuickPockets(session_.Buffer());
        std::vector<std::string> labels;
        for (int i = 0; i < 5; ++i) {
            const auto found = pockets.find(kTypes[i]);
            labels.push_back(std::string(kLabels[i]) + (found != pockets.end() && found->second.ready ? "" : " [unavailable]"));
        }
        const int pocket_choice = Menu("Bag", labels, "Edit item IDs and quantities");
        if (pocket_choice < 0) return;
        const auto pocket = pockets.find(kTypes[pocket_choice]);
        if (pocket == pockets.end() || !pocket->second.ready) {
            Message("Pocket unavailable", {pocket == pockets.end() ? "Pocket not detected." : pocket->second.locked_reason});
            continue;
        }
        while (g_running) {
            auto slots = puse::core::MapPocketFromAnchor(session_.Buffer(), pocket->second.anchor_offset);
            std::vector<std::string> rows;
            for (size_t i = 0; i < slots.size(); ++i) {
                const auto &slot = slots[i];
                rows.push_back(std::to_string(i + 1) + ". " + NameFor(items_, slot.item_id) + " x" + std::to_string(slot.qty));
            }
            const int slot_choice = Menu(kLabels[pocket_choice], rows, pocket->second.quality);
            if (slot_choice < 0) break;
            auto slot = slots[slot_choice];
            long item = slot.item_id;
            long qty = slot.qty;
            if (!EditNumber("Item ID", item, 0, 4095, &item)) continue;
            const std::string type = puse::core::PocketTypeForItemId(static_cast<uint16_t>(item));
            if (type == "key" || type == "tm" || type == "hm") qty = item == 0 ? 0 : 1;
            else if (!EditNumber("Quantity", qty, 0, 999, &qty)) continue;
            if (puse::core::WriteSlot(session_.MutableBuffer(), slot.offset, item, qty, slot.encoding_swapped) &&
                puse::core::CommitBagSectorChecksums(session_.MutableBuffer(), &error)) dirty_ = true;
            else Message("Bag edit failed", {error});
        }
    }
}

void App::MoneyMenu() {
    while (g_running) {
        uint32_t money = 0;
        uint16_t bp = 0;
        std::string error;
        puse::core::ReadMoney(session_.Buffer(), &money, &error);
        puse::core::ReadBp(session_.Buffer(), &bp, &error);
        const int choice = Menu("Money and BP", {
            "Money: " + std::to_string(money),
            "Battle Points: " + std::to_string(bp),
        });
        if (choice < 0) return;
        long value = choice == 0 ? static_cast<long>(money) : static_cast<long>(bp);
        const long maximum = choice == 0 ? puse::core::kMaxMoney : puse::core::kMaxBp;
        if (!EditNumber(choice == 0 ? "Money" : "Battle Points", value, 0, maximum, &value)) continue;
        const bool ok = choice == 0
            ? puse::core::WriteMoney(session_.MutableBuffer(), value, &error)
            : puse::core::WriteBp(session_.MutableBuffer(), value, &error);
        if (ok) dirty_ = true;
        else Message("Edit failed", {error});
    }
}

void App::PokedexMenu() {
    long species = 1;
    while (g_running) {
        const auto flags = puse::core::GetPokedexFlags(session_.Buffer(), species);
        const std::string name = NameFor(species_, species);
        const int choice = Menu("Pokedex", {
            "Species: " + name + " [" + std::to_string(species) + "]",
            std::string("Seen: ") + (flags.seen ? "Yes" : "No"),
            std::string("Caught: ") + (flags.caught ? "Yes" : "No"),
        }, flags.trackable ? "National Dex ID " + std::to_string(puse::core::DexIdForSpecies(species)) : "Species is not trackable");
        if (choice < 0) return;
        if (choice == 0) {
            EditNumber("Species ID", species, 1, puse::core::profile::MaxSpeciesId(), &species);
            continue;
        }
        if (!flags.trackable) {
            Message("Pokedex", {"This species has no National Dex mapping."});
            continue;
        }
        const bool ok = puse::core::SetPokedexFlag(session_.MutableBuffer(), species,
            choice == 1 ? "seen" : "caught", choice == 1 ? !flags.seen : !flags.caught);
        if (ok) dirty_ = true;
        else Message("Pokedex edit failed", {"The flag could not be written."});
    }
}

void App::RtcMenu() {
    const int choice = Menu("RTC Recovery", {
        "Re-enable Frozen Heights Time Fixer (recommended)",
        "Legacy Quick Fix profile 1",
        "Legacy Quick Fix profile 2",
        "Legacy Quick Fix profile 3",
        "Pair Repair using an NPC-fixed save",
    }, "Correct the PSP RTC before applying recovery");
    if (choice < 0) return;
    std::string error;
    if (choice == 0) {
        if (!Confirm("RTC Recovery", "Is the PSP date and time now correct?")) return;
        puse::core::RtcTimeFixerResult result{};
        if (!puse::core::ReenableTimeFixer(session_.Buffer(), &result, &error)) {
            Message("RTC recovery failed", {error});
            return;
        }
        session_.MutableBuffer() = result.bytes;
        pc_stream_ = puse::core::BuildPcStream(session_.Buffer(), &error);
        dirty_ = true;
        Message("Time Fixer re-enabled", {
            "One used-bit was cleared in the newest generation.",
            "Save changes, run Unbound, use the Frozen Heights NPC,",
            "save in-game, and restart."
        });
        return;
    }
    if (choice == 4) {
        auto saves = FindSaves();
        saves.erase(std::remove(saves.begin(), saves.end(), save_path_), saves.end());
        if (saves.empty()) {
            Message("Pair Repair", {"No second compatible save was found."});
            return;
        }
        const int fixed_choice = Menu("Choose NPC-fixed save", saves,
            "Loaded save is treated as the broken source");
        if (fixed_choice < 0) return;
        std::vector<uint8_t> fixed;
        if (!ReadFile(saves[fixed_choice], &fixed)) {
            Message("Pair Repair", {"Could not read the selected fixed save."});
            return;
        }
        puse::core::RtcPairResult pair{};
        if (!puse::core::BuildPairCandidates(session_.Buffer(), fixed, &pair, &error)) {
            Message("Pair Repair failed", {error});
            return;
        }
        std::vector<std::string> profiles;
        for (int i = 0; i < 7; ++i) profiles.push_back(puse::core::kPairProfileOrder[i]);
        const int profile_choice = Menu("Pair Repair candidate", profiles,
            "Recommended candidates appear first");
        if (profile_choice < 0) return;
        const auto candidate = pair.candidates.find(profiles[profile_choice]);
        if (candidate == pair.candidates.end()) {
            Message("Pair Repair failed", {"Selected candidate was not generated."});
            return;
        }
        if (!Confirm("Pair Repair", "Apply the selected candidate to the loaded save?")) return;
        session_.MutableBuffer() = candidate->second.bytes;
        pc_stream_ = puse::core::BuildPcStream(session_.Buffer(), &error);
        dirty_ = true;
        Message("Pair Repair applied", {"Review Save Health, save changes, and test in-game."});
        return;
    }

    puse::core::RtcManifest manifest{};
    const std::string manifest_path = puse::io::ResolveAssetPath("data/rtc_manifest_unbound_v1.json");
    if (!puse::core::LoadRtcManifest(manifest_path, &manifest, &error)) {
        Message("RTC manifest error", {error});
        return;
    }
    puse::core::RtcQuickResult result{};
    if (!puse::core::BuildQuickCandidates(session_.Buffer(), manifest, &result, &error)) {
        Message("RTC candidate error", {error});
        return;
    }
    const std::string profile = puse::core::kQuickProfileOrder[choice - 1];
    const auto candidate = result.candidates.find(profile);
    if (candidate == result.candidates.end()) {
        Message("RTC candidate error", {"Requested profile was not generated."});
        return;
    }
    if (!Confirm("Legacy RTC recovery", "Apply " + profile + " to the loaded save?")) return;
    session_.MutableBuffer() = candidate->second;
    pc_stream_ = puse::core::BuildPcStream(session_.Buffer(), &error);
    dirty_ = true;
    Message("RTC candidate applied", {"Save changes, test in-game, and keep the backup."});
}

void App::GameProgressScreen() {
    puse::core::GameProgressSnapshot snapshot{};
    std::string error;
    if (!puse::core::BuildGameProgressSnapshot(session_.Buffer(), "normal", &snapshot, &error)) {
        Message("Game progress", {error});
        return;
    }
    Message("Game progress", {
        "Badges: " + std::to_string(snapshot.badge_count),
        "Champion: " + std::string(snapshot.is_champion ? "Yes" : "No"),
        "Mega unlocked: " + std::string(snapshot.mega_unlocked ? "Yes" : "No"),
        "Effective level cap: " + std::to_string(snapshot.effective_level_cap),
        "Money: " + std::to_string(snapshot.money),
        "Battle Points: " + std::to_string(snapshot.battle_points),
        "TM Case: " + std::string(snapshot.tm_case_owned ? "Yes" : "No"),
        "DexNav: " + std::string(snapshot.dexnav ? "Yes" : "No"),
        "Stat Scanner: " + std::string(snapshot.stat_scanner ? "Yes" : "No"),
    });
}

void App::SaveHealthScreen() {
    const auto report = puse::core::BuildSaveHealthReport(session_.OriginalBuffer(), session_.Buffer());
    int invalid = 0;
    for (const auto &checksum : report.checksums) if (checksum.status == "mismatch") ++invalid;
    std::vector<std::string> lines = {
        "Size: " + std::to_string(report.size) + " bytes",
        "Sections: " + std::to_string(report.section_count),
        "Active generation: " + std::to_string(report.active_save_index),
        "Changed bytes: " + std::to_string(report.changed_bytes),
        "Checksum mismatches: " + std::to_string(invalid),
        "Warnings: " + std::to_string(report.warning_codes.size()),
    };
    for (const auto &warning : report.warning_codes) lines.push_back("- " + warning);
    Message("Save health", lines);
}

void App::AboutScreen() {
    Message("About PUSE-PSP", {
        "Native PSP port of PUSE save-editing logic.",
        "Original PUSE: github.com/Zannael/PUSE",
        "PUSE core is MIT licensed.",
        "Always keep the automatic .puse.bak backup.",
        "Pokemon and Pokemon Unbound belong to their owners.",
    });
}

void App::MainMenu() {
    while (g_running && session_.IsLoaded()) {
        puse::core::TrainerIdentity trainer{};
        puse::core::ReadTrainerIdentity(session_.Buffer(), &trainer);
        std::vector<std::string> rows = {
            "Party",
            "PC Boxes",
            "Bag",
            "Money and Battle Points",
            "Pokedex",
            "RTC Recovery",
            "Game Progress",
            "Save Health",
            std::string("Legit Mode: ") + (legit_mode_ ? "On" : "Off"),
            std::string("Save changes") + (dirty_ ? " *" : ""),
            "Load another save",
            "About",
            "Exit",
        };
        const std::string subtitle = trainer.name + " | " + session_.FileName() + (dirty_ ? " | UNSAVED" : "");
        const int choice = Menu("Main Menu", rows, subtitle);
        if (choice < 0) continue;
        switch (choice) {
            case 0: PartyMenu(); break;
            case 1: PcMenu(); break;
            case 2: BagMenu(); break;
            case 3: MoneyMenu(); break;
            case 4: PokedexMenu(); break;
            case 5: RtcMenu(); break;
            case 6: GameProgressScreen(); break;
            case 7: SaveHealthScreen(); break;
            case 8: legit_mode_ = !legit_mode_; break;
            case 9:
                if (!dirty_) Message("Save", {"There are no unsaved changes."});
                else if (Confirm("Save changes", "Create a backup and overwrite the selected save?")) SaveWithBackup();
                break;
            case 10:
                if (dirty_ && !Confirm("Unsaved changes", "Discard changes and load another save?")) break;
                session_.Clear();
                return;
                break;
            case 11: AboutScreen(); break;
            case 12:
                if (dirty_ && !Confirm("Unsaved changes", "Exit without saving?")) break;
                g_running = false;
                return;
        }
    }
}

void App::Run() {
    while (g_running) {
        if (!SelectAndLoadSave()) {
            if (!Confirm("PUSE-PSP", "Search again?")) break;
            continue;
        }
        MainMenu();
    }
}

} // namespace

int main() {
    SetupCallbacks();
    pspDebugScreenInit();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    scePowerSetClockFrequency(333, 333, 166);

    App app;
    app.Run();
    sceKernelExitGame();
    return 0;
}

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
#include <puse/core/Roster.hpp>
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
#include <functional>
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
    pspDebugScreenPrintf("PUSE-PSP 0.2.0\n");
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
    pspDebugScreenPrintf("\n[O/X] Continue");
    WaitButton();
}

bool Confirm(const std::string &title, const std::string &question) {
    DrawHeader(title);
    pspDebugScreenPrintf("%s\n\n", Truncate(question, 59).c_str());
    pspDebugScreenPrintf("[O] Yes    [X] No\n");
    while (g_running) {
        const uint32_t key = WaitButton();
        if (key & PSP_CTRL_CIRCLE) return true;
        if (key & PSP_CTRL_CROSS) return false;
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
        pspDebugScreenPrintf("\n[O] Select  [X] Back  [Up/Down] Move\n");

        const uint32_t key = WaitButton();
        if (key & PSP_CTRL_UP) selected = (selected + static_cast<int>(items.size()) - 1) % items.size();
        if (key & PSP_CTRL_DOWN) selected = (selected + 1) % items.size();
        if (key & PSP_CTRL_LTRIGGER) selected = std::max(0, selected - kVisibleRows);
        if (key & PSP_CTRL_RTRIGGER) selected = std::min(static_cast<int>(items.size()) - 1, selected + kVisibleRows);
        if (key & PSP_CTRL_CIRCLE) return selected;
        if (key & PSP_CTRL_CROSS) return -1;
    }
    return -1;
}

bool EditNumber(const std::string &title, long current, long minimum, long maximum, long *out) {
    long value = std::max(minimum, std::min(maximum, current));
    long step = 1;
    while (g_running) {
        DrawHeader(title);
        pspDebugScreenPrintf("Value: %ld\nRange: %ld - %ld\nStep:  %ld\n\n", value, minimum, maximum, step);
        pspDebugScreenPrintf("Left/Right: change value\nUp/Down: change by 10 x step\nL/R: change step (1, 10, 100, 1000)\n\n[O] Apply  [X] Cancel\n");
        const uint32_t key = WaitButton();
        if (key & PSP_CTRL_LEFT) value = std::max(minimum, value - step);
        if (key & PSP_CTRL_RIGHT) value = std::min(maximum, value + step);
        if (key & PSP_CTRL_UP) value = std::min(maximum, value + step * 10);
        if (key & PSP_CTRL_DOWN) value = std::max(minimum, value - step * 10);
        if (key & PSP_CTRL_LTRIGGER) step = std::max(1L, step / 10);
        if (key & PSP_CTRL_RTRIGGER) step = std::min(1000L, step * 10);
        if (key & PSP_CTRL_CIRCLE) {
            *out = value;
            return true;
        }
        if (key & PSP_CTRL_CROSS) return false;
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
        pspDebugScreenPrintf("[O] Apply  [X] Cancel\n");
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
        if (key & PSP_CTRL_CIRCLE) {
            while (!value.empty() && value.back() == ' ') value.pop_back();
            *out = value;
            return true;
        }
        if (key & PSP_CTRL_CROSS) return false;
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

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::unordered_map<int, std::vector<puse::io::WikiAbility>> g_wiki_abilities;

std::string WikiAbilityName(const uint16_t species, const int slot, const std::string &fallback) {
    const auto found = g_wiki_abilities.find(species);
    if (found == g_wiki_abilities.end()) return fallback;
    const std::string normalized = Lower(fallback);
    for (const auto &ability : found->second) {
        if (ability.slot == slot && Lower(ability.name) == normalized) return ability.name;
    }
    return fallback;
}

std::string CatalogName(const std::unordered_map<int, std::string> &db,
    const std::unordered_map<int, std::string> *aliases, const int id) {
    std::string name = NameFor(db, id);
    if (aliases != nullptr) {
        const auto alias = aliases->find(id);
        if (alias != aliases->end() && !alias->second.empty()) name += " (" + alias->second + ")";
    }
    return name;
}

int CatalogPicker(const std::string &title,
    const std::unordered_map<int, std::string> &db,
    const int current,
    const std::vector<int> *compatible,
    const std::unordered_map<int, std::string> *aliases,
    const std::unordered_map<int, puse::io::MoveDetail> *move_details,
    const bool include_none,
    const bool allow_all_toggle = true,
    const std::unordered_map<int, int> *display_numbers = nullptr) {
    bool alphabetical = false;
    bool compatible_only = compatible != nullptr;
    int selected_id = current;
    while (g_running) {
        std::vector<int> ids;
        if (compatible_only && compatible != nullptr) {
            ids = *compatible;
        } else {
            ids.reserve(db.size());
            for (const auto &entry : db) if (entry.first > 0) ids.push_back(entry.first);
        }
        if (include_none) ids.push_back(0);
        std::sort(ids.begin(), ids.end(), [&](const int a, const int b) {
            if (display_numbers == nullptr) return a < b;
            const auto av = display_numbers->find(a);
            const auto bv = display_numbers->find(b);
            const int an = av == display_numbers->end() ? a : av->second;
            const int bn = bv == display_numbers->end() ? b : bv->second;
            return an == bn ? a < b : an < bn;
        });
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        if (alphabetical) {
            std::stable_sort(ids.begin(), ids.end(), [&](const int a, const int b) {
                if (a == b) return false;
                if (a == 0 || b == 0) return a == 0;
                return Lower(CatalogName(db, aliases, a)) < Lower(CatalogName(db, aliases, b));
            });
        }
        if (ids.empty()) {
            Message(title, {"No entries are available for this selection."});
            return -1;
        }
        int selected = 0;
        const auto found = std::find(ids.begin(), ids.end(), selected_id);
        if (found != ids.end()) selected = static_cast<int>(found - ids.begin());

        while (g_running) {
            const int id = ids[static_cast<size_t>(selected)];
            std::string subtitle = alphabetical ? "Alphabetical" : (display_numbers == nullptr ? "ID order" : "National Dex order");
            if (compatible != nullptr) subtitle += allow_all_toggle ? (compatible_only ? " | Compatible" : " | All") : " | Filtered list";
            if (move_details != nullptr) {
                const auto detail = move_details->find(id);
                if (detail != move_details->end()) {
                    subtitle += " | " + detail->second.type + " " + detail->second.category +
                        " Pwr " + detail->second.power + " Acc " + detail->second.accuracy + " PP " + std::to_string(detail->second.pp);
                }
            }
            DrawHeader(title, subtitle);
            int first = selected - kVisibleRows / 2;
            first = std::max(0, std::min(first, std::max(0, static_cast<int>(ids.size()) - kVisibleRows)));
            const int last = std::min(static_cast<int>(ids.size()), first + kVisibleRows);
            for (int i = first; i < last; ++i) {
                const int row_id = ids[static_cast<size_t>(i)];
                std::string number = std::to_string(row_id);
                if (display_numbers != nullptr) {
                    const auto dex = display_numbers->find(row_id);
                    if (dex != display_numbers->end()) {
                        number = std::to_string(dex->second);
                        if (dex->second != row_id) number += " (ID " + std::to_string(row_id) + ")";
                    }
                }
                const std::string label = row_id == 0 ? "0 - None" : number + " - " + CatalogName(db, aliases, row_id);
                pspDebugScreenSetTextColor(i == selected ? 0x0000FF00 : 0x00FFFFFF);
                pspDebugScreenPrintf("%c %-57s\n", i == selected ? '>' : ' ', Truncate(label, 57).c_str());
            }
            pspDebugScreenSetTextColor(0x00808080);
            pspDebugScreenPrintf("\n[O] Choose [X] Cancel [Triangle] Sort");
            if (compatible != nullptr && allow_all_toggle) pspDebugScreenPrintf(" [Square] Mode");
            if (move_details != nullptr) pspDebugScreenPrintf(" [Select] Details");
            pspDebugScreenPrintf("\n");
            const uint32_t key = WaitButton();
            if (key & PSP_CTRL_UP) selected = (selected + static_cast<int>(ids.size()) - 1) % ids.size();
            if (key & PSP_CTRL_DOWN) selected = (selected + 1) % ids.size();
            if (key & PSP_CTRL_LTRIGGER) selected = std::max(0, selected - kVisibleRows);
            if (key & PSP_CTRL_RTRIGGER) selected = std::min(static_cast<int>(ids.size()) - 1, selected + kVisibleRows);
            if (key & PSP_CTRL_CIRCLE) return ids[static_cast<size_t>(selected)];
            if (key & PSP_CTRL_CROSS) return -1;
            if (key & PSP_CTRL_TRIANGLE) {
                selected_id = ids[static_cast<size_t>(selected)];
                alphabetical = !alphabetical;
                break;
            }
            if ((key & PSP_CTRL_SQUARE) && compatible != nullptr && allow_all_toggle) {
                selected_id = ids[static_cast<size_t>(selected)];
                compatible_only = !compatible_only;
                break;
            }
            if ((key & PSP_CTRL_SELECT) && move_details != nullptr) {
                const auto detail = move_details->find(ids[static_cast<size_t>(selected)]);
                if (detail == move_details->end()) {
                    Message("Move details", {"No Unbound Wiki details are available."});
                } else {
                    Message(CatalogName(db, nullptr, ids[static_cast<size_t>(selected)]), {
                        "ID: " + std::to_string(ids[static_cast<size_t>(selected)]),
                        "Type: " + detail->second.type,
                        "Category: " + detail->second.category,
                        "Power: " + detail->second.power,
                        "Accuracy: " + detail->second.accuracy,
                        "Base PP: " + std::to_string(detail->second.pp),
                        "Source: Pokemon Unbound Wiki"
                    });
                }
            }
        }
    }
    return -1;
}

const std::array<const char *, 25> kNatureLabels = {{
    "0 - Hardy (neutral)", "1 - Lonely (ATK+ / DEF-)", "2 - Brave (ATK+ / SPE-)",
    "3 - Adamant (ATK+ / SPATK-)", "4 - Naughty (ATK+ / SPDEF-)",
    "5 - Bold (DEF+ / ATK-)", "6 - Docile (neutral)", "7 - Relaxed (DEF+ / SPE-)",
    "8 - Impish (DEF+ / SPATK-)", "9 - Lax (DEF+ / SPDEF-)",
    "10 - Timid (SPE+ / ATK-)", "11 - Hasty (SPE+ / DEF-)", "12 - Serious (neutral)",
    "13 - Jolly (SPE+ / SPATK-)", "14 - Naive (SPE+ / SPDEF-)",
    "15 - Modest (SPATK+ / ATK-)", "16 - Mild (SPATK+ / DEF-)",
    "17 - Quiet (SPATK+ / SPE-)", "18 - Bashful (neutral)", "19 - Rash (SPATK+ / SPDEF-)",
    "20 - Calm (SPDEF+ / ATK-)", "21 - Gentle (SPDEF+ / DEF-)",
    "22 - Sassy (SPDEF+ / SPE-)", "23 - Careful (SPDEF+ / SPATK-)", "24 - Quirky (neutral)"
}};

int NaturePicker(const int current) {
    std::vector<std::string> labels;
    for (const char *label : kNatureLabels) labels.emplace_back(label);
    return Menu("Choose nature", labels, "Number, name, and affected stats", current);
}

int AbilityPicker(const uint16_t species_id, const int current) {
    const auto choices = puse::core::GetSpeciesAbilityChoices(species_id);
    if (choices.empty()) {
        Message("Ability", {"No Unbound ability data is available."});
        return -1;
    }
    std::vector<std::string> rows;
    int initial = 0;
    for (size_t i = 0; i < choices.size(); ++i) {
        const auto &choice = choices[i];
        const std::string name = WikiAbilityName(species_id, choice.slot, choice.name);
        rows.push_back(std::to_string(choice.slot) + " - " + name +
            (choice.hidden ? " (Hidden Ability)" : " (Ability " + std::to_string(choice.slot + 1) + ")"));
        if (choice.slot == current) initial = static_cast<int>(i);
    }
    const int selected = Menu("Choose ability", rows, "Pokemon Unbound abilities only", initial);
    return selected < 0 ? -1 : choices[static_cast<size_t>(selected)].slot;
}

std::string AbilityLabel(const uint16_t species_id, const int current) {
    for (const auto &choice : puse::core::GetSpeciesAbilityChoices(species_id)) {
        if (choice.slot == current) return WikiAbilityName(species_id, choice.slot, choice.name) +
            (choice.hidden ? " (Hidden)" : " (Ability " + std::to_string(choice.slot + 1) + ")");
    }
    return "Invalid slot " + std::to_string(current);
}

std::string NatureLabel(const int nature) {
    if (nature < 0 || nature >= static_cast<int>(kNatureLabels.size())) return "Unknown";
    const std::string full = kNatureLabels[static_cast<size_t>(nature)];
    const size_t dash = full.find(" - ");
    return dash == std::string::npos ? full : full.substr(dash + 3);
}

void CompactMoves(std::array<uint16_t, 4> *moves, std::array<uint8_t, 4> *pp_ups = nullptr,
    std::array<uint8_t, 4> *current_pp = nullptr) {
    std::array<uint16_t, 4> next_moves{};
    std::array<uint8_t, 4> next_ups{};
    std::array<uint8_t, 4> next_pp{};
    size_t target = 0;
    for (size_t i = 0; i < moves->size(); ++i) {
        if ((*moves)[i] == 0) continue;
        next_moves[target] = (*moves)[i];
        if (pp_ups != nullptr) next_ups[target] = (*pp_ups)[i];
        if (current_pp != nullptr) next_pp[target] = (*current_pp)[i];
        ++target;
    }
    *moves = next_moves;
    if (pp_ups != nullptr) *pp_ups = next_ups;
    if (current_pp != nullptr) *current_pp = next_pp;
}

std::vector<int> HoldableItemIds(const std::unordered_map<int, std::string> &items) {
    puse::core::EnsureBagDataLoaded(nullptr);
    std::vector<int> ids;
    for (const auto &entry : items) {
        const std::string type = puse::core::PocketTypeForItemId(static_cast<uint16_t>(entry.first));
        if (type != "key" && type != "tm" && type != "hm") ids.push_back(entry.first);
    }
    return ids;
}

struct CreationPlan {
    int preset = 0;
    uint16_t species = 1;
    int level = 5;
    uint8_t nature = 0;
    int ability_slot = 0;
    bool shiny = false;
    std::array<uint16_t, 4> moves{};
    std::array<uint8_t, 4> pp_ups{};
    std::array<uint8_t, 6> ivs{};
};

class App {
  public:
    void Run();

  private:
    puse::core::SaveSession session_;
    std::unordered_map<int, std::string> species_;
    std::unordered_map<int, std::string> items_;
    std::unordered_map<int, std::string> moves_;
    std::unordered_map<int, std::string> form_aliases_;
    std::unordered_map<int, std::vector<int>> learnsets_;
    std::unordered_map<int, puse::io::MoveDetail> move_details_;
    std::unordered_map<int, int> dex_numbers_;
    std::vector<uint8_t> pc_stream_;
    std::string save_path_;
    bool dirty_ = false;
    bool safeguards_ = true;

    bool SelectAndLoadSave();
    bool LoadSave(const std::string &path);
    bool LoadAssets();
    bool SaveWithBackup();
    void MainMenu();
    void PartyMenu();
    void EditPartyMon(int slot);
    void EditStats(bool ivs, int slot, puse::core::PartyEntry entry);
    void EditPartyMoves(int slot, puse::core::PartyEntry entry);
    void CreatePartyMon();
    void DepositParty(int slot);
    void ReviewPartyLegality(int slot);
    void PcMenu();
    void EditPcMon(int box, int slot);
    void EditPcStats(bool ivs, int box, int slot, puse::core::PcMon mon);
    void EditPcMoves(int box, int slot, puse::core::PcMon mon);
    void CreatePcMon(int box, int slot);
    void WithdrawPc(int box, int slot);
    void ReviewPcLegality(int box, int slot);
    bool BuildCreationPlan(CreationPlan *plan);
    void BagMenu();
    void MoneyMenu();
    void PokedexMenu();
    void RtcMenu();
    void GameProgressScreen();
    void SaveHealthScreen();
    void AboutScreen();
    bool CommitPc();
    void MarkPartyChanged();
    std::string SpeciesName(int id) const;
};

std::string App::SpeciesName(const int id) const {
    return CatalogName(species_, &form_aliases_, id);
}

bool App::BuildCreationPlan(CreationPlan *plan) {
    const int preset = Menu("Creation preset", {
        "Natural - varied IVs, normal PP",
        "Competitive - 31 IVs, PP Max",
        "Custom - create, then open full editor"
    }, "Created Pokemon are not event/trade legality certified");
    if (preset < 0) return false;
    plan->preset = preset;
    const int species = CatalogPicker("Choose species", species_, plan->species, nullptr, &form_aliases_, nullptr, false, true, &dex_numbers_);
    if (species < 0) return false;
    plan->species = static_cast<uint16_t>(species);
    long level = plan->level;
    if (!EditNumber("Level", level, 1, 100, &level)) return false;
    plan->level = static_cast<int>(level);
    const int nature = NaturePicker(plan->nature);
    if (nature < 0) return false;
    plan->nature = static_cast<uint8_t>(nature);
    const int ability = AbilityPicker(plan->species, 0);
    if (ability < 0) return false;
    plan->ability_slot = ability;
    plan->shiny = Confirm("Shiny", "Create this Pokemon as shiny?");

    if (Confirm("Moves", "Choose up to four compatible moves now?")) {
        const auto compatible = learnsets_.find(plan->species);
        const std::vector<int> *allowed = compatible == learnsets_.end() ? nullptr : &compatible->second;
        for (size_t i = 0; i < plan->moves.size(); ++i) {
            const int move = CatalogPicker("Choose move " + std::to_string(i + 1), moves_, 0, allowed, nullptr, &move_details_, true);
            if (move < 0) return false;
            plan->moves[i] = static_cast<uint16_t>(move);
            if (move == 0) break;
        }
        CompactMoves(&plan->moves);
    }

    uint32_t seed = static_cast<uint32_t>(plan->species) * 2654435761U ^ static_cast<uint32_t>(plan->level * 977 + plan->nature);
    for (size_t i = 0; i < plan->ivs.size(); ++i) {
        seed ^= seed << 13U;
        seed ^= seed >> 17U;
        seed ^= seed << 5U;
        plan->ivs[i] = preset == 0 ? static_cast<uint8_t>(seed & 31U) : 31;
    }
    for (size_t i = 0; i < plan->pp_ups.size(); ++i) plan->pp_ups[i] = preset == 1 && plan->moves[i] != 0 ? 3 : 0;

    return Confirm("Create Pokemon",
        SpeciesName(plan->species) + " Lv." + std::to_string(plan->level) + " | " +
        NatureLabel(plan->nature) + " | " + AbilityLabel(plan->species, plan->ability_slot));
}

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
    form_aliases_ = puse::io::LoadSpeciesFormAliases(puse::io::ResolveAssetPath("data/species_form_aliases.json"));
    learnsets_ = puse::io::LoadSpeciesLearnsets(puse::io::ResolveAssetPath("data/wiki_species_learnsets.json"));
    const auto rom_learnsets = puse::io::LoadSpeciesLearnsets(puse::io::ResolveAssetPath("data/species_learnsets.json"));
    for (const auto &entry : rom_learnsets) if (learnsets_.find(entry.first) == learnsets_.end()) learnsets_[entry.first] = entry.second;
    move_details_ = puse::io::LoadMoveDetails(puse::io::ResolveAssetPath("data/move_details.txt"));
    g_wiki_abilities = puse::io::LoadWikiAbilities(puse::io::ResolveAssetPath("data/wiki_species_abilities.txt"));
    const auto dex_text = puse::io::LoadIdNameFile(puse::io::ResolveAssetPath("data/pokedex_species_map.txt"));
    for (const auto &entry : dex_text) dex_numbers_[entry.first] = std::atoi(entry.second.c_str());
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
    const std::string temporary = save_path_ + ".puse.tmp";
    std::remove(temporary.c_str());
    if (!session_.ExportToFile(temporary, &error)) {
        Message("Save failed", {"Could not write the temporary save.", error});
        return false;
    }
    puse::core::SaveSession validation;
    if (!validation.LoadFromFile(temporary, &error, "unbound-2.1.1.1") ||
        validation.Buffer() != session_.Buffer()) {
        std::remove(temporary.c_str());
        Message("Save failed", {"Temporary save validation failed.", error});
        return false;
    }
    if (!CopyFile(save_path_, backup)) {
        std::remove(temporary.c_str());
        Message("Save failed", {"Could not create backup:", backup});
        return false;
    }
    if (std::rename(temporary.c_str(), save_path_.c_str()) != 0) {
        std::remove(save_path_.c_str());
    }
    if (access(temporary.c_str(), F_OK) == 0 && std::rename(temporary.c_str(), save_path_.c_str()) != 0) {
        CopyFile(backup, save_path_);
        std::remove(temporary.c_str());
        Message("Save failed", {"Could not replace the original save.", "The backup was restored."});
        return false;
    }
    std::vector<uint8_t> written;
    if (!ReadFile(save_path_, &written) || written != session_.Buffer()) {
        CopyFile(backup, save_path_);
        Message("Save failed", {"Written bytes did not verify.", "The backup was restored."});
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
        if (found == party.end()) CreatePartyMon();
        else EditPartyMon(selected);
    }
}

void App::EditStats(bool ivs, int slot, puse::core::PartyEntry entry) {
    static const char *kNames[] = {"HP", "Attack", "Defense", "Speed", "Sp. Attack", "Sp. Defense"};
    auto values = ivs ? entry.ivs : entry.evs;
    while (g_running) {
        std::vector<std::string> rows;
        for (int i = 0; i < 6; ++i) rows.push_back(std::string(kNames[i]) + ": " + std::to_string(values[i]));
        const int choice = Menu(ivs ? "Edit IVs" : "Edit EVs", rows, safeguards_ ? "Legality Safeguards enabled" : "Raw per-stat limits");
        if (choice < 0) return;
        long value = values[choice];
        const long max_value = ivs ? 31 : 252;
        if (!EditNumber(rows[choice], value, 0, max_value, &value)) continue;
        values[choice] = static_cast<uint8_t>(value);
        if (!ivs && safeguards_) {
            int total = 0;
            for (uint8_t stat : values) total += stat;
            if (total > 510) {
                Message("EV limit", {"Legality Safeguards limit total EVs to 510."});
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
    auto move_pp = entry.move_pp;
    auto pp_ups = entry.move_pp_ups;
    while (g_running) {
        std::vector<std::string> rows;
        for (int i = 0; i < 4; ++i) {
            const int maximum = puse::core::CalcMaxMovePp(move_ids[i], pp_ups[i]);
            rows.push_back(std::to_string(i + 1) + ". " + std::to_string(move_ids[i]) + " - " + NameFor(moves_, move_ids[i]) +
                " PP " + std::to_string(move_pp[i]) + "/" + std::to_string(maximum) + " Ups " + std::to_string(pp_ups[i]));
        }
        const int choice = Menu("Edit moves", rows, "Unbound Wiki compatibility and details");
        if (choice < 0) return;
        const int action = Menu("Move slot " + std::to_string(choice + 1), {
            "Choose or remove move", "Set current PP", "Set PP Ups", "Apply PP Max and refill"
        });
        if (action < 0) continue;
        if (action == 0) {
            const auto compatible = learnsets_.find(entry.species_id);
            const std::vector<int> *allowed = compatible == learnsets_.end() ? nullptr : &compatible->second;
            const int selected = CatalogPicker("Choose move", moves_, move_ids[choice], allowed, nullptr, &move_details_, true);
            if (selected < 0) continue;
            move_ids[choice] = static_cast<uint16_t>(selected);
            if (selected == 0) CompactMoves(&move_ids, &pp_ups, &move_pp);
        } else if (action == 1) {
            long value = move_pp[choice];
            const int maximum = puse::core::CalcMaxMovePp(move_ids[choice], pp_ups[choice]);
            if (!EditNumber("Current PP", value, 0, maximum, &value)) continue;
            move_pp[choice] = static_cast<uint8_t>(value);
        } else if (action == 2) {
            long value = pp_ups[choice];
            if (!EditNumber("PP Ups", value, 0, 3, &value)) continue;
            pp_ups[choice] = static_cast<uint8_t>(value);
            move_pp[choice] = static_cast<uint8_t>(std::min<int>(move_pp[choice], puse::core::CalcMaxMovePp(move_ids[choice], pp_ups[choice])));
        } else {
            pp_ups[choice] = move_ids[choice] == 0 ? 0 : 3;
            move_pp[choice] = static_cast<uint8_t>(puse::core::CalcMaxMovePp(move_ids[choice], pp_ups[choice]));
        }
        std::string error;
        if (puse::core::UpdatePartyMoves(session_.MutableBuffer(), slot, move_ids, move_pp, pp_ups, &error)) MarkPartyChanged();
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
            "Species: " + std::to_string(entry.species_id) + " - " + SpeciesName(entry.species_id),
            "Level: " + std::to_string(entry.level),
            "Held item: " + std::to_string(entry.item_id) + " - " + NameFor(items_, entry.item_id),
            "Happiness: " + std::to_string(entry.happiness),
            "Nature: " + std::to_string(entry.nature_id) + " - " + NatureLabel(entry.nature_id),
            std::string("Shiny: ") + (entry.is_shiny ? "Yes" : "No"),
            "Gender: " + entry.gender,
            "Ability: " + AbilityLabel(entry.species_id, entry.current_ability_index),
            "IVs...",
            "EVs...",
            "Moves...",
            "Legality review...",
            "Deposit or swap with PC...",
            "Release Pokemon",
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
                value = CatalogPicker("Choose species", species_, entry.species_id, nullptr, &form_aliases_, nullptr, false, true, &dex_numbers_);
                if (value >= 0) {
                    ok = puse::core::UpdatePartySpecies(session_.MutableBuffer(), slot, value, &error);
                    if (ok && Confirm("Normalize species", "Reset ability and remove incompatible moves?")) {
                        const auto abilities = puse::core::GetSpeciesAbilityChoices(static_cast<uint16_t>(value));
                        if (!abilities.empty()) ok = puse::core::UpdatePartyAbilitySwitch(session_.MutableBuffer(), slot, abilities.front().slot, &error);
                        auto normalized_moves = entry.move_ids;
                        auto normalized_ups = entry.move_pp_ups;
                        auto normalized_pp = entry.move_pp;
                        const auto learnset = learnsets_.find(static_cast<int>(value));
                        if (learnset != learnsets_.end()) {
                            for (auto &move : normalized_moves) {
                                if (move != 0 && std::find(learnset->second.begin(), learnset->second.end(), move) == learnset->second.end()) move = 0;
                            }
                            CompactMoves(&normalized_moves, &normalized_ups, &normalized_pp);
                            ok = ok && puse::core::UpdatePartyMoves(session_.MutableBuffer(), slot, normalized_moves, normalized_pp, normalized_ups, &error);
                        }
                        if (entry.nickname == entry.species_name) puse::core::UpdatePartyNickname(session_.MutableBuffer(), slot, NameFor(species_, value), &error);
                    }
                }
                break;
            case 3: {
                value = entry.level;
                puse::core::PartyLevelResult result{};
                if (EditNumber("Level", value, 1, 100, &value)) ok = puse::core::UpdatePartyLevel(session_.MutableBuffer(), slot, value, std::nullopt, &result, &error);
                break;
            }
            case 4:
                {
                    const auto held_items = HoldableItemIds(items_);
                    value = CatalogPicker("Choose held item", items_, entry.item_id, &held_items, nullptr, nullptr, true, false);
                }
                if (value >= 0) ok = puse::core::UpdatePartyItem(session_.MutableBuffer(), slot, value, &error);
                break;
            case 5:
                value = entry.happiness;
                if (EditNumber("Happiness", value, 0, 255, &value)) ok = puse::core::UpdatePartyHappiness(session_.MutableBuffer(), slot, value, &error);
                break;
            case 6:
                value = NaturePicker(entry.nature_id);
                if (value >= 0) ok = puse::core::UpdatePartyNature(session_.MutableBuffer(), slot, value, &error);
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
                value = AbilityPicker(entry.species_id, entry.current_ability_index);
                if (value >= 0) ok = puse::core::UpdatePartyAbilitySwitch(session_.MutableBuffer(), slot, value, &error);
                break;
            case 10: EditStats(true, slot, entry); continue;
            case 11: EditStats(false, slot, entry); continue;
            case 12: EditPartyMoves(slot, entry); continue;
            case 13: ReviewPartyLegality(slot); continue;
            case 14: DepositParty(slot); return;
            case 15:
                if (Confirm("Release " + entry.species_name, "Remove this Pokemon from the party?")) {
                    ok = puse::core::ReleasePartyMon(session_.MutableBuffer(), slot, &error);
                    if (ok) dirty_ = true;
                    else Message("Release failed", {error});
                    return;
                }
                break;
        }
        if (ok) MarkPartyChanged();
        else if (!error.empty()) Message("Edit failed", {error});
    }
}

void App::DepositParty(const int slot) {
    std::vector<std::string> boxes;
    for (int i = 1; i <= puse::core::PcStreamBoxCount(); ++i) boxes.push_back("Box " + std::to_string(i));
    const int box_choice = Menu("Deposit to PC", boxes);
    if (box_choice < 0) return;
    const int box = box_choice + 1;
    const auto mons = puse::core::ParsePcBox(pc_stream_, box, species_);
    std::vector<std::string> rows(puse::core::kPcBoxSlotCount);
    for (int i = 1; i <= puse::core::kPcBoxSlotCount; ++i) rows[static_cast<size_t>(i - 1)] = std::to_string(i) + ". (empty)";
    for (const auto &mon : mons) rows[static_cast<size_t>(mon.slot - 1)] = std::to_string(mon.slot) + ". " + SpeciesName(mon.species_id) + " Lv." + std::to_string(mon.level);
    const int slot_choice = Menu("Choose PC slot", rows, "Occupied slots will be swapped");
    if (slot_choice < 0) return;
    const int pc_slot = slot_choice + 1;
    const bool occupied = std::any_of(mons.begin(), mons.end(), [pc_slot](const auto &mon) { return mon.slot == pc_slot; });
    if (!Confirm(occupied ? "Swap Pokemon" : "Deposit Pokemon",
        occupied ? "Swap the party and PC Pokemon?" : "Move this party Pokemon into the PC?")) return;
    std::string error;
    if (!puse::core::DepositPartyMon(session_.MutableBuffer(), pc_stream_, slot, box, pc_slot, occupied, &error)) {
        Message("Transfer failed", {error});
        return;
    }
    CommitPc();
}

void App::CreatePartyMon() {
    const auto before = puse::core::ParseParty(session_.Buffer(), species_);
    if (before.size() >= 6) {
        Message("Party full", {"Deposit a Pokemon before creating another."});
        return;
    }
    CreationPlan plan{};
    if (!BuildCreationPlan(&plan)) return;
    puse::core::TrainerIdentity trainer{};
    puse::core::ReadTrainerIdentity(session_.Buffer(), &trainer);
    std::string error;
    if (!puse::core::InsertPartyMon(session_.MutableBuffer(), plan.species, plan.level, "",
        trainer.trainer_id, trainer.name, species_, &error)) {
        Message("Creation failed", {error});
        return;
    }
    const int index = static_cast<int>(before.size());
    bool ok = puse::core::UpdatePartyNature(session_.MutableBuffer(), index, plan.nature, &error) &&
        puse::core::UpdatePartyAbilitySwitch(session_.MutableBuffer(), index, plan.ability_slot, &error) &&
        puse::core::UpdatePartyIvs(session_.MutableBuffer(), index, plan.ivs, &error) &&
        puse::core::UpdatePartyMoves(session_.MutableBuffer(), index, plan.moves, std::nullopt, plan.pp_ups, &error) &&
        puse::core::UpdatePartyIdentity(session_.MutableBuffer(), index, plan.shiny, std::nullopt, &error);
    if (!ok) {
        Message("Creation warning", {error, "The base Pokemon was created; review it before saving."});
    }
    MarkPartyChanged();
    if (plan.preset == 2) EditPartyMon(index);
}

void App::ReviewPartyLegality(const int slot) {
    const auto party = puse::core::ParseParty(session_.Buffer(), species_);
    const auto found = std::find_if(party.begin(), party.end(), [slot](const auto &entry) { return entry.index == slot; });
    if (found == party.end()) return;
    const auto entry = *found;
    std::vector<std::string> warnings;
    const auto abilities = puse::core::GetSpeciesAbilityChoices(entry.species_id);
    if (std::none_of(abilities.begin(), abilities.end(), [&](const auto &choice) { return choice.slot == entry.current_ability_index; })) {
        warnings.push_back("Ability slot is unavailable for this species.");
    }
    const auto learnset = learnsets_.find(entry.species_id);
    for (size_t i = 0; i < entry.move_ids.size(); ++i) {
        if (entry.move_ids[i] != 0 && learnset != learnsets_.end() &&
            std::find(learnset->second.begin(), learnset->second.end(), entry.move_ids[i]) == learnset->second.end()) {
            warnings.push_back("Move " + std::to_string(i + 1) + " is not documented as compatible.");
        }
    }
    int ev_total = 0;
    for (const uint8_t ev : entry.evs) ev_total += ev;
    if (ev_total > 510) warnings.push_back("EV total exceeds 510 (" + std::to_string(ev_total) + ").");
    if (warnings.empty()) {
        Message("Legality review", {"No safeguard warnings found.", "Event and trade legality are not certified."});
        return;
    }
    std::vector<std::string> lines = warnings;
    lines.push_back("Event and trade legality are not certified.");
    Message("Legality review", lines);
    if (!Confirm("Apply safe fixes", "Fix ability and remove incompatible moves?")) return;
    std::string error;
    if (!abilities.empty()) puse::core::UpdatePartyAbilitySwitch(session_.MutableBuffer(), slot, abilities.front().slot, &error);
    auto moves = entry.move_ids;
    auto ups = entry.move_pp_ups;
    auto pp = entry.move_pp;
    if (learnset != learnsets_.end()) {
        for (size_t i = 0; i < moves.size(); ++i) {
            if (moves[i] != 0 && std::find(learnset->second.begin(), learnset->second.end(), moves[i]) == learnset->second.end()) moves[i] = 0;
        }
        CompactMoves(&moves, &ups, &pp);
        puse::core::UpdatePartyMoves(session_.MutableBuffer(), slot, moves, pp, ups, &error);
    }
    MarkPartyChanged();
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
            safeguards_ ? "Legality Safeguards enabled" : "Raw per-stat limits");
        if (choice < 0) return;
        long value = values[choice];
        if (!EditNumber(rows[choice], value, 0, ivs ? 31 : 252, &value)) continue;
        const uint8_t old = values[choice];
        values[choice] = static_cast<uint8_t>(value);
        if (!ivs && safeguards_) {
            int total = 0;
            for (uint8_t stat : values) total += stat;
            if (total > 510) {
                values[choice] = old;
                Message("EV limit", {"Legality Safeguards limit total EVs to 510."});
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
            rows.push_back(std::to_string(i + 1) + ". " + std::to_string(move_ids[i]) + " - " + NameFor(moves_, move_ids[i]) +
                " Max PP " + std::to_string(puse::core::CalcMaxMovePp(move_ids[i], pp_ups[i])) + " Ups " + std::to_string(pp_ups[i]));
        }
        const int choice = Menu("Edit PC moves", rows, "Enter native Unbound move IDs");
        if (choice < 0) return;
        const int action = Menu("Move slot " + std::to_string(choice + 1), {"Choose or remove move", "Set PP Ups", "Apply PP Max"});
        if (action < 0) continue;
        if (action == 0) {
            const auto compatible = learnsets_.find(mon.species_id);
            const std::vector<int> *allowed = compatible == learnsets_.end() ? nullptr : &compatible->second;
            const int selected = CatalogPicker("Choose move", moves_, move_ids[choice], allowed, nullptr, &move_details_, true);
            if (selected < 0) continue;
            move_ids[choice] = static_cast<uint16_t>(selected);
            if (selected == 0) CompactMoves(&move_ids, &pp_ups);
        } else if (action == 1) {
            long value = pp_ups[choice];
            if (!EditNumber("PP Ups", value, 0, 3, &value)) continue;
            pp_ups[choice] = static_cast<uint8_t>(value);
        } else {
            pp_ups[choice] = move_ids[choice] == 0 ? 0 : 3;
        }
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
            "Species: " + std::to_string(mon.species_id) + " - " + SpeciesName(mon.species_id),
            "Level: " + std::to_string(mon.level),
            "Held item: " + std::to_string(mon.item_id) + " - " + NameFor(items_, mon.item_id),
            "Happiness: " + std::to_string(mon.happiness),
            "Nature: " + std::to_string(mon.nature_id) + " - " + NatureLabel(mon.nature_id),
            std::string("Shiny: ") + (mon.is_shiny ? "Yes" : "No"),
            "Ability: " + AbilityLabel(mon.species_id, mon.current_ability_index),
            "IVs...",
            "EVs...",
            "Moves...",
            "Legality review...",
            "Withdraw or swap with Party...",
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
                value = CatalogPicker("Choose species", species_, mon.species_id, nullptr, &form_aliases_, nullptr, false, true, &dex_numbers_);
                if (value >= 0) {
                    ok = puse::core::UpdatePcMonSpecies(pc_stream_, box, slot, value, &error);
                    if (ok && Confirm("Normalize species", "Reset ability and remove incompatible moves?")) {
                        const auto abilities = puse::core::GetSpeciesAbilityChoices(static_cast<uint16_t>(value));
                        if (!abilities.empty()) ok = puse::core::UpdatePcMonAbilitySwitch(pc_stream_, box, slot, abilities.front().slot, &error);
                        auto normalized_moves = mon.move_ids;
                        auto normalized_ups = mon.move_pp_ups;
                        const auto learnset = learnsets_.find(static_cast<int>(value));
                        if (learnset != learnsets_.end()) {
                            for (auto &move : normalized_moves) {
                                if (move != 0 && std::find(learnset->second.begin(), learnset->second.end(), move) == learnset->second.end()) move = 0;
                            }
                            CompactMoves(&normalized_moves, &normalized_ups);
                            ok = ok && puse::core::UpdatePcMonMoves(pc_stream_, box, slot, normalized_moves, &normalized_ups, &error);
                        }
                        if (mon.nickname == mon.species_name) puse::core::UpdatePcMonNickname(pc_stream_, box, slot, NameFor(species_, value), &error);
                    }
                }
                break;
            case 3:
                value = mon.level;
                if (EditNumber("Level", value, 1, 100, &value)) ok = puse::core::UpdatePcMonLevel(pc_stream_, box, slot, value, &error);
                break;
            case 4:
                {
                    const auto held_items = HoldableItemIds(items_);
                    value = CatalogPicker("Choose held item", items_, mon.item_id, &held_items, nullptr, nullptr, true, false);
                }
                if (value >= 0) ok = puse::core::UpdatePcMonItem(pc_stream_, box, slot, value, &error);
                break;
            case 5:
                value = mon.happiness;
                if (EditNumber("Happiness", value, 0, 255, &value)) ok = puse::core::UpdatePcMonHappiness(pc_stream_, box, slot, value, &error);
                break;
            case 6:
                value = NaturePicker(mon.nature_id);
                if (value >= 0) ok = puse::core::UpdatePcMonNature(pc_stream_, box, slot, value, &error);
                break;
            case 7: ok = puse::core::UpdatePcMonShiny(pc_stream_, box, slot, !mon.is_shiny, &error); break;
            case 8:
                value = AbilityPicker(mon.species_id, mon.current_ability_index);
                if (value >= 0) ok = puse::core::UpdatePcMonAbilitySwitch(pc_stream_, box, slot, value, &error);
                break;
            case 9: EditPcStats(true, box, slot, mon); continue;
            case 10: EditPcStats(false, box, slot, mon); continue;
            case 11: EditPcMoves(box, slot, mon); continue;
            case 12: ReviewPcLegality(box, slot); continue;
            case 13: WithdrawPc(box, slot); return;
            case 14:
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

void App::WithdrawPc(const int box, const int slot) {
    const auto party = puse::core::ParseParty(session_.Buffer(), species_);
    int swap_index = -1;
    if (party.size() >= 6) {
        std::vector<std::string> rows;
        for (const auto &entry : party) rows.push_back(std::to_string(entry.index + 1) + ". " + SpeciesName(entry.species_id) + " Lv." + std::to_string(entry.level));
        const int selected = Menu("Party is full", rows, "Choose a Pokemon to swap into this PC slot");
        if (selected < 0) return;
        swap_index = party[static_cast<size_t>(selected)].index;
    }
    if (!Confirm(swap_index < 0 ? "Withdraw Pokemon" : "Swap Pokemon",
        swap_index < 0 ? "Move this PC Pokemon into the party?" : "Swap the selected Party and PC Pokemon?")) return;
    std::string error;
    if (!puse::core::WithdrawPcMon(session_.MutableBuffer(), pc_stream_, box, slot, swap_index, &error)) {
        Message("Transfer failed", {error});
        return;
    }
    CommitPc();
}

void App::CreatePcMon(const int box, const int slot) {
    CreationPlan plan{};
    if (!BuildCreationPlan(&plan)) return;
    puse::core::TrainerIdentity trainer{};
    puse::core::ReadTrainerIdentity(session_.Buffer(), &trainer);
    std::string error;
    bool ok = puse::core::InsertPcMon(pc_stream_, box, slot, plan.species, plan.level, "",
        trainer.trainer_id, trainer.name, species_, &error);
    if (ok) ok = puse::core::UpdatePcMonNature(pc_stream_, box, slot, plan.nature, &error);
    if (ok) ok = puse::core::UpdatePcMonAbilitySwitch(pc_stream_, box, slot, plan.ability_slot, &error);
    if (ok) ok = puse::core::UpdatePcMonIvs(pc_stream_, box, slot, plan.ivs, &error);
    if (ok) ok = puse::core::UpdatePcMonMoves(pc_stream_, box, slot, plan.moves, &plan.pp_ups, &error);
    if (ok) ok = puse::core::UpdatePcMonShiny(pc_stream_, box, slot, plan.shiny, &error);
    if (!ok) {
        Message("Creation failed", {error});
        return;
    }
    CommitPc();
    if (plan.preset == 2) EditPcMon(box, slot);
}

void App::ReviewPcLegality(const int box, const int slot) {
    const auto mons = puse::core::ParsePcBox(pc_stream_, box, species_);
    const auto found = std::find_if(mons.begin(), mons.end(), [slot](const auto &mon) { return mon.slot == slot; });
    if (found == mons.end()) return;
    const auto mon = *found;
    std::vector<std::string> warnings;
    const auto abilities = puse::core::GetSpeciesAbilityChoices(mon.species_id);
    if (std::none_of(abilities.begin(), abilities.end(), [&](const auto &choice) { return choice.slot == mon.current_ability_index; })) warnings.push_back("Ability slot is unavailable for this species.");
    const auto learnset = learnsets_.find(mon.species_id);
    for (size_t i = 0; i < mon.move_ids.size(); ++i) {
        if (mon.move_ids[i] != 0 && learnset != learnsets_.end() &&
            std::find(learnset->second.begin(), learnset->second.end(), mon.move_ids[i]) == learnset->second.end()) {
            warnings.push_back("Move " + std::to_string(i + 1) + " is not documented as compatible.");
        }
    }
    int ev_total = 0;
    for (const uint8_t ev : mon.evs) ev_total += ev;
    if (ev_total > 510) warnings.push_back("EV total exceeds 510 (" + std::to_string(ev_total) + ").");
    if (warnings.empty()) {
        Message("Legality review", {"No safeguard warnings found.", "Event and trade legality are not certified."});
        return;
    }
    warnings.push_back("Event and trade legality are not certified.");
    Message("Legality review", warnings);
    if (!Confirm("Apply safe fixes", "Fix ability and remove incompatible moves?")) return;
    std::string error;
    if (!abilities.empty()) puse::core::UpdatePcMonAbilitySwitch(pc_stream_, box, slot, abilities.front().slot, &error);
    auto moves = mon.move_ids;
    auto ups = mon.move_pp_ups;
    if (learnset != learnsets_.end()) {
        for (size_t i = 0; i < moves.size(); ++i) {
            if (moves[i] != 0 && std::find(learnset->second.begin(), learnset->second.end(), moves[i]) == learnset->second.end()) moves[i] = 0;
        }
        CompactMoves(&moves, &ups);
        puse::core::UpdatePcMonMoves(pc_stream_, box, slot, moves, &ups, &error);
    }
    CommitPc();
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
        CreatePcMon(box, slot);
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
        bool alphabetical = false;
        while (g_running) {
            auto slots = puse::core::MapPocketFromAnchor(session_.Buffer(), pocket->second.anchor_offset);
            std::vector<size_t> order(slots.size());
            for (size_t i = 0; i < order.size(); ++i) order[i] = i;
            if (alphabetical) {
                std::stable_sort(order.begin(), order.end(), [&](const size_t a, const size_t b) {
                    if (a == b) return false;
                    if (slots[a].item_id == 0 || slots[b].item_id == 0) return slots[b].item_id == 0 && slots[a].item_id != 0;
                    return Lower(NameFor(items_, slots[a].item_id)) < Lower(NameFor(items_, slots[b].item_id));
                });
            }
            std::vector<std::string> rows;
            rows.push_back(std::string("Sort: ") + (alphabetical ? "Alphabetical" : "Bag slot order"));
            for (const size_t physical : order) {
                const auto &slot = slots[physical];
                rows.push_back(std::to_string(physical + 1) + ". " + std::to_string(slot.item_id) + " - " +
                    NameFor(items_, slot.item_id) + " x" + std::to_string(slot.qty));
            }
            const int choice = Menu(kLabels[pocket_choice], rows, pocket->second.quality + " | display sorting only");
            if (choice < 0) break;
            if (choice == 0) {
                alphabetical = !alphabetical;
                continue;
            }
            const size_t slot_choice = order[static_cast<size_t>(choice - 1)];
            auto slot = slots[slot_choice];
            std::vector<int> allowed;
            for (const auto &entry : items_) {
                std::string type = puse::core::PocketTypeForItemId(static_cast<uint16_t>(entry.first));
                if (type == "hm") type = "tm";
                if (type == "generic") type = "main";
                if (type == kTypes[pocket_choice]) allowed.push_back(entry.first);
            }
            long item = CatalogPicker(slot.item_id == 0 ? "Create item" : "Choose item", items_, slot.item_id,
                &allowed, nullptr, nullptr, true, false);
            if (item < 0) continue;
            long qty = slot.qty;
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
        const std::string name = SpeciesName(species);
        const int choice = Menu("Pokedex", {
            "Species: " + std::to_string(species) + " - " + name,
            std::string("Seen: ") + (flags.seen ? "Yes" : "No"),
            std::string("Caught: ") + (flags.caught ? "Yes" : "No"),
        }, flags.trackable ? "National Dex ID " + std::to_string(puse::core::DexIdForSpecies(species)) : "Species is not trackable");
        if (choice < 0) return;
        if (choice == 0) {
            const int selected = CatalogPicker("Choose species", species_, species, nullptr, &form_aliases_, nullptr, false, true, &dex_numbers_);
            if (selected >= 0) species = selected;
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
            std::string("Legality Safeguards: ") + (safeguards_ ? "On" : "Off"),
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
            case 8:
                safeguards_ = !safeguards_;
                Message("Legality Safeguards", {
                    safeguards_ ? "Enabled." : "Disabled.",
                    "Checks EV totals, species abilities, and documented",
                    "Pokemon Unbound move compatibility.",
                    "This does not certify event or trade legality."
                });
                break;
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

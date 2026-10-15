#include <puse/io/DataLoader.hpp>

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <sstream>
#include <vector>

namespace {

bool ProbeReadable(const std::string &path) {
    FILE *fp = std::fopen(path.c_str(), "rb");
    if (fp == nullptr) return false;
    std::fclose(fp);
    return true;
}

void Trim(std::string *value) {
    const auto first = value->find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        value->clear();
        return;
    }
    const auto last = value->find_last_not_of(" \t\r\n");
    *value = value->substr(first, last - first + 1);
}

std::string ReadWholeFile(const std::string &path) {
    FILE *fp = std::fopen(path.c_str(), "rb");
    if (fp == nullptr) return "";
    std::fseek(fp, 0, SEEK_END);
    const long size = std::ftell(fp);
    std::rewind(fp);
    if (size <= 0) {
        std::fclose(fp);
        return "";
    }
    std::string out(static_cast<size_t>(size), '\0');
    const size_t read = std::fread(&out[0], 1, out.size(), fp);
    std::fclose(fp);
    if (read != out.size()) return "";
    return out;
}

size_t MatchingClose(const std::string &text, const size_t open, const char left, const char right) {
    int depth = 0;
    bool quoted = false;
    for (size_t i = open; i < text.size(); ++i) {
        if (text[i] == '"' && (i == 0 || text[i - 1] != '\\')) quoted = !quoted;
        if (quoted) continue;
        if (text[i] == left) ++depth;
        if (text[i] == right && --depth == 0) return i;
    }
    return std::string::npos;
}

std::vector<int> ParseIntArray(const std::string &text, const size_t open, const size_t close) {
    std::vector<int> out;
    size_t cursor = open + 1;
    while (cursor < close) {
        while (cursor < close && !(text[cursor] >= '0' && text[cursor] <= '9')) ++cursor;
        if (cursor >= close) break;
        char *end = nullptr;
        const long value = std::strtol(text.c_str() + cursor, &end, 10);
        if (end == text.c_str() + cursor) break;
        out.push_back(static_cast<int>(value));
        cursor = static_cast<size_t>(end - text.c_str());
    }
    return out;
}

std::vector<std::string> Split(const std::string &line, const char delimiter) {
    std::vector<std::string> out;
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, delimiter)) out.push_back(field);
    return out;
}

} // namespace

namespace puse::io {

std::string ResolveAssetPath(const std::string &relative_path) {
    static const char *kRoots[] = {
        "./",
        "ms0:/PSP/GAME/PUSE-PSP/",
        "ef0:/PSP/GAME/PUSE-PSP/",
    };

    for (const char *root : kRoots) {
        const std::string candidate = std::string(root) + relative_path;
        if (ProbeReadable(candidate)) return candidate;
    }
    return "";
}

std::unordered_map<int, std::string> LoadIdNameFile(const std::string &path) {
    std::unordered_map<int, std::string> out;
    FILE *fp = std::fopen(path.c_str(), "rb");
    if (fp == nullptr) return out;

    char line_buf[4096];
    while (std::fgets(line_buf, sizeof(line_buf), fp) != nullptr) {
        std::string raw(line_buf);
        const auto colon = raw.find(':');
        if (colon == std::string::npos) continue;

        std::string left = raw.substr(0, colon);
        std::string right = raw.substr(colon + 1);
        Trim(&left);
        Trim(&right);
        if (left.empty()) continue;

        char *end = nullptr;
        const long parsed = std::strtol(left.c_str(), &end, 10);
        if (end != nullptr && *end == '\0') out[static_cast<int>(parsed)] = right;
    }
    std::fclose(fp);
    return out;
}

std::unordered_map<int, std::vector<int>> LoadSpeciesLearnsets(const std::string &path) {
    std::unordered_map<int, std::vector<int>> out;
    const std::string text = ReadWholeFile(path);
    const size_t root_key = text.find("\"learnsets\"");
    const size_t root_open = root_key == std::string::npos ? std::string::npos : text.find('{', root_key);
    const size_t root_close = root_open == std::string::npos ? std::string::npos : MatchingClose(text, root_open, '{', '}');
    if (root_close == std::string::npos) return out;

    size_t cursor = root_open + 1;
    while (cursor < root_close) {
        const size_t q1 = text.find('"', cursor);
        if (q1 == std::string::npos || q1 >= root_close) break;
        const size_t q2 = text.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        const std::string key = text.substr(q1 + 1, q2 - q1 - 1);
        char *end = nullptr;
        const long species = std::strtol(key.c_str(), &end, 10);
        const size_t object_open = text.find('{', q2);
        if (object_open == std::string::npos || object_open >= root_close) break;
        const size_t object_close = MatchingClose(text, object_open, '{', '}');
        if (object_close == std::string::npos) break;
        if (end != nullptr && *end == '\0') {
            const size_t all_key = text.find("\"all\"", object_open);
            if (all_key != std::string::npos && all_key < object_close) {
                const size_t array_open = text.find('[', all_key);
                const size_t array_close = array_open == std::string::npos ? std::string::npos : MatchingClose(text, array_open, '[', ']');
                if (array_close != std::string::npos && array_close < object_close) {
                    out[static_cast<int>(species)] = ParseIntArray(text, array_open, array_close);
                }
            }
        }
        cursor = object_close + 1;
    }
    return out;
}

std::unordered_map<int, std::string> LoadSpeciesFormAliases(const std::string &path) {
    std::unordered_map<int, std::string> out;
    const std::string text = ReadWholeFile(path);
    size_t cursor = 0;
    while (cursor < text.size()) {
        const size_t q1 = text.find('"', cursor);
        if (q1 == std::string::npos) break;
        const size_t q2 = text.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        const std::string key = text.substr(q1 + 1, q2 - q1 - 1);
        char *end = nullptr;
        const long species = std::strtol(key.c_str(), &end, 10);
        const size_t object_open = text.find('{', q2);
        if (end == nullptr || *end != '\0' || object_open == std::string::npos) {
            cursor = q2 + 1;
            continue;
        }
        const size_t object_close = MatchingClose(text, object_open, '{', '}');
        if (object_close == std::string::npos) break;
        const size_t alias_key = text.find("\"alias\"", object_open);
        if (alias_key != std::string::npos && alias_key < object_close) {
            const size_t value_q1 = text.find('"', text.find(':', alias_key));
            const size_t value_q2 = value_q1 == std::string::npos ? std::string::npos : text.find('"', value_q1 + 1);
            if (value_q2 != std::string::npos && value_q2 < object_close) {
                out[static_cast<int>(species)] = text.substr(value_q1 + 1, value_q2 - value_q1 - 1);
            }
        }
        cursor = object_close + 1;
    }
    return out;
}

std::unordered_map<int, MoveDetail> LoadMoveDetails(const std::string &path) {
    std::unordered_map<int, MoveDetail> out;
    FILE *fp = std::fopen(path.c_str(), "rb");
    if (fp == nullptr) return out;
    char buffer[4096];
    while (std::fgets(buffer, sizeof(buffer), fp) != nullptr) {
        std::string line(buffer);
        Trim(&line);
        if (line.empty() || line[0] == '#') continue;
        const auto fields = Split(line, '|');
        if (fields.size() < 7) continue;
        char *end = nullptr;
        const long id = std::strtol(fields[0].c_str(), &end, 10);
        if (end == nullptr || *end != '\0') continue;
        MoveDetail detail{};
        detail.type = fields[2];
        detail.category = fields[3];
        detail.power = fields[4];
        detail.accuracy = fields[5];
        detail.pp = std::atoi(fields[6].c_str());
        out[static_cast<int>(id)] = detail;
    }
    std::fclose(fp);
    return out;
}

std::unordered_map<int, std::vector<WikiAbility>> LoadWikiAbilities(const std::string &path) {
    std::unordered_map<int, std::vector<WikiAbility>> out;
    FILE *fp = std::fopen(path.c_str(), "rb");
    if (fp == nullptr) return out;
    char buffer[4096];
    while (std::fgets(buffer, sizeof(buffer), fp) != nullptr) {
        std::string line(buffer);
        Trim(&line);
        if (line.empty() || line[0] == '#') continue;
        const auto fields = Split(line, '|');
        if (fields.size() < 3) continue;
        const int species = std::atoi(fields[0].c_str());
        const int slot = std::atoi(fields[1].c_str());
        if (species > 0 && slot >= 0 && slot <= 2) out[species].push_back({slot, fields[2]});
    }
    std::fclose(fp);
    return out;
}

} // namespace puse::io

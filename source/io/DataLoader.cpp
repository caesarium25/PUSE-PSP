#include <puse/io/DataLoader.hpp>

#include <cstdio>
#include <cstdlib>

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

} // namespace puse::io

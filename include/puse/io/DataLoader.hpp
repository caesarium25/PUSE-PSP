#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace puse::io {

struct MoveDetail {
    std::string type;
    std::string category;
    std::string power;
    std::string accuracy;
    int pp = 0;
};

struct WikiAbility {
    int slot = 0;
    std::string name;
};

std::string ResolveAssetPath(const std::string &relative_path);
std::unordered_map<int, std::string> LoadIdNameFile(const std::string &path);
std::unordered_map<int, std::vector<int>> LoadSpeciesLearnsets(const std::string &path);
std::unordered_map<int, std::string> LoadSpeciesFormAliases(const std::string &path);
std::unordered_map<int, MoveDetail> LoadMoveDetails(const std::string &path);
std::unordered_map<int, std::vector<WikiAbility>> LoadWikiAbilities(const std::string &path);

} // namespace puse::io

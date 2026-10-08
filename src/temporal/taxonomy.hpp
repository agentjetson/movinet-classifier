#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace temporal {

// Contract taxonomy (agentjetson/contract/domain/taxonomy.yaml):
//   levels.
//     <l1>.specialists: [...]
//     <l1>.l2.<l2_id>: { description: ... }
struct Taxonomy {
  // l2 id → parent l1
  std::unordered_map<std::string, std::string> l2_to_l1;
  // l2 id → specialists (inherited from l1)
  std::unordered_map<std::string, std::vector<std::string>> l2_specialists;
  // all known l2 ids
  std::vector<std::string> level2_ids;

  bool empty() const { return l2_to_l1.empty(); }

  std::string level1_for(const std::string& l2) const {
    auto it = l2_to_l1.find(l2);
    return it != l2_to_l1.end() ? it->second : "unknown";
  }

  std::vector<std::string> specialists_for(const std::string& l2) const {
    auto it = l2_specialists.find(l2);
    if (it != l2_specialists.end()) return it->second;
    return {};
  }
};

// Loads contract format; returns empty Taxonomy on failure.
Taxonomy load_taxonomy(const std::string& path);

}  // namespace temporal

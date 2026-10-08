#include "temporal/taxonomy.hpp"

#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

namespace temporal {

Taxonomy load_taxonomy(const std::string& path) {
  Taxonomy t;
  try {
    YAML::Node root = YAML::LoadFile(path);
    if (!root["levels"]) {
      spdlog::error("taxonomy {}: missing top-level 'levels' (need contract format)", path);
      return t;
    }
    for (const auto& kv : root["levels"]) {
      const std::string l1 = kv.first.as<std::string>();
      const YAML::Node& node = kv.second;

      std::vector<std::string> specialists;
      if (node["specialists"]) {
        for (const auto& s : node["specialists"])
          specialists.push_back(s.as<std::string>());
      }

      if (!node["l2"]) continue;
      for (const auto& l2kv : node["l2"]) {
        const std::string l2 = l2kv.first.as<std::string>();
        t.l2_to_l1[l2] = l1;
        t.l2_specialists[l2] = specialists;
        t.level2_ids.push_back(l2);
      }
    }
    spdlog::info("Loaded taxonomy from {} ({} level-2 labels)", path,
                 t.level2_ids.size());
  } catch (const std::exception& e) {
    spdlog::error("Failed to load taxonomy {}: {}", path, e.what());
  }
  return t;
}

}  // namespace temporal

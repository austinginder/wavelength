#pragma once
// `wavelength upgrade`: replaces this binary with the latest GitHub release for this platform, checked
// against the release's SHA256SUMS.txt and run once before it takes this one's place. `version --check`
// and `upgrade --check` only report whether a newer release exists.
#include <nlohmann/json.hpp>

#include <string>

namespace wl {

struct UpgradeOptions {
    bool check = false;   // report only
    bool force = false;   // replace a development build, or reinstall the same version
};

// {"ok", "current", "latest", "upgradeAvailable", "url", "upgraded"?, "path"?, "notes": [...]}
bool selfUpgrade(const UpgradeOptions &opt, nlohmann::json &result, std::string &err);

// "0.4.1" newer than "0.4.0", "0.4.0" newer than "0.4.0-dev": -1, 0 or 1
int compareVersions(const std::string &a, const std::string &b);

} // namespace wl

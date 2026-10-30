#pragma once

#include "net/lobby_metadata.h"

#include <string_view>

namespace ghost {

// Steam hosts may publish a preset without ContractId. The Epic join routine
// needs ContractId; these single-map presets match LocalGhost's H3 menu data.
// Never replace a contract already chosen by the host (including Random Map).
inline void FillContractFromPreset(metadata::Fields& fields) {
    const auto contract = fields.find("ContractId");
    if (contract != fields.end() && !contract->second.empty()) return;
    const auto preset = fields.find("PresetId");
    if (preset == fields.end()) return;

    struct Map { std::string_view preset, contract; };
    static constexpr Map maps[] = {
        {"d72d7cc9-ee26-4c7d-857a-75abdc9ccb61", "c854d9c7-8991-42db-a2e4-3cf93e645e81"}, // Miami
        {"f5d7c89d-1428-42b2-b3cb-acaa036e246d", "d5ad70f4-7dbb-4b2b-b183-061778a51468"}, // Santa Fortuna
        {"d5ad70f4-7dbb-4b2b-b183-061778a51468", "d5ad70f4-7dbb-4b2b-b183-061778a51468"}, // legacy alias
        {"8fcf8f5d-0fe9-47d7-bff3-d747d0cd1e89", "8fcf8f5d-0fe9-47d7-bff3-d747d0cd1e89"}, // Hawke's Bay
        {"5e895adf-9879-44d9-b43f-13bb24e897bc", "5e895adf-9879-44d9-b43f-13bb24e897bc"}, // Haven Island
        {"59d1caab-2056-4f4b-a921-9958ef0a88eb", "59d1caab-2056-4f4b-a921-9958ef0a88eb"}, // Paris
    };
    for (const auto& map : maps) {
        if (preset->second == map.preset) {
            fields["ContractId"] = map.contract;
            return;
        }
    }
}

} // namespace ghost

#include "game_maps.h"

#include <iostream>
#include <stdexcept>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    try {
        using ghost::FillContractFromPreset;
        using ghost::metadata::Fields;

        const Fields expected{
            {"d72d7cc9-ee26-4c7d-857a-75abdc9ccb61", "c854d9c7-8991-42db-a2e4-3cf93e645e81"},
            {"f5d7c89d-1428-42b2-b3cb-acaa036e246d", "d5ad70f4-7dbb-4b2b-b183-061778a51468"},
            {"d5ad70f4-7dbb-4b2b-b183-061778a51468", "d5ad70f4-7dbb-4b2b-b183-061778a51468"},
            {"8fcf8f5d-0fe9-47d7-bff3-d747d0cd1e89", "8fcf8f5d-0fe9-47d7-bff3-d747d0cd1e89"},
            {"5e895adf-9879-44d9-b43f-13bb24e897bc", "5e895adf-9879-44d9-b43f-13bb24e897bc"},
            {"59d1caab-2056-4f4b-a921-9958ef0a88eb", "59d1caab-2056-4f4b-a921-9958ef0a88eb"},
        };
        for (const auto& [preset, contract] : expected) {
            Fields fields{{"PresetId", preset}};
            FillContractFromPreset(fields);
            Require(fields.at("ContractId") == contract, "preset resolves to the correct contract");
            fields["ContractId"].clear();
            FillContractFromPreset(fields);
            Require(fields.at("ContractId") == contract, "empty contract is filled too");
        }

        Fields chosen{{"PresetId", expected.begin()->first}, {"ContractId", "host-selected-contract"}};
        FillContractFromPreset(chosen);
        Require(chosen.at("ContractId") == "host-selected-contract", "do not overwrite host contract");

        // Random Map is resolved by the server, not guessed from a preset.
        Fields random{{"PresetId", "c853e7e4-a3e3-451d-8f77-2e3e62e9954e"}};
        FillContractFromPreset(random);
        Require(!random.contains("ContractId"), "do not invent a contract for multi-map presets");
        Fields unknown{{"PresetId", "unknown"}};
        FillContractFromPreset(unknown);
        Require(!unknown.contains("ContractId"), "unknown presets remain untouched");
        Fields empty;
        FillContractFromPreset(empty);
        Require(empty.empty(), "missing metadata remains untouched");
        std::cout << "Map fallback tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

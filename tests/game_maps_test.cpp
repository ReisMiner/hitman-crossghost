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
            {"566ec2b6-27d4-4ade-b38e-7a63877724aa", "566ec2b6-27d4-4ade-b38e-7a63877724aa"}, // ICA Facility
            {"59d1caab-2056-4f4b-a921-9958ef0a88eb", "59d1caab-2056-4f4b-a921-9958ef0a88eb"}, // Paris
            {"457f18d3-0075-4a3c-aab0-bded15748138", "457f18d3-0075-4a3c-aab0-bded15748138"}, // Sapienza
            {"bc3b7358-7226-4061-860f-5896e4b2906c", "bc3b7358-7226-4061-860f-5896e4b2906c"}, // Marrakesh
            {"a13c32cc-d337-4b1e-a996-412469c01695", "a13c32cc-d337-4b1e-a996-412469c01695"}, // Bangkok
            {"581504c1-6336-4f9c-b53f-16a6436e199a", "581504c1-6336-4f9c-b53f-16a6436e199a"}, // Colorado
            {"006117fc-4c7b-4053-9a43-410a9fa7f7f1", "006117fc-4c7b-4053-9a43-410a9fa7f7f1"}, // Hokkaido
            {"8fcf8f5d-0fe9-47d7-bff3-d747d0cd1e89", "8fcf8f5d-0fe9-47d7-bff3-d747d0cd1e89"}, // Hawke's Bay
            {"d72d7cc9-ee26-4c7d-857a-75abdc9ccb61", "c854d9c7-8991-42db-a2e4-3cf93e645e81"}, // Miami
            {"f5d7c89d-1428-42b2-b3cb-acaa036e246d", "d5ad70f4-7dbb-4b2b-b183-061778a51468"}, // Santa Fortuna
            {"d5ad70f4-7dbb-4b2b-b183-061778a51468", "d5ad70f4-7dbb-4b2b-b183-061778a51468"}, // legacy alias
            {"611afe7f-e150-4098-9858-db16ec2a125f", "611afe7f-e150-4098-9858-db16ec2a125f"}, // Mumbai
            {"38d4803c-2622-47ba-8e57-a3d3fb10e025", "38d4803c-2622-47ba-8e57-a3d3fb10e025"}, // Whittleton Creek
            {"a608f1e4-4e4e-4ac0-a57d-b14b4bf69e7f", "a608f1e4-4e4e-4ac0-a57d-b14b4bf69e7f"}, // Ambrose Island
            {"c4a70bbe-7f67-43ea-9e9d-b3184a61b8a5", "c4a70bbe-7f67-43ea-9e9d-b3184a61b8a5"}, // Isle of Sgail
            {"5352638d-41e4-4761-845b-5a963c172af2", "5352638d-41e4-4761-845b-5a963c172af2"}, // New York
            {"5e895adf-9879-44d9-b43f-13bb24e897bc", "5e895adf-9879-44d9-b43f-13bb24e897bc"}, // Haven Island
            {"30eaeec0-11cb-4178-8bc8-40da3568fafc", "30eaeec0-11cb-4178-8bc8-40da3568fafc"}, // Dubai
            {"9b7d0445-4e4a-4534-b788-8365ac616976", "9b7d0445-4e4a-4534-b788-8365ac616976"}, // Dartmoor
            {"5f0cc0b6-9dfa-4ad9-9f27-01d68e083516", "5f0cc0b6-9dfa-4ad9-9f27-01d68e083516"}, // Berlin
            {"cddcc656-3a85-4d60-b8b5-1e0dd55fcfb2", "cddcc656-3a85-4d60-b8b5-1e0dd55fcfb2"}, // Chongqing
            {"336c4701-f6c8-4161-aebb-3de84c6eb696", "336c4701-f6c8-4161-aebb-3de84c6eb696"}, // Mendoza
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
        Require(expected.size() == 22, "every Ghost map preset (21 maps + legacy alias) is covered");

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

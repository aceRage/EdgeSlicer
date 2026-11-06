#include "CliPresetSlots.hpp"

#include <algorithm>

namespace Slic3r {

int cli_filament_slot_count(int project_filament_count, int loaded_filament_count)
{
    return std::max(project_filament_count, loaded_filament_count);
}

void resize_cli_preset_slots(std::vector<std::string> &slots, int filament_count)
{
    const size_t wanted = size_t(std::max(filament_count, 0)) + 2;
    if (slots.size() >= 2 && slots.size() < wanted) {
        std::string printer = std::move(slots.back());
        slots.back().clear();
        slots.resize(wanted);
        slots.back() = std::move(printer);
    } else
        slots.resize(wanted);
}

} // namespace Slic3r

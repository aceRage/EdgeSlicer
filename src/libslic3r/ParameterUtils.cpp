#include "ParameterUtils.hpp"

#include <boost/log/trivial.hpp>
#include <cstddef>

namespace Slic3r {

std::vector<LayerPrintSequence> get_other_layers_print_sequence(int sequence_nums, const std::vector<int> &sequence)
{
    std::vector<LayerPrintSequence> res;
    if (sequence_nums <= 0 || sequence.empty())
        return res;

    // The two keys are written together by the GUI, but a config edited by hand or
    // overridden on the command line can carry a count that does not divide the
    // values, or groups too short for a layer range and one extruder. Treat such a
    // pair as no custom sequence rather than reading past the groups.
    size_t item_nums = sequence.size() / size_t(sequence_nums);
    if (sequence.size() % size_t(sequence_nums) != 0 || item_nums < 3) {
        BOOST_LOG_TRIVIAL(warning) << "other_layers_print_sequence_nums " << sequence_nums << " does not fit the "
                                   << sequence.size() << " values of other_layers_print_sequence, ignoring the custom sequence";
        return res;
    }

    res.reserve(sequence_nums);

    for (int i = 0; i < sequence_nums; ++i) {
        std::vector<int> item;
        item.assign(sequence.begin() + i * item_nums, sequence.begin() + ((i + 1) * item_nums));

        std::pair<std::pair<int, int>, std::vector<int>> res_item;
        res_item.first.first  = item[0];
        res_item.first.second = item[1];
        res_item.second.assign(item.begin() + 2, item.end());
        res.emplace_back(std::move(res_item));
    }

    return res;
}

void get_other_layers_print_sequence(const std::vector<LayerPrintSequence> &customize_sequences, int &sequence_nums, std::vector<int> &sequence)
{
    sequence_nums = 0;
    sequence.clear();
    if (customize_sequences.empty()) { return; }

    sequence_nums = (int) customize_sequences.size();
    for (const auto &customize_sequence : customize_sequences) {
        sequence.push_back(customize_sequence.first.first);
        sequence.push_back(customize_sequence.first.second);
        sequence.insert(sequence.end(), customize_sequence.second.begin(), customize_sequence.second.end());
    }
}

}; // namespace Slic3r

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <bitset>
#include <limits>
#include <utility>
#include <vector>

#include "video_core/macro_turing.h"

namespace Tegra {

/// Staging for macros with known register effects and no external reads. The caller models
/// register side effects while executing; raw method writes are replayed in order only on success.
class NativeMacroTransaction {
public:
    using Word = TuringMacroInterpreter::Word;
    using DataRam = TuringMacroInterpreter::DataRam;
    static constexpr size_t NumRegisters = 0xe00;

    template <typename Read>
    Word ReadRegister(Word method, Read&& read) const {
        return written[method] ? registers[method] : read(method);
    }

    void SetRegister(Word method, Word value) {
        registers[method] = value;
        written[method] = true;
    }

    bool Emit(Word method, Word argument) {
        if (writes.size() >= max_writes) {
            return false;
        }
        writes.emplace_back(method, argument);
        return true;
    }

    template <typename Execute, typename Commit>
    TuringMacroInterpreter::Result Run(DataRam* original, Execute&& execute, Commit&& commit,
                                      size_t write_limit = std::numeric_limits<size_t>::max()) {
        max_writes = write_limit;
        written.reset();
        writes.clear();
        if (original) {
            data_ram = *original;
        }
        const auto result = execute();
        if (result.success) {
            if (original) {
                *original = data_ram;
            }
            for (const auto& [method, argument] : writes) {
                commit(method, argument);
            }
        }
        return result;
    }

    size_t WriteCount() const { return writes.size(); }
    DataRam& Ram() { return data_ram; }

private:
    std::array<Word, NumRegisters> registers{};
    std::bitset<NumRegisters> written;
    DataRam data_ram{};
    std::vector<std::pair<Word, Word>> writes;
    size_t max_writes{};
};

} // namespace Tegra

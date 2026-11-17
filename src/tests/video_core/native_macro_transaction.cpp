// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "video_core/native_macro_transaction.h"

namespace {
using Interpreter = Tegra::TuringMacroInterpreter;
using Word = Interpreter::Word;
using Write = std::pair<Word, Word>;

struct Instruction {
    bool end{};
    std::array<Word, 2> op{}, dst{24, 24}, a{24, 24}, b{24, 24}, imm{}, method{}, emit{};
};
std::vector<Word> Code(std::initializer_list<Instruction> instructions) {
    std::vector<Word> code;
    for (const auto& inst : instructions) {
        std::array<Word, 3> words{};
        const auto field = [&](Word offset, Word size, Word value) {
            for (Word bit = 0; bit < size; ++bit) {
                words[(offset + bit) / 32] |= ((value >> bit) & 1U) << ((offset + bit) % 32);
            }
        };
        field(0, 1, inst.end);
        for (Word lane = 0; lane < 2; ++lane) {
            field(10 + lane * 36, 5, inst.op[lane]);
            field(15 + lane * 36, 5, inst.dst[lane]);
            field(20 + lane * 36, 5, inst.a[lane]);
            field(25 + lane * 36, 5, inst.b[lane]);
            field(30 + lane * 36, 16, inst.imm[lane]);
            field(82 + lane * 7, 3, inst.method[lane]);
            field(85 + lane * 7, 4, inst.emit[lane]);
        }
        code.insert(code.end(), {words[2], words[1], words[0]});
    }
    return code;
}
Instruction Fifo(Word method, bool end = false) {
    return {.end = end, .imm = {method, 0}, .method = {5, 0}, .emit = {3, 0}};
}
} // namespace

TEST_CASE("Native macro transaction discards a partial instruction-limited attempt",
          "[video_core][native_macro]") {
    // Emit, update data RAM, then two more emits. The last instruction is the END delay slot.
    const auto code = Code({Fifo(0x585),
        {.op = {31, 0}, .a = {25, 24}, .b = {26, 24}, .imm = {3, 77}},
        Fifo(0x586, true), Fifo(0x585)});
    std::vector<Word> parameters{10, 20, 30};
    std::vector<Write> committed;
    Interpreter::DataRam ram{};
    ram[3] = 9;
    Tegra::NativeMacroTransaction transaction;
    Interpreter interpreter;
    const auto run = [&] {
        return transaction.Run(&ram, [&] {
            interpreter.active_dram = &transaction.Ram();
            return interpreter.Execute(code, 0, parameters, [](Word) { return 0U; },
                [&](Word method, Word value) {
                    transaction.SetRegister(method, value);
                    transaction.Emit(method, value);
                    return true;
                });
        }, [&](Word method, Word value) { committed.emplace_back(method, value); });
    };
    interpreter.step_limit = 2;
    const auto failed = run();
    CHECK_FALSE(failed.success);
    CHECK(failed.error == "instruction limit exceeded");
    CHECK(failed.consumed == 1);
    CHECK(transaction.WriteCount() == 1);
    CHECK(committed.empty());
    CHECK(ram[3] == 9);
    // Retry from the original input: no partial write or data RAM update is replayed twice.
    interpreter.step_limit = 100;
    REQUIRE(run().success);
    CHECK(committed == std::vector<Write>{{0x585, 10}, {0x586, 20}, {0x585, 30}});
    CHECK(ram[3] == 77);
    CHECK(parameters == std::vector<Word>{10, 20, 30});
}

TEST_CASE("Native macro transaction isolates register reads and rejected emits",
          "[video_core][native_macro]") {
    Tegra::NativeMacroTransaction transaction;
    Interpreter::DataRam ram{};
    std::vector<Write> committed;
    const auto original = [](Word) { return 7U; };
    const auto commit = [&](Word method, Word value) { committed.emplace_back(method, value); };
    const auto failed = transaction.Run(&ram, [&] {
        CHECK(transaction.ReadRegister(0x585, original) == 7);
        transaction.SetRegister(0x585, 99);
        transaction.Emit(0x585, 99);
        CHECK(transaction.ReadRegister(0x585, original) == 99);
        return Interpreter::Result{false, 2, 1, "unsupported emitted method"};
    }, commit);
    CHECK_FALSE(failed.success);
    CHECK(committed.empty());
    const auto succeeded = transaction.Run(&ram, [&] {
        CHECK(transaction.ReadRegister(0x585, original) == 7);
        transaction.Emit(0x586, 1);
        return Interpreter::Result{true, 2, 1, {}};
    }, commit);
    CHECK(succeeded.success);
    CHECK(committed == std::vector<Write>{{0x586, 1}});
}

TEST_CASE("Native macro staging refuses output beyond the retry instruction budget",
          "[video_core][native_macro]") {
    Tegra::NativeMacroTransaction transaction;
    std::vector<Write> committed;
    const auto result = transaction.Run(nullptr, [&] {
        REQUIRE(transaction.Emit(0x585, 1));
        CHECK_FALSE(transaction.Emit(0x585, 2));
        CHECK(transaction.WriteCount() == 1);
        return Interpreter::Result{false, 2, 1, "unsupported emitted method"};
    }, [&](Word method, Word value) { committed.emplace_back(method, value); }, 1);
    CHECK_FALSE(result.success);
    CHECK(committed.empty());
}

// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/translation_pass_trace.h"

namespace {
using Event = Shader::TranslationPassEvent;
struct Record {
    std::string name;
    Event event;
};
void RecordPass(void* context, const char* name, Event event, std::chrono::nanoseconds) {
    static_cast<std::vector<Record>*>(context)->push_back({name, event});
}
} // namespace

TEST_CASE("Translation tracing preserves values and nested pass order", "[shader][trace]") {
    int calls{};
    CHECK(Shader::TraceTranslationPass(nullptr, "disabled", [&] { return ++calls; }) == 1);
    std::vector<Record> records;
    const Shader::TranslationPassTrace trace{RecordPass, &records};
    auto result = Shader::TraceTranslationPass(&trace, "outer", [&] {
        return Shader::TraceTranslationPass(&trace, "inner", [] {
            return std::make_unique<int>(42);
        });
    });
    REQUIRE(*result == 42);
    REQUIRE(records.size() == 4);
    CHECK(records[0].name == "outer");
    CHECK(records[0].event == Event::Begin);
    CHECK(records[1].name == "inner");
    CHECK(records[1].event == Event::Begin);
    CHECK(records[2].name == "inner");
    CHECK(records[2].event == Event::End);
    CHECK(records[3].name == "outer");
    CHECK(records[3].event == Event::End);
}

TEST_CASE("Translation tracing cannot mask a pass failure", "[shader][trace]") {
    std::vector<Record> records;
    const Shader::TranslationPassTrace trace{RecordPass, &records};
    CHECK_THROWS_WITH(Shader::TraceTranslationPass(&trace, "failure", [] {
        throw std::runtime_error("original pass failure");
    }), "original pass failure");
    REQUIRE(records.size() == 2);
    CHECK(records.back().event == Event::Failed);

    const Shader::TranslationPassTrace throwing{
        [](void*, const char*, Event, std::chrono::nanoseconds) { throw 7; }, nullptr};
    CHECK(Shader::TraceTranslationPass(&throwing, "success", [] { return 3; }) == 3);
    CHECK_THROWS_WITH(Shader::TraceTranslationPass(&throwing, "failure", [] {
        throw std::runtime_error("original pass failure");
    }), "original pass failure");
}

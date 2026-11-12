// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <exception>
#include <utility>

namespace Shader {

enum class TranslationPassEvent { Begin, End, Failed };

/// A caller-owned diagnostic sink, valid only for the synchronous translation call.
struct TranslationPassTrace {
    using Callback = void (*)(void*, const char*, TranslationPassEvent, std::chrono::nanoseconds);
    Callback callback{};
    void* context{};
};

class TranslationPassScope {
public:
    TranslationPassScope(const TranslationPassTrace* trace_, const char* name_)
        : trace{trace_}, name{name_} {
        if (trace && trace->callback) {
            Emit(TranslationPassEvent::Begin, {});
            exceptions = std::uncaught_exceptions();
            begin = std::chrono::steady_clock::now();
        }
    }

    ~TranslationPassScope() {
        if (trace && trace->callback) {
            const auto elapsed = std::chrono::steady_clock::now() - begin;
            Emit(std::uncaught_exceptions() > exceptions ? TranslationPassEvent::Failed
                                                        : TranslationPassEvent::End,
                 std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed));
        }
    }

    TranslationPassScope(const TranslationPassScope&) = delete;
    TranslationPassScope& operator=(const TranslationPassScope&) = delete;

private:
    void Emit(TranslationPassEvent event, std::chrono::nanoseconds elapsed) noexcept {
        try {
            trace->callback(trace->context, name, event, elapsed);
        } catch (...) {
            // A diagnostic sink must not change translation or mask its exception.
        }
    }

    const TranslationPassTrace* trace;
    const char* name;
    int exceptions{};
    std::chrono::steady_clock::time_point begin{};
};

template <typename Function>
decltype(auto) TraceTranslationPass(const TranslationPassTrace* trace, const char* name,
                                    Function&& function) {
    const TranslationPassScope scope{trace, name};
    return std::forward<Function>(function)();
}

} // namespace Shader

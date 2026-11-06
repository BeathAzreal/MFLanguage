// test_api_cpp.cpp - the public header must work from C++ (game engines,
// plugins). Built only when a C++ compiler is available.
// SPDX-License-Identifier: MIT
#include <cstdio>
#include <cstring>
#include <string>

#include "mfl/mfl.h"

int main()
{
    std::string out;
    MflConfig cfg;
    cfg.output = [](void *user, const char *text, size_t n) {
        static_cast<std::string *>(user)->append(text, n);
    };
    cfg.user = &out;
    cfg.memory_limit = 0;

    MflState *S = mfl_state_new(&cfg);
    if (S == nullptr) {
        return 1;
    }
    mfl_set_number(S, "voltage", 20, "V");
    mfl_set_number(S, "current", 150, "A");
    mfl_set_number(S, "speed", 5, "mm/s");
    const char *model = "let q = 0.8 * voltage * current / speed\nprint(q.to(J/mm))";
    MflStatus st = mfl_eval(S, model, std::strlen(model), "model.mfl");
    double q = 0.0;
    MflStatus st2 = mfl_get_number(S, "q", "kJ/mm", &q);
    bool ok = st == MFL_OK && st2 == MFL_OK && q == 0.48 && out == "480 J/mm\n";
    std::printf("test_api_cpp: %s\n", ok ? "ok" : mfl_error_message(S));
    mfl_state_free(S);
    return ok ? 0 : 1;
}

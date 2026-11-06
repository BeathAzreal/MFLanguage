/*
 * heat_input_host.c - embedding MFL in a host program (e.g. a game).
 *
 * The "player" picks welding parameters; the host passes them to an MFL
 * model as inputs with units, runs it, and reads the results back in the
 * units its interface wants. The model is compiled once and run for each
 * setting. The player never sees MFL code.
 *
 * Build: part of the CMake project (target mfl_embed_example).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "mfl/mfl.h"

/* The model, as an engineer would write it. voltage, current, speed and
 * efficiency are provided by the host. */
static const char *const model =
    "# Net heat input per unit length (arc welding)\n"
    "let power  = efficiency * voltage * current\n"
    "let energy = power / speed\n"
    "let travel_per_minute = speed.to(mm/min)\n";

/* Collects print() output instead of writing to the console. */
typedef struct {
    char   text[256];
    size_t len;
} Log;

static void log_output(void *user, const char *text, size_t length)
{
    Log *log = (Log *)user;
    size_t room = sizeof log->text - 1 - log->len;
    size_t n = length < room ? length : room;
    memcpy(log->text + log->len, text, n);
    log->len += n;
    log->text[log->len] = '\0';
}

int main(void)
{
    static const struct {
        const char *process;
        double      volts, amps, speed_mm_s, efficiency;
    } settings[] = {
        {"MIG/MAG, thin sheet", 18.0, 120.0, 8.0, 0.8},
        {"MIG/MAG, conformance", 20.0, 150.0, 5.0, 0.8},
        {"MIG/MAG, thick plate", 28.0, 280.0, 4.0, 0.8},
    };
    Log log = {"", 0};
    MflConfig config;
    MflState *S;
    MflScript *script;
    size_t i;
    double q;

    printf("MFL %s embedding example\n\n", mfl_version());

    config.output = log_output;
    config.user = &log;
    config.memory_limit = 0;
    S = mfl_state_new(&config);
    if (S == NULL) {
        return 1;
    }

    /* Inputs must exist (with their units) before compiling: the model's
     * dimensional analysis uses them. Values can change later. */
    mfl_set_number(S, "voltage", 0, "V");
    mfl_set_number(S, "current", 0, "A");
    mfl_set_number(S, "speed", 0, "mm/s");
    mfl_set_number(S, "efficiency", 0, NULL);
    script = mfl_script_compile(S, model, strlen(model), "heat_input_model.mfl");
    if (script == NULL) {
        fprintf(stderr, "%s", mfl_error_message(S));
        mfl_state_free(S);
        return 1;
    }

    printf("%-22s %6s %6s %9s %10s %12s\n", "setting", "U [V]", "I [A]", "v [mm/s]", "P [kW]",
           "Q [kJ/mm]");
    for (i = 0; i < sizeof settings / sizeof settings[0]; i++) {
        double power;
        mfl_set_number(S, "voltage", settings[i].volts, "V");
        mfl_set_number(S, "current", settings[i].amps, "A");
        mfl_set_number(S, "speed", settings[i].speed_mm_s, "mm/s");
        mfl_set_number(S, "efficiency", settings[i].efficiency, NULL);
        if (mfl_script_run(S, script) != MFL_OK ||
            mfl_get_number(S, "power", "kW", &power) != MFL_OK ||
            mfl_get_number(S, "energy", "kJ/mm", &q) != MFL_OK) {
            fprintf(stderr, "%s\n", mfl_error_message(S));
            break;
        }
        printf("%-22s %6.1f %6.1f %9.1f %10.3f %12.3f\n", settings[i].process, settings[i].volts,
               settings[i].amps, settings[i].speed_mm_s, power, q);
    }

    /* The same value in other units: the host chooses. */
    if (mfl_get_number(S, "energy", "J/mm", &q) == MFL_OK) {
        printf("\nlast setting: Q = %g J/mm\n", q);
    }
    if (mfl_get_number(S, "travel_per_minute", "mm/min", &q) == MFL_OK) {
        printf("last setting: v = %g mm/min\n", q);
    }

    /* Mistakes are reported, never fatal. */
    printf("\nerror handling:\n");
    {
        /* Call first, then read the message (argument evaluation order in
         * C is unspecified). */
        MflStatus st = mfl_get_number(S, "energy", "m/s", &q);
        printf("  %s: %s\n", mfl_status_name(st), mfl_error_message(S));
        st = mfl_set_number(S, "speed", 5, "furlong/s");
        printf("  %s: %s\n", mfl_status_name(st), mfl_error_message(S));
    }
    {
        const char *bad = "let p = voltage + current\n";
        MflStatus st = mfl_eval(S, bad, strlen(bad), "bad_model.mfl");
        printf("  %s: %s", mfl_status_name(st), mfl_error_message(S));
    }

    /* print() inside a model goes to the host's callback. */
    {
        const char *talk = "print(\"Q =\", (efficiency * voltage * current / speed).to(J/mm))\n";
        if (mfl_eval(S, talk, strlen(talk), "talk.mfl") == MFL_OK) {
            printf("\nmodel printed (captured by the host): %s", log.text);
        }
    }

    mfl_script_free(script);
    mfl_state_free(S);
    return 0;
}

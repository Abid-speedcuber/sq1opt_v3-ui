#include "karnotation.h"
#include "sq1-logic.h"
#include "sq1opt-runner.h"

#include <cstring>
#include <sstream>
#include <string>
#include <vector>

void sq1optSetExtendedOutput(bool val);

std::vector<int> twoGenPreADF(const int pos[24], int two_gen, bool specific_angle_bot, bool first_match_only);
bool cornersAre2GenSolvable(const int pos[24], int two_gen, bool specific_angle_bot);
namespace TwoGenExact {
bool cornersAre2GenSolvableExact(const int pos[24], int two_gen, bool specific_angle_bot);
std::vector<int> twoGenPreADFExact(const int pos[24], int two_gen, bool specific_angle_bot);
}

// Static initializer — enables extended output for the WASM bridge before main()
struct EnableExtended { EnableExtended() { sq1optSetExtendedOutput(true); } };
static EnableExtended s_enableExtended;

namespace {
char *copy_allocated(const std::string &value) {
    auto *result = new char[value.size() + 1];
    std::memcpy(result, value.data(), value.size());
    result[value.size()] = '\0';
    return result;
}

std::string json_bool(bool value) {
    return value ? "true" : "false";
}
}

extern "C" char *sq1_web_unkarnify_alloc(const char *input) {
    try {
        return copy_allocated(replaceShorthands(unkarnifyHelp(input ? input : "")));
    } catch (...) {
        return copy_allocated("");
    }
}

extern "C" char *sq1_web_karnify_alloc(const char *input, const char *position, bool generator) {
    try {
        if (position && position[0] != '\0') {
            return copy_allocated(karnifycs(input ? input : "", position, generator));
        }
        return copy_allocated(karnify(input ? input : ""));
    } catch (...) {
        return copy_allocated("");
    }
}

extern "C" char *sq1_web_rate_algorithm_json_alloc(const char *algorithm, bool initial_top_a) {
    try {
        const RatingWeights w = getRatingWeights();
        const AlgRating rating = rateAlg(algorithm ? algorithm : "", initial_top_a, w.w1, w.w2, w.w3, w.w4);
        std::ostringstream out;
        out << "{\"finalScore\":" << rating.FINAL
            << ",\"phase1\":" << rating.PHASE1
            << ",\"phase2\":" << rating.PHASE2
            << ",\"phase3\":" << rating.PHASE3
            << ",\"phase4\":" << rating.PHASE4
            << ",\"ergoUp\":" << rating.ergo_up
            << ",\"ergoDown\":" << rating.ergo_down
            << ",\"sliceCount\":" << rating.sliceCount
            << ",\"movement\":" << rating.movement
            << ",\"bonus\":" << rating.bonus
            << ",\"valid\":" << json_bool(rating.valid)
            << ",\"sliceStart\":" << (rating.sliceStart.empty() ? 0 : static_cast<int>(rating.sliceStart.front()))
            << "}";
        return copy_allocated(out.str());
    } catch (...) {
        return copy_allocated("{\"finalScore\":0,\"phase1\":0,\"phase2\":0,\"phase3\":0,\"phase4\":0,\"ergoUp\":0,\"ergoDown\":0,\"sliceCount\":0,\"movement\":0,\"bonus\":0,\"valid\":false,\"sliceStart\":0}");
    }
}

extern "C" char *sq1_web_two_gen_status_json_alloc(const int *position, bool specific_angle_bot) {
    if (!position) {
        return copy_allocated("{\"compatibility\":0,\"cornersTwo\":false,\"cornersPseudo\":false}");
    }
    const bool corners_two = TwoGenExact::cornersAre2GenSolvableExact(position, 2, specific_angle_bot);
    const bool corners_pseudo = TwoGenExact::cornersAre2GenSolvableExact(position, 1, specific_angle_bot);
    const int compatibility = !TwoGenExact::twoGenPreADFExact(position, 2, specific_angle_bot).empty() ? 2
        : !TwoGenExact::twoGenPreADFExact(position, 1, specific_angle_bot).empty() ? 1
        : 0;
    std::ostringstream out;
    out << "{\"compatibility\":" << compatibility
        << ",\"cornersTwo\":" << json_bool(corners_two)
        << ",\"cornersPseudo\":" << json_bool(corners_pseudo)
        << "}";
    return copy_allocated(out.str());
}

extern "C" void sq1_web_free_string(char *value) {
    delete[] value;
}

// Runtime configuration for the ergonomics rater — see sq1-logic.h. These
// three symbols must be added to the Emscripten EXPORTED_FUNCTIONS list
// alongside the other sq1_web_* symbols for the WASM build to expose them.
extern "C" void sq1_web_set_rating_weights(double w1, double w2, double w3, double w4) {
    setRatingWeights(w1, w2, w3, w4);
}

extern "C" bool sq1_web_set_move_value(const char *key, int value) {
    return key ? setMoveValueOverride(key, value) : false;
}

extern "C" void sq1_web_reset_rating_config() {
    resetRatingConfig();
}

extern "C" int sq1_web_batch_init(int argc, char** argv) {
    return sq1_batch_init(argc, argv, "/tables");
}

extern "C" int sq1_web_batch_solve(const char* position) {
    return sq1_batch_solve(position);
}

extern "C" int sq1_web_batch_solve_multi(int argc, char** argv) {
    return sq1_batch_solve_multi((const char**)argv, argc);
}

extern "C" void sq1_web_batch_destroy() {
    sq1_batch_destroy();
}

#include <cstring>
#include <exception>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "sq1opt-runner.h"

void sq1optSetExtendedOutput(bool val);
std::vector<int> twoGenPreADF(const int pos[24], int two_gen, bool specific_angle_bot, bool first_match_only);
bool cornersAre2GenSolvable(const int pos[24], int two_gen, bool specific_angle_bot);
namespace TwoGenExact {
bool cornersAre2GenSolvableExact(const int pos[24], int two_gen, bool specific_angle_bot);
std::vector<int> twoGenPreADFExact(const int pos[24], int two_gen, bool specific_angle_bot);
}

namespace {
using LineCallback = void (*)(const char *, void *);
class CaptureBuffer final : public std::streambuf {
public:
    CaptureBuffer(LineCallback callback, void *context) : callback(callback), context(context) {}
    std::string text;
protected:
    int overflow(int character) override {
        if (character != traits_type::eof()) {
            const char value = static_cast<char>(character);
            text.push_back(value);
            if (value == '\n') flush_line();
            else if (value != '\r') line.push_back(value);
        }
        return character;
    }
    std::streamsize xsputn(const char *input, std::streamsize count) override {
        for (std::streamsize i = 0; i < count; ++i) overflow(static_cast<unsigned char>(input[i]));
        return count;
    }
    int sync() override { flush_line(); return 0; }
private:
    void flush_line() {
        if (!line.empty() && callback) callback(line.c_str(), context);
        line.clear();
    }
    LineCallback callback;
    void *context;
    std::string line;
};

char *copy_allocated(const std::string &value) {
    auto *result = new char[value.size() + 1];
    std::memcpy(result, value.data(), value.size());
    result[value.size()] = '\0';
    return result;
}
}

extern "C" char *sq1_run_alloc(int argc, const char *const *input_argv,
                                const char *table_directory, int *exit_code,
                                LineCallback callback, void *callback_context) {
    std::vector<std::string> storage;
    std::vector<char *> argv;
    storage.reserve(static_cast<std::size_t>(argc));
    argv.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) storage.emplace_back(input_argv[i] ? input_argv[i] : "");
    for (auto &argument : storage) argv.push_back(argument.data());

    CaptureBuffer capture(callback, callback_context);
    auto *old_out = std::cout.rdbuf(&capture);
    auto *old_error = std::cerr.rdbuf(&capture);
    int code = -1;
    try {
        sq1optSetExtendedOutput(true);
        sq1optSetTableDirectory(table_directory ? table_directory : "");
        code = sq1optMain(argc, argv.data());
    } catch (const std::exception &error) {
        capture.text += "ERROR: "; capture.text += error.what(); capture.text += '\n';
    } catch (...) {
        capture.text += "ERROR: Unknown solver failure.\n";
    }
    std::cout.flush(); std::cerr.flush();
    std::cout.rdbuf(old_out); std::cerr.rdbuf(old_error);
    if (exit_code) *exit_code = code;
    return copy_allocated(capture.text);
}

extern "C" void sq1_free_string(char *value) { delete[] value; }
extern "C" void sq1_request_stop() { sq1optRequestStop(); }

extern "C" char *sq1_batch_init_alloc(int argc, const char *const *input_argv,
                                       const char *table_directory, int *exit_code,
                                       LineCallback callback, void *callback_context) {
    std::vector<std::string> storage;
    std::vector<char *> argv;
    storage.reserve(static_cast<std::size_t>(argc));
    argv.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) storage.emplace_back(input_argv[i] ? input_argv[i] : "");
    for (auto &argument : storage) argv.push_back(argument.data());

    CaptureBuffer capture(callback, callback_context);
    auto *old_out = std::cout.rdbuf(&capture);
    auto *old_error = std::cerr.rdbuf(&capture);
    int code = -1;
    try {
        code = sq1_batch_init(argc, argv.data(), table_directory);
    } catch (const std::exception &error) {
        capture.text += "ERROR: "; capture.text += error.what(); capture.text += '\n';
    } catch (...) {
        capture.text += "ERROR: Unknown batch init failure.\n";
    }
    std::cout.flush(); std::cerr.flush();
    std::cout.rdbuf(old_out); std::cerr.rdbuf(old_error);
    if (exit_code) *exit_code = code;
    return copy_allocated(capture.text);
}

extern "C" char *sq1_batch_solve_alloc(const char *position, int *exit_code,
                                        LineCallback callback, void *callback_context) {
    CaptureBuffer capture(callback, callback_context);
    auto *old_out = std::cout.rdbuf(&capture);
    auto *old_error = std::cerr.rdbuf(&capture);
    int code = -1;
    try {
        code = sq1_batch_solve(position);
    } catch (const std::exception &error) {
        capture.text += "ERROR: "; capture.text += error.what(); capture.text += '\n';
    } catch (...) {
        capture.text += "ERROR: Unknown batch solve failure.\n";
    }
    std::cout.flush(); std::cerr.flush();
    std::cout.rdbuf(old_out); std::cerr.rdbuf(old_error);
    if (exit_code) *exit_code = code;
    return copy_allocated(capture.text);
}

extern "C" char *sq1_batch_solve_multi_alloc(int num_candidates, const char **candidates,
                                              int *exit_code,
                                              LineCallback callback, void *callback_context) {
    CaptureBuffer capture(callback, callback_context);
    auto *old_out = std::cout.rdbuf(&capture);
    auto *old_error = std::cerr.rdbuf(&capture);
    int code = -1;
    try {
        code = sq1_batch_solve_multi(candidates, num_candidates);
    } catch (const std::exception &error) {
        capture.text += "ERROR: "; capture.text += error.what(); capture.text += '\n';
    } catch (...) {
        capture.text += "ERROR: Unknown batch solve multi failure.\n";
    }
    std::cout.flush(); std::cerr.flush();
    std::cout.rdbuf(old_out); std::cerr.rdbuf(old_error);
    if (exit_code) *exit_code = code;
    return copy_allocated(capture.text);
}

extern "C" void sq1_batch_destroy_alloc() {
    sq1_batch_destroy();
}
extern "C" int sq1_two_gen_compatibility(const int *position, bool specific_angle_bot,
                                          bool *corners_two, bool *corners_pseudo) {
    if (!position) return 0;
    if (corners_two) *corners_two = TwoGenExact::cornersAre2GenSolvableExact(position, 2, specific_angle_bot);
    if (corners_pseudo) *corners_pseudo = TwoGenExact::cornersAre2GenSolvableExact(position, 1, specific_angle_bot);
    if (!TwoGenExact::twoGenPreADFExact(position, 2, specific_angle_bot).empty()) return 2;
    if (!TwoGenExact::twoGenPreADFExact(position, 1, specific_angle_bot).empty()) return 1;
    return 0;
}

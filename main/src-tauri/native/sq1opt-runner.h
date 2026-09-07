#pragma once

#include <string>
#include <vector>

void sq1optSetTableDirectory(const std::string& dir);
void sq1optRequestStop();
int sq1optMain(int argc, char* argv[]);

// Batch solver API — initialize once, solve many positions.
#ifdef __cplusplus
extern "C" {
#endif
int sq1_batch_init(int argc, char* argv[], const char* table_directory);
int sq1_batch_solve(const char* position);
int sq1_batch_solve_multi(const char** candidates, int num_candidates);
void sq1_batch_destroy();
#ifdef __cplusplus
}
#endif

// Direct position injection — bypasses string encoding/decoding.
// pos[24] uses FullPosition encoding: concrete 0-15, partial corners <0, partial edges >15.
// middle: 1=-, -1=+, 0=ignore.
void sq1optSetPosition(const int pos[24], int middle);

// valid preADFs when 2g or p2g is enabled
// specificAngleBot: passed from UI
// firstMatchOnly: stop and return after the first valid preADF
std::vector<int> twoGenPreADF(const int pos[24], int twoGen, bool specificAngleBot = false, bool firstMatchOnly = false);

bool has2GenCorners(const int pos[24]);

bool partialHas2GenCorners(const int pos[24]);

// Whether the corners are 2g, checked once per valid preADF candidate
// specificAngleBot: from UI. restrict preADF candidates
// This is the entry point the solver guards and the Solve-button gate should use.
bool cornersAre2GenSolvable(const int pos[24], int twoGen, bool specificAngleBot = false);

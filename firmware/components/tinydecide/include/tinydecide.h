// TinyDecide device engine — public interface (filled in by milestone M1, see docs/PLAN.md).
//
// The engine answers plain-language questions (choice / noul / score / span) about one
// typed message in a single encoder pass. Reference implementation: model/tinydecide.js.
#pragma once
#include <stdint.h>

#define TD_MODEL_VARIANT "S768"   // model/model.bin, see model/SOURCE.md

#ifdef __cplusplus
extern "C" {
#endif

// v3 Q4_0 row-planar dot product against Q8_0 activations (dot_q4_pie.S, from cardputer_ai).
float dot_q4q8_pie(const uint8_t* nib, const int8_t* xq, const uint8_t* scales, const float* xs, int nb);

#ifdef __cplusplus
}
#endif

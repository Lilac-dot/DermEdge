// The tier-1 eczema-vs-normal CNN (MobileNetV1-0.25, 96x96, INT8) on TFLite Micro.
// Trained by scripts/train_esp32_rash_gate.py --data v1; the C array in model/ is its
// rash_gate_model_data.cc output.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Loads the model and allocates its tensor arena in PSRAM. Call once.
bool screen_model_init(void);

// rgb: a w x h frame, 3 bytes per pixel. swap_rb: the frame is B,G,R (esp32-camera's
// fmt2rgb888) and must be flipped to the R,G,B the model was trained on.
// Uses the centre square, resized to 96x96 -- the same crop as training.
// Returns P(eczema) in [0, 1], or -1 on an inference error.
float screen_model_predict(const uint8_t *rgb, int w, int h, bool swap_rb);

#ifdef __cplusplus
}
#endif

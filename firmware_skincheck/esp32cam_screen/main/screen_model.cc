#include "screen_model.h"

#include <math.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "rash_gate_model_data.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

static const char *TAG = "screen_model";

// Activations peak around 100 KB for MobileNetV1-0.25 at 96x96; PSRAM has room to spare.
static constexpr size_t kArenaSize = 300 * 1024;
static constexpr int kSide = 96;

static uint8_t *s_arena = nullptr;
static tflite::MicroInterpreter *s_interpreter = nullptr;

bool screen_model_init(void)
{
    const tflite::Model *model = tflite::GetModel(rash_gate_model_data);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "model schema %lu != runtime %d", (unsigned long)model->version(), TFLITE_SCHEMA_VERSION);
        return false;
    }
    // Exactly the ops the converted Keras model uses (checked with the TFLite interpreter).
    static tflite::MicroMutableOpResolver<8> resolver;
    resolver.AddConv2D();
    resolver.AddDepthwiseConv2D();
    resolver.AddPad();
    resolver.AddMul();
    resolver.AddAdd();
    resolver.AddMean();
    resolver.AddFullyConnected();
    resolver.AddLogistic();

    s_arena = (uint8_t *)heap_caps_malloc(kArenaSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_arena) {
        ESP_LOGE(TAG, "no PSRAM for the %u-byte arena -- is PSRAM enabled?", (unsigned)kArenaSize);
        return false;
    }
    static tflite::MicroInterpreter interpreter(model, resolver, s_arena, kArenaSize);
    if (interpreter.AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors failed (arena too small?)");
        return false;
    }
    s_interpreter = &interpreter;
    TfLiteTensor *in = interpreter.input(0);
    ESP_LOGI(TAG, "ready: input %dx%dx%d int8 (scale %.3f, zero %d), arena used %u bytes",
             in->dims->data[1], in->dims->data[2], in->dims->data[3], in->params.scale,
             (int)in->params.zero_point, (unsigned)interpreter.arena_used_bytes());
    return true;
}

float screen_model_predict(const uint8_t *rgb, int w, int h, bool swap_rb)
{
    if (!s_interpreter) {
        return -1.f;
    }
    TfLiteTensor *in = s_interpreter->input(0);
    const float scale = in->params.scale;
    const int zero = in->params.zero_point;

    // Centre square, box-averaged down to 96x96 (training: centre crop, then resize).
    const int side = w < h ? w : h;
    const int x0 = (w - side) / 2, y0 = (h - side) / 2;
    int8_t *dst = in->data.int8;
    for (int oy = 0; oy < kSide; oy++) {
        const int sy0 = y0 + oy * side / kSide, sy1 = y0 + (oy + 1) * side / kSide;
        for (int ox = 0; ox < kSide; ox++) {
            const int sx0 = x0 + ox * side / kSide, sx1 = x0 + (ox + 1) * side / kSide;
            uint32_t sum[3] = {0, 0, 0};
            int n = 0;
            for (int sy = sy0; sy < (sy1 > sy0 ? sy1 : sy0 + 1); sy++) {
                const uint8_t *row = rgb + (size_t)sy * w * 3;
                for (int sx = sx0; sx < (sx1 > sx0 ? sx1 : sx0 + 1); sx++) {
                    sum[0] += row[sx * 3];
                    sum[1] += row[sx * 3 + 1];
                    sum[2] += row[sx * 3 + 2];
                    n++;
                }
            }
            for (int c = 0; c < 3; c++) {
                const int src_c = swap_rb ? 2 - c : c;
                const float px = (float)sum[src_c] / n;
                int q = (int)lroundf(px / scale) + zero;  // pixel - 128 for this model
                dst[c] = (int8_t)(q < -128 ? -128 : q > 127 ? 127 : q);
            }
            dst += 3;
        }
    }

    if (s_interpreter->Invoke() != kTfLiteOk) {
        ESP_LOGE(TAG, "Invoke failed");
        return -1.f;
    }
    TfLiteTensor *out = s_interpreter->output(0);
    return (out->data.int8[0] - out->params.zero_point) * out->params.scale;
}

/* 
  This code is sourced from the project 'TestContinuous' from Dr. Blake
*/

#include "mic_code.h"

#include <Arduino.h>
#include <ESP_I2S.h>
#include <zogr288-project-1_inferencing.h>
#include <string.h> // used for strncpy & strcmp

#define PDM_CLK_PIN   42
#define PDM_DATA_PIN  41
#define SAMPLE_RATE   EI_CLASSIFIER_FREQUENCY
#define SAMPLE_COUNT  EI_CLASSIFIER_RAW_SAMPLE_COUNT
#define CAPTURE_CHUNK 2048
#define WARMUP_MS     30
#define CONFIDENCE_THRESHOLD 0.60f // 60% out of 100%
#define ML_LISTEN_MS  10000

typedef struct {
  int16_t *buffer;
  uint8_t buf_ready;
  uint32_t buf_count;
  uint32_t n_samples;
} inference_t;

static I2SClass i2s;
static inference_t inference;
static int16_t capture_buf[CAPTURE_CHUNK];
static bool record_status = false;
static bool debug_nn = false;
static bool mic_ready = false;

static bool mic_init_once(){
  if (mic_ready) return true;

  i2s.setPinsPdmRx(PDM_CLK_PIN, PDM_DATA_PIN);

  bool ok = i2s.begin(I2S_MODE_PDM_RX, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);

  if (!ok){
    Serial.println("[ERROR] PDM mic init failed.");
    return false;
  }

  size_t warmup_samples = (size_t)(SAMPLE_RATE * WARMUP_MS / 1000);
  char *discard = (char *)malloc(warmup_samples * sizeof(int16_t));
  if (discard){
    i2s.readBytes(discard, warmup_samples * sizeof(int16_t));
    free(discard);
  }

  mic_ready = true;
  return true;
}

static void audio_callback(uint32_t n_bytes){
  for (uint32_t i = 0; i < (n_bytes >> 1); i++){
    inference.buffer[inference.buf_count++] = capture_buf[i];
    if (inference.buf_count >= inference.n_samples){
      inference.buf_count = 0;
      inference.buf_ready = 1;
    }
  }
}

static void capture_task(void *arg){
  const int32_t bytes_to_read = (uint32_t)arg;

  while (record_status){
    size_t got = i2s.readBytes((char *)capture_buf, bytes_to_read);
    if (got <= 0){
      delay(1);
      continue;
    }

    for (int x = 0; x < bytes_to_read / 2; x++){
      capture_buf[x] = (int16_t)(capture_buf[x]) * 8;
    }

    if (record_status){
      audio_callback(bytes_to_read);
    }
  }

  vTaskDelete(NULL);
}

static bool inference_start(uint32_t n_samples){
  inference.buffer = (int16_t *)malloc(n_samples * sizeof(int16_t));
  if (!inference.buffer) return false;

  inference.buf_count = 0;
  inference.n_samples = n_samples;
  inference.buf_ready = 0;
  record_status = true;

  xTaskCreate(capture_task, "CaptureTask", 1024 * 32, (void *)CAPTURE_CHUNK, 10, NULL);

  return true;
}

static bool inference_record(uint32_t timeout_ms = 2000){
  unsigned long start = millis();
  while (inference.buf_ready == 0){
    if (millis() - start >= timeout_ms) return false;
    delay(10);
  }

  inference.buf_ready = 0;
  return true;
}

static void inference_end(){
  record_status = false;
  delay(20);

  if (inference.buffer){
    ei_free(inference.buffer);
    inference.buffer = nullptr;
  }
}

static int get_audio_signal_data(size_t offset, size_t length, float *out_ptr){
  numpy::int16_to_float(inference.buffer + offset, out_ptr, length);
  return EIDSP_OK;
}

static bool get_best_label(char *label_out, size_t label_out_size, float *score_out){
  signal_t signal;
  signal.total_length = SAMPLE_COUNT;
  signal.get_data = &get_audio_signal_data;

  ei_impulse_result_t result = { 0 };
  EI_IMPULSE_ERROR err = run_classifier(&signal, &result, debug_nn);
  if (err != EI_IMPULSE_OK){
    Serial.printf("[ERROR] run_classifier() returned %d\n", err);
    return false;
  }

  float best_val = 0.0f;
  int best_idx = -1;

  for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++){
    if (result.classification[i].value > best_val){
      best_val = result.classification[i].value;
      best_idx = (int)i;
    }
  }

  if (best_idx < 0) return false;

  strncpy(label_out, result.classification[best_idx].label, label_out_size - 1);
  label_out[label_out_size - 1] = '\0';
  *score_out = best_val;
  return true;
}

int mic_listen_for_word(const char *expected_word){
  if (!expected_word) return 0;
  if (!mic_init_once()) return 0;
  if (!inference_start(SAMPLE_COUNT)) return 0;

  Serial.print("Listening for password word: ");
  Serial.println(expected_word);

  unsigned long start_time = millis();
  int matched = 0;

  while (millis() - start_time < ML_LISTEN_MS){
    if (!inference_record()) continue;

    char heard_label[16] = "";
    float heard_score = 0.0f;

    if (!get_best_label(heard_label, sizeof(heard_label), &heard_score)) continue;

    Serial.print("Heard: ");
    Serial.print(heard_label);
    Serial.print("  score: ");
    Serial.println(heard_score, 4);

    // if greater than 60% confidence...
    if (heard_score >= CONFIDENCE_THRESHOLD &&
        strcmp(heard_label, expected_word) == 0){
      matched = 1;
      break;
    }
  }

  inference_end();
  return matched;
}

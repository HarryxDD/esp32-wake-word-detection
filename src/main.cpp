#include <freertos/FreeRTOS.h>
#include <driver/gpio.h>

#include <esp_log.h>

#include "Config.hpp"
#include "NeuralNetwork.hpp"
#include "AudioProcessor.hpp"
#include "MemsMicrophone.hpp"
#include "MemoryPool.hpp"

static const char* TAG = "ESP32 TFLITE WWD - Main";

static void setup_led() {
    gpio_config_t led_conf = {
        .pin_bit_mask = (1ULL << LED_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&led_conf);
    gpio_set_level(LED_PIN, 0); // LED off initially
}

static void led_blink(int times, int delay_ms) {
    for (int i = 0; i < times; i++) {
        gpio_set_level(LED_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        gpio_set_level(LED_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

extern "C" [[noreturn]] void
app_main()
{
    // Setup LED first
    setup_led();
    
    // Startup indication
    ESP_LOGI(TAG, "=== ESP32 Wake Word Detection Starting ===");
    ESP_LOGI(TAG, "Hardware: ESP32-WROOM-32 + INMP441");
    ESP_LOGI(TAG, "INMP441 Pins - SCK: %d, WS: %d, SD: %d", I2S_INMP441_SCK, I2S_INMP441_WS, I2S_INMP441_SD);
    ESP_LOGI(TAG, "LED Pin: %d", LED_PIN);
    ESP_LOGI(TAG, "Sample Rate: %d Hz", I2S_SAMPLE_RATE);
    
    // LED blink to show startup
    led_blink(3, 200);
    
    static const TickType_t kMaxBlockTime = pdMS_TO_TICKS(300);
    static const float kDetectionThreshold = 0.5;

    ESP_LOGI(TAG, "Initializing Neural Network...");
    NeuralNetwork nn;
    if (!nn.setUp()) {
        ESP_LOGE(TAG, "Unable to set-up neural network");
        // Error indication - fast blinks
        for(;;) {
            led_blink(5, 100);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    ESP_LOGI(TAG, "Neural Network initialized successfully");

    ESP_LOGI(TAG, "Initializing Microphone...");
    MemoryPool memoryPool;
    MemsMicrophone mic{memoryPool};
    if (!mic.start(xTaskGetCurrentTaskHandle())) {
        ESP_LOGE(TAG, "Unable to start microphone");
        // Error indication - different pattern
        for(;;) {
            led_blink(2, 300);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    ESP_LOGI(TAG, "Microphone started successfully");
    
    // Ready indication - single long blink
    gpio_set_level(LED_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(1000));
    gpio_set_level(LED_PIN, 0);
    
    ESP_LOGI(TAG, "=== Wake Word Detection Ready ===");
    ESP_LOGI(TAG, "Listening for 'Marvin'... (threshold: %.2f)", kDetectionThreshold);

    /**
     * Recognition loop (main task: CPU0)
     */
    AudioProcessor processor{WWD_AUDIO_LENGTH, WWD_WINDOW_SIZE, WWD_STEP_SIZE, WWD_POOLING_SIZE};
    uint32_t loop_count = 0;
    
    while (true) {
        if (ulTaskNotifyTake(pdTRUE, kMaxBlockTime) > 0 /* notification value after reset */) {
            auto buffer = mic.buffer();
            buffer.seek(buffer.pos() - I2S_SAMPLE_RATE);
            float* inputBuffer = nn.getInputBuffer();
            processor.getSpectrogram(buffer, inputBuffer);
            const float output = nn.predict();
            
            loop_count++;
            
            // Show activity every 100 loops (about every 30 seconds at 16kHz)
            if (loop_count % 100 == 0) {
                ESP_LOGI(TAG, "Processing... (loop: %lu, last output: %.3f)", loop_count, output);
            }
            
            if (output > kDetectionThreshold) {
                ESP_LOGI(TAG, "*** WAKE WORD DETECTED! *** Confidence: %.2f", output);
                // Wake word detected - bright LED for 2 seconds
                gpio_set_level(LED_PIN, 1);
                vTaskDelay(pdMS_TO_TICKS(2000));
                gpio_set_level(LED_PIN, 0);
            }
        } else {
            // Timeout - brief LED blink to show we're alive
            ESP_LOGW(TAG, "Timeout waiting for audio data");
            led_blink(1, 50);
        }
    }
}

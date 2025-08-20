#include <freertos/FreeRTOS.h>
#include <driver/gpio.h>

#include <esp_log.h>

#include "Config.hpp"
#include "NeuralNetwork.hpp"
#include "AudioProcessor.hpp"
#include "MemsMicrophone.hpp"
#include "MemoryPool.hpp"
#include "WiFiManager.hpp"
#include "MQTTManager.hpp"

static const char* TAG = "WWD";

// Global instances
static WiFiManager wifi_manager;
static MQTTManager mqtt_manager;
static float detection_threshold = 0.6f;
static int recording_duration = 5000; // ms

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

// MQTT configuration callback
static void mqtt_config_callback(const mqtt_config_t* config) {
    // Update global configuration
    recording_duration = config->record_ms;
    detection_threshold = config->min_conf;
    
    // Blink LED to indicate config update
    led_blink(2, 100);
}

// WiFi and MQTT setup function
static bool setup_connectivity() {
    ESP_LOGI(TAG, "🚀 Starting connectivity setup...");
    
    if (!wifi_manager.initialize()) {
        ESP_LOGE(TAG, "❌ Failed to initialize WiFi manager");
        return false;
    }
    ESP_LOGI(TAG, "✅ WiFi manager initialized");
    
    if (!wifi_manager.connect(WIFI_SSID, WIFI_PASSWORD)) {
        ESP_LOGE(TAG, "❌ Failed to connect to WiFi");
        return false;
    }
    
    ESP_LOGI(TAG, "✅ WiFi connection established");
    ESP_LOGI(TAG, "📡 Connected to: %s", WIFI_SSID);
    ESP_LOGI(TAG, "🌐 IP Address: %s", wifi_manager.getIPAddress());
    
    // Setup MQTT
    ESP_LOGI(TAG, "🔧 Setting up MQTT...");
    if (!mqtt_manager.initialize(MQTT_BROKER_HOST, MQTT_BROKER_PORT, DEVICE_ID, 
                                MQTT_USERNAME, MQTT_PASSWORD)) {
        ESP_LOGE(TAG, "❌ Failed to initialize MQTT manager");
        return false;
    }
    ESP_LOGI(TAG, "✅ MQTT manager initialized");
    
    mqtt_manager.setConfigCallback(mqtt_config_callback);
    
    if (!mqtt_manager.connect()) {
        ESP_LOGE(TAG, "❌ Failed to connect to MQTT broker");
        return false;
    }
    
    ESP_LOGI(TAG, "✅ MQTT connection established");
    ESP_LOGI(TAG, "🏠 Broker: %s:%d", MQTT_BROKER_HOST, MQTT_BROKER_PORT);
    ESP_LOGI(TAG, "🆔 Device ID: %s", DEVICE_ID);
    ESP_LOGI(TAG, "🌟 Connectivity setup complete!");
    return true;
}

extern "C" [[noreturn]] void
app_main()
{
    // Setup LED first
    setup_led();
    
    // LED blink to show startup
    led_blink(3, 200);
    
    // Setup WiFi and MQTT connectivity
    if (!setup_connectivity()) {
        ESP_LOGE(TAG, "Failed to setup connectivity");
        // Error indication - fast blinks indefinitely
        for(;;) {
            led_blink(10, 50);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    
    // Connectivity success indication
    led_blink(5, 100);
    
    static const TickType_t kMaxBlockTime = pdMS_TO_TICKS(300);

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
    
    ESP_LOGI(TAG, "Ready");
    
    // Ready indication - single long blink
    gpio_set_level(LED_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(1000));
    gpio_set_level(LED_PIN, 0);

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
            
            // Send heartbeat every 1000 loops
            if (loop_count % 1000 == 0 && mqtt_manager.isConnected()) {
                mqtt_manager.publishHeartbeat();
            }
            
            if (output > detection_threshold) {
                ESP_LOGI(TAG, "DETECTED! %.2f", output);
                
                // Create and publish simple MQTT alert
                mqtt_alert_t alert = {};
                strncpy(alert.device_id, DEVICE_ID, sizeof(alert.device_id) - 1);
                alert.confidence = output;
                
                if (mqtt_manager.isConnected()) {
                    mqtt_manager.publishAlert(&alert);
                }
                
                // Wake word detected - bright LED for 2 seconds
                gpio_set_level(LED_PIN, 1);
                vTaskDelay(pdMS_TO_TICKS(2000));
                gpio_set_level(LED_PIN, 0);
            }
        } else {
            // Timeout - brief LED blink to show we're alive
            ESP_LOGW(TAG, "Timeout waiting for audio data - checking connectivity...");
            led_blink(1, 50);
            
            // Check connectivity and try to reconnect if needed
            bool wifi_status = wifi_manager.isConnected();
            bool mqtt_status = mqtt_manager.isConnected();
            
            ESP_LOGI(TAG, "📊 Status - WiFi: %s | MQTT: %s | IP: %s", 
                    wifi_status ? "✅ Connected" : "❌ Disconnected",
                    mqtt_status ? "✅ Connected" : "❌ Disconnected",
                    wifi_manager.getIPAddress());
            
            if (!wifi_status) {
                ESP_LOGW(TAG, "🔄 WiFi disconnected - attempting reconnect...");
                wifi_manager.reconnect(WIFI_SSID, WIFI_PASSWORD);
            }
            
            if (!mqtt_status && wifi_status) {
                ESP_LOGW(TAG, "🔄 MQTT disconnected - attempting reconnect...");
                mqtt_manager.connect();
            }
        }
    }
}

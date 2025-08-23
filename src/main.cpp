#include <freertos/FreeRTOS.h>
#include <driver/gpio.h>
#include <esp_system.h>
#include <esp_log.h>

#include "Config.hpp"
#include "NeuralNetwork.hpp"
#include "AudioProcessor.hpp"
#include "MemsMicrophone.hpp"
#include "MemoryPool.hpp"
#include "WiFiManager.hpp"
#include "MQTTManager.hpp"
#include "bluetooth_provisioning.h" // Re-enabled for WiFi provisioning over Bluetooth

static const char* TAG = "WWD";

// Global instances
static WiFiManager wifi_manager;
static MQTTManager mqtt_manager;
static float detection_threshold = 0.6f;
static int recording_duration = 5000; // ms
// device_id is declared as extern in bluetooth_provisioning.h

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
    
    // If no stored WiFi, go straight to WiFi AP provisioning BEFORE WiFi init
    if (!has_stored_wifi()) {
        ESP_LOGW(TAG, "📡 No stored WiFi credentials found - entering WiFi AP provisioning mode");
        ESP_LOGI(TAG, "📱 Connect to WakeGuard-Setup-XXXX network to configure WiFi");
        
        // Try WiFi AP provisioning with proper error handling
        start_wifi_ap_provisioning();
        ESP_LOGI(TAG, "✅ Provisioning complete, continuing with normal operation...");
        
        // After provisioning, WiFi is already connected in APSTA mode
        // We need to switch to STA mode and continue without WiFi Manager init
        stop_provisioning_server();
        ESP_LOGI(TAG, "🔄 Switching from AP+STA mode to STA mode only...");
        esp_wifi_set_mode(WIFI_MODE_STA);
        ESP_LOGI(TAG, "✅ WiFi switched to STA mode, ready for normal operation");
        
        // Skip WiFi Manager initialization and go directly to MQTT setup
    } else {
        // Initialize WiFi manager only if we have stored credentials
        if (!wifi_manager.initialize()) {
            ESP_LOGE(TAG, "❌ Failed to initialize WiFi manager");
            return false;
        }
        ESP_LOGI(TAG, "✅ WiFi manager initialized");
        
        // Try to connect with stored credentials with retries
        ESP_LOGI(TAG, "🔄 Attempting to connect with stored credentials...");
        int connection_attempts = 0;
        const int max_attempts = 5;
        bool connected = false;
        
        while (connection_attempts < max_attempts && !connected) {
            connection_attempts++;
            ESP_LOGI(TAG, "🔌 WiFi connection attempt %d/%d", connection_attempts, max_attempts);
            
            if (wifi_manager.connectWithStoredCredentials()) {
                connected = true;
                ESP_LOGI(TAG, "✅ WiFi connection established on attempt %d", connection_attempts);
                break;
            } else {
                ESP_LOGE(TAG, "❌ WiFi connection attempt %d failed", connection_attempts);
                if (connection_attempts < max_attempts) {
                    ESP_LOGI(TAG, "⏳ Waiting 3 seconds before retry...");
                    vTaskDelay(pdMS_TO_TICKS(3000));
                }
            }
        }
        
        if (!connected) {
            ESP_LOGE(TAG, "❌ Failed to connect to WiFi after %d attempts", max_attempts);
            ESP_LOGE(TAG, "🔌 Device will continue without WiFi - check network availability");
            return false;
        }
        ESP_LOGI(TAG, "🌐 IP Address: %s", wifi_manager.getIPAddress());
    }
    
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
    
    // Generate runtime Bluetooth device ID (used by provisioning module)
    device_id = generate_device_id();
    ESP_LOGI(TAG, "🆔 Device ID: %s (BT:%s)", DEVICE_ID, device_id.c_str());
    
    // Try to setup connectivity first
    if (!setup_connectivity()) {
        ESP_LOGE(TAG, "❌ Failed to setup connectivity");
        ESP_LOGI(TAG, "🗑️ Clearing any corrupted WiFi credentials...");
        clear_stored_wifi();
        ESP_LOGI(TAG, "🔵 Starting WiFi AP provisioning mode...");
        
        // Try WiFi AP provisioning with proper error handling
        start_wifi_ap_provisioning();
        
        // Restart to use new credentials
        ESP_LOGI(TAG, "🔄 Restarting to apply new WiFi credentials...");
        esp_restart();
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
                wifi_manager.reconnect(NULL, NULL);  // Use stored credentials
            }
            
            if (!mqtt_status && wifi_status) {
                ESP_LOGW(TAG, "🔄 MQTT disconnected - attempting reconnect...");
                mqtt_manager.connect();
            }
        }
    }
}

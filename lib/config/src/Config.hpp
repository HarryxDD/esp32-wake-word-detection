#include <driver/i2s_std.h>

#define I2S_DMA_BUFFER_COUNT (4) // The total amount of DMA buffers count
#define I2S_DMA_BUFFER_LEN (64)  // The length of each DMA buffer in samples
#define I2S_SAMPLE_BYTES (4)     // The total aboumt of bytes per sample
#define I2S_SAMPLE_RATE (16000)  // The total amount of samples per second

// WiFi Configuration
#define WIFI_SSID "DNA-WIFI-A474"
#define WIFI_PASSWORD "7tADYEhq"

// MQTT Configuration
#define MQTT_BROKER_HOST "192.168.1.176"  // Replace with your Raspberry Pi IP
#define MQTT_BROKER_PORT 1883
#define MQTT_USERNAME "harryxd"          // Replace with your MQTT username
#define MQTT_PASSWORD "harryxd"      // Replace with your MQTT password
#define DEVICE_ID "esp32_wwd_001"  // Unique device identifier
#define DEVICE_LOCATION "living_room"  // Device location

// MQTT Topics
#define MQTT_TOPIC_ALERTS "alerts/" DEVICE_ID
#define MQTT_TOPIC_STATUS "devices/" DEVICE_ID "/status"
#define MQTT_TOPIC_HEARTBEAT "devices/" DEVICE_ID "/heartbeat"
#define MQTT_TOPIC_CONFIG "config/" DEVICE_ID

// MQTT Configuration
#define MQTT_HEARTBEAT_INTERVAL_MS 30000  // 30 seconds
#define MQTT_KEEPALIVE_SEC 60
#define MQTT_CLEAN_SESSION true

// INMP441 Pin Configuration (user's correct pins)
#define I2S_INMP441_SCK (GPIO_NUM_32)  // Serial Clock
#define I2S_INMP441_WS (GPIO_NUM_25)   // Word Select (Left/Right Clock)
#define I2S_INMP441_SD (GPIO_NUM_33)   // Serial Data

// LED Pin (separate from microphone pins)
#define LED_PIN (GPIO_NUM_26)  // LED pin as specified by user

#define WWD_WINDOW_SIZE (320)
#define WWD_STEP_SIZE (160)
#define WWD_POOLING_SIZE (6)
#define WWD_AUDIO_LENGTH (I2S_SAMPLE_RATE)

/* Setting the configurations */
const i2s_std_config_t I2S_CONFIG = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE),
    /***
     * INMP1441:
     *  - 64 SCK cycles in each WS stereo frame (or 32 SCK cycles per data-word)
     *  - 24bit per channel
     *  - MSB first with one SCK cycle delay
     ***/
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
        .mclk = I2S_GPIO_UNUSED,
        .bclk = I2S_INMP441_SCK,
        .ws = I2S_INMP441_WS,
        .dout = I2S_GPIO_UNUSED,
        .din = I2S_INMP441_SD,
        .invert_flags = {
            .mclk_inv = false,
            .bclk_inv = false,
            .ws_inv = false,
        },
    },
};

#pragma once

#include <mqtt_client.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <cJSON.h>

typedef struct {
    char device_id[32];
    float confidence;
} mqtt_alert_t;

typedef struct {
    int record_ms;
    float min_conf;
} mqtt_config_t;

typedef void (*mqtt_config_callback_t)(const mqtt_config_t* config);

class MQTTManager {
public:
    MQTTManager();
    ~MQTTManager();
    
    bool initialize(const char* broker_host, int broker_port, const char* device_id, 
                   const char* username = nullptr, const char* password = nullptr);
    bool connect();
    void disconnect();
    bool isConnected();
    
    // Publishing functions
    bool publishAlert(const mqtt_alert_t* alert);
    bool publishStatus(const char* status); // "online" or "offline"
    bool publishHeartbeat();
    
    // Configuration subscription
    void setConfigCallback(mqtt_config_callback_t callback);
    
private:
    esp_mqtt_client_handle_t client;
    mqtt_config_callback_t config_callback;
    
    char device_id[16];
    char topic_alerts[32];
    char topic_status[32];
    char topic_heartbeat[32];
    char topic_config[32];
    
    bool connected;
    
    // Static callbacks
    static void mqtt_event_handler(void *handler_args, esp_event_base_t base, 
                                 int32_t event_id, void *event_data);
    
    // Helper functions
    void handleConfigMessage(const char* data, int data_len);
    
    static const char* TAG;
};

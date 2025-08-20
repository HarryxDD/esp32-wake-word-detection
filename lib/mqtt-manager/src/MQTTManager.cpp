#include "MQTTManager.hpp"
#include <string.h>

const char* MQTTManager::TAG = "MQTT";

MQTTManager::MQTTManager() 
    : client(nullptr), config_callback(nullptr), connected(false) {
    memset(device_id, 0, sizeof(device_id));
    memset(topic_alerts, 0, sizeof(topic_alerts));
    memset(topic_status, 0, sizeof(topic_status));
    memset(topic_heartbeat, 0, sizeof(topic_heartbeat));
    memset(topic_config, 0, sizeof(topic_config));
}

MQTTManager::~MQTTManager() {
    disconnect();
}

bool MQTTManager::initialize(const char* broker_host, int broker_port, const char* device_id_param,
                            const char* username, const char* password) {
    // Store device ID and build topics
    strncpy(device_id, device_id_param, sizeof(device_id) - 1);
    snprintf(topic_alerts, sizeof(topic_alerts), "alerts/%s", device_id);
    snprintf(topic_status, sizeof(topic_status), "devices/%s/status", device_id);
    snprintf(topic_heartbeat, sizeof(topic_heartbeat), "devices/%s/hb", device_id);
    snprintf(topic_config, sizeof(topic_config), "config/%s", device_id);
    
    // Build the MQTT URI
    char broker_uri[128];
    snprintf(broker_uri, sizeof(broker_uri), "mqtt://%s:%d", broker_host, broker_port);
    
    // MQTT client configuration
    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = broker_uri;
    mqtt_cfg.session.keepalive = 60;
    mqtt_cfg.session.disable_clean_session = false;
    
    // Authentication (if provided)
    if (username && password) {
        mqtt_cfg.credentials.username = username;
        mqtt_cfg.credentials.authentication.password = password;
    }
    
    // Last Will Testament - simple offline status
    mqtt_cfg.session.last_will.topic = topic_status;
    mqtt_cfg.session.last_will.msg = "offline";
    mqtt_cfg.session.last_will.msg_len = 7;
    mqtt_cfg.session.last_will.qos = 1;
    mqtt_cfg.session.last_will.retain = true;
    
    // Create MQTT client
    client = esp_mqtt_client_init(&mqtt_cfg);
    if (!client) {
        return false;
    }
    
    // Register event handler
    esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, mqtt_event_handler, this);
    
    return true;
}

bool MQTTManager::connect() {
    if (!client) {
        return false;
    }
    
    esp_err_t ret = esp_mqtt_client_start(client);
    return (ret == ESP_OK);
}

void MQTTManager::disconnect() {
    if (client) {
        // Publish offline status before disconnecting
        publishStatus("offline");
        esp_mqtt_client_stop(client);
        esp_mqtt_client_destroy(client);
        client = nullptr;
    }
    
    connected = false;
}

bool MQTTManager::isConnected() {
    return connected;
}

bool MQTTManager::publishAlert(const mqtt_alert_t* alert) {
    if (!connected || !client) {
        return false;
    }
    
    // Create simple JSON payload - let Raspberry Pi add timestamp and details
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "id", alert->device_id);
    cJSON_AddNumberToObject(json, "conf", alert->confidence);
    
    char *json_string = cJSON_Print(json);
    
    // Publish with QoS 1
    int msg_id = esp_mqtt_client_publish(client, topic_alerts, json_string, 
                                        strlen(json_string), 1, false);
    
    free(json_string);
    cJSON_Delete(json);
    
    return msg_id != -1;
}

bool MQTTManager::publishStatus(const char* status) {
    if (!client) {
        return false;
    }
    
    // Simple status message
    int msg_id = esp_mqtt_client_publish(client, topic_status, status, 
                                        strlen(status), 1, true);
    
    return msg_id != -1;
}

bool MQTTManager::publishHeartbeat() {
    if (!connected || !client) {
        return false;
    }
    
    // Super simple heartbeat - just "1"
    int msg_id = esp_mqtt_client_publish(client, topic_heartbeat, "1", 1, 0, false);
    
    return msg_id != -1;
}

void MQTTManager::setConfigCallback(mqtt_config_callback_t callback) {
    config_callback = callback;
}

void MQTTManager::mqtt_event_handler(void *handler_args, esp_event_base_t base, 
                                   int32_t event_id, void *event_data) {
    MQTTManager* mqtt_mgr = static_cast<MQTTManager*>(handler_args);
    esp_mqtt_event_handle_t event = static_cast<esp_mqtt_event_handle_t>(event_data);
    
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            mqtt_mgr->connected = true;
            
            // Subscribe to config topic
            esp_mqtt_client_subscribe(mqtt_mgr->client, mqtt_mgr->topic_config, 1);
            
            // Publish online status
            mqtt_mgr->publishStatus("online");
            break;
            
        case MQTT_EVENT_DISCONNECTED:
            mqtt_mgr->connected = false;
            break;
            
        case MQTT_EVENT_DATA:
            // Check if it's a config message
            if (strncmp(event->topic, mqtt_mgr->topic_config, event->topic_len) == 0) {
                mqtt_mgr->handleConfigMessage(event->data, event->data_len);
            }
            break;
            
        default:
            break;
    }
}

void MQTTManager::handleConfigMessage(const char* data, int data_len) {
    if (!config_callback) {
        return;
    }
    
    // Create null-terminated string
    char* json_str = (char*)malloc(data_len + 1);
    memcpy(json_str, data, data_len);
    json_str[data_len] = '\0';
    
    // Parse JSON
    cJSON *json = cJSON_Parse(json_str);
    if (!json) {
        free(json_str);
        return;
    }
    
    // Extract configuration values
    mqtt_config_t config = {};
    
    cJSON *record_ms = cJSON_GetObjectItemCaseSensitive(json, "record_ms");
    if (cJSON_IsNumber(record_ms)) {
        config.record_ms = record_ms->valueint;
    } else {
        config.record_ms = 5000; // Default
    }
    
    cJSON *min_conf = cJSON_GetObjectItemCaseSensitive(json, "min_conf");
    if (cJSON_IsNumber(min_conf)) {
        config.min_conf = (float)min_conf->valuedouble;
    } else {
        config.min_conf = 0.75f; // Default
    }
    
    // Call callback
    config_callback(&config);
    
    free(json_str);
    cJSON_Delete(json);
}

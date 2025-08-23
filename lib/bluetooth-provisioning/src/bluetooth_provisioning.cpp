#include "bluetooth_provisioning.h"
#include "Config.hpp"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_system.h>
#include <driver/gpio.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <lwip/err.h>
#include <lwip/sys.h>
#include <esp_efuse.h>
#include <esp_mac.h>
#include <string.h>
#include <stdlib.h>

// Web server includes
#include <esp_http_server.h>
#include <esp_netif.h>
#include <lwip/ip_addr.h>
#include <esp_netif_ip_addr.h>

// JSON parsing
#include "cJSON.h"

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

static const char* TAG = "WIFI_PROVISIONING";

// Global variables
std::string device_id;
bool wifi_configured = false;
static httpd_handle_t server = NULL;
static esp_netif_t* ap_netif = NULL;
static esp_netif_t* sta_netif = NULL;

// WiFi event handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

// HTTP handlers
static esp_err_t root_handler(httpd_req_t *req);
static esp_err_t scan_handler(httpd_req_t *req);
static esp_err_t connect_handler(httpd_req_t *req);
static esp_err_t status_handler(httpd_req_t *req);

// Forward declarations
void blink_led(int times, int delay_ms);
bool test_wifi_connection(const char* ssid, const char* password);

// HTML page for captive portal
static const char* setup_page_html = R"html(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>WakeGuard Setup</title>
    <style>
        body { font-family: Arial, sans-serif; margin: 20px; background: #f5f5f5; }
        .container { max-width: 400px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }
        h1 { color: #2c3e50; text-align: center; }
        .form-group { margin-bottom: 15px; }
        label { display: block; margin-bottom: 5px; font-weight: bold; }
        select, input[type="password"], button { width: 100%; padding: 10px; border: 1px solid #ddd; border-radius: 5px; box-sizing: border-box; }
        button { background: #3498db; color: white; border: none; cursor: pointer; margin-top: 10px; }
        button:hover { background: #2980b9; }
        .status { margin-top: 15px; padding: 10px; border-radius: 5px; text-align: center; }
        .success { background: #d4edda; color: #155724; border: 1px solid #c3e6cb; }
        .error { background: #f8d7da; color: #721c24; border: 1px solid #f5c6cb; }
        .loading { background: #fff3cd; color: #856404; border: 1px solid #ffeaa7; }
    </style>
</head>
<body>
    <div class="container">
        <h1>WakeGuard Setup</h1>
        <form id="wifiForm">
            <div class="form-group">
                <label for="ssid">Select WiFi Network:</label>
                <select id="ssid" name="ssid" required>
                    <option value="">Scanning networks...</option>
                </select>
                <button type="button" onclick="refreshNetworks()">Refresh</button>
            </div>
            <div class="form-group">
                <label for="password">WiFi Password:</label>
                <input type="password" id="password" name="password" placeholder="Enter WiFi password">
            </div>
            <button type="submit">Connect to WiFi</button>
        </form>
        <div id="status"></div>
    </div>

    <script>
        function showStatus(message, type) {
            const status = document.getElementById('status');
            status.innerHTML = message;
            status.className = 'status ' + type;
        }

        function refreshNetworks() {
            showStatus('Scanning WiFi networks...', 'loading');
            fetch('/scan')
                .then(response => response.json())
                .then(data => {
                    const select = document.getElementById('ssid');
                    select.innerHTML = '<option value="">Select a network...</option>';
                    data.networks.forEach(network => {
                        const option = document.createElement('option');
                        option.value = network.ssid;
                        option.textContent = network.ssid + ' (' + network.rssi + ' dBm) ' + (network.auth ? '[SECURED]' : '[OPEN]');
                        select.appendChild(option);
                    });
                    showStatus('Found ' + data.networks.length + ' networks', 'success');
                })
                .catch(error => {
                    showStatus('Failed to scan networks: ' + error, 'error');
                });
        }

        document.getElementById('wifiForm').addEventListener('submit', function(e) {
            e.preventDefault();
            const ssid = document.getElementById('ssid').value;
            const password = document.getElementById('password').value;
            
            if (!ssid) {
                showStatus('Please select a WiFi network', 'error');
                return;
            }

            showStatus('Connecting to ' + ssid + '...', 'loading');
            
            fetch('/connect', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ ssid: ssid, password: password })
            })
            .then(response => response.json())
            .then(data => {
                if (data.success) {
                    showStatus('Successfully connected! Device will restart...', 'success');
                    setTimeout(() => {
                        showStatus('Setup complete! You can now close this page.', 'success');
                    }, 3000);
                } else {
                    showStatus('Connection failed: ' + data.message, 'error');
                }
            })
            .catch(error => {
                showStatus('Connection error: ' + error, 'error');
            });
        });

        // Auto-load networks on page load
        window.onload = function() {
            refreshNetworks();
        };
    </script>
</body>
</html>
)html";

// WiFi event handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    ESP_LOGI(TAG, "📡 WiFi Event: base=%s, id=%ld", 
             (event_base == WIFI_EVENT) ? "WIFI_EVENT" : "IP_EVENT", event_id);
             
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "📱 Device connected to AP, MAC: " MACSTR, MAC2STR(event->mac));
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "📱 Device disconnected from AP, MAC: " MACSTR, MAC2STR(event->mac));
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "📡 WiFi station started");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        wifi_event_sta_connected_t* event = (wifi_event_sta_connected_t*) event_data;
        ESP_LOGI(TAG, "✅ Connected to WiFi network: %s (channel %d)", event->ssid, event->channel);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*) event_data;
        ESP_LOGE(TAG, "❌ Disconnected from WiFi network: %s (reason: %d)", event->ssid, event->reason);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "🌐 Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "🔧 Setting wifi_configured = true");
        wifi_configured = true;
    } else {
        ESP_LOGI(TAG, "🔍 Unhandled WiFi event: base=%s, id=%ld", 
                 (event_base == WIFI_EVENT) ? "WIFI_EVENT" : 
                 (event_base == IP_EVENT) ? "IP_EVENT" : "UNKNOWN", event_id);
    }
}

std::string generate_device_id() {
    // Read base MAC without requiring WiFi init
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    uint32_t chipid = (mac[2] << 24) | (mac[3] << 16) | (mac[4] << 8) | mac[5];
    char device_id_str[32];
    snprintf(device_id_str, sizeof(device_id_str), "esp32_wwd_%08lx", (unsigned long)chipid);
    return std::string(device_id_str);
}

bool has_stored_wifi() {
    ESP_LOGI(TAG, "🔍 Checking for stored WiFi credentials...");
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("wifi", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "❌ Failed to open NVS for wifi namespace: %s", esp_err_to_name(err));
        return false;
    }
    
    size_t required_size = 0;
    err = nvs_get_str(nvs_handle, "ssid", NULL, &required_size);
    ESP_LOGI(TAG, "📊 NVS get_str result: %s, required_size: %d", esp_err_to_name(err), required_size);
    
    nvs_close(nvs_handle);
    
    bool has_wifi = (err == ESP_OK && required_size > 1);
    ESP_LOGI(TAG, "🔍 Stored WiFi check result: %s", has_wifi ? "FOUND" : "NOT FOUND");
    
    return has_wifi;
}

void clear_stored_wifi() {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("wifi", NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        nvs_erase_all(nvs_handle);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "🗑️ Cleared stored WiFi credentials");
    }
}

// HTTP Handler: Serve the main setup page
static esp_err_t root_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, setup_page_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// HTTP Handler: Scan for WiFi networks
static esp_err_t scan_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "🔍 Scanning WiFi networks...");
    
    // Start WiFi scan
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {
            .active = {
                .min = 100,
                .max = 300
            },
            .passive = 120
        },
        .home_chan_dwell_time = 0,
        .channel_bitmap = {0, 0}
    };    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Scan failed");
        return ESP_FAIL;
    }
    
    // Get scan results
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    
    if (ap_count == 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"networks\":[]}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    wifi_ap_record_t *ap_records = (wifi_ap_record_t*)malloc(sizeof(wifi_ap_record_t) * ap_count);
    esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    
    // Build JSON response
    cJSON *json = cJSON_CreateObject();
    cJSON *networks = cJSON_CreateArray();
    
    for (int i = 0; i < ap_count; i++) {
        cJSON *network = cJSON_CreateObject();
        cJSON_AddStringToObject(network, "ssid", (char*)ap_records[i].ssid);
        cJSON_AddNumberToObject(network, "rssi", ap_records[i].rssi);
        cJSON_AddBoolToObject(network, "auth", ap_records[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(networks, network);
    }
    
    cJSON_AddItemToObject(json, "networks", networks);
    char *json_string = cJSON_Print(json);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_string, strlen(json_string));
    
    free(json_string);
    cJSON_Delete(json);
    free(ap_records);
    
    ESP_LOGI(TAG, "✅ Found %d networks", ap_count);
    return ESP_OK;
}

// HTTP Handler: Connect to WiFi network
static esp_err_t connect_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "📨 Received connect request, content length: %d", req->content_len);
    
    char content[512];
    size_t recv_size = MIN(req->content_len, sizeof(content));
    
    int ret = httpd_req_recv(req, content, recv_size);
    if (ret <= 0) {
        ESP_LOGE(TAG, "❌ Failed to receive data, ret: %d", ret);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive data");
        return ESP_FAIL;
    }
    content[ret] = '\0';
    
    ESP_LOGI(TAG, "📝 Received JSON: %s", content);
    
    // Parse JSON
    cJSON *json = cJSON_Parse(content);
    if (json == NULL) {
        ESP_LOGE(TAG, "❌ Invalid JSON received");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    
    cJSON *ssid_json = cJSON_GetObjectItem(json, "ssid");
    cJSON *password_json = cJSON_GetObjectItem(json, "password");
    
    if (!cJSON_IsString(ssid_json)) {
        ESP_LOGE(TAG, "❌ Missing or invalid SSID in JSON");
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
        return ESP_FAIL;
    }
    
    const char *ssid = ssid_json->valuestring;
    const char *password = cJSON_IsString(password_json) ? password_json->valuestring : "";
    
    ESP_LOGI(TAG, "� Parsed credentials - SSID: '%s', Password: '%s'", ssid, password);
    ESP_LOGI(TAG, "�🔗 Attempting to connect to WiFi: %s", ssid);
    
    // Test WiFi connection
    bool success = test_wifi_connection(ssid, password);
    
    ESP_LOGI(TAG, "📊 WiFi connection result: %s", success ? "SUCCESS" : "FAILED");
    
    // Prepare response
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", success);
    if (success) {
        ESP_LOGI(TAG, "✅ WiFi connection successful, storing credentials in NVS");
        cJSON_AddStringToObject(response, "message", "Connected successfully");
        // Store credentials
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open("wifi", NVS_READWRITE, &nvs_handle);
        if (err == ESP_OK) {
            nvs_set_str(nvs_handle, "ssid", ssid);
            nvs_set_str(nvs_handle, "password", password);
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
            ESP_LOGI(TAG, "💾 WiFi credentials stored in NVS");
        } else {
            ESP_LOGE(TAG, "❌ Failed to open NVS for storing credentials: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "❌ WiFi connection failed");
        cJSON_AddStringToObject(response, "message", "Connection failed");
    }
    
    char *response_string = cJSON_Print(response);
    ESP_LOGI(TAG, "📤 Sending response: %s", response_string);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response_string, strlen(response_string));
    
    free(response_string);
    cJSON_Delete(response);
    cJSON_Delete(json);
    
    if (success) {
        // WiFi configured successfully - let the main code continue instead of restarting
        ESP_LOGI(TAG, "✅ WiFi configured successfully, returning to main flow");
        // Note: wifi_configured is already set to true in test_wifi_connection()
        // The provisioning loop will exit and main code will continue
    }
    
    return ESP_OK;
}

// HTTP Handler: Get status
static esp_err_t status_handler(httpd_req_t *req) {
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "device_id", device_id.c_str());
    cJSON_AddBoolToObject(json, "wifi_configured", wifi_configured);
    cJSON_AddBoolToObject(json, "wifi_connected", wifi_configured); // Same as wifi_configured since it's set on IP assignment
    
    char *json_string = cJSON_Print(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_string, strlen(json_string));
    
    free(json_string);
    cJSON_Delete(json);
    return ESP_OK;
}

void start_wifi_ap_provisioning() {
    ESP_LOGI(TAG, "🔵 Starting WiFi AP provisioning...");
    
    // Generate device ID first if not already set
    if (device_id.empty()) {
        device_id = generate_device_id();
        ESP_LOGI(TAG, "Generated device ID: %s", device_id.c_str());
    }
    
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Initialize WiFi
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Create WiFi interfaces
    ap_netif = esp_netif_create_default_wifi_ap();
    sta_netif = esp_netif_create_default_wifi_sta();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // Register event handler
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    
    // Configure WiFi AP
    std::string ap_ssid = "WakeGuard-Setup-" + device_id.substr(device_id.length() - 4);
    wifi_config_t wifi_config = {};
    strcpy((char*)wifi_config.ap.ssid, ap_ssid.c_str());
    wifi_config.ap.ssid_len = strlen(ap_ssid.c_str());
    strcpy((char*)wifi_config.ap.password, ""); // Open network
    wifi_config.ap.channel = 1;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(TAG, "📡 WiFi AP started: %s", ap_ssid.c_str());
    ESP_LOGI(TAG, "🌐 Connect to this network and go to: http://192.168.4.1");
    
    // Start HTTP server
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;
    
    if (httpd_start(&server, &config) == ESP_OK) {
        // Register URI handlers
        httpd_uri_t root_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &root_uri);
        
        httpd_uri_t scan_uri = {
            .uri = "/scan",
            .method = HTTP_GET,
            .handler = scan_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &scan_uri);
        
        httpd_uri_t connect_uri = {
            .uri = "/connect",
            .method = HTTP_POST,
            .handler = connect_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &connect_uri);
        
        httpd_uri_t status_uri = {
            .uri = "/status",
            .method = HTTP_GET,
            .handler = status_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &status_uri);
        
        ESP_LOGI(TAG, "✅ HTTP server started on port 80");
    } else {
        ESP_LOGE(TAG, "❌ Failed to start HTTP server");
    }
    
    // Wait for provisioning to complete
    while (!wifi_configured) {
        blink_led(1, 500);  // Slow blink to indicate setup mode
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    ESP_LOGI(TAG, "🔵 WiFi provisioning completed");
    
    // Stop the provisioning server and switch to STA mode
    stop_provisioning_server();
    
    ESP_LOGI(TAG, "🔄 Switching from AP+STA mode to STA mode only...");
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_LOGI(TAG, "✅ WiFi switched to STA mode, ready for normal operation");
}

void stop_provisioning_server() {
    if (server) {
        httpd_stop(server);
        server = NULL;
        ESP_LOGI(TAG, "🛑 HTTP server stopped");
    }
}

bool test_wifi_connection(const char* ssid, const char* password) {
    ESP_LOGI(TAG, "🧪 Testing WiFi connection to: %s", ssid);
    ESP_LOGI(TAG, "🔐 Password length: %d", strlen(password));
    
    // Configure WiFi station
    wifi_config_t wifi_config = {};
    strcpy((char*)wifi_config.sta.ssid, ssid);
    strcpy((char*)wifi_config.sta.password, password);
    
    ESP_LOGI(TAG, "⚙️  Configuring WiFi station with SSID: %s", (char*)wifi_config.sta.ssid);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    
    ESP_LOGI(TAG, "🔌 Attempting WiFi connection...");
    ESP_ERROR_CHECK(esp_wifi_connect());
    
    // Wait for connection (timeout after 10 seconds)
    int timeout = 10000; // 10 seconds
    ESP_LOGI(TAG, "⏳ Waiting for WiFi connection (timeout: %d ms)...", timeout);
    
    while (timeout > 0 && !wifi_configured) {
        if (timeout % 1000 == 0) {
            ESP_LOGI(TAG, "⏰ Still waiting... %d seconds remaining", timeout / 1000);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        timeout -= 100;
    }
    
    if (wifi_configured) {
        ESP_LOGI(TAG, "✅ WiFi connection test successful! wifi_configured = true");
        return true;
    } else {
        ESP_LOGE(TAG, "❌ WiFi connection test failed after timeout! wifi_configured = false");
        ESP_LOGI(TAG, "🔌 Disconnecting from WiFi...");
        esp_wifi_disconnect();
        return false;
    }
}

void connect_to_stored_wifi() {
    // This function is called when we have stored WiFi credentials
    // and want to connect normally (not in provisioning mode)
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("wifi", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS");
        return;
    }
    
    size_t ssid_len = 32;
    size_t password_len = 64;
    char ssid[33] = {0};
    char password[65] = {0};
    
    err = nvs_get_str(nvs_handle, "ssid", ssid, &ssid_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SSID from NVS");
        nvs_close(nvs_handle);
        return;
    }
    
    err = nvs_get_str(nvs_handle, "password", password, &password_len);
    nvs_close(nvs_handle);
    
    ESP_LOGI(TAG, "🔗 Connecting to stored WiFi: %s", ssid);
    
    // Initialize WiFi if not already initialized
    if (sta_netif == NULL) {
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        sta_netif = esp_netif_create_default_wifi_sta();
        
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    }
    
    // Configure and connect
    wifi_config_t wifi_config = {};
    strcpy((char*)wifi_config.sta.ssid, ssid);
    strcpy((char*)wifi_config.sta.password, password);
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());
}

void blink_led(int times, int delay_ms) {
    for (int i = 0; i < times; i++) {
        gpio_set_level(LED_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        gpio_set_level(LED_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

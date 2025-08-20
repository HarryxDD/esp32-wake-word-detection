#pragma once

#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <nvs_flash.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

class WiFiManager {
public:
    WiFiManager();
    ~WiFiManager();
    
    bool initialize();
    bool connect(char* ssid, char* password);
    bool isConnected();
    void disconnect();
    void reconnect(char* ssid, char* password);
    
    // Get IP address as string
    const char* getIPAddress();
    
private:
    bool connected;
    char ip_address[16];
    bool ap_fallback;
    char stored_ssid[32];
    char stored_password[64];
    
    static const char* TAG;
};

#pragma once

#include <string>

// Global variables for provisioning state
extern std::string device_id;
extern bool wifi_configured;

// Function declarations
std::string generate_device_id();
bool has_stored_wifi();
void clear_stored_wifi();
void start_wifi_ap_provisioning();
void connect_to_stored_wifi();
void stop_provisioning_server();
bool test_wifi_connection(const char* ssid, const char* password);

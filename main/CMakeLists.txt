set(ENV_FILE "${CMAKE_CURRENT_LIST_DIR}/../.env")

if(NOT EXISTS "${ENV_FILE}")
    message(FATAL_ERROR ".env file not found!")
endif()

file(STRINGS "${ENV_FILE}" ENV_LINES)

foreach(LINE IN LISTS ENV_LINES)
    string(STRIP "${LINE}" LINE)

    if(LINE MATCHES "^WIFI_SSID=(.*)$")
        set(WIFI_SSID "${CMAKE_MATCH_1}")
    elseif(LINE MATCHES "^WIFI_PASSWORD=(.*)$")
        set(WIFI_PASSWORD "${CMAKE_MATCH_1}")
    elseif(LINE MATCHES "^API_URL=(.*)$")
        set(API_URL "${CMAKE_MATCH_1}")
    endif()
endforeach()

if(WIFI_SSID STREQUAL "" OR WIFI_PASSWORD STREQUAL "" OR API_URL STREQUAL "")
    message(FATAL_ERROR "WiFi credentials or API URL missing in .env")
endif()

configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/secret.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/secret.h"
    @ONLY
)

idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "." "${CMAKE_CURRENT_BINARY_DIR}"
    REQUIRES
        esp-hub75
        esp_http_client
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
)
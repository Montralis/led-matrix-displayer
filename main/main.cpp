#include <stdio.h>
#include <string.h>
#include <cstdlib>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_err.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_timer.h"

#include "nvs_flash.h"
#include "cJSON.h"

#include "lvgl.h"
#include "hub75.h"

#include "secret.h"

// ============================================================
// Configuration
// ============================================================

static const char *TAG = "main";

static constexpr uint32_t API_INTERVAL_MS = 5000;
static constexpr size_t HTTP_BUFFER_SIZE = 1024;

#define WIFI_CONNECTED_BIT BIT0

// ============================================================
// WiFi
// ============================================================

static EventGroupHandle_t wifi_event_group = nullptr;

// ============================================================
// LVGL
// ============================================================

static SemaphoreHandle_t lvgl_mutex = nullptr;

static lv_obj_t *callsign_label = nullptr;
static lv_obj_t *from_label = nullptr;
static lv_obj_t *to_label = nullptr;
static lv_obj_t *arrow_label = nullptr;
static lv_display_t *lvgl_display = nullptr;

// ============================================================
// Display update queue
// ============================================================

struct FlightDisplayUpdate
{
    bool has_flight;
    char callsign[32];
    char from[8];
    char to[8];
};

static QueueHandle_t display_queue = nullptr;

// ============================================================
// LVGL mutex
// ============================================================

static void lvgl_lock()
{
    xSemaphoreTakeRecursive(lvgl_mutex, portMAX_DELAY);
}

static void lvgl_unlock()
{
    xSemaphoreGiveRecursive(lvgl_mutex);
}

// LVGL needs a monotonically increasing millisecond tick for its refresh timers.
static uint32_t lvgl_tick_get_cb()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

// ============================================================
// Publish display update
// Called by API task. LVGL objects are NOT accessed here.
// ============================================================

static void publish_display_update(
    bool has_flight,
    const char *callsign = "",
    const char *from = "",
    const char *to = "")
{
    if (display_queue == nullptr)
    {
        ESP_LOGE(TAG, "Display queue not initialized");
        return;
    }

    FlightDisplayUpdate update{};
    update.has_flight = has_flight;

    if (has_flight)
    {
        snprintf(update.callsign, sizeof(update.callsign), "%s", callsign);
        snprintf(update.from, sizeof(update.from), "%s", from);
        snprintf(update.to, sizeof(update.to), "%s", to);
    }

    // Queue length is one: keep the newest display state.
    if (xQueueOverwrite(display_queue, &update) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to publish display update");
        return;
    }

    if (has_flight)
    {
        ESP_LOGI(
            TAG,
            "Display update queued: %s | %s -> %s",
            update.callsign,
            update.from,
            update.to);
    }
    else
    {
        ESP_LOGI(TAG, "Display clear queued");
    }
}

// ============================================================
// Apply display update
// Called exclusively from LVGL task while holding the mutex.
// ============================================================

static void apply_display_update(const FlightDisplayUpdate &update)
{
    if (callsign_label == nullptr ||
        from_label == nullptr ||
        to_label == nullptr ||
        arrow_label == nullptr)
    {
        ESP_LOGE(TAG, "Display labels are not initialized");
        return;
    }

    // Keep the label objects visible at all times. Empty text produces
    // a completely black screen without relying on hidden-object flags.
    if (update.has_flight)
    {
        lv_label_set_text(callsign_label, update.callsign);
        lv_label_set_text(from_label, update.from);
        lv_label_set_text(to_label, update.to);
        lv_label_set_text(arrow_label, ">");

        ESP_LOGI(
            TAG,
            "LVGL display data applied: %s | %s -> %s",
            update.callsign,
            update.from,
            update.to);
    }
    else
    {
        lv_label_set_text(callsign_label, "");
        lv_label_set_text(from_label, "");
        lv_label_set_text(to_label, "");
        lv_label_set_text(arrow_label, "");

        ESP_LOGI(TAG, "Display cleared (empty flight array)");
    }

    // Mark the screen dirty and refresh it immediately.
    // This ensures the HUB75 flush callback runs after every flight update.
    lv_obj_invalidate(lv_screen_active());

    if (lvgl_display != nullptr)
    {
        lv_refr_now(lvgl_display);
    }
}

// ============================================================
// WiFi event handler
// ============================================================

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START)
    {
        ESP_LOGI(TAG, "WiFi started. Connecting...");

        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "WiFi connect failed: %s", esp_err_to_name(err));
        }
    }
    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);

        auto *event = static_cast<wifi_event_sta_disconnected_t *>(event_data);

        ESP_LOGW(
            TAG,
            "WiFi disconnected. Reason: %d. Reconnecting...",
            event->reason);

        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Reconnect failed: %s", esp_err_to_name(err));
        }
    }
    else if (event_base == IP_EVENT &&
             event_id == IP_EVENT_STA_GOT_IP)
    {
        auto *event = static_cast<ip_event_got_ip_t *>(event_data);

        ESP_LOGI(
            TAG,
            "WiFi connected! IP: " IPSTR,
            IP2STR(&event->ip_info.ip));

        ESP_LOGI(
            TAG,
            "Gateway: " IPSTR,
            IP2STR(&event->ip_info.gw));

        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// ============================================================
// WiFi initialization
// ============================================================

static void wifi_init_sta()
{
    ESP_LOGI(TAG, "Initializing WiFi...");

    wifi_event_group = xEventGroupCreate();
    if (wifi_event_group == nullptr)
    {
        ESP_LOGE(TAG, "Failed to create WiFi event group");
        abort();
    }

    // Initialize NVS.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize TCP/IP stack and default event loop.
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create WiFi station interface.
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (sta_netif == nullptr)
    {
        ESP_LOGE(TAG, "Failed to create WiFi station interface");
        abort();
    }

    // Initialize WiFi driver.
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handlers.
    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        wifi_event_handler,
        nullptr));

    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        wifi_event_handler,
        nullptr));

    // Configure credentials.
    wifi_config_t wifi_config{};

    const size_t ssid_len = strlen(WIFI_SSID);
    const size_t password_len = strlen(WIFI_PASSWORD);

    if (ssid_len == 0 ||
        ssid_len > sizeof(wifi_config.sta.ssid) ||
        password_len > sizeof(wifi_config.sta.password))
    {
        ESP_LOGE(TAG, "Invalid WiFi credentials");
        abort();
    }

    memcpy(wifi_config.sta.ssid, WIFI_SSID, ssid_len);
    memcpy(wifi_config.sta.password, WIFI_PASSWORD, password_len);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi initialization finished");
}

// ============================================================
// HUB75 flush callback
// ============================================================

static void display_flush_cb(
    lv_display_t *disp,
    const lv_area_t *area,
    uint8_t *px_map)
{
    auto *driver = static_cast<Hub75Driver *>(lv_display_get_user_data(disp));

    if (driver == nullptr)
    {
        lv_display_flush_ready(disp);
        return;
    }

    const int32_t width = area->x2 - area->x1 + 1;
    const int32_t height = area->y2 - area->y1 + 1;

    static uint32_t flush_log_count = 0;
    if (flush_log_count < 5)
    {
        ESP_LOGI(
            TAG,
            "LVGL flush #%lu: area=(%ld,%ld)-(%ld,%ld), size=%ldx%ld",
            static_cast<unsigned long>(flush_log_count + 1),
            static_cast<long>(area->x1),
            static_cast<long>(area->y1),
            static_cast<long>(area->x2),
            static_cast<long>(area->y2),
            static_cast<long>(width),
            static_cast<long>(height));
        ++flush_log_count;
    }

    driver->draw_pixels(
        area->x1,
        area->y1,
        width,
        height,
        px_map,
        Hub75PixelFormat::RGB565);

    lv_display_flush_ready(disp);
}

// ============================================================
// LVGL task
// All LVGL object updates happen here.
// ============================================================

static void lvgl_task(void *arg)
{
    (void)arg;

    FlightDisplayUpdate pending_update{};

    while (true)
    {
        // Apply the newest queued display update, if available.
        if (display_queue != nullptr &&
            xQueueReceive(display_queue, &pending_update, 0) == pdPASS)
        {
            ESP_LOGI(
                TAG,
                "LVGL task received display update (has_flight=%d)",
                pending_update.has_flight);

            lvgl_lock();
            apply_display_update(pending_update);
            lvgl_unlock();
        }

        // Process LVGL timers and rendering.
        lvgl_lock();
        uint32_t delay_ms = lv_timer_handler();
        lvgl_unlock();

        if (delay_ms < 5)
        {
            delay_ms = 5;
        }
        if (delay_ms > 20)
        {
            delay_ms = 20;
        }

        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

// ============================================================
// HTTP response structure
// ============================================================

struct HttpResponse
{
    char body[HTTP_BUFFER_SIZE];
    size_t length;
    bool overflow;
};

// ============================================================
// HTTP event handler
// ============================================================

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA &&
        evt->user_data != nullptr)
    {
        auto *response = static_cast<HttpResponse *>(evt->user_data);

        if (response->overflow)
        {
            return ESP_OK;
        }

        if (evt->data_len <= 0 || evt->data == nullptr)
        {
            return ESP_OK;
        }

        const size_t data_len = static_cast<size_t>(evt->data_len);

        if (response->length + data_len >= sizeof(response->body))
        {
            response->overflow = true;
            ESP_LOGE(TAG, "HTTP response too large");
            return ESP_OK;
        }

        memcpy(
            response->body + response->length,
            evt->data,
            data_len);

        response->length += data_len;
        response->body[response->length] = '\0';
    }

    return ESP_OK;
}

// ============================================================
// JSON helper
// ============================================================

static const char *get_json_string(const cJSON *object, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);

    if (!cJSON_IsString(item) || item->valuestring == nullptr)
    {
        return nullptr;
    }

    return item->valuestring;
}

// ============================================================
// Process JSON response
// ============================================================

static void process_flight_response(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (root == nullptr)
    {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return;
    }

    if (!cJSON_IsArray(root))
    {
        ESP_LOGE(TAG, "JSON response is not an array");
        cJSON_Delete(root);
        return;
    }

    const int flight_count = cJSON_GetArraySize(root);
    ESP_LOGI(TAG, "Number of flights received: %d", flight_count);

    // An empty array means there is no flight: clear the display.
    if (flight_count == 0)
    {
        ESP_LOGI(TAG, "No flights available");
        publish_display_update(false);
        cJSON_Delete(root);
        return;
    }

    // Always use the first flight in the array.
    cJSON *first_flight = cJSON_GetArrayItem(root, 0);
    if (first_flight == nullptr || !cJSON_IsObject(first_flight))
    {
        ESP_LOGE(TAG, "Invalid first flight entry");
        cJSON_Delete(root);
        return;
    }

    const char *callsign = get_json_string(first_flight, "Callsign");
    const char *from = get_json_string(first_flight, "From");
    const char *to = get_json_string(first_flight, "To");

    if (callsign == nullptr || from == nullptr || to == nullptr)
    {
        ESP_LOGE(TAG, "Missing flight information");
        cJSON_Delete(root);
        return;
    }

    ESP_LOGI(
        TAG,
        "Flight received: %s (%s -> %s)",
        callsign,
        from,
        to);

    publish_display_update(true, callsign, from, to);
    cJSON_Delete(root);
}

static void fetch_flight_data(){
    HttpResponse response{};

    esp_http_client_config_t config{};

    config.url = API_URL;
    config.method = HTTP_METHOD_GET;
    config.event_handler = http_event_handler;
    config.user_data = &response;
    config.timeout_ms = 8000;
    config.buffer_size = 1024;
    config.buffer_size_tx = 512;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr)
    {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return;
    }

    ESP_LOGI(TAG, "Requesting flight data...");

    const esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return;
    }

    const int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP status: %d", status_code);

    esp_http_client_cleanup(client);

    if (status_code != 200)
    {
        ESP_LOGE(TAG, "Unexpected HTTP status: %d", status_code);
        return;
    }

    if (response.overflow)
    {
        ESP_LOGE(TAG, "HTTP response exceeded buffer");
        return;
    }

    if (response.length == 0)
    {
        ESP_LOGW(TAG, "Empty HTTP response");
        return;
    }

    ESP_LOGI(TAG, "HTTP response: %s", response.body);
    process_flight_response(response.body);
}

// ============================================================
// Flight API task
// ============================================================

static void flight_api_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Flight API task started");

    while (true)
    {
        // Wait until WiFi has a valid IP address.
        xEventGroupWaitBits(
            wifi_event_group,
            WIFI_CONNECTED_BIT,
            pdFALSE,
            pdTRUE,
            portMAX_DELAY);

        // Poll on a 5-second schedule while WiFi remains connected.
        TickType_t last_wake_time = xTaskGetTickCount();

        do
        {
            fetch_flight_data();

            vTaskDelayUntil(
                &last_wake_time,
                pdMS_TO_TICKS(API_INTERVAL_MS));

        } while ((xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT) != 0);

        ESP_LOGW(TAG, "Waiting for WiFi reconnection");
    }
}

// ============================================================
// Application entry point
// ============================================================

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Starting HUB75 and LVGL");

    // --------------------------------------------------------
    // Create LVGL mutex
    // --------------------------------------------------------

    lvgl_mutex = xSemaphoreCreateRecursiveMutex();
    if (lvgl_mutex == nullptr)
    {
        ESP_LOGE(TAG, "Failed to create LVGL mutex");
        return;
    }

    // --------------------------------------------------------
    // Create display queue (length one, latest state wins)
    // --------------------------------------------------------

    display_queue = xQueueCreate(1, sizeof(FlightDisplayUpdate));
    if (display_queue == nullptr)
    {
        ESP_LOGE(TAG, "Failed to create display queue");
        return;
    }

    // --------------------------------------------------------
    // HUB75 configuration
    // EXACTLY AS IN YOUR WORKING DEMO
    // --------------------------------------------------------

    Hub75Config config{};

    config.panel_width = 64;
    config.panel_height = 32;
    config.layout_cols = 1;
    config.layout_rows = 1;

    config.shift_driver = Hub75ShiftDriver::FM6126A;

    config.pins.r1 = 4;
    config.pins.g1 = 5;
    config.pins.b1 = 6;

    config.pins.r2 = 7;
    config.pins.g2 = 15;
    config.pins.b2 = 16;

    config.pins.a = 8;
    config.pins.b = 9;
    config.pins.c = 10;
    config.pins.d = 11;
    config.pins.e = -1;

    config.pins.lat = 13;
    config.pins.oe = 14;
    config.pins.clk = 12;

    Hub75Driver driver(config);

    if (!driver.begin())
    {
        ESP_LOGE(TAG, "driver.begin() failed");
        while (true)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    ESP_LOGI(
        TAG,
        "HUB75 initialized: %dx%d",
        driver.get_width(),
        driver.get_height());

    // --------------------------------------------------------
    // LVGL initialization
    // --------------------------------------------------------

    lv_init();

    // Provide LVGL with a real elapsed-time source. Without this, the
    // display refresh timer may never become due after the initial render.
    lv_tick_set_cb(lvgl_tick_get_cb);

    lvgl_display = lv_display_create(
        driver.get_width(),
        driver.get_height());

    lv_display_t *disp = lvgl_display;

    if (disp == nullptr)
    {
        ESP_LOGE(TAG, "Failed to create LVGL display");
        return;
    }

    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_user_data(disp, &driver);
    lv_display_set_flush_cb(disp, display_flush_cb);

    lv_draw_buf_t *draw_buf = lv_draw_buf_create(
        driver.get_width(),
        driver.get_height(),
        LV_COLOR_FORMAT_RGB565,
        0);

    if (draw_buf == nullptr)
    {
        ESP_LOGE(TAG, "Failed to create LVGL draw buffer");
        return;
    }

    lv_display_set_draw_buffers(disp, draw_buf, nullptr);
    lv_display_set_render_mode(disp, LV_DISPLAY_RENDER_MODE_PARTIAL);

    // --------------------------------------------------------
    // Screen background
    // --------------------------------------------------------

    lv_obj_t *screen = lv_screen_active();

    lv_obj_set_style_bg_color(
        screen,
        lv_color_hex(0x000000),
        0);

    lv_obj_set_style_bg_opa(
        screen,
        LV_OPA_COVER,
        0);

    // --------------------------------------------------------
    // Callsign label (empty initially; blank screen until a flight arrives)
    // --------------------------------------------------------

    callsign_label = lv_label_create(screen);
    lv_label_set_text(callsign_label, "");

    lv_obj_set_style_text_color(
        callsign_label,
        lv_color_hex(0xFFFFFF),
        0);

    lv_obj_set_style_text_font(
        callsign_label,
        &lv_font_montserrat_12,
        0);

    lv_obj_align(callsign_label, LV_ALIGN_TOP_MID, 0, 0);

    // --------------------------------------------------------
    // From label (empty initially)
    // --------------------------------------------------------

    from_label = lv_label_create(screen);
    lv_label_set_text(from_label, "");

    lv_obj_set_style_text_color(
        from_label,
        lv_color_hex(0xFFFFFF),
        0);

    lv_obj_set_style_text_font(
        from_label,
        &lv_font_montserrat_12,
        0);

    lv_obj_align(from_label, LV_ALIGN_BOTTOM_LEFT, 1, 0);

    // --------------------------------------------------------
    // To label (empty initially)
    // --------------------------------------------------------

    to_label = lv_label_create(screen);
    lv_label_set_text(to_label, "");

    lv_obj_set_style_text_color(
        to_label,
        lv_color_hex(0xFFFFFF),
        0);

    lv_obj_set_style_text_font(
        to_label,
        &lv_font_montserrat_12,
        0);

    lv_obj_align(to_label, LV_ALIGN_BOTTOM_RIGHT, -1, 0);

    // --------------------------------------------------------
    // Direction arrow (empty initially)
    // --------------------------------------------------------

    arrow_label = lv_label_create(screen);
    lv_label_set_text(arrow_label, "");

    lv_obj_set_style_text_color(
        arrow_label,
        lv_color_hex(0xFFFFFF),
        0);

    lv_obj_set_style_text_font(
        arrow_label,
        &lv_font_montserrat_12,
        0);

    lv_obj_align(arrow_label, LV_ALIGN_BOTTOM_MID, 0, 0);

    // --------------------------------------------------------
    // Initial rendering
    // --------------------------------------------------------

    lvgl_lock();
    lv_refr_now(disp);
    lvgl_unlock();

    ESP_LOGI(TAG, "LVGL initialized");

    // --------------------------------------------------------
    // Start LVGL task
    // --------------------------------------------------------

    BaseType_t result = xTaskCreate(
        lvgl_task,
        "lvgl_task",
        4096,
        nullptr,
        5,
        nullptr);

    if (result != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create LVGL task");
        return;
    }

    // --------------------------------------------------------
    // Initialize WiFi
    // --------------------------------------------------------

    wifi_init_sta();

    // --------------------------------------------------------
    // Start Flight API task
    // --------------------------------------------------------

    result = xTaskCreate(
        flight_api_task,
        "flight_api_task",
        6144,
        nullptr,
        4,
        nullptr);

    if (result != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create flight API task");
        return;
    }

    ESP_LOGI(TAG, "Display, WiFi and Flight API initialized");

    // --------------------------------------------------------
    // Keep app_main alive
    // --------------------------------------------------------

    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

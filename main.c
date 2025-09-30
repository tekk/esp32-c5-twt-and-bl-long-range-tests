// main/main.c
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "esp_wifi.h"
#include "esp_wifi_he.h"            // Wi-Fi 6 (HE) / TWT API

#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"

#include "esp_http_client.h"

static const char *TAG = "C5_TWT_BLELR";

/* ====== CONFIG (adjust for your AP) ====== */
#define WIFI_SSID       "lab-ap-wifi6"
#define WIFI_PASS       "SuperSecret123"
#define WIFI_MAX_RETRY  5

/* BLE extended advertising instance */
#define ADV_INSTANCE    1
/* Example 16-bit Service UUID in advertising data */
#define DEMO_SERVICE_UUID 0x181A /* Environmental Sensing */

static int s_retry_num = 0;

/* ====== Wi-Fi event handling (incl. iTWT lifecycle) ====== */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            if (s_retry_num < WIFI_MAX_RETRY) {
                s_retry_num++;
                ESP_LOGW(TAG, "Wi-Fi disconnected, retry %d/%d", s_retry_num, WIFI_MAX_RETRY);
                esp_wifi_connect();
            } else {
                ESP_LOGE(TAG, "Failed to connect to AP");
            }
            break;

        case WIFI_EVENT_ITWT_SETUP:
            ESP_LOGI(TAG, "iTWT SETUP confirmed by AP");
            break;

        case WIFI_EVENT_ITWT_TEARDOWN:
            ESP_LOGW(TAG, "iTWT TEARDOWN");
            break;

        case WIFI_EVENT_ITWT_SUSPEND:
            ESP_LOGI(TAG, "iTWT SUSPEND");
            break;

        case WIFI_EVENT_ITWT_RESUME:
            ESP_LOGI(TAG, "iTWT RESUME");
            break;

        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
    }
}

/* ====== Wi-Fi init + iTWT (individual TWT) configuration ====== */
static esp_err_t wifi_init_with_twt(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    (void)sta;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_cfg = { 0 };
    strncpy((char *)wifi_cfg.sta.ssid, WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strncpy((char *)wifi_cfg.sta.password, WIFI_PASS, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_WPA3_PSK;
    wifi_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));

    // Enable Wi-Fi modem power save (light sleep managed by driver)
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Wi-Fi started, connecting…");

    // Wait a moment so association/DHCP can finish before negotiating TWT.
    vTaskDelay(pdMS_TO_TICKS(3000));

    // iTWT config: ~10 s service period, ~200 ms wake duration.
    // AP must support Wi-Fi 6 TWT for this to succeed.
    wifi_twt_config_t twt_cfg = {
        .setup_cmd     = TWT_SETUP_CMD_REQUEST,
        .flow_id       = 0,
        .responder     = false,      // STA is initiator
        .trigger       = false,      // non-triggered TWT
        .implicit      = true,       // implicit schedule
        .announce      = false,
        .protect       = false,
        .twt           = 0,          // using mantissa/exponent
        .mantissa      = 10240,      // ≈10.49 s (10240 TU, 1 TU = 1024 µs)
        .exponent      = 0,
        .min_wake_dur  = 195,        // ≈200 TU wake (~200 ms)
        .req_type      = TWT_REQUEST_TYPE_ITWT,
        .negotiation_type = TWT_NEGO_TYPE_BOTH,
        .channel       = 0,
        .twt_info_frame_disabled = false,
    };

    esp_err_t err = esp_wifi_sta_twt_config(&twt_cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "TWT config sent; awaiting WIFI_EVENT_ITWT_SETUP");
    } else {
        ESP_LOGE(TAG, "TWT config failed: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

/* ====== BLE Long Range (LE Coded PHY, S=8 preference) – Extended Advertising ====== */

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_EXT_ADV_SET_PARAMS_COMPLETE_EVT: {
        ESP_LOGI(TAG, "EXT_ADV params set (LE Coded primary/secondary)");
        // Minimal ADV data: Flags + Complete List of 16-bit UUIDs (1 UUID)
        uint8_t adv_data[8];
        int idx = 0;
        // Flags (LE General Discoverable, BR/EDR Not Supported)
        adv_data[idx++] = 0x02; // len
        adv_data[idx++] = 0x01; // Flags
        adv_data[idx++] = 0x06;

        // Complete list of 16-bit Service UUIDs
        adv_data[idx++] = 0x03; // len
        adv_data[idx++] = 0x03; // Complete 16-bit UUIDs
        adv_data[idx++] = (uint8_t)(DEMO_SERVICE_UUID & 0xFF);
        adv_data[idx++] = (uint8_t)(DEMO_SERVICE_UUID >> 8);

        esp_ble_gap_ext_adv_set_data(ADV_INSTANCE, idx, adv_data);
        break;
    }

    case ESP_GAP_BLE_EXT_ADV_DATA_SET_COMPLETE_EVT: {
        ESP_LOGI(TAG, "EXT_ADV data set");
        esp_ble_gap_ext_adv_t start = {
            .instance = ADV_INSTANCE,
            .duration = 0,      // continuous
            .max_events = 0
        };
        esp_ble_gap_ext_adv_start(1, &start);
        break;
    }

    case ESP_GAP_BLE_EXT_ADV_START_COMPLETE_EVT:
        ESP_LOGI(TAG, "EXT_ADV started on LE Coded PHY (S=8 preferred at peer negotiation)");
        break;

    default:
        break;
    }
}

static esp_err_t ble_init_coded_phy_adv(void)
{
    // BLE only; free Classic BT memory
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));

    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_cb));

    // Configure Extended Advertising to use LE Coded PHY for both primary and secondary.
    esp_ble_gap_ext_adv_params_t params = {
        .type = ESP_BLE_GAP_SET_EXT_ADV_PROP_NONCONN_NONSCANNABLE_UNDIRECTED, // beacon-like
        .interval_min = 0x00A0, // ~100 ms
        .interval_max = 0x00A0,
        .channel_map = 0x07,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
        .tx_power = EXT_ADV_TX_PWR_NO_PREFERENCE,
        .primary_phy = ESP_BLE_GAP_PRI_PHY_CODED,
        .max_skip = 0,
        .secondary_phy = ESP_BLE_GAP_PHY_CODED,
        .sid = 1,
        .scan_req_notif = 0
    };

    ESP_ERROR_CHECK(esp_ble_gap_ext_adv_set_params(ADV_INSTANCE, &params));
    return ESP_OK;
}

/* ====== Demo periodic net task ======
   Illustrative network activity roughly aligned with the ~10 s TWT period.
   In a real sensor you’d schedule publish exactly inside the awake window. */
static void periodic_net_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));

        esp_http_client_config_t http_cfg = {
            .url = "http://example.com/",
            .method = HTTP_METHOD_HEAD,
            .timeout_ms = 2000,
        };
        esp_http_client_handle_t h = esp_http_client_init(&http_cfg);
        if (h && esp_http_client_perform(h) == ESP_OK) {
            ESP_LOGI(TAG, "HTTP HEAD ok, status=%d", esp_http_client_get_status_code(h));
        } else {
            ESP_LOGW(TAG, "HTTP HEAD failed");
        }
        if (h) esp_http_client_cleanup(h);
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(wifi_init_with_twt());
    ESP_ERROR_CHECK(ble_init_coded_phy_adv());

    xTaskCreate(periodic_net_task, "net_task", 4096, NULL, 5, NULL);
}

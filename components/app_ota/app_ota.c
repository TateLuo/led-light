#include "app_ota.h"

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_battery_test.h"
#include "app_light.h"
#include "app_state.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "APP_OTA";

#define APP_OTA_AP_PASSWORD              "12345678"
#define APP_OTA_AP_CHANNEL               6
#define APP_OTA_AP_MAX_CONNECTIONS       1
#define APP_OTA_HTTP_PORT                80
#define APP_OTA_RECV_BUFFER_SIZE         1024
#define APP_OTA_RESTART_DELAY_MS         1500
#define APP_OTA_MIN_START_BATTERY_V      7.0f
#define APP_OTA_SSID_PREFIX              "LED-Light-OTA"
#define APP_OTA_DEFAULT_IP               "192.168.4.1"
#define APP_OTA_HTTPD_STACK_SIZE         8192

static SemaphoreHandle_t ota_mutex;
static httpd_handle_t ota_http_server;
static esp_netif_t *ota_ap_netif;
static bool initialized;
static bool netif_initialized;
static bool event_loop_ready;
static bool wifi_initialized;
static bool ota_active_flag;
static bool upload_in_progress;
static char ota_ssid[APP_STATE_OTA_TEXT_MAX_LEN];
static char ota_ip[APP_STATE_OTA_TEXT_MAX_LEN] = APP_OTA_DEFAULT_IP;

static esp_err_t lock_ota(void)
{
    if (!initialized || ota_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return xSemaphoreTake(ota_mutex, portMAX_DELAY) == pdTRUE ?
           ESP_OK : ESP_ERR_TIMEOUT;
}

static void unlock_ota(void)
{
    xSemaphoreGive(ota_mutex);
}

static void publish_ota_state(app_ota_status_t status, bool active,
                              bool uploading, uint8_t progress,
                              const char *error)
{
    app_state_ota_update_t update = {
        .active = active,
        .uploading = uploading,
        .progress_percent = progress,
        .status = status,
        .ssid = ota_ssid,
        .password = APP_OTA_AP_PASSWORD,
        .ip = ota_ip,
        .error = error,
    };
    esp_err_t err = app_state_update_ota(&update);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to publish OTA state: %s", esp_err_to_name(err));
    }
}

static void make_ota_ssid(void)
{
    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    if (err == ESP_OK) {
        (void)snprintf(ota_ssid, sizeof(ota_ssid), "%s-%02X%02X",
                       APP_OTA_SSID_PREFIX, mac[4], mac[5]);
    } else {
        (void)snprintf(ota_ssid, sizeof(ota_ssid), "%s", APP_OTA_SSID_PREFIX);
    }
}

static esp_err_t prepare_wifi_stack(void)
{
    if (!netif_initialized) {
        esp_err_t err = esp_netif_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return err;
        }
        netif_initialized = true;
    }

    if (!event_loop_ready) {
        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return err;
        }
        event_loop_ready = true;
    }

    if (ota_ap_netif == NULL) {
        ota_ap_netif = esp_netif_create_default_wifi_ap();
        if (ota_ap_netif == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (!wifi_initialized) {
        wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
        esp_err_t err = esp_wifi_init(&config);
        if (err != ESP_OK) {
            return err;
        }
        wifi_initialized = true;
    }

    return ESP_OK;
}

static esp_err_t start_wifi_ap(void)
{
    make_ota_ssid();

    wifi_config_t wifi_config = {0};
    (void)snprintf((char *)wifi_config.ap.ssid,
                   sizeof(wifi_config.ap.ssid), "%s", ota_ssid);
    (void)snprintf((char *)wifi_config.ap.password,
                   sizeof(wifi_config.ap.password), "%s",
                   APP_OTA_AP_PASSWORD);
    wifi_config.ap.ssid_len = strlen(ota_ssid);
    wifi_config.ap.channel = APP_OTA_AP_CHANNEL;
    wifi_config.ap.max_connection = APP_OTA_AP_MAX_CONNECTIONS;
    wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    wifi_config.ap.pmf_cfg.required = false;

    esp_err_t err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (err != ESP_OK) {
        return err;
    }
    return esp_wifi_start();
}

static const char ota_index_html[] =
    "<!doctype html><html lang='en'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>LED Light Service</title><style>"
    "*{box-sizing:border-box}body{margin:0;min-height:100vh;padding:20px;"
    "font-family:system-ui,-apple-system,Segoe UI,sans-serif;"
    "background:#0f172a;color:#0f172a;display:grid;place-items:center}"
    ".card{width:min(100%,500px);background:#f8fafc;border:1px solid #dbe4ee;"
    "border-radius:14px;box-shadow:0 24px 80px rgba(2,6,23,.28);padding:24px}"
    "h1{margin:0;font-size:25px;line-height:1.1}.sub{margin:8px 0 18px;"
    "color:#526173;font-size:14px;line-height:1.45}.panel{border-top:1px solid #dbe4ee;"
    "padding-top:18px;margin-top:18px}h2{font-size:17px;margin:0 0 10px}"
    ".drop{display:block;border:1px dashed #8aa0b8;border-radius:10px;"
    "background:#eef4fb;padding:18px;text-align:center;cursor:pointer}"
    ".drop strong{display:block;font-size:15px}.drop span{display:block;margin-top:6px;"
    "color:#64748b;font-size:13px}.drop input{display:none}.actions{display:flex;gap:10px;"
    "margin-top:14px}.btn{width:100%;border:0;border-radius:9px;padding:12px 14px;"
    "font-weight:700;font-size:14px;color:white;background:#2563eb;cursor:pointer}"
    ".btn.secondary{background:#334155}.btn.danger{background:#b91c1c}"
    ".btn:disabled{background:#94a3b8;cursor:not-allowed}"
    "select{width:100%;padding:11px;border:1px solid #b8c6d5;border-radius:9px;"
    "background:white;color:#0f172a;font-size:14px}"
    ".bar{height:10px;margin-top:14px;background:#d9e3ef;border-radius:999px;overflow:hidden}"
    ".fill{height:100%;width:0%;background:linear-gradient(90deg,#2563eb,#14b8a6);"
    "transition:width .16s ease}.status{min-height:58px;margin:14px 0 0;padding:13px;"
    "border-radius:10px;background:#e8eef6;color:#334155;white-space:pre-wrap;"
    "font-size:13px;line-height:1.45}.foot{margin-top:14px;color:#64748b;font-size:12px;line-height:1.4}"
    "@media(max-width:420px){body{padding:12px}.card{border-radius:12px;padding:18px}"
    "h1{font-size:23px}.actions{display:block}.btn{margin-top:10px}}"
    "</style></head><body><main class='card'><h1>25W LED Light Service</h1>"
    "<p class='sub'>Download battery-test data and update firmware.</p>"
    "<section class='panel'><h2>Battery SOC Test</h2>"
    "<div class='actions'><button class='btn' id='testDownload'>Download CSV</button></div>"
    "<p class='foot'>Start or stop Battery Test on the device Settings screen. A completed CSV download clears the recorded data automatically.</p>"
    "</section>"
    "<section class='panel'><h2>Firmware OTA</h2>"
    "<label class='drop' id='fwDrop'><input id='fwFile' type='file' accept='.bin'>"
    "<strong id='fwName'>Choose firmware</strong><span>Tap to select, or drag a .bin here on PC</span>"
    "</label><div class='actions'><button class='btn' id='fwUpload'>Upload firmware</button></div>"
    "<div class='bar'><div class='fill' id='fwFill'></div></div>"
    "<pre class='status' id='fwStatus'>Idle</pre></section>"
    "<p class='foot'>Firmware upload is locked while battery-test data exists. Download CSV first. Firmware upload reboots automatically.</p>"
    "</main><script>"
    "const $=id=>document.getElementById(id);"
    "const fwFile=$('fwFile'),fwName=$('fwName'),fwDrop=$('fwDrop'),fwBtn=$('fwUpload'),"
    "fwFill=$('fwFill'),fwStatus=$('fwStatus');"
    "$('testDownload').onclick=()=>location.href='/battery-test/data.csv';"
    "function bindDrop(el,cb){el.ondragover=e=>{e.preventDefault();el.style.borderColor='#2563eb'};"
    "el.ondragleave=()=>el.style.borderColor='#8aa0b8';el.ondrop=e=>{e.preventDefault();"
    "el.style.borderColor='#8aa0b8';cb(e.dataTransfer.files[0])}}"
    "function fwSet(f){if(!f)return;if(!/\\.bin$/i.test(f.name)){fwStatus.textContent='Please choose a .bin firmware file.';return}"
    "fwFile._selected=f;fwName.textContent=f.name;fwFill.style.width='0%';"
    "fwStatus.textContent='Ready: '+f.name+'\\nSize: '+Math.ceil(f.size/1024)+' KB'}"
    "fwFile.onchange=()=>fwSet(fwFile.files[0]);bindDrop(fwDrop,fwSet);"
    "fwBtn.onclick=()=>{let f=fwFile._selected||fwFile.files[0];if(!f){fwStatus.textContent='Please choose a .bin file first.';return}"
    "fwBtn.disabled=true;fwFill.style.width='0%';fwStatus.textContent='Uploading '+f.name+' ...';"
    "let x=new XMLHttpRequest();x.open('POST','/update');x.upload.onprogress=e=>{if(e.lengthComputable){"
    "let p=Math.round(e.loaded*100/e.total);fwFill.style.width=p+'%';fwStatus.textContent='Uploading '+p+'%'}};"
    "x.onload=()=>{fwFill.style.width='100%';fwStatus.textContent=x.status===200?'Upload complete. Rebooting...':"
    "'Upload failed: '+x.responseText;fwBtn.disabled=false};x.onerror=()=>{fwStatus.textContent='Connection lost. "
    "The device may be rebooting or power may be unstable.';fwBtn.disabled=false};x.send(f)};"
    "</script></body></html>";

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, ota_index_html, HTTPD_RESP_USE_STRLEN);
}

static const char *fault_name(system_fault_t fault)
{
    switch (fault) {
    case SYSTEM_FAULT_LOW_BATTERY:
        return "low_battery";
    case SYSTEM_FAULT_CRITICAL_BATTERY:
        return "critical_battery";
    case SYSTEM_FAULT_OVER_TEMP:
        return "over_temp";
    case SYSTEM_FAULT_NTC_ERROR:
        return "ntc_error";
    case SYSTEM_FAULT_ADC_ERROR:
        return "adc_error";
    case SYSTEM_FAULT_NONE:
    default:
        return "none";
    }
}

static esp_err_t battery_test_csv_handler(httpd_req_t *req)
{
    app_battery_test_status_t status;
    if (app_battery_test_get_status(&status) != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "Recorder unavailable");
    }
    if (status.run_state == APP_BATTERY_TEST_RUNNING
        || status.run_state == APP_BATTERY_TEST_PAUSED) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req,
                                  "Stop or complete Battery Test first");
    }
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=battery-test.csv");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send_chunk(
        req,
        "sequence,elapsed_ms,phase,battery_adc_mv,battery_voltage_v,"
        "battery_percent,battery_valid,ntc_adc_mv,ntc_temp_c,ntc_valid,"
        "light_enabled,brightness_percent,cct_kelvin,fan_percent,"
        "cold_duty,warm_duty,fault\r\n",
        HTTPD_RESP_USE_STRLEN);
    char line[256];
    for (size_t index = 0; err == ESP_OK && index < status.sample_count;
         ++index) {
        app_battery_test_sample_t sample;
        if (app_battery_test_get_sample(index, &sample) != ESP_OK) {
            break;
        }
        const int length = snprintf(
            line, sizeof(line),
            "%" PRIu32 ",%" PRIu32 ",%s,%" PRIu32 ",%.3f,%u,%u,"
            "%" PRIu32 ",%.1f,%u,%u,%u,%u,%u,%u,%u,%s\r\n",
            sample.sequence, sample.elapsed_ms, sample.phase,
            sample.battery_adc_mv,
            sample.battery_valid ? sample.battery_voltage_v : 0.0f,
            (unsigned int)sample.battery_percent,
            sample.battery_valid ? 1U : 0U, sample.ntc_adc_mv,
            sample.ntc_valid ? sample.ntc_temp_c : 0.0f,
            sample.ntc_valid ? 1U : 0U, sample.light_enabled ? 1U : 0U,
            (unsigned int)sample.brightness_percent,
            (unsigned int)sample.cct_kelvin,
            (unsigned int)sample.fan_percent,
            (unsigned int)sample.cold_duty,
            (unsigned int)sample.warm_duty, fault_name(sample.fault));
        if (length <= 0 || length >= (int)sizeof(line)) {
            err = ESP_FAIL;
            break;
        }
        err = httpd_resp_send_chunk(req, line, length);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    if (err == ESP_OK) {
        const esp_err_t clear_err = app_battery_test_clear();
        if (clear_err != ESP_OK) {
            ESP_LOGW(TAG, "CSV sent but recorder clear failed: %s",
                     esp_err_to_name(clear_err));
        } else {
            ESP_LOGI(TAG, "Battery-test CSV sent; recorded data cleared");
        }
    }
    return err;
}

static void restart_task(void *context)
{
    (void)context;
    vTaskDelay(pdMS_TO_TICKS(APP_OTA_RESTART_DELAY_MS));
    esp_restart();
}

static esp_err_t send_upload_error(httpd_req_t *req, const char *status,
                                   const char *message)
{
    publish_ota_state(APP_OTA_STATUS_FAILED, ota_active_flag, false, 0,
                      message);
    httpd_resp_set_status(req, status);
    return httpd_resp_sendstr(req, message);
}

static esp_err_t update_handler(httpd_req_t *req)
{
    if (req->content_len <= 0) {
        return send_upload_error(req, "400 Bad Request", "Empty upload");
    }

    app_battery_test_status_t test_status;
    esp_err_t err = app_battery_test_get_status(&test_status);
    if (err != ESP_OK) {
        return send_upload_error(req, "503 Service Unavailable",
                                 "Battery-test state unavailable");
    }
    if (test_status.sample_count > 0
        || test_status.run_state != APP_BATTERY_TEST_STOPPED) {
        return send_upload_error(req, "409 Conflict",
                                 "Download Battery Test CSV before OTA");
    }

    err = lock_ota();
    if (err != ESP_OK) {
        return send_upload_error(req, "503 Service Unavailable", "OTA busy");
    }
    if (upload_in_progress) {
        unlock_ota();
        return send_upload_error(req, "409 Conflict", "OTA already running");
    }
    upload_in_progress = true;
    unlock_ota();

    ESP_LOGI(TAG, "OTA upload started, length=%d", req->content_len);
    publish_ota_state(APP_OTA_STATUS_UPLOADING, true, true, 0, NULL);

    const esp_partition_t *update_partition =
        esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        (void)lock_ota();
        upload_in_progress = false;
        unlock_ota();
        return send_upload_error(req, "500 Internal Server Error",
                                 "No OTA partition");
    }

    esp_ota_handle_t ota_handle = 0;
    err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        (void)lock_ota();
        upload_in_progress = false;
        unlock_ota();
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        return send_upload_error(req, "500 Internal Server Error",
                                 "OTA begin failed");
    }

    char buffer[APP_OTA_RECV_BUFFER_SIZE];
    int remaining = req->content_len;
    int written = 0;
    uint8_t last_progress = UINT8_MAX;

    while (remaining > 0) {
        const size_t to_read = remaining > APP_OTA_RECV_BUFFER_SIZE ?
                               APP_OTA_RECV_BUFFER_SIZE :
                               (size_t)remaining;
        int received = httpd_req_recv(req, buffer, to_read);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (received <= 0) {
            ESP_LOGE(TAG, "Upload receive failed: %d", received);
            (void)esp_ota_abort(ota_handle);
            (void)lock_ota();
            upload_in_progress = false;
            unlock_ota();
            return send_upload_error(req, "500 Internal Server Error",
                                     "Upload receive failed");
        }

        err = esp_ota_write(ota_handle, buffer, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            (void)esp_ota_abort(ota_handle);
            (void)lock_ota();
            upload_in_progress = false;
            unlock_ota();
            return send_upload_error(req, "500 Internal Server Error",
                                     "OTA write failed");
        }

        remaining -= received;
        written += received;
        const uint8_t progress =
            (uint8_t)(((uint32_t)written * 100U) / (uint32_t)req->content_len);
        if (progress != last_progress) {
            publish_ota_state(APP_OTA_STATUS_UPLOADING, true, true,
                              progress, NULL);
            last_progress = progress;
        }
    }

    publish_ota_state(APP_OTA_STATUS_VERIFYING, true, true, 100, NULL);
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        (void)lock_ota();
        upload_in_progress = false;
        unlock_ota();
        return send_upload_error(req, "500 Internal Server Error",
                                 "OTA verify failed");
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s",
                 esp_err_to_name(err));
        (void)lock_ota();
        upload_in_progress = false;
        unlock_ota();
        return send_upload_error(req, "500 Internal Server Error",
                                 "Set boot partition failed");
    }

    publish_ota_state(APP_OTA_STATUS_SUCCESS, true, false, 100, NULL);
    (void)lock_ota();
    upload_in_progress = false;
    unlock_ota();

    ESP_LOGI(TAG, "OTA upload complete, rebooting");
    BaseType_t task_created = xTaskCreate(restart_task, "ota_restart", 2048,
                                          NULL, 3, NULL);
    if (task_created != pdPASS) {
        esp_restart();
    }

    return httpd_resp_sendstr(req, "OK: rebooting");
}

static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = APP_OTA_HTTP_PORT;
    config.stack_size = APP_OTA_HTTPD_STACK_SIZE;

    esp_err_t err = httpd_start(&ota_http_server, &config);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t update_uri = {
        .uri = "/update",
        .method = HTTP_POST,
        .handler = update_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t battery_csv_uri = {
        .uri = "/battery-test/data.csv",
        .method = HTTP_GET,
        .handler = battery_test_csv_handler,
    };
    err = httpd_register_uri_handler(ota_http_server, &index_uri);
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(ota_http_server, &update_uri);
    }
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(ota_http_server, &battery_csv_uri);
    }
    if (err != ESP_OK) {
        httpd_stop(ota_http_server);
        ota_http_server = NULL;
    }
    return err;
}

esp_err_t app_ota_init(void)
{
    if (initialized) {
        return ESP_OK;
    }

    ota_mutex = xSemaphoreCreateMutex();
    if (ota_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    make_ota_ssid();
    initialized = true;
    publish_ota_state(APP_OTA_STATUS_IDLE, false, false, 0, NULL);
    return ESP_OK;
}

esp_err_t app_ota_confirm_running_firmware(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_ota_img_states_t state;
    esp_err_t err = esp_ota_get_state_partition(running, &state);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        return ESP_OK;
    }

    err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Confirmed running OTA image after successful startup");
    }
    return err;
}

esp_err_t app_ota_start(void)
{
    app_battery_test_status_t test_status;
    esp_err_t test_err = app_battery_test_get_status(&test_status);
    if (test_err == ESP_OK
        && (test_status.run_state == APP_BATTERY_TEST_RUNNING
            || test_status.run_state == APP_BATTERY_TEST_PAUSED)) {
        ESP_LOGW(TAG, "OTA start rejected while battery test is active");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = lock_ota();
    if (err != ESP_OK) {
        return err;
    }
    if (ota_active_flag) {
        unlock_ota();
        return ESP_OK;
    }
    ota_active_flag = true;
    unlock_ota();

    app_state_t state;
    err = app_state_get(&state);
    if (err == ESP_OK && isfinite(state.battery_voltage_v)
        && state.battery_voltage_v < APP_OTA_MIN_START_BATTERY_V) {
        publish_ota_state(APP_OTA_STATUS_FAILED, false, false, 0, "BAT LOW");
        (void)lock_ota();
        ota_active_flag = false;
        unlock_ota();
        return ESP_ERR_INVALID_STATE;
    }

    (void)app_state_set_light_enabled(false);
    (void)app_light_off();
    publish_ota_state(APP_OTA_STATUS_STARTING, true, false, 0, NULL);

    err = prepare_wifi_stack();
    if (err == ESP_OK) {
        err = start_wifi_ap();
    }
    if (err == ESP_OK) {
        err = start_http_server();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start OTA service: %s", esp_err_to_name(err));
        if (ota_http_server != NULL) {
            httpd_stop(ota_http_server);
            ota_http_server = NULL;
        }
        if (wifi_initialized) {
            (void)esp_wifi_stop();
        }
        (void)lock_ota();
        ota_active_flag = false;
        upload_in_progress = false;
        unlock_ota();
        publish_ota_state(APP_OTA_STATUS_FAILED, false, false, 0,
                          "START FAILED");
        return err;
    }

    ESP_LOGI(TAG, "OTA SoftAP ready: ssid=%s password=%s ip=%s", ota_ssid,
             APP_OTA_AP_PASSWORD, ota_ip);
    publish_ota_state(APP_OTA_STATUS_READY, true, false, 0, NULL);
    return ESP_OK;
}

esp_err_t app_ota_stop(void)
{
    esp_err_t err = lock_ota();
    if (err != ESP_OK) {
        return err;
    }
    if (!ota_active_flag) {
        unlock_ota();
        return ESP_OK;
    }
    if (upload_in_progress) {
        unlock_ota();
        return ESP_ERR_INVALID_STATE;
    }
    ota_active_flag = false;
    unlock_ota();

    if (ota_http_server != NULL) {
        httpd_stop(ota_http_server);
        ota_http_server = NULL;
    }
    if (wifi_initialized) {
        err = esp_wifi_stop();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Stopping WiFi failed: %s", esp_err_to_name(err));
        }
    }

    publish_ota_state(APP_OTA_STATUS_IDLE, false, false, 0, NULL);
    return ESP_OK;
}

bool app_ota_active(void)
{
    if (lock_ota() != ESP_OK) {
        return false;
    }
    const bool active = ota_active_flag;
    unlock_ota();
    return active;
}

bool app_ota_uploading(void)
{
    if (lock_ota() != ESP_OK) {
        return false;
    }
    const bool uploading = upload_in_progress;
    unlock_ota();
    return uploading;
}

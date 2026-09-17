#include "wifi_config_portal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#define WIFI_FORM_BODY_MAX_LENGTH  320U
#define WIFI_FORM_SSID_MAX_LENGTH  32U
#define WIFI_FORM_PASSWORD_MAX_LENGTH 64U

static const char *TAG = "WIFI_PORTAL";
static httpd_handle_t s_server;
static wifi_config_portal_submit_cb_t s_submit_cb;
static void *s_submit_context;
static wifi_config_portal_status_t s_status = WIFI_CONFIG_PORTAL_READY;
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;

static const char s_setup_page[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>Wi-Fi Setup</title><style>"
    "body{font-family:Arial,sans-serif;margin:0;background:#f4f6f8;color:#17202a}"
    "main{max-width:420px;margin:40px auto;padding:24px}"
    "h1{font-size:24px;margin:0 0 24px}label{display:block;margin:16px 0 6px}"
    "input{box-sizing:border-box;width:100%;padding:12px;border:1px solid #aab2bd;border-radius:6px;font-size:16px}"
    "button{width:100%;margin-top:22px;padding:13px;border:0;border-radius:6px;background:#1769aa;color:white;font-size:16px}"
    "#status{min-height:24px;margin-top:16px;font-size:14px}</style></head>"
    "<body><main><h1>Wi-Fi Setup</h1><form id=f>"
    "<label for=s>SSID</label><input id=s name=ssid maxlength=32 required>"
    "<label for=p>Password</label><input id=p name=password type=password maxlength=64>"
    "<button type=submit>Save &amp; Connect</button></form><div id=status></div>"
    "<script>const f=document.getElementById('f'),o=document.getElementById('status');"
    "f.onsubmit=async e=>{e.preventDefault();o.textContent='Connecting...';"
    "let r=await fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(new FormData(f))});"
    "o.textContent=await r.text()};setInterval(async()=>{try{let r=await fetch('/status');let j=await r.json();"
    "if(j.status==='failed')o.textContent='Connection failed. Check SSID/password and try again.';"
    "else if(j.status==='storage_failed')o.textContent='Connected, but saving settings failed. Please try again.';"
    "else if(j.status==='connecting')o.textContent='Connecting...'}catch(e){}},1000);</script>"
    "</main></body></html>";

static int hex_value(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    character = (char)tolower((unsigned char)character);
    return character >= 'a' && character <= 'f' ? character - 'a' + 10 : -1;
}

static bool url_decode(const char *source, size_t source_length,
                       char *destination, size_t destination_size)
{
    size_t output = 0U;
    for (size_t index = 0U; index < source_length; index++) {
        if (output + 1U >= destination_size) {
            return false;
        }
        if (source[index] == '+') {
            destination[output++] = ' ';
        } else if (source[index] == '%' && index + 2U < source_length) {
            int high = hex_value(source[index + 1U]);
            int low = hex_value(source[index + 2U]);
            if (high < 0 || low < 0) {
                return false;
            }
            char decoded = (char)((high << 4) | low);
            if (decoded == '\0') {
                return false;
            }
            destination[output++] = decoded;
            index += 2U;
        } else {
            destination[output++] = source[index];
        }
    }
    destination[output] = '\0';
    return true;
}

static bool form_value(const char *body, const char *name,
                       char *value, size_t value_size)
{
    size_t name_length = strlen(name);
    const char *cursor = body;
    while (*cursor != '\0') {
        const char *entry_end = strchr(cursor, '&');
        if (entry_end == NULL) {
            entry_end = cursor + strlen(cursor);
        }
        if ((size_t)(entry_end - cursor) > name_length &&
            memcmp(cursor, name, name_length) == 0 &&
            cursor[name_length] == '=') {
            return url_decode(cursor + name_length + 1U,
                              (size_t)(entry_end - cursor) - name_length - 1U,
                              value, value_size);
        }
        cursor = *entry_end == '\0' ? entry_end : entry_end + 1U;
    }
    return false;
}

static esp_err_t root_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_send(request, s_setup_page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t save_handler(httpd_req_t *request)
{
    if (request->content_len <= 0 ||
        request->content_len > WIFI_FORM_BODY_MAX_LENGTH) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                            "Invalid form length");
        return ESP_FAIL;
    }

    char body[WIFI_FORM_BODY_MAX_LENGTH + 1U];
    size_t received = 0U;
    while (received < (size_t)request->content_len) {
        int result = httpd_req_recv(request, body + received,
                                    (size_t)request->content_len - received);
        if (result <= 0) {
            httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                "Invalid request");
            return ESP_FAIL;
        }
        received += (size_t)result;
    }
    body[received] = '\0';

    char ssid[WIFI_FORM_SSID_MAX_LENGTH + 1U];
    char password[WIFI_FORM_PASSWORD_MAX_LENGTH + 1U];
    if (!form_value(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0' ||
        !form_value(body, "password", password, sizeof(password))) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                            "SSID/password is invalid or too long");
        return ESP_FAIL;
    }

    size_t password_length = strlen(password);
    bool is_hex_psk = password_length == 64U;
    for (size_t index = 0U; is_hex_psk && index < password_length; index++) {
        is_hex_psk = isxdigit((unsigned char)password[index]) != 0;
    }
    if ((password_length > 0U && password_length < 8U) ||
        (password_length == 64U && !is_hex_psk)) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                            "Password must be empty, 8-63 characters, or a 64-character hex PSK");
        return ESP_FAIL;
    }

    esp_err_t err = s_submit_cb != NULL
                        ? s_submit_cb(ssid, password, s_submit_context)
                        : ESP_ERR_INVALID_STATE;
    memset(password, 0, sizeof(password));
    memset(body, 0, sizeof(body));
    if (err != ESP_OK) {
        httpd_resp_send_err(request,
                            err == ESP_ERR_INVALID_ARG
                                ? HTTPD_400_BAD_REQUEST
                                : HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Unable to start connection");
        return ESP_FAIL;
    }

    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(request, "Credentials accepted. Connecting...");
}

static esp_err_t status_handler(httpd_req_t *request)
{
    wifi_config_portal_status_t status;
    portENTER_CRITICAL(&s_status_lock);
    status = s_status;
    portEXIT_CRITICAL(&s_status_lock);

    const char *name = "ready";
    if (status == WIFI_CONFIG_PORTAL_CONNECTING) {
        name = "connecting";
    } else if (status == WIFI_CONFIG_PORTAL_CONNECTION_FAILED) {
        name = "failed";
    } else if (status == WIFI_CONFIG_PORTAL_STORAGE_FAILED) {
        name = "storage_failed";
    }

    char response[48];
    snprintf(response, sizeof(response), "{\"status\":\"%s\"}", name);
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, response);
}

esp_err_t wifi_config_portal_start(wifi_config_portal_submit_cb_t submit_cb,
                                   void *context)
{
    if (submit_cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_server != NULL) {
        return ESP_OK;
    }

    s_submit_cb = submit_cb;
    s_submit_context = context;
    wifi_config_portal_set_status(WIFI_CONFIG_PORTAL_READY);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 4096;
    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        s_server = NULL;
        return err;
    }

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
    };
    const httpd_uri_t save = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = save_handler,
    };
    const httpd_uri_t status = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_handler,
    };

    err = httpd_register_uri_handler(s_server, &root);
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(s_server, &save);
    }
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(s_server, &status);
    }
    if (err != ESP_OK) {
        wifi_config_portal_stop();
        return err;
    }

    ESP_LOGI(TAG, "Wi-Fi config web server started");
    return ESP_OK;
}

void wifi_config_portal_stop(void)
{
    if (s_server != NULL) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    s_submit_cb = NULL;
    s_submit_context = NULL;
}

bool wifi_config_portal_is_running(void)
{
    return s_server != NULL;
}

void wifi_config_portal_set_status(wifi_config_portal_status_t status)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status = status;
    portEXIT_CRITICAL(&s_status_lock);
}

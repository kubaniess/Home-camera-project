#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "sdkconfig.h"

#if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_ARDUHAL_ESP_LOG)
#include "esp32-hal-log.h"
#define TAG ""
#else
#include "esp_log.h"
static const char *TAG = "camera_httpd";
#endif

httpd_handle_t camera_httpd = NULL;

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *_STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *_STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *_STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\nX-Timestamp: %ld.%06ld\r\n\r\n";

typedef struct
{
    httpd_req_t *req;
    size_t len;
} jpg_chunking_t;

static size_t jpg_encode_stream(void *arg, size_t index, const void *data, size_t len)
{
    jpg_chunking_t *j = (jpg_chunking_t *)arg;
    if (!index)
    {
        j->len = 0;
    }
    if (httpd_resp_send_chunk(j->req, (const char *)data, len) != ESP_OK)
    {
        return 0;
    }
    j->len += len;
    return len;
}

static esp_err_t capture_handler(httpd_req_t *req)
{
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;

    fb = esp_camera_fb_get();
    if (!fb)
    {
        ESP_LOGE(TAG, "Camera capture failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    char ts[32];
    snprintf(ts, sizeof(ts), "%ld.%06ld", fb->timestamp.tv_sec, fb->timestamp.tv_usec);
    httpd_resp_set_hdr(req, "X-Timestamp", ts);

    size_t fb_len = 0;
    if (fb->format == PIXFORMAT_JPEG)
    {
        fb_len = fb->len;
        res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    }
    else
    {
        jpg_chunking_t jchunk = {req, 0};
        res = frame2jpg_cb(fb, 80, jpg_encode_stream, &jchunk) ? ESP_OK : ESP_FAIL;
        httpd_resp_send_chunk(req, NULL, 0);
        fb_len = jchunk.len;
    }
    esp_camera_fb_return(fb);

    return res;
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    uint8_t *jpg_buf = NULL;
    size_t jpg_buf_len = 0;
    char part_buf[128];

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (res != ESP_OK)
    {
        return res;
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "X-Framerate", "60");

    while (true)
    {
        fb = esp_camera_fb_get();
        if (!fb)
        {
            ESP_LOGE(TAG, "Camera capture failed");
            res = ESP_FAIL;
            break;
        }
        else
        {
            struct timeval timestamp = fb->timestamp;

            if (fb->format != PIXFORMAT_JPEG)
            {
                bool jpeg_converted = frame2jpg(fb, 80, &jpg_buf, &jpg_buf_len);
                esp_camera_fb_return(fb);
                fb = NULL;
                if (!jpeg_converted)
                {
                    ESP_LOGE(TAG, "JPEG compression failed");
                    res = ESP_FAIL;
                    break;
                }
            }
            else
            {
                jpg_buf_len = fb->len;
                jpg_buf = fb->buf;
            }

            if (res == ESP_OK)
            {
                size_t hlen = snprintf(part_buf, sizeof(part_buf), _STREAM_PART, jpg_buf_len, timestamp.tv_sec, timestamp.tv_usec);
                res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
                if (res == ESP_OK)
                {
                    res = httpd_resp_send_chunk(req, part_buf, hlen);
                }
                if (res == ESP_OK)
                {
                    res = httpd_resp_send_chunk(req, (const char *)jpg_buf, jpg_buf_len);
                }
            }

            if (fb)
            {
                esp_camera_fb_return(fb);
                fb = NULL;
                jpg_buf = NULL;
            }
            else if (jpg_buf)
            {
                free(jpg_buf);
                jpg_buf = NULL;
            }

            if (res != ESP_OK)
            {
                break;
            }
        }
    }
    return res;
}

static esp_err_t parse_get(httpd_req_t *req, char **obuf)
{
    char *buf = NULL;
    size_t buf_len = 0;

    buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1)
    {
        buf = (char *)malloc(buf_len);
        if (!buf)
        {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK)
        {
            *obuf = buf;
            return ESP_OK;
        }
        free(buf);
    }
    httpd_resp_send_404(req);
    return ESP_FAIL;
}

static esp_err_t cmd_handler(httpd_req_t *req)
{
    char *buf = NULL;
    char variable[32];
    char value[32];

    if (parse_get(req, &buf) != ESP_OK)
    {
        return ESP_FAIL;
    }
    if (httpd_query_key_value(buf, "var", variable, sizeof(variable)) != ESP_OK ||
        httpd_query_key_value(buf, "val", value, sizeof(value)) != ESP_OK)
    {
        free(buf);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    free(buf);

    int val = atoi(value);
    ESP_LOGI(TAG, "%s = %d", variable, val);
    sensor_t *s = esp_camera_sensor_get();
    int res = 0;

    if (!strcmp(variable, "framesize"))
    {
        if (s->pixformat == PIXFORMAT_JPEG)
        {
            res = s->set_framesize(s, (framesize_t)val);
        }
    }
    else if (!strcmp(variable, "quality"))
        res = s->set_quality(s, val);
    else if (!strcmp(variable, "brightness"))
        res = s->set_brightness(s, val);
    else
    {
        ESP_LOGI(TAG, "Unknown command: %s", variable);
        res = -1;
    }

    if (res < 0)
    {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    static char json_response[256];

    sensor_t *s = esp_camera_sensor_get();
    char *p = json_response;
    *p++ = '{';

    p += sprintf(p, "\"framesize\":%u,", s->status.framesize);
    p += sprintf(p, "\"quality\":%u,", s->status.quality);
    p += sprintf(p, "\"brightness\":%d", s->status.brightness);

    *p++ = '}';
    *p++ = 0;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json_response, strlen(json_response));
}

static const char index_html[] =
"<!DOCTYPE html>"
"<html>"
"<head>"
"<title>ESP32-CAM Web Server</title>"
"<style>"
"body { background-color: #181818; color: #ffffff; font-family: Arial, sans-serif; }"
"h1 { color: #ff0000; }"
".button { background-color: #ff0000; color: #ffffff; padding: 15px 32px; text-align: center; text-decoration: none; display: inline-block; font-size: 16px; margin: 4px 2px; cursor: pointer; }"
".slider { width: 300px; }"
".center { text-align: center; }"
"</style>"
"</head>"
"<body>"
"<div class=\"center\">"
"<h1>ESP32-CAM Web Server</h1>"
"<img id=\"stream\" src=\"\" style=\"display: none;\">"
"<div>"
"<button class=\"button\" id=\"toggle-stream\">Start Stream</button>"
"<button class=\"button\" id=\"get-still\">Get Still</button>"
"</div>"
"<div>"
"<label for=\"framesize\">Resolution:</label>"
"<select id=\"framesize\">"
"<option value=\"10\">UXGA (1600x1200)</option>"
"<option value=\"9\">SXGA (1280x1024)</option>"
"<option value=\"8\">XGA (1024x768)</option>"
"<option value=\"7\">SVGA (800x600)</option>"
"<option value=\"6\">VGA (640x480)</option>"
"<option value=\"5\" selected>QVGA (320x240)</option>"
"</select>"
"</div>"
"<div>"
"<label for=\"quality\">Quality:</label>"
"<input type=\"range\" id=\"quality\" min=\"10\" max=\"63\" value=\"10\" class=\"slider\">"
"</div>"
"<div>"
"<label for=\"brightness\">Brightness:</label>"
"<input type=\"range\" id=\"brightness\" min=\"-2\" max=\"2\" value=\"0\" class=\"slider\">"
"</div>"
"</div>"
"<script>"
"var streamButton = document.getElementById('toggle-stream');"
"var getStillButton = document.getElementById('get-still');"
"var streamImg = document.getElementById('stream');"
"var framesizeSelect = document.getElementById('framesize');"
"var qualitySlider = document.getElementById('quality');"
"var brightnessSlider = document.getElementById('brightness');"
"var streaming = false;"
""
"streamButton.onclick = function() {"
"if (streaming) {"
"stopStream();"
"} else {"
"startStream();"
"}"
"};"
""
"getStillButton.onclick = function() {"
"stopStream();"
"streamImg.src = '/capture?_cb=' + Date.now();"
"streamImg.style.display = 'block';"
"};"
""
"framesizeSelect.onchange = function() {"
"var val = framesizeSelect.value;"
"fetch('/control?var=framesize&val=' + val);"
"};"
""
"qualitySlider.oninput = function() {"
"var val = qualitySlider.value;"
"fetch('/control?var=quality&val=' + val);"
"};"
""
"brightnessSlider.oninput = function() {"
"var val = brightnessSlider.value;"
"fetch('/control?var=brightness&val=' + val);"
"};"
""
"function startStream() {"
"streamImg.src = '/stream';"
"streamImg.style.display = 'block';"
"streamButton.textContent = 'Stop Stream';"
"streaming = true;"
"}"
""
"function stopStream() {"
"streamImg.src = '';"
"streamImg.style.display = 'none';"
"streamButton.textContent = 'Start Stream';"
"streaming = false;"
"}"
"</script>"
"</body>"
"</html>";

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, index_html, strlen(index_html));
    return ESP_OK;
}

void startCameraServer()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;

    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
        .user_ctx = NULL
    };

    httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_handler,
        .user_ctx = NULL
    };

    httpd_uri_t cmd_uri = {
        .uri = "/control",
        .method = HTTP_GET,
        .handler = cmd_handler,
        .user_ctx = NULL
    };

    httpd_uri_t capture_uri = {
        .uri = "/capture",
        .method = HTTP_GET,
        .handler = capture_handler,
        .user_ctx = NULL
    };

    httpd_uri_t stream_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_handler,
        .user_ctx = NULL
    };

    ESP_LOGI(TAG, "Starting web server on port: '%d'", config.server_port);
    if (httpd_start(&camera_httpd, &config) == ESP_OK)
    {
        httpd_register_uri_handler(camera_httpd, &index_uri);
        httpd_register_uri_handler(camera_httpd, &cmd_uri);
        httpd_register_uri_handler(camera_httpd, &status_uri);
        httpd_register_uri_handler(camera_httpd, &capture_uri);
        httpd_register_uri_handler(camera_httpd, &stream_uri); // Register stream handler on the same server
    }
}

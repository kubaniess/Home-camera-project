#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "nvs_flash.h"
#include "nvs.h"
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

static const char *USERNAME_KEY = "username";
static const char *PASSWORD_KEY = "password";
static const char *NVS_NAMESPACE = "storage";

// Forward declarations
static esp_err_t parse_get(httpd_req_t *req, char **obuf);
static esp_err_t index_handler(httpd_req_t *req);
static esp_err_t change_password_handler(httpd_req_t *req);

static const char index_html[] =
"<!DOCTYPE html>"
"<html>"
"<head>"
"<title>ESP32-CAM Web Server</title>"
"<style>"
"body { background-color: #181818; color: #ffffff; font-family: Arial, sans-serif; }"
"h1 { color: #ff0000; }"
".button { background-color: #ff0000; color: #ffffff; padding: 10px 20px; text-align: center; text-decoration: none; display: inline-block; font-size: 16px; margin: 4px 2px; cursor: pointer; }"
".slider { width: 300px; }"
".center { text-align: center; }"
".top-right { position: absolute; top: 10px; right: 10px; }"
"</style>"
"</head>"
"<body>"
"<div class=\"top-right\">"
"<button class=\"button\" onclick=\"location.href='/change_password'\">Change Password</button>"
"</div>"
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

// Function to check if credentials are set
static bool credentials_set()
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK)
    {
        return false;
    }
    size_t required_size;
    err = nvs_get_str(nvs_handle, USERNAME_KEY, NULL, &required_size);
    nvs_close(nvs_handle);
    return (err == ESP_OK && required_size > 0);
}

// Function to verify credentials
static bool verify_credentials(const char *username, const char *password)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK)
    {
        return false;
    }

    size_t username_size = 64;
    size_t password_size = 64;
    char stored_username[64];
    char stored_password[64];

    err = nvs_get_str(nvs_handle, USERNAME_KEY, stored_username, &username_size);
    if (err != ESP_OK)
    {
        nvs_close(nvs_handle);
        return false;
    }

    err = nvs_get_str(nvs_handle, PASSWORD_KEY, stored_password, &password_size);
    nvs_close(nvs_handle);
    if (err != ESP_OK)
    {
        return false;
    }

    if (strcmp(username, stored_username) == 0 && strcmp(password, stored_password) == 0)
    {
        return true;
    }
    return false;
}

// Middleware to check authentication
esp_err_t check_auth(httpd_req_t *req)
{
    if (!credentials_set())
    {
        // Redirect to setup page
        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", "/setup");
        httpd_resp_send(req, NULL, 0);
        return ESP_FAIL;
    }

    // Check if authenticated (In a real application, you should use secure cookies or tokens)
    size_t buf_len = httpd_req_get_hdr_value_len(req, "Cookie") + 1;
    if (buf_len > 1)
    {
        char *buf = (char *)malloc(buf_len);
        if (httpd_req_get_hdr_value_str(req, "Cookie", buf, buf_len) == ESP_OK)
        {
            if (strstr(buf, "authenticated=1") != NULL)
            {
                free(buf);
                return ESP_OK;
            }
        }
        free(buf);
    }

    // Redirect to login page
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/login");
    httpd_resp_send(req, NULL, 0);
    return ESP_FAIL;
}

// Handler for the setup page to set username and password
static esp_err_t setup_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET)
    {
        const char setup_html[] =
            "<!DOCTYPE html>"
            "<html>"
            "<head>"
            "<title>Setup Credentials</title>"
            "<style>"
            "body { background-color: #181818; color: #ffffff; font-family: Arial, sans-serif; }"
            "h1 { color: #ff0000; }"
            ".center { text-align: center; }"
            "input { padding: 10px; margin: 5px; width: 200px; }"
            "button { background-color: #ff0000; color: #ffffff; padding: 10px 20px; border: none; cursor: pointer; }"
            "</style>"
            "</head>"
            "<body>"
            "<div class=\"center\">"
            "<h1>Setup Credentials</h1>"
            "<form action=\"/setup\" method=\"post\">"
            "<input type=\"text\" name=\"username\" placeholder=\"Username\" required><br>"
            "<input type=\"password\" name=\"password\" placeholder=\"Password\" required><br>"
            "<button type=\"submit\">Save</button>"
            "</form>"
            "</div>"
            "</body>"
            "</html>";
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, setup_html, strlen(setup_html));
    }
    else if (req->method == HTTP_POST)
    {
        char buf[128];
        int ret, remaining = req->content_len;

        char username[64] = {0};
        char password[64] = {0};

        // Read the request body
        if (remaining >= sizeof(buf))
        {
            ESP_LOGE(TAG, "Request content too long");
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        ret = httpd_req_recv(req, buf, remaining);
        if (ret <= 0)
        {
            ESP_LOGE(TAG, "Failed to receive request body");
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        buf[ret] = '\0';

        // Parse form data
        char *user_ptr = strstr(buf, "username=");
        char *pass_ptr = strstr(buf, "password=");

        if (user_ptr)
        {
            user_ptr += strlen("username=");
            char *end = strchr(user_ptr, '&');
            if (end)
            {
                *end = '\0';
            }
            strncpy(username, user_ptr, sizeof(username) - 1);
        }

        if (pass_ptr)
        {
            pass_ptr += strlen("password=");
            char *end = strchr(pass_ptr, '&');
            if (end)
            {
                *end = '\0';
            }
            strncpy(password, pass_ptr, sizeof(password) - 1);
        }

        // Save credentials to NVS
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
        if (err != ESP_OK)
        {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        err = nvs_set_str(nvs_handle, USERNAME_KEY, username);
        if (err != ESP_OK)
        {
            nvs_close(nvs_handle);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        err = nvs_set_str(nvs_handle, PASSWORD_KEY, password);
        if (err != ESP_OK)
        {
            nvs_close(nvs_handle);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);

        // Redirect to login page
        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", "/login");
        httpd_resp_send(req, NULL, 0);
    }
    return ESP_OK;
}

// Handler for the login page
static esp_err_t login_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET)
    {
        const char login_html[] =
            "<!DOCTYPE html>"
            "<html>"
            "<head>"
            "<title>Login</title>"
            "<style>"
            "body { background-color: #181818; color: #ffffff; font-family: Arial, sans-serif; }"
            "h1 { color: #ff0000; }"
            ".center { text-align: center; }"
            "input { padding: 10px; margin: 5px; width: 200px; }"
            "button { background-color: #ff0000; color: #ffffff; padding: 10px 20px; border: none; cursor: pointer; }"
            "</style>"
            "</head>"
            "<body>"
            "<div class=\"center\">"
            "<h1>Login</h1>"
            "<form action=\"/login\" method=\"post\">"
            "<input type=\"text\" name=\"username\" placeholder=\"Username\" required><br>"
            "<input type=\"password\" name=\"password\" placeholder=\"Password\" required><br>"
            "<button type=\"submit\">Login</button>"
            "</form>"
            "</div>"
            "</body>"
            "</html>";
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, login_html, strlen(login_html));
    }
    else if (req->method == HTTP_POST)
    {
        char buf[128];
        int ret, remaining = req->content_len;

        char username[64] = {0};
        char password[64] = {0};

        // Read the request body
        if (remaining >= sizeof(buf))
        {
            ESP_LOGE(TAG, "Request content too long");
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        ret = httpd_req_recv(req, buf, remaining);
        if (ret <= 0)
        {
            ESP_LOGE(TAG, "Failed to receive request body");
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        buf[ret] = '\0';

        // Parse form data
        char *user_ptr = strstr(buf, "username=");
        char *pass_ptr = strstr(buf, "password=");

        if (user_ptr)
        {
            user_ptr += strlen("username=");
            char *end = strchr(user_ptr, '&');
            if (end)
            {
                *end = '\0';
            }
            strncpy(username, user_ptr, sizeof(username) - 1);
        }

        if (pass_ptr)
        {
            pass_ptr += strlen("password=");
            char *end = strchr(pass_ptr, '&');
            if (end)
            {
                *end = '\0';
            }
            strncpy(password, pass_ptr, sizeof(password) - 1);
        }

        // Verify credentials
        if (verify_credentials(username, password))
        {
            // Set session (In a real application, you should use secure cookies or tokens)
            httpd_resp_set_hdr(req, "Set-Cookie", "authenticated=1");

            // Redirect to main page
            httpd_resp_set_status(req, "303 See Other");
            httpd_resp_set_hdr(req, "Location", "/");
            httpd_resp_send(req, NULL, 0);
        }
        else
        {
            // Show error message
            const char error_html[] =
                "<!DOCTYPE html>"
                "<html>"
                "<head>"
                "<title>Login Failed</title>"
                "<style>"
                "body { background-color: #181818; color: #ffffff; font-family: Arial, sans-serif; }"
                "h1 { color: #ff0000; }"
                ".center { text-align: center; }"
                "</style>"
                "</head>"
                "<body>"
                "<div class=\"center\">"
                "<h1>Wrong Credentials</h1>"
                "<p>Please try again.</p>"
                "<a href=\"/login\">Go back to login page</a>"
                "</div>"
                "</body>"
                "</html>";
            httpd_resp_set_type(req, "text/html");
            httpd_resp_send(req, error_html, strlen(error_html));
        }
    }
    return ESP_OK;
}

// Handler for changing the password
static esp_err_t change_password_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Entered change_password_handler");

    if (check_auth(req) != ESP_OK)
    {
        ESP_LOGE(TAG, "Authentication failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Authenticated successfully");

    if (req->method == HTTP_GET)
    {
        ESP_LOGI(TAG, "Serving Change Password page");
        const char change_password_html[] =
            "<!DOCTYPE html>"
            "<html>"
            "<head>"
            "<title>Change Password</title>"
            "<style>"
            "body { background-color: #181818; color: #ffffff; font-family: Arial, sans-serif; }"
            "h1 { color: #ff0000; }"
            ".center { text-align: center; }"
            "input { padding: 10px; margin: 5px; width: 200px; }"
            "button { background-color: #ff0000; color: #ffffff; padding: 10px 20px; border: none; cursor: pointer; }"
            "</style>"
            "</head>"
            "<body>"
            "<div class=\"center\">"
            "<h1>Change Password</h1>"
            "<form action=\"/change_password\" method=\"post\">"
            "<input type=\"password\" name=\"new_password\" placeholder=\"New Password\" required><br>"
            "<button type=\"submit\">Change Password</button>"
            "</form>"
            "</div>"
            "</body>"
            "</html>";
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, change_password_html, strlen(change_password_html));
    }
    else if (req->method == HTTP_POST)
    {
        ESP_LOGI(TAG, "Processing Change Password POST request");

        char buf[128];
        int ret, remaining = req->content_len;

        char new_password[64] = {0};

        if (remaining >= sizeof(buf))
        {
            ESP_LOGE(TAG, "Request content too long");
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        int received = 0;
        while (remaining > 0)
        {
            if ((ret = httpd_req_recv(req, buf + received, remaining)) <= 0)
            {
                if (ret == HTTPD_SOCK_ERR_TIMEOUT)
                {
                    continue;
                }
                ESP_LOGE(TAG, "Failed to receive request body");
                httpd_resp_send_500(req);
                return ESP_FAIL;
            }
            received += ret;
            remaining -= ret;
        }
        buf[received] = '\0';

        ESP_LOGI(TAG, "Received POST data: %s", buf);

        // Parse form data
        char *pass_ptr = strstr(buf, "new_password=");

        if (pass_ptr)
        {
            pass_ptr += strlen("new_password=");
            char *end = strchr(pass_ptr, '&');
            if (end)
            {
                *end = '\0';
            }
            strncpy(new_password, pass_ptr, sizeof(new_password) - 1);

            // URL decode the password if necessary
            // For now, assuming no special characters
        }
        else
        {
            ESP_LOGE(TAG, "new_password not found in POST data");
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_send(req, "Bad Request", strlen("Bad Request"));
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "New password: %s", new_password);

        // Update password in NVS
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
        if (err != ESP_OK)
        {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        err = nvs_set_str(nvs_handle, PASSWORD_KEY, new_password);
        if (err != ESP_OK)
        {
            nvs_close(nvs_handle);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);

        ESP_LOGI(TAG, "Password changed successfully");

        // Show success message
        const char success_html[] =
            "<!DOCTYPE html>"
            "<html>"
            "<head>"
            "<title>Password Changed</title>"
            "<style>"
            "body { background-color: #181818; color: #ffffff; font-family: Arial, sans-serif; }"
            "h1 { color: #00ff00; }"
            ".center { text-align: center; }"
            "</style>"
            "</head>"
            "<body>"
            "<div class=\"center\">"
            "<h1>Password Changed Successfully</h1>"
            "<a href=\"/\">Go back to Home</a>"
            "</div>"
            "</body>"
            "</html>";
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, success_html, strlen(success_html));
    }
    else
    {
        ESP_LOGE(TAG, "Unsupported method");
        httpd_resp_set_status(req, "405 Method Not Allowed");
        httpd_resp_send(req, "Method Not Allowed", strlen("Method Not Allowed"));
    }

    return ESP_OK;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    if (check_auth(req) != ESP_OK)
    {
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, index_html, strlen(index_html));
    return ESP_OK;
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
    if (check_auth(req) != ESP_OK)
    {
        return ESP_FAIL;
    }

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
    if (check_auth(req) != ESP_OK)
    {
        return ESP_FAIL;
    }

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

static esp_err_t capture_handler(httpd_req_t *req)
{
    if (check_auth(req) != ESP_OK)
    {
        return ESP_FAIL;
    }

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
    if (check_auth(req) != ESP_OK)
    {
        return ESP_FAIL;
    }

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

// Start the web server and register URI handlers
void startCameraServer()
{
    // Initialize NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to init NVS");
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;

    httpd_uri_t setup_uri = {
        .uri = "/setup",
        .method = HTTP_GET,
        .handler = setup_handler,
        .user_ctx = NULL};

    httpd_uri_t setup_post_uri = {
        .uri = "/setup",
        .method = HTTP_POST,
        .handler = setup_handler,
        .user_ctx = NULL};

    httpd_uri_t login_uri = {
        .uri = "/login",
        .method = HTTP_GET,
        .handler = login_handler,
        .user_ctx = NULL};

    httpd_uri_t login_post_uri = {
        .uri = "/login",
        .method = HTTP_POST,
        .handler = login_handler,
        .user_ctx = NULL};

    httpd_uri_t change_password_uri = {
        .uri = "/change_password",
        .method = HTTP_GET,
        .handler = change_password_handler,
        .user_ctx = NULL};

    httpd_uri_t change_password_post_uri = {
        .uri = "/change_password",
        .method = HTTP_POST,
        .handler = change_password_handler,
        .user_ctx = NULL};

    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
        .user_ctx = NULL};

    httpd_uri_t capture_uri = {
        .uri = "/capture",
        .method = HTTP_GET,
        .handler = capture_handler,
        .user_ctx = NULL};

    httpd_uri_t stream_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_handler,
        .user_ctx = NULL};

    httpd_uri_t cmd_uri = {
        .uri = "/control",
        .method = HTTP_GET,
        .handler = cmd_handler,
        .user_ctx = NULL};

    httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_handler,
        .user_ctx = NULL};

    ESP_LOGI(TAG, "Starting web server on port: '%d'", config.server_port);
    if (httpd_start(&camera_httpd, &config) == ESP_OK)
    {
        httpd_register_uri_handler(camera_httpd, &setup_uri);
        httpd_register_uri_handler(camera_httpd, &setup_post_uri);
        httpd_register_uri_handler(camera_httpd, &login_uri);
        httpd_register_uri_handler(camera_httpd, &login_post_uri);
        httpd_register_uri_handler(camera_httpd, &change_password_uri);
        httpd_register_uri_handler(camera_httpd, &change_password_post_uri);
        httpd_register_uri_handler(camera_httpd, &index_uri);
        httpd_register_uri_handler(camera_httpd, &capture_uri);
        httpd_register_uri_handler(camera_httpd, &stream_uri);
        httpd_register_uri_handler(camera_httpd, &cmd_uri);
        httpd_register_uri_handler(camera_httpd, &status_uri);
    }
}

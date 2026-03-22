/* ============================================================
 *  Rakshak 2.0 — ESP32-CAM Stream Server
 *  AI Thinker Module
 * ============================================================
 *
 *  WHAT CHANGED FROM YOUR CURRENT CODE:
 *  - Added /capture endpoint (single JPEG, needed by YOLO)
 *  - Fixed MJPEG boundary to standard format (cv2 compatible)
 *  - Added /status endpoint (health check from laptop)
 *  - Increased frame buffer to 2 (smoother stream)
 *  - Prints stream URL to Serial clearly
 *
 *  ENDPOINTS:
 *    http://<IP>/        → MJPEG live stream  (Python OpenCV)
 *    http://<IP>/capture → Single JPEG frame
 *    http://<IP>/status  → JSON health check
 *
 *  BOARD: AI Thinker ESP32-CAM
 *  Partition: Huge APP (3MB No OTA)
 * ============================================================ */

#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"

// ── WiFi ─────────────────────────────────────────────────────
// WiFi credentials — fill in your own
const char *ssid = "YOUR_WIFI_SSID";
const char *password = "YOUR_WIFI_PASSWORD";

// ── Camera Pins (AI Thinker — same as your current code) ─────
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

httpd_handle_t camera_httpd = NULL;

// ── MJPEG Stream Handler ─────────────────────────────────────
// Fixed boundary format compatible with Python cv2.VideoCapture
#define STREAM_CONTENT_TYPE "multipart/x-mixed-replace;boundary=frame"
#define STREAM_BOUNDARY "\r\n--frame\r\n"
#define STREAM_PART "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n"

static esp_err_t stream_handler(httpd_req_t *req)
{
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
  char part_buf[64];

  res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK)
    return res;

  // Disable timeout for streaming
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  while (true)
  {
    fb = esp_camera_fb_get();
    if (!fb)
    {
      Serial.println("Camera capture failed");
      res = ESP_FAIL;
      break;
    }

    res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (res == ESP_OK)
    {
      size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, fb->len);
      res = httpd_resp_send_chunk(req, part_buf, hlen);
    }
    if (res == ESP_OK)
    {
      res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    }
    if (res == ESP_OK)
    {
      res = httpd_resp_send_chunk(req, "\r\n", 2);
    }

    esp_camera_fb_return(fb);
    if (res != ESP_OK)
      break;
  }
  return res;
}

// ── Single Capture Handler ───────────────────────────────────
static esp_err_t capture_handler(httpd_req_t *req)
{
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb)
  {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

// ── Status Handler ───────────────────────────────────────────
static esp_err_t status_handler(httpd_req_t *req)
{
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  String json = "{\"status\":\"ok\",\"uptime\":" + String(millis()) + "}";
  return httpd_resp_send(req, json.c_str(), json.length());
}

// ── Start HTTP Server ─────────────────────────────────────────
void startCameraServer()
{
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_uri_handlers = 8;
  config.max_resp_headers = 8;

  httpd_uri_t stream_uri = {.uri = "/", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL};
  httpd_uri_t capture_uri = {.uri = "/capture", .method = HTTP_GET, .handler = capture_handler, .user_ctx = NULL};
  httpd_uri_t status_uri = {.uri = "/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = NULL};

  if (httpd_start(&camera_httpd, &config) == ESP_OK)
  {
    httpd_register_uri_handler(camera_httpd, &stream_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &status_uri);
    Serial.println("[Camera] HTTP server started");
  }
  else
  {
    Serial.println("[Camera] Server start failed!");
  }
}

void setup()
{
  Serial.begin(115200);
  Serial.println("\nRakshak 2.0 — ESP32-CAM Starting...");

  // Camera config
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_VGA; // 640x480 — best for YOLO accuracy
  config.jpeg_quality = 12;
  config.fb_count = 2; // 2 buffers for smoother stream

  if (esp_camera_init(&config) != ESP_OK)
  {
    Serial.println("Camera init FAILED");
    return;
  }

  // Flip/mirror if needed for your mounting
  sensor_t *s = esp_camera_sensor_get();
  s->set_vflip(s, 0);   // set to 1 if image is upside down
  s->set_hmirror(s, 0); // set to 1 if image is mirrored

  Serial.println("Camera OK");

  WiFi.begin(ssid, password);
  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi connected!");
  Serial.println("─────────────────────────────────────");
  Serial.print("  Stream  : http://");
  Serial.print(WiFi.localIP());
  Serial.println("/");
  Serial.print("  Capture : http://");
  Serial.print(WiFi.localIP());
  Serial.println("/capture");
  Serial.print("  Status  : http://");
  Serial.print(WiFi.localIP());
  Serial.println("/status");
  Serial.println("─────────────────────────────────────");
  Serial.println("Copy the Stream URL → paste into main_esp32_audio.py --cam-url");

  startCameraServer();
}

void loop()
{
  delay(10000); // server runs on FreeRTOS tasks
}

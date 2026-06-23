/*********
  ESP32-CAM Web Server + MJPEG Stream + Servo Control + Flash Control
  Fixed: Single HTTP Server + Flash standby on stream
*********/

#include "esp_camera.h"
#include <WiFi.h>
#include "esp_timer.h"
#include "img_converters.h"
#include "Arduino.h"
#include "fb_gfx.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_http_server.h"
#include <ESP32Servo.h>

// WiFi credentials
const char* ssid = "Qwerty";
const char* password = "123456789";

// Pins
#define SERVO_PIN     14
#define FLASH_PIN     4     // ESP32-CAM onboard flash LED

Servo myServo;

// Camera model
#define CAMERA_MODEL_AI_THINKER

#if defined(CAMERA_MODEL_AI_THINKER)
  #define PWDN_GPIO_NUM     32
  #define RESET_GPIO_NUM    -1
  #define XCLK_GPIO_NUM      0
  #define SIOD_GPIO_NUM     26
  #define SIOC_GPIO_NUM     27
  #define Y9_GPIO_NUM       35
  #define Y8_GPIO_NUM       34
  #define Y7_GPIO_NUM       39
  #define Y6_GPIO_NUM       36
  #define Y5_GPIO_NUM       21
  #define Y4_GPIO_NUM       19
  #define Y3_GPIO_NUM       18
  #define Y2_GPIO_NUM        5
  #define VSYNC_GPIO_NUM    25
  #define HREF_GPIO_NUM     23
  #define PCLK_GPIO_NUM     22
#else
  #error "Camera model not selected"
#endif

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

httpd_handle_t camera_httpd = NULL;

// Flash state
bool flashOn = false;

// ==================== HELPER: FLASH CONTROL ====================
void setFlash(bool state) {
  flashOn = state;
  digitalWrite(FLASH_PIN, state ? HIGH : LOW);
}

// ==================== STREAM HANDLER (Flash standby ON) ====================
static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t * fb = NULL;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t * _jpg_buf = NULL;
  char * part_buf[64];

  // Turn flash ON when stream starts (standby/active mode)
  setFlash(true);

  res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) {
    setFlash(false);
    return res;
  }

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      res = ESP_FAIL;
    } else {
      if (fb->format != PIXFORMAT_JPEG) {
        bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
        esp_camera_fb_return(fb);
        fb = NULL;
        if (!jpeg_converted) {
          Serial.println("JPEG compression failed");
          res = ESP_FAIL;
        }
      } else {
        _jpg_buf_len = fb->len;
        _jpg_buf = fb->buf;
      }
    }

    if (res == ESP_OK) {
      size_t hlen = snprintf((char *)part_buf, 64, STREAM_PART, _jpg_buf_len);
      res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    }

    if (fb) {
      esp_camera_fb_return(fb);
      fb = NULL;
      _jpg_buf = NULL;
    } else if (_jpg_buf) {
      free(_jpg_buf);
      _jpg_buf = NULL;
    }

    if (res != ESP_OK) break;

    delay(50); // Adjust for FPS / performance
  }

  // Turn flash OFF when stream ends
  setFlash(false);
  return res;
}

// ==================== INDEX PAGE ====================
static esp_err_t index_handler(httpd_req_t *req) {
  const char* html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>ESP32-CAM</title>
  <style>
    body { font-family: Arial; text-align: center; margin: 0; padding: 20px; background: #f0f0f0;}
    img { width: 100%; max-width: 800px; border: 2px solid #333; }
    .btn { padding: 15px 30px; margin: 10px; font-size: 18px; cursor: pointer; border: none; border-radius: 8px; }
    .btn-on { background: #4CAF50; color: white; }
    .btn-off { background: #f44336; color: white; }
    .status { font-size: 16px; margin: 10px; }
  </style>
</head>
<body>
  <h1>ESP32-CAM Live Stream</h1>
  <img src="/stream" alt="Live Stream" />
  
  <h2>Controls</h2>
  <button class="btn" onclick="trigger(1)">Servo Type 1 (10°)</button>
  <button class="btn" onclick="trigger(2)">Servo Type 2 (170°)</button>
  
  <h2>Flash Control</h2>
  <button class="btn btn-on" onclick="setFlash(1)">Flash ON</button>
  <button class="btn btn-off" onclick="setFlash(0)">Flash OFF</button>
  <div class="status" id="flashStatus">Flash: OFF</div>

  <script>
    function trigger(type) {
      fetch(`/detect?type=${type}`)
        .then(response => response.text())
        .then(data => console.log(data));
    }

    function setFlash(state) {
      fetch(`/flash?state=${state}`)
        .then(response => response.text())
        .then(data => {
          console.log(data);
          document.getElementById('flashStatus').textContent = 
            'Flash: ' + (state == 1 ? 'ON' : 'OFF');
        });
    }
  </script>
</body>
</html>
)rawliteral";

  httpd_resp_send(req, html, strlen(html));
  return ESP_OK;
}

// ==================== DETECT API (Servo) ====================
static esp_err_t detect_handler(httpd_req_t *req) {
  char query[32];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char param[10];
    if (httpd_query_key_value(query, "type", param, sizeof(param)) == ESP_OK) {
      int type = atoi(param);

      if (type == 1) {
        Serial.println("→ Servo to 10°");
        myServo.write(10);
        delay(800);
        myServo.write(90);
      } else if (type == 2) {
        Serial.println("→ Servo to 170°");
        myServo.write(170);
        delay(800);
        myServo.write(90);
      } else {
        httpd_resp_send(req, "Invalid type", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
      }

      httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
      return ESP_OK;
    }
  }
  httpd_resp_send(req, "Missing parameter", HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

// ==================== FLASH API ====================
static esp_err_t flash_handler(httpd_req_t *req) {
  char query[32];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char param[10];
    if (httpd_query_key_value(query, "state", param, sizeof(param)) == ESP_OK) {
      int state = atoi(param);
      setFlash(state == 1);
      httpd_resp_send(req, state == 1 ? "Flash ON" : "Flash OFF", HTTPD_RESP_USE_STRLEN);
      return ESP_OK;
    }
  }
  httpd_resp_send(req, "Missing state parameter", HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

// ==================== START SERVER ====================
void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_uri_handlers = 10;
  config.stack_size = 8192;

  // Index page
  httpd_uri_t index_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = index_handler,
    .user_ctx  = NULL
  };

  // Stream (with flash standby)
  httpd_uri_t stream_uri = {
    .uri       = "/stream",
    .method    = HTTP_GET,
    .handler   = stream_handler,
    .user_ctx  = NULL
  };

  // Servo control
  httpd_uri_t detect_uri = {
    .uri       = "/detect",
    .method    = HTTP_GET,
    .handler   = detect_handler,
    .user_ctx  = NULL
  };

  // Flash control
  httpd_uri_t flash_uri = {
    .uri       = "/flash",
    .method    = HTTP_GET,
    .handler   = flash_handler,
    .user_ctx  = NULL
  };

  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &stream_uri);
    httpd_register_uri_handler(camera_httpd, &detect_uri);
    httpd_register_uri_handler(camera_httpd, &flash_uri);
    Serial.println("HTTP server started successfully on port 80");
  } else {
    Serial.println("Failed to start HTTP server");
  }
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // Disable brownout

  Serial.begin(115200);

  // Flash pin setup
  pinMode(FLASH_PIN, OUTPUT);
  setFlash(false);  // Start with flash OFF

  // Servo setup
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  myServo.setPeriodHertz(50);
  myServo.attach(SERVO_PIN, 500, 2500);
  myServo.write(90);

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

  if (psramFound()) {
    config.frame_size = FRAMESIZE_SVGA;   // 800x600
    config.jpeg_quality = 10;
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_VGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    return;
  }

  // WiFi
  WiFi.begin(ssid, password);
  WiFi.setSleep(false);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi connected");
  Serial.print("IP Address: http://");
  Serial.println(WiFi.localIP());

  startCameraServer();
}

void loop() {
  delay(1000);
}
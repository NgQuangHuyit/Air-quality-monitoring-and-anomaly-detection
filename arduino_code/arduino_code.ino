// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

/*
 * This is an Arduino-based Azure IoT Hub sample for ESPRESSIF ESP32 boards.
 * It uses our Azure Embedded SDK for C to help interact with Azure IoT.
 * For reference, please visit https://github.com/azure/azure-sdk-for-c.
 *
 * To connect and work with Azure IoT Hub you need an MQTT client, connecting, subscribing
 * and publishing to specific topics to use the messaging features of the hub.
 * Our azure-sdk-for-c is an MQTT client support library, helping composing and parsing the
 * MQTT topic names and messages exchanged with the Azure IoT Hub.
 *
 * This sample performs the following tasks:
 * - Synchronize the device clock with a NTP server;
 * - Initialize our "az_iot_hub_client" (struct for data, part of our azure-sdk-for-c);
 * - Initialize the MQTT client (here we use ESPRESSIF's esp_mqtt_client, which also handle the tcp
 * connection and TLS);
 * - Connect the MQTT client (using server-certificate validation, SAS-tokens for client
 * authentication);
 * - Periodically send telemetry data to the Azure IoT Hub.
 *
 * To properly connect to your Azure IoT Hub, please fill the information in the `iot_configs.h`
 * file.
 */

// C99 libraries
#include <cstdlib>
#include <string.h>
#include <time.h>

// Libraries for MQTT client and WiFi connection
#include <WiFi.h>
#include <mqtt_client.h>

// Azure IoT SDK for C includes
#include <az_core.h>
#include <az_iot.h>
#include <azure_ca.h>

// Additional sample headers
#include "AzIoTSasToken.h"
#include "SerialLogger.h"
#include "iot_configs.h"

#include <DHT.h>
#include <MQUnifiedsensor.h>
#include <Arduino_JSON.h>


#define Board ("ESP32")
#define Voltage_Resolution (3.3)
#define ADC_Bit_Resolution (12)

// Định nghĩa các giá trị mặc định cho các cảm biến trong không khí sạch
#define RatioMQ2CleanAir (9.83)
#define RatioMQ4CleanAir (4.4)
#define RatioMQ135CleanAir (3.6)


// Constants for DHT11
#define DHTPIN 4    // DHT11 is connected to pin D5
#define DHTTYPE DHT11 // DHT11 sensor type
#define LED_PIN 14 
unsigned long ledOnTime = 0; // Thời gian LED được bật
const long duration = 5000; // Thời gian bật LED (5000 ms = 5 giây)
bool ledState = false; // Trạng thái của LED

void alert_led() {
  digitalWrite(LED_PIN, HIGH); // Bật LED
  ledOnTime = millis(); // Ghi lại thời gian bật LED
  ledState = true; // Cập nhật trạng thái LED
}



// DHT11 sensor object
DHT dht(DHTPIN, DHTTYPE);


// Cảm biến MQ-2
// MQUnifiedsensor MQ2_Alcohol(Board, Voltage_Resolution, ADC_Bit_Resolution, 33, "MQ-2");
MQUnifiedsensor MQ2_LPG(Board, Voltage_Resolution, ADC_Bit_Resolution, 33, "MQ-2");
MQUnifiedsensor MQ2_CO(Board, Voltage_Resolution, ADC_Bit_Resolution, 33, "MQ-2");

// Cảm biến MQ-4
MQUnifiedsensor MQ4_smoke(Board, Voltage_Resolution, ADC_Bit_Resolution, 35, "MQ-4");
MQUnifiedsensor MQ4_CH4(Board, Voltage_Resolution, ADC_Bit_Resolution, 35, "MQ-4");

// Cảm biến MQ-135
MQUnifiedsensor MQ135_CO2(Board, Voltage_Resolution, ADC_Bit_Resolution, 32, "MQ-135");
MQUnifiedsensor MQ135_NH4(Board, Voltage_Resolution, ADC_Bit_Resolution, 32, "MQ-135");



// Constants for dust sensor
#define OPERATING_VOLTAGE 3.3
constexpr uint8_t PIN_AOUT = 34;
constexpr uint8_t PIN_IR_LED = 19;

float zeroSensorDustDensity = 0.6;

int sensorADC;
float sensorVoltage;
float sensorDustDensity;




// When developing for your own Arduino-based platform,
// please follow the format '(ard;<platform>)'.
#define AZURE_SDK_CLIENT_USER_AGENT "c%2F" AZ_SDK_VERSION_STRING "(ard;esp32)"

// Utility macros and defines
#define sizeofarray(a) (sizeof(a) / sizeof(a[0]))
#define NTP_SERVERS "pool.ntp.org", "time.nist.gov"
#define MQTT_QOS1 1
#define DO_NOT_RETAIN_MSG 0
#define SAS_TOKEN_DURATION_IN_MINUTES 60
#define UNIX_TIME_NOV_13_2017 1510592825

#define PST_TIME_ZONE -8
#define PST_TIME_ZONE_DAYLIGHT_SAVINGS_DIFF 1

#define GMT_OFFSET_SECS (PST_TIME_ZONE * 3600)
#define GMT_OFFSET_SECS_DST ((PST_TIME_ZONE + PST_TIME_ZONE_DAYLIGHT_SAVINGS_DIFF) * 3600)

// Translate iot_configs.h defines into variables used by the sample
static const char* ssid = IOT_CONFIG_WIFI_SSID;
static const char* password = IOT_CONFIG_WIFI_PASSWORD;
static const char* host = IOT_CONFIG_IOTHUB_FQDN;
static const char* mqtt_broker_uri = "mqtts://" IOT_CONFIG_IOTHUB_FQDN;
static const char* device_id = IOT_CONFIG_DEVICE_ID;
static const int mqtt_port = AZ_IOT_DEFAULT_MQTT_CONNECT_PORT;

// Memory allocated for the sample's variables and structures.
static esp_mqtt_client_handle_t mqtt_client;
static az_iot_hub_client client;

static char mqtt_client_id[128];
static char mqtt_username[128];
static char mqtt_password[200];
static uint8_t sas_signature_buffer[256];
static unsigned long next_telemetry_send_time_ms = 0;
static char telemetry_topic[128];
static uint32_t telemetry_send_count = 0;
static String telemetry_payload = "{}";

#define INCOMING_DATA_BUFFER_SIZE 128
static char incoming_data[INCOMING_DATA_BUFFER_SIZE];

// Auxiliary functions
#ifndef IOT_CONFIG_USE_X509_CERT
static AzIoTSasToken sasToken(
    &client,
    AZ_SPAN_FROM_STR(IOT_CONFIG_DEVICE_KEY),
    AZ_SPAN_FROM_BUFFER(sas_signature_buffer),
    AZ_SPAN_FROM_BUFFER(mqtt_password));
#endif // IOT_CONFIG_USE_X509_CERT

static void connectToWiFi()
{
  Logger.Info("Connecting to WIFI SSID " + String(ssid));

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");

  Logger.Info("WiFi connected, IP address: " + WiFi.localIP().toString());
}

static void initializeTime()
{
  Logger.Info("Setting time using SNTP");

  configTime(GMT_OFFSET_SECS, GMT_OFFSET_SECS_DST, NTP_SERVERS);
  time_t now = time(NULL);
  while (now < UNIX_TIME_NOV_13_2017)
  {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println("");
  Logger.Info("Time initialized!");
}

void receivedCallback(char* topic, byte* payload, unsigned int length)
{
  Logger.Info("Received [");
  Logger.Info(topic);
  Logger.Info("]: ");
  for (int i = 0; i < length; i++)
  {
    Serial.print((char)payload[i]);
  }
  Serial.println("");
}

void processC2DMessage(char* incoming_data) {
  // Ghi log dữ liệu nhận được
  Logger.Info("Received Data: " + String(incoming_data));

  // Parse chuỗi JSON
  JSONVar myObject = JSON.parse(incoming_data);

  // Kiểm tra xem parsing có thành công không
  if (JSON.typeof(myObject) == "undefined") {
    Logger.Error("Parsing input failed!");
    return;
  }

  // Đọc lệnh từ message JSON
  String command = myObject["command"];

  if (command == "toggleLED") {
    alert_led();
    Logger.Info("Toggled LED");
  } 
  else {
    Logger.Error("Unknown command: " + command);
  }
}


#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
  (void)handler_args;
  (void)base;
  (void)event_id;

  esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
#else // ESP_ARDUINO_VERSION_MAJOR
static esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event)
{
#endif // ESP_ARDUINO_VERSION_MAJOR
  switch (event->event_id)
  {
    int i, r;

    case MQTT_EVENT_ERROR:
      Logger.Info("MQTT event MQTT_EVENT_ERROR");
      break;
    case MQTT_EVENT_CONNECTED:
      Logger.Info("MQTT event MQTT_EVENT_CONNECTED");

      r = esp_mqtt_client_subscribe(mqtt_client, AZ_IOT_HUB_CLIENT_C2D_SUBSCRIBE_TOPIC, 1);
      if (r == -1)
      {
        Logger.Error("Could not subscribe for cloud-to-device messages.");
      }
      else
      {
        Logger.Info("Subscribed for cloud-to-device messages; message id:" + String(r));
      }

      break;
    case MQTT_EVENT_DISCONNECTED:
      Logger.Info("MQTT event MQTT_EVENT_DISCONNECTED");
      break;
    case MQTT_EVENT_SUBSCRIBED:
      Logger.Info("MQTT event MQTT_EVENT_SUBSCRIBED");
      break;
    case MQTT_EVENT_UNSUBSCRIBED:
      Logger.Info("MQTT event MQTT_EVENT_UNSUBSCRIBED");
      break;
    case MQTT_EVENT_PUBLISHED:
      Logger.Info("MQTT event MQTT_EVENT_PUBLISHED");
      break;
    case MQTT_EVENT_DATA:
      Logger.Info("MQTT event MQTT_EVENT_DATA");

      for (i = 0; i < (INCOMING_DATA_BUFFER_SIZE - 1) && i < event->topic_len; i++)
      {
        incoming_data[i] = event->topic[i];
      }
      incoming_data[i] = '\0';
      Logger.Info("Topic: " + String(incoming_data));

      for (i = 0; i < (INCOMING_DATA_BUFFER_SIZE - 1) && i < event->data_len; i++)
      {
        incoming_data[i] = event->data[i];
      }
      incoming_data[i] = '\0';
      Logger.Info("Data: " + String(incoming_data));
      processC2DMessage(incoming_data);

      break;
    case MQTT_EVENT_BEFORE_CONNECT:
      Logger.Info("MQTT event MQTT_EVENT_BEFORE_CONNECT");
      break;
    default:
      Logger.Error("MQTT event UNKNOWN");
      break;
  }

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
#else // ESP_ARDUINO_VERSION_MAJOR
  return ESP_OK;
#endif // ESP_ARDUINO_VERSION_MAJOR
}

static void initializeIoTHubClient()
{
  az_iot_hub_client_options options = az_iot_hub_client_options_default();
  options.user_agent = AZ_SPAN_FROM_STR(AZURE_SDK_CLIENT_USER_AGENT);

  if (az_result_failed(az_iot_hub_client_init(
          &client,
          az_span_create((uint8_t*)host, strlen(host)),
          az_span_create((uint8_t*)device_id, strlen(device_id)),
          &options)))
  {
    Logger.Error("Failed initializing Azure IoT Hub client");
    return;
  }

  size_t client_id_length;
  if (az_result_failed(az_iot_hub_client_get_client_id(
          &client, mqtt_client_id, sizeof(mqtt_client_id) - 1, &client_id_length)))
  {
    Logger.Error("Failed getting client id");
    return;
  }

  if (az_result_failed(az_iot_hub_client_get_user_name(
          &client, mqtt_username, sizeofarray(mqtt_username), NULL)))
  {
    Logger.Error("Failed to get MQTT clientId, return code");
    return;
  }

  Logger.Info("Client ID: " + String(mqtt_client_id));
  Logger.Info("Username: " + String(mqtt_username));
}

static int initializeMqttClient()
{
#ifndef IOT_CONFIG_USE_X509_CERT
  if (sasToken.Generate(SAS_TOKEN_DURATION_IN_MINUTES) != 0)
  {
    Logger.Error("Failed generating SAS token");
    return 1;
  }
#endif

  esp_mqtt_client_config_t mqtt_config;
  memset(&mqtt_config, 0, sizeof(mqtt_config));

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    mqtt_config.broker.address.uri = mqtt_broker_uri;
    mqtt_config.broker.address.port = mqtt_port;
    mqtt_config.credentials.client_id = mqtt_client_id;
    mqtt_config.credentials.username = mqtt_username;

  #ifdef IOT_CONFIG_USE_X509_CERT
    LogInfo("MQTT client using X509 Certificate authentication");
    mqtt_config.credentials.authentication.certificate = IOT_CONFIG_DEVICE_CERT;
    mqtt_config.credentials.authentication.certificate_len = (size_t)sizeof(IOT_CONFIG_DEVICE_CERT);
    mqtt_config.credentials.authentication.key = IOT_CONFIG_DEVICE_CERT_PRIVATE_KEY;
    mqtt_config.credentials.authentication.key_len = (size_t)sizeof(IOT_CONFIG_DEVICE_CERT_PRIVATE_KEY);
  #else // Using SAS key
    mqtt_config.credentials.authentication.password = (const char*)az_span_ptr(sasToken.Get());
  #endif

    mqtt_config.session.keepalive = 30;
    mqtt_config.session.disable_clean_session = 0;
    mqtt_config.network.disable_auto_reconnect = false;
    mqtt_config.broker.verification.certificate = (const char*)ca_pem;
    mqtt_config.broker.verification.certificate_len = (size_t)ca_pem_len;
#else // ESP_ARDUINO_VERSION_MAJOR  
  mqtt_config.uri = mqtt_broker_uri;
  mqtt_config.port = mqtt_port;
  mqtt_config.client_id = mqtt_client_id;
  mqtt_config.username = mqtt_username;

#ifdef IOT_CONFIG_USE_X509_CERT
  Logger.Info("MQTT client using X509 Certificate authentication");
  mqtt_config.client_cert_pem = IOT_CONFIG_DEVICE_CERT;
  mqtt_config.client_key_pem = IOT_CONFIG_DEVICE_CERT_PRIVATE_KEY;
#else // Using SAS key
  mqtt_config.password = (const char*)az_span_ptr(sasToken.Get());
#endif

  mqtt_config.keepalive = 30;
  mqtt_config.disable_clean_session = 0;
  mqtt_config.disable_auto_reconnect = false;
  mqtt_config.event_handle = mqtt_event_handler;
  mqtt_config.user_context = NULL;
  mqtt_config.cert_pem = (const char*)ca_pem;
#endif // ESP_ARDUINO_VERSION_MAJOR

  mqtt_client = esp_mqtt_client_init(&mqtt_config);

  if (mqtt_client == NULL)
  {
    Logger.Error("Failed creating mqtt client");
    return 1;
  }

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    esp_mqtt_client_register_event(mqtt_client, MQTT_EVENT_ANY, mqtt_event_handler, NULL);
#endif // ESP_ARDUINO_VERSION_MAJOR

  esp_err_t start_result = esp_mqtt_client_start(mqtt_client);

  if (start_result != ESP_OK)
  {
    Logger.Error("Could not start mqtt client; error code:" + start_result);
    return 1;
  }
  else
  {
    Logger.Info("MQTT client started");
    return 0;
  }
}

/*
 * @brief           Gets the number of seconds since UNIX epoch until now.
 * @return uint32_t Number of seconds.
 */
static uint32_t getEpochTimeInSecs() { return (uint32_t)time(NULL); }

static void establishConnection()
{
  connectToWiFi();
  initializeTime();
  initializeIoTHubClient();
  (void)initializeMqttClient();
}

static void generateTelemetryPayload()
{
   // Đo cảm biến khí 
  // MQ2_Alcohol.update();
  MQ2_LPG.update();
  MQ2_CO.update();
  MQ4_smoke.update();
  MQ4_CH4.update();
  MQ135_CO2.update();
  MQ135_NH4.update();


  // Đọc nồng độ khí
  // float alcoholConcentration = MQ2_Alcohol.readSensor();
  float lpgConcentration = MQ2_LPG.readSensor();
  float coConcentration = MQ2_CO.readSensor();
  float smokeConcentration = MQ4_smoke.readSensor();
  float ch4Concentration = MQ4_CH4.readSensor();
  float co2Concentration = MQ135_CO2.readSensor();
  float nh4Concentration = MQ135_NH4.readSensor();

  // Đo nhiệt độ độ ẩm
  float humidity = dht.readHumidity();
  float temperature = dht.readTemperature();

  // Đo cảm biến bụi
  sensorADC = 0;
  // float tmp = 0.0;
  for (int i = 0; i < 10 ; i++) {
    digitalWrite(PIN_IR_LED, LOW);
    delayMicroseconds(280);
    sensorADC += analogRead(PIN_AOUT);
    // if (tmp > sensorADC) {
    //   sensorADC = tmp;
    // }
    delayMicroseconds(40);
    digitalWrite(PIN_IR_LED, HIGH);
    delayMicroseconds(9680);
  }
  sensorADC = sensorADC / 10;
  sensorVoltage = (OPERATING_VOLTAGE / 4096.0) * sensorADC;

  if (sensorVoltage < zeroSensorDustDensity) {
    Serial.print(sensorVoltage);
    sensorDustDensity = 0;
  } else {
    sensorDustDensity = 0.17 * sensorVoltage - 0.1;
  }

  // Check if readings are valid
  if (isnan(humidity) || isnan(temperature)) {
    Serial.println("Failed to read from DHT11 sensor!");
  }   
  JSONVar doc; 
  doc["deviceid"] = "device01";
  doc["timestamp"] = getEpochTimeInSecs();
  doc["humidity"] = humidity;
  doc["temperature"] = temperature;
  doc["dustVoltage"] = sensorVoltage;
  doc["dustdensity"] = sensorDustDensity * 1000; // Chuyển đổi sang µg/m³
  // doc["alcohol"] = alcoholConcentration;
  doc["smoke"] = smokeConcentration;
  doc["co"] = coConcentration;
  doc["lpg"] = lpgConcentration;
  doc["ch4"] = ch4Concentration;
  doc["co2"] = co2Concentration;
  doc["nh4"] = nh4Concentration;
  // Xuất JSON
  String jsonMessage = JSON.stringify(doc);
    Logger.Info(jsonMessage);
  telemetry_payload = jsonMessage;

}

static void sendTelemetry()
{
  Logger.Info("Sending telemetry ...");

  // The topic could be obtained just once during setup,
  // however if properties are used the topic need to be generated again to reflect the
  // current values of the properties.
  if (az_result_failed(az_iot_hub_client_telemetry_get_publish_topic(
          &client, NULL, telemetry_topic, sizeof(telemetry_topic), NULL)))
  {
    Logger.Error("Failed az_iot_hub_client_telemetry_get_publish_topic");
    return;
  }

  generateTelemetryPayload();

  if (esp_mqtt_client_publish(
          mqtt_client,
          telemetry_topic,
          (const char*)telemetry_payload.c_str(),
          telemetry_payload.length(),
          MQTT_QOS1,
          DO_NOT_RETAIN_MSG)
      == 0)
  {
    Logger.Error("Failed publishing");
  }
  else
  {
    Logger.Info("Message published successfully");
  }
}

// Arduino setup and loop main functions.

void setup() { 
  establishConnection(); 
  Serial.begin(9600);
  // Initialize the dust sensor
  pinMode(PIN_IR_LED, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  digitalWrite(PIN_IR_LED, LOW);
  
  // Initialize DHT11 sensor
  dht.begin();
    

  // Cấu hình hồi quy và khởi tạo cho các cảm biến MQ-2
  MQ2_LPG.setRegressionMethod(1);
  MQ2_LPG.setA(574.25); 
  MQ2_LPG.setB(-2.222);


  MQ2_CO.setRegressionMethod(1);
  MQ2_CO.setA(36974); 
  MQ2_CO.setB(-3.109);

  // MQ2_Alcohol.init();
  MQ2_LPG.setRL(1);
  MQ2_LPG.init();
  MQ2_CO.setRL(1);
  MQ2_CO.init();

  // Cấu hình hồi quy và khởi tạo cho các cảm biến MQ-4
  MQ4_smoke.setRegressionMethod(1);
  MQ4_smoke.setA(30000000);
  MQ4_smoke.setB(-8.308);

  MQ4_CH4.setRegressionMethod(1);
  MQ4_CH4.setA(1012.7);
  MQ4_CH4.setB(-2.786);

  MQ4_smoke.setRL(1);
  MQ4_smoke.init();
  MQ4_CH4.init();

  // Cấu hình hồi quy và khởi tạo cho các cảm biến MQ-135
  MQ135_CO2.setRegressionMethod(1);
  MQ135_CO2.setA(110.47);
  MQ135_CO2.setB(-2.862);

  MQ135_NH4.setRegressionMethod(1);
  MQ135_NH4.setA(102.2);
  MQ135_NH4.setB(-2.473);

  MQ135_CO2.setRL(1);
  MQ135_CO2.init();
  MQ135_NH4.setRL(1);
  MQ135_NH4.init();

  // Hiệu chuẩn cảm biến trong không khí sạch
  Serial.print("Calibrating please wait.");
  //calcR0_Alcohol = 0,
  float calcR0_Smoke = 0, calcR0_CO = 0;
  float calcR0_LPG = 0, calcR0_CH4 = 0;
  float calcR0_CO2 = 0, calcR0_NH4 = 0;

  for (int i = 1; i <= 10; i++) {
    // MQ2_Alcohol.update();
    MQ2_LPG.update();
    MQ2_CO.update();
    MQ4_smoke.update();
    MQ4_CH4.update();
    MQ135_CO2.update();
    MQ135_NH4.update();

    // calcR0_Alcohol += MQ2_Alcohol.calibrate(RatioMQ2CleanAir);
    calcR0_LPG += MQ2_LPG.calibrate(RatioMQ2CleanAir);
    calcR0_CO += MQ2_CO.calibrate(RatioMQ2CleanAir);
    calcR0_Smoke += MQ4_smoke.calibrate(RatioMQ4CleanAir);
    calcR0_CH4 += MQ4_CH4.calibrate(RatioMQ4CleanAir);
    calcR0_CO2 += MQ135_CO2.calibrate(RatioMQ135CleanAir);
    calcR0_NH4 += MQ135_NH4.calibrate(RatioMQ135CleanAir);

    Serial.print(".");
    delay(1000);
  }

  // MQ2_Alcohol.setR0(calcR0_Alcohol / 10);
  MQ2_LPG.setR0(calcR0_LPG / 10);
  MQ2_CO.setR0(calcR0_CO / 10);
  MQ4_smoke.setR0(calcR0_Smoke / 10);
  MQ4_CH4.setR0(calcR0_CH4 / 10);
  MQ135_CO2.setR0(calcR0_CO2 / 10);
  MQ135_NH4.setR0(calcR0_NH4 / 10);

  Serial.println("  done!");

  // Debug thông tin cảm biến
  // MQ2_Alcohol.serialDebug(true);
  MQ2_LPG.serialDebug(true);
  MQ2_CO.serialDebug(true);
  MQ4_smoke.serialDebug(true);
  MQ4_CH4.serialDebug(true);
  MQ135_CO2.serialDebug(true);
  MQ135_NH4.serialDebug(true);
}

void loop()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    connectToWiFi();
  }
#ifndef IOT_CONFIG_USE_X509_CERT
  else if (sasToken.IsExpired())
  {
    Logger.Info("SAS token expired; reconnecting with a new one.");
    (void)esp_mqtt_client_destroy(mqtt_client);
    initializeMqttClient();
  }
#endif
  else if (millis() > next_telemetry_send_time_ms)
  {
    sendTelemetry();
    next_telemetry_send_time_ms = millis() + TELEMETRY_FREQUENCY_MILLISECS;
  }
  if (ledState) {
    // Nếu LED đang sáng, kiểm tra xem đã đến thời gian tắt chưa
    if (millis() - ledOnTime >= duration) {
      digitalWrite(LED_PIN, LOW); // Tắt LED
      ledState = false; // Cập nhật trạng thái LED
    }
  }
}


#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_BMP085.h>
#include <PubSubClient.h>
#include <math.h>


#define SDA_PIN       21
#define SCL_PIN       22

#define LED_PIN       26
#define BUZZER_PIN    27

const char* WIFI_SSID = "Wokwi-GUEST";   
const char* WIFI_PASS = "";

const char* MQTT_SERVER = "broker.hivemq.com";
const int   MQTT_PORT   = 1883;
#define TOPIC_PREFIX "runner_demo_2026/"   

// Topic ESP32 gui len (Web nhan)
const char* TOPIC_TELEMETRY = TOPIC_PREFIX "telemetry";   
const char* TOPIC_TEMP      = TOPIC_PREFIX "temp";
const char* TOPIC_PRESSURE  = TOPIC_PREFIX "pressure";
const char* TOPIC_ALTITUDE  = TOPIC_PREFIX "altitude";
const char* TOPIC_ALARM     = TOPIC_PREFIX "alarm";
const char* TOPIC_LED_STATE = TOPIC_PREFIX "led/state";
const char* TOPIC_BUZ_STATE = TOPIC_PREFIX "buzzer/state";
const char* TOPIC_MODE      = TOPIC_PREFIX "mode";

// Topic ESP32 nhan lenh (Web gui)
const char* TOPIC_LED    = TOPIC_PREFIX "cmd/led";
const char* TOPIC_BUZZER = TOPIC_PREFIX "cmd/buzzer";
// =================================================
// 4. KHOI TAO DOI TUONG
// =================================================

Adafruit_BMP085 bmp;

WiFiClient espClient;
PubSubClient mqtt(espClient);

// =================================================
// 5. CAU TRUC DU LIEU
// =================================================

struct SensorData {
    float temperature;
    float pressure;
    float altitude;
};

struct AlertData {
    float temperature;
    float altitudeChange;
};

// =================================================
// 6. KHAI BAO QUEUE VA MUTEX
// =================================================

QueueHandle_t dataQueue;
QueueHandle_t alertQueue;

SemaphoreHandle_t stateMutex;

// =================================================
// 7. BIEN TRANG THAI
// =================================================

// Trang thai dieu khien bang Web
bool manualLED = false;
bool manualBuzzer = false;

// Trang thai canh bao tu dong
bool alertLED = false;
bool alertBuzzer = false;

// Ten canh bao gui len Web: NORMAL / TEMP_HIGH / TEMP_LOW / ALT_JUMP
const char* alarmName = "NORMAL";

// =================================================
// 8. NGUONG CANH BAO
// =================================================

#define TEMP_MAX 35.0
#define TEMP_MIN 10.0

#define ALT_CHANGE_LIMIT 5.0

// =================================================
// 9. HAM CAP NHAT LED VA BUZZER
// =================================================

void updateOutputs() {

    bool ledState;
    bool buzzerState;

    // Doc trang thai dung chung
    xSemaphoreTake(stateMutex, portMAX_DELAY);

    ledState = manualLED || alertLED;
    buzzerState = manualBuzzer || alertBuzzer;

    xSemaphoreGive(stateMutex);

    // Dieu khien thiet bi
    digitalWrite(LED_PIN, ledState ? HIGH : LOW);

    digitalWrite(BUZZER_PIN, buzzerState ? HIGH : LOW);
}

// =================================================
// 10. NHAN LENH TU WEBSITE
// =================================================

void mqttCallback(char* topic, byte* payload, unsigned int length) {

    String message = "";

    for (unsigned int i = 0; i < length; i++) {
        message += (char)payload[i];
    }

    message.trim();

    xSemaphoreTake(stateMutex, portMAX_DELAY);

    if (String(topic) == TOPIC_LED) {

        if (message == "ON") {
            manualLED = true;
        }
        else if (message == "OFF") {
            manualLED = false;
        }
    }

    else if (String(topic) == TOPIC_BUZZER) {

        if (message == "ON") {
            manualBuzzer = true;
        }
        else if (message == "OFF") {
            manualBuzzer = false;
        }
    }

    xSemaphoreGive(stateMutex);

    updateOutputs();

    Serial.print("MQTT Command: ");
    Serial.print(topic);
    Serial.print(" -> ");
    Serial.println(message);
}

// =================================================
// 11. TASK TRUYEN DU LIEU MQTT
// CORE 0 - PRIORITY 1
// =================================================

void Task_Transmit(void *parameter) {

    // Ket noi WiFi
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    Serial.println("Dang ket noi WiFi...");

    while (WiFi.status() != WL_CONNECTED) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    Serial.println("Da ket noi WiFi!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    // Cau hinh MQTT
    mqtt.setServer(MQTT_SERVER, MQTT_PORT);
    mqtt.setCallback(mqttCallback);
    mqtt.setBufferSize(512);
    mqtt.setKeepAlive(30);

    SensorData data;

    while (1) {

        // Kiem tra WiFi
        if (WiFi.status() != WL_CONNECTED) {

            WiFi.reconnect();

            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // Ket noi MQTT
        if (!mqtt.connected()) {

            Serial.println("Dang ket noi MQTT...");

            String clientID =
                "ESP32_Runner_" +
                String((uint32_t)ESP.getEfuseMac(), HEX);

            if (mqtt.connect(clientID.c_str())) {

                Serial.println("Da ket noi MQTT!");

                mqtt.subscribe(TOPIC_LED);
                mqtt.subscribe(TOPIC_BUZZER);
            }
            else {

                Serial.print("MQTT loi: ");
                Serial.println(mqtt.state());

                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
        }

        // Duy tri ket noi MQTT
        mqtt.loop();

        // Nhan du lieu tu Queue
        if (xQueueReceive(
                dataQueue,
                &data,
                pdMS_TO_TICKS(100)
            ) == pdTRUE) {

            // Tao chuoi JSON
            char json[200];

            snprintf(
                json,
                sizeof(json),
                "{\"temperature\":%.2f,"
                "\"pressure\":%.2f,"
                "\"altitude\":%.2f}",
                data.temperature,
                data.pressure,
                data.altitude
            );

            // Gui du lieu len MQTT
            mqtt.publish(TOPIC_TELEMETRY, json);

            Serial.print("MQTT Publish: ");
            Serial.println(json);

            // ---- Gui cho Web: moi gia tri 1 topic ----
            char text[16];

            snprintf(text, sizeof(text), "%.1f", data.temperature);
            mqtt.publish(TOPIC_TEMP, text);

            snprintf(text, sizeof(text), "%.2f", data.pressure);
            mqtt.publish(TOPIC_PRESSURE, text);

            snprintf(text, sizeof(text), "%.1f", data.altitude);
            mqtt.publish(TOPIC_ALTITUDE, text);

            bool led, buzzer, manual;
            const char* alarm;

            xSemaphoreTake(stateMutex, portMAX_DELAY);
            led    = manualLED || alertLED;
            buzzer = manualBuzzer || alertBuzzer;
            manual = manualLED || manualBuzzer;
            alarm  = alarmName;
            xSemaphoreGive(stateMutex);

            mqtt.publish(TOPIC_ALARM,     alarm);
            mqtt.publish(TOPIC_LED_STATE, led    ? "ON" : "OFF");
            mqtt.publish(TOPIC_BUZ_STATE, buzzer ? "ON" : "OFF");
            mqtt.publish(TOPIC_MODE,      manual ? "MANUAL" : "AUTO");
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// =================================================
// 12. TASK DOC CAM BIEN BMP180
// CORE 1 - PRIORITY 2
// =================================================

void Task_ReadSensor(void *parameter) {

    float previousAltitude = 0;
    bool firstReading = true;

    while (1) {

        // Doc nhiet do
        float temperature = bmp.readTemperature();

        // Doc ap suat (hPa)
        float pressure = bmp.readPressure() / 100.0;

        // Tinh do cao (m)
        float altitude = bmp.readAltitude(101325);

        // Kiem tra du lieu cam bien
        if (!isfinite(temperature) ||
            !isfinite(pressure) ||
            !isfinite(altitude)) {

            Serial.println("Loi doc BMP180!");

            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        // Tinh thay doi do cao
        float altitudeChange = 0;

        if (!firstReading) {
            altitudeChange = fabsf(
                altitude - previousAltitude
            );
        }

        previousAltitude = altitude;
        firstReading = false;

        // Tao cau truc du lieu
        SensorData sensor;

        sensor.temperature = temperature;
        sensor.pressure = pressure;
        sensor.altitude = altitude;

        // Gui du lieu cam bien vao Queue
        xQueueOverwrite(dataQueue, &sensor);

        // Tao du lieu canh bao
        AlertData alert;

        alert.temperature = temperature;
        alert.altitudeChange = altitudeChange;

        // Gui du lieu vao Queue canh bao
        xQueueOverwrite(alertQueue, &alert);

        // Hien thi Serial Monitor
        Serial.println("-----------------------");

        Serial.print("Nhiet do: ");
        Serial.print(temperature);
        Serial.println(" *C");

        Serial.print("Ap suat: ");
        Serial.print(pressure);
        Serial.println(" hPa");

        Serial.print("Do cao: ");
        Serial.print(altitude);
        Serial.println(" m");

        Serial.print("Thay doi do cao: ");
        Serial.print(altitudeChange);
        Serial.println(" m");

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

// =================================================
// 13. TASK CANH BAO
// CORE 1 - PRIORITY 3
// =================================================

void Task_Alert(void *parameter) {

    AlertData alert;

    while (1) {

        // Cho du lieu tu Queue
        if (xQueueReceive(
                alertQueue,
                &alert,
                portMAX_DELAY
            ) == pdTRUE) {

            bool temperatureWarning = false;
            bool altitudeWarning = false;

            // Kiem tra nhiet do
            if (alert.temperature > TEMP_MAX ||
                alert.temperature < TEMP_MIN) {

                temperatureWarning = true;
            }

            // Kiem tra thay doi do cao
            if (alert.altitudeChange >= ALT_CHANGE_LIMIT) {

                altitudeWarning = true;
            }

            // Cap nhat trang thai canh bao
            xSemaphoreTake(stateMutex, portMAX_DELAY);

            alertLED = temperatureWarning || altitudeWarning;

            alertBuzzer = temperatureWarning || altitudeWarning;

            if (altitudeWarning)                         alarmName = "ALT_JUMP";
            else if (alert.temperature > TEMP_MAX)       alarmName = "TEMP_HIGH";
            else if (alert.temperature < TEMP_MIN)       alarmName = "TEMP_LOW";
            else                                         alarmName = "NORMAL";

            xSemaphoreGive(stateMutex);

            // Cap nhat LED va coi
            updateOutputs();

            // Hien thi canh bao
            if (temperatureWarning) {
                Serial.println("CANH BAO: NHIET DO BAT THUONG!");
            }

            if (altitudeWarning) {
                Serial.println("CANH BAO: DO CAO THAY DOI DOT NGOT!");
            }

            if (!temperatureWarning && !altitudeWarning) {
                Serial.println("Trang thai: BINH THUONG");
            }
        }
    }
}

// =================================================
// 14. SETUP
// =================================================

void setup() {

    Serial.begin(115200);

    // Cau hinh GPIO
    pinMode(LED_PIN, OUTPUT);
    pinMode(BUZZER_PIN, OUTPUT);

    digitalWrite(LED_PIN, LOW);
    digitalWrite(BUZZER_PIN, LOW);

    // Khoi tao I2C
    Wire.begin(SDA_PIN, SCL_PIN);

    // Khoi tao BMP180
    if (!bmp.begin()) {

        Serial.println("Khong tim thay BMP180!");

        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    Serial.println("Da ket noi BMP180!");

    // Tao Queue
    dataQueue = xQueueCreate(1, sizeof(SensorData));

    alertQueue = xQueueCreate(1, sizeof(AlertData));

    // Tao Mutex
    stateMutex = xSemaphoreCreateMutex();

    // Kiem tra khoi tao
    if (dataQueue == NULL ||
        alertQueue == NULL ||
        stateMutex == NULL) {

        Serial.println("Loi khoi tao Queue/Mutex!");

        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    // Tao Task truyen du lieu - Core 0
    BaseType_t task1 = xTaskCreatePinnedToCore(
        Task_Transmit,
        "Task_Transmit",
        8192,
        NULL,
        1,
        NULL,
        0
    );

    // Tao Task doc cam bien - Core 1
    BaseType_t task2 = xTaskCreatePinnedToCore(
        Task_ReadSensor,
        "Task_ReadSensor",
        4096,
        NULL,
        2,
        NULL,
        1
    );

    // Tao Task canh bao - Core 1
    BaseType_t task3 = xTaskCreatePinnedToCore(
        Task_Alert,
        "Task_Alert",
        4096,
        NULL,
        3,
        NULL,
        1
    );

    if (task1 != pdPASS ||
        task2 != pdPASS ||
        task3 != pdPASS) {

        Serial.println("Loi tao FreeRTOS Task!");

        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    Serial.println("He thong Runner Monitor da san sang!");
}

// =================================================
// 15. LOOP
// =================================================

void loop() {

    vTaskDelay(portMAX_DELAY);
}

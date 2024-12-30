#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <SocketIOclient.h>
#include <Hash.h>

SocketIOclient socketIO;

#define USE_SERIAL Serial

// Configuration
const char* chipId = "CHIP123";
#define PIR_SENSOR_PIN D1
#define MOTOR_PIN D4

// State variables
bool isRegistered = false;
volatile bool pirSensorStatus = false;
bool motorStatus = false;

// Timing variables
volatile unsigned long lastMovementTime = 0;
const unsigned long MOTION_TIMEOUT = 60000;  // 60 seconds (1 minute)
const unsigned long STATUS_INTERVAL = 5000;   // 5 seconds
const unsigned long HEARTBEAT_INTERVAL = 30000; // 30 seconds
unsigned long lastStatusUpdate = 0;
unsigned long lastHeartbeat = 0;

// Connection management
const uint8_t MAX_RECONNECT_ATTEMPTS = 5;
uint8_t reconnectAttempts = 0;
bool needsReconnect = false;

//register
const unsigned long REGISTER_RETRY_INTERVAL = 5000;  // 5 seconds between registration attempts
unsigned long lastRegistrationAttempt = 0;

void sendEvent(const String& eventName, JsonObject& data) {
    if (!socketIO.isConnected()) {
        USE_SERIAL.println("Socket not connected, queuing event");
        return;
    }

    DynamicJsonDocument doc(512);
    JsonArray array = doc.to<JsonArray>();
    array.add(eventName);
    
    // Standardize status field names
    if (eventName == "sensorResponse") {
        data["relayStatus"] = motorStatus;
        data["pirStatus"] = pirSensorStatus;
    }
    
    array.add(data);

    String output;
    serializeJson(doc, output);
    socketIO.sendEVENT(output);
    USE_SERIAL.println("Sent: " + output);
}

void handleSocketEvent(const String& event, JsonObject& data) {
    DynamicJsonDocument responseDoc(512);
    JsonObject responseData = responseDoc.to<JsonObject>();
    responseData["chipId"] = chipId;

    if (event == "registerConfirm") {
         if (!isRegistered) {  // Only log and update if not already registered
            isRegistered = true;
            USE_SERIAL.println("Registration confirmed successfully");
            lastRegistrationAttempt = 0;  // Reset the registration attempt timer
        }
        
    } else if (event == "sensorRequest") {
        String action = data["action"].as<String>();
        
        if (action == "turnOn") {
            digitalWrite(MOTOR_PIN, HIGH);
            motorStatus = true;
        } else if (action == "turnOff") {
            digitalWrite(MOTOR_PIN, LOW);
            motorStatus = false;
        }
        
        responseData["relayStatus"] = motorStatus;
        responseData["pirStatus"] = pirSensorStatus;
        sendEvent("sensorResponse", responseData);
    }
}

void IRAM_ATTR onMotionDetected() {
    static unsigned long lastInterruptTime = 0;
    unsigned long interruptTime = millis();
    
    // Improved debounce with minimum 200ms between interrupts
    if (interruptTime - lastInterruptTime > 200) {
        pirSensorStatus = true;
        lastMovementTime = interruptTime;
        lastInterruptTime = interruptTime;
    }
}

void socketIOEvent(socketIOmessageType_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case sIOtype_DISCONNECT:
            isRegistered = false;
            USE_SERIAL.println("Disconnected");
            needsReconnect = true;
            break;

        case sIOtype_CONNECT:
            USE_SERIAL.println("Connected");
            socketIO.send(sIOtype_CONNECT, "/");
            reconnectAttempts = 0;
            break;

        case sIOtype_EVENT: {
            DynamicJsonDocument doc(512);
            DeserializationError error = deserializeJson(doc, payload, length);
            if (!error) {
                String eventName = doc[0];
                JsonObject data = doc[1];
                handleSocketEvent(eventName, data);
            } else {
                USE_SERIAL.println("JSON parsing failed");
            }
            break;
        }
        
        default:
            break;
    }
}

void handleReconnection() {
    if (!needsReconnect) return;
    
    if (reconnectAttempts < MAX_RECONNECT_ATTEMPTS) {
        USE_SERIAL.printf("Reconnection attempt %d/%d\n", reconnectAttempts + 1, MAX_RECONNECT_ATTEMPTS);
        reconnectAttempts++;
        socketIO.begin("192.168.1.6", 3000, "/socket.io/?EIO=4");
        needsReconnect = false;
    } else {
        USE_SERIAL.println("Max reconnection attempts reached. Restarting...");
        delay(1000);  // Brief delay before restart
        ESP.restart();
    }
}

void setup() {
    USE_SERIAL.begin(115200);
    USE_SERIAL.println("\nStarting up...");
    
    // Configure pins
    pinMode(PIR_SENSOR_PIN, INPUT);
    pinMode(MOTOR_PIN, OUTPUT);
    digitalWrite(MOTOR_PIN, LOW);
    
    attachInterrupt(digitalPinToInterrupt(PIR_SENSOR_PIN), onMotionDetected, RISING);

    // WiFi setup
    WiFiManager wifiManager;
    wifiManager.setConfigPortalTimeout(180); // 3 minute timeout
    
    if (!wifiManager.autoConnect("acMotionAP")) {
        USE_SERIAL.println("Failed to connect. Restarting...");
        delay(3000);
        ESP.restart();
    }

    USE_SERIAL.printf("Connected to WiFi. IP: %s\n", WiFi.localIP().toString().c_str());
    
    // socketIO.begin("192.168.1.6", 3000, "/socket.io/?EIO=4");
    socketIO.beginSSL("iti-dodamarg-rac.onrender.com", 443, "/socket.io/?EIO=4");
    socketIO.onEvent(socketIOEvent);
}

void loop() {
    socketIO.loop();
    unsigned long now = millis();

    // Handle reconnection if needed
    if (needsReconnect) {
        handleReconnection();
        return;
    }

    // Register device if needed
    if (!isRegistered && socketIO.isConnected()) {
        if (now - lastRegistrationAttempt >= REGISTER_RETRY_INTERVAL) {
            lastRegistrationAttempt = now;
            DynamicJsonDocument doc(512);
            JsonObject data = doc.to<JsonObject>();
            data["chipId"] = chipId;
            sendEvent("deviceRegister", data);
            USE_SERIAL.println("Attempting device registration...");
        }
    }

    // Check motion timeout
    if (pirSensorStatus && (now - lastMovementTime > MOTION_TIMEOUT)) {
        pirSensorStatus = false;
        digitalWrite(MOTOR_PIN, LOW);
        motorStatus = false;
        USE_SERIAL.println("Motion timeout - turning off");
    }

    // Send regular status updates
    if (isRegistered && (now - lastStatusUpdate >= STATUS_INTERVAL)) {
        lastStatusUpdate = now;
        DynamicJsonDocument doc(512);
        JsonObject data = doc.to<JsonObject>();
        data["chipId"] = chipId;
        sendEvent("sensorResponse", data);
    }

    // Send heartbeat
    if (isRegistered && (now - lastHeartbeat >= HEARTBEAT_INTERVAL)) {
        lastHeartbeat = now;
        DynamicJsonDocument doc(512);
        JsonObject data = doc.to<JsonObject>();
        data["chipId"] = chipId;
        sendEvent("heartbeat", data);
    }
}
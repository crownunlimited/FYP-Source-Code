#include <SoftwareSerial.h>

#define PIN_REED 3
#define PIN_BTN_ARM 4
#define PIN_LED_GREEN 5
#define PIN_LED_RED 6
#define PIN_BUZZER 7
#define PIN_GSM_TX 8
#define PIN_GSM_RX 9
#define PIN_VIB 10
#define ARM_DELAY_MS 5000UL
#define GATE_SECONDS 30UL
#define DEBOUNCE_MS 50
#define PHONE_NUMBER "+2347045018401"

enum SystemState {
    STATE_IDLE, STATE_SENSING, STATE_ARMED, STATE_VIBRATION_DETECTED, STATE_BUZZER_ACTIVE, STATE_SMS_SENDING,STATE_SLEEP
};
void setState(SystemState &state, SystemState next) {
    state = next;
    Serial.print(F("[STATE] "));
    switch (next) {
        case STATE_IDLE: Serial.println(F("IDLE")); break;
        case STATE_SENSING: Serial.println(F("SENSING")); break;
        case STATE_ARMED: Serial.println(F("ARMED")); break;
        case STATE_VIBRATION_DETECTED:Serial.println(F("VIBRATION_DETECTED")); break;
        case STATE_BUZZER_ACTIVE: Serial.println(F("BUZZER_ACTIVE")); break;
        case STATE_SMS_SENDING: Serial.println(F("SMS_SENDING")); break;
        case STATE_SLEEP:Serial.println(F("SLEEP")); break;
    }
}
class SensorReader {
    private:
    bool isArmed;
    int reedSwitchPin;
    int vibrationPin;
    bool prevReedState;
    bool prevVibState;
public:
    SensorReader(int reedPin, int vibPin) {
        reedSwitchPin = reedPin;
        vibrationPin = vibPin;
        isArmed = false;
    }
    void begin() {
        pinMode(reedSwitchPin, INPUT_PULLUP);
        pinMode(vibrationPin, INPUT);
        prevReedState = digitalRead(reedSwitchPin);
        prevVibState = digitalRead(vibrationPin);
    }
    bool readReedSwitch() {
        bool now = digitalRead(reedSwitchPin);
        bool edge = (now == HIGH && prevReedState == LOW);
        prevReedState = now;
        return edge;
    }
    float readVibration() {
        bool now = digitalRead(vibrationPin);
        bool edge = (now == HIGH && prevVibState == LOW);
        prevVibState = now;
        return edge ? 1.0 : 0.0;
    }
    bool getArmedState() {
        return isArmed;
    }
    void setArmedState(bool state) {
        isArmed = state;
    }
};
class SMS_Manager {
    private:
    String phoneNumber;
    String gsmStatus;
    SoftwareSerial gsmSerial;
    bool gsmReady;
    unsigned long nextAttemptAt;
    bool sendAT(const String &cmd, const String &expected, unsigned long timeoutMs) {
        while (gsmSerial.available()) gsmSerial.read();
        gsmSerial.println(cmd);
        unsigned long start = millis();
        String resp = "";
        while (millis() - start < timeoutMs) {
            while (gsmSerial.available()) {
                resp += (char)gsmSerial.read();
                if (resp.indexOf(expected) != -1) return true;
            }
        }
        Serial.print(F("GSM: no/unexpected reply to '"));
        Serial.print(cmd);
        Serial.print(F("': "));
        Serial.println(resp);
        return false;
    }
public:
     SMS_Manager(int rxPin, int txPin, String phone): gsmSerial(rxPin, txPin) {
        phoneNumber = phone;
        gsmStatus = "UNINITIALIZED";
        gsmReady = false;
        nextAttemptAt = 0;
    }
    void begin() {
        gsmSerial.begin(1200);
        nextAttemptAt = millis() + 3000;
    }
    void pollInit() {
        if (gsmReady) return;
        if (millis() < nextAttemptAt) return;
        bool alive = sendAT("AT", "OK", 1500);
        if (alive) {
            sendAT("ATE0", "OK", 1500);
            sendAT("AT+CMGF=1", "OK", 1500);
            gsmReady = true;
            gsmStatus = "READY";
            Serial.println(F("GSM ready."));
        } else {
            gsmStatus = "NO_RESPONSE";
            nextAttemptAt = millis() + 4000;
        }
    }
    bool isReady() {
        return gsmReady;
    }
    void sendAlert(String message) {
        while (gsmSerial.available()) gsmSerial.read();
        gsmSerial.print(F("AT+CMGS=\""));
        gsmSerial.print(phoneNumber);
        gsmSerial.println(F("\""));
        delay(200);
        gsmSerial.print(message);
        delay(200);
        gsmSerial.write(26);
        unsigned long start = millis();
        String resp = "";
        while (millis() - start < 15000) {
            while (gsmSerial.available()) resp += (char)gsmSerial.read();
            if (resp.indexOf("+CMGS") != -1 ||resp.indexOf("ERROR") != -1) break;
        }
        if (resp.indexOf("+CMGS") != -1) {
            gsmStatus = "SENT";
            Serial.println(F("SMS sent."));
        } else {
            gsmStatus = "FAILED";
            Serial.print(F("SMS send failed. Raw reply: "));
            Serial.println(resp);
        }
    }
};
class BuzzerManager {
    private:
    bool timerActive;
    int countdownSec;
    int buzzerPin;
    int redLedPin;
    SMS_Manager* smsManager;
    SystemState* state;
    String alertSource;
    unsigned long startMillis;
    unsigned long lastBlinkMillis;
    unsigned long lastPrintMillis;
    bool blinkState;
    bool justDispatched;
public:
    BuzzerManager(int buzzer, int redLed, int gateSeconds,SMS_Manager* sms, SystemState* sys) {
        buzzerPin = buzzer;
        redLedPin = redLed;
        countdownSec = gateSeconds;
        smsManager = sms;
        state = sys;
        timerActive = false;
        justDispatched = false;
    }
    void begin() {
        pinMode(buzzerPin, OUTPUT);
        pinMode(redLedPin, OUTPUT);
        digitalWrite(buzzerPin, LOW);
        digitalWrite(redLedPin, LOW);
    }
    void activate(String source) {
        if (timerActive) return;
        alertSource = source;
        timerActive = true;
        startMillis = millis();
        lastBlinkMillis = millis();
        lastPrintMillis = millis();
        blinkState = true;
        digitalWrite(buzzerPin, HIGH);
        digitalWrite(redLedPin, HIGH);
        setState(*state, STATE_BUZZER_ACTIVE);
        Serial.print(F("Alert triggered - source: "));
        Serial.println(source);
    }
    void cancel() {
        if (!timerActive) return;
        timerActive = false;
        digitalWrite(buzzerPin, LOW);
        digitalWrite(redLedPin, LOW);
        Serial.println(F("Alert cancelled."));
    }
    void onTimeout() {
        if (!timerActive) return;
        if (millis() - lastBlinkMillis >= 500) {
            blinkState = !blinkState;
            digitalWrite(redLedPin, blinkState ? HIGH : LOW);
            lastBlinkMillis = millis();
        }
        unsigned long elapsed = millis() - startMillis;
        if (millis() - lastPrintMillis >= 5000) {
            lastPrintMillis = millis();
            Serial.print(F("Gate: "));
            Serial.print(elapsed / 1000UL);
            Serial.print(F("s of "));
            Serial.print(countdownSec);
            Serial.println(F("s."));
        }
        if (elapsed >= (unsigned long)countdownSec * 1000UL) {
            digitalWrite(buzzerPin, LOW);
            digitalWrite(redLedPin, HIGH);
            setState(*state, STATE_SMS_SENDING);
            String msg = "INTRUSION ALERT: " + alertSource + " sensor triggered. Please check the premises.";
            smsManager->sendAlert(msg);
            digitalWrite(redLedPin, LOW);
            timerActive = false;
            justDispatched = true;
            Serial.println(F("SMS process completed."));
        }
    }
    bool isActive() {
        return timerActive;
    }
    bool consumeDispatchFlag() {
        if (justDispatched) {
            justDispatched = false;
            return true;
        }
        return false;
    }
};
class SignalProcessor {
    private:
    BuzzerManager* buzzerManager;
    SystemState* state;
public:
    SignalProcessor(BuzzerManager* buzzer, SystemState* sys) {
        buzzerManager = buzzer;
        state = sys;
    }
    void processFrameDisplacement(bool reedEvent, bool armed) {
        if (!reedEvent) return;
        if (!armed) {
            Serial.println(F("Reed event ignored - system not armed."));
            return;
        }
        if (buzzerManager->isActive()) return;
        startBuzzerWindow("REED");
    }
    void processVibration(float vibReading, bool armed) {
        if (vibReading <= 0.0) return;
        if (!armed) {
            Serial.println(F("Vibration event ignored - system not armed."));
            return;
        }
        if (buzzerManager->isActive()) return;
        setState(*state, STATE_VIBRATION_DETECTED);
        startBuzzerWindow("VIBRATION");
    }
    void startBuzzerWindow(String source) {
        buzzerManager->activate(source);
    }
};
class ButtonHandler {
    private:
    int armButtonPin;
    int greenLedPin;
    bool prevArmState;
    bool armPending;
    unsigned long armPendingUntil;
    SensorReader* sensorReader;
    BuzzerManager* buzzerManager;
    SystemState* state;
    void applyArmedState(bool armed) {
        sensorReader->setArmedState(armed);
        digitalWrite(greenLedPin, armed ? HIGH : LOW);
        setState( *state,armed ? STATE_ARMED : STATE_SENSING );
        Serial.println( armed ? F("System armed.") : F("System disarmed.") );
    }
public:
     ButtonHandler(int armPin, int greenLed, SensorReader* sensor, BuzzerManager* buzzer,SystemState* sys) {
        armButtonPin = armPin;
        greenLedPin = greenLed;
        sensorReader = sensor;
        buzzerManager = buzzer;
        state = sys;
        armPending = false;
    }
    void begin() {
        pinMode(armButtonPin, INPUT_PULLUP);
        pinMode(greenLedPin, OUTPUT);
        digitalWrite(greenLedPin, LOW);
        prevArmState = digitalRead(armButtonPin);
    }
    bool debounce(int pin) {
        delay(DEBOUNCE_MS);
        return digitalRead(pin) == LOW;
    }
    void startArmDelay() {
        sensorReader->setArmedState(false);
        digitalWrite(greenLedPin, LOW);
        armPending = true;
        armPendingUntil = millis() + ARM_DELAY_MS;
        setState(*state, STATE_SENSING);
        Serial.println(F("System will arm in 5s..."));
    }
    void onArmingButtonPress() {
        bool now = digitalRead(armButtonPin);
        bool pressed = (now == LOW && prevArmState == HIGH);
        prevArmState = now;
        if (!pressed || !debounce(armButtonPin)) return;
        if (buzzerManager->isActive()) {
            buzzerManager->cancel();
            startArmDelay();
            return;
        }
        if (armPending) {
            armPending = false;
            sensorReader->setArmedState(false);
            digitalWrite(greenLedPin, LOW);
            Serial.println(F("Arming cancelled."));
            setState(*state, STATE_SENSING);
            return;
        }
        if (sensorReader->getArmedState()) {
            applyArmedState(false);
            return;
        }
        startArmDelay();
    }
    void update() {
        if (armPending && millis() >= armPendingUntil) {
            armPending = false;
            applyArmedState(true);
        }
    }
};
class IntrusionSystem {
    private:
    SystemState state;
    SensorReader sensorReader;
    SMS_Manager smsManager;
    BuzzerManager buzzerManager;
    SignalProcessor signalProcessor;
    ButtonHandler buttonHandler;
public:
     IntrusionSystem() : state(STATE_IDLE),sensorReader(PIN_REED, PIN_VIB), smsManager(PIN_GSM_RX, PIN_GSM_TX, PHONE_NUMBER),buzzerManager(PIN_BUZZER, PIN_LED_RED, GATE_SECONDS, &smsManager, &state),signalProcessor(&buzzerManager, &state), buttonHandler(PIN_BTN_ARM, PIN_LED_GREEN,&sensorReader, &buzzerManager, &state) {
    }
    void init() {
        Serial.begin(1200);
        sensorReader.begin();
        buttonHandler.begin();
        buzzerManager.begin();
        smsManager.begin();
        for (uint8_t i = 0; i < 3; i++) {
            digitalWrite(PIN_LED_GREEN, HIGH);
            digitalWrite(PIN_LED_RED, HIGH);
            delay(200);
            digitalWrite(PIN_LED_GREEN, LOW);
            digitalWrite(PIN_LED_RED, LOW);
            delay(200);
        }
        setState(state, STATE_SENSING);
        Serial.println( F("Ready. Press ARM/DISARM to activate sensors.") );
    }
    void run() {
        buttonHandler.onArmingButtonPress();
        buttonHandler.update();
        smsManager.pollInit();
        bool armed = sensorReader.getArmedState();
        bool reedEvent = sensorReader.readReedSwitch();
        float vibReading =sensorReader.readVibration();
        if (!buzzerManager.isActive()) {
            signalProcessor.processFrameDisplacement( reedEvent, armed );
        }
        if (!buzzerManager.isActive()) {
            signalProcessor.processVibration( vibReading, armed );
        }
        buzzerManager.onTimeout();
        if (buzzerManager.consumeDispatchFlag()) {
            buttonHandler.startArmDelay();
        }
    }
};
IntrusionSystem system_;
void setup() {
    system_.init();
}
void loop() {
    system_.run();
}

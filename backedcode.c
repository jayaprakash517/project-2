// --- LIBRARIES ---
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Servo.h>
#include <DHT.h>
#include <SoftwareSerial.h>

// --- SENSOR & ACTUATOR PINS ---
#define DHT_PIN 2
#define SOIL_SENSOR_PIN A0
#define SOUND_SENSOR_PIN 7
#define FAN_RELAY_PIN 3
#define SERVO_PIN 5
#define BUZZER_PIN 4
#define GSM_RX_PIN 10
#define GSM_TX_PIN 11

// --- CONSTANTS ---
#define DHT_TYPE DHT11
#define LCD_ADDRESS 0x27
#define TEMPERATURE_THRESHOLD 30.0  // Temp in Celsius to turn on the fan
#define WETNESS_THRESHOLD 500       // Analog value from soil sensor indicating wetness
#define CRADLE_SWING_SPEED 30       // Milliseconds between servo position updates
#define CRADLE_POS_REST 60
#define CRADLE_POS_MIN 30
#define CRADLE_POS_MAX 90
const char* PARENT_PHONE_NUMBER = "+917416640739"; // Phone number for SMS alerts

// --- OBJECT INITIALIZATION ---
DHT dht(DHT_PIN, DHT_TYPE);
Servo cradleServo;
LiquidCrystal_I2C lcd(LCD_ADDRESS, 16, 2);
SoftwareSerial gsm(GSM_RX_PIN, GSM_TX_PIN);

// --- GLOBAL VARIABLES for State Management ---
unsigned long lastSensorReadTime = 0;
unsigned long lastLcdUpdateTime = 0;

// Diaper alert state
bool isDiaperAlertActive = false;
unsigned long diaperAlertStartTime = 0;

// Cradle swing state
enum CradleState { IDLE, SWINGING_FORWARD, SWINGING_BACK, RETURNING };
CradleState cradleState = IDLE;
unsigned long lastSwingTime = 0;
int swingCycles = 0;

// Buzzer state
bool isBuzzerActive = false;
unsigned long buzzerPatternStartTime = 0;
int beepCount = 0;
enum BuzzerMode { OFF, CRY_ALERT, WET_ALERT };
BuzzerMode buzzerMode = OFF;

// --- SETUP FUNCTION ---
void setup() {
    Serial.begin(9600);
    gsm.begin(9600);
    dht.begin();

    lcd.begin(16, 2);
    lcd.backlight();
    lcd.setCursor(0, 0);
    lcd.print(" Baby Monitor ");

    pinMode(FAN_RELAY_PIN, OUTPUT);
    digitalWrite(FAN_RELAY_PIN, HIGH); // Fan OFF initially

    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);

    cradleServo.attach(SERVO_PIN);
    cradleServo.write(CRADLE_POS_REST);

    pinMode(SOIL_SENSOR_PIN, INPUT);
    pinMode(SOUND_SENSOR_PIN, INPUT);

    delay(2000); // Initial delay for system to stabilize
    lcd.clear();

    initializeGSM(); 
    Serial.println("System Ready.");
}

// --- MAIN LOOP ---
void loop() {
    unsigned long currentTime = millis();

    // Read sensors periodically
    if (currentTime - lastSensorReadTime >= 500) {
        lastSensorReadTime = currentTime;

        float temperature = dht.readTemperature();
        int soilValue = analogRead(SOIL_SENSOR_PIN);
        int soundValue = digitalRead(SOUND_SENSOR_PIN);

        // Only proceed if temperature reading is valid
        if (!isnan(temperature)) {
            handleTemperature(temperature);
            handleCry(soundValue);
            handleUrine(soilValue);
            updateLCD(temperature);
        }
    }
    
    // Update ongoing actions
    manageCradleSwing(currentTime);
    manageBuzzer(currentTime);
    manageDiaperAlertMessage(currentTime);
}

// --- CORE LOGIC FUNCTIONS ---
void handleTemperature(float temperature) {
    if (temperature > TEMPERATURE_THRESHOLD) {
        digitalWrite(FAN_RELAY_PIN, LOW); // Turn fan ON
    } else {
        digitalWrite(FAN_RELAY_PIN, HIGH); // Turn fan OFF
    }
}

void handleCry(int soundValue) {
    // If baby is crying AND cradle is not already swinging
    if (soundValue == LOW && cradleState == IDLE) {
        Serial.println("Baby Crying! Starting cradle and alert.");
        cradleState = SWINGING_FORWARD; // Start the swing cycle
        swingCycles = 3; // Swing for 3 full cycles
        startBuzzer(CRY_ALERT, 3); // Start fast beeping
        sendSMS(PARENT_PHONE_NUMBER, "Alert: Baby is Crying!");
    }
}

void handleUrine(int soilValue) {
    // If diaper is wet AND an alert is not already active
    if (soilValue < WETNESS_THRESHOLD && !isDiaperAlertActive) {
        Serial.println("Baby Urinated! Starting alert.");
        isDiaperAlertActive = true;
        diaperAlertStartTime = millis();
        startBuzzer(WET_ALERT, 3); // Start slow beeping
        sendSMS(PARENT_PHONE_NUMBER, "Alert: Diaper is wet. Please check.");
    }
}

// --- ACTION MANAGEMENT FUNCTIONS (Non-Blocking) ---

void manageCradleSwing(unsigned long currentTime) {
    if (cradleState == IDLE || currentTime - lastSwingTime < CRADLE_SWING_SPEED) {
        return; // Not swinging or not time to move yet
    }
    lastSwingTime = currentTime;

    int currentPos = cradleServo.read();

    switch (cradleState) {
        case SWINGING_FORWARD:
            if (currentPos < CRADLE_POS_MAX) {
                cradleServo.write(currentPos + 1);
            } else {
                cradleState = SWINGING_BACK; // Reached max, swing back
            }
            break;

        case SWINGING_BACK:
            if (currentPos > CRADLE_POS_MIN) {
                cradleServo.write(currentPos - 1);
            } else {
                // Completed one part of the cycle, check if more cycles are needed
                swingCycles--;
                if (swingCycles > 0) {
                    cradleState = SWINGING_FORWARD;
                } else {
                    cradleState = RETURNING; // All cycles done, return to rest
                }
            }
            break;
            
        case RETURNING:
            if (currentPos < CRADLE_POS_REST) {
                cradleServo.write(currentPos + 1);
            } else if (currentPos > CRADLE_POS_REST) {
                cradleServo.write(currentPos - 1);
            } else {
                cradleState = IDLE; // Returned to rest position
                Serial.println("Cradle stopped.");
                lcd.setCursor(0, 1);
                lcd.print("                "); // Clear cradle message line
            }
            break;
    }
}

void startBuzzer(BuzzerMode mode, int count) {
    if (isBuzzerActive) return; // Don't start a new pattern if one is running
    buzzerMode = mode;
    beepCount = count * 2; // Each beep is an ON and OFF state
    buzzerPatternStartTime = millis();
    isBuzzerActive = true;
}

void manageBuzzer(unsigned long currentTime) {
    if (!isBuzzerActive) return;

    unsigned long onDuration, offDuration;
    if (buzzerMode == CRY_ALERT) {
        onDuration = 200; offDuration = 200; // Fast beep
    } else { // WET_ALERT
        onDuration = 500; offDuration = 500; // Slow beep
    }

    unsigned long elapsedTime = currentTime - buzzerPatternStartTime;
    
    if (beepCount > 0) {
        // Check if we are in an ON or OFF part of the beep cycle
        if ( (elapsedTime % (onDuration + offDuration)) < onDuration ) {
            digitalWrite(BUZZER_PIN, HIGH); // Beep ON
        } else {
            digitalWrite(BUZZER_PIN, LOW); // Beep OFF
        }
        
        // Decrement beepCount at the end of each full cycle
        if (elapsedTime >= onDuration + offDuration) {
            beepCount -= 2; // One ON and one OFF state finished
            buzzerPatternStartTime = currentTime; // Reset timer for the next beep
        }
    } else {
        // Pattern finished
        digitalWrite(BUZZER_PIN, LOW);
        isBuzzerActive = false;
        buzzerMode = OFF;
    }
}


void manageDiaperAlertMessage(unsigned long currentTime) {
    // If the alert is active and has been for more than 5 seconds, clear it
    if (isDiaperAlertActive && (currentTime - diaperAlertStartTime > 5000)) {
        isDiaperAlertActive = false;
        // The LCD update function will clear the message automatically
    }
}


// --- DISPLAY FUNCTION ---
void updateLCD(float temperature) {
    // Line 0: Temperature and Fan Status
    lcd.setCursor(0, 0);
    lcd.print("Temp: ");
    lcd.print(temperature, 1); // Temperature with 1 decimal place
    lcd.print("C  ");

    lcd.setCursor(11, 0);
    lcd.print(digitalRead(FAN_RELAY_PIN) == LOW ? "F:ON " : "F:OFF");

    // Line 1: Status Messages
    lcd.setCursor(0, 1);
    if (cradleState != IDLE) {
        lcd.print("Cradle Swinging ");
    } else if (isDiaperAlertActive) {
        lcd.print("Diaper is Wet!  ");
    } else {
        lcd.print("System OK       ");
    }
}

// --- GSM FUNCTIONS ---
void initializeGSM() {
    Serial.println("Initializing GSM Module...");
    // Give the module some time to boot
    delay(3000);
    
    for (int i = 0; i < 5; i++) {
        gsm.println("AT");
        delay(1000);
        
        while (gsm.available()) {
            if (gsm.readString().indexOf("OK") != -1) {
                Serial.println("GSM Module Initialized Successfully!");
                gsm.println("AT+CMGF=1"); // Set to text mode
                delay(500);
                return;
            }
        }
        Serial.println("Retrying GSM Connection...");
    }
    Serial.println("GSM Module Initialization Failed!");
}

void sendSMS(const char* number, const char* message) {
    Serial.print("Sending SMS to ");
    Serial.println(number);

    // This function is blocking due to the delays required for GSM communication.
    // It will cause a brief pause in the main loop's responsiveness.
    gsm.print("AT+CMGS=\"");
    gsm.print(number);
    gsm.println("\"");
    delay(1000);

    gsm.print(message);
    delay(100);

    gsm.write(26); // ASCII for Ctrl+Z to send the message
    delay(1000);

    Serial.println("SMS Sent Command Issued!");
}
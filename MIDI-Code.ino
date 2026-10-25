#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include "USB.h"
#include "USBMIDI.h"

// ==========================================
// 1. PIN DEFINITIONS
// ==========================================
#define PIN_SWITCH1   44  // Button 1 (Octave Up)
#define PIN_SWITCH2   43  // Button 2 (Octave Down)
#define PIN_SWITCH3   6   // Button 3 (Power Toggle Button)

#define PIN_SLIDE1    3   // Slider 1 (Pitch Control)
#define PIN_SLIDE2    5   // Slider 2 (Volume Control)

#define PIN_LED_DATA  4   // Chain Data Pin for 3x SK6812MINI-E
#define NUM_LEDS      3   

// Touch Pins mapping from Touch1 to Touch7
const int TOUCH_PINS[7] = {9, 10, 11, 12, 13, 2, 1}; 

// Default Base Notes (C-Major scale notes in MIDI decimals: C4 to B4)
const int BASE_NOTES[7] = {60, 62, 64, 65, 67, 69, 71}; 

// ==========================================
// 2. OBJECTS & STATE GLOBAL VARIABLES
// ==========================================
USBMIDI MIDI;
Adafruit_NeoPixel leds(NUM_LEDS, PIN_LED_DATA, NEO_GRB + NEO_KHZ800);

// Touch tracking variables
#define TOUCH_THRESHOLD 25000 
bool touchState[7] = {false, false, false, false, false, false, false};

// Octave Shifting State (Altered by Button 1 and Button 2)
// Retained in RTC Memory across deep sleep events so your preferred octave persists!
RTC_DATA_ATTR int octaveOffset = 0; 

// Potentiometer jitter-reduction variables
int lastPitchVal = -1;
int lastVolumeVal = -1;

// ==========================================
// 3. FEATURE MODES & HANDLERS
// ==========================================

// Handle Touch Pads with Octave Shift Context
void handleTouchPads() {
  for (int i = 0; i < 7; i++) {
    uint32_t touchVal = touchRead(TOUCH_PINS[i]);
    bool isTouched = (touchVal > TOUCH_THRESHOLD);

    // Calculate current target note using the active octave adjustment
    int currentNote = BASE_NOTES[i] + octaveOffset;

    if (isTouched && !touchState[i]) {
      touchState[i] = true;
      MIDI.noteOn(currentNote, 127, 1); // Send MIDI Note On (Channel 1)
      
      // Light up LEDs to visually mirror contact
      leds.setPixelColor(i % NUM_LEDS, leds.Color(0, 180, 255)); // Bright Cyan
      leds.show();
      
      Serial.printf("Pad %d Pressed! Playing Note: %d (Octave Shift: %d)\n", i + 1, currentNote, octaveOffset / 12);
    } 
    else if (!isTouched && touchState[i]) {
      touchState[i] = false;
      MIDI.noteOff(currentNote, 0, 1);  // Send MIDI Note Off
      
      leds.setPixelColor(i % NUM_LEDS, leds.Color(0, 0, 0)); // Extinguish LED
      leds.show();
    }
  }
}

// Slider 1: Controls Pitch Bend (14-bit Unsigned Standard: 0 to 16383)
void handleSlider1() {
  int rawVal = analogRead(PIN_SLIDE1);
  
  // Map 12-bit ADC (0-4095) up to standard 14-bit MIDI Pitch Bend range (0-16383)
  // Center value 8192 represents a perfectly centered pitch bend wheel
  int pitchBendVal = map(rawVal, 0, 4095, 0, 16383);
  
  // Filter out minor analog noise fluctuations
  if (abs(pitchBendVal - lastPitchVal) > 64) { 
    lastPitchVal = pitchBendVal;
    MIDI.pitchBend(pitchBendVal, 1); // Channel 1 Pitch Bend
    Serial.printf("Slider 1 -> Pitch Bend Value: %d\n", pitchBendVal);
  }
}

// Slider 2: Controls Main Audio Volume
void handleSlider2() {
  int rawVal = analogRead(PIN_SLIDE2);
  int volumeMidiVal = map(rawVal, 0, 4095, 0, 127); // Standard 7-bit MIDI Value
  
  if (abs(volumeMidiVal - lastVolumeVal) > 1) {
    lastVolumeVal = volumeMidiVal;
    MIDI.controlChange(7, volumeMidiVal, 1); // MIDI CC #7 is standard for Channel Volume
    Serial.printf("Slider 2 -> Volume Value: %d\n", volumeMidiVal);
  }
}

// Handle Buttons 1 & 2 for Octave Scaling
void handleButtons() {
  // Button 1: Octave Up (Max ceiling limit of +3 Octaves / +36 semitones)
  if (digitalRead(PIN_SWITCH1) == LOW) {
    if (octaveOffset < 36) {
      octaveOffset += 12;
      Serial.printf("Octave Increased! Active Shift: %+d Octaves\n", octaveOffset / 12);
      
      // Flash LEDs green to confirm confirmation click
      flashLEDs(leds.Color(0, 150, 0));
    }
    while(digitalRead(PIN_SWITCH1) == LOW); // Wait for release to prevent repeating
  }
  
  // Button 2: Octave Down (Min floor limit of -3 Octaves / -36 semitones)
  if (digitalRead(PIN_SWITCH2) == LOW) {
    if (octaveOffset > -36) {
      octaveOffset -= 12;
      Serial.printf("Octave Decreased! Active Shift: %+d Octaves\n", octaveOffset / 12);
      
      // Flash LEDs orange to confirm configuration click
      flashLEDs(leds.Color(150, 60, 0));
    }
    while(digitalRead(PIN_SWITCH2) == LOW); // Wait for release
  }

  // Button 3: System Power Down Toggle Sequence
  if (digitalRead(PIN_SWITCH3) == LOW) {
    Serial.println("Power Button Pressed! Shutting down into Deep Sleep...");
    delay(200); // Allow logs to flush
    
    // Animate a shutdown color fade out to visually notify user
    for(int i=150; i>=0; i-=10) {
      leds.fill(leds.Color(i, 0, 0)); // Fade red down
      leds.show();
      delay(20);
    }
    leds.clear();
    leds.show();

    // Configure Button 3 (GPIO 6) to Wake Up the Controller on logic LOW (when pressed again)
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_6, 0); 
    
    // Enter Deep Sleep Mode (Powers down CPU, Radio, and Core Peripherals)
    esp_deep_sleep_start();
  }
}

// Helper to flash confirmation color across LEDs
void flashLEDs(uint32_t color) {
  leds.fill(color);
  leds.show();
  delay(100);
  leds.clear();
  leds.show();
}

// ==========================================
// 4. ARDUINO SETUP & LOOP EXECUTION
// ==========================================
void setup() {
  Serial.begin(115200);
  
  // Initialize native USB stack components
  MIDI.begin();
  USB.begin();

  // Initialize LEDs
  leds.begin();
  leds.show(); 

  // Check if we just woke up from deep sleep to show a quick startup flash
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("System Powered Back On!");
    flashLEDs(leds.Color(0, 0, 150)); // Startup flash Blue
  }

  // Setup Button I/O pins with internal pullup resistors
  pinMode(PIN_SWITCH1, INPUT_PULLUP);
  pinMode(PIN_SWITCH2, INPUT_PULLUP);
  pinMode(PIN_SWITCH3, INPUT_PULLUP);
}

void loop() {
  handleTouchPads();
  handleSlider1();
  handleSlider2();
  handleButtons();
  
  // Rotating potentiometer code block left blank intentionally
  
  delay(10); 
}
/*
 * AndonStack Firmware v1.1
 * ESP32-C3 + WS2812B NeoPixel Stack Light Controller
 * 
 * Dual-mode operation:
 * - Console Mode: Receives light commands from Common Dispatch Console via USB Serial
 * - Standalone Mode: Hosts WiFi AP with captive portal for manual control
 * 
 * Hardware:
 * - ESP32-C3 (USB-CDC serial)
 * - 3x WS2812B 16-LED rings (48 LEDs total)
 *   - Ring 1 (LEDs 0-15): Green (bottom)
 *   - Ring 2 (LEDs 16-31): Yellow (middle)  
 *   - Ring 3 (LEDs 32-47): Red (top)
 * 
 * v1.1 Changes:
 * - Added debug output to diagnose serial communication
 * - Fixed serial buffer handling on startup
 * - Ensured polling continues in all modes
 * 
 * MIT License - Feel free to modify for your needs
 */

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Adafruit_NeoPixel.h>

// =============================================================================
// CONFIGURATION
// =============================================================================

// NeoPixel Configuration
#define NEOPIXEL_PIN        4       // GPIO4 - adjust for your wiring
#define NUM_LEDS_PER_RING   16
#define NUM_RINGS           3
#define TOTAL_LEDS          (NUM_LEDS_PER_RING * NUM_RINGS)

// Ring indices (bottom to top)
#define GREEN_RING_START    0
#define YELLOW_RING_START   16
#define RED_RING_START      32

// LED Brightness (0-255)
#define BRIGHTNESS          128

// Timing Configuration
#define CONSOLE_POLL_INTERVAL   10000   // Poll console every 10 seconds
#define CONSOLE_TIMEOUT         30000   // Enter standalone mode after 30s no response
#define BLINK_INTERVAL          500     // Standard blink rate (ms)
#define CHASE_SPEED             50      // Chase animation speed (ms)
#define DOUBLE_BLINK_ON         150     // Double blink on duration
#define DOUBLE_BLINK_OFF        100     // Double blink off duration (between blinks)
#define DOUBLE_BLINK_PAUSE      600     // Pause between double-blink sets
#define STARTUP_DELAY           2000    // Wait for USB serial to stabilize

// WiFi AP Configuration
const char* AP_SSID = "AndonStack";
const char* AP_PASS = "el3c+riC";

// Serial Protocol
#define CMD_STATUS_REQUEST  0xFF        // Arduino sends to request status
#define CMD_STATUS_RESPONSE 0xFE        // Console response header

// Light states
#define LIGHT_OFF           0x00
#define LIGHT_SOLID         0x01
#define LIGHT_BLINK         0x02

// Debug mode - set to true to see serial debug messages
#define DEBUG_MODE          true

// =============================================================================
// GLOBAL VARIABLES
// =============================================================================

Adafruit_NeoPixel strip(TOTAL_LEDS, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

WebServer server(80);
DNSServer dnsServer;

// Operating mode
enum OperatingMode {
    MODE_STARTUP,
    MODE_CONSOLE,
    MODE_STANDALONE
};
OperatingMode currentMode = MODE_STARTUP;

// Console light states (from dispatch console)
uint8_t consoleGreen = LIGHT_OFF;
uint8_t consoleYellow = LIGHT_OFF;
uint8_t consoleRed = LIGHT_OFF;

// Standalone light states (from web interface)
uint8_t standaloneGreen = LIGHT_OFF;
uint8_t standaloneYellow = LIGHT_OFF;
uint8_t standaloneRed = LIGHT_OFF;
bool chaseMode = false;

// Timing
unsigned long startupTime = 0;
unsigned long lastConsolePoll = 0;
unsigned long lastConsoleResponse = 0;
unsigned long lastBlinkToggle = 0;
unsigned long lastChaseUpdate = 0;
unsigned long lastDoubleBlink = 0;
bool blinkState = false;
int chasePosition = 0;
int doubleBlinkPhase = 0;  // 0=first on, 1=first off, 2=second on, 3=pause

// Poll counter for debugging
unsigned long pollCount = 0;

// =============================================================================
// DEBUG HELPER
// =============================================================================

void debugPrint(const char* msg) {
    if (DEBUG_MODE) {
        Serial.print("[AndonStack] ");
        Serial.println(msg);
    }
}

void debugPrintf(const char* format, ...) {
    if (DEBUG_MODE) {
        char buffer[128];
        va_list args;
        va_start(args, format);
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        Serial.print("[AndonStack] ");
        Serial.println(buffer);
    }
}

// =============================================================================
// NEOPIXEL FUNCTIONS
// =============================================================================

void setRingColor(int ringStart, uint32_t color) {
    for (int i = 0; i < NUM_LEDS_PER_RING; i++) {
        strip.setPixelColor(ringStart + i, color);
    }
}

void setRingOff(int ringStart) {
    setRingColor(ringStart, strip.Color(0, 0, 0));
}

void setGreenRing(bool on) {
    setRingColor(GREEN_RING_START, on ? strip.Color(0, 255, 0) : strip.Color(0, 0, 0));
}

void setYellowRing(bool on) {
    setRingColor(YELLOW_RING_START, on ? strip.Color(255, 180, 0) : strip.Color(0, 0, 0));
}

void setRedRing(bool on) {
    setRingColor(RED_RING_START, on ? strip.Color(255, 0, 0) : strip.Color(0, 0, 0));
}

// Chase pattern for red ring
void updateChaseRed() {
    for (int i = 0; i < NUM_LEDS_PER_RING; i++) {
        int pos = (chasePosition + i) % NUM_LEDS_PER_RING;
        // Create a trailing effect - bright at head, fading tail
        int brightness;
        if (i == 0) brightness = 255;
        else if (i == 1) brightness = 180;
        else if (i == 2) brightness = 100;
        else if (i == 3) brightness = 40;
        else brightness = 0;
        
        strip.setPixelColor(RED_RING_START + pos, strip.Color(brightness, 0, 0));
    }
}

// Double blink blue for middle ring (chase mode)
void setYellowRingBlue(bool on) {
    setRingColor(YELLOW_RING_START, on ? strip.Color(0, 0, 255) : strip.Color(0, 0, 0));
}

// Double blink red for bottom ring (chase mode)
void setGreenRingRed(bool on) {
    setRingColor(GREEN_RING_START, on ? strip.Color(255, 0, 0) : strip.Color(0, 0, 0));
}

void allOff() {
    strip.clear();
    strip.show();
}

// =============================================================================
// LIGHT UPDATE LOGIC
// =============================================================================

void updateConsoleLights() {
    unsigned long now = millis();
    
    // Handle blink timing
    if (now - lastBlinkToggle >= BLINK_INTERVAL) {
        blinkState = !blinkState;
        lastBlinkToggle = now;
    }
    
    // Green light
    if (consoleGreen == LIGHT_SOLID) {
        setGreenRing(true);
    } else if (consoleGreen == LIGHT_BLINK) {
        setGreenRing(blinkState);
    } else {
        setGreenRing(false);
    }
    
    // Yellow light
    if (consoleYellow == LIGHT_SOLID) {
        setYellowRing(true);
    } else if (consoleYellow == LIGHT_BLINK) {
        setYellowRing(blinkState);
    } else {
        setYellowRing(false);
    }
    
    // Red light
    if (consoleRed == LIGHT_SOLID) {
        setRedRing(true);
    } else if (consoleRed == LIGHT_BLINK) {
        setRedRing(blinkState);
    } else {
        setRedRing(false);
    }
    
    strip.show();
}

void updateStandaloneLights() {
    unsigned long now = millis();
    
    if (chaseMode) {
        // Chase mode animation
        
        // Red ring: chasing pattern
        if (now - lastChaseUpdate >= CHASE_SPEED) {
            chasePosition = (chasePosition + 1) % NUM_LEDS_PER_RING;
            lastChaseUpdate = now;
        }
        updateChaseRed();
        
        // Middle and bottom rings: offset double-blink
        // Phase: 0=both on, 1=both off, 2=both on, 3=long pause
        unsigned long blinkElapsed = now - lastDoubleBlink;
        
        bool middleOn = false;
        bool bottomOn = false;
        
        if (doubleBlinkPhase == 0 || doubleBlinkPhase == 2) {
            // On phase
            if (blinkElapsed >= DOUBLE_BLINK_ON) {
                doubleBlinkPhase++;
                lastDoubleBlink = now;
            }
            middleOn = true;
            bottomOn = false;  // Offset - opposite of middle
        } else if (doubleBlinkPhase == 1) {
            // Short off between blinks
            if (blinkElapsed >= DOUBLE_BLINK_OFF) {
                doubleBlinkPhase++;
                lastDoubleBlink = now;
            }
            middleOn = false;
            bottomOn = true;  // Offset
        } else {
            // Long pause
            if (blinkElapsed >= DOUBLE_BLINK_PAUSE) {
                doubleBlinkPhase = 0;
                lastDoubleBlink = now;
            }
            middleOn = false;
            bottomOn = false;
        }
        
        setYellowRingBlue(middleOn);
        setGreenRingRed(bottomOn);
        
    } else {
        // Standard mode - handle blink timing
        if (now - lastBlinkToggle >= BLINK_INTERVAL) {
            blinkState = !blinkState;
            lastBlinkToggle = now;
        }
        
        // Green light
        if (standaloneGreen == LIGHT_SOLID) {
            setGreenRing(true);
        } else if (standaloneGreen == LIGHT_BLINK) {
            setGreenRing(blinkState);
        } else {
            setGreenRing(false);
        }
        
        // Yellow light
        if (standaloneYellow == LIGHT_SOLID) {
            setYellowRing(true);
        } else if (standaloneYellow == LIGHT_BLINK) {
            setYellowRing(blinkState);
        } else {
            setYellowRing(false);
        }
        
        // Red light
        if (standaloneRed == LIGHT_SOLID) {
            setRedRing(true);
        } else if (standaloneRed == LIGHT_BLINK) {
            setRedRing(blinkState);
        } else {
            setRedRing(false);
        }
    }
    
    strip.show();
}

// =============================================================================
// SERIAL COMMUNICATION
// =============================================================================

void pollConsole() {
    pollCount++;
    debugPrintf("Sending poll #%lu (0xFF)", pollCount);
    
    // Send the poll request byte
    Serial.write(CMD_STATUS_REQUEST);
    Serial.flush();  // Ensure it's sent immediately
    
    lastConsolePoll = millis();
}

void processSerialData() {
    // Process all available bytes
    while (Serial.available() > 0) {
        uint8_t byte = Serial.read();
        
        debugPrintf("Received byte: 0x%02X", byte);
        
        // Check for status response header
        if (byte == CMD_STATUS_RESPONSE) {
            debugPrint("Got status response header (0xFE)");
            
            // Wait briefly for remaining bytes if needed
            unsigned long waitStart = millis();
            while (Serial.available() < 3 && (millis() - waitStart) < 100) {
                delay(1);
            }
            
            if (Serial.available() >= 3) {
                uint8_t green = Serial.read();
                uint8_t yellow = Serial.read();
                uint8_t red = Serial.read();
                
                debugPrintf("Status: G=%d Y=%d R=%d", green, yellow, red);
                
                // Validate values
                if (green <= LIGHT_BLINK && yellow <= LIGHT_BLINK && red <= LIGHT_BLINK) {
                    consoleGreen = green;
                    consoleYellow = yellow;
                    consoleRed = red;
                    lastConsoleResponse = millis();
                    
                    debugPrint("Valid status received - updating lights");
                    
                    // If we were in standalone mode, switch to console mode
                    if (currentMode == MODE_STANDALONE) {
                        debugPrint("Exiting standalone mode -> console mode");
                        enterConsoleMode();
                    } else if (currentMode == MODE_STARTUP) {
                        debugPrint("Startup complete -> console mode");
                        currentMode = MODE_CONSOLE;
                    }
                } else {
                    debugPrint("Invalid status values - ignoring");
                }
            } else {
                debugPrint("Timeout waiting for status bytes");
            }
        }
        // Ignore other bytes (boot messages, etc.)
    }
}

// =============================================================================
// WIFI & WEB SERVER
// =============================================================================

const char* htmlPage = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>AndonStack Control</title>
    <style>
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body { 
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            background: linear-gradient(135deg, #1a1a2e 0%, #16213e 100%);
            min-height: 100vh;
            padding: 20px;
            color: #fff;
        }
        .container { max-width: 400px; margin: 0 auto; }
        h1 { 
            text-align: center; 
            margin-bottom: 20px;
            font-size: 24px;
            text-shadow: 0 2px 4px rgba(0,0,0,0.3);
        }
        .card {
            background: rgba(255,255,255,0.1);
            border-radius: 16px;
            padding: 20px;
            margin-bottom: 16px;
            backdrop-filter: blur(10px);
        }
        .all-buttons {
            display: flex;
            gap: 8px;
            margin-bottom: 20px;
        }
        .all-buttons button {
            flex: 1;
            padding: 12px 8px;
            border: none;
            border-radius: 8px;
            font-weight: 600;
            cursor: pointer;
            transition: transform 0.1s, opacity 0.2s;
        }
        .all-buttons button:active { transform: scale(0.95); }
        .btn-solid { background: #4CAF50; color: white; }
        .btn-blink { background: #FF9800; color: white; }
        .btn-off { background: #607D8B; color: white; }
        
        .light-row {
            display: flex;
            align-items: center;
            padding: 12px 0;
            border-bottom: 1px solid rgba(255,255,255,0.1);
        }
        .light-row:last-child { border-bottom: none; }
        
        .light-indicator {
            width: 50px;
            height: 50px;
            border-radius: 50%;
            margin-right: 16px;
            box-shadow: 0 0 20px rgba(0,0,0,0.3);
        }
        .light-red { background: radial-gradient(circle at 30% 30%, #ff6b6b, #c0392b); }
        .light-yellow { background: radial-gradient(circle at 30% 30%, #ffd93d, #f39c12); }
        .light-green { background: radial-gradient(circle at 30% 30%, #6bcb77, #27ae60); }
        
        .light-label {
            flex: 1;
            font-weight: 600;
            font-size: 18px;
        }
        
        .light-buttons {
            display: flex;
            gap: 6px;
        }
        .light-buttons button {
            padding: 8px 12px;
            border: none;
            border-radius: 6px;
            font-size: 12px;
            font-weight: 600;
            cursor: pointer;
            transition: transform 0.1s;
        }
        .light-buttons button:active { transform: scale(0.95); }
        .light-buttons .btn-solid { background: #4CAF50; }
        .light-buttons .btn-blink { background: #FF9800; }
        .light-buttons .btn-off { background: #607D8B; }
        
        .chase-section {
            margin-top: 20px;
        }
        .chase-section h3 {
            margin-bottom: 12px;
            font-size: 16px;
            opacity: 0.9;
        }
        .chase-buttons {
            display: flex;
            gap: 8px;
        }
        .chase-buttons button {
            flex: 1;
            padding: 14px;
            border: none;
            border-radius: 8px;
            font-weight: 600;
            cursor: pointer;
            transition: transform 0.1s;
        }
        .chase-buttons button:active { transform: scale(0.95); }
        .btn-chase-on { background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); color: white; }
        .btn-chase-off { background: #607D8B; color: white; }
        
        .status {
            text-align: center;
            padding: 10px;
            font-size: 12px;
            opacity: 0.7;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>&#128308; AndonStack Control</h1>
        
        <div class="card">
            <div class="all-buttons">
                <button class="btn-solid" onclick="setAll('solid')">All Solid</button>
                <button class="btn-blink" onclick="setAll('blink')">All Blink</button>
                <button class="btn-off" onclick="setAll('off')">All Off</button>
            </div>
            
            <div class="light-row">
                <div class="light-indicator light-red"></div>
                <span class="light-label">Red</span>
                <div class="light-buttons">
                    <button class="btn-solid" onclick="setLight('red','solid')">Solid</button>
                    <button class="btn-blink" onclick="setLight('red','blink')">Blink</button>
                    <button class="btn-off" onclick="setLight('red','off')">Off</button>
                </div>
            </div>
            
            <div class="light-row">
                <div class="light-indicator light-yellow"></div>
                <span class="light-label">Yellow</span>
                <div class="light-buttons">
                    <button class="btn-solid" onclick="setLight('yellow','solid')">Solid</button>
                    <button class="btn-blink" onclick="setLight('yellow','blink')">Blink</button>
                    <button class="btn-off" onclick="setLight('yellow','off')">Off</button>
                </div>
            </div>
            
            <div class="light-row">
                <div class="light-indicator light-green"></div>
                <span class="light-label">Green</span>
                <div class="light-buttons">
                    <button class="btn-solid" onclick="setLight('green','solid')">Solid</button>
                    <button class="btn-blink" onclick="setLight('green','blink')">Blink</button>
                    <button class="btn-off" onclick="setLight('green','off')">Off</button>
                </div>
            </div>
            
            <div class="chase-section">
                <h3>&#127752; Emergency Chase Mode</h3>
                <div class="chase-buttons">
                    <button class="btn-chase-on" onclick="setChase('on')">Chase Mode On</button>
                    <button class="btn-chase-off" onclick="setChase('off')">Chase Mode Off</button>
                </div>
            </div>
        </div>
        
        <div class="status">
            AndonStack v1.1 | Standalone Mode
        </div>
    </div>
    
    <script>
        function setLight(color, state) {
            fetch('/set?light=' + color + '&state=' + state)
                .catch(err => console.log('Request sent'));
        }
        
        function setAll(state) {
            fetch('/setall?state=' + state)
                .catch(err => console.log('Request sent'));
        }
        
        function setChase(state) {
            fetch('/chase?state=' + state)
                .catch(err => console.log('Request sent'));
        }
    </script>
</body>
</html>
)rawliteral";

void setupWebServer() {
    // Main page
    server.on("/", HTTP_GET, []() {
        server.send(200, "text/html", htmlPage);
    });
    
    // Set individual light
    server.on("/set", HTTP_GET, []() {
        String light = server.arg("light");
        String state = server.arg("state");
        
        uint8_t stateValue = LIGHT_OFF;
        if (state == "solid") stateValue = LIGHT_SOLID;
        else if (state == "blink") stateValue = LIGHT_BLINK;
        
        if (light == "red") standaloneRed = stateValue;
        else if (light == "yellow") standaloneYellow = stateValue;
        else if (light == "green") standaloneGreen = stateValue;
        
        chaseMode = false;  // Exit chase mode when setting individual lights
        
        server.send(200, "text/plain", "OK");
    });
    
    // Set all lights
    server.on("/setall", HTTP_GET, []() {
        String state = server.arg("state");
        
        uint8_t stateValue = LIGHT_OFF;
        if (state == "solid") stateValue = LIGHT_SOLID;
        else if (state == "blink") stateValue = LIGHT_BLINK;
        
        standaloneRed = stateValue;
        standaloneYellow = stateValue;
        standaloneGreen = stateValue;
        chaseMode = false;
        
        server.send(200, "text/plain", "OK");
    });
    
    // Chase mode
    server.on("/chase", HTTP_GET, []() {
        String state = server.arg("state");
        chaseMode = (state == "on");
        
        if (chaseMode) {
            // Reset animation states
            chasePosition = 0;
            doubleBlinkPhase = 0;
            lastChaseUpdate = millis();
            lastDoubleBlink = millis();
        }
        
        server.send(200, "text/plain", "OK");
    });
    
    // Captive portal - redirect all requests to main page
    server.onNotFound([]() {
        server.sendHeader("Location", "/", true);
        server.send(302, "text/plain", "");
    });
    
    server.begin();
}

void startAP() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    
    // Start DNS server for captive portal
    dnsServer.start(53, "*", WiFi.softAPIP());
    
    setupWebServer();
    
    debugPrint("AP Started: AndonStack");
    debugPrintf("IP: %s", WiFi.softAPIP().toString().c_str());
}

void stopAP() {
    debugPrint("Stopping AP...");
    server.stop();
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
}

// =============================================================================
// MODE MANAGEMENT
// =============================================================================

void enterConsoleMode() {
    if (currentMode == MODE_STANDALONE) {
        stopAP();
    }
    
    currentMode = MODE_CONSOLE;
    chaseMode = false;
    standaloneGreen = LIGHT_OFF;
    standaloneYellow = LIGHT_OFF;
    standaloneRed = LIGHT_OFF;
    
    debugPrint("=== CONSOLE MODE ===");
}

void enterStandaloneMode() {
    currentMode = MODE_STANDALONE;
    
    // Reset console light states
    consoleGreen = LIGHT_OFF;
    consoleYellow = LIGHT_OFF;
    consoleRed = LIGHT_OFF;
    
    // Start AP
    startAP();
    
    debugPrint("=== STANDALONE MODE ===");
}

// =============================================================================
// MAIN SETUP & LOOP
// =============================================================================

void setup() {
    // Initialize serial - use higher baud rate for ESP32-C3 USB-CDC
    Serial.begin(115200);
    
    // Wait for USB serial to be ready
    delay(STARTUP_DELAY);
    
    // Clear any garbage in serial buffer from boot
    while (Serial.available()) {
        Serial.read();
    }
    
    debugPrint("==============================");
    debugPrint("AndonStack v1.1 Starting...");
    debugPrint("==============================");
    
    // Initialize NeoPixels
    strip.begin();
    strip.setBrightness(BRIGHTNESS);
    strip.clear();
    strip.show();
    
    debugPrint("NeoPixels initialized");
    
    // Startup animation - quick sweep
    for (int i = 0; i < TOTAL_LEDS; i++) {
        strip.setPixelColor(i, strip.Color(0, 100, 255));
        strip.show();
        delay(20);
    }
    delay(200);
    strip.clear();
    strip.show();
    
    // Initialize timing
    startupTime = millis();
    lastConsolePoll = millis();
    lastConsoleResponse = 0;  // No response yet
    lastBlinkToggle = millis();
    
    debugPrint("Waiting for console connection...");
    debugPrintf("Will enter standalone mode in %d seconds if no response", CONSOLE_TIMEOUT / 1000);
    
    // Send first poll immediately
    pollConsole();
}

void loop() {
    unsigned long now = millis();
    
    // Always check for serial data (in ALL modes)
    if (Serial.available()) {
        processSerialData();
    }
    
    // Poll console at interval (in ALL modes - so we can detect console connection)
    if (now - lastConsolePoll >= CONSOLE_POLL_INTERVAL) {
        pollConsole();
    }
    
    // Check for console timeout (only during startup or console mode)
    if (currentMode == MODE_STARTUP || currentMode == MODE_CONSOLE) {
        if (lastConsoleResponse == 0) {
            // Never received a response - check startup timeout
            if (now - startupTime >= CONSOLE_TIMEOUT) {
                debugPrint("No console response - entering standalone mode");
                enterStandaloneMode();
            }
        } else if (now - lastConsoleResponse >= CONSOLE_TIMEOUT) {
            // Lost console connection
            debugPrint("Console connection lost - entering standalone mode");
            enterStandaloneMode();
        }
    }
    
    // Handle current mode
    switch (currentMode) {
        case MODE_STARTUP:
            // Waiting for console - lights off (or could do a waiting animation)
            break;
            
        case MODE_CONSOLE:
            updateConsoleLights();
            break;
            
        case MODE_STANDALONE:
            dnsServer.processNextRequest();
            server.handleClient();
            updateStandaloneLights();
            break;
    }
    
    // Small delay to prevent tight loop
    delay(10);
}

#include <Arduino.h>
#include <stdlib.h>
#include <SoftWire.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include <DNSServer.h>
#ifdef ESP32
  #include <AsyncTCP.h>
  #include <WiFi.h>
#elif defined(ESP8266)
  #include <ESP8266WiFi.h>
  #include <ESPAsyncTCP.h>
#elif defined(TARGET_RP2040)
  #include <WebServer.h>
  #include <WiFi.h>
#endif

#include "ESPAsyncWebServer.h"

//#include <LittleFS.h>

bool core1_separate_stack = true;

#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 32 // OLED display height, in pixels

// Declaration for an SSD1306 display connected to I2C (SDA, SCL pins)
// The pins for I2C are defined by the Wire-library. 
// On an arduino UNO:       A4(SDA), A5(SCL)
// On an arduino MEGA 2560: 20(SDA), 21(SCL)
// On an arduino LEONARDO:   2(SDA),  3(SCL)
// On an espressif 8266:    D2(SDA), D1(SCL)
#define OLED_RESET     -1 // Reset pin # (or -1 if sharing Arduino reset pin)
#define SCREEN_ADDRESS 0x3C ///< See datasheet for Address; 0x3D for 128x64, 0x3C for 128x32
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

const char* HTML PROGMEM = R"(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>IV</title>
</head>
<body>
    <input type="number" id="ml"><span>mL</span>
    <br>
    <input type="number" id="min"><span>minutes</span>
    <br>
    <button onclick='start()'>Start</button>
    <button onclick='stop()'>Stop</button>
    <h3>Astronaut</h3>
    <select id="astros">
    </select>
    <h3>Medication</h3>
    <select id="medications">
    </select>

    <h1 id="status">Status: Unknown</h1>

    <script>
        const mlEl = document.querySelector('#ml');
        const minEl = document.querySelector("#min");
        const statusEl = document.querySelector('#status');
        const astrosEl = document.querySelector('#astros');
        const medicationsEl = document.querySelector('#medications');

        const astros = [
            {
                name: "Billy Bob",
                mass: 60 // kg
            },
            {
                name: "Jane Doe",
                mass: 52 // kg
            },
            {
                name: "Luke Becker",
                mass: 81 // kg
            }
        ];

        const medications = [
            {
                name: 'Butane',
                amountFunc: (mass) => mass/4,
                timeFunc: (mass) => mass/30
            },
            {
                name: 'Monster Energy',
                amountFunc: (mass) => mass/2,
                timeFunc: (mass) => mass/35
            },
            {
                name: 'Mountain Dew',
                amountFunc: (mass) => mass*5,
                timeFunc: (mass) => mass/4
            },
        ]

        astros.forEach(astro => {
            const astroEl = document.createElement('option');
            astroEl.innerText = `${astro.name} (${astro.mass} kg)`;
            astroEl.value = astroEl.innerText;
            astrosEl.appendChild(astroEl);
        });

        medications.forEach(medic => {
            const medicEl = document.createElement('option');
            medicEl.innerText = `${medic.name}`;
            medicEl.value = medicEl.innerText;
            medicationsEl.appendChild(medicEl);
        });

        let currentAstro = astros[0];

        astrosEl.addEventListener('change', e => {
            astros.forEach(astro => {
                if(e.target.value.startsWith(astro.name)) {
                    currentAstro = astro;
                }
            })
        });

        medicationsEl.addEventListener('change', e => {
            medications.forEach(medic => {
                if(medic.name == e.target.value) {
                    console.log(medic.amountFunc(currentAstro.mass))
                    mlEl.value = medic.amountFunc(currentAstro.mass);
                    mlEl.innerText = medic.amountFunc(currentAstro.mass);
                    minEl.value = medic.timeFunc(currentAstro.mass);
                    minEl.innerText = medic.timeFunc(currentAstro.mass);
                }
            });
        });

        async function start() {
            fetch(`http://192.168.42.1/run?volume=${mlEl.value}&minutes=${minEl.value}`);
        }

        function stop() {
            fetch(`http://192.168.42.1/stop`)
        }

        setInterval(async () => {
            const res = await fetch('http://192.168.42.1/status');
            const text = await res.text();
            console.log(text);
            const ab = text.split(',');
            const totalTime = parseInt(ab[1]);
            const mins = Math.floor(totalTime/60);
            const hours = Math.floor(totalTime/60/60);
            const seconds = totalTime % 60;
            statusEl.innerHTML = `${ab[0]} mL <br> ${hours}:${mins}:${seconds}`
        }, 1000)
    </script>
</body>
</html>
)";

#define STEPS_PER_DRIP 100

#define CHA 8
#define CHB 7
// Rotary
#define CSW 6

// Emergency
#define BUZZ 28
#define ESTOP 0

// Display
#define DISP_SDA 4
#define DISP_SCL 5

#include "DRV8834.h"
#define DIR 18
#define STEP 19
#define ENABLE 26

#define STEPS 200
#define CALIBRATION_STEPS 50000.0
DRV8834 stepper(STEPS, DIR, STEP, ENABLE, 16, 22);

float coefficient = 5.26; // mL

void notFound(AsyncWebServerRequest* request) {
    request->send(404, "text/plain", "Not found");
}

DNSServer dnsServer;
AsyncWebServer server(80);

float volume = 0; // mL
long minutes = 0;
int encoderNum = 0;
bool startweb = false;
bool stopweb = false;
long remaining = 0;

void setup() {
    /*if(!LittleFS.begin(true)){
        Serial.println("An Error has occurred while mounting LittleFS");
        return;
    }

    File file = LittleFS.open("/index.html");
    file.read();
    if(!file){
        Serial.println("Failed to open file for reading");
        return;
    }*/


    if (!WiFi.softAP("IV", "verysecurepasswordIV")) {
        Serial.println("Soft AP creation failed.");
        while (1);
    }

    pinMode(CHA, INPUT);
    pinMode(CHB, INPUT);
    pinMode(CSW, INPUT_PULLUP);
    pinMode(ESTOP, INPUT_PULLUP);
    pinMode(BUZZ, OUTPUT);


    Wire.setSDA(4);
    Wire.setSCL(5);
    if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
        Serial.println(F("SSD1306 allocation failed"));
        //for(;;); // Don't proceed, loop forever
    }

    display.setTextSize(1);      // Normal 1:1 pixel scale
    display.setTextColor(SSD1306_WHITE); // Draw white text
    display.cp437(true);
    display.setRotation(2);

    display.clearDisplay();
    display.display();

    server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(200, "text/html", HTML);
    });

    server.on("/run", HTTP_GET, [](AsyncWebServerRequest* request) {
        volume = request->getParam("volume")->value().toFloat();
        minutes = request->getParam("minutes")->value().toInt();
        startweb = true;

        request->send(200);
    });

    server.on("/stop", HTTP_GET, [](AsyncWebServerRequest* request) {
        stopweb = true;
        request->send(200);
    });

    server.on("/status", HTTP_GET, [](AsyncWebServerRequest* request) {
        String val = String(volume);
        val.concat(",");
        val.concat(remaining);
        request->send(200, "text/plain", val);
    });

    server.begin();
}

char buf[20];
int idx = 0;

char screen_buf[20];

void text(char* text) {
    int i = 0;
    
    while(text[i] != '\0') {
        display.write(text[i]);
        i++;
    }
}

bool last_chA = false;
bool last_chB = false;
bool chA = false;
bool chB = false;

int lastVelocity = 0;
unsigned long velocityTimer = 0;

int readEncoder() {
    last_chA = chA;
    chA = !digitalRead(CHA);
    last_chB = chB;
    chB = !digitalRead(CHB);

    if(velocityTimer + 200 < millis()) {
        lastVelocity += lastVelocity == 0 ? 0 : (lastVelocity > 0 ? -1 : 1);
    }

    if(last_chA == chA && last_chB == chB) return 0;
    if(last_chA && !last_chB && lastVelocity <= 0) {
        lastVelocity = max(-5, lastVelocity - 1);
        velocityTimer = millis();
        return -1;
    }else if (!last_chA && last_chB && lastVelocity >= 0) {
        lastVelocity = min(5, lastVelocity + 1);
        velocityTimer = millis();
        return 1;
    }

    return 0;
}

bool click = true;

enum Page {
    DPM,
    MINS,
    ADMIN,
    CALIBRATE,
    SAVE
};
Page page = Page::DPM;

long startTime = 0;
long lastShow = 0;
long clickStart = 0;
long clickEnd = 0;

void render() {
    display.clearDisplay();
    display.setCursor(0, 0);
    
    switch(page) {
    case Page::DPM:
        sprintf(screen_buf, "%f mL", (float)encoderNum*10.0);
        text(screen_buf);
        display.display();
        break;
    case Page::MINS:
        sprintf(screen_buf, "%i Minutes", encoderNum);
        text(screen_buf);
        display.display();
        break;
    case Page::ADMIN:
        {
            remaining = startTime/1000 + minutes*60 - millis()/1000;
            sprintf(screen_buf, "%i:%i:%i Remaining", (int)floor((long double)remaining/60.0/60.0), (int)floor((double)remaining/60.0)%60, (int)remaining%60);
            text(screen_buf);
            display.setCursor(0, 12);
            sprintf(screen_buf, "%f mL", volume);
            text(screen_buf);
            display.setCursor(0, 24);
            text("Click to STOP");
            display.display();
            if(remaining <= 0) {
                stepper.stop();
                page = Page::DPM;
                render();
            }
        }
        break;
    case Page::CALIBRATE:
        display.setCursor(0, 0);
        text("Click to start calibration");
        display.display();
        break;
    case Page::SAVE:
        display.setCursor(0, 0);
        sprintf(screen_buf, "%f", volume);
        text(screen_buf);
        display.display();
    }
}

bool lastClick = false;
bool stopLastClick = false;
bool first = false;

void innerLoop() {
    click = !digitalRead(CSW);

    if(page == Page::ADMIN && stopweb) {
        page = Page::DPM;
        rp2040.fifo.push_nb(0);
        return;
    }
    
    if(stepper.getCurrentState() == BasicStepperDriver::State::STOPPED || (lastClick != click)) {
        stepper.disable();
        if(lastClick != click) {
            if(click) {
                clickStart = millis();
            }else {
                clickEnd = millis();
                if(clickEnd - clickStart >= 3 * 1000 && page == Page::DPM) {
                    page = Page::CALIBRATE;
                    lastClick = click;
                    return;
                }
            }
        }
        
        int encOut = readEncoder();
        int newEnc = max(encoderNum + encOut, 0);

        if(page == Page::ADMIN) {
            page = Page::DPM;
            rp2040.fifo.push_nb(0);
            return;
        }

        // Rotary Clicked
        if(lastClick != click && !click) {
            if(page == Page::DPM) {
                volume = (float)encoderNum*10.0;
                page = Page::MINS;
            }else if(page == Page::MINS) {
                minutes = encoderNum;

                rp2040.fifo.push_nb(0);
                startTime = millis();

                page = Page::ADMIN;
            }else if(page == Page::CALIBRATE) {
                stepper.enable();
                stepper.setRPM(20);
                stepper.move(CALIBRATION_STEPS);
                page = Page::SAVE;
            }else if(page == Page::SAVE) {

            }

            encoderNum = 0;
            newEnc = 0;
            render();
        }

        if(newEnc != encoderNum || click || !first) {
            first = true;
            encoderNum = newEnc;
            render();
        }

        lastClick = click;
    }else {
        stopLastClick = click;

        if(lastShow + 1000 < millis()) {
            render();
            lastShow = millis();
        }
    }
}

bool stopped = false;

void stop() {
    stopped = true;
    digitalWrite(BUZZ, HIGH);
}

void loop() {
    if(startweb) {
        rp2040.fifo.push_nb(0);
        startTime = millis();
        page = Page::ADMIN;
        startweb = false;
        render();
    }
    if(!stopped) innerLoop();
    else if(!digitalRead(CSW)) {
        stopped = false;
        digitalWrite(BUZZ, LOW);
    }
    if(digitalRead(ESTOP) == LOW) {
        stop();
    }
}

void setup1() {
    stepper.begin(60, 16);
    stepper.enable();
    stepper.setMicrostep(16);
    stepper.setEnableActiveState(LOW);
}

void loop1() {
    if(rp2040.fifo.available()) {
        rp2040.fifo.pop();
        if(stepper.getCurrentState() == BasicStepperDriver::State::STOPPED) {
            stepper.enable();
            float fast = 1;
            if(volume/(float)minutes > 40.0) {
                stepper.setMicrostep(1);
                fast = 1/16;
            }
            stepper.setRPM((float)volume*CALIBRATION_STEPS/coefficient/1600.0);
            stepper.startMove(volume*CALIBRATION_STEPS/coefficient*fast, minutes*60*1000*1000);
        }else {
            stepper.disable();
        }
    }
    if(stepper.getCurrentState() != BasicStepperDriver::State::STOPPED) {
        stepper.nextAction();
    }
}
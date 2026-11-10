#include <PZEM004Tv30.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET   -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

#if defined(ESP32)
PZEM004Tv30 pzem(Serial2, 16, 17);  // RX=16, TX=17
#else
PZEM004Tv30 pzem(Serial2);
#endif

void setup() {
    Serial.begin(115200);

    Wire.begin(21, 22);

    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println(F("SSD1306 allocation failed"));
        while (true);
    }

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 30);
    display.println("Initializing...");
    display.display();

    delay(1000);
}

void loop() {
    float voltage = pzem.voltage();
    float current = pzem.current();
    float power = pzem.power();
    float energy = pzem.energy();
    float frequency = pzem.frequency();
    float pf = pzem.pf();

    bool dataValid = !(isnan(voltage) || isnan(current) || isnan(power) ||
                       isnan(energy) || isnan(frequency) || isnan(pf));

    Serial.print("Custom Address:");
    Serial.println(pzem.readAddress(), HEX);

    if (!dataValid) {
        Serial.println("Error reading from PZEM!");

        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(0, 30);
        display.println("  COMM ERROR!  ");
        display.display();
    } else {
        Serial.print("Voltage: ");      Serial.print(voltage);      Serial.println("V");
        Serial.print("Current: ");      Serial.print(current);      Serial.println("A");
        Serial.print("Power: ");        Serial.print(power);        Serial.println("W");
        Serial.print("Energy: ");       Serial.print(energy, 3);    Serial.println("kWh");
        Serial.print("Frequency: ");    Serial.print(frequency, 1); Serial.println("Hz");
        Serial.print("PF: ");           Serial.println(pf);
        Serial.println();

        // === Display on OLED with proper spacing ===
        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);

        int y = 0;      // Start at top
        int spacing = 14; // Reduced spacing to fit 6 lines

        display.setCursor(0, y);
        display.print("Volt: "); display.print(voltage, 1); display.println(" V");

        y += spacing;
        display.setCursor(0, y);
        display.print("Curr: "); display.print(current, 2); display.println(" A");

        y += spacing;
        display.setCursor(0, y);
        display.print("Pwr: "); display.print(power, 1); display.println(" W");

        y += spacing;
        display.setCursor(0, y);
        display.print("Eng: "); display.print(energy, 3); display.println(" kWh");

        y += spacing;
        display.setCursor(0, y);
        display.print("Freq: "); display.print(frequency, 1); display.println(" Hz");

        y += spacing;
        display.setCursor(0, y);
        display.print("PF: "); display.println(pf);

        display.display(); // Update screen
    }

    delay(2000);
}
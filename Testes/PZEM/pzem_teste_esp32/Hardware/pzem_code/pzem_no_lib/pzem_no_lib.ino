#include <HardwareSerial.h>

// Define Serial Port (Serial2 on ESP32)
#define PZEM_SERIAL Serial2

// PZEM Slave Address (set via setAddress or default is often 0xF8)
// You mentioned "Custom Address: 1" → so we use 0x01
#define PZEM_ADDR 0x01

// Modbus Function Code for Read Input Registers
#define CMD_RIR 0x04

// Register addresses (from pzem.txt)
#define REG_VOLTAGE     0x0000
#define REG_CURRENT_L   0x0001
#define REG_POWER_L     0x0003
#define REG_ENERGY_L    0x0005
#define REG_FREQUENCY   0x0007
#define REG_PF          0x0008

// Response size: 25 bytes = [addr][func][byteCnt][20 data][CRC Lo][CRC Hi]
#define RESPONSE_SIZE 25
#define READ_TIMEOUT 100

// RX and TX Pins for ESP32 (choose any two pins)
#define PZEM_RX_PIN 16
#define PZEM_TX_PIN 17

void setup() {
  Serial.begin(115200);           // USB serial for monitoring
  PZEM_SERIAL.begin(9600, SERIAL_8N1, PZEM_RX_PIN, PZEM_TX_PIN); // PZEM connected here

  delay(1000);
  Serial.println("PZEM-004T Reader - No Library | Raw Modbus RTU");
}

// === MODBUS CRC16 Calculation ===
uint16_t modbusCRC16(const uint8_t *buf, int len) {
  uint16_t crc = 0xFFFF;
  for (int i = 0; i < len; i++) {
    crc ^= buf[i];
    for (int j = 0; j < 8; j++) {
      if (crc & 0x0001) {
        crc = (crc >> 1) ^ 0xA001; // Polynomial for Modbus
      } else {
        crc >>= 1;
      }
    }
  }
  return crc;
}

// === SEND MODBUS COMMAND: Read Input Registers (0x04) ===
bool sendReadCommand(uint16_t startReg, uint16_t numRegs, uint8_t *response, uint8_t respLen) {
  uint8_t cmd[8];
  cmd[0] = PZEM_ADDR;             // Slave address
  cmd[1] = CMD_RIR;               // Function: Read Input Registers
  cmd[2] = (startReg >> 8) & 0xFF;// Start register high byte
  cmd[3] = startReg & 0xFF;       // Start register low byte
  cmd[4] = (numRegs >> 8) & 0xFF; // Number of registers high
  cmd[5] = numRegs & 0xFF;        // Number of registers low

  // Calculate CRC16 and append (little-endian)
  uint16_t crc = modbusCRC16(cmd, 6);
  cmd[6] = crc & 0xFF;            // CRC Low
  cmd[7] = (crc >> 8) & 0xFF;     // CRC High

  // Flush input buffer before sending
  while (PZEM_SERIAL.available()) PZEM_SERIAL.read();

  // Send command
  PZEM_SERIAL.write(cmd, 8);

  // Wait for response
  unsigned long startTime = millis();
  int index = 0;
  while ((millis() - startTime) < READ_TIMEOUT && index < respLen) {
    if (PZEM_SERIAL.available()) {
      response[index++] = PZEM_SERIAL.read();
    }
  }

  // Check if response length matches
  if (index != respLen) return false;

  // Verify CRC of response
  uint16_t respCRC = modbusCRC16(response, respLen - 2);
  uint16_t receivedCRC = (response[respLen - 1] << 8) | response[respLen - 2];
  if (respCRC != receivedCRC) return false;

  // Verify address and function match
  if (response[0] != PZEM_ADDR || response[1] != CMD_RIR) return false;

  return true;
}

// === MAIN LOOP: Read All Values ===
void loop() {
  uint8_t response[RESPONSE_SIZE];

  // Send command to read 10 registers starting from 0x0000
  if (!sendReadCommand(0x0000, 0x000A, response, RESPONSE_SIZE)) {
    Serial.println("*** Failed to read from PZEM-004T");
    delay(2000);
    return;
  }

  // === Parse Response ===
  // Voltage: Reg 0x0000 → 2 bytes @ offset 3,4 → ×0.1 V
  float voltage = ((response[3] << 8) | response[4]) / 10.0;

  // Current: Reg 0x0001–0x0002 → 4 bytes @ 5–8 → fixed point ×0.001 A
  float current = ((uint32_t)(response[5] << 8 | response[6]) |
                   (uint32_t)(response[7] << 24) |
                   (uint32_t)(response[8] << 16)) / 1000.0;

  // Power: Reg 0x0003–0x0004 → 4 bytes @ 9–12 → ×0.1 W
  float power = ((uint32_t)(response[9] << 8 | response[10]) |
                 (uint32_t)(response[11] << 24) |
                 (uint32_t)(response[12] << 16)) / 10.0;

  // Energy: Reg 0x0005–0x0006 → 4 bytes @ 13–16 → Wh → ÷1000 → kWh
  float energy = ((uint32_t)(response[13] << 8 | response[14]) |
                  (uint32_t)(response[15] << 24) |
                  (uint32_t)(response[16] << 16)) / 1000.0;

  // Frequency: Reg 0x0007 → 2 bytes @ 17,18 → ×0.1 Hz
  float frequency = ((response[17] << 8) | response[18]) / 10.0;

  // Power Factor: Reg 0x0008 → 2 bytes @ 19,20 → ×0.01
  float pf = ((response[19] << 8) | response[20]) / 100.0;

  // === Print Results ===
  Serial.println("─────────────────────────────");
  Serial.print("Voltage:   "); Serial.print(voltage, 1); Serial.println(" V");
  Serial.print("Current:   "); Serial.print(current, 2); Serial.println(" A");
  Serial.print("Power:     "); Serial.print(power, 1); Serial.println(" W");
  Serial.print("Energy:    "); Serial.print(energy, 3); Serial.println(" kWh");
  Serial.print("Frequency: "); Serial.print(frequency, 1); Serial.println(" Hz");
  Serial.print("PF:        "); Serial.println(pf, 2);

  delay(2000); // Update every 2 seconds
}
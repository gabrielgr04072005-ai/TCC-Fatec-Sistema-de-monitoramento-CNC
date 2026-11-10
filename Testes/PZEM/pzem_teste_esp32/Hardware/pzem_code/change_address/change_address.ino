#include <PZEM004Tv30.h>

// === Choose constructor based on board ===
#if defined(ESP32)
  // For ESP32: Specify Serial2 + RX/TX pins
  PZEM004Tv30 pzem(Serial2, 16, 17);
#else
  // For Arduino (e.g., Mega)
  PZEM004Tv30 pzem(Serial2);
#endif

void setup() {
  Serial.begin(115200); // USB serial for interaction
  delay(1000);

  Serial.println("\n INTERACTIVE PZEM-004T ADDRESS CHANGER");
  Serial.println("────────────────────────────────────────");
  Serial.println(" Requirements:");
  Serial.println("   • AC power ON");
  Serial.println("   • Load connected (>20W)");
  Serial.println("   • Wiring: PZEM-TX → ESP32-GPIO16, PZEM-RX → GPIO17\n");

  uint8_t currentAddr = pzem.readAddress();

  if (currentAddr == 0x00) {
    Serial.println(".... Failed to read current address.");
    Serial.println("   ➤ Check: AC power, load, wiring, or try resetting PZEM.\n");
    while (true) { delay(100); } // Halt until fixed
  }

  Serial.print("✅ Current Address: 0x"); Serial.println(currentAddr, HEX);
  Serial.println();

  // Ask user for new address
  int newAddr = -1;
  while (newAddr == -1) {
    Serial.print("👉 Enter new address (1–247): ");
    
    while (!Serial.available()) {
      delay(100);
    }

    String input = Serial.readStringUntil('\n');
    input.trim(); // Remove whitespace

    if (input.length() == 0) continue;

    newAddr = input.toInt();

    if (newAddr < 1 || newAddr > 247) {
      Serial.printf("❌ Invalid: %d. Must be 1–247.\n\n", newAddr);
      newAddr = -1; // Retry
    }
  }

  Serial.printf("🎯 You entered: %d (0x%X)\n", newAddr, newAddr);

  // Confirm action
  Serial.print("⚠️  WARNING: This will change the PZEM's address permanently!\n");
  Serial.print("   Power cycle required after change.\n");
  Serial.print("   Type 'yes' to confirm: ");

  while (!Serial.available()) { delay(100); }
  String confirm = Serial.readStringUntil('\n');
  confirm.toLowerCase();
  confirm.trim();

  if (confirm != "yes" && confirm != "y") {
    Serial.println("\n🛑 Operation canceled.");
    return;
  }

  // Attempt to change address
  Serial.println("\n🔄 Changing address...");
  delay(100);

  if (pzem.setAddress(newAddr)) {
    Serial.println("✅ Success! Address changed.");
    Serial.printf("   ➤ New Address: %d (0x%X)\n", newAddr, newAddr);
  } else {
    Serial.println("❌ Failed to set new address.");
    Serial.println("   Possible causes:");
    Serial.println("     - Communication error");
    Serial.println("     - Invalid address (though validated)");
    Serial.println("     - PZEM reset during write");
    return;
  }

  // Verify by reading back
  delay(500);
  uint8_t verifiedAddr = pzem.readAddress();
  Serial.printf("🔍 Verifying... Read back: %d (0x%X)\n", verifiedAddr, verifiedAddr);

  if (verifiedAddr == newAddr) {
    Serial.println("🎉 SUCCESS: Address confirmed changed!");
  } else {
    Serial.println("⚠️  FAILED: Verification mismatch!");
    Serial.println("   ⚠️  The device may still respond at old address.");
  }

  Serial.println("\n📌 FINAL STEP: Power cycle the PZEM module now.");
  Serial.println("   After power-off/on, it will use the NEW address.");
}

void loop() {
  // Nothing more to do
}
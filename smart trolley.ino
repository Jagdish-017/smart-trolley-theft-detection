
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <WiFi.h>
#include <WebServer.h>

const char* WIFI_SSID = "Deveshbari";
const char* WIFI_PASSWORD = "Deveshbari";

WebServer server(80);
// ---------------- PIN CONFIGURATION ----------------
#define SS_PIN 5
#define RST_PIN 27

#define BUTTON1 25  // Simulate unauthorized removal
#define BUTTON2 26  // Complete billing / allow removal
#define BUTTON3 32  // Start billing
#define BUTTON4 14  // Confirm one returned item

#define BUZZER_PIN 4
#define RED_LED 13
#define GREEN_LED 12

// ---------------- HARDWARE OBJECTS ----------------
MFRC522 rfid(SS_PIN, RST_PIN);
Adafruit_SH1106G display(128, 64, &Wire, -1);

// ---------------- PRODUCTS ----------------
const int PRODUCT_COUNT = 3;
const int MAX_STACK = 50;

String productNames[PRODUCT_COUNT] = {
  "Rice 1kg",
  "Sugar 500g",
  "Thums Up 1L"
};

String productUIDs[PRODUCT_COUNT] = {
  "8C 45 51 07",
  "21 ED 50 07",
  "7C EE 51 07"
};

int productPrices[PRODUCT_COUNT] = {
  70, 25, 45
};

int quantities[PRODUCT_COUNT] = { 0, 0, 0 };
int total = 0;

// Each entry represents one simulated removed item.
// -1 means an item was removed when no cart item was available
// to identify in the simulation.
int removalStack[MAX_STACK];
int stackTop = -1;

// ---------------- SYSTEM STATE ----------------
bool billingStarted = false;
bool removalAllowed = false;
bool theftAlert = false;
bool dashboardReset = false;

unsigned long messageUntil = 0;
bool temporaryMessageActive = false;
String messageLine1 = "";
String messageLine2 = "";

unsigned long lastDisplayUpdate = 0;
unsigned long lastScrollUpdate = 0;
unsigned long lastBeepUpdate = 0;

bool buzzerIsOn = false;
bool previousButton1 = HIGH;
bool previousButton2 = HIGH;
bool previousButton3 = HIGH;
bool previousButton4 = HIGH;

int scrollOffset = 0;

// ---------------- BASIC FUNCTIONS ----------------
bool hasUnreturnedItems() {
  return stackTop >= 0;
}

int getTotalItems() {
  int count = 0;
  for (int i = 0; i < PRODUCT_COUNT; i++) {
    count += quantities[i];
  }
  return count;
}

void updateLEDs() {
  bool alertActive = theftAlert || hasUnreturnedItems();

  digitalWrite(RED_LED, alertActive ? HIGH : LOW);
  digitalWrite(GREEN_LED, alertActive ? LOW : HIGH);
}

void updateAlertState() {
  theftAlert = hasUnreturnedItems();
  updateLEDs();
}

void stopBuzzer() {
  noTone(BUZZER_PIN);
  buzzerIsOn = false;
}

void updateBuzzer() {
  bool shouldBeep = hasUnreturnedItems();

  if (!shouldBeep) {
    stopBuzzer();
    return;
  }

  // Repeating beep pattern without blocking button handling.
  unsigned long now = millis();

  if (now - lastBeepUpdate >= 300) {
    lastBeepUpdate = now;
    buzzerIsOn = !buzzerIsOn;

    if (buzzerIsOn) {
      tone(BUZZER_PIN, 2000);
    } else {
      noTone(BUZZER_PIN);
    }
  }
}

void showTemporaryMessage(
  String line1,
  String line2,
  unsigned long duration) {
  messageLine1 = line1;
  messageLine2 = line2;
  messageUntil = millis() + duration;
  temporaryMessageActive = true;
  drawDisplay();
}

// ---------------- OLED DISPLAY ----------------
void drawDisplay() {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(1);
  display.setTextWrap(false);

  // Temporary message takes priority.
  if (temporaryMessageActive && millis() < messageUntil) {
    display.setTextSize(1);
    display.setCursor(0, 15);
    display.println(messageLine1);
    display.setCursor(0, 32);
    display.println(messageLine2);
    display.display();
    return;
  }

  temporaryMessageActive = false;

  // Welcome screen when billing is not active.
  if (!billingStarted) {
    display.setCursor(8, 5);
    display.println("SMART TROLLEY");
    display.setCursor(25, 17);
    display.println("SYSTEM READY");

    display.drawLine(0, 29, 127, 29, SH110X_WHITE);

    display.setCursor(0, 36);
    display.println("Press D32 to");

    display.setCursor(0, 47);
    display.println("START BILLING");

    display.display();
    return;
  }

  // Billing/cart screen.
  display.setCursor(0, 0);
  display.println("SMART TROLLEY - BILL");

  display.drawLine(0, 10, 127, 10, SH110X_WHITE);

  int y = 13;

  for (int i = 0; i < PRODUCT_COUNT; i++) {
    if (quantities[i] > 0) {
      display.setCursor(0, y);
      display.print(productNames[i]);
      display.print(" x");
      display.print(quantities[i]);
      display.setCursor(91, y);
      display.print("Rs");
      display.print(quantities[i] * productPrices[i]);

      y += 11;
    }
  }

  display.drawLine(0, 48, 127, 48, SH110X_WHITE);
  display.setCursor(0, 51);
  display.print("TOTAL: Rs ");
  display.print(total);

  display.display();
}

// ---------------- RFID HELPERS ----------------
String readUID() {
  String uid = "";

  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) {
      uid += "0";
    }

    uid += String(rfid.uid.uidByte[i], HEX);

    if (i < rfid.uid.size - 1) {
      uid += " ";
    }
  }

  uid.toUpperCase();
  return uid;
}

int findProduct(String uid) {
  for (int i = 0; i < PRODUCT_COUNT; i++) {
    if (uid == productUIDs[i]) {
      return i;
    }
  }

  return -1;
}

void addProduct(String uid) {
  if (!billingStarted || removalAllowed) {
    return;
  }

  // Ignore this known tag.
  if (uid == "50 8C 3E 58") {
    showTemporaryMessage("TAG IGNORED", "Not a trolley item", 2000);
    return;
  }

  int index = findProduct(uid);

  if (index < 0) {
    showTemporaryMessage("UNKNOWN TAG", "Product not found", 2000);
    return;
  }

  quantities[index]++;
  total += productPrices[index];

  showTemporaryMessage(
    "ITEM ADDED",
    productNames[index],
    1200);
}

void scanRFID() {
  if (!billingStarted || removalAllowed) {
    return;
  }

  if (!rfid.PICC_IsNewCardPresent()) {
    return;
  }

  if (!rfid.PICC_ReadCardSerial()) {
    return;
  }

  String uid = readUID();
  addProduct(uid);

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

// ---------------- BUTTON 1: UNAUTHORIZED REMOVAL ----------------

void handleTheftButton() {
  // If removal is already authorized, do nothing.
  if (removalAllowed) {
    return;
  }

  // Count the items currently available in the trolley.
  int availableItems = getTotalItems();

  // Do not allow more simulated removals than available items.
  if (availableItems <= 0) {
    showTemporaryMessage(
      "NO ITEMS LEFT",
      "Removal blocked",
      1500);
    return;
  }

  if (stackTop >= MAX_STACK - 1) {
    showTemporaryMessage(
      "STACK FULL",
      "Return items first",
      1500);
    return;
  }

  // Select the last available product type.
  int removedIndex = -1;

  for (int i = PRODUCT_COUNT - 1; i >= 0; i--) {
    if (quantities[i] > 0) {
      removedIndex = i;
      break;
    }
  }

  if (removedIndex == -1) {
    return;
  }

  // Record exactly one removal.
  quantities[removedIndex]--;
  total -= productPrices[removedIndex];

  removalStack[++stackTop] = removedIndex;

  theftAlert = true;
  updateLEDs();

  showTemporaryMessage(
    "THEFT ALERT!",
    "Return item! D14",
    1500);
}


// ---------------- BUTTON 2: BILLING COMPLETE ----------------
// ---------------- BUTTON 2: BILLING COMPLETE ----------------
void handleAllowRemovalButton() {
  if (!billingStarted) {
    showTemporaryMessage("NO ACTIVE BILL", "Press D32 first", 2000);
    return;
  }

  // Do not complete billing while theft items are unreturned.
  if (hasUnreturnedItems()) {
    showTemporaryMessage(
      "ITEMS UNRETURNED",
      "Clear alert first",
      2000);
    return;
  }

  // Reset dashboard display
  dashboardReset = true;

  // Complete billing and allow removal
  billingStarted = false;
  removalAllowed = true;

  // Clear all products from the cart
  for (int i = 0; i < PRODUCT_COUNT; i++) {
    quantities[i] = 0;
  }

  // Reset total bill
  total = 0;
  scrollOffset = 0;

  // Show completion message
  showTemporaryMessage(
    "BILLING COMPLETE",
    "REMOVAL ALLOWED",
    5000);
}

// ---------------- BUTTON 3: START BILLING ----------------
void handleStartBillingButton() {
  if (billingStarted) {
    showTemporaryMessage("BILLING ACTIVE", "Scan your items", 1500);
    return;
  }

  if (!removalAllowed) {
    showTemporaryMessage(
      "CANNOT START",
      "Press D26 first",
      2000);
    return;
  }

  if (hasUnreturnedItems()) {
    showTemporaryMessage(
      "THEFT ALERT ACTIVE",
      "Return all items",
      2000);
    return;
  }

  // Begin a new billing session.
  dashboardReset = false;
  billingStarted = true;
  removalAllowed = false;

  for (int i = 0; i < PRODUCT_COUNT; i++) {
    quantities[i] = 0;
  }

  total = 0;
  scrollOffset = 0;

  showTemporaryMessage(
    "BILLING STARTED",
    "Scan RFID items",
    1500);
}

// ---------------- BUTTON 4: CONFIRM RETURN ----------------
void handleReturnButton() {
  if (!hasUnreturnedItems()) {
    showTemporaryMessage(
      "NO ITEMS TO RETURN",
      "No pending alert",
      1500);
    return;
  }

  // LIFO: return the most recently removed item first.
  int returnedIndex = removalStack[stackTop];
  stackTop--;

  if (returnedIndex >= 0 && returnedIndex < PRODUCT_COUNT) {
    quantities[returnedIndex]++;
    total += productPrices[returnedIndex];
  }

  updateAlertState();

  if (hasUnreturnedItems()) {
    showTemporaryMessage(
      "ITEM RETURNED",
      "More items pending",
      1200);
  } else {
    showTemporaryMessage(
      "ALL ITEMS RETURNED",
      "ALERT CLEARED",
      2000);
  }
}


const char dashboardPage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Smart Trolley Dashboard</title>
<style>
:root {
  color-scheme: dark;
  --bg: #0b1120;
  --panel: #151f32;
  --border: #29364c;
  --muted: #9aabc2;
  --blue: #60a5fa;
  --green: #4ade80;
  --red: #f87171;
}
* { box-sizing: border-box; }
body {
  margin: 0;
  background: var(--bg);
  color: #f8fafc;
  font-family: Arial, sans-serif;
}
main { max-width: 1000px; margin: auto; padding: 24px 16px; }
header {
  display: flex; justify-content: space-between;
  align-items: center; flex-wrap: wrap; gap: 12px;
  margin-bottom: 24px;
}
h1 { margin: 0; font-size: 25px; }
.subtitle { color: var(--muted); margin-top: 7px; font-size: 14px; }
.pill {
  padding: 8px 12px; border-radius: 20px;
  background: #183b2a; color: var(--green); font-size: 13px;
}
.grid {
  display: grid; grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 14px; margin-bottom: 18px;
}
.card {
  background: var(--panel); border: 1px solid var(--border);
  border-radius: 14px; padding: 18px;
}
.label { color: var(--muted); font-size: 12px; text-transform: uppercase; }
.value { font-size: 27px; font-weight: bold; margin-top: 10px; }
.section-title { font-size: 18px; margin: 0 0 16px; }
table { width: 100%; border-collapse: collapse; }
th, td {
  padding: 13px 5px; text-align: left;
  border-bottom: 1px solid var(--border); font-size: 14px;
}
th { color: var(--muted); font-weight: normal; }
td:last-child, th:last-child { text-align: right; }
.total {
  display: flex; justify-content: space-between;
  padding-top: 18px; font-size: 19px; font-weight: bold;
}
.status { font-size: 17px; font-weight: bold; margin-top: 10px; }
.good { color: var(--green); }
.bad { color: var(--red); }
.info { color: var(--blue); }
.footer { color: var(--muted); font-size: 12px; margin-top: 18px; }
@media(min-width:700px) {
  .grid { grid-template-columns: repeat(4, minmax(0, 1fr)); }
}
</style>
</head>
<body>
<main>
<header>
  <div>
    <h1>SMART TROLLEY</h1>
    <div class="subtitle">IoT Real-Time Billing & Theft Prevention</div>
  </div>
  <div class="pill" id="connection">Connecting...</div>
</header>

<div class="grid">
  <section class="card">
    <div class="label">Total Bill</div>
    <div class="value" id="total">₹0</div>
  </section>
  <section class="card">
    <div class="label">Items in Cart</div>
    <div class="value" id="items">0</div>
  </section>
  <section class="card">
    <div class="label">Billing Status</div>
    <div class="status info" id="billing">Waiting</div>
  </section>
  <section class="card">
    <div class="label">Security Status</div>
    <div class="status good" id="security">Secure</div>
    <div class="subtitle" id="pending">0 pending returns</div>
  </section>
</div>

<section class="card">
  <h2 class="section-title">Shopping Cart</h2>
  <table>
    <thead>
      <tr><th>Product</th><th>Qty</th><th>Unit Price</th><th>Amount</th></tr>
    </thead>
    <tbody id="cart">
      <tr><td colspan="4">Waiting for ESP32 data...</td></tr>
    </tbody>
  </table>
  <div class="total"><span>Grand Total</span><span id="grandTotal">₹0</span></div>
</section>

<section class="card" style="margin-top:16px">
  <div class="label">Removal Authorization</div>
  <div class="status info" id="removal">Not authorized</div>
  <div class="subtitle" id="updated">Waiting for first update</div>
</section>

<div class="footer">
  Live data from your ESP32 · Updates automatically every second
</div>
</main>

<script>
const money = n => '₹' + Number(n).toLocaleString('en-IN');

async function refreshDashboard() {
  try {
    const response = await fetch('/api', {cache: 'no-store'});
    if (!response.ok) throw new Error('HTTP ' + response.status);
    const d = await response.json();

    document.getElementById('connection').textContent = '● Connected';
    document.getElementById('total').textContent = money(d.total);
    document.getElementById('grandTotal').textContent = money(d.total);
    document.getElementById('items').textContent = d.itemCount;

    const billing = document.getElementById('billing');
    billing.textContent = d.billingStarted ? 'Active' : 'Not active';
    billing.className = 'status ' + (d.billingStarted ? 'good' : 'info');

    const security = document.getElementById('security');
    security.textContent = d.alert ? 'THEFT ALERT' : 'Secure';
    security.className = 'status ' + (d.alert ? 'bad' : 'good');
    document.getElementById('pending').textContent =
      d.pending + ' pending return(s)';

    const removal = document.getElementById('removal');
    removal.textContent = d.removalAllowed ? 'Removal allowed' : 'Not authorized';
    removal.className = 'status ' + (d.removalAllowed ? 'good' : 'info');

    const cart = document.getElementById('cart');
    cart.replaceChildren();

    let hasItems = false;
    d.products.forEach(p => {
      if (p.qty <= 0) return;
      hasItems = true;

      const row = document.createElement('tr');
      [p.name, p.qty, money(p.price), money(p.qty * p.price)]
        .forEach(value => {
          const cell = document.createElement('td');
          cell.textContent = value;
          row.appendChild(cell);
        });
      cart.appendChild(row);
    });

    if (!hasItems) {
      const row = document.createElement('tr');
      const cell = document.createElement('td');
      cell.colSpan = 4;
      cell.textContent = 'No items in cart';
      row.appendChild(cell);
      cart.appendChild(row);
    }

    document.getElementById('updated').textContent =
      'Last updated: ' + new Date().toLocaleTimeString();
  } catch (error) {
    document.getElementById('connection').textContent = '● Disconnected';
    document.getElementById('connection').style.background = '#482326';
    document.getElementById('connection').style.color = '#f87171';
    document.getElementById('updated').textContent =
      'Cannot reach ESP32. Checking again...';
  }
}

refreshDashboard();
setInterval(refreshDashboard, 1000);
</script>
</body>
</html>
)rawliteral";



void setupDashboardRoutes() {
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", dashboardPage);
  });

  server.on("/api", HTTP_GET, []() {
    String json = "{";

    json += "\"total\":" + String(dashboardReset ? 0 : total) + ",";
    json += "\"itemCount\":" + String(dashboardReset ? 0 : getTotalItems()) + ",";
    json += "\"billingStarted\":";
    json += (billingStarted ? "true" : "false");
    json += ",";
    json += "\"removalAllowed\":";
    json += (removalAllowed ? "true" : "false");
    json += ",";
    json += "\"alert\":";
    json += ((theftAlert || hasUnreturnedItems()) ? "true" : "false");
    json += ",";
    json += "\"pending\":" + String(stackTop + 1) + ",";
    json += "\"products\":[";

    for (int i = 0; i < PRODUCT_COUNT; i++) {
      if (i > 0) json += ",";

      json += "{";
      json += "\"name\":\"" + productNames[i] + "\",";
      json += "\"qty\":" + String(quantities[i]) + ",";
      json += "\"price\":" + String(productPrices[i]);
      json += "}";
    }

    json += "]}";
    server.send(200, "application/json", json);
  });

  server.onNotFound([]() {
    server.send(404, "text/plain", "Not found");
  });

  server.begin();
  Serial.println("Dashboard web server started");
}


// ---------------- SETUP ----------------
void setup() {
  Serial.begin(115200);

  pinMode(BUTTON1, INPUT_PULLUP);
  pinMode(BUTTON2, INPUT_PULLUP);
  pinMode(BUTTON3, INPUT_PULLUP);
  pinMode(BUTTON4, INPUT_PULLUP);

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);

  stopBuzzer();

  // Preserve the working OLED configuration.
  Wire.begin(21, 22);
  Wire.setClock(100000);

  if (!display.begin(0x3C, true)) {
    Serial.println("OLED initialization failed!");
  } else {
    display.setRotation(0);
    display.clearDisplay();
    display.setTextColor(SH110X_WHITE);
    display.display();
  }

  // RFID reader: SCK, MISO, MOSI, SS.
  SPI.begin(18, 19, 23, SS_PIN);
  rfid.PCD_Init();

  // Initial state: ready for the first billing session.
  billingStarted = false;
  removalAllowed = true;
  theftAlert = false;

  updateLEDs();
  drawDisplay();

  Serial.println("Smart Trolley System Ready");


  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to home Wi-Fi");

  unsigned long wifiStart = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println("Wi-Fi connected!");
    Serial.print("Dashboard address: http://");
    Serial.println(WiFi.localIP());

    setupDashboardRoutes();
  } else {
    Serial.println();
    Serial.println("Wi-Fi connection failed.");
    Serial.println("Check SSID and password, then reset ESP32.");
  }
}

// ---------------- MAIN LOOP ----------------
void loop() {
  server.handleClient();
  unsigned long now = millis();

  bool button1 = digitalRead(BUTTON1);
  bool button2 = digitalRead(BUTTON2);
  bool button3 = digitalRead(BUTTON3);
  bool button4 = digitalRead(BUTTON4);

  // Detect button press edges (HIGH -> LOW).
  if (previousButton1 == HIGH && button1 == LOW) {
    handleTheftButton();
  }

  if (previousButton2 == HIGH && button2 == LOW) {
    handleAllowRemovalButton();
  }

  if (previousButton3 == HIGH && button3 == LOW) {
    handleStartBillingButton();
  }

  if (previousButton4 == HIGH && button4 == LOW) {
    handleReturnButton();
  }

  previousButton1 = button1;
  previousButton2 = button2;
  previousButton3 = button3;
  previousButton4 = button4;

  // Scan RFID only while billing is active.
  scanRFID();

  // Keep the buzzer active until all unauthorized removals
  // have been confirmed returned.
  updateBuzzer();
  updateLEDs();

  // Expire temporary messages.
  if (temporaryMessageActive && now >= messageUntil) {
    temporaryMessageActive = false;
    drawDisplay();
  }

  // Refresh the cart display periodically.
  if (!temporaryMessageActive && now - lastDisplayUpdate >= 250) {
    lastDisplayUpdate = now;
    drawDisplay();
  }

  delay(10);
}

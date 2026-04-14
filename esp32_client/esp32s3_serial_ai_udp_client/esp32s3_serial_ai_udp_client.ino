#include <Arduino.h>
#include <string.h>
#include <Preferences.h>

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiUdp.h>

#ifndef ENABLE_BLE_CLIENT
// Disable by default to reduce flash usage.
#define ENABLE_BLE_CLIENT 0
#endif
#if ENABLE_BLE_CLIENT
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEClient.h>
#include <BLERemoteService.h>
#include <BLERemoteCharacteristic.h>
#endif

#ifndef ENABLE_I2S_MIC
// Disable by default to reduce flash usage.
#define ENABLE_I2S_MIC 1
#endif
#ifndef ENABLE_TTS
// Disable by default to reduce flash usage.
#define ENABLE_TTS 0
#endif
#if !ENABLE_I2S_MIC
#undef ENABLE_TTS
#define ENABLE_TTS 0
#endif
#ifndef MIC_MAX_RECORD_SECONDS
// 愈短上傳愈快、雲端 STT 愈快（辨識品質略降）；可改 2
#define MIC_MAX_RECORD_SECONDS 10
#endif
// 等 STT HTTP 回應：雲端用 OpenAI 時通常 <1 分鐘；若仍用 Poe 請改大（例如 360）
#ifndef STT_HTTP_READ_TIMEOUT_SEC
#define STT_HTTP_READ_TIMEOUT_SEC 120
#endif

#if ENABLE_I2S_MIC
#include <ESP_I2S.h>
#include "esp_heap_caps.h"
#endif

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#define WIFI_SSID "GAN"
#define WIFI_PASS "chen0605"
#define STT_URL   "https://magicwandmain-production.up.railway.app/api/stt"
#define TTS_URL   "https://magicwandmain-production.up.railway.app/api/tts"
#define CHAT_URL  "https://magicwandmain-production.up.railway.app/api/chat"
#define LOG_URL  "https://magicwandmain-production.up.railway.app/api/log"
#define DEVICE_ID "esp32s3_ai_client_001"
#endif

#ifndef UDP_TARGET_IP
#define UDP_TARGET_IP "10.232.188.113"
#endif
#ifndef UDP_TARGET_PORT
#define UDP_TARGET_PORT 4211
#endif

// PC sends natural-language lines over WiFi UDP here (same pipeline as Serial -> processUserTextLine).
// Must differ from C3 command port (UDP_TARGET_PORT).
#ifndef S3_PC_CMD_UDP_PORT
#define S3_PC_CMD_UDP_PORT 4209
#endif

// Fixed local port for the socket used to talk to C3 (S3 -> C3 UDP). Using begin(0) (random source port)
// can make C3 replies unreliable on some builds; C3 always replies to remoteIP:remotePort of the request.
#ifndef S3_C3_UDP_LOCAL_PORT
#define S3_C3_UDP_LOCAL_PORT 4213
#endif

#if ENABLE_I2S_MIC && !defined(STT_URL)
#define STT_URL "https://magicwandmain-production.up.railway.app/api/stt"
#endif
#if ENABLE_I2S_MIC && ENABLE_TTS && !defined(TTS_URL)
#define TTS_URL "https://magicwandmain-production.up.railway.app/api/tts"
#endif

#if ENABLE_I2S_MIC
static const int PIN_BCLK = 14;
static const int PIN_WS = 13;
static const int PIN_DOUT = 11;
static const int PIN_DIN = 12;

static const uint32_t SAMPLE_RATE = 16000;
static const uint16_t BITS_PER_SAMPLE = 16;
static const uint16_t CHANNELS = 1;
static const size_t REC_CHUNK_FRAMES = 256;
static int g_recShift = 16;
static bool g_micUseRight = true;

static I2SClass I2S;

static bool g_recording = false;
static uint8_t* g_wavBuf = nullptr;
static size_t g_wavCap = 0;
static uint32_t g_pcmDataBytes = 0;
static uint32_t g_recStartMs = 0;
static uint32_t g_recMaxMs = 0;
static String g_serialLineBuf;
#endif

static WiFiUDP g_udp;
static WiFiUDP g_udpPcCmd;
static IPAddress g_targetIp;

// When processUserTextLine() is triggered from pollWifiPcCommand(), reply with AI text over UDP.
static IPAddress g_wifiPcReplyIp;
static uint16_t g_wifiPcReplyPort = 0;

// Avoid large stack allocations in loopTask (can trigger stack canary / Guru Meditation).
static char g_pcUdpBuf[2048];

#ifndef UDP_TARGET_MAX
#define UDP_TARGET_MAX 8
#endif

static String g_udpNames[UDP_TARGET_MAX];
static IPAddress g_udpIps[UDP_TARGET_MAX];
static int g_udpCount = 0;
static int g_udpCurrent = 0;
static bool g_udpSendEnabled = true;

static bool g_run_wifi = true;
static bool g_run_ble = true;
static bool g_run_mic = true;

static bool g_udpSockStarted = false;
static bool g_udpPcCmdStarted = false;
static uint32_t g_wifiRetryAtMs = 0;

static void wifiTick() {
  if (!g_run_wifi) return;
  if (WiFi.status() == WL_CONNECTED) {
    if (!g_udpSockStarted) {
      if (!g_udp.begin((uint16_t)S3_C3_UDP_LOCAL_PORT)) {
        Serial.println("[BOOT] WARN: g_udp fixed port bind failed; using ephemeral port");
        g_udp.begin(0);
      }
      g_udpSockStarted = true;
      g_udpPcCmd.begin((uint16_t)S3_PC_CMD_UDP_PORT);
      g_udpPcCmdStarted = true;
      Serial.print("[BOOT] WiFi STA IP: ");
      Serial.println(WiFi.localIP());
      Serial.print("[BOOT] C3 command UDP local port: ");
      Serial.println((unsigned)S3_C3_UDP_LOCAL_PORT);
      Serial.print("[BOOT] PC text (UDP) listen port: ");
      Serial.println((unsigned)S3_PC_CMD_UDP_PORT);
    }
    return;
  }

  if (g_udpSockStarted) {
    g_udp.stop();
    g_udpSockStarted = false;
  }
  if (g_udpPcCmdStarted) {
    g_udpPcCmd.stop();
    g_udpPcCmdStarted = false;
  }

  uint32_t now = millis();
  if (now < g_wifiRetryAtMs) return;
  g_wifiRetryAtMs = now + 5000;

  Serial.println("[WiFi] reconnect...");
  WiFi.disconnect(true);
  delay(50);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

#if ENABLE_BLE_CLIENT
static const char* BLE_PEER_NAME = "MagicWand-C3";
static const char* NUS_SERVICE_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static const char* NUS_RX_UUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
static BLEClient* g_bleClient = nullptr;
static BLERemoteCharacteristic* g_bleRxChar = nullptr;
static bool g_bleConnected = false;
static uint32_t g_bleLastScanMs = 0;
static BLEAddress* g_blePendingAddr = nullptr;

class BleClientCallbacks : public BLEClientCallbacks {
  void onDisconnect(BLEClient* c) {
    (void)c;
    g_bleConnected = false;
    g_bleRxChar = nullptr;
    Serial.println("[BLE] disconnected");
  }
};
static BleClientCallbacks g_bleClientCb;

class BleAdvCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (!advertisedDevice.haveName()) return;
    if (strcmp(advertisedDevice.getName().c_str(), BLE_PEER_NAME) != 0) return;
    BLEDevice::getScan()->stop();
    if (g_blePendingAddr) {
      delete g_blePendingAddr;
      g_blePendingAddr = nullptr;
    }
    g_blePendingAddr = new BLEAddress(advertisedDevice.getAddress());
  }
};
static BleAdvCallbacks g_bleAdvCb;

static bool bleConnectToAddress(const BLEAddress& addr) {
  if (!g_bleClient) return false;
  if (!g_bleClient->connect(addr)) {
    Serial.println("[BLE] connect failed");
    return false;
  }
  BLERemoteService* svc = g_bleClient->getService(NUS_SERVICE_UUID);
  if (!svc) {
    Serial.println("[BLE] NUS service not found");
    g_bleClient->disconnect();
    return false;
  }
  g_bleRxChar = svc->getCharacteristic(NUS_RX_UUID);
  if (!g_bleRxChar) {
    Serial.println("[BLE] RX characteristic not found");
    g_bleClient->disconnect();
    return false;
  }
  g_bleConnected = true;
  Serial.println("[BLE] connected — same payload as UDP (ON/OFF/Bxx)");
  return true;
}

static void bleMirrorCommand(const char* cmd) {
  if (!g_run_ble) return;
  if (!g_udpSendEnabled) return;
  if (!g_bleConnected || !g_bleRxChar) return;
  size_t len = strlen(cmd);
  if (len == 0 || len > 64) return;
  g_bleRxChar->writeValue((uint8_t*)cmd, len, false);
  Serial.print("[BLE] ");
  Serial.println(cmd);
}

static void bleClientInit() {
  BLEDevice::init("");
  g_bleClient = BLEDevice::createClient();
  g_bleClient->setClientCallbacks(&g_bleClientCb);
  Serial.println("[BOOT] BLE client (Bluedroid): scan for \"" + String(BLE_PEER_NAME) + "\"");
}

static void bleClientTick() {
  if (g_bleConnected) return;
  uint32_t now = millis();
  if (now - g_bleLastScanMs < 8000) return;
  g_bleLastScanMs = now;
  BLEScan* scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(&g_bleAdvCb, false);
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  g_blePendingAddr = nullptr;
  scan->start(5, false);
  if (g_blePendingAddr) {
    if (bleConnectToAddress(*g_blePendingAddr)) {
      delete g_blePendingAddr;
      g_blePendingAddr = nullptr;
      return;
    }
    delete g_blePendingAddr;
    g_blePendingAddr = nullptr;
  }
}
#endif

static bool parseIp(const char* s, IPAddress& out) {
  int a, b, c, d;
  if (sscanf(s, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return false;
  if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255) return false;
  out = IPAddress((uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d);
  return true;
}

static void syncTargetIpFromIndex() {
  if (g_udpCount > 0 && g_udpCurrent >= 0 && g_udpCurrent < g_udpCount) {
    g_targetIp = g_udpIps[g_udpCurrent];
  } else {
    g_targetIp = IPAddress(0, 0, 0, 0);
  }
}

static void udpPairsFromString(const String& s) {
  g_udpCount = 0;
  unsigned start = 0;
  while (start < s.length() && g_udpCount < UDP_TARGET_MAX) {
    int semi = s.indexOf(';', (int)start);
    String seg = (semi < 0) ? s.substring(start) : s.substring(start, (unsigned)semi);
    seg.trim();
    if (seg.length() == 0) break;
    int pipe = seg.indexOf('|');
    if (pipe < 0) break;
    String nm = seg.substring(0, pipe);
    String ipStr = seg.substring(pipe + 1);
    nm.trim();
    ipStr.trim();
    IPAddress ip;
    if (nm.length() && parseIp(ipStr.c_str(), ip)) {
      g_udpNames[g_udpCount] = nm;
      g_udpIps[g_udpCount] = ip;
      g_udpCount++;
    }
    if (semi < 0) break;
    start = (unsigned)semi + 1;
  }
}

static String udpPairsToString() {
  String o;
  for (int i = 0; i < g_udpCount; i++) {
    if (i) o += ";";
    o += g_udpNames[i];
    o += "|";
    o += g_udpIps[i].toString();
  }
  return o;
}

static void udpTargetsSave() {
  Preferences p;
  if (!p.begin("c3udp", false)) return;
  p.putString("pairs", udpPairsToString());
  p.putUInt("cur", (unsigned)g_udpCurrent);
  p.putBool("send", g_udpSendEnabled);
  p.end();
}

static void udpTargetsLoad() {
  Preferences p;
  if (!p.begin("c3udp", true)) {
    g_udpSendEnabled = true;
    g_udpCount = 0;
    IPAddress ip;
    if (parseIp(UDP_TARGET_IP, ip)) {
      g_udpNames[0] = "default";
      g_udpIps[0] = ip;
      g_udpCount = 1;
      g_udpCurrent = 0;
      udpTargetsSave();
    }
    syncTargetIpFromIndex();
    return;
  }
  String pairs = p.getString("pairs", "");
  g_udpCurrent = (int)p.getUInt("cur", 0);
  g_udpSendEnabled = p.getBool("send", true);
  p.end();

  if (pairs.length() == 0) {
    g_udpCount = 0;
    IPAddress ip;
    if (parseIp(UDP_TARGET_IP, ip)) {
      g_udpNames[0] = "default";
      g_udpIps[0] = ip;
      g_udpCount = 1;
      g_udpCurrent = 0;
      udpTargetsSave();
    }
  } else {
    udpPairsFromString(pairs);
    if (g_udpCount == 0) {
      IPAddress ip;
      if (parseIp(UDP_TARGET_IP, ip)) {
        g_udpNames[0] = "default";
        g_udpIps[0] = ip;
        g_udpCount = 1;
        g_udpCurrent = 0;
        udpTargetsSave();
      }
    } else if (g_udpCurrent < 0 || g_udpCurrent >= g_udpCount) {
      g_udpCurrent = 0;
      udpTargetsSave();
    }
  }
  syncTargetIpFromIndex();
}

static void udpPrintHelp() {
  Serial.println("--- UDP 多個 ESP32-C3 ---");
  Serial.println("udp list              列出已儲存目標與目前選中");
  Serial.println("udp add <名稱> <IP>   新增（例: udp add room1 192.168.1.50）");
  Serial.println("udp use <名稱|序號>  切換目標並連接送出（同 connect）");
  Serial.println("udp connect [名稱|序號]  連接：有參數時同 udp use；無參數時只恢復送出");
  Serial.println("udp disconnect        斷開：暫停送 UDP（清單保留，之後 udp connect）");
  Serial.println("udp del <名稱|序號>  刪除一筆");
  Serial.println("udp clear             清空 NVS；若有 UDP_TARGET_IP 則恢復一筆 default");
}

static void udpPrintList() {
  Serial.println("--- UDP 目標 (port=" + String(UDP_TARGET_PORT) + ") ---");
  for (int i = 0; i < g_udpCount; i++) {
    Serial.print(i);
    Serial.print(": ");
    Serial.print(g_udpNames[i]);
    Serial.print("  ");
    Serial.print(g_udpIps[i]);
    if (i == g_udpCurrent) Serial.print("  <-- 目前");
    Serial.println();
  }
  Serial.print("UDP 送出: ");
  Serial.println(g_udpSendEnabled ? "開啟（已連接）" : "關閉（已斷開，不送指令）");
  Serial.print("目前送出指令 -> ");
  Serial.print(g_targetIp);
  Serial.print(":");
  Serial.println(UDP_TARGET_PORT);
}

static int udpFindName(const String& name) {
  for (int i = 0; i < g_udpCount; i++) {
    if (g_udpNames[i].equalsIgnoreCase(name)) return i;
  }
  return -1;
}

static bool udpSelectByNameOrIndex(const String& u) {
  String u2 = u;
  u2.trim();
  if (u2.length() == 0) return false;
  int idx = -1;
  bool allDigits = true;
  for (unsigned i = 0; i < u2.length(); i++) {
    if (u2[i] < '0' || u2[i] > '9') {
      allDigits = false;
      break;
    }
  }
  if (allDigits && u2.length() > 0) {
    idx = u2.toInt();
  } else {
    idx = udpFindName(u2);
  }
  if (idx < 0 || idx >= g_udpCount) return false;
  g_udpCurrent = idx;
  syncTargetIpFromIndex();
  return true;
}

static bool udpResolveAiDeviceTarget(const String& raw) {
  String s = raw;
  s.trim();
  if (s.length() == 0) return true;
  if (s.equalsIgnoreCase("none")) return true;
  int idx = udpFindName(s);
  if (idx < 0) {
    Serial.print("[UDP] AI 目標名稱未找到: ");
    Serial.println(s);
    return false;
  }
  g_udpCurrent = idx;
  udpTargetsSave();
  syncTargetIpFromIndex();
  Serial.print("[UDP] AI 選目標 -> ");
  Serial.print(g_udpNames[g_udpCurrent]);
  Serial.print(" ");
  Serial.println(g_targetIp);
  return true;
}

static void udpApplyAiDeviceLink(const String& raw) {
  String s = raw;
  s.trim();
  if (s.length() == 0) return;
  String sl = s;
  sl.toLowerCase();
  if (sl == "none") return;
  if (sl == "disconnect") {
    g_udpSendEnabled = false;
    udpTargetsSave();
    Serial.println("[UDP] AI：斷開連接（不送 UDP，直至 AI 或 Serial 恢復連接）");
    return;
  }
  if (sl == "connect") {
    g_udpSendEnabled = true;
    udpTargetsSave();
    Serial.println("[UDP] AI：恢復連接（允許送 UDP）");
  }
}

static bool tryHandleUdpTargetLine(const String& line) {
  if (line.length() < 3) return false;
  if (!line.substring(0, 3).equalsIgnoreCase("udp")) return false;

  String rest = line.substring(3);
  rest.trim();
  if (rest.length() == 0 || rest.equalsIgnoreCase("help")) {
    udpPrintHelp();
    return true;
  }

  String rlow = rest;
  rlow.toLowerCase();

  if (rlow == "list") {
    udpPrintList();
    return true;
  }

  if (rlow == "disconnect") {
    g_udpSendEnabled = false;
    udpTargetsSave();
    Serial.println("[UDP] 已斷開：暫不向裝置送 UDP（udp connect 可恢復）");
    udpPrintList();
    return true;
  }

  if (rlow == "connect" || rlow.startsWith("connect ")) {
    String u = (rlow == "connect") ? "" : rest.substring(8);
    u.trim();
    if (u.length() == 0) {
      if (!g_targetIp) {
        Serial.println("[UDP] 無有效目標，請先 udp add");
        return true;
      }
      g_udpSendEnabled = true;
      udpTargetsSave();
      Serial.println("[UDP] 已連接送出（使用目前選中目標）");
      udpPrintList();
      return true;
    }
    if (!udpSelectByNameOrIndex(u)) {
      Serial.println("[UDP] 找不到目標");
      return true;
    }
    g_udpSendEnabled = true;
    udpTargetsSave();
    Serial.println("[UDP] 已連接並切換目標");
    udpPrintList();
    return true;
  }

  if (rlow == "clear") {
    Preferences p;
    if (p.begin("c3udp", false)) {
      p.clear();
      p.end();
    }
    g_udpCount = 0;
    IPAddress ip;
    if (parseIp(UDP_TARGET_IP, ip)) {
      g_udpNames[0] = "default";
      g_udpIps[0] = ip;
      g_udpCount = 1;
      g_udpCurrent = 0;
      g_udpSendEnabled = true;
      udpTargetsSave();
      syncTargetIpFromIndex();
      Serial.println("[UDP] 已清空並恢復預設 IP");
    } else {
      g_udpSendEnabled = true;
      udpTargetsSave();
      syncTargetIpFromIndex();
      Serial.println("[UDP] 已清空；未設定 UDP_TARGET_IP，請 udp add");
    }
    udpPrintList();
    return true;
  }

  if (rlow.startsWith("add ")) {
    String a = rest.substring(4);
    a.trim();
    int sp = a.indexOf(' ');
    if (sp < 0) {
      Serial.println("[UDP] 用法: udp add <名稱> <IP>");
      return true;
    }
    String nm = a.substring(0, sp);
    String ipStr = a.substring(sp + 1);
    nm.trim();
    ipStr.trim();
    if (nm.length() == 0 || nm.indexOf('|') >= 0 || nm.indexOf(';') >= 0) {
      Serial.println("[UDP] 名稱不可為空或含 | ;");
      return true;
    }
    IPAddress ip;
    if (!parseIp(ipStr.c_str(), ip)) {
      Serial.println("[UDP] IP 格式錯誤");
      return true;
    }
    int ex = udpFindName(nm);
    if (ex >= 0) {
      g_udpIps[ex] = ip;
      Serial.println("[UDP] 已更新同名目標");
    } else {
      if (g_udpCount >= UDP_TARGET_MAX) {
        Serial.println("[UDP] 已滿，請先 udp del");
        return true;
      }
      g_udpNames[g_udpCount] = nm;
      g_udpIps[g_udpCount] = ip;
      g_udpCount++;
      Serial.println("[UDP] 已新增");
    }
    udpTargetsSave();
    syncTargetIpFromIndex();
    udpPrintList();
    return true;
  }

  if (rlow.startsWith("use ")) {
    String u = rest.substring(4);
    u.trim();
    if (u.length() == 0) {
      Serial.println("[UDP] 用法: udp use <名稱|序號>");
      return true;
    }
    if (!udpSelectByNameOrIndex(u)) {
      Serial.println("[UDP] 找不到目標");
      return true;
    }
    g_udpSendEnabled = true;
    udpTargetsSave();
    Serial.println("[UDP] 已切換（已連接送出）");
    udpPrintList();
    return true;
  }

  if (rlow.startsWith("del ")) {
    String u = rest.substring(4);
    u.trim();
    int idx = -1;
    bool allDigits = true;
    for (unsigned i = 0; i < u.length(); i++) {
      if (u[i] < '0' || u[i] > '9') {
        allDigits = false;
        break;
      }
    }
    if (allDigits && u.length() > 0) {
      idx = u.toInt();
    } else {
      idx = udpFindName(u);
    }
    if (idx < 0 || idx >= g_udpCount) {
      Serial.println("[UDP] 找不到目標");
      return true;
    }
    for (int i = idx; i < g_udpCount - 1; i++) {
      g_udpNames[i] = g_udpNames[i + 1];
      g_udpIps[i] = g_udpIps[i + 1];
    }
    g_udpCount--;
    if (g_udpCurrent >= g_udpCount) g_udpCurrent = g_udpCount - 1;
    if (g_udpCurrent < 0) g_udpCurrent = 0;
    udpTargetsSave();
    syncTargetIpFromIndex();
    Serial.println("[UDP] 已刪除");
    udpPrintList();
    return true;
  }

  udpPrintHelp();
  return true;
}

static bool udpSendCommand(const char* cmd) {
  if (!g_udpSendEnabled) {
    Serial.println("[UDP] 送出已關閉 — 請 udp connect 以連接並恢復");
    return false;
  }
  bool udpOk = false;
  if (g_run_wifi && WiFi.status() == WL_CONNECTED && g_targetIp) {
    g_udp.beginPacket(g_targetIp, (uint16_t)UDP_TARGET_PORT);
    g_udp.print(cmd);
    udpOk = g_udp.endPacket();
    Serial.print("[UDP] ");
    Serial.print(cmd);
    Serial.print(" -> ");
    Serial.print(g_targetIp);
    Serial.print(":");
    Serial.println(UDP_TARGET_PORT);
  } else if (g_run_wifi && WiFi.status() == WL_CONNECTED && !g_targetIp) {
    Serial.println("[UDP] 無有效目標 — 執行 udp list / udp add / udp use");
  }
  return udpOk;
}

static bool udpSendCommandWaitAck(const char* cmd, unsigned long timeoutMs) {
  if (!g_run_wifi || WiFi.status() != WL_CONNECTED) return false;
  if (!g_targetIp) return false;

  while (g_udp.parsePacket() > 0) {
    char tmp[32];
    (void)g_udp.read(tmp, (int)sizeof(tmp));
    delay(0);
  }

  bool sent = udpSendCommand(cmd);
  if (!sent) return false;

  unsigned long t0 = millis();
  while (millis() - t0 < timeoutMs) {
    int p = g_udp.parsePacket();
    if (p > 0) {
      IPAddress rip = g_udp.remoteIP();
      char buf[64];
      int n = g_udp.read(buf, (int)sizeof(buf) - 1);
      if (n > 0) {
        buf[n] = '\0';
        if (rip == g_targetIp && (strncmp(buf, "OK", 2) == 0)) {
          Serial.print("[ACK] ");
          Serial.println(buf);
          return true;
        }
      }
    }
    delay(2);
  }
  return false;
}

static bool deviceSendCommandAuto(const char* cmd) {
  if (!g_udpSendEnabled) {
    Serial.println("[PATH] 已斷開（udp disconnect）— 不送指令");
    return false;
  }

  if (g_run_wifi && WiFi.status() == WL_CONNECTED) {
    udpPrintList();
    if (udpSendCommandWaitAck(cmd, 120)) {
      return true;
    }
    Serial.println("[PATH] UDP 無回覆，改用 BLE（若已開啟/已連線）");
  }

#if ENABLE_BLE_CLIENT
  if (g_run_ble && g_bleConnected && g_bleRxChar) {
    bleMirrorCommand(cmd);
    return true;
  }
#endif

  if (g_run_wifi && WiFi.status() == WL_CONNECTED) {
    return udpSendCommand(cmd);
  }
  Serial.println("[PATH] WiFi 未連線且 BLE 未連線，未送出");
  return false;
}

static void pollIncomingUdpMessages() {
  if (!g_run_wifi || WiFi.status() != WL_CONNECTED) return;
  for (int i = 0; i < 4; i++) {
    int p = g_udp.parsePacket();
    if (p <= 0) return;
    IPAddress rip = g_udp.remoteIP();
    uint16_t rport = (uint16_t)g_udp.remotePort();
    char buf[256];
    int n = g_udp.read(buf, (int)sizeof(buf) - 1);
    if (n <= 0) continue;
    buf[n] = '\0';
    Serial.print("[UDP IN] ");
    Serial.print(rip);
    Serial.print(":");
    Serial.print(rport);
    Serial.print("  ");
    Serial.println(buf);
  }
}

static String extractC3CmdFromReply(const String& reply) {
  // Look for a line like: C3_CMD: <command>
  // Prefer the *last* occurrence so a corrected final line wins over earlier mistakes.
  int k = reply.lastIndexOf("C3_CMD:");
  if (k < 0) k = reply.lastIndexOf("c3_cmd:");
  if (k < 0) k = reply.lastIndexOf("C3:");
  if (k < 0) k = reply.lastIndexOf("c3:");
  if (k < 0) return "";
  int end = reply.indexOf('\n', k);
  String line = (end < 0) ? reply.substring(k) : reply.substring(k, end);
  int colon = line.indexOf(':');
  if (colon < 0) return "";
  String cmd = line.substring(colon + 1);
  cmd.trim();
  if (cmd.length() > 200) cmd = cmd.substring(0, 200);
  return cmd;
}

// Map generic AI tokens to the current C3 sketch (fan PWM + UDP).
static String normalizeC3UdpCommand(const String& cmd) {
  String t = cmd;
  t.trim();
  if (!t.length()) return t;
  String low = t;
  low.toLowerCase();
  if (low == "on" || low == "led_on") return "fan on";
  if (low == "off" || low == "led_off") return "fan off";
  return t;
}

static bool sendRawToC3(const String& cmd) {
  String t = normalizeC3UdpCommand(cmd);
  if (!t.length()) return false;
  Serial.print("[C3 CMD] ");
  Serial.println(t);
  return udpSendCommand(t.c_str());
}

// Drop stale UDP packets from C3 before sending a new command (same socket as udpSendCommand).
static void drainUdpFromC3() {
  if (!g_run_wifi || WiFi.status() != WL_CONNECTED) return;
  while (g_udp.parsePacket() > 0) {
    char tmp[256];
    while (g_udp.available() > 0) {
      (void)g_udp.read(tmp, (int)sizeof(tmp));
    }
  }
}

// Wait for one UDP reply from the current C3 target (after sendRawToC3).
static String waitUdpReplyFromC3(unsigned long timeoutMs) {
  String acc;
  if (!g_run_wifi || WiFi.status() != WL_CONNECTED || !g_targetIp) return acc;
  unsigned long t0 = millis();
  while (millis() - t0 < timeoutMs) {
    int p = g_udp.parsePacket();
    if (p > 0) {
      IPAddress rip = g_udp.remoteIP();
      if (rip != g_targetIp) {
        Serial.print("[C3 RX drop] from ");
        Serial.print(rip);
        Serial.print(" (expect ");
        Serial.print(g_targetIp);
        Serial.println(")");
        char tmp[256];
        while (g_udp.available() > 0) (void)g_udp.read(tmp, (int)sizeof(tmp));
        continue;
      }
      char buf[2048];
      int n = g_udp.read(buf, (int)sizeof(buf) - 1);
      if (n > 0) {
        buf[n] = '\0';
        acc += buf;
        while (g_udp.available() > 0) {
          n = g_udp.read(buf, (int)sizeof(buf) - 1);
          if (n > 0) {
            buf[n] = '\0';
            acc += buf;
          }
        }
        break;
      }
    }
    delay(2);
    yield();
  }
  acc.trim();
  return acc;
}

static String escapeJsonString(const String& s) {
  String o;
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '\\' || c == '"' || c == '\n' || c == '\r') {
      if (c == '\n') {
        o += "\\n";
        continue;
      }
      if (c == '\r') continue;
      o += '\\';
    }
    o += c;
  }
  return o;
}

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

static void appendUtf8(String& out, uint32_t cp) {
  if (cp <= 0x7F) {
    out += (char)cp;
  } else if (cp <= 0x7FF) {
    out += (char)(0xC0 | ((cp >> 6) & 0x1F));
    out += (char)(0x80 | (cp & 0x3F));
  } else if (cp <= 0xFFFF) {
    out += (char)(0xE0 | ((cp >> 12) & 0x0F));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  } else {
    out += (char)(0xF0 | ((cp >> 18) & 0x07));
    out += (char)(0x80 | ((cp >> 12) & 0x3F));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  }
}

static bool consumeJsonEscape(const String& json, unsigned& i, String& out) {
  if (i >= json.length()) return false;
  char n = json[i];
  if (n == 'n') {
    out += '\n';
    i++;
    return true;
  }
  if (n == 't') {
    out += '\t';
    i++;
    return true;
  }
  if (n == 'r') {
    i++;
    return true;
  }
  if (n == '"') {
    out += '"';
    i++;
    return true;
  }
  if (n == '\\') {
    out += '\\';
    i++;
    return true;
  }
  if (n != 'u') {
    out += n;
    i++;
    return true;
  }
  if (i + 4 >= json.length()) return false;
  uint32_t u1 = 0;
  for (int k = 0; k < 4; k++) {
    int v = hexVal(json[i + 1 + k]);
    if (v < 0) return false;
    u1 = (u1 << 4) | (uint32_t)v;
  }
  i += 5;
  if (u1 >= 0xD800 && u1 <= 0xDBFF) {
    if (i + 5 < json.length() && json[i] == '\\' && json[i + 1] == 'u') {
      uint32_t u2 = 0;
      for (int k = 0; k < 4; k++) {
        int v = hexVal(json[i + 2 + k]);
        if (v < 0) {
          appendUtf8(out, u1);
          return true;
        }
        u2 = (u2 << 4) | (uint32_t)v;
      }
      if (u2 >= 0xDC00 && u2 <= 0xDFFF) {
        uint32_t cp = 0x10000 + (((u1 - 0xD800) << 10) | (u2 - 0xDC00));
        appendUtf8(out, cp);
        i += 6;
        return true;
      }
    }
  }
  appendUtf8(out, u1);
  return true;
}

static String extractJsonStringField(const String& json, const char* field) {
  String key = "\"";
  key += field;
  key += "\"";
  int k = json.indexOf(key);
  if (k < 0) return "";
  int colon = json.indexOf(':', k);
  if (colon < 0) return "";
  int q = json.indexOf('"', colon + 1);
  if (q < 0) return "";
  String out;
  for (unsigned i = (unsigned)q + 1; i < json.length();) {
    char c = json[i];
    if (c == '\\' && i + 1 < json.length()) {
      i++;
      if (!consumeJsonEscape(json, i, out)) break;
      continue;
    }
    if (c == '"') break;
    out += c;
    i++;
  }
  return out;
}

static String udpTargetsJsonArray() {
  if (g_udpCount <= 0) return "[]";
  String j = "[";
  for (int i = 0; i < g_udpCount; i++) {
    if (i) j += ",";
    j += "{\"name\":\"";
    j += escapeJsonString(g_udpNames[i]);
    j += "\",\"ip\":\"";
    j += escapeJsonString(g_udpIps[i].toString());
    j += "\"}";
  }
  j += "]";
  return j;
}

static void httpPostLogBestEffort(const String& inputText, const String& outputText, const char* kind) {
  if (!g_run_wifi) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (!inputText.length() || !outputText.length()) return;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, LOG_URL)) return;
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(15000);
  String body = "{\"device_id\":\"";
  body += DEVICE_ID;
  body += "\",\"kind\":\"";
  body += kind;
  body += "\",\"input_text\":\"";
  body += escapeJsonString(inputText);
  body += "\",\"output_text\":\"";
  body += escapeJsonString(outputText);
  body += "\"}";
  (void)http.POST(body);
  http.end();
}

static bool httpPostChat(const String& text, String& outReply, String& outDeviceCmd, String& outDeviceTarget,
                         String& outDeviceLink, String& outTtsUrl) {
  outReply = "";
  outDeviceCmd = "";
  outDeviceTarget = "";
  outDeviceLink = "";
  outTtsUrl = "";
  if (!g_run_wifi) return false;
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, CHAT_URL)) return false;
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(120000);
  String body = "{\"device_id\":\"";
  body += DEVICE_ID;
  body += "\",\"message\":\"";
  body += escapeJsonString(text);
  body += "\",\"udp_targets\":";
  body += udpTargetsJsonArray();
#if ENABLE_I2S_MIC && ENABLE_TTS
  body += ",\"include_tts\":true";
#endif
  body += "}";
  int code = http.POST(body);
  String resp = http.getString();
  http.end();
  if (code != 200) {
    Serial.print("[CHAT HTTP ");
    Serial.print(code);
    Serial.println("]");
    Serial.println(resp);
    return false;
  }
  outReply = extractJsonStringField(resp, "reply");
  outDeviceCmd = extractJsonStringField(resp, "device_cmd");
  outDeviceTarget = extractJsonStringField(resp, "device_target");
  outDeviceLink = extractJsonStringField(resp, "device_link");
  outTtsUrl = extractJsonStringField(resp, "tts_absolute_url");
  if (!outReply.length()) {
    Serial.println("[CHAT] missing reply");
    Serial.println(resp);
    return false;
  }
  return true;
}

#if ENABLE_I2S_MIC
static void writeWavHeader(uint8_t* dst, uint32_t dataBytes) {
  const uint32_t byteRate = SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE / 8);
  const uint16_t blockAlign = CHANNELS * (BITS_PER_SAMPLE / 8);
  const uint32_t riffSize = 36 + dataBytes;

  auto W4 = [&](int off, uint32_t v) {
    dst[off + 0] = (uint8_t)(v & 0xFF);
    dst[off + 1] = (uint8_t)((v >> 8) & 0xFF);
    dst[off + 2] = (uint8_t)((v >> 16) & 0xFF);
    dst[off + 3] = (uint8_t)((v >> 24) & 0xFF);
  };
  auto W2 = [&](int off, uint16_t v) {
    dst[off + 0] = (uint8_t)(v & 0xFF);
    dst[off + 1] = (uint8_t)((v >> 8) & 0xFF);
  };

  memcpy(dst + 0, "RIFF", 4);
  W4(4, riffSize);
  memcpy(dst + 8, "WAVE", 4);
  memcpy(dst + 12, "fmt ", 4);
  W4(16, 16);
  W2(20, 1);
  W2(22, CHANNELS);
  W4(24, SAMPLE_RATE);
  W4(28, byteRate);
  W2(32, blockAlign);
  W2(34, BITS_PER_SAMPLE);
  memcpy(dst + 36, "data", 4);
  W4(40, dataBytes);
}

static bool i2sBeginForMic() {
  I2S.end();
  I2S.setPins(PIN_BCLK, PIN_WS, PIN_DOUT, PIN_DIN);
  return I2S.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
}

static void recFreeAbort() {
  g_recording = false;
  I2S.end();
  if (g_wavBuf) {
    free(g_wavBuf);
    g_wavBuf = nullptr;
  }
  g_wavCap = 0;
  g_pcmDataBytes = 0;
  g_recMaxMs = 0;
}

static uint8_t* allocRecordingBuffer(size_t cap) {
  uint8_t* p = nullptr;
  if (ESP.getPsramSize() > 0) {
    p = (uint8_t*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (!p) {
    p = (uint8_t*)heap_caps_malloc(cap, MALLOC_CAP_8BIT);
  }
  if (!p) {
    p = (uint8_t*)malloc(cap);
  }
  return p;
}

static void printMallocHint(size_t needBytes) {
  Serial.print("[REC] malloc 失敗，需要約 ");
  Serial.print((unsigned)needBytes);
  Serial.println(" bytes（WAV PCM 緩衝）");
  Serial.print("[REC] freeHeap=");
  Serial.print(ESP.getFreeHeap());
  Serial.print(" largestBlock=");
  Serial.print((unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  if (ESP.getPsramSize() > 0) {
    Serial.print(" psramFree=");
    Serial.print((unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  }
  Serial.println();
  Serial.println("[REC] 可縮小 MIC_MAX_RECORD_SECONDS，或啟用板載 PSRAM（Arduino 選項）");
}

static bool recTryStart() {
  if (!g_run_mic) {
    Serial.println("[REC] 麥克風未啟用（開機選 3）");
    return false;
  }
  if (!g_run_wifi) {
    Serial.println("[REC] 需要 WiFi 才能上傳 STT（開機選 1）");
    return false;
  }
  if (g_recording) {
    Serial.println("[REC] 已在錄音中");
    return false;
  }

  int wantSec = MIC_MAX_RECORD_SECONDS;
  if (wantSec < 1) wantSec = 1;
  if (wantSec > 60) wantSec = 60;

  g_wavBuf = nullptr;
  g_wavCap = 0;
  int gotSec = 0;
  for (int sec = wantSec; sec >= 1; sec--) {
    uint32_t maxSamples = SAMPLE_RATE * (uint32_t)sec;
    size_t dataMax = (size_t)maxSamples * sizeof(int16_t);
    size_t cap = 44 + dataMax;
    uint8_t* buf = allocRecordingBuffer(cap);
    if (buf) {
      g_wavBuf = buf;
      g_wavCap = cap;
      gotSec = sec;
      if (sec != wantSec) {
        Serial.print("[REC] 記憶體不足，已自動改為最長 ");
        Serial.print(sec);
        Serial.println(" 秒");
      }
      break;
    }
  }

  if (!g_wavBuf) {
    size_t need1 = 44 + (size_t)SAMPLE_RATE * sizeof(int16_t);
    printMallocHint(need1);
    return false;
  }

  g_recMaxMs = (uint32_t)gotSec * 1000UL;
  g_pcmDataBytes = 0;
  writeWavHeader(g_wavBuf, 0);
  if (!i2sBeginForMic()) {
    Serial.println("[REC] I2S 開始失敗");
    free(g_wavBuf);
    g_wavBuf = nullptr;
    g_wavCap = 0;
    return false;
  }
  delay(200);
  g_recording = true;
  g_recStartMs = millis();
  g_serialLineBuf = "";
  Serial.println("[REC] 錄音中… 再輸入 r + Enter 停止並上傳（或達最長時間自動停止）");
  return true;
}

static void recPump() {
  if (!g_recording || !g_wavBuf) return;
  size_t maxPcm = g_wavCap - 44;
  if (g_pcmDataBytes >= maxPcm) return;

  int32_t tmp[REC_CHUNK_FRAMES * 2];
  size_t spacePcm = maxPcm - g_pcmDataBytes;
  size_t maxSamples = spacePcm / sizeof(int16_t);
  size_t chunk = maxSamples < REC_CHUNK_FRAMES ? maxSamples : REC_CHUNK_FRAMES;
  if (chunk == 0) return;

  size_t got = I2S.readBytes((char*)tmp, chunk * sizeof(int32_t) * 2);
  if (!got) return;

  int16_t* dst = (int16_t*)(g_wavBuf + 44 + g_pcmDataBytes);
  size_t frames = got / (sizeof(int32_t) * 2);
  size_t outN = 0;
  for (size_t i = 0; i < frames; i++) {
    if (g_pcmDataBytes + (outN + 1) * sizeof(int16_t) > maxPcm) break;
    int32_t l = tmp[i * 2 + 0];
    int32_t r = tmp[i * 2 + 1];
    int32_t s32 = g_micUseRight ? r : l;
    int32_t v = s32 >> g_recShift;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    dst[outN++] = (int16_t)v;
  }
  g_pcmDataBytes += outN * sizeof(int16_t);
}

static bool pollSerialRecordingStop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      String t = g_serialLineBuf;
      g_serialLineBuf = "";
      t.trim();
      if (t.length() == 1 && (t[0] == 'r' || t[0] == 'R')) return true;
      if (t.length() > 0) {
        Serial.println("[REC] 停止請只輸入 r（已忽略此行）");
      }
      continue;
    }
    if (g_serialLineBuf.length() < 32) g_serialLineBuf += c;
  }
  return false;
}

static bool httpPostStt(const uint8_t* wav, size_t wavLen, String& outText) {
  outText = "";
  if (!g_run_wifi) return false;
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(30);
  HTTPClient http;
  // 必須 >= http.setTimeout：否則等待 STT 回應時會先被底層 TCP read 截斷（HTTP -11）
  client.setTimeout(STT_HTTP_READ_TIMEOUT_SEC);
  if (!http.begin(client, STT_URL)) return false;
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("Content-Type", "audio/wav");
  http.setTimeout((uint32_t)STT_HTTP_READ_TIMEOUT_SEC * 1000u);
  int code = http.POST((uint8_t*)wav, wavLen);
  String resp = http.getString();
  http.end();
  if (code != 200) {
    Serial.print("[STT HTTP ");
    Serial.print(code);
    Serial.println("]");
    if (code < 0) {
      Serial.print("[STT ERR] ");
      Serial.println(http.errorToString(code));
      Serial.print("[STT URL] ");
      Serial.println(STT_URL);
      Serial.print("[WiFi] RSSI=");
      Serial.println(WiFi.RSSI());
      Serial.print("[SYS] freeHeap=");
      Serial.println(ESP.getFreeHeap());
    }
    Serial.println(resp);
    return false;
  }
  outText = extractJsonStringField(resp, "text");
  return outText.length() > 0;
}

static void processUserTextLine(const String& line);

static void micFinishAndUpload() {
  g_recording = false;
  g_recMaxMs = 0;
  I2S.end();
  if (!g_wavBuf) {
    g_pcmDataBytes = 0;
    return;
  }
  const uint32_t minBytes = (uint32_t)(SAMPLE_RATE / 2) * (uint32_t)sizeof(int16_t);
  if (g_pcmDataBytes < minBytes) {
    Serial.println("[REC] 錄音太短，已取消");
    free(g_wavBuf);
    g_wavBuf = nullptr;
    g_wavCap = 0;
    g_pcmDataBytes = 0;
    Serial.println("請繼續輸入：");
    return;
  }
  writeWavHeader(g_wavBuf, g_pcmDataBytes);
  const size_t wavLen = 44 + (size_t)g_pcmDataBytes;
  String said;
  if (!httpPostStt(g_wavBuf, wavLen, said)) {
    Serial.println("[REC] STT 失敗（檢查 STT_URL、WiFi、後端金鑰）");
    free(g_wavBuf);
    g_wavBuf = nullptr;
    g_wavCap = 0;
    g_pcmDataBytes = 0;
    Serial.println("請繼續輸入：");
    return;
  }
  free(g_wavBuf);
  g_wavBuf = nullptr;
  g_wavCap = 0;
  g_pcmDataBytes = 0;
  said.trim();
  if (said.length() == 0) {
    Serial.println("[REC] 辨識為空");
    Serial.println("請繼續輸入：");
    return;
  }
  Serial.print("[YOU voice] ");
  Serial.println(said);
  processUserTextLine(said);
}

#if ENABLE_TTS
static String stripForTts(const String& s) {
  String t = s;
  t.replace('\r', ' ');
  t.replace('\n', ' ');
  t.trim();
  if (t.length() > 400) t = t.substring(0, 400);
  return t;
}

static bool httpPostTts(const String& text, String& outAbsoluteUrl) {
  outAbsoluteUrl = "";
  if (!g_run_wifi || WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(30);
  HTTPClient http;
  if (!http.begin(client, TTS_URL)) return false;
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(180000);
  String body = "{\"device_id\":\"";
  body += DEVICE_ID;
  body += "\",\"text\":\"";
  body += escapeJsonString(text);
  body += "\"}";
  int code = http.POST(body);
  String resp = http.getString();
  http.end();
  if (code != 200) {
    Serial.print("[TTS HTTP ");
    Serial.print(code);
    Serial.println("]");
    Serial.println(resp);
    return false;
  }
  outAbsoluteUrl = extractJsonStringField(resp, "absolute_url");
  return outAbsoluteUrl.length() > 0;
}

static uint8_t* downloadMp3ToRam(const char* url, size_t& outSize) {
  outSize = 0;
  if (!g_run_wifi || WiFi.status() != WL_CONNECTED) return nullptr;
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(30);
  HTTPClient http;
  if (!http.begin(client, url)) return nullptr;
  int code = http.GET();
  if (code != 200) {
    Serial.print("[TTS dl HTTP ");
    Serial.print(code);
    Serial.println("]");
    http.end();
    return nullptr;
  }
  int len = http.getSize();
  const size_t kMax = 512 * 1024;
  WiFiClient* stream = http.getStreamPtr();
  size_t cap = (len > 0) ? (size_t)len : (64 * 1024);
  if (cap > kMax) cap = kMax;
  uint8_t* buf = nullptr;
  if (ESP.getPsramSize() > 0) {
    buf = (uint8_t*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (!buf) buf = (uint8_t*)malloc(cap);
  if (!buf) {
    http.end();
    return nullptr;
  }
  size_t pos = 0;
  unsigned long start = millis();
  while (http.connected()) {
    size_t avail = stream->available();
    if (avail) {
      size_t toRead = avail;
      if (pos + toRead > cap) {
        size_t newCap = cap * 2;
        if (newCap < pos + toRead) newCap = pos + toRead;
        if (newCap > kMax) {
          free(buf);
          http.end();
          return nullptr;
        }
        uint8_t* nb = (uint8_t*)realloc(buf, newCap);
        if (!nb) {
          free(buf);
          http.end();
          return nullptr;
        }
        buf = nb;
        cap = newCap;
      }
      int r = stream->readBytes(buf + pos, toRead);
      if (r > 0) pos += (size_t)r;
    } else {
      if (millis() - start > 20000) break;
      delay(5);
    }
    if (len > 0 && (int)pos >= len) break;
  }
  http.end();
  outSize = pos;
  return buf;
}

static bool i2sBeginForPlayback() {
  I2S.end();
  I2S.setPins(PIN_BCLK, PIN_WS, PIN_DOUT, -1, -1);
  return I2S.begin(I2S_MODE_STD, 44100, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
}

static void speakAiReply(const String& replyText, const String& inlineTtsUrl) {
  String t = stripForTts(replyText);
  if (!t.length()) return;

  Serial.println("[TTS] 播放 AI 回覆…");
  I2S.end();
  delay(50);

  String mp3Url;
  if (inlineTtsUrl.length() > 0) {
    mp3Url = inlineTtsUrl;
    Serial.println("[TTS] 使用 /api/chat 內嵌網址（省一次 POST）");
  } else if (!httpPostTts(t, mp3Url)) {
    Serial.println("[TTS] 請求音訊 URL 失敗");
    i2sBeginForMic();
    return;
  }

  size_t mp3Len = 0;
  uint8_t* mp3 = downloadMp3ToRam(mp3Url.c_str(), mp3Len);
  if (!mp3 || mp3Len < 16) {
    Serial.println("[TTS] MP3 下載失敗");
    if (mp3) free(mp3);
    i2sBeginForMic();
    return;
  }

  if (!i2sBeginForPlayback()) {
    Serial.println("[TTS] I2S 播放模式啟動失敗");
    free(mp3);
    i2sBeginForMic();
    return;
  }

  bool ok = I2S.playMP3(mp3, mp3Len);
  Serial.print("[TTS] playMP3=");
  Serial.println(ok ? "ok" : "fail");
  free(mp3);
  i2sBeginForMic();
}
#endif
#endif

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("=== ESP32-S3 AI + UDP -> ESP32-C3 LED ===");
  Serial.println("[BOOT] WiFi auto-connect enabled (will retry)");

  udpTargetsLoad();
  syncTargetIpFromIndex();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.println();
  Serial.println("輸入一行文字送 AI；雲端依 device_cmd 送 UDP；若 UDP 無回覆則改用 BLE（需可用 BLE）");
  Serial.print("PC 亦可經 WiFi UDP 送同一行文字：埠 ");
  Serial.println((unsigned)S3_PC_CMD_UDP_PORT);
#if ENABLE_I2S_MIC
  Serial.println("輸入 r 開始錄音，再輸入 r 停止並上傳 STT（需要 WiFi）");
#if ENABLE_TTS
  Serial.println("TTS：AI 回覆後會播 MP3（需要 WiFi）");
#endif
  Serial.println("micL/micR、shift8..shift20");
#endif

#if ENABLE_BLE_CLIENT
  bleClientInit();
#endif
}

static void wifiPcReplyIf(uint16_t pcRport, const IPAddress& pcRip, const String& msg) {
  if (pcRport == 0 || WiFi.status() != WL_CONNECTED) return;
  g_udpPcCmd.beginPacket(pcRip, pcRport);
  g_udpPcCmd.print(msg);
  (void)g_udpPcCmd.endPacket();
}

static void processUserTextLine(const String& line) {
  IPAddress pcRip = g_wifiPcReplyIp;
  uint16_t pcRport = g_wifiPcReplyPort;
  g_wifiPcReplyPort = 0;

  if (WiFi.status() != WL_CONNECTED && g_run_ble) {
    String u = line;
    u.trim();
    String up = u;
    up.toUpperCase();
    if (up.length() == 0) {
      Serial.println("請繼續輸入（ON/OFF/B0..B100）：");
      return;
    }
#if ENABLE_BLE_CLIENT
    if (up == "ON") {
      bleMirrorCommand("ON");
      Serial.println("[DONE] BLE");
      Serial.println("請繼續輸入：");
      return;
    }
    if (up == "OFF") {
      bleMirrorCommand("OFF");
      Serial.println("[DONE] BLE");
      Serial.println("請繼續輸入：");
      return;
    }
    if (up == "B0" || up == "B25" || up == "B50" || up == "B75" || up == "B100") {
      bleMirrorCommand(up.c_str());
      Serial.println("[DONE] BLE");
      Serial.println("請繼續輸入：");
      return;
    }
#endif
    Serial.println("[BLE] 未知指令；請輸入 ON / OFF / B0 / B25 / B50 / B75 / B100");
    Serial.println("請繼續輸入：");
    wifiPcReplyIf(pcRport, pcRip, "[S3] ERR: use Serial/BLE for ON/OFF here (WiFi chat needs STA).\n");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] 尚未連線，可先用 BLE 送 ON/OFF/Bxx（若 BLE 可用）");
    Serial.println("請繼續輸入：");
    wifiPcReplyIf(pcRport, pcRip, "[S3] ERR: WiFi not connected\n");
    return;
  }
  String reply;
  String deviceCmd;
  String deviceTarget;
  String deviceLink;
  String ttsUrl;
  Serial.println("[CHAT] ...");
  // Ask the cloud model to also emit a C3 command when appropriate.
  // This doesn't require backend schema changes; we parse from reply text.
  String prompt = line;
  prompt += "\n\n請在最後一行（如果需要控制 C3 或 OTA）輸出：C3_CMD: <cmd>\n"
            "C3 為 LED：led_on / led_off / led 0..255（不要用單獨的 on 或 off）。\n"
            "查目前韌體/腳位/狀態：用 C3_CMD: status 或 buildid（S3 會把 C3 的 UDP 回覆再送交雲端整理成最終回答）。\n"
            "【重要】若 C3_CMD 不是 (none)：第一輪正文可簡短；最終給使用者的完整說明會由裝置在收到 C3 實際 UDP 回覆後再經第二輪雲端整理後輸出。\n"
            "純問答、不需查裝置：正文直接完整回答，並輸出 C3_CMD: (none)；"
            "不要對「查線路/問腳位」使用 ai，因為 ai 只會觸發遠端 PC 改碼+OTA，不會把結果文字送回這裡。\n"
            "僅當使用者要改 C3 韌體時才用：C3_CMD: ai <給 PC 的改碼需求>；"
            "OTA：ota http://<pc>:8000/<file>.bin\n"
            "不需送 C3：C3_CMD: (none)";
  if (!httpPostChat(prompt, reply, deviceCmd, deviceTarget, deviceLink, ttsUrl)) {
    Serial.println("請繼續輸入：");
    wifiPcReplyIf(pcRport, pcRip, "[S3] ERR: cloud chat request failed\n");
    return;
  }

  String c3cmd = extractC3CmdFromReply(reply);
  bool hasC3Cmd =
      c3cmd.length() > 0 && !c3cmd.equalsIgnoreCase("(none)") && !c3cmd.equalsIgnoreCase("none");
  String c3low = c3cmd;
  c3low.toLowerCase();
  // ai / ota：不等待 C3 UDP 文字回饋第二輪（改由 PC bridge 或長時間 OTA），維持第一輪即回覆。
  bool c3IsAiOrOta = c3low.startsWith("ai ") || c3low.startsWith("ota ");
  bool deferFinalAi = hasC3Cmd && g_targetIp && !c3IsAiOrOta;

  String outReply = reply;
  String outTtsUrl = ttsUrl;
  bool gotSecondPass = false;

  if (!deferFinalAi) {
    Serial.print("[AI] ");
    Serial.println(reply);
  } else {
    Serial.println("[CHAT] 偵測到需送 C3 的指令：先等 C3 UDP 回饋，再輸出最終整理回答（不先播報第一輪正文）。");
  }

  deviceCmd.toLowerCase();
  deviceCmd.trim();
  deviceTarget.trim();
  deviceLink.trim();
  Serial.print("[CLOUD] device_cmd=");
  Serial.print(deviceCmd.length() ? deviceCmd : "(empty)");
  Serial.print("  device_target=");
  Serial.print(deviceTarget.length() ? deviceTarget : "(unchanged)");
  Serial.print("  device_link=");
  Serial.println(deviceLink.length() ? deviceLink : "(unchanged)");

  httpPostLogBestEffort(line, reply + " |cmd:" + deviceCmd + "|tgt:" + deviceTarget + "|link:" + deviceLink,
                        "chat");

  udpApplyAiDeviceLink(deviceLink);

  bool targetOk = udpResolveAiDeviceTarget(deviceTarget);
  if (!targetOk) {
    Serial.println("[PATH] AI device_target 無效，不送出 UDP");
  }

  if (targetOk && deviceCmd == "on") {
    Serial.println("[PATH] AI device_cmd -> UDP ON");
    deviceSendCommandAuto("ON");
    httpPostLogBestEffort(line, "ON", "ai_device_cmd");
  } else if (targetOk && deviceCmd == "off") {
    Serial.println("[PATH] AI device_cmd -> UDP OFF");
    deviceSendCommandAuto("OFF");
    httpPostLogBestEffort(line, "OFF", "ai_device_cmd");
  } else if (targetOk && (deviceCmd == "b0" || deviceCmd == "b25" || deviceCmd == "b50" || deviceCmd == "b75" ||
                          deviceCmd == "b100")) {
    String u = deviceCmd;
    u.toUpperCase();
    Serial.print("[PATH] AI device_cmd -> UDP ");
    Serial.println(u);
    deviceSendCommandAuto(u.c_str());
    httpPostLogBestEffort(line, u, "ai_device_cmd");
  }

  // Send C3_CMD from first reply (already parsed into c3cmd / hasC3Cmd).
  if (hasC3Cmd) {
    if (!g_targetIp) {
      Serial.println("[C3] 無有效目標 IP。請先用 udp add/use 設定 C3 的 IP。");
    } else {
      if (c3low.startsWith("ai ")) {
        Serial.println(
            "[NOTE] ai 會轉發到 PC；Bridge 結束後會以 UDP 回傳 BRIDGE_RESULT（請開著 udp_bridge）。"
            "若只問接線請用 status 或看上文。");
      }
      drainUdpFromC3();
      bool sentOk = sendRawToC3(c3cmd);
      if (!sentOk && deferFinalAi) {
        Serial.println("[CHAT] 無法送出 UDP，改輸出第一輪回答。");
        Serial.print("[AI] ");
        Serial.println(reply);
        deferFinalAi = false;
      }
      if (sentOk) {
        // ai/ota: no second cloud summary here (PC bridge or long OTA).
        if (!c3low.startsWith("ai ") && !c3low.startsWith("ota ")) {
          String c3rx = waitUdpReplyFromC3(4000);
          if (c3rx.length() == 0) {
            delay(80);
            c3rx = waitUdpReplyFromC3(600);
          }
          if (c3rx.length() == 0) {
            Serial.println(
                "[C3 RX] (timeout / no UDP payload) — check C3 replies to S3 source port, "
                "router AP isolation, and that S3 uses fixed C3 UDP local port.");
            if (deferFinalAi) {
              Serial.println("[CHAT] 未取得 C3 回饋，改輸出第一輪回答。");
              Serial.print("[AI] ");
              Serial.println(reply);
              deferFinalAi = false;
            }
          }
          if (c3rx.length() > 0) {
            Serial.print("[C3 RX] ");
            Serial.println(c3rx);
            String c3rxCopy = c3rx;
            if (c3rxCopy.length() > 3500) {
              c3rxCopy = c3rxCopy.substring(0, 3500);
              c3rxCopy += "...";
            }
            String replyDraft = reply;
            if (replyDraft.length() > 1500) {
              replyDraft = replyDraft.substring(0, 1500);
              replyDraft += "...";
            }
            String fp = "使用者問：\n";
            fp += line;
            fp += "\n\n雲端第一輪回覆草稿（請與下方 C3 實測對照後一併整理，勿與裝置實際狀態矛盾）：\n";
            fp += replyDraft;
            fp += "\n\nC3 裝置經 UDP 回覆如下（請據此與上文整理成給使用者的最終說明）：\n";
            fp += c3rxCopy;
            fp += "\n\n請用繁體中文整理最終回答（可說明已依 C3 實際回覆確認）。最後一行請輸出 C3_CMD: (none)。";
            String reply2;
            String dc2;
            String dt2;
            String dl2;
            String tu2;
            if (httpPostChat(fp, reply2, dc2, dt2, dl2, tu2)) {
              gotSecondPass = true;
              Serial.print("[AI] ");
              Serial.println(reply2);
              outReply = reply2;
              outTtsUrl = tu2;
              httpPostLogBestEffort(line, reply2 + " |c3_udp_followup", "chat");
            } else {
              Serial.println("[AI] ERR: second pass (C3 follow-up) httpPostChat failed");
              if (deferFinalAi) {
                outReply = reply;
                outReply += "\n\n[NOTE] 第二輪整理失敗；上為第一輪草稿。";
                outTtsUrl = ttsUrl;
                Serial.print("[AI] ");
                Serial.println(reply);
                deferFinalAi = false;
              }
            }
          }
        }
      }
    }
  }

  if (deferFinalAi && !gotSecondPass) {
    // 仍有延遲旗標但第二輪未成功（理論上上面已回退；此為保險）。
    Serial.print("[AI] ");
    Serial.println(reply);
    outReply = reply;
    outTtsUrl = ttsUrl;
  }

#if ENABLE_I2S_MIC && ENABLE_TTS
  if (g_run_wifi) {
    speakAiReply(outReply, outTtsUrl);
  }
#endif

  if (pcRport != 0) {
    String summary = outReply;
    if (summary.length() > 1200) {
      summary = summary.substring(0, 1200);
      summary += "\n...";
    }
    wifiPcReplyIf(pcRport, pcRip, summary);
  }

  Serial.println("[DONE]\n請繼續輸入：");
}

static void pollWifiPcCommand() {
  if (!g_run_wifi || WiFi.status() != WL_CONNECTED || !g_udpPcCmdStarted) return;
  int n = g_udpPcCmd.parsePacket();
  if (n <= 0) return;
  IPAddress rip = g_udpPcCmd.remoteIP();
  uint16_t rport = (uint16_t)g_udpPcCmd.remotePort();
  int r = g_udpPcCmd.read(g_pcUdpBuf, (int)sizeof(g_pcUdpBuf) - 1);
  if (r <= 0) return;
  g_pcUdpBuf[r] = '\0';
  // If the packet was larger than our buffer, discard the remaining bytes
  // to avoid confusing the next parsePacket().
  while (g_udpPcCmd.available() > 0) {
    (void)g_udpPcCmd.read();
  }
  String raw(g_pcUdpBuf);
  // PC udp_bridge.py sends BRIDGE_RESULT + summary (do not feed into cloud chat).
  if (raw.startsWith("BRIDGE_RESULT")) {
    Serial.println("\n[PC_BRIDGE] ----------");
    String rest = raw.substring(13);
    if (rest.startsWith("\n")) rest = rest.substring(1);
    if (rest.startsWith("\r\n")) rest = rest.substring(2);
    Serial.println(rest);
    Serial.println("[PC_BRIDGE] ----------\n");
    return;
  }
  String line = raw;
  line.trim();
  if (!line.length()) return;
  if (line.length() > 2000) {
    wifiPcReplyIf(rport, rip, "[S3] ERR: line too long (max 2000)\n");
    return;
  }
  g_wifiPcReplyIp = rip;
  g_wifiPcReplyPort = rport;
  Serial.print("[WIFI] ");
  Serial.println(line);
  processUserTextLine(line);
}

void loop() {
#if ENABLE_BLE_CLIENT
  if (g_run_ble) {
    bleClientTick();
  }
#endif
#if defined(ARDUINO_ARCH_ESP32)
  wifiTick();
#endif

  // Always show any UDP replies (e.g., buildid) when idle.
  pollIncomingUdpMessages();
  pollWifiPcCommand();
#if ENABLE_I2S_MIC
  if (g_recording) {
    recPump();
    if (g_wavBuf && g_pcmDataBytes >= g_wavCap - 44) {
      Serial.println("[REC] 緩衝已滿，停止並上傳");
      micFinishAndUpload();
      return;
    }
    if (g_recMaxMs > 0 && millis() - g_recStartMs >= g_recMaxMs) {
      Serial.println("[REC] 已達最長錄音時間，自動停止並上傳");
      micFinishAndUpload();
      return;
    }
    if (pollSerialRecordingStop()) {
      Serial.println("[REC] 停止，上傳 STT…");
      micFinishAndUpload();
      return;
    }
    delay(1);
    return;
  }
#endif

  if (!Serial.available()) {
    delay(10);
    return;
  }

  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;
  if (line.length() > 2000) {
    Serial.println("[ERR] 超過 2000 字元");
    return;
  }

  if (tryHandleUdpTargetLine(line)) return;

#if ENABLE_I2S_MIC
  if (g_run_mic && WiFi.status() == WL_CONNECTED && line.startsWith("shift")) {
    int v = line.substring(5).toInt();
    if (v >= 8 && v <= 20) {
      g_recShift = v;
      Serial.print("[CFG] g_recShift=");
      Serial.println(g_recShift);
    } else {
      Serial.println("[CFG] 用法: shift8 .. shift20（與 esp32_voice_ai_robot）");
    }
    return;
  }
  if (g_run_mic && WiFi.status() == WL_CONNECTED && (line == "micL" || line == "micl")) {
    g_micUseRight = false;
    Serial.println("[CFG] mic channel = LEFT");
    return;
  }
  if (g_run_mic && WiFi.status() == WL_CONNECTED && (line == "micR" || line == "micr")) {
    g_micUseRight = true;
    Serial.println("[CFG] mic channel = RIGHT");
    return;
  }
#endif

#if ENABLE_I2S_MIC
  if (g_run_mic && WiFi.status() == WL_CONNECTED) {
    String key = line;
    key.toLowerCase();
    if (key == "r") {
      if (!recTryStart()) {
        Serial.println("請繼續輸入：");
      }
      return;
    }
  }
#endif

  Serial.print("[YOU] ");
  Serial.println(line);
  processUserTextLine(line);
}

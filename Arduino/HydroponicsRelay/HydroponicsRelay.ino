// Подключение Wi‑Fi и Web‑сервер
#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include <sys/time.h>
#include <Preferences.h>

// Конфигурация пинов реле:
// IN1 -> GPIO41 (Lighting), IN2 -> GPIO42 (Pump)
const int relayLightingPin = 41;  // Канал 1 — освещение (IN1)
const int relayPumpPin     = 42;  // Канал 2 — помпа (IN2)

// Логика срабатывания реле: true для HIGH‑level (RY‑VCC↔VCC), false для LOW‑level (VOC↔GND)
const bool relayActiveHigh = false;  // инвертировано

// Скорость Serial для управления из монитора порта
const unsigned long serialBaudRate = 115200;

// Настройки Wi‑Fi (замените на свои)
const char* WIFI_SSID     = "Smart Home";
const char* WIFI_PASSWORD = "qazwsxedc";

// NTP/время
bool   ntpEnabled   = true;
long   gmtOffsetSec = 0;           // смещение от UTC в секундах
const char* NTP1 = "pool.ntp.org";
const char* NTP2 = "time.nist.gov";
unsigned long lastTimeSyncCheckMs = 0;

// NVS
Preferences prefs;

// HTTP сервер
WebServer server(80);
// Кольцевой буфер для последних N событий
struct Event { unsigned long ts; String msg; };
const size_t MAX_EVENTS = 50;
Event events[MAX_EVENTS];
size_t eventsHead = 0, eventsCount = 0;

// Параметры: помпа — циклический скедулер (в минутах)
struct CycleCfg { bool enabled; uint16_t onMin; uint16_t offMin; unsigned long lastTickMs; bool currentlyOn; };
CycleCfg schedPump { false, 1, 14, 0, false };

// Параметры: свет — расписание по времени суток (минуты с начала дня)
struct ClockCfg { bool enabled; uint16_t onMins; uint16_t offMins; bool lastAppliedOn; };
ClockCfg lightClock { false, 6*60, 22*60, false };

void saveConfig() {
  if (!prefs.begin("hydro", false)) return;
  prefs.putBool ("ntp", ntpEnabled);
  prefs.putLong ("tz",  gmtOffsetSec);
  prefs.putBool ("p_en", schedPump.enabled);
  prefs.putUShort("p_on", schedPump.onMin);
  prefs.putUShort("p_off",schedPump.offMin);
  prefs.putBool ("l_en", lightClock.enabled);
  prefs.putUShort("l_on", lightClock.onMins);
  prefs.putUShort("l_off",lightClock.offMins);
  prefs.end();
}

void loadConfig() {
  if (!prefs.begin("hydro", true)) return;
  ntpEnabled          = prefs.getBool ("ntp", ntpEnabled);
  gmtOffsetSec        = prefs.getLong ("tz",  gmtOffsetSec);
  schedPump.enabled   = prefs.getBool ("p_en", schedPump.enabled);
  schedPump.onMin     = prefs.getUShort("p_on", schedPump.onMin);
  schedPump.offMin    = prefs.getUShort("p_off",schedPump.offMin);
  lightClock.enabled  = prefs.getBool ("l_en", lightClock.enabled);
  lightClock.onMins   = prefs.getUShort("l_on", lightClock.onMins);
  lightClock.offMins  = prefs.getUShort("l_off", lightClock.offMins);
  prefs.end();
}

void pushEvent(const String &m) {
  events[eventsHead] = { millis(), m };
  eventsHead = (eventsHead + 1) % MAX_EVENTS;
  if (eventsCount < MAX_EVENTS) eventsCount++;
}

void setRelayState(int pinNumber, bool shouldTurnOn) {
  const int activeState   = relayActiveHigh ? HIGH : LOW;
  const int inactiveState = relayActiveHigh ? LOW  : HIGH;
  digitalWrite(pinNumber, shouldTurnOn ? activeState : inactiveState);
  String name = (pinNumber == relayLightingPin) ? F("Lighting") : F("Water pump");
  pushEvent(name + (shouldTurnOn ? F(" -> ON") : F(" -> OFF")));
}

void applyPumpCycle(CycleCfg &cfg, int pin) {
  if (!cfg.enabled) return;
  unsigned long now = millis();
  if (cfg.lastTickMs == 0) cfg.lastTickMs = now;
  unsigned long periodMs = (unsigned long)(cfg.currentlyOn ? cfg.onMin : cfg.offMin) * 60UL * 1000UL;
  if (periodMs == 0) return;
  if (now - cfg.lastTickMs >= periodMs) {
    cfg.currentlyOn = !cfg.currentlyOn;
    cfg.lastTickMs = now;
    setRelayState(pin, cfg.currentlyOn);
  }
}

// Вспомогательные функции для времени
bool getLocalTM(struct tm &out) {
  time_t now; time(&now);
  if (now < 100000) return false; // время не синхронизировано
  struct tm *lt = localtime(&now);
  if (!lt) return false;
  out = *lt; return true;
}

uint16_t hhmmToMins(const String &s) {
  int p = s.indexOf(':');
  if (p <= 0) return 0;
  int hh = s.substring(0,p).toInt();
  int mm = s.substring(p+1).toInt();
  if (hh < 0) hh=0; if (hh>23) hh=23; if (mm<0) mm=0; if (mm>59) mm=59;
  return (uint16_t)(hh*60+mm);
}

String minsToHHMM(uint16_t m) {
  uint16_t hh = (m/60)%24; uint16_t mm = m%60;
  char buf[6]; snprintf(buf,sizeof(buf), "%02u:%02u", (unsigned)hh,(unsigned)mm);
  return String(buf);
}

void applyLightByClock() {
  if (!lightClock.enabled) return;
  struct tm lt; if (!getLocalTM(lt)) return;
  uint16_t nowM = (uint16_t)(lt.tm_hour*60 + lt.tm_min);
  bool shouldOn;
  if (lightClock.onMins == lightClock.offMins) {
    shouldOn = false;
  } else if (lightClock.onMins < lightClock.offMins) {
    shouldOn = (nowM >= lightClock.onMins && nowM < lightClock.offMins);
  } else {
    // период через полночь
    shouldOn = (nowM >= lightClock.onMins || nowM < lightClock.offMins);
  }
  if (shouldOn != lightClock.lastAppliedOn) {
    lightClock.lastAppliedOn = shouldOn;
    setRelayState(relayLightingPin, shouldOn);
  }
}

void ensureTimeSync() {
  if (!ntpEnabled) return;
  if (WiFi.status() != WL_CONNECTED) return;
  unsigned long now = millis();
  if (lastTimeSyncCheckMs!=0 && now - lastTimeSyncCheckMs < 60000UL) return;
  lastTimeSyncCheckMs = now;
  static bool configured = false;
  if (!configured) {
    configTime(gmtOffsetSec, 0, NTP1, NTP2);
    configured = true;
  }
}

void printStatus() {
  int lightLevel = digitalRead(relayLightingPin);
  int pumpLevel  = digitalRead(relayPumpPin);
  const int activeState   = relayActiveHigh ? HIGH : LOW;
  Serial.println();
  Serial.println(F("== Relay status =="));
  Serial.print(F("Lighting: ")); Serial.println(lightLevel == activeState ? F("ON") : F("OFF"));
  Serial.print(F("Pump    : ")); Serial.println(pumpLevel  == activeState ? F("ON") : F("OFF"));
  Serial.println();
}

String jsonStatus() {
  const int activeState = relayActiveHigh ? HIGH : LOW;
  bool lightOn = (digitalRead(relayLightingPin) == activeState);
  bool pumpOn  = (digitalRead(relayPumpPin)     == activeState);
  struct tm lt; bool tv = getLocalTM(lt);
  char tbuf[20]=""; if (tv) snprintf(tbuf,sizeof(tbuf), "%04d-%02d-%02d %02d:%02d:%02d",
    lt.tm_year+1900, lt.tm_mon+1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
  String json = "{";
  json += "\"lighting\":"; json += lightOn?"true":"false";
  json += ",\"pump\":";     json += pumpOn?"true":"false";
  json += ",\"time\":\"";   json += String(tbuf); json += "\"";
  json += ",\"ntp\":";      json += ntpEnabled?"true":"false";
  json += ",\"tz\":";       json += gmtOffsetSec;
  json += ",\"sched\":{\"lightClock\":{\"en\":"; json += lightClock.enabled?"true":"false";
  json += ",\"on\":\""; json += minsToHHMM(lightClock.onMins); json += "\"";
  json += ",\"off\":\""; json += minsToHHMM(lightClock.offMins); json += "\"";
  json += "},\"pump\":{\"en\":"; json += schedPump.enabled?"true":"false"; json += ",\"on\":"; json += schedPump.onMin; json += ",\"off\":"; json += schedPump.offMin; json += "}}}";
  return json;
}

void handleStatus() {
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
  server.send(200, "application/json", jsonStatus());
}

void handleRelayApi() {
  // /api/relay?ch=1&on=1
  if (!server.hasArg("ch") || !server.hasArg("on")) {
    server.send(400, "application/json", "{\"error\":\"params ch,on required\"}");
    return;
  }
  int ch = server.arg("ch").toInt();
  bool on = (server.arg("on") == "1" || server.arg("on") == "true");
  if (ch == 1)      { setRelayState(relayLightingPin, on); lightClock.lastAppliedOn = on; }
  else if (ch == 2) { setRelayState(relayPumpPin, on);     schedPump.currentlyOn  = on; schedPump.lastTickMs  = millis(); }
  else { server.send(400, "application/json", "{\"error\":\"ch must be 1 or 2\"}"); return; }
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
  server.send(200, "application/json", jsonStatus());
}

void handleEvents() {
  String json = "[";
  for (size_t i = 0; i < eventsCount; i++) {
    size_t idx = (eventsHead + MAX_EVENTS - eventsCount + i) % MAX_EVENTS;
    if (i) json += ',';
    json += '{';
    json += "\"ts\":"; json += events[idx].ts; json += ',';
    json += "\"msg\":\""; json += events[idx].msg; json += "\"";
    json += '}';
  }
  json += "]";
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
  server.send(200, "application/json", json);
}

void handleSchedulePump() {
  // /api/schedule_pump?en=1&on=1&off=14  (минуты)
  if (server.hasArg("en"))  schedPump.enabled = (server.arg("en")=="1" || server.arg("en")=="true");
  if (server.hasArg("on"))  schedPump.onMin   = (uint16_t) server.arg("on").toInt();
  if (server.hasArg("off")) schedPump.offMin  = (uint16_t) server.arg("off").toInt();
  schedPump.lastTickMs = millis();
  saveConfig();
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
  server.send(200, "application/json", jsonStatus());
}

void handleScheduleLight() {
  // /api/schedule_light?en=1&on=HH:MM&off=HH:MM
  if (server.hasArg("en"))  lightClock.enabled = (server.arg("en")=="1" || server.arg("en")=="true");
  if (server.hasArg("on"))  lightClock.onMins  = hhmmToMins(server.arg("on"));
  if (server.hasArg("off")) lightClock.offMins = hhmmToMins(server.arg("off"));
  saveConfig();
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
  server.send(200, "application/json", jsonStatus());
}

void handleTimeConfig() {
  // /api/time?ntp=1&tz=10800   |  /api/time?epoch=...
  if (server.hasArg("ntp")) ntpEnabled = (server.arg("ntp")=="1" || server.arg("ntp")=="true");
  if (server.hasArg("tz"))  gmtOffsetSec = server.arg("tz").toInt();
  if (server.hasArg("epoch")) {
    time_t e = (time_t) server.arg("epoch").toInt();
    struct timeval tv{ .tv_sec = e, .tv_usec = 0 }; settimeofday(&tv, nullptr);
  }
  if (ntpEnabled && WiFi.status()==WL_CONNECTED) { configTime(gmtOffsetSec, 0, NTP1, NTP2); }
  saveConfig();
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
  server.send(200, "application/json", jsonStatus());
}

const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Hydroponic Tower</title>
<style>
  :root{--bg:#121212;--fg:#e6e6e6;--muted:#9aa0a6;--card:#1e1e1e;--acc:#4dd0e1;--on:#00e676;--off:#ff5252}
  body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.4 system-ui,Segoe UI,Roboto,Ubuntu}
  .wrap{max-width:980px;margin:24px auto;padding:0 16px}
  h1{font-size:28px;margin:10px 0 18px}
  table{width:100%;border-collapse:collapse;background:var(--card);border-radius:10px;overflow:hidden}
  th,td{padding:12px 14px;border-bottom:1px solid #2a2a2a}
  th{color:var(--muted);text-align:left;font-weight:600}
  .row:last-child td{border-bottom:none}
  .actions{white-space:nowrap}
  .grid{display:grid;grid-template-columns:1fr;gap:16px;margin-top:18px}
  .card{background:var(--card);padding:12px 14px;border-radius:10px}
  .log{max-height:240px;overflow:auto;font-family:ui-monospace,Consolas,monospace;font-size:13px}
  .muted{color:var(--muted)}
  .sw{position:relative;display:inline-block;width:54px;height:28px;vertical-align:middle;margin-left:10px}
  .sw input{display:none}
  .slider{position:absolute;inset:0;background:#3a3a3a;border-radius:28px;transition:.18s}
  .slider:before{content:"";position:absolute;left:3px;top:3px;width:22px;height:22px;background:#fff;border-radius:50%;transition:.18s}
  input:checked+.slider{background:var(--on)}
  input:checked+.slider:before{transform:translateX(26px)}
  input[type=number],input[type=time]{width:110px;padding:6px 8px;border-radius:6px;border:1px solid #2a2a2a;background:#252525;color:var(--fg);} 
  label.small{font-size:12px;color:var(--muted);margin-right:8px}
  @media(max-width:720px){.grid{grid-template-columns:1fr}}
</style></head>
<body><div class="wrap">
  <h1>Hydroponic Tower</h1>
  <table>
    <tr><th style="width:40%">Name</th><th>State / Schedule</th></tr>
    <tr class="row"><td>Lighting</td>
      <td class="actions">
        <label class="sw"><input id="tL" type="checkbox"><span class="slider"></span></label>
        <label class="small">Enable</label><input id="enLC" type="checkbox">
        <label class="small">ON</label><input id="onLC" type="time" value="06:00">
        <label class="small">OFF</label><input id="offLC" type="time" value="22:00">
      </td></tr>
    <tr class="row"><td>Water pump</td>
      <td class="actions">
        <label class="sw"><input id="tP" type="checkbox"><span class="slider"></span></label>
        <label class="small">Enable</label><input id="enP" type="checkbox">
        <label class="small">ON (min)</label><input id="onP" type="number" min="0" value="1">
        <label class="small">OFF (min)</label><input id="offP" type="number" min="0" value="14">
      </td></tr>
  </table>
  <div class="grid">
    <div class="card">
      <div class="muted">Time</div>
      <div id="timeBox" class="muted">--</div>
    </div>
    <div class="card">
      <div class="muted">Events</div>
      <pre id="ev" class="log"></pre>
    </div>
  </div>
  <p class="muted" style="margin-top:14px">API: <a href="/api/status">/api/status</a>, <a href="/api/events">/api/events</a>, <code>/api/relay?ch=1&on=1</code>, <code>/api/schedule_pump?en=1&on=1&off=14</code>, <code>/api/schedule_light?en=1&on=06:00&off=22:00</code>, <code>/api/time?ntp=1&tz=10800</code>, <code>/api/time?epoch=...</code></p>
</div>
<script src="/app.js"></script>
</body></html>
)HTML";

void handleIndex() { server.send_P(200, "text/html", INDEX_HTML); }

void setupWebServer() {
  server.on("/", HTTP_GET, handleIndex);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/relay", HTTP_GET, handleRelayApi);
  server.on("/api/events", HTTP_GET, handleEvents);
  server.on("/api/schedule_pump", HTTP_GET, handleSchedulePump);
  server.on("/api/schedule_light", HTTP_GET, handleScheduleLight);
  server.on("/api/time", HTTP_GET, handleTimeConfig);
  // JS
  server.on("/app.js", HTTP_GET, [](){
    static const char APP_JS[] PROGMEM = R"JS(
const tL=document.getElementById('tL');
const tP=document.getElementById('tP');
const enP=document.getElementById('enP');
const onP=document.getElementById('onP');
const offP=document.getElementById('offP');
const enLC=document.getElementById('enLC');
const onLC=document.getElementById('onLC');
const offLC=document.getElementById('offLC');
const ev=document.getElementById('ev');
const timeBox=document.getElementById('timeBox');

// поля, которые нельзя перетирать во время ввода
const editing=new Set();
['enP','onP','offP','enLC','onLC','offLC'].forEach(id=>{
  const el=document.getElementById(id);
  el.addEventListener('focus',()=>editing.add(id));
  el.addEventListener('blur',()=>{ editing.delete(id); load(); });
});

async function api(url){ await fetch(url,{cache:'no-store'}); }
async function apiSet(ch,on){ await api(`/api/relay?ch=${ch}&on=${on?1:0}&t=${Date.now()}`); }
async function apiSchedPump(){ await api(`/api/schedule_pump?en=${enP.checked?1:0}&on=${onP.value}&off=${offP.value}&t=${Date.now()}`); }
async function apiSchedLight(){ await api(`/api/schedule_light?en=${enLC.checked?1:0}&on=${onLC.value}&off=${offLC.value}&t=${Date.now()}`); }
async function apiTimeTZEpoch(tzSec,epoch){ await api(`/api/time?tz=${tzSec}&epoch=${epoch}&t=${Date.now()}`); }

let didAutoSync=false;
async function autoSync(){
  if(didAutoSync) return; didAutoSync=true;
  const tzSec = -new Date().getTimezoneOffset()*60; // сек. восточнее UTC
  const epoch = Math.floor(Date.now()/1000);
  await apiTimeTZEpoch(tzSec,epoch);
}

async function load(){
  const r=await fetch(`/api/status?t=${Date.now()}`,{cache:'no-store'}); const j=await r.json();
  // тумблеры
  tL.checked=!!j.lighting; tP.checked=!!j.pump;
  // помпа — только если поле не редактируется
  if(!editing.has('enP'))   enP.checked=!!j.sched.pump.en;
  if(!editing.has('onP'))   onP.value=j.sched.pump.on;
  if(!editing.has('offP'))  offP.value=j.sched.pump.off;
  // свет по часам
  if(!editing.has('enLC'))  enLC.checked=!!j.sched.lightClock.en;
  if(!editing.has('onLC'))  onLC.value=j.sched.lightClock.on;
  if(!editing.has('offLC')) offLC.value=j.sched.lightClock.off;
  // время
  timeBox.textContent=j.time||'--';
  // события
  const e=await fetch(`/api/events?t=${Date.now()}`,{cache:'no-store'}); const ej=await e.json();
  ev.textContent=ej.map(x=>`${(x.ts/1000).toFixed(1)}  ${x.msg}`).reverse().join('\n');
}

tL.addEventListener('change',()=>apiSet(1,tL.checked));
tP.addEventListener('change',()=>apiSet(2,tP.checked));
enP.addEventListener('change',apiSchedPump);
onP.addEventListener('change',apiSchedPump);
offP.addEventListener('change',apiSchedPump);

enLC.addEventListener('change',apiSchedLight);
onLC.addEventListener('change',apiSchedLight);
offLC.addEventListener('change',apiSchedLight);

setInterval(load,1500);
autoSync().then(load);
)JS";
    server.sendHeader("Cache-Control","no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma","no-cache");
    server.sendHeader("Expires","0");
    server.send_P(200, "application/javascript", APP_JS);
  });
  server.begin();
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) { delay(250); Serial.print('.'); }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("WiFi connected, IP: ")); Serial.println(WiFi.localIP());
  } else {
    Serial.println(F("WiFi not connected (timeout)"));
  }
}

String readLineFromSerial() {
  static String buffer; while (Serial.available() > 0) { char c = (char)Serial.read(); if (c=='\n'||c=='\r'){ if(buffer.length()>0){ String line=buffer; buffer=""; return line; } } else { buffer+=c; } } return String(); }

void handleCommand(const String &raw) {
  String cmd = raw; cmd.trim(); cmd.toLowerCase();
  if (cmd == "help" || cmd == "h") {
    Serial.println(F("Commands:"));
    Serial.println(F("  light on/off | l on/off"));
    Serial.println(F("  pump on/off  | p on/off"));
    Serial.println(F("  schedule_pump en=1 on=1 off=14"));
    Serial.println(F("  schedule_light en=1 on=HH:MM off=HH:MM"));
    Serial.println(F("  time ntp=1 tz=10800 | time epoch=..."));
    Serial.println(F("  status"));
    return;
  }
  if (cmd == "status") { printStatus(); return; }
}

void setup() {
  Serial.begin(serialBaudRate); delay(200);
  loadConfig();
  pinMode(relayLightingPin, OUTPUT); pinMode(relayPumpPin, OUTPUT);
  setRelayState(relayLightingPin, false); setRelayState(relayPumpPin, false);
  Serial.println(F("Hydroponics Relay Controller (ESP32-S3)"));
  connectWiFi(); ensureTimeSync();
  server.stop(); setupWebServer();
}

void loop() {
  ensureTimeSync();
  applyLightByClock();
  applyPumpCycle(schedPump,  relayPumpPin);
  server.handleClient();
}



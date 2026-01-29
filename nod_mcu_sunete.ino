/*
  ESP8266 Relay Scheduler + Modern Web UI + Weekly Schedules + WiFi Change from Web + Backup/Import + HA REST
  - NTP time + DST auto (EET/EEST for Moldova/Romania)
  - Weekly schedules: separate list per day (Mon..Sun). Saturday can be special.
  - Relay pulse for duration seconds
  - Web UI: choose day, add/edit/delete/enable, manual trigger, import/export
  - Storage: LittleFS (/schedule_week.json)
  - WiFiManager:
      - autoConnect at boot
      - start config portal from Web UI button (Change WiFi)
  - HA integration: /ha/state, /ha/switch, /ha/trigger

  Board: NodeMCU 1.0 (ESP-12E)
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <time.h>
#include <WiFiManager.h>

// ===================== CONFIG =====================
const char* NTP_POOL = "pool.ntp.org";

// Relay
#define RELAY_PIN D1
const bool RELAY_ACTIVE_LOW = true;

// Files
const char* WEEK_FILE = "/schedule_week.json";

// Limits
const int MAX_EVENTS_PER_DAY = 64;

// ===================== GLOBALS =====================
ESP8266WebServer server(80);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, NTP_POOL, 0, 60 * 1000);

WiFiManager wm;

struct Event {
  int hh;
  int mm;
  int duration;   // seconds
  bool enabled;
  bool firedToday;
};

Event weekEvents[7][MAX_EVENTS_PER_DAY];
int weekCount[7] = {0,0,0,0,0,0,0};

bool relayOn = false;
unsigned long relayOffAtMs = 0;

int lastYDay = -1;

// ===================== UTIL =====================
static String two(int v){ return (v < 10) ? ("0"+String(v)) : String(v); }

static void relayWrite(bool on){
  relayOn = on;
  if (RELAY_ACTIVE_LOW) digitalWrite(RELAY_PIN, on ? LOW : HIGH);
  else digitalWrite(RELAY_PIN, on ? HIGH : LOW);
}

static void relayPulse(int seconds){
  if (seconds < 1) seconds = 1;
  relayWrite(true);
  relayOffAtMs = millis() + (unsigned long)seconds * 1000UL;
}

static bool ensureFS(){
  return LittleFS.begin();
}

static void setupTZ(){
  // EET/EEST with DST (Moldova/Romania)
  setenv("TZ", "EET-2EEST,M3.5.0/3,M10.5.0/4", 1);
  tzset();
}

static bool syncTimeNTP(){
  timeClient.begin();
  timeClient.forceUpdate();
  time_t epoch = (time_t)timeClient.getEpochTime();
  if (epoch < 1700000000) return false;
  timeval tv = { epoch, 0 };
  settimeofday(&tv, nullptr);
  return true;
}

static String getLocalTimeStr(){
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  return two(lt.tm_hour)+":"+two(lt.tm_min)+":"+two(lt.tm_sec);
}

static void getLocal(int &hh,int &mm,int &ss,int &yday,int &wday){
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  hh = lt.tm_hour; mm = lt.tm_min; ss = lt.tm_sec; yday = lt.tm_yday;
  // lt.tm_wday: 0=Sun..6=Sat. Convert to 0=Mon..6=Sun:
  int wd = lt.tm_wday;            // 0..6 (Sun..Sat)
  wday = (wd == 0) ? 6 : (wd - 1); // 0..6 (Mon..Sun)
}

static int parseHH(const char* t){
  return (t[0]-'0')*10 + (t[1]-'0');
}
static int parseMM(const char* t){
  return (t[3]-'0')*10 + (t[4]-'0');
}

static const char* dayNameRO(int d){
  // 0=Mon..6=Sun
  static const char* names[] = {"Luni","Marti","Miercuri","Joi","Vineri","Sambata","Duminica"};
  if (d<0 || d>6) return "??";
  return names[d];
}

// ===================== DEFAULT SCHEDULE (from your list) =====================
static void seedDefaultWeek(){
  // default = your list for Mon-Fri and Sunday; Saturday special can be different later via UI
  // For now: same list on all days, user can edit Saturday from UI.
  const char* times[] = {
    "00:10","00:15","02:00","02:30","04:00","04:15","06:30",
    "07:55","08:00","10:00","10:08","10:10","12:00","12:20","12:35","12:40",
    "15:00","15:08","15:10","17:00","17:10","17:15","20:00","20:15","20:20","22:30"
  };
  int n = sizeof(times)/sizeof(times[0]);

  for(int d=0; d<7; d++){
    weekCount[d] = 0;
    for(int i=0;i<n && weekCount[d] < MAX_EVENTS_PER_DAY;i++){
      Event &e = weekEvents[d][weekCount[d]++];
      e.hh = parseHH(times[i]);
      e.mm = parseMM(times[i]);
      e.duration = 5;
      e.enabled = true;
      e.firedToday = false;
    }
  }
}

// ===================== STORAGE (WEEK JSON) =====================
// Format:
// { "days": [ [ {time,duration,enabled}, ... ], ... 7 arrays ... ] }
static bool saveWeekToFS(){
  StaticJsonDocument<12000> doc;
  JsonArray days = doc.createNestedArray("days");

  for(int d=0; d<7; d++){
    JsonArray arr = days.createNestedArray();
    for(int i=0; i<weekCount[d]; i++){
      JsonObject o = arr.createNestedObject();
      char t[6]; snprintf(t,sizeof(t),"%02d:%02d",weekEvents[d][i].hh,weekEvents[d][i].mm);
      o["time"] = t;
      o["duration"] = weekEvents[d][i].duration;
      o["enabled"] = weekEvents[d][i].enabled;
    }
  }

  File f = LittleFS.open(WEEK_FILE, "w");
  if(!f) return false;
  serializeJson(doc, f);
  f.close();
  return true;
}

static bool loadWeekFromFS(){
  if(!LittleFS.exists(WEEK_FILE)){
    seedDefaultWeek();
    return saveWeekToFS();
  }

  File f = LittleFS.open(WEEK_FILE, "r");
  if(!f) return false;

  StaticJsonDocument<12000> doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if(err) return false;

  if(!doc["days"].is<JsonArray>()) return false;
  JsonArray days = doc["days"].as<JsonArray>();
  if(days.size() != 7) return false;

  for(int d=0; d<7; d++){
    weekCount[d]=0;
    JsonArray arr = days[d].as<JsonArray>();
    for(JsonObject o : arr){
      if(weekCount[d] >= MAX_EVENTS_PER_DAY) break;
      const char* t = o["time"] | "";
      if(strlen(t) < 5) continue;

      int hh = parseHH(t);
      int mm = parseMM(t);
      if(hh<0||hh>23||mm<0||mm>59) continue;

      Event &e = weekEvents[d][weekCount[d]++];
      e.hh = hh;
      e.mm = mm;
      e.duration = (int)(o["duration"] | 5);
      if(e.duration < 1) e.duration = 1;
      e.enabled = (bool)(o["enabled"] | true);
      e.firedToday = false;
    }
  }
  return true;
}

static bool saveDayFromJson(int day, const String& json){
  if(day < 0 || day > 6) return false;

  StaticJsonDocument<8192> doc;
  DeserializationError err = deserializeJson(doc, json);
  if(err) return false;
  if(!doc.is<JsonArray>()) return false;

  JsonArray arr = doc.as<JsonArray>();
  weekCount[day] = 0;

  for(JsonObject o : arr){
    if(weekCount[day] >= MAX_EVENTS_PER_DAY) break;
    const char* t = o["time"] | "";
    if(strlen(t) < 5) continue;

    int hh = parseHH(t);
    int mm = parseMM(t);
    if(hh<0||hh>23||mm<0||mm>59) continue;

    Event &e = weekEvents[day][weekCount[day]++];
    e.hh = hh;
    e.mm = mm;
    e.duration = (int)(o["duration"] | 5);
    if(e.duration < 1) e.duration = 1;
    e.enabled = (bool)(o["enabled"] | true);
    e.firedToday = false;
  }

  return saveWeekToFS();
}

// ===================== WEB UI =====================
static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="ro">
<head>
  <meta charset="utf-8"/>
  <meta name="viewport" content="width=device-width,initial-scale=1"/>
  <title>Sunete Scheduler</title>
  <style>
    :root { --bg:#0b1020; --card:#121a33; --muted:#9fb0ff; --text:#e9ecff; --ok:#3ddc97; --bad:#ff5c77; --line:rgba(255,255,255,.08); }
    *{box-sizing:border-box;font-family:system-ui,-apple-system,Segoe UI,Roboto,Ubuntu,"Helvetica Neue",Arial}
    body{margin:0;background:radial-gradient(1200px 600px at 20% 0%, #1b2a6b 0%, var(--bg) 55%); color:var(--text);}
    .wrap{max-width:1040px;margin:0 auto;padding:18px}
    .top{display:flex;gap:12px;align-items:center;justify-content:space-between;flex-wrap:wrap}
    .title{display:flex;gap:10px;align-items:center}
    .dot{width:10px;height:10px;border-radius:999px;background:var(--bad);box-shadow:0 0 18px rgba(255,92,119,.6)}
    .dot.ok{background:var(--ok);box-shadow:0 0 18px rgba(61,220,151,.6)}
    .card{background:rgba(18,26,51,.72);backdrop-filter: blur(10px);border:1px solid var(--line);border-radius:16px;padding:14px}
    .grid{display:grid;grid-template-columns:1.35fr .65fr;gap:12px}
    @media (max-width: 900px){ .grid{grid-template-columns:1fr} }
    .row{display:flex;gap:10px;align-items:center;flex-wrap:wrap}
    .k{color:var(--muted);font-size:12px}
    .v{font-weight:650}
    button{border:1px solid var(--line);background:#1a2550;color:var(--text);padding:10px 12px;border-radius:12px;cursor:pointer}
    button:hover{filter:brightness(1.08)}
    button.danger{background:#3a1730}
    button.ok{background:#153a2c}
    input, textarea, select{border:1px solid var(--line);background:#0f1736;color:var(--text);padding:10px;border-radius:12px;outline:none}
    textarea{width:100%;min-height:150px;resize:vertical}
    table{width:100%;border-collapse:collapse}
    th,td{padding:10px;border-bottom:1px solid var(--line);text-align:left}
    th{color:var(--muted);font-size:12px;font-weight:600}
    .pill{padding:6px 10px;border-radius:999px;border:1px solid var(--line);font-size:12px;color:var(--muted)}
    .right{margin-left:auto}
    .small{font-size:12px;color:var(--muted)}
    .split{display:flex;gap:10px;flex-wrap:wrap}
    .sep{height:10px}
    .tabs{display:flex;gap:8px;flex-wrap:wrap}
    .tab{padding:8px 10px;border-radius:999px;border:1px solid var(--line);background:#0f1736;color:var(--text);cursor:pointer}
    .tab.active{background:#1a2550}
  </style>
</head>
<body>
  <div class="wrap">
    <div class="top">
      <div class="title">
        <div id="dot" class="dot"></div>
        <div>
          <div class="v">Sunete Scheduler</div>
          <div class="k">ESP8266 • NTP + DST • orar pe zile • WiFi change din web</div>
        </div>
      </div>
      <div class="row card">
        <div>
          <div class="k">Ora curentă</div>
          <div id="now" class="v">--:--:--</div>
        </div>
        <div class="right">
          <div class="k">Releu</div>
          <div id="relay" class="v">--</div>
        </div>
      </div>
    </div>

    <div class="sep"></div>

    <div class="grid">
      <div class="card">
        <div class="row">
          <div class="v">Programări</div>
          <span class="pill" id="count">0</span>
          <div class="right split">
            <button class="ok" onclick="trigger()">Test ON 5s</button>
            <button onclick="loadAll()">Reload</button>
            <button class="danger" onclick="changeWiFi()">Schimbă WiFi</button>
          </div>
        </div>

        <div class="sep"></div>

        <div class="tabs" id="tabs"></div>

        <div class="sep"></div>

        <div class="row">
          <input id="t" type="time" value="22:30"/>
          <input id="d" type="number" min="1" max="120" value="5" style="width:110px" />
          <button class="ok" onclick="add()">Adaugă</button>
          <span class="small">Durata (sec)</span>
        </div>

        <div class="sep"></div>

        <table>
          <thead>
            <tr><th>Activ</th><th>Ora</th><th>Durata</th><th>Acțiuni</th></tr>
          </thead>
          <tbody id="tbody"></tbody>
        </table>
      </div>

      <div class="card">
        <div class="v">Backup / Import (pentru ziua selectată)</div>
        <div class="k">Exportă orarul zilei, editează, apoi importă înapoi</div>
        <div class="sep"></div>
        <div class="split">
          <button onclick="exportJson()">Export JSON</button>
          <button class="danger" onclick="importJson()">Import JSON</button>
        </div>
        <div class="sep"></div>
        <textarea id="json" onfocus="autoRefresh=false" onblur="autoRefresh=true"></textarea>
        <div class="sep"></div>
        <div class="small">Format: [ {"time":"HH:MM","duration":5,"enabled":true}, ... ]</div>

        <div class="sep"></div>
        <div class="v">Home Assistant</div>
        <div class="k">GET /ha/state • POST /ha/trigger • GET/POST /ha/switch</div>
      </div>
    </div>
  </div>

<script>
let day = 0;                 // 0=Mon..6=Sun
let schedule = [];
let autoRefresh = true;

const dayNames = ["Luni","Marti","Miercuri","Joi","Vineri","Sambata","Duminica"];

async function api(url, opt){
  const r = await fetch(url, opt);
  if(!r.ok) throw new Error(await r.text());
  const ct = r.headers.get("content-type")||"";
  if(ct.includes("application/json")) return await r.json();
  return await r.text();
}

function renderTabs(){
  const el = document.getElementById("tabs");
  el.innerHTML = "";
  dayNames.forEach((n, idx)=>{
    const b = document.createElement("div");
    b.className = "tab" + (idx===day ? " active":"");
    b.textContent = n;
    b.onclick = ()=>{ day = idx; renderTabs(); loadDay(); };
    el.appendChild(b);
  });
}

function render(){
  const tb = document.getElementById("tbody");
  tb.innerHTML = "";
  document.getElementById("count").textContent = schedule.length;

  schedule.sort((a,b)=> a.time.localeCompare(b.time));

  schedule.forEach((e, idx)=>{
    const tr = document.createElement("tr");

    const td0 = document.createElement("td");
    const chk = document.createElement("input");
    chk.type="checkbox"; chk.checked=!!e.enabled;
    chk.onchange = ()=>{ e.enabled = chk.checked; save(); };
    td0.appendChild(chk);

    const td1 = document.createElement("td");
    td1.innerHTML = `<b>${e.time}</b>`;

    const td2 = document.createElement("td");
    td2.innerHTML = `<span class="pill">${e.duration||5}s</span>`;

    const td3 = document.createElement("td");
    td3.className="split";

    const btnEdit = document.createElement("button");
    btnEdit.textContent="Editează";
    btnEdit.onclick = ()=>{
      const nt = prompt("Ora (HH:MM)", e.time);
      if(!nt) return;
      const nd = prompt("Durata sec", e.duration||5);
      if(!nd) return;
      e.time = nt;
      e.duration = parseInt(nd,10)||5;
      save();
    };

    const btnDel = document.createElement("button");
    btnDel.className="danger";
    btnDel.textContent="Șterge";
    btnDel.onclick = ()=>{ schedule.splice(idx,1); save(); };

    td3.appendChild(btnEdit);
    td3.appendChild(btnDel);

    tr.appendChild(td0); tr.appendChild(td1); tr.appendChild(td2); tr.appendChild(td3);
    tb.appendChild(tr);
  });

  document.getElementById("json").value = JSON.stringify(schedule, null, 2);
}

async function loadAll(){
  try{
    const st = await api("/api/status");
    document.getElementById("now").textContent = st.now + " • " + st.day_name;
    document.getElementById("relay").textContent = st.relay_on ? "ON" : "OFF";
    document.getElementById("dot").classList.toggle("ok", st.time_ok);
  }catch(e){
    console.log(e);
  }
}

async function loadDay(){
  try{
    schedule = await api("/api/day?d="+day);
    render();
  }catch(e){ console.log(e); }
}

async function save(){
  try{
    await api("/api/day?d="+day, {
      method:"POST",
      headers:{ "Content-Type":"application/json" },
      body: JSON.stringify(schedule)
    });
    await loadDay();
  }catch(e){
    alert("Save error: "+e.message);
  }
}

async function add(){
  const t = document.getElementById("t").value;
  const d = parseInt(document.getElementById("d").value,10)||5;
  if(!t){ alert("Alege ora"); return; }
  schedule.push({time:t, duration:d, enabled:true});
  await save();
}

async function trigger(){
  try{ await api("/api/trigger", {method:"POST"}); }
  catch(e){ alert(e.message); }
}

function exportJson(){
  document.getElementById("json").value = JSON.stringify(schedule, null, 2);
}

async function importJson(){
  const txt = document.getElementById("json").value;
  try{
    const parsed = JSON.parse(txt);
    if(!Array.isArray(parsed)) throw new Error("Nu e array");
    await api("/api/day?d="+day, {
      method:"POST",
      headers:{ "Content-Type":"application/json" },
      body: JSON.stringify(parsed)
    });
    await loadDay();
    alert("Import OK");
  }catch(e){
    alert("Import error: " + e.message);
  }
}

async function changeWiFi(){
  if(!confirm("Pornești portalul de configurare WiFi? Te vei conecta la AP-ul ESP (192.168.4.1).")) return;
  try{
    await api("/api/wifi", {method:"POST"});
    alert("Portal WiFi pornit. Caută WiFi-ul ESP-SUNETE-xxxx și intră la 192.168.4.1");
  }catch(e){
    alert("Eroare: "+e.message);
  }
}

// periodic status refresh (but not overwrite textarea when focused)
setInterval(()=>{ if(autoRefresh) loadAll(); }, 1000);

renderTabs();
loadAll();
loadDay();
</script>
</body>
</html>
)HTML";

// ===================== API HELPERS =====================
static void sendJson(int code, const String& body){
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(code, "application/json", body);
}

static String dayToJson(int day){
  StaticJsonDocument<8192> doc;
  JsonArray arr = doc.to<JsonArray>();
  for(int i=0;i<weekCount[day];i++){
    JsonObject o = arr.createNestedObject();
    char t[6]; snprintf(t,sizeof(t),"%02d:%02d",weekEvents[day][i].hh, weekEvents[day][i].mm);
    o["time"] = t;
    o["duration"] = weekEvents[day][i].duration;
    o["enabled"] = weekEvents[day][i].enabled;
  }
  String out; serializeJson(doc, out);
  return out;
}

// ===================== ROUTES =====================
static void handleRoot(){
  server.send(200, "text/html; charset=utf-8", FPSTR(INDEX_HTML));
}

static void handleStatus(){
  int hh,mm,ss,yday,wday;
  getLocal(hh,mm,ss,yday,wday);

  StaticJsonDocument<256> doc;
  doc["now"] = getLocalTimeStr();
  doc["relay_on"] = relayOn;
  time_t now = time(nullptr);
  doc["time_ok"] = (now > 1700000000);
  doc["day"] = wday;
  doc["day_name"] = dayNameRO(wday);

  String out; serializeJson(doc, out);
  sendJson(200, out);
}

static void handleTrigger(){
  relayPulse(5);
  sendJson(200, "{\"ok\":true}");
}

static void handleDayGet(){
  int day = server.hasArg("d") ? server.arg("d").toInt() : 0;
  if(day < 0 || day > 6){ sendJson(400, "{\"error\":\"bad_day\"}"); return; }
  sendJson(200, dayToJson(day));
}

static void handleDayPost(){
  int day = server.hasArg("d") ? server.arg("d").toInt() : 0;
  if(day < 0 || day > 6){ sendJson(400, "{\"error\":\"bad_day\"}"); return; }
  if(!server.hasArg("plain")){ sendJson(400, "{\"error\":\"no_body\"}"); return; }
  if(!saveDayFromJson(day, server.arg("plain"))){ sendJson(400, "{\"error\":\"bad_json\"}"); return; }
  sendJson(200, "{\"ok\":true}");
}

static void handleOptions(){
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.send(204);
}

// Start WiFiManager portal from web
static void handleWiFiPortal(){
  sendJson(200, "{\"ok\":true,\"msg\":\"starting_portal\"}");
  delay(300);

  // 1) Oprește serverul tău (evită conflicte)
  server.stop();

  // 2) Deconectează-te de la router
  WiFi.disconnect(true);
  delay(300);

  // 3) Forțează AP mode
  WiFi.mode(WIFI_AP_STA);
  delay(200);

  // 4) Setează IP fix pentru AP (ca să fie 192.168.4.1)
  IPAddress apIP(192,168,4,1);
  IPAddress gw(192,168,4,1);
  IPAddress sn(255,255,255,0);
  WiFi.softAPConfig(apIP, gw, sn);

  String apName = "ESP-SUNETE-" + String(ESP.getChipId(), HEX);
  wm.setConfigPortalTimeout(180);

  Serial.println();
  Serial.println("=== WIFI PORTAL START ===");
  Serial.print("AP SSID: ");
  Serial.println(apName);
  Serial.println("Open: http://192.168.4.1");
  Serial.println("=========================");

  // 5) Pornește portalul
  bool ok = wm.startConfigPortal(apName.c_str());

  Serial.println("=== WIFI PORTAL END ===");
  Serial.print("Result: ");
  Serial.println(ok ? "SAVED" : "TIMEOUT/FAIL");

  // 6) Revino la STA după portal
  WiFi.mode(WIFI_STA);
  delay(200);

  // 7) Repornește serverul tău web + NTP
  server.begin();
  syncTimeNTP();
}


// ===================== HOME ASSISTANT =====================
static void handleHaState(){
  int hh,mm,ss,yday,wday;
  getLocal(hh,mm,ss,yday,wday);

  StaticJsonDocument<512> doc;
  doc["time"] = getLocalTimeStr();
  doc["relay_on"] = relayOn;
  doc["day"] = wday;
  doc["day_name"] = dayNameRO(wday);
  doc["events_today"] = weekCount[wday];

  // next event today (or next day)
  int bestDay=-1, bestIdx=-1;
  int bestDeltaMin = 8*24*60; // big
  for(int d=0; d<7; d++){
    int deltaDay = (d - wday);
    if(deltaDay < 0) deltaDay += 7;

    for(int i=0;i<weekCount[d];i++){
      if(!weekEvents[d][i].enabled) continue;
      int nowMin = hh*60 + mm;
      int evMin  = weekEvents[d][i].hh*60 + weekEvents[d][i].mm;

      int delta = 0;
      if(deltaDay == 0){
        delta = evMin - nowMin;
        if(delta < 0) delta += 7*24*60; // next week
      } else {
        delta = deltaDay*24*60 + (evMin - nowMin);
      }

      if(delta < bestDeltaMin){
        bestDeltaMin = delta;
        bestDay = d; bestIdx = i;
      }
    }
  }

  if(bestDay>=0){
    char t[6]; snprintf(t,sizeof(t),"%02d:%02d", weekEvents[bestDay][bestIdx].hh, weekEvents[bestDay][bestIdx].mm);
    doc["next_day"] = bestDay;
    doc["next_day_name"] = dayNameRO(bestDay);
    doc["next_time"] = t;
    doc["next_in_min"] = bestDeltaMin;
  }

  String out; serializeJson(doc, out);
  sendJson(200, out);
}

static void handleHaSwitchGet(){
  StaticJsonDocument<64> doc;
  doc["on"] = relayOn;
  String out; serializeJson(doc, out);
  sendJson(200, out);
}

static void handleHaSwitchPost(){
  if(!server.hasArg("plain")){ sendJson(400, "{\"error\":\"no_body\"}"); return; }
  StaticJsonDocument<128> doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if(err){ sendJson(400, "{\"error\":\"bad_json\"}"); return; }
  bool on = doc["on"] | false;
  relayWrite(on);
  sendJson(200, "{\"ok\":true}");
}

static void handleHaTrigger(){
  int dur = 5;
  if(server.hasArg("plain") && server.arg("plain").length() > 0){
    StaticJsonDocument<128> doc;
    if(deserializeJson(doc, server.arg("plain")) == DeserializationError::Ok){
      dur = doc["duration"] | 5;
    }
  }
  relayPulse(dur);
  sendJson(200, "{\"ok\":true}");
}

// ===================== SCHEDULER =====================
static void schedulerTick(){
  int hh,mm,ss,yday,wday;
  getLocal(hh,mm,ss,yday,wday);

  // reset fired flags on day change
  if(lastYDay != yday){
    for(int d=0; d<7; d++){
      for(int i=0;i<weekCount[d];i++) weekEvents[d][i].firedToday = false;
    }
    lastYDay = yday;
  }

  // relay auto-off
  if(relayOn && (long)(millis() - relayOffAtMs) >= 0){
    relayWrite(false);
  }

  // trigger only on exact second
  if(ss != 0) return;

  // today's schedule
  for(int i=0;i<weekCount[wday];i++){
    Event &e = weekEvents[wday][i];
    if(!e.enabled) continue;
    if(e.firedToday) continue;
    if(e.hh == hh && e.mm == mm){
      relayPulse(e.duration);
      e.firedToday = true;
      break;
    }
  }
}

// ===================== SETUP / LOOP =====================
void setup(){
  pinMode(RELAY_PIN, OUTPUT);
  relayWrite(false);

  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("=== ESP SUNETE SCHEDULER START ===");

  ensureFS();
  setupTZ();

  if(!loadWeekFromFS()){
    Serial.println("⚠️ loadWeekFromFS failed -> seeding defaults");
    seedDefaultWeek();
    saveWeekToFS();
  }

  // WiFiManager autoConnect
  WiFi.mode(WIFI_STA);
  String apName = "ESP-SUNETE-" + String(ESP.getChipId(), HEX);
  wm.setConfigPortalTimeout(180);

  Serial.print("WiFi connect / AP: ");
  Serial.println(apName);

  bool res = wm.autoConnect(apName.c_str());
  if(!res){
    Serial.println("❌ WiFi NOT connected (offline mode)");
  } else {
    Serial.println("✅ WiFi connected");
    Serial.print("IP: "); Serial.println(WiFi.localIP());
  }

  // NTP sync
  bool ok=false;
  for(int i=0;i<5 && !ok;i++){
    ok = syncTimeNTP();
    delay(300);
  }
  Serial.print("Time sync: "); Serial.println(ok ? "OK" : "FAIL");
  Serial.print("Local time: "); Serial.println(getLocalTimeStr());

  // Routes
  server.on("/", handleRoot);

  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/trigger", HTTP_POST, handleTrigger);

  server.on("/api/day", HTTP_GET, handleDayGet);
  server.on("/api/day", HTTP_POST, handleDayPost);

  server.on("/api/wifi", HTTP_POST, handleWiFiPortal);

  // CORS
  server.on("/api/status", HTTP_OPTIONS, handleOptions);
  server.on("/api/trigger", HTTP_OPTIONS, handleOptions);
  server.on("/api/day", HTTP_OPTIONS, handleOptions);
  server.on("/api/wifi", HTTP_OPTIONS, handleOptions);

  // Home Assistant
  server.on("/ha/state", HTTP_GET, handleHaState);
  server.on("/ha/switch", HTTP_GET, handleHaSwitchGet);
  server.on("/ha/switch", HTTP_POST, handleHaSwitchPost);
  server.on("/ha/trigger", HTTP_POST, handleHaTrigger);

  server.onNotFound([](){ server.send(404,"text/plain","Not found"); });

  server.begin();
  Serial.println("HTTP server started");
  Serial.println("=================================");
}

void loop(){
  server.handleClient();
  schedulerTick();

  // periodic NTP resync each 6 hours (if internet exists)
  static unsigned long lastResync = 0;
  if(millis() - lastResync > 6UL*60UL*60UL*1000UL){
    syncTimeNTP();
    lastResync = millis();
  }
}

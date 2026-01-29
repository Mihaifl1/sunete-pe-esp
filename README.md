📢 ESP8266 Sunete Scheduler (Relay + Web UI + Home Assistant)

Sistem complet bazat pe ESP8266 (NodeMCU) pentru controlul unui releu de sunete / clopoțel / sirenă, cu:

⏰ programări pe zilele săptămânii

🌐 interfață web modernă

🕒 NTP + oră de vară/iarnă automată (Moldova / România)

🏠 integrare Home Assistant (REST)

📦 stocare locală în LittleFS

🔧 schimbare WiFi din interfață (WiFiManager)

✨ Funcționalități
🔔 Programare sunete

Orar independent pentru fiecare zi (Luni–Duminică)

Sâmbătă poate avea program special

Declanșare exact la secundă

Durată configurabilă (ex: 5 secunde)

🌐 Interfață Web

Adăugare / editare / ștergere ore

Activare / dezactivare evenimente

Import / Export JSON pentru fiecare zi

Test manual „Sunet ON 5s”

Afișare oră curentă + zi

Buton „Schimbă WiFi”

📡 Rețea & timp

Sincronizare oră prin NTP

Trecere automată ora de vară / iarnă

Funcționează offline (dacă internetul cade)

WiFi configurabil fără reflash

🏠 Home Assistant

Control și monitorizare prin REST API

Switch + Button + Sensor

Perfect pentru automatizări HA

🧰 Hardware necesar

ESP8266 NodeMCU (ESP-12E)

Modul releu (5V sau 3.3V)

Sarcină (clopoțel / sirenă / difuzor activ)

Alimentare stabilă (recomandat ≥ 1A)

🔌 Conectare releu
D1  → IN (releu)
GND → GND
VCC → 5V / 3.3V (în funcție de modul)

📂 Structura fișierelor (LittleFS)
/schedule_week.json
{
  "days": [
    [ { "time":"08:00", "duration":5, "enabled":true } ],
    [],
    [],
    [],
    [],
    [],
    []
  ]
}

🌐 Acces interfață web

După conectare la WiFi:

http://IP-ESP/


Exemplu:

http://192.168.1.120/

🔁 Schimbare WiFi din interfață

Apasă „Schimbă WiFi”

ESP pornește hotspot:
ESP-SUNETE-xxxx

Conectează-te la acest WiFi

Deschide în browser:

http://192.168.4.1


Alege noua rețea WiFi și parola

⚠️ Dacă apare „No Internet” → Stay connected

🔗 API Home Assistant (REST)
📍 Status complet
GET /ha/state


Răspuns:

{
  "time": "14:32:01",
  "relay_on": false,
  "day_name": "Sambata",
  "events_today": 12,
  "next_time": "15:00"
}

🔔 Sunet temporar (recomandat)
POST /ha/trigger


Cu durată:

{ "duration": 10 }

🔌 Control releu
GET  /ha/switch
POST /ha/switch

{ "on": true }

🏠 Integrare Home Assistant (exemplu YAML)
Switch
switch:
  - platform: rest
    name: Sunete ESP
    resource: http://192.168.1.120/ha/switch
    body_on: '{"on": true}'
    body_off: '{"on": false}'
    is_on_template: "{{ value_json.on }}"
    headers:
      Content-Type: application/json

Button (sunet)
button:
  - platform: rest
    name: Sunet manual
    resource: http://192.168.1.120/ha/trigger
    method: POST

⚠️ Recomandări importante

Pentru sunete / clopoțel → folosește /ha/trigger

Evită /ha/switch pentru impulsuri (poate rămâne ON)

Dezactivează VPN când configurezi WiFi

Nu opri alimentarea în timpul scrierii în LittleFS

🚀 Posibile extensii

🔐 autentificare (token/parolă)

📡 MQTT (mai stabil decât REST)

📊 statistici (câte sunete/zi)

🧑‍🏫 profil „școală / vacanță”

🌍 multi-timezone

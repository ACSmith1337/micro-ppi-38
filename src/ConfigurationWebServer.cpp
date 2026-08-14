#include "ConfigurationWebServer.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <ESPmDNS.h>
#elif defined(ARDUINO_ARCH_ESP8266)
#include <ESP8266mDNS.h>
#endif

// Forward declaration for HandleSync
#include "AircraftManager.h"

// Global log buffer instance
LogBuffer g_logBuffer;

// Log helper — writes to Serial AND the web-viewable buffer
void GridLog(const char* msg) {
    Serial.println(msg);
    g_logBuffer.log(msg);
}

// ── Minimal inline CSS (no CDN, no external deps) ──
static const char CSS_HEAD[] PROGMEM = R"rawliteral(
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Courier New',monospace;background:#0a0a0a;color:#00ff00;padding:1rem}
h1{color:#00ff00;font-size:1.3rem;margin-bottom:0.8rem;text-align:center}
fieldset{border:1px solid #004400;padding:0.8rem;margin-bottom:0.8rem}
legend{color:#00aa00;padding:0 0.4rem;font-size:0.85rem}
label{display:flex;flex-direction:column;gap:0.2rem;margin-bottom:0.6rem}
label span{color:#00aa00;font-size:0.85rem}
input,select{background:#001100;border:1px solid #004400;color:#00ff00;padding:0.4rem;font-family:inherit;font-size:0.9rem}
input:focus,select:focus{outline:2px solid #00ff00}
input[type=checkbox]{accent-color:#00ff00}
.btn{background:#003300;border:1px solid #00ff00;color:#00ff00;padding:0.5rem 1rem;font-family:inherit;font-size:0.9rem;cursor:pointer;margin:0.3rem}
.btn:hover{background:#004400}
.btn-s{background:#111;border-color:#00aa00;color:#00aa00}
.nav{display:flex;gap:0.4rem;margin-bottom:0.8rem;flex-wrap:wrap}
.nav a{color:#00aa00;text-decoration:none;padding:0.3rem 0.5rem;border:1px solid #004400}
.nav a:hover{color:#00ff00;border-color:#00ff00}
.stat{background:#001100;border:1px solid #004400;padding:0.6rem;margin:0.5rem 0;font-size:0.85rem}
.row{display:flex;justify-content:space-between;padding:0.2rem 0;border-bottom:1px solid #002200}
.lbl{color:#00aa00}.val{color:#00ff00}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:0.4rem}
@media(max-width:600px){.grid{grid-template-columns:1fr}}
.toast{position:fixed;top:0.5rem;right:0.5rem;background:#003300;border:1px solid #00ff00;color:#00ff00;padding:0.6rem;opacity:0;transition:opacity 0.3s;z-index:100}
.toast.show{opacity:1}
</style>
</head><body>
)rawliteral";

static const char NAV[] PROGMEM = R"rawliteral(
<div class="nav"><a href="/">Config</a><a href="/stats">Status</a><a href="/logs">Logs</a></div>
)rawliteral";

static const char FOOTER[] PROGMEM = R"rawliteral(
</body></html>
)rawliteral";

// ── Helper: stream a PROGMEM string ──
#if defined(ARDUINO_ARCH_ESP8266)
static void streamP(ESP8266WebServer& srv, const char* str, size_t len) {
    char buf[64];
    size_t remaining = len;
    const char* p = str;
    while (remaining > 0) {
        size_t chunk = remaining > sizeof(buf) ? sizeof(buf) : remaining;
        for (size_t i = 0; i < chunk; i++) {
            buf[i] = pgm_read_byte(p + i);
        }
        srv.sendContent(buf, chunk);
        p += chunk;
        remaining -= chunk;
    }
}
#endif

// ── Read all config values into a struct ──
struct ConfigVals {
    String latitude, longitude, maxrange, radiusDeg;
    String scanline, infotext, triangle, trails, squawkalert;
    String phosphor, scanmode;
    String datasource, readsbhost, readsbport, readsbpath, fetchinterval;
};

static ConfigVals readConfig() {
    ConfigVals c;
    Preferences p;
    p.begin("config", true);
    c.latitude    = p.getString("latitude", "");
    c.longitude   = p.getString("longitude", "");
    c.radiusDeg   = p.getString("radius", "1.0");
    c.maxrange    = p.getString("maxrange", "");
    if (c.maxrange.isEmpty()) {
        c.maxrange = String(c.radiusDeg.toFloat() * 60.0f, 1);
    }
    c.scanline    = p.getString("scanline", "true");
    c.infotext    = p.getString("infotext", "true");
    c.triangle    = p.getString("triangle", "true");
    c.trails      = p.getString("trails", "false");
    c.squawkalert = p.getString("squawkalert", "false");
    c.phosphor    = p.getString("phosphor", "green");
    c.scanmode    = p.getString("scanmode", "angular");
    c.datasource  = p.getString("datasource", "local");
    c.readsbhost  = p.getString("readsbhost", "");
    c.readsbport  = p.getString("readsbport", "8080");
    c.readsbpath  = p.getString("readsbpath", "/data/aircraft.json");
    c.fetchinterval = p.getString("fetchinterval", "3");
    p.end();
    return c;
}

// ── Checkbox/option helpers ──
static const char CHECKED[] PROGMEM = " checked";
static const char SELECTED[] PROGMEM = " selected";

#if defined(ARDUINO_ARCH_ESP32)

void ConfigurationWebServer::Initialise() {
    if (!MDNS.begin("microradar")) {
        Serial.println("[WARN] Failed to start mDNS. Continuing without mDNS...");
    }

    server->on("/", HTTP_GET, [&](AsyncWebServerRequest* request) {
        Serial.println("[GET] Config page requested");
        ConfigVals c = readConfig();

        // Build response with template processor
        String html;
        html.reserve(3000);
        for (size_t i = 0; i < sizeof(CSS_HEAD) - 1; i++) html += (char)pgm_read_byte(CSS_HEAD + i);
        html += "<title>Micro Radar</title>";
        for (size_t i = 0; i < sizeof(NAV) - 1; i++) html += (char)pgm_read_byte(NAV + i);
        html += "<h1>Micro Radar</h1><form id='cfg' action='/save' method='POST'>";
        html += "<fieldset><legend>Location &amp; Range</legend>";
        html += "<div class='grid'>";
        html += "<label><span>Latitude</span><input name='latitude' value='" + c.latitude + "'></label>";
        html += "<label><span>Longitude</span><input name='longitude' value='" + c.longitude + "'></label>";
        html += "</div>";
        html += "<label><span>Max Range (NM)</span><input name='maxrange' value='" + c.maxrange + "'></label>";
        html += "</fieldset>";
        html += "<fieldset><legend>Data Source</legend>";
        html += "<label><span>Aircraft Data</span><select name='datasource' id='ds'>";
        html += "<option value='local'";
        if (c.datasource == "local") for (size_t i = 0; i < sizeof(SELECTED)-1; i++) html += (char)pgm_read_byte(SELECTED+i);
        html += ">Local readsb/dump1090</option>";
        html += "<option value='adsblol'";
        if (c.datasource == "adsblol") for (size_t i = 0; i < sizeof(SELECTED)-1; i++) html += (char)pgm_read_byte(SELECTED+i);
        html += ">ADSB.lol (API)</option></select></label>";
        html += "<div id='lf'><div class='grid'>";
        html += "<label><span>readsb Host</span><input name='readsbhost' value='" + c.readsbhost + "'></label>";
        html += "<label><span>Port</span><input name='readsbport' value='" + c.readsbport + "'></label>";
        html += "</div><div class='grid'>";
        html += "<label><span>Fetch Interval (sec)</span><input name='fetchinterval' value='" + c.fetchinterval + "'></label>";
        html += "<label><span>JSON Path</span><input name='readsbpath' value='" + c.readsbpath + "'></label>";
        html += "</div></div></fieldset>";
        html += "<fieldset><legend>Display</legend>";
        html += "<div class='grid'>";
        html += "<label><span>Phosphor</span><select name='phosphor'>";
        html += "<option value='green'";
        if (c.phosphor == "green") for (size_t i = 0; i < sizeof(SELECTED)-1; i++) html += (char)pgm_read_byte(SELECTED+i);
        html += ">Green (P1)</option>";
        html += "<option value='amber'";
        if (c.phosphor == "amber") for (size_t i = 0; i < sizeof(SELECTED)-1; i++) html += (char)pgm_read_byte(SELECTED+i);
        html += ">Amber (P4)</option></select></label>";
        html += "<label><span>Scan Mode</span><select name='scanmode'>";
        html += "<option value='angular'";
        if (c.scanmode == "angular") for (size_t i = 0; i < sizeof(SELECTED)-1; i++) html += (char)pgm_read_byte(SELECTED+i);
        html += ">Angular Sweep</option>";
        html += "<option value='radial'";
        if (c.scanmode == "radial") for (size_t i = 0; i < sizeof(SELECTED)-1; i++) html += (char)pgm_read_byte(SELECTED+i);
        html += ">Radial Ping</option></select></label>";
        html += "</div>";
        auto cb = [&](const char* name, const String& val) {
            html += "<label style='flex-direction:row;align-items:center'><input name='";
            html += name; html += "' type='checkbox'";
            if (val == "true") for (size_t i = 0; i < sizeof(CHECKED)-1; i++) html += (char)pgm_read_byte(CHECKED+i);
            html += "><span style='margin-left:0.4rem'>";
            html += name; html += "</span></label>";
        };
        cb("scanline", c.scanline); cb("infotext", c.infotext);
        cb("triangle", c.triangle); cb("trails", c.trails); cb("squawkalert", c.squawkalert);
        html += "</fieldset>";
        html += "<div><button type='submit' class='btn'>Save</button>";
        html += "<button type='button' class='btn btn-s' id='sb'>Sync Now</button></div>";
        html += "</form>";
        for (size_t i = 0; i < sizeof(FOOTER) - 1; i++) html += (char)pgm_read_byte(FOOTER + i);

        // Inline JS
        String js;
        js.reserve(500);
        js += "<script>";
        js += "document.getElementById('cfg').addEventListener('submit',function(e){e.preventDefault();fetch(this.action,{method:'POST',body:new URLSearchParams(new FormData(this))}).then(function(){var t=document.createElement('div');t.className='toast show';t.textContent='Saved';document.body.appendChild(t);setTimeout(function(){t.remove();location.reload();},1500)});} );";
        js += "document.getElementById('sb').addEventListener('click',function(){var b=this;b.disabled=1;b.textContent='Syncing...';fetch('/sync').then(function(){b.disabled=0;b.textContent='Sync Now';var t=document.createElement('div');t.className='toast show';t.textContent='Sync triggered';document.body.appendChild(t);setTimeout(function(){t.remove();},2000)});} );";
        js += "var d=document.getElementById('ds'),l=document.getElementById('lf');function t(){l.style.display=d.value==='local'?'block':'none'}d.addEventListener('change',t);t();";
        js += "</script>";
        html += js;

        request->send(200, "text/html", html);
    });

    server->on("/save", HTTP_POST, [this](AsyncWebServerRequest* request) {
        Serial.println("[POST] Save config");
        prefs.begin("config", false);
        auto save = [request, this](const char* p) {
            const auto* pr = request->getParam(p, true);
            if (pr) prefs.putString(p, pr->value());
        };
        save("latitude"); save("longitude"); save("maxrange");
        save("datasource"); save("readsbhost"); save("readsbport");
        save("readsbpath"); save("fetchinterval"); save("phosphor"); save("scanmode");
        const auto* mr = request->getParam("maxrange", true);
        if (mr) { float v = mr->value().toFloat(); if (v > 0) prefs.putString("radius", String(v / 60.0f, 4)); }
        prefs.putString("scanline", request->hasParam("scanline", true) ? "true" : "false");
        prefs.putString("triangle", request->hasParam("triangle", true) ? "true" : "false");
        prefs.putString("infotext", request->hasParam("infotext", true) ? "true" : "false");
        prefs.putString("trails", request->hasParam("trails", true) ? "true" : "false");
        prefs.putString("squawkalert", request->hasParam("squawkalert", true) ? "true" : "false");
        prefs.end();
        request->send(200, "text/plain", "OK");
        reloadRequested = true;
    });

    server->on("/sync", HTTP_GET, [&](AsyncWebServerRequest* request) {
        Serial.println("[GET] Sync requested");
        AircraftManager::RequestForceSync();
        request->send(200, "text/plain", "Sync triggered");
    });

    server->on("/logs", HTTP_GET, [&](AsyncWebServerRequest* request) {
        String logs = g_logBuffer.dump();
        String html;
        html.reserve(sizeof(CSS_HEAD) + sizeof(NAV) + sizeof(FOOTER) + logs.length() + 200);
        for (size_t i = 0; i < sizeof(CSS_HEAD) - 1; i++) html += (char)pgm_read_byte(CSS_HEAD + i);
        html += "<title>Micro Radar Logs</title>";
        for (size_t i = 0; i < sizeof(NAV) - 1; i++) html += (char)pgm_read_byte(NAV + i);
        html += "<h1>Logs</h1><div class='stat'>";
        // Escape HTML in log text
        for (size_t i = 0; i < logs.length(); i++) {
            char ch = logs[i];
            if (ch == '<') html += "&lt;";
            else if (ch == '>') html += "&gt;";
            else if (ch == '\n') html += "<br>";
            else html += ch;
        }
        html += "</div><div style='text-align:center;margin:1rem'><button class='btn' onclick='location.reload()'>Refresh</button></div>";
        for (size_t i = 0; i < sizeof(FOOTER) - 1; i++) html += (char)pgm_read_byte(FOOTER + i);
        request->send(200, "text/html", html);
    });

    server->on("/stats", HTTP_GET, [&](AsyncWebServerRequest* request) {
        String html;
        html.reserve(2000);
        for (size_t i = 0; i < sizeof(CSS_HEAD) - 1; i++) html += (char)pgm_read_byte(CSS_HEAD + i);
        html += "<title>Micro Radar Status</title>";
        for (size_t i = 0; i < sizeof(NAV) - 1; i++) html += (char)pgm_read_byte(NAV + i);
        html += "<h1>Status</h1><div class='stat'>";
        auto row = [&](const char* l, const String& v) {
            html += "<div class='row'><span class='lbl'>"; html += l;
            html += "</span><span class='val'>"; html += v; html += "</span></div>";
        };
        row("WiFi", WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
        row("Signal", String(WiFi.RSSI()) + " dBm");
        row("IP", WiFi.localIP().toString().c_str());
        row("Heap", String(ESP.getFreeHeap()) + " B");
#if defined(ARDUINO_ARCH_ESP32)
        row("Max Block", String(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)) + " B");
#else
        row("Max Block", String(ESP.getMaxFreeBlockSize()) + " B");
#endif
        row("Sketch", String(ESP.getSketchSize()) + " / " + String(ESP.getFreeSketchSpace()));
        row("Uptime", String(millis() / 1000) + " s");
        html += "</div><div style='text-align:center;margin:1rem'>";
        html += "<button class='btn' onclick='location.reload()'>Refresh</button>";
        html += "<button class='btn btn-s' onclick='fetch(\"/sync\")'>Sync</button>";
        html += "</div>";
        for (size_t i = 0; i < sizeof(FOOTER) - 1; i++) html += (char)pgm_read_byte(FOOTER + i);
        request->send(200, "text/html", html);
    });

    server->begin();
}

#elif defined(ARDUINO_ARCH_ESP8266)

void ConfigurationWebServer::HandleRoot() {
    Serial.println("[GET] Config page requested");
    ConfigVals c = readConfig();

    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");

    // Stream header
    streamP(server, CSS_HEAD, sizeof(CSS_HEAD) - 1);
    server.sendContent("<title>Micro Radar</title>");
    streamP(server, NAV, sizeof(NAV) - 1);

    server.sendContent("<h1>Micro Radar</h1>");
    server.sendContent("<form id='cfg' action='/save' method='POST'>");

    // Location & Range
    server.sendContent("<fieldset><legend>Location &amp; Range</legend>");
    server.sendContent("<div class='grid'>");
    server.sendContent("<label><span>Latitude</span><input name='latitude' value='");
    server.sendContent(c.latitude); server.sendContent("'></label>");
    server.sendContent("<label><span>Longitude</span><input name='longitude' value='");
    server.sendContent(c.longitude); server.sendContent("'></label>");
    server.sendContent("</div>");
    server.sendContent("<label><span>Max Range (NM)</span><input name='maxrange' value='");
    server.sendContent(c.maxrange); server.sendContent("'></label>");
    server.sendContent("</fieldset>");

    // Data Source
    server.sendContent("<fieldset><legend>Data Source</legend>");
    server.sendContent("<label><span>Aircraft Data</span><select name='datasource' id='ds'>");
    server.sendContent("<option value='local'");
    if (c.datasource == "local") streamP(server, SELECTED, sizeof(SELECTED) - 1);
    server.sendContent(">Local readsb/dump1090</option>");
    server.sendContent("<option value='adsblol'");
    if (c.datasource == "adsblol") streamP(server, SELECTED, sizeof(SELECTED) - 1);
    server.sendContent(">ADSB.lol (API)</option></select></label>");

    server.sendContent("<div id='lf'><div class='grid'>");
    server.sendContent("<label><span>readsb Host</span><input name='readsbhost' value='");
    server.sendContent(c.readsbhost); server.sendContent("'></label>");
    server.sendContent("<label><span>Port</span><input name='readsbport' value='");
    server.sendContent(c.readsbport); server.sendContent("'></label>");
    server.sendContent("</div><div class='grid'>");
    server.sendContent("<label><span>Fetch Interval (sec)</span><input name='fetchinterval' value='");
    server.sendContent(c.fetchinterval); server.sendContent("'></label>");
    server.sendContent("<label><span>JSON Path</span><input name='readsbpath' value='");
    server.sendContent(c.readsbpath); server.sendContent("'></label>");
    server.sendContent("</div></div></fieldset>");

    // Display
    server.sendContent("<fieldset><legend>Display</legend>");
    server.sendContent("<div class='grid'>");
    server.sendContent("<label><span>Phosphor</span><select name='phosphor'>");
    server.sendContent("<option value='green'");
    if (c.phosphor == "green") streamP(server, SELECTED, sizeof(SELECTED) - 1);
    server.sendContent(">Green (P1)</option>");
    server.sendContent("<option value='amber'");
    if (c.phosphor == "amber") streamP(server, SELECTED, sizeof(SELECTED) - 1);
    server.sendContent(">Amber (P4)</option></select></label>");
    server.sendContent("<label><span>Scan Mode</span><select name='scanmode'>");
    server.sendContent("<option value='angular'");
    if (c.scanmode == "angular") streamP(server, SELECTED, sizeof(SELECTED) - 1);
    server.sendContent(">Angular Sweep</option>");
    server.sendContent("<option value='radial'");
    if (c.scanmode == "radial") streamP(server, SELECTED, sizeof(SELECTED) - 1);
    server.sendContent(">Radial Ping</option></select></label>");
    server.sendContent("</div>");

    // Checkboxes
    auto cb = [&](const char* name, const String& val) {
        server.sendContent("<label style='flex-direction:row;align-items:center'><input name='");
        server.sendContent(name);
        server.sendContent("' type='checkbox'");
        if (val == "true") streamP(server, CHECKED, sizeof(CHECKED) - 1);
        server.sendContent("><span style='margin-left:0.4rem'>");
        server.sendContent(name);
        server.sendContent("</span></label>");
    };
    cb("scanline", c.scanline);
    cb("infotext", c.infotext);
    cb("triangle", c.triangle);
    cb("trails", c.trails);
    cb("squawkalert", c.squawkalert);

    server.sendContent("</fieldset>");
    server.sendContent("<div><button type='submit' class='btn'>Save</button>");
    server.sendContent("<button type='button' class='btn btn-s' id='sb'>Sync Now</button></div>");
    server.sendContent("</form>");

    // Inline JS (minimal)
    server.sendContent("<script>");
    server.sendContent("document.getElementById('cfg').addEventListener('submit',function(e){e.preventDefault();fetch(this.action,{method:'POST',body:new URLSearchParams(new FormData(this))}).then(function(){var t=document.createElement('div');t.className='toast show';t.textContent='Saved';document.body.appendChild(t);setTimeout(function(){t.remove();location.reload();},1500)});} );");
    server.sendContent("document.getElementById('sb').addEventListener('click',function(){var b=this;b.disabled=1;b.textContent='Syncing...';fetch('/sync').then(function(){b.disabled=0;b.textContent='Sync Now';var t=document.createElement('div');t.className='toast show';t.textContent='Sync triggered';document.body.appendChild(t);setTimeout(function(){t.remove();},2000)});} );");
    server.sendContent("var d=document.getElementById('ds'),l=document.getElementById('lf');function t(){l.style.display=d.value==='local'?'block':'none'}d.addEventListener('change',t);t();");
    server.sendContent("</script>");

    streamP(server, FOOTER, sizeof(FOOTER) - 1);
    server.sendContent("");
}

void ConfigurationWebServer::HandleSave() {
    Serial.println("[POST] Save config");

    for (uint8_t i = 0; i < server.args(); i++) {
        Serial.printf("[POST]   %s = %s\n", server.argName(i).c_str(), server.arg(i).c_str());
    }

    prefs.begin("config", false);

    auto TrySaveParam = [&](const char* paramName) {
        if (server.hasArg(paramName)) {
            String val = server.arg(paramName);
            prefs.putString(paramName, val);
        }
    };

    TrySaveParam("latitude"); TrySaveParam("longitude"); TrySaveParam("maxrange");
    TrySaveParam("datasource"); TrySaveParam("readsbhost"); TrySaveParam("readsbport");
    TrySaveParam("readsbpath"); TrySaveParam("fetchinterval"); TrySaveParam("phosphor"); TrySaveParam("scanmode");

    if (server.hasArg("maxrange")) {
        float maxRangeNm = server.arg("maxrange").toFloat();
        if (maxRangeNm > 0.0f) {
            prefs.putString("radius", String(maxRangeNm / 60.0f, 4));
        }
    }

    prefs.putString("scanline", server.hasArg("scanline") ? "true" : "false");
    prefs.putString("triangle", server.hasArg("triangle") ? "true" : "false");
    prefs.putString("infotext", server.hasArg("infotext") ? "true" : "false");
    prefs.putString("trails", server.hasArg("trails") ? "true" : "false");
    prefs.putString("squawkalert", server.hasArg("squawkalert") ? "true" : "false");
    prefs.end();

    server.send(200, "text/plain", "OK");
    reloadRequested = true;
}

void ConfigurationWebServer::RequestReload() {
    reloadRequested = true;
}

void ConfigurationWebServer::Initialise() {
    if (!MDNS.begin("microradar")) {
        Serial.println("[WARN] Failed to start mDNS. Continuing without mDNS...");
    }

    server.on("/", std::bind(&ConfigurationWebServer::HandleRoot, this));
    server.on("/save", std::bind(&ConfigurationWebServer::HandleSave, this));
    server.on("/sync", std::bind(&ConfigurationWebServer::HandleSync, this));
    server.on("/logs", std::bind(&ConfigurationWebServer::HandleLogs, this));
    server.on("/stats", std::bind(&ConfigurationWebServer::HandleStatus, this));
    server.on("/restart", std::bind(&ConfigurationWebServer::HandleRestart, this));

    server.begin();
    Serial.println("[INFO] Config server listening on port 80");
}

void ConfigurationWebServer::HandleClient() {
    server.handleClient();
}

void ConfigurationWebServer::HandleSync() {
    Serial.println("[GET] Sync requested");
    AircraftManager::RequestForceSync();
    server.send(200, "text/plain", "Sync triggered");
}

void ConfigurationWebServer::HandleLogs() {
    String logs = g_logBuffer.dump();

    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");

    streamP(server, CSS_HEAD, sizeof(CSS_HEAD) - 1);
    server.sendContent("<title>Micro Radar Logs</title>");
    streamP(server, NAV, sizeof(NAV) - 1);
    server.sendContent("<h1>Logs</h1><div class='stat'>");

    for (size_t i = 0; i < logs.length(); i++) {
        char ch = logs[i];
        if (ch == '<') server.sendContent("&lt;");
        else if (ch == '>') server.sendContent("&gt;");
        else if (ch == '\n') server.sendContent("<br>");
        else { char b[1] = {ch}; server.sendContent(b); }
    }

    server.sendContent("</div>");
    server.sendContent("<div style='text-align:center;margin:1rem'><button class='btn' onclick='location.reload()'>Refresh</button></div>");
    streamP(server, FOOTER, sizeof(FOOTER) - 1);
    server.sendContent("");
}

void ConfigurationWebServer::HandleStatus() {
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");

    streamP(server, CSS_HEAD, sizeof(CSS_HEAD) - 1);
    server.sendContent("<title>Micro Radar Status</title>");
    streamP(server, NAV, sizeof(NAV) - 1);
    server.sendContent("<h1>Status</h1><div class='stat'>");

    auto row = [&](const char* l, const String& v) {
        server.sendContent("<div class='row'><span class='lbl'>");
        server.sendContent(l);
        server.sendContent("</span><span class='val'>");
        server.sendContent(v);
        server.sendContent("</span></div>");
    };

    row("WiFi", WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
    row("Signal", String(WiFi.RSSI()) + " dBm");
    row("IP", WiFi.localIP().toString().c_str());
    row("Heap", String(ESP.getFreeHeap()) + " B");
    row("Max Block", String(ESP.getMaxFreeBlockSize()) + " B");
    row("Sketch", String(ESP.getSketchSize()) + " / " + String(ESP.getFreeSketchSpace()));
    row("Uptime", String(millis() / 1000) + " s");

    server.sendContent("</div>");
    server.sendContent("<div style='text-align:center;margin:1rem'>");
    server.sendContent("<button class='btn' onclick='location.reload()'>Refresh</button>");
    server.sendContent("<button class='btn btn-s' onclick='fetch(\"/sync\")'>Sync</button>");
    server.sendContent("</div>");

    streamP(server, FOOTER, sizeof(FOOTER) - 1);
    server.sendContent("");
}

void ConfigurationWebServer::HandleRestart() {
    Serial.println("[INFO] Restart requested");
    server.send(200, "text/plain", "Restarting...");
    delay(500);
    ESP.restart();
}

#endif

const String ConfigurationWebServer::GetStoredString(const char* key) {
    prefs.begin("config", true);
    const String value = prefs.getString(key, "");
    prefs.end();
    return value;
}

#include "AircraftManager.h"

#include "ConfigurationWebServer.h"
#include <ArduinoJson.h>
#include <algorithm>
#include <cmath>

// ─── BlockingReadAdapter ───
// Wraps WiFiClient so ArduinoJson stream reader blocks with yield()
// instead of returning -1 on empty TCP buffer.
// ArduinoJson 7.x Reader calls source_->read() and source_->readBytes().
struct BlockingReadAdapter {
    WiFiClient* _client;
    BlockingReadAdapter(WiFiClient* c) : _client(c) {}
    int read() {
        uint32_t t0 = millis();
        while (!_client->available() && _client->connected()) { yield(); delay(1); if (millis()-t0 > 10000) return -1; }
        return (int)_client->read();
    }
    size_t readBytes(char* buf, size_t len) {
        uint32_t t0 = millis();
        size_t total = 0;
        while (total < len) {
            while (!_client->available() && _client->connected()) { yield(); delay(1); if (millis()-t0 > 10000) return total; }
            size_t n = _client->read((uint8_t*)buf + total, len - total);
            if (n <= 0) return total;
            total += n;
        }
        return total;
    }
};

// ─── Phosphor palettes (RGB565) ───
// Green P1 phosphor (P1 is actually yellowish-green CRT)
constexpr uint16_t CLR_RING_G        = 0x0140;       // R:0  G:10 B:0
constexpr uint16_t CLR_RING_BRIGHT_G = 0x07E0;       // R:0  G:63 B:0 (max brightness labels)
constexpr uint16_t CLR_SCAN_G        = 0x07E0;       // R:0  G:63 B:0
constexpr uint16_t CLR_GLOW_G        = 0x05A0;       // R:0  G:42 B:0
constexpr uint16_t CLR_CROSSHAIR_G   = 0x00A0;       // R:0  G:5  B:0
constexpr uint16_t CLR_COMMERIAL_G   = 0x05E0;       // R:0  G:47 B:0
constexpr uint16_t CLR_GLOW_COMM_G   = 0x03E0;       // R:0  G:31 B:0

// Gold P4 phosphor
constexpr uint16_t CLR_RING_A        = 0x5200;       // R:10 G:32 B:0 (dark gold ring)
constexpr uint16_t CLR_RING_BRIGHT_A = 0xE720;       // R:28 G:44 B:0 (bright gold labels)
constexpr uint16_t CLR_SCAN_A        = 0xF720;       // R:31 G:44 B:0 (gold scan line)
constexpr uint16_t CLR_GLOW_A        = 0x7380;       // R:14 G:28 B:0
constexpr uint16_t CLR_CROSSHAIR_A   = 0x0000;       // Invisible (no crosshairs in amber)
constexpr uint16_t CLR_COMMERIAL_A   = 0xE720;       // Bright gold aircraft (match labels)
constexpr uint16_t CLR_GLOW_COMM_A   = 0x6180;       // R:12 G:20 B:0

// Shared
constexpr uint16_t CLR_BG            = 0x0000;
constexpr uint16_t CLR_MILITARY      = 0xFD20;       // Bright orange (R:31 G:21 B:0)
constexpr uint16_t CLR_GLOW_MIL      = 0xF608;       // Dim orange glow
constexpr uint16_t CLR_UNKNOWN       = 0x0520;
constexpr uint16_t CLR_ALERT         = 0xF800;       // Red - emergency squawk
constexpr uint16_t CLR_ALERT_YELLOW  = 0xFFE0;       // Yellow - cycling squawk flash

// ─── Timing ───
constexpr uint32_t SCAN_INTERVAL     = 16;           // ~60fps for max smoothness
constexpr uint32_t ROTATION_MS       = 10000;        // 1 full sweep = 10s (configurable at runtime)
constexpr uint32_t FETCH_DEFAULT     = ROTATION_MS;  // fetch at each rotation
constexpr uint32_t DECAY_INTERVAL_MS = 16;           // decay tick rate (60fps for smooth fades)
constexpr uint32_t WARMUP_MS         = 10000;        // startup warm-up screen
constexpr int      MAX_AIRCRAFT      = 48;           // draw/load protection
constexpr int      MAX_RESP_BYTES    = 8192;         // heap protection
constexpr float    SCAN_SPEED        = (6.2831853f / ROTATION_MS);
constexpr uint8_t  AIRCRAFT_ERASE_RADIUS = 18;  // Cover icon + glow + trail dots
constexpr uint8_t  BRIGHTNESS_MAX    = 64;         // 64 levels for smooth fades (fade ~10s at 60fps tick)

// Ring geometry
constexpr int      RING_OUTER_PX     = 110;
constexpr int      RING_MID_PX       = (RING_OUTER_PX * 2) / 3;
constexpr int      RING_INNER_PX     = (RING_OUTER_PX * 1) / 3;

// ── Trail: 6° black erase wedge (no gradient shadow) ──
// 2 black segments behind the beam clear old pixels.
constexpr int   TRAIL_SEGMENTS    = 2;
constexpr float TRAIL_STEP_DEG    = 3.0f;

// Green phosphor gradient: 2 black erase, no shadow
constexpr uint16_t TRAIL_GRADIENT_G[] = { 0x0000, 0x0000 };

// Amber phosphor gradient: 2 black erase, no shadow
constexpr uint16_t TRAIL_GRADIENT_A[] = { 0x0000, 0x0000 };

// ── Precomputed tick directions (30° increments) ──
constexpr const float TICK_DIRS[] = {
     0, -1,  0.5f, -0.8660f,  0.8660f, -0.5f,
     1,  0,  0.5f,  0.8660f,  0.8660f,  0.5f,
     0,  1, -0.5f,  0.8660f, -0.8660f,  0.5f,
    -1,  0, -0.5f, -0.8660f, -0.8660f, -0.5f
};

// ─── Colour helpers ───
static inline uint16_t PalRing(bool amber)        { return amber ? CLR_RING_A        : CLR_RING_G; }
static inline uint16_t PalRingBright(bool amber)  { return amber ? CLR_RING_BRIGHT_A  : CLR_RING_BRIGHT_G; }
static inline uint16_t PalScan(bool amber)        { return amber ? CLR_SCAN_A         : CLR_SCAN_G; }
static inline uint16_t PalGlow(bool amber)        { return amber ? CLR_GLOW_A         : CLR_GLOW_G; }
static inline uint16_t PalCrosshair(bool amber)   { return amber ? CLR_CROSSHAIR_A    : CLR_CROSSHAIR_G; }
static inline uint16_t PalCommercial(bool amber)  { return amber ? CLR_COMMERIAL_A    : CLR_COMMERIAL_G; }
static inline uint16_t PalGlowComm(bool amber)    { return amber ? CLR_GLOW_COMM_A    : CLR_GLOW_COMM_G; }
static inline const uint16_t* PalTrailGradient(bool amber) { return amber ? TRAIL_GRADIENT_A : TRAIL_GRADIENT_G; }

// ─── Incremental scan state ───
struct ScanState {
    float angle = 0.0f;
    float c = 1.0f;
    float s = 0.0f;
};

static ScanState scanState;
static bool useAmber = false;  // Phosphor colour palette
static ScanMode currentMode = ScanMode::ANGULAR;



// ─── Shared JSON document (single 8KB BSS allocation) ───
StaticJsonDocument<16384> AircraftManager::jsonDoc;

// ─── Radial ping state ───
static uint8_t pingRadius = 0;
static uint8_t pingPhase = 0;  // 0=expand, 1=pause
static uint32_t pingLastTime = 0;
constexpr uint32_t PING_EXPAND_MS = 2500;  // ~2.5s expand
constexpr uint32_t PING_PAUSE_MS = 3000;   // ~3s pause
constexpr uint8_t PING_MAX_RADIUS = 160;   // Off-screen (240x240 display)

// ─── Incremental trig ───
static inline void RotateAngle(float &c, float &s, float delta)
{
    float nc = c - s * delta;
    float ns = s + c * delta;
    c = nc;
    s = ns;
}

static inline void Renormalise(float &c, float &s)
{
    float mag = sqrt(c * c + s * s);
    if (mag < 1e-6f) { c = 1.0f; s = 0.0f; return; }
    c /= mag;
    s /= mag;
}

static String FormatRangeNm(float nm)
{
    if (nm < 10.0f) {
        int tenths = (int)(nm * 10.0f + 0.5f);
        int whole = tenths / 10;
        int frac = tenths % 10;
        return String(whole) + "." + String(frac) + "nm";
    }
    return String((int)(nm + 0.5f)) + "nm";
}

// ════════════════════════════════════════════════════════════

enum class TargetGlyph {
    FIXED_WING,
    HELICOPTER,
    HEAVY,
    UNKNOWN
};

// ─── Aircraft type detection ───
static AircraftType GetAircraftType(const SimpleAircraft& ac)
{
    if (ac.squawk.isEmpty()) {
        return AircraftType::COMMERCIAL;
    }
    int sq = ac.squawk.toInt();
    if ((sq >= 4000 && sq <= 4999) ||
        sq >= 7000) {
        return AircraftType::MILITARY;
    }
    return AircraftType::COMMERCIAL;
}

// ─── Squawk alert detection ───
static bool IsAlertSquawk(const SimpleAircraft& ac)
{
    if (ac.squawk.isEmpty()) return false;
    int sq = ac.squawk.toInt();
    return (sq == 1200 || sq == 7500 || sq == 7600 || sq == 7700);
}

// Check if an aircraft (by ICAO) has an alert squawk — used in DecayAircraft where we only have the ICAO key
static bool IsAlertSquawkFromIcao(const String& icao);

static TargetGlyph GetTargetGlyph(const SimpleAircraft& ac)
{
    if (ac.category == "A7") return TargetGlyph::HELICOPTER;
    if (ac.category == "A5" || ac.category == "A6") return TargetGlyph::HEAVY;
    if (ac.groundspeed > 1.0f) return TargetGlyph::FIXED_WING;
    return TargetGlyph::UNKNOWN;
}

// ── Cache config values (avoids repeated Preferences reads per cycle) ──
void AircraftManager::CacheConfig()
{
    cfgDataSource = configServer.GetStoredString("datasource");
    cfgReadsbHost = configServer.GetStoredString("readsbhost");
    cfgReadsbPort = configServer.GetStoredString("readsbport");
    cfgReadsbPath = configServer.GetStoredString("readsbpath");
    String cfgInterval = configServer.GetStoredString("fetchinterval");
    if (!cfgInterval.isEmpty()) {
        int secs = (int)cfgInterval.toFloat();
        if (secs > 0) fetchInterval = (uint32_t)secs * 1000;
    }
}

// ════════════════════════════════════════════════════════════

void AircraftManager::Initialise()
{
    // ── Load config ──
    lat             = configServer.GetStoredString("latitude").toFloat();
    lon             = configServer.GetStoredString("longitude").toFloat();
    String maxRangeNmStr = configServer.GetStoredString("maxrange");
    if (!maxRangeNmStr.isEmpty()) {
        rad = maxRangeNmStr.toFloat() / 60.0f;
    } else {
        rad = configServer.GetStoredString("radius").toFloat();
    }
    displayInfoText = configServer.GetStoredString("infotext") == "true";
    displayTriangles = configServer.GetStoredString("triangle") == "true";
    displayScanLine = configServer.GetStoredString("scanline") != "false";
    displayTrailDots = configServer.GetStoredString("trails") == "true";
    alertSquawk = configServer.GetStoredString("squawkalert") == "true";
    useAmber = configServer.GetStoredString("phosphor") == "amber";

    fetchInterval = FETCH_DEFAULT;

    // Cache config values for fetch cycle
    CacheConfig();

    const float outerNm = rad * 60.0f;
    ringLabelInner = FormatRangeNm(outerNm * ((float)RING_INNER_PX / (float)RING_OUTER_PX));
    ringLabelMid   = FormatRangeNm(outerNm * ((float)RING_MID_PX   / (float)RING_OUTER_PX));
    ringLabelOuter = FormatRangeNm(outerNm);

    GridLog(String("[RADAR] lat=").c_str());
    if (rad <= 0.001f) {
        GridLog("[RADAR] WARNING: radius not set — no aircraft will appear");
    }

    scanState = {0.0f, 1.0f, 0.0f};
    initialSyncComplete = false;
    initialSyncLastAttempt = 0;
    warmupStartMs = millis();
    warmupComplete = false;
    airportsFetched = false;
    airportsFetchRetry = 0;
    fadeInComplete = false;
    fadeInRow = 0;
    lastFadeIn = 0;
    prevRad = rad;
    lastAdsblolFetch = 0;
    lastAirportFetch = 0;

    tft.fillScreen(CLR_BG);
    tft.setTextColor(PalRingBright(useAmber), CLR_BG);
    tft.setTextSize(2);
    tft.drawCentreString("RADAR WARMUP", 120, 112, 1);
}

// ── Live config reload (no restart) ──
void AircraftManager::ReloadDisplayConfig()
{
    // ── Reload radar center ──
    float newLat = configServer.GetStoredString("latitude").toFloat();
    float newLon = configServer.GetStoredString("longitude").toFloat();
    if (newLat != lat || newLon != lon) {
        lat = newLat;
        lon = newLon;
        Serial.printf("[RADAR] Center updated: %.4f, %.4f\n", lat, lon);
        // Center changed — airports need refetch
        airportsFetched = false;
    }

    // ── Reload range (affects ring labels + aircraft filtering) ──
    String newMaxRangeNmStr = configServer.GetStoredString("maxrange");
    if (!newMaxRangeNmStr.isEmpty()) {
        rad = newMaxRangeNmStr.toFloat() / 60.0f;
    } else {
        rad = configServer.GetStoredString("radius").toFloat();
    }
    const float outerNm = rad * 60.0f;
    ringLabelInner = FormatRangeNm(outerNm * ((float)RING_INNER_PX / (float)RING_OUTER_PX));
    ringLabelMid   = FormatRangeNm(outerNm * ((float)RING_MID_PX   / (float)RING_OUTER_PX));
    ringLabelOuter = FormatRangeNm(outerNm);
    Serial.printf("[RADAR] Range updated: %.1f NM outer\n", outerNm);

    // Range changed — airports need refetch
    if (abs(rad - prevRad) > 0.001f) {
        airportsFetched = false;
        prevRad = rad;
        Serial.println("[RADAR] Range changed — airports will refetch");
    }

    // ── Reload theme ──
    bool newAmber = configServer.GetStoredString("phosphor") == "amber";
    if (newAmber != useAmber) {
        useAmber = newAmber;
        Serial.printf("[RADAR] Theme changed to %s\n", useAmber ? "amber" : "green");
    }

    // ── Reload scan mode ──
    String newMode = configServer.GetStoredString("scanmode");
    if (newMode.isEmpty()) newMode = "angular";
    bool newRadial = newMode == "radial";
    if (newRadial && currentMode != ScanMode::RADIAL) {
        currentMode = ScanMode::RADIAL;
        pingRadius = 0;
        pingPhase = 0;
        pingLastTime = millis();
        Serial.println("[RADAR] Scan mode: radial ping");
    } else if (!newRadial && currentMode != ScanMode::ANGULAR) {
        currentMode = ScanMode::ANGULAR;
        scanState = {0.0f, 1.0f, 0.0f};
        Serial.println("[RADAR] Scan mode: angular sweep");
    }

    // ── Reload display toggles ──
    displayInfoText = configServer.GetStoredString("infotext") == "true";
    displayTriangles = configServer.GetStoredString("triangle") == "true";
    displayScanLine = configServer.GetStoredString("scanline") != "false";
    displayTrailDots = configServer.GetStoredString("trails") == "true";
    alertSquawk = configServer.GetStoredString("squawkalert") == "true";

    // Refresh cached fetch config
    CacheConfig();

    // ── Redraw everything with new config ──
    tft.fillScreen(CLR_BG);
    DrawRadarGrid();
    DrawRadarLabels();
}

// ── Apply single setting change (avoids EEPROM re-read race) ──
void AircraftManager::ApplyThemeChange(bool amber)
{
    if (amber != useAmber) {
        useAmber = amber;
        Serial.printf("[RADAR] Theme: %s\n", useAmber ? "amber" : "green");
    }
    tft.fillScreen(CLR_BG);
    DrawRadarGrid();
    DrawRadarLabels();
}

void AircraftManager::ApplyModeChange(bool radial)
{
    if (radial && currentMode != ScanMode::RADIAL) {
        currentMode = ScanMode::RADIAL;
        pingRadius = 0;
        pingPhase = 0;
        pingLastTime = millis();
        Serial.println("[RADAR] Scan mode: radial ping");
    } else if (!radial && currentMode != ScanMode::ANGULAR) {
        currentMode = ScanMode::ANGULAR;
        scanState = {0.0f, 1.0f, 0.0f};
        Serial.println("[RADAR] Scan mode: angular sweep");
    }
    tft.fillScreen(CLR_BG);
    DrawRadarGrid();
    DrawRadarLabels();
}

// ── Common label drawing ──
void AircraftManager::DrawRadarLabels() const
{
    const int cx = 120, cy = 120;
    tft.setTextColor(PalRingBright(useAmber));
    tft.setTextSize(1);
    tft.drawCentreString("N", cx, 2, 1);
    tft.drawCentreString("N", cx + 1, 2, 1);
    tft.drawCentreString("S", cx, 228, 1);
    tft.drawCentreString("S", cx + 1, 228, 1);
    tft.drawCentreString("E", 236, cy - 3, 1);
    tft.drawCentreString("E", 237, cy - 3, 1);
    tft.drawCentreString("W", 4, cy - 3, 1);
    tft.drawCentreString("W", 5, cy - 3, 1);
    tft.drawString(ringLabelOuter, cx + 6, cy - RING_OUTER_PX + 4, 1);
    tft.drawString(ringLabelMid,   cx + 6, cy - RING_MID_PX   + 4, 1);
    tft.drawString(ringLabelInner, cx + 6, cy - RING_INNER_PX + 4, 1);
}

bool AircraftManager::IsAmber() const { return useAmber; }
bool AircraftManager::IsRadial() const { return currentMode == ScanMode::RADIAL; }

// ── Force sync (static, called from web UI) ──
volatile bool AircraftManager::forceSyncRequested = false;

void AircraftManager::RequestForceSync()
{
    forceSyncRequested = true;
    GridLog("[RADAR] Force sync requested");
}

bool AircraftManager::HasForceSyncRequested()
{
    return forceSyncRequested;
}

void AircraftManager::Update()
{
    static uint32_t lastRotation = 0;

    // ── Startup sequence ──
    if (!initialSyncComplete) {
        uint32_t now = millis();

        // Static warmup text
        static uint32_t lastBlink = 0;
        if (now - lastBlink >= 1000) {
            lastBlink = now;
            tft.fillRect(50, 100, 140, 24, CLR_BG);
            tft.setTextColor(PalRingBright(useAmber), CLR_BG);
            tft.setTextSize(2);
            tft.drawCentreString("RADAR WARMUP", 120, 112, 1);
        }

        // Fetch airports once during warmup. A failed attempt is not retried
        // until the complete Overpass cooldown has elapsed.
        if (!airportsFetched) {
            const bool airportCooldownDone =
                lastAirportFetch == 0 || (now - lastAirportFetch >= 60000UL);
            if (lat != 0.0f && lon != 0.0f && rad > 0.001f && airportCooldownDone)
                FetchAirports(3000);
        }

        // Try aircraft sync every 1.5s
        if ((now - initialSyncLastAttempt) >= 1500 || (now - warmupStartMs) >= 10000) {
            initialSyncLastAttempt = now;
            if (RefreshAircraft() || (now - warmupStartMs) >= 10000) {
                initialSyncComplete = true;
                lastRotation = now;

                if ((now - warmupStartMs) >= 10000) {
                    GridLog("[RADAR] Warmup timeout, starting sweep without sync");
                } else {
                    GridLog("[RADAR] Initial sync complete, starting sweep");
                }

                // Beam reveal: screen is already black. Sweep a thin bright beam down,
                // drawing radar rows as we go.
                fadeInComplete = false;
                fadeInRow = 0;
                lastFadeIn = now;

                return;  // Let Update() handle reveal next calls
            }
        }
        return;
    }

    // ── Beam reveal ──
    if (!fadeInComplete) {
        uint32_t now = millis();
        if (now - lastFadeIn >= 12) {
            lastFadeIn = now;
            if (fadeInRow < 240) {
                int y = fadeInRow;
                int cx = 120, cy = 120;
                uint16_t ringClr = PalRing(useAmber);
                uint16_t ringBrightClr = PalRingBright(useAmber);
                uint16_t crossClr = PalCrosshair(useAmber);
                uint16_t scanClr = PalScan(useAmber);

                tft.startWrite();

                // ── Helper: draw radar content for one row ──
                auto drawRow = [&](int row) {
                    // Circles
                    for (int r : {RING_OUTER_PX, RING_MID_PX, RING_INNER_PX}) {
                        int dy = row - cy;
                        if (abs(dy) <= r) {
                            int dx = (int)round(sqrtf((float)(r * r - dy * dy)));
                            int x1 = cx - dx, x2 = cx + dx;
                            if (x1 < 0) x1 = 0;
                            if (x2 > 239) x2 = 239;
                            if (x1 <= x2) {
                                tft.drawPixel(x1, row, ringClr);
                                tft.drawPixel(x2, row, ringClr);
                            }
                        }
                    }
                    // Crosshairs
                    if (crossClr != CLR_BG) {
                        tft.drawPixel(cx, row, crossClr);
                        tft.drawPixel(cx + 1, row, crossClr);
                        if (row == cy) tft.drawFastHLine(0, row, 240, crossClr);
                    }
                    // Tick marks (30° intervals, skip cardinal)
                    for (int i = 0; i < 12; i++) {
                        if (i == 0 || i == 3 || i == 6 || i == 9) continue;
                        float tdx = TICK_DIRS[i * 2], tdy = TICK_DIRS[i * 2 + 1];
                        int tx1 = cx + (int)(tdx * 106), ty1 = cy + (int)(tdy * 106);
                        int tx2 = cx + (int)(tdx * 114), ty2 = cy + (int)(tdy * 114);
                        int tMinY = min(ty1, ty2), tMaxY = max(ty1, ty2);
                        if (row >= tMinY && row <= tMaxY) {
                            int tx = (ty2 == ty1) ? tx1 : tx1 + (int)((float)(tx2 - tx1) * (row - ty1) / (ty2 - ty1));
                            tft.drawPixel(tx, row, ringClr);
                        }
                    }
                    // North bright tick (y=4..14)
                    if (row >= 4 && row <= 14) tft.drawPixel(cx, row, ringBrightClr);
                    // Labels — draw only on the exact target row (no redundant redraws)
                    tft.setTextColor(ringBrightClr);
                    tft.setTextSize(1);
                    if (row == 2) { tft.drawCentreString("N", cx, 2, 1); tft.drawCentreString("N", cx + 1, 2, 1); }
                    if (row == 228) { tft.drawCentreString("S", cx, 228, 1); tft.drawCentreString("S", cx + 1, 228, 1); }
                    if (row == 117) { tft.drawCentreString("E", 236, 117, 1); tft.drawCentreString("E", 237, 117, 1); tft.drawCentreString("W", 4, 117, 1); tft.drawCentreString("W", 5, 117, 1); }
                    if (row == 14) tft.drawString(ringLabelOuter.c_str(), cx + 6, 14, 1);
                    if (row == 51) tft.drawString(ringLabelMid.c_str(), cx + 6, 51, 1);
                    if (row == 88) tft.drawString(ringLabelInner.c_str(), cx + 6, 88, 1);
                    // Airport markers (use cached screen coords)
                    for (const auto& ap : airports) {
                        int apSx = ap.sx;
                        int apSy = ap.sy;
                        if (apSx <= 0 || apSx >= 239 || apSy <= 0 || apSy >= 239) continue;
                        if (apSy >= row - 3 && apSy <= row + 2) {
                            tft.drawPixel(apSx, apSy, 0xFFFF);
                            tft.drawPixel(apSx - 1, apSy - 1, 0xFFFF);
                            tft.drawPixel(apSx + 1, apSy - 1, 0xFFFF);
                            tft.drawPixel(apSx - 2, apSy - 2, 0xFFFF);
                            tft.drawPixel(apSx + 2, apSy - 2, 0xFFFF);
                            tft.drawPixel(apSx, apSy - 3, 0xFFFF);
                            tft.drawPixel(apSx - 1, apSy + 1, 0xFFFF);
                            tft.drawPixel(apSx + 1, apSy + 1, 0xFFFF);
                            tft.drawPixel(apSx, apSy + 2, 0xFFFF);
                        }
                    }
                };

                // ── Erase beam from previous row, redraw content ──
                if (y > 0) {
                    int py = y - 1;
                    // Clear the row (erases beam line from previous frame)
                    tft.drawFastHLine(0, py, 240, CLR_BG);
                    // Redraw grid content for that row
                    drawRow(py);
                }

                // ── Clear current row before drawing content ──
                tft.drawFastHLine(0, y, 240, CLR_BG);

                // ── Draw content for current row ──
                drawRow(y);

                // ── Beam line on top of the grid, never on top of HUD ──
                tft.drawFastHLine(0, y, 240, scanClr);
                // Restore multi-row HUD glyphs and airport symbols after the
                // line is drawn. This prevents row erasure from leaving holes.
                DrawRadarLabels();
                DrawAirportMarkers();

                tft.endWrite();

                fadeInRow++;
            } else {
                // Reveal complete — redraw clean grid, labels, and airports in order.
                fadeInComplete = true;
                DrawRadarGrid();
                DrawRadarLabels();
                DrawAirportMarkers();
            }
        }
        return;
    }

    // ── Fetch once per rotation (or more often after failures) ──
    // Exponential backoff: 10s → 5s → 10s → 5s on consecutive failures, max 5s
    static uint8_t consecutiveFailures = 0;
    bool forceSync = AircraftManager::HasForceSyncRequested();
    if (millis() - lastRotation >= fetchInterval || forceSync) {
        if (forceSync) {
            AircraftManager::forceSyncRequested = false;
            airportsFetched = false;  // Also refresh airports
            GridLog("[RADAR] Executing force sync");
        }
        lastRotation = millis();
        if (!RefreshAircraft()) {
            consecutiveFailures++;
            // Backoff: 5s on first failure, stay at 5s on repeated failures
            fetchInterval = 5000;
        } else {
            consecutiveFailures = 0;
            // Restore fetch interval from config on success
            CacheConfig();
        }
    }

    // ── Retry only after the complete Overpass cooldown ──
    if (!airportsFetched && lat != 0.0f && lon != 0.0f && rad > 0.001f) {
        uint32_t now = millis();
        if (lastAirportFetch == 0 || now - lastAirportFetch >= 60000UL)
            FetchAirports(10000);
    }

    // ── Scan animation ──
    static uint32_t nextScan = 0;
    uint32_t now = millis();
    if (nextScan == 0) nextScan = now;
    if ((int32_t)(now - nextScan) >= 0) {
        DrawRadarFrame();
        nextScan += SCAN_INTERVAL;
        if ((uint32_t)(now - nextScan) > (SCAN_INTERVAL * 4)) {
            nextScan = now + SCAN_INTERVAL;
        }
    }

    // ── PPI phosphor decay ──
    static uint32_t lastDecay = 0;
    if (now - lastDecay >= DECAY_INTERVAL_MS) {
        DecayAircraft(now - lastDecay);
        lastDecay = now;
    }
}

// ── Called once per rotation ──
bool AircraftManager::RefreshAircraft()
{
    // WiFi dropout detection
    static bool wifiWasConnected = true;
    if (WiFi.status() != WL_CONNECTED) {
        if (wifiWasConnected) {
            GridLog("[FETCH] WiFi disconnected — waiting for reconnection");
            wifiWasConnected = false;
        }
        return false;
    }
    if (!wifiWasConnected) {
        GridLog("[FETCH] WiFi reconnected");
        wifiWasConnected = true;
    }

    String dataSource = cfgDataSource;
    if (dataSource.isEmpty()) dataSource = "adsblol";

    bool fetchOk = false;
    if (dataSource == "local") {
        constexpr uint32_t LOCAL_FETCH_MIN = 10000;
        if (lastFetchAttempt > 0 && millis() - lastFetchAttempt < LOCAL_FETCH_MIN)
            return true;
        lastFetchAttempt = millis();
        fetchOk = FetchLocal();
    } else if (dataSource == "adsblol") {
        // ADSB.lol requires a hard 60-second minimum between attempts.
        constexpr uint32_t ADSBLol_FETCH_MIN = 60000;
        if (lastFetchAttempt > 0 && millis() - lastFetchAttempt < ADSBLol_FETCH_MIN)
            return true;
        lastFetchAttempt = millis();
        fetchOk = FetchAdsblol();
    } else {
        Serial.printf("[FETCH] Unknown datasource: %s\n", dataSource.c_str());
        return false;
    }

    if (!fetchOk) {
        GridLog("[FETCH] Network error - keeping existing aircraft");
        return false;
    }
    lastFetch = millis();

    // Update positions from API data
    int onScreenCount = 0;
    for (auto& [icao, ac] : trackedAircraft) {
        auto proj = ProjectCoordinateToScreen(ac.lat, ac.lon);
        int x = proj.first, y = proj.second;
        bool on = (x > 0 && x < 239 && y > 0 && y < 239);
        if (on) onScreenCount++;

        // ── Record trail history (waypoint-compressed) ──
        if (on && displayTrailDots) {
            auto& hist = trailHistories[icao];
            // Only store a new waypoint if direction changed significantly
            bool shouldRecord = false;
            if (hist.count == 0) {
                shouldRecord = true;  // First point always recorded
            } else if (hist.count == 1) {
                shouldRecord = true;  // Second point needed to establish direction
            } else {
                // Check if heading changed enough from the last two waypoints
                int tail = (hist.head - hist.count + TRAIL_WAYPOINTS_MAX) % TRAIL_WAYPOINTS_MAX;
                int prev = (hist.head - 1 + TRAIL_WAYPOINTS_MAX) % TRAIL_WAYPOINTS_MAX;
                const auto& p0 = hist.points[tail];
                const auto& p1 = hist.points[prev];
                // Direction from p0→p1
                int dx1 = p1.x - p0.x;
                int dy1 = p1.y - p0.y;
                // Direction from p1→new
                int dx2 = x - p1.x;
                int dy2 = y - p1.y;
                // Cross product magnitude = |v1|×|v2|×sin(θ)
                // Dot product = |v1|×|v2|×cos(θ)
                // sin²(15°) ≈ 0.06699 → compare cross² ≥ 0.067 × dot²
                // Use fixed-point: 67/1000 to avoid float
                long cross = (long)dx1 * dy2 - (long)dy1 * dx2;
                long dot = (long)dx1 * dx2 + (long)dy1 * dy2;
                if (cross < 0) cross = -cross;
                // |cross| / |dot| ≥ tan(15°) ≈ 0.268 → cross × 1000 ≥ dot × 268
                if (cross * 1000 >= (long)abs(dot) * 268) {
                    shouldRecord = true;
                }
            }
            if (shouldRecord) {
                hist.points[hist.head].x = x;
                hist.points[hist.head].y = y;
                hist.points[hist.head].timestamp = millis();
                hist.head = (hist.head + 1) % TRAIL_WAYPOINTS_MAX;
                if (hist.count < TRAIL_WAYPOINTS_MAX) hist.count++;
            }
        }

        auto lpIt = lastPositions.find(icao);
        if (on) {
            // New aircraft start invisible — beam sweep reveals them
            if (lpIt == lastPositions.end()) {
                lastPositions[icao] = {x, y, false, 0, ac.rssi};
            } else {
                // Position updated by feed — erase old position, reset to invisible
                // Beam must re-illuminate at new position (real PPI behavior)
                lpIt->second.x = x;
                lpIt->second.y = y;
                lpIt->second.visible = false;
                lpIt->second.brightness = 0;
                lpIt->second.decayAccum = 0.0f;
                lpIt->second.rssi = ac.rssi;
            }
        } else {
            // Off-screen
            if (lpIt != lastPositions.end()) {
                lpIt->second.x = x;
                lpIt->second.y = y;
                lpIt->second.visible = false;
                lpIt->second.brightness = 0;
                lpIt->second.rssi = ac.rssi;
            } else {
                lastPositions[icao] = {x, y, false, 0, ac.rssi};
            }
        }
    }

    return true;
}

// ── Decay blip brightness (RSSI-based: 4.4s weak → 8.8s strong) ──
// Tracked aircraft decay at their fixed position. Beam recharges on contact.
// Ghosts (left the feed) also decay, then are removed when faded.
void AircraftManager::DecayAircraft(uint32_t tickMs)
{
    for (auto& [icao, lp] : lastPositions) {
        // Skip fully faded ghosts — they'll be cleaned up
        if (!lp.visible && lp.brightness == 0 && !trackedAircraft.count(icao)) continue;

        bool isTracked = trackedAircraft.count(icao) > 0;

        // Tracked aircraft: persist at full brightness even if off-screen
        // Ghosts: skip if invisible (wait for fade cleanup)
        if (!isTracked && !lp.visible) continue;

        // Tracked aircraft: decay at fixed position; beam recharges on contact
        // Ghosts (left the feed): fade out with RSSI-based duration
        if (isTracked) {
            // Tracked aircraft decay so they reappear on each beam sweep
            // RSSI-based fade: 5s weak (-120dBm) → 8.8s strong (-70dBm)
            auto acIt = trackedAircraft.find(icao);
            bool isSquawkAircraft = (acIt != trackedAircraft.end()) && IsAlertSquawk(acIt->second);

            // RSSI-driven fade duration
            float rssi = 0.0f;
            if (acIt != trackedAircraft.end()) rssi = acIt->second.rssi;
            float fadeDuration = 8.8f;  // default strong
            if (rssi < 0.0f) {
                float rssiNorm = constrain((rssi + 120.0f) / 50.0f, 0.0f, 1.0f);
                fadeDuration = 5.0f + 3.8f * rssiNorm;  // 5s weak → 8.8s strong
            }

            // Squawk aircraft fade 2x faster
            if (isSquawkAircraft) fadeDuration *= 0.5f;

            // Rate: brightness steps per ms, multiplied by actual tick duration
            float stepsPerMs = (float)BRIGHTNESS_MAX / (fadeDuration * 1000.0f);
            float decayAmount = stepsPerMs * tickMs;

            float& acc = lp.decayAccum;
            acc += decayAmount;
            while (acc >= 1.0f && lp.brightness > 0) {
                acc -= 1.0f;
                lp.brightness--;
            }
            if (lp.brightness == 0) {
                lp.visible = false;
                lp.decayAccum = 0.0f;
            }
        } else {
            // Ghost: RSSI-based fade duration
            float rssi = lp.rssi;
            float fadeDuration = 6.6f;
            if (rssi < 0.0f) {
                float rssiNorm = (rssi + 90.0f) / 60.0f;
                if (rssiNorm < 0.0f) rssiNorm = 0.0f;
                if (rssiNorm > 1.0f) rssiNorm = 1.0f;
                fadeDuration = 4.4f + 4.4f * rssiNorm;
            }

            // Rate: brightness steps per ms, multiplied by actual tick duration
            float stepsPerMs = (float)BRIGHTNESS_MAX / (fadeDuration * 1000.0f);
            float decayAmount = stepsPerMs * tickMs;

            float& acc = lp.decayAccum;
            acc += decayAmount;
            while (acc >= 1.0f && lp.brightness > 0) {
                acc -= 1.0f;
                lp.brightness--;
            }

            if (lp.brightness == 0) {
                lp.visible = false;
                lp.decayAccum = 0.0f;
            } else {
                SimpleAircraft ghost;
                ghost.category = "";
                ghost.squawk = "";
                DrawAircraftBlip(lp.x, lp.y, ghost, lp.brightness);
            }
        }
    }

    // Remove ghosts that have fully faded OR vanished from feed >30s ago
    // Only remove if the aircraft is actually gone from trackedAircraft
    std::vector<String> gone;
    uint32_t now = millis();
    constexpr uint32_t GHOST_TIMEOUT_MS = 30000;  // Force-remove ghosts 30s after leaving feed
    for (auto& [icao, lp] : lastPositions) {
        bool isGhost = !trackedAircraft.count(icao);
        bool faded = !lp.visible && lp.brightness == 0;
        bool timedOut = isGhost && lp.vanishedMs > 0 && (now - lp.vanishedMs) > GHOST_TIMEOUT_MS;
        // Only purge if it's a ghost AND either faded or timed out
        if (isGhost && (faded || timedOut)) {
            gone.push_back(icao);
        }
    }
    for (auto& icao : gone) {
        lastPositions.erase(icao);
        trailHistories.erase(icao);
    }
}

// ── Shared alert state accessible from both scan modes ──
struct AlertGlobals {
    static bool blinkOn;
    static char icaoBuf[8];
    static char textBuf[32];
    static bool active;
};
bool AlertGlobals::blinkOn = false;
char AlertGlobals::icaoBuf[8] = {0};
char AlertGlobals::textBuf[32] = {0};
bool AlertGlobals::active = false;

// ── Update alert state (call once per frame from both scan modes) ──
void AircraftManager::UpdateAlertState(bool displayAlerts)
{
    static uint32_t lastBlink = 0;
    static uint32_t lastCycle = 0;
    static int idx = 0;
    static char pool[16][8];
    static bool initialised = false;
    constexpr uint32_t CYCLE_MS = 3000;

    int count = 0;
    if (displayAlerts) {
        for (const auto& [k, ac] : trackedAircraft) {
            if (IsAlertSquawk(ac)) {
                if (count < 16) {
                    strncpy(pool[count], k.c_str(), 7);
                    pool[count][7] = '\0';
                    count++;
                }
            }
        }
    }

    if (count > 0) {
        uint32_t now = millis();
        if (!initialised) {
            lastCycle = now;
            lastBlink = now;
            initialised = true;
        }
        char* cur;
        if (count == 1) {
            cur = pool[0];
        } else {
            if (now - lastCycle >= CYCLE_MS) {
                idx = (idx + 1) % count;
                lastCycle = now;
                AlertGlobals::blinkOn = true;
                lastBlink = now;
            }
            cur = pool[idx];
        }
        if (strcmp(AlertGlobals::icaoBuf, cur) != 0) {
            strncpy(AlertGlobals::icaoBuf, cur, 7);
            AlertGlobals::icaoBuf[7] = '\0';
            const char* icaoC = AlertGlobals::icaoBuf;
            SimpleAircraft* ac = nullptr;
            for (auto& [k, v] : trackedAircraft) {
                if (strncmp(k.c_str(), icaoC, 8) == 0) { ac = &v; break; }
            }
            if (ac) snprintf(AlertGlobals::textBuf, sizeof(AlertGlobals::textBuf), "SQUAWK %s", ac->squawk.c_str());
        }
        AlertGlobals::active = true;
    } else {
        if (AlertGlobals::active) {
            tft.fillRect(80, 214, 80, 12, CLR_BG);
            AlertGlobals::active = false;
        }
        AlertGlobals::textBuf[0] = '\0';
        AlertGlobals::icaoBuf[0] = '\0';
        idx = 0;
    }

    if (AlertGlobals::textBuf[0] != '\0') {
        uint32_t now = millis();
        if (now - lastBlink >= 400) {
            AlertGlobals::blinkOn = !AlertGlobals::blinkOn;
            lastBlink = now;
        }
    }
}

// ── Draw alert text with box border (call from both scan modes) ──
// Always redraw when active — trail erases it every frame
void AircraftManager::DrawAlertText(bool displayAlerts)
{
    bool active = displayAlerts && AlertGlobals::textBuf[0] != '\0';

    if (active) {
        // Mask the complete indicator interior so aircraft and the beam
        // cannot remain visible beneath or around the notification text.
        tft.fillRect(79, 196, 82, 14, CLR_BG);
        // Draw box border
        uint16_t boxCol = AlertGlobals::blinkOn ? CLR_ALERT : CLR_RING_BRIGHT_A;
        tft.drawRect(78, 195, 84, 16, boxCol);
        // Draw text inside box
        tft.setTextColor(boxCol, CLR_BG);
        tft.setTextSize(1);
        tft.drawCentreString(AlertGlobals::textBuf, 120, 200, 1);
    } else {
        // Clear alert area when no active alert — prevents stale squawk text/box
        if (AlertGlobals::active || AlertGlobals::textBuf[0] != '\0') {
            tft.fillRect(78, 195, 84, 16, CLR_BG);
        }
        AlertGlobals::active = false;
        AlertGlobals::textBuf[0] = '\0';
        AlertGlobals::icaoBuf[0] = '\0';
    }

    AlertGlobals::active = active;
}

// ── Draw all aircraft blips with alert flash support ──
void AircraftManager::DrawAllAircraft(bool displayAlerts)
{
    // Draw ALL visible aircraft every frame — beam sets brightness
    for (const auto& [icao, lp] : lastPositions) {
        bool isTracked = trackedAircraft.count(icao) > 0;
        // Tracked aircraft: always draw if visible (brightness may be 0 off-screen)
        // Ghosts: only draw if visible and has brightness
        if (!isTracked && (!lp.visible || lp.brightness == 0)) continue;
        if (!lp.visible) continue;  // Don't draw off-screen aircraft
        auto it = trackedAircraft.find(icao);
        SimpleAircraft* acPtr = (it != trackedAircraft.end()) ? &it->second : nullptr;
        SimpleAircraft ghost; ghost.category = ""; ghost.squawk = "";
        const SimpleAircraft& acRef = acPtr ? *acPtr : ghost;

        // Determine color: emergency squawk = red/yellow flash
        // Military aircraft (squawk 4000-4999, 7000+) keep their orange color
        uint16_t drawColor = 0;
        bool isEmergencySquawk = displayAlerts && IsAlertSquawk(acRef);
        bool isMilitary = false;
        if (acRef.squawk.length() > 0) {
            int sq = acRef.squawk.toInt();
            isMilitary = (sq >= 4000 && sq <= 4999) || (sq >= 7000);
        }
        bool isCycling = displayAlerts && AlertGlobals::icaoBuf[0] != '\0' && strncmp(icao.c_str(), AlertGlobals::icaoBuf, 8) == 0;

        // Only override color for emergency squawks, not military
        if (isEmergencySquawk && !isMilitary) {
            if (isCycling) {
                drawColor = AlertGlobals::blinkOn ? CLR_ALERT_YELLOW : CLR_ALERT;
            } else {
                drawColor = CLR_ALERT;
            }
        }

        if (drawColor) {
            DrawAircraftBlip(lp.x, lp.y, acRef, lp.brightness, drawColor);
        } else {
            DrawAircraftBlip(lp.x, lp.y, acRef, lp.brightness);
        }
    }
}

// ── Incremental scan frame ──
void AircraftManager::DrawRadarFrame()
{
    if (!displayScanLine) return;

    const int cx = 120, cy = 120;
    const int r = 119;

    // ── Mode switch: angular sweep vs radial ping ──
    if (currentMode == ScanMode::RADIAL) {
        DrawRadarPing(cx, cy, r);
        return;
    }

    // ── Advance scan angle ──
    constexpr float DEG1 = 0.0174533f;
    static uint32_t lastStepMs = 0;
    uint32_t nowMs = millis();
    uint32_t dtMs = (lastStepMs == 0) ? SCAN_INTERVAL : (nowMs - lastStepMs);
    lastStepMs = nowMs;
    if (dtMs > 250) dtMs = SCAN_INTERVAL;

    float delta = SCAN_SPEED * (float)dtMs;
    float prevHeadC = scanState.c;
    float prevHeadS = scanState.s;
    RotateAngle(scanState.c, scanState.s, -delta);

    static int normCount = 0;
    if (++normCount >= 180) {
        Renormalise(scanState.c, scanState.s);
        normCount = 0;
    }

    float headC = scanState.c;
    float headS = scanState.s;

    // ── Phosphor trail (clears + fades behind beam) ──
    DrawTrail(cx, cy, RING_OUTER_PX, headC, headS);

    // ── Bright scan line (clipped to ring boundary — no edge artifacts) ──
    const int beamR = RING_OUTER_PX - 1;
    float headNextC = headC + headS * DEG1;
    float headNextS = headS - headC * DEG1;
    int tx1 = cx + (int)(headC * beamR);
    int ty1 = cy - (int)(headS * beamR);
    int tx2 = cx + (int)(headNextC * beamR);
    int ty2 = cy - (int)(headNextS * beamR);
    tx1 = max(0, min(239, tx1)); ty1 = max(0, min(239, ty1));
    tx2 = max(0, min(239, tx2)); ty2 = max(0, min(239, ty2));
    tft.fillTriangle(cx, cy, tx1, ty1, tx2, ty2, PalScan(useAmber));

    // ── Clean beam outer edge (3px arc band beyond ring) ──
    // Draw a thin trapezoid at the beam edge, not from center
    for (int e = beamR + 1; e <= beamR + 3; e++) {
        int x1 = cx + (int)(headC * e);
        int y1 = cy - (int)(headS * e);
        int x2 = cx + (int)(headNextC * e);
        int y2 = cy - (int)(headNextS * e);
        x1 = max(0, min(239, x1)); y1 = max(0, min(239, y1));
        x2 = max(0, min(239, x2)); y2 = max(0, min(239, y2));
        tft.drawLine(x1, y1, x2, y2, CLR_BG);
    }

    // Grid redraw is throttled. Trail erasure only needs periodic restoration;
    // redrawing the full grid at 60 fps creates avoidable SPI load and jitter.
    static uint8_t gridCounter = 29;
    if (++gridCounter >= 30) {
        gridCounter = 0;
        DrawRadarGrid();
    }
    // The beam/trail can cross HUD text and airport glyphs every frame.
    // Restore these lightweight overlays each frame while keeping expensive
    // circle/tick redraws throttled.
    DrawRadarLabels();
    DrawAirportMarkers();

    // ── Update screen positions from last known API coordinates (no dead reckoning) ──
    for (auto& [icao, lp] : lastPositions) {
        auto it = trackedAircraft.find(icao);
        if (it == trackedAircraft.end()) continue;

        auto proj = ProjectCoordinateToScreen(it->second.lat, it->second.lon);
        int nx = proj.first;
        int ny = proj.second;
        bool on = (nx > 0 && nx < 239 && ny > 0 && ny < 239);

        if (on) {
            lp.x = nx;
            lp.y = ny;
        }
    }

    // ── PPI beam-hit refresh (brightness only — DrawAllAircraft draws everything) ──
    float touchHalfAngle = delta + (DEG1 * 2.0f);
    if (touchHalfAngle < (DEG1 * 4.0f)) touchHalfAngle = DEG1 * 4.0f;
    if (touchHalfAngle > (DEG1 * 12.0f)) touchHalfAngle = DEG1 * 12.0f;
    // Avoid sqrtf: compare (dot·d)² >= d² × cos² instead of dot >= cos
    float beamTouchCos = cosf(touchHalfAngle);
    float beamTouchCos2 = beamTouchCos * beamTouchCos;
    for (auto& [icao, lp] : lastPositions) {
        int vx = lp.x - cx;
        int vy = cy - lp.y;
        float d2 = (float)(vx * vx + vy * vy);
        if (d2 < 16.0f) continue;

        // Dot product of (vx,vy) with beam directions
        float dotNow  = vx * headC     + vy * headS;
        float dotPrev = vx * prevHeadC + vy * prevHeadS;
        // Only consider aircraft in front of the beam (positive dot)
        float dot = (dotNow > dotPrev) ? dotNow : dotPrev;
        if (dot < 0) continue;
        float dot2 = dot * dot;

        if (dot2 >= d2 * beamTouchCos2) {
            // Set brightness based on RSSI: strong signal = bright, weak = dimmer
            auto it = trackedAircraft.find(icao);
            if (it != trackedAircraft.end()) {
                float rssi = it->second.rssi;
                if (rssi != 0.0f) {
                    float normalized = constrain((rssi + 95.0f) / 25.0f, 0.0f, 1.0f);
                    lp.brightness = (uint8_t)(BRIGHTNESS_MAX * (0.5f + normalized * 0.5f));
                } else {
                    lp.brightness = BRIGHTNESS_MAX;
                }
            } else {
                lp.brightness = BRIGHTNESS_MAX / 2;  // Ghost aircraft
            }
            lp.visible = true;
            lp.decayAccum = 0.0f;
        }
    }

    // ── Alert update, aircraft, then alert box as the final foreground layer ──
    UpdateAlertState(alertSquawk);
    DrawAllAircraft(alertSquawk);
    DrawAlertText(alertSquawk); // Masks beam and aircraft overlap
}

// ── Draw phosphor trail ──
void AircraftManager::DrawTrail(int cx, int cy, int r, float headC, float headS)
{
    constexpr float STEP_C = 0.9986295f;  // cos(3°) precomputed
    constexpr float STEP_S = 0.0523360f;  // sin(3°) precomputed
    const uint16_t* gradient = PalTrailGradient(useAmber);

    float prevC = headC;
    float prevS = headS;

    for (int i = 0; i < TRAIL_SEGMENTS; i++) {
        float segC = prevC * STEP_C - prevS * STEP_S;
        float segS = prevS * STEP_C + prevC * STEP_S;
        uint16_t color = gradient[i];
        // Clip trail endpoints to radar circle boundary
        int x1 = cx + (int)(prevC * r);
        int y1 = cy - (int)(prevS * r);
        int x2 = cx + (int)(segC * r);
        int y2 = cy - (int)(segS * r);
        // Ensure endpoints are within display bounds
        x1 = max(0, min(239, x1)); y1 = max(0, min(239, y1));
        x2 = max(0, min(239, x2)); y2 = max(0, min(239, y2));
        tft.fillTriangle(cx, cy, x1, y1, x2, y2, color);
        prevC = segC; prevS = segS;
    }
}

// ── Radial ping (sonar mode) ──
void AircraftManager::DrawRadarPing(int cx, int cy, int r)
{
    (void)r;
    uint32_t now = millis();
    if (pingLastTime == 0) pingLastTime = now;

    uint32_t elapsed = now - pingLastTime;

    // Track previous ring radius to erase it (no full screen clear)
    static uint8_t prevRadius = 255;

    switch (pingPhase) {
        case 0: { // EXPAND: ring grows from center off screen
            float progress = (float)elapsed / (float)PING_EXPAND_MS;
            if (progress > 1.0f) progress = 1.0f;
            pingRadius = (uint8_t)(progress * PING_MAX_RADIUS);

            // Hit detection: recharge all aircraft when ring crosses their distance
            // Avoid sqrtf: compare d² against (r±3)²
            int rMin = pingRadius - 3;
            int rMax = pingRadius + 3;
            if (rMin < 0) rMin = 0;
            long rMin2 = (long)rMin * rMin;
            long rMax2 = (long)rMax * rMax;
            for (auto& [icao, lp] : lastPositions) {
                long vx = lp.x - cx;
                long vy = cy - lp.y;
                long d2 = vx * vx + vy * vy;
                if (d2 >= rMin2 && d2 <= rMax2) {
                    // RSSI-based brightness
                    auto it = trackedAircraft.find(icao);
                    if (it != trackedAircraft.end()) {
                        float rssi = it->second.rssi;
                        if (rssi != 0.0f) {
                            float normalized = constrain((rssi + 95.0f) / 25.0f, 0.0f, 1.0f);
                            lp.brightness = (uint8_t)(BRIGHTNESS_MAX * (0.5f + normalized * 0.5f));
                        } else {
                            lp.brightness = BRIGHTNESS_MAX;
                        }
                    } else {
                        lp.brightness = BRIGHTNESS_MAX / 2;
                    }
                    lp.visible = true;
                    lp.decayAccum = 0.0f;
                }
            }

            if (elapsed >= PING_EXPAND_MS) {
                pingPhase = 1;
                pingLastTime = now;
            }
            break;
        }
        case 1: { // PAUSE: blank grid, waiting for next ping
            if (elapsed >= PING_PAUSE_MS) {
                pingPhase = 0;
                pingRadius = 0;
                pingLastTime = now;
            }
            break;
        }
    }

    // ── Draw: erase previous ring, redraw affected grid, then draw current ring ──
    // Batch all drawing in startWrite/endWrite to reduce SPI overhead
    if (prevRadius > 0 && prevRadius <= PING_MAX_RADIUS + 2) {
        tft.startWrite();
        // Erase previous ring (wider band to prevent artifacts)
        for (int i = 0; i <= 4; i++) {
            tft.drawCircle(cx, cy, prevRadius - i, CLR_BG);
            tft.drawCircle(cx, cy, prevRadius + i, CLR_BG);
        }
        // Only redraw grid elements that the ring actually crossed
        // Check if previous ring radius was near a grid circle
        uint16_t ringClr = PalRing(useAmber);
        uint16_t crosshairClr = PalCrosshair(useAmber);
        uint16_t ringBrightClr = PalRingBright(useAmber);
        for (int gridR : {RING_OUTER_PX, RING_MID_PX, RING_INNER_PX}) {
            if (abs(prevRadius - gridR) <= 6) {
                tft.drawCircle(cx, cy, gridR, ringClr);
            }
        }
        // Redraw crosshairs if ring crossed them
        if (prevRadius <= RING_OUTER_PX + 6) {
            tft.drawFastHLine(1, cy, 238, crosshairClr);
            tft.drawFastVLine(cx, 1, 238, crosshairClr);
        }
        // Redraw tick marks if ring crossed them
        for (int i = 0; i < 12; i++) {
            if (i == 0 || i == 3 || i == 6 || i == 9) continue;
            if (abs(prevRadius - 110) <= 6) {
                float dx = TICK_DIRS[i * 2], dy = TICK_DIRS[i * 2 + 1];
                tft.drawLine(cx + (int)(dx * 106), cy + (int)(dy * 106),
                             cx + (int)(dx * 114), cy + (int)(dy * 114), ringClr);
            }
        }
        // Redraw north tick if ring crossed it
        if (abs(prevRadius - 110) <= 6) {
            tft.drawLine(cx, cy - 106, cx, cy - 116, ringBrightClr);
        }
        // Redraw labels if ring crossed them
        tft.setTextColor(ringBrightClr);
        tft.setTextSize(1);
        if (abs(prevRadius - RING_OUTER_PX) <= 6) tft.drawString(ringLabelOuter.c_str(), cx + 6, 14, 1);
        if (abs(prevRadius - RING_MID_PX) <= 6) tft.drawString(ringLabelMid.c_str(), cx + 6, 51, 1);
        if (abs(prevRadius - RING_INNER_PX) <= 6) tft.drawString(ringLabelInner.c_str(), cx + 6, 88, 1);
        // Redraw airports if ring crossed them
        for (const auto& ap : airports) {
            int apSx = ap.sx;
            int apSy = ap.sy;
            if (apSx <= 0 || apSx >= 239 || apSy <= 0 || apSy >= 239) continue;
            long dxa = apSx - cx, dya = apSy - cy;
            long d2a = dxa * dxa + dya * dya;
            if (abs((int)sqrt((double)d2a) - prevRadius) <= 6) {
                tft.drawPixel(apSx, apSy, 0xFFFF);
                tft.drawPixel(apSx - 1, apSy - 1, 0xFFFF);
                tft.drawPixel(apSx + 1, apSy - 1, 0xFFFF);
                tft.drawPixel(apSx - 2, apSy - 2, 0xFFFF);
                tft.drawPixel(apSx + 2, apSy - 2, 0xFFFF);
                tft.drawPixel(apSx, apSy - 3, 0xFFFF);
                tft.drawPixel(apSx - 1, apSy + 1, 0xFFFF);
                tft.drawPixel(apSx + 1, apSy + 1, 0xFFFF);
                tft.drawPixel(apSx, apSy + 2, 0xFFFF);
            }
        }
        tft.endWrite();
    }

    // ── Draw single ring at current radius (3px thick, no persistence) ──
    if (pingPhase == 0 && pingRadius > 0) {
        uint16_t ringColor = PalScan(useAmber);
        tft.drawCircle(cx, cy, pingRadius, ringColor);
        tft.drawCircle(cx, cy, pingRadius + 1, ringColor);
        tft.drawCircle(cx, cy, pingRadius + 2, ringColor);
        prevRadius = pingRadius;
    } else {
        prevRadius = 255;
    }

    // ── Alert update + draw ──
    // Restore static HUD/cardinals after ping erasure and ring drawing.
    // Keep the ping underneath the HUD, matching angular mode layering.
    DrawRadarLabels();
    DrawAirportMarkers();
    UpdateAlertState(alertSquawk);
    DrawAllAircraft(alertSquawk);
    DrawAlertText(alertSquawk);
}

// ── Static grid ──
void AircraftManager::DrawRadarGrid() const
{
    const int cx = 120, cy = 120;
    uint16_t ringClr = PalRing(useAmber);
    uint16_t crosshairClr = PalCrosshair(useAmber);

    tft.drawCircle(cx, cy, RING_OUTER_PX, ringClr);
    tft.drawCircle(cx, cy, RING_MID_PX,   ringClr);
    tft.drawCircle(cx, cy, RING_INNER_PX, ringClr);

    tft.drawFastHLine(1, cy, 238, crosshairClr);
    tft.drawFastVLine(cx, 1, 238, crosshairClr);

    for (int i = 0; i < 12; i++) {
        if (i == 0 || i == 3 || i == 6 || i == 9) continue;
        float dx = TICK_DIRS[i * 2], dy = TICK_DIRS[i * 2 + 1];
        tft.drawLine(cx + (int)(dx * 106), cy + (int)(dy * 106),
                     cx + (int)(dx * 114), cy + (int)(dy * 114), ringClr);
    }

    tft.drawLine(cx, cy - 106, cx, cy - 116, PalRingBright(useAmber));
}

#if defined(ARDUINO_ARCH_ESP8266)
void AircraftManager::Draw(LGFX& /*buf*/)
{
}
#endif

// ── Fade color toward black ──
// fadeScale[level] = (level * 256 + BRIGHTNESS_MAX/2) / BRIGHTNESS_MAX  (fixed-point ×256)
// result = (component * fadeScale[level]) >> 8  — one multiply, one shift
static const uint16_t fadeScale[65] = {
    0, 4, 8, 12, 16, 20, 25, 29, 33, 37, 41, 45, 49, 53, 57, 61, 65, 70, 74, 78, 82, 86, 90, 94, 98, 102, 106, 110, 114, 118, 122, 126, 130, 135, 139, 143, 147, 151, 155, 159, 163, 167, 171, 175, 179, 183, 187, 191, 195, 199, 203, 207, 212, 216, 220, 224, 228, 232, 236, 240, 244, 248, 252, 256
};

static inline uint16_t FadeColor(uint16_t base, uint8_t level)
{
    if (level <= 0) return CLR_BG;
    if (level >= BRIGHTNESS_MAX) return base;
    uint16_t s = fadeScale[level];
    uint16_t r5 = (base >> 11) & 0x1F;
    uint16_t g6 = (base >> 5) & 0x3F;
    uint16_t b5 = base & 0x1F;
    uint16_t fr = (r5 * s) >> 8;
    uint16_t fg = (g6 * s) >> 8;
    uint16_t fb = (b5 * s) >> 8;
    return (fr << 11) | (fg << 5) | fb;
}

void AircraftManager::ErasePosition(int x, int y, uint8_t radius) const
{
    tft.fillCircle(x, y, radius, CLR_BG);

    const int cx = 120, cy = 120;
    uint16_t ringClr = PalRing(useAmber);
    uint16_t crosshairClr = PalCrosshair(useAmber);
    tft.drawCircle(cx, cy, RING_OUTER_PX, ringClr);
    tft.drawCircle(cx, cy, RING_MID_PX,   ringClr);
    tft.drawCircle(cx, cy, RING_INNER_PX, ringClr);
    tft.drawFastHLine(1, cy, 238, crosshairClr);
    tft.drawFastVLine(cx, 1, 238, crosshairClr);
}

// ── Draw aircraft blip ──
void AircraftManager::DrawAircraftBlip(int x, int y, const SimpleAircraft& ac, uint8_t brightness) const
{
    DrawAircraftBlip(x, y, ac, brightness, 0);
}

void AircraftManager::DrawAircraftBlip(int x, int y, const SimpleAircraft& ac, uint8_t brightness, uint16_t overrideColor) const
{
    AircraftType type = GetAircraftType(ac);
    TargetGlyph glyph = GetTargetGlyph(ac);

    // Brightness maps linearly: BRIGHTNESS_MAX = full bright (scan line level), 0 = black
    // Decay controls fade — no quality gate on drawing
    uint8_t effective = brightness;
    if (effective < 1) effective = 1;
    if (effective > BRIGHTNESS_MAX) effective = BRIGHTNESS_MAX;

    // Use scan line color as the base (full bright), fade toward aircraft color as it decays
    uint16_t baseColor;
    uint16_t glowColor;
    switch (type) {
        case AircraftType::MILITARY:
            baseColor = CLR_MILITARY;
            glowColor = CLR_GLOW_MIL;
            break;
        case AircraftType::COMMERCIAL:
            baseColor = PalScan(useAmber);  // Full bright = scan line color
            glowColor = PalGlow(useAmber);
            break;
        default:
            baseColor = CLR_UNKNOWN;
            glowColor = CLR_UNKNOWN;
            break;
    }

    uint16_t color;
    if (overrideColor) {
        color = overrideColor;
    } else {
        color = FadeColor(baseColor, effective);
    }

    float hRad = ac.heading * 0.0174533f;

    // ── Trail (dashed line through position history) ──
    // Draw BEFORE glow and icon so they sit underneath
    if (displayTrailDots) {
        auto histIt = trailHistories.find(ac.icao);
        if (histIt != trailHistories.end()) {
            const auto& hist = histIt->second;
            if (hist.count >= 2) {
                uint32_t now = millis();
                // Build list of valid trail points (not expired)
                struct TrailPt { int x, y; uint8_t bright; };
                TrailPt pts[TRAIL_WAYPOINTS_MAX];
                int pCount = 0;
                for (int n = 0; n < hist.count; n++) {
                    int idx = ((hist.head - hist.count + n) % TRAIL_WAYPOINTS_MAX + TRAIL_WAYPOINTS_MAX) % TRAIL_WAYPOINTS_MAX;
                    const auto& tp = hist.points[idx];
                    float ageSec = (float)(now - tp.timestamp) / 1000.0f;
                    float fade = 1.0f - (ageSec / 600.0f);
                    if (fade <= 0.0f) continue;
                    uint8_t trailBright = (uint8_t)(effective * fade * 0.6f);
                    if (trailBright < 1) continue;
                    pts[pCount++] = {tp.x, tp.y, trailBright};
                }
                // Draw dashed line: draw every other segment
                for (int i = 0; i < pCount - 1; i += 2) {
                    uint16_t dc;
                    if (overrideColor) {
                        dc = FadeColor(overrideColor, pts[i].bright);
                    } else {
                        dc = FadeColor(baseColor, pts[i].bright);
                    }
                    tft.drawLine(pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y, dc);
                }
            }
        }
    }

    // Phosphor glow (drawn after trail dots, before icon)
    if (effective > BRIGHTNESS_MAX * 0.2f) {
        uint16_t glowFaded = FadeColor(glowColor, (uint8_t)(effective * 0.5f));
        tft.fillCircle(x, y, 3, glowFaded);
    }

    // Class glyphs
    switch (glyph) {
        case TargetGlyph::HELICOPTER: {
            const int rr = 5;
            tft.drawCircle(x, y, rr, color);
            tft.drawLine(x - rr + 1, y - rr + 1, x + rr - 1, y + rr - 1, color);
            tft.drawLine(x - rr + 1, y + rr - 1, x + rr - 1, y - rr + 1, color);
            break;
        }
        case TargetGlyph::HEAVY: {
            const int size = 6;
            tft.fillTriangle(x, y - size, x - size, y + size, x + size, y + size, color);
            break;
        }
        case TargetGlyph::FIXED_WING:
        default: {
            const int size = 5;
            tft.fillTriangle(x, y - size, x - size, y + size, x + size, y + size, color);
            break;
        }
    }

    // Heading cue for helicopters only
    if (glyph == TargetGlyph::HELICOPTER) {
        int tx = x + (int)(sin(hRad) * 10);
        int ty = y - (int)(cos(hRad) * 10);
        tft.drawLine(x, y, tx, ty, FadeColor(baseColor, (uint8_t)(effective * 0.85f)));
    }
}

std::pair<int, int> AircraftManager::ProjectCoordinateToScreen(float lat2, float lon2) const
{
    if (rad <= 0.001f) return {999, 999};

    constexpr double DEG2RAD_F64 = 0.017453292519943295;
    constexpr double RAD2DEG_F64 = 57.29577951308232;

    const double lat1r = (double)lat * DEG2RAD_F64;
    const double lon1r = (double)lon * DEG2RAD_F64;
    const double lat2r = (double)lat2 * DEG2RAD_F64;
    const double lon2r = (double)lon2 * DEG2RAD_F64;

    const double dlat = lat2r - lat1r;
    const double dlon = lon2r - lon1r;

    const double sinHLat = sin(dlat * 0.5);
    const double sinHLon = sin(dlon * 0.5);
    double a = sinHLat * sinHLat + cos(lat1r) * cos(lat2r) * sinHLon * sinHLon;
    if (a < 0.0) a = 0.0;
    if (a > 1.0) a = 1.0;
    const double central = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));

    const double distDeg = central * RAD2DEG_F64;
    const float screenDist = (float)((distDeg / (double)rad) * (double)RING_OUTER_PX);
    if (screenDist <= 1e-6f) return {120, 120};

    const double y = sin(dlon) * cos(lat2r);
    const double x = cos(lat1r) * sin(lat2r) - sin(lat1r) * cos(lat2r) * cos(dlon);
    const double brg = atan2(y, x);

    int sx = 120 + (int)(screenDist * sin(brg));
    int sy = 120 - (int)(screenDist * cos(brg));
    return {sx, sy};
}

bool AircraftManager::FetchLocal()
{
    String host = cfgReadsbHost;
    if (host.isEmpty()) {
        static int warnCount = 0;
        if (++warnCount <= 3) Serial.println("[FETCH] No readsb host configured");
        return false;
    }

    String port = cfgReadsbPort;
    if (port.isEmpty()) port = "8080";

    String path = cfgReadsbPath;
    if (path.isEmpty()) path = "/data/aircraft.json";

    String url = "http://" + host + ":" + port + path;
    Serial.printf("[FETCH] GET %s\n", url.c_str());

    HttpResult result = http.Get(url);
    if (!result.success) {
        GridLog("[FETCH] FAILED");
        return false;
    }
    Serial.printf("[FETCH] Got %d bytes\n", result.response.length());
    if (result.response.length() == 0) {
        GridLog("[FETCH] Empty response");
        return false;
    }
    if (result.response.length() > MAX_RESP_BYTES) {
        Serial.printf("[FETCH] Response %d bytes > %d cap, discarding\n",
                       result.response.length(), MAX_RESP_BYTES);
        return false;
    }

    // Use shared document to avoid multiple 8KB BSS allocations
    jsonDoc.clear();
    DeserializationError err = deserializeJson(jsonDoc, result.response);
    if (err) {
        GridLog("[FETCH] JSON parse error");
        jsonDoc.clear();
        return false;
    }

    auto arr = jsonDoc["aircraft"];
    if (!arr.is<JsonArray>()) {
        Serial.println("[FETCH] No 'aircraft' array in JSON");
        jsonDoc.clear();
        return false;
    }

    std::vector<std::pair<double, SimpleAircraft>> candidates;
    candidates.reserve(arr.size());

    int droppedNoPos = 0;
    int droppedStale = 0;
    for (size_t i = 0; i < arr.size(); i++) {
        auto item = arr[i];
        const char* hexVal = item["hex"];
        if (!hexVal) continue;
        String icao(hexVal);
        if (icao.isEmpty()) continue;

        double latVal = item["lat"] | 0.0;
        double lonVal = item["lon"] | 0.0;
        if (latVal == 0.0 && lonVal == 0.0) {
            droppedNoPos++;
            continue;
        }

        double dLat = latVal - (double)lat;
        double dLon = (lonVal - (double)lon) * cos((double)lat * 0.0174533);
        double distDegApprox = sqrt(dLat * dLat + dLon * dLon);
        if (rad > 0.001f && distDegApprox > (double)rad) continue;

        SimpleAircraft ac;
        ac.icao      = icao;
        ac.lat       = latVal;
        ac.lon       = lonVal;
        ac.altitude  = item["alt_baro"] | 0.0;
        ac.heading   = item["track"] | 0.0;
        if (isnan(ac.heading)) ac.heading = 0.0;
        ac.groundspeed = item["gs"] | 0.0;
        if (isnan(ac.groundspeed) || ac.groundspeed < 0.0f) ac.groundspeed = 0.0f;
        ac.seen = item["seen"] | 0.0;
        if (isnan(ac.seen) || ac.seen < 0.0f) ac.seen = 0.0f;
        ac.seenPos = item["seen_pos"] | ac.seen;
        if (isnan(ac.seenPos) || ac.seenPos < 0.0f) ac.seenPos = ac.seen;
        const char* cat = item["category"];
        ac.category = cat ? cat : "";
        const char* sq = item["squawk"];
        ac.squawk    = sq ? sq : "";
        ac.rssi      = item["rssi"] | 0.0f;

        // Drop stale aircraft (seen_pos > 30s at source)
        if (ac.seenPos > 30.0f) {
            droppedStale++;
            continue;
        }

        candidates.push_back({distDegApprox, ac});
    }

    // Clear JSON doc before further allocations — free heap nodes
    jsonDoc.clear();

    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::map<String, SimpleAircraft> next;
    for (size_t i = 0; i < candidates.size() && i < MAX_AIRCRAFT; i++) {
        const auto& ac = candidates[i].second;
        next[ac.icao] = ac;
    }

    jsonDoc.clear();

    // Merge: update existing aircraft with new data, add new ones
    // Remove aircraft that left the feed, stamp vanishedMs on their draw positions
    uint32_t now = millis();
    std::vector<String> removed;
    for (auto& [icao, ac] : trackedAircraft) {
        if (!next.count(icao)) {
            removed.push_back(icao);
        }
    }
    for (auto& icao : removed) {
        auto lpIt = lastPositions.find(icao);
        if (lpIt != lastPositions.end() && lpIt->second.vanishedMs == 0) {
            lpIt->second.vanishedMs = now;
        }
        trackedAircraft.erase(icao);
    }
    for (auto& [icao, ac] : next) {
        trackedAircraft[icao] = ac;
        auto lpIt = lastPositions.find(icao);
        if (lpIt != lastPositions.end()) {
            lpIt->second.vanishedMs = 0;  // Still in feed
        }
    }

    GridLog("[FETCH] OK");
    return true;
}

// ── Fetch from ADSB.lol API (streaming — no buffer cap) ──
bool AircraftManager::FetchAdsblol()
{
#if defined(ARDUINO_ARCH_ESP8266)
    // 60s rate limit — return true (skip) so caller doesn't treat as failure
    uint32_t now = millis();
    if (lastAdsblolFetch > 0 && (now - lastAdsblolFetch) < 60000) {
        Serial.printf("[FETCH] ADSB.lol rate limited (%.0fs until next)\n", (60000.0f - (now - lastAdsblolFetch)) / 1000.0f);
        return true;  // skip — not a failure
    }
    lastAdsblolFetch = now;

    if (lat == 0.0f || lon == 0.0f) {
        static int warnCount = 0;
        if (++warnCount <= 3) Serial.println("[FETCH] ADSB.lol: lat/lon not configured");
        return false;
    }
    if (rad <= 0.001f) {
        static int warnCount = 0;
        if (++warnCount <= 3) Serial.println("[FETCH] ADSB.lol: range not configured");
        return false;
    }

    int rangeNm = (int)(rad * 60.0f + 0.5f);
    if (rangeNm < 1) rangeNm = 1;
    String url = "http://api.adsb.lol/v2/lat/" + String(lat, 6) + "/lon/" + String(lon, 6) + "/dist/" + String(rangeNm);
    Serial.printf("[FETCH] ADSB.lol GET %s\n", url.c_str());

    HttpStreamResult stream = http.StreamGet(url);
    if (!stream.success) {
        Serial.printf("[FETCH] ADSB.lol FAILED: code=%d err=%s\n", stream.statusCode, stream.errorMessage.c_str());
        return false;
    }

    jsonDoc.clear();

    // Filter: keep only "ac" array with essential fields
    // ArduinoJson 7.x: filter["ac"] = true keeps entire array
    // To filter specific fields within array elements, set them on filter["ac"] as an object template
    StaticJsonDocument<256> filter;
    filter["ac"] = true;  // Keep the entire "ac" array

    BlockingReadAdapter adapter(stream.client);
    DeserializationError err = deserializeJson(jsonDoc, adapter, DeserializationOption::Filter(filter));
    stream.client->stop();
    delete stream.client;

    if (err) {
        Serial.printf("[FETCH] ADSB.lol JSON parse error: %s\n", err.c_str());
        jsonDoc.clear();
        return false;
    }
    Serial.printf("[FETCH] ADSB.lol ac count=%u mem=%d\n", jsonDoc["ac"].size(), jsonDoc.memoryUsage());
    auto arr = jsonDoc["ac"];
    if (!arr.is<JsonArray>()) {
        Serial.println("[FETCH] ADSB.lol No 'ac' array in JSON");
        jsonDoc.clear();
        return false;
    }

    // Extract ALL data from JSON before clearing doc to free heap
    std::vector<std::pair<double, SimpleAircraft>> candidates;
    candidates.reserve(arr.size());

    int droppedNoPos = 0;
    int droppedStale = 0;
    for (size_t i = 0; i < arr.size(); i++) {
        auto item = arr[i];
        const char* hexVal = item["hex"];
        if (!hexVal) continue;

        double latVal = item["lat"] | 0.0;
        double lonVal = item["lon"] | 0.0;
        if (latVal == 0.0 && lonVal == 0.0) {
            droppedNoPos++;
            continue;
        }

        double dLat = latVal - (double)lat;
        double dLon = (lonVal - (double)lon) * cos((double)lat * 0.0174533);
        double distDegApprox = sqrt(dLat * dLat + dLon * dLon);
        if (rad > 0.001f && distDegApprox > (double)rad) continue;

        SimpleAircraft ac;
        strncpy(ac.icaoBuf, hexVal, sizeof(ac.icaoBuf) - 1);
        ac.icaoBuf[sizeof(ac.icaoBuf) - 1] = '\0';
        ac.icao = ac.icaoBuf;

        ac.lat       = latVal;
        ac.lon       = lonVal;
        ac.altitude  = item["alt_baro"] | 0.0;
        ac.heading   = item["track"] | 0.0;
        if (isnan(ac.heading)) ac.heading = 0.0;
        ac.groundspeed = item["gs"] | 0.0;
        if (isnan(ac.groundspeed) || ac.groundspeed < 0.0f) ac.groundspeed = 0.0f;
        ac.seen = item["seen"] | 0.0;
        if (isnan(ac.seen) || ac.seen < 0.0f) ac.seen = 0.0f;
        ac.seenPos = item["seen_pos"] | ac.seen;
        if (isnan(ac.seenPos) || ac.seenPos < 0.0f) ac.seenPos = ac.seen;
        const char* cat = item["category"];
        strncpy(ac.categoryBuf, cat ? cat : "", sizeof(ac.categoryBuf) - 1);
        ac.categoryBuf[sizeof(ac.categoryBuf) - 1] = '\0';
        ac.category = ac.categoryBuf;
        const char* sq = item["squawk"];
        strncpy(ac.squawkBuf, sq ? sq : "", sizeof(ac.squawkBuf) - 1);
        ac.squawkBuf[sizeof(ac.squawkBuf) - 1] = '\0';
        ac.squawk = ac.squawkBuf;
        ac.rssi      = item["rssi"] | 0.0f;

        // Drop stale aircraft (seen_pos > 30s at source)
        if (ac.seenPos > 30.0f) {
            droppedStale++;
            continue;
        }

        candidates.push_back({distDegApprox, ac});
    }

    // Clear JSON doc before further allocations — free heap nodes
    jsonDoc.clear();

    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::map<String, SimpleAircraft> next;
    for (size_t i = 0; i < candidates.size() && i < MAX_AIRCRAFT; i++) {
        const auto& ac = candidates[i].second;
        next[ac.icao] = ac;
    }

    jsonDoc.clear();

    // Merge: update existing aircraft with new data, add new ones
    // Remove aircraft that left the feed, stamp vanishedMs on their draw positions
    uint32_t mergeTime = millis();
    std::vector<String> removed;
    for (auto& [icao, ac] : trackedAircraft) {
        if (!next.count(icao)) {
            removed.push_back(icao);
        }
    }
    for (auto& icao : removed) {
        auto lpIt = lastPositions.find(icao);
        if (lpIt != lastPositions.end() && lpIt->second.vanishedMs == 0) {
            lpIt->second.vanishedMs = mergeTime;
        }
        trackedAircraft.erase(icao);
    }
    for (auto& [icao, ac] : next) {
        trackedAircraft[icao] = ac;
        auto lpIt = lastPositions.find(icao);
        if (lpIt != lastPositions.end()) {
            lpIt->second.vanishedMs = 0;  // Still in feed
        }
    }

    Serial.printf("[FETCH] ADSB.lol OK tracked=%u\n", trackedAircraft.size());
    GridLog("[FETCH] ADSB.lol OK");
    return true;
#else
    (void)lat; (void)lon; (void)rad;
    Serial.println("[FETCH] ADSB.lol: streaming not available on this platform");
    return false;
#endif
}

// ── Fetch airports from Overpass API (HTTP) ──
void AircraftManager::FetchAirports(int timeout_ms)
{
    (void)timeout_ms;  // Overpass POST uses HttpRequestManager's bounded timeout.
    // 60s rate limit
    uint32_t now = millis();
    if (lastAirportFetch > 0 && (now - lastAirportFetch) < 60000) {
        GridLog("[AIRPORTS] rate limited");
        return;
    }
    if (lat == 0.0f || lon == 0.0f || rad <= 0.001f) {
        // Auto-detect: if ADSB.lol has aircraft, estimate center from first tracked aircraft
        if ((lat == 0.0f || lon == 0.0f) && !trackedAircraft.empty()) {
            float sumLat = 0, sumLon = 0;
            int count = 0;
            for (const auto& [icao, ac] : trackedAircraft) {
                if (ac.lat != 0.0f && ac.lon != 0.0f) {
                    sumLat += ac.lat;
                    sumLon += ac.lon;
                    count++;
                }
            }
            if (count > 0) {
                lat = sumLat / count;
                lon = sumLon / count;
                GridLog("[AIRPORTS] Auto-detected center from aircraft");
            }
        }
        if (lat == 0.0f || lon == 0.0f || rad <= 0.001f) {
            return;
        }
    }

    // Start the cooldown only after coordinates are valid and an actual
    // Overpass request is about to begin. Invalid configuration must not
    // consume the airport fetch slot.
    lastAirportFetch = now;

    // Convert range to meters (Overpass uses meters)
    int rangeM = (int)(rad * 185200.0f);
    if (rangeM < 1000) rangeM = 1000;
    if (rangeM > 500000) rangeM = 500000;

    // Build Overpass query — URL-encoded GET. Include nodes and ways: major
    // airports are commonly mapped as areas, not point nodes. The IATA filter
    // keeps the response small enough for the ESP8266 JSON document.
    // Use overpass-api.de (no redirect, supports chunked transfer).
    // `tags center qt` keeps only tags plus usable coordinates and uses the
    // documented quadtile output order, reducing the ESP8266 response size.
    String encodedQuery = "%5Bout%3Ajson%5D%5Btimeout%3A20%5D%3Bnwr%5B%22aeroway%22%3D%22aerodrome%22%5D%5B%22iata%22%5D%28around%3A" +
                          String(rangeM) + "," + String(lat, 6) + "," + String(lon, 6) +
                          "%29%3Bout+tags+center+qt%3B";
    String body = "data=" + encodedQuery;
    GridLog("[AIRPORTS] Fetching from Overpass API");

    HttpResult result = http.Post("http://overpass-api.de/api/interpreter", body);
    if (!result.success) {
        String msg = "[AIRPORTS] HTTP failed status=";
        msg += result.statusCode;
        msg += " err=";
        msg += result.errorMessage;
        GridLog(msg.c_str());
        return;
    }
    String okMsg = "[AIRPORTS] HTTP OK body=";
    okMsg += result.response.length();
    okMsg += "B";
    GridLog(okMsg.c_str());

    // Parse JSON — shared document (single 8KB BSS allocation)
    jsonDoc.clear();
    DeserializationError err = deserializeJson(jsonDoc, result.response);
    if (err) {
        String msg = "[AIRPORTS] JSON parse error: ";
        msg += err.c_str();
        GridLog(msg.c_str());
        Serial.printf("[AIRPORTS] body=%u prefix=%.80s\n", result.response.length(), result.response.c_str());
        jsonDoc.clear();
        return;
    }

    airports.clear();
    auto elements = jsonDoc["elements"];
    if (!elements.is<JsonArray>()) {
        jsonDoc.clear();
        return;
    }

    for (int i = 0; i < elements.size(); i++) {
        auto item = elements[i];
        float latVal = item["lat"] | 0.0f;
        float lonVal = item["lon"] | 0.0f;
        if (latVal == 0.0f) latVal = item["center"]["lat"] | 0.0f;
        if (lonVal == 0.0f) lonVal = item["center"]["lon"] | 0.0f;
        if (latVal == 0.0f && lonVal == 0.0f) continue;

        airports.push_back(AirportMarker(latVal, lonVal));
    }

    // Cache screen coordinates for all airports
    for (auto& ap : airports) {
        auto proj = ProjectCoordinateToScreen(ap.lat, ap.lon);
        ap.sx = proj.first;
        ap.sy = proj.second;
    }

    jsonDoc.clear();
    airportsFetched = true;
    String loadedMsg = "[AIRPORTS] Loaded ";
    loadedMsg += airports.size();
    loadedMsg += " airports";
    GridLog(loadedMsg.c_str());
}

// ── Draw airport markers on the grid ──
void AircraftManager::DrawAirportMarkers() const
{
    uint16_t color = 0xFFFF;  // White
    for (const auto& ap : airports) {
        int sx = ap.sx;
        int sy = ap.sy;
        if (sx <= 0 || sx >= 239 || sy <= 0 || sy >= 239) continue;
        // Draw Y shape (runway symbol)
        tft.drawLine(sx, sy - 3, sx, sy, color);        // Vertical stem
        tft.drawLine(sx, sy, sx - 2, sy + 3, color);   // Left branch
        tft.drawLine(sx, sy, sx + 2, sy + 3, color);   // Right branch
    }
}

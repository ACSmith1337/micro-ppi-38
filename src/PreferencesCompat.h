#pragma once

#if defined(ARDUINO_ARCH_ESP32)
#include <Preferences.h>

#elif defined(ARDUINO_ARCH_ESP8266)
// ESP8266: LittleFS + JSON config — reliable, no corruption
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <string.h>

#define CONFIG_JSON_PATH "/config.json"

class Preferences {
private:
    const char* _namespace = nullptr;
    bool _opened = false;

    // In-memory cache — avoid re-reading LittleFS on every getString()
    static char _cache[512];
    static bool _cacheValid;

    static bool LoadCache() {
        if (_cacheValid) return true;
        if (!LittleFS.exists(CONFIG_JSON_PATH)) {
            _cacheValid = true; // Mark valid so we don't retry
            _cache[0] = '\0';
            return false;
        }
        File f = LittleFS.open(CONFIG_JSON_PATH, "r");
        if (!f) {
            _cacheValid = true;
            _cache[0] = '\0';
            return false;
        }
        size_t len = f.size();
        if (len >= sizeof(_cache)) len = sizeof(_cache) - 1;
        f.readBytes(_cache, len);
        _cache[len] = '\0';
        f.close();
        _cacheValid = true;
        return true;
    }

public:
    bool begin(const char* ns, bool readOnly = false) {
        (void)readOnly;
        _namespace = ns;
        if (!LittleFS.begin()) {
            Serial.println("[FS] Mount failed — retrying once");
            delay(100);
            if (!LittleFS.begin()) {
                Serial.println("[FS] Critical: LittleFS unavailable. Config will use defaults.");
                _opened = false;
                return false;
            }
        }
        _opened = true;
        _cacheValid = false; // Invalidate cache on new begin()
        return true;
    }

    void end() {
        if (_opened) {
            _opened = false;
            _cacheValid = false;
        }
    }

    String getString(const char* key, const String& def = "") {
        if (!_opened) return def;
        if (!LoadCache()) return def;

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, _cache);
        if (err) return def;

        const char* val = doc[key];
        if (val) return String(val);
        return def;
    }

    void getString(const char* key, char* out, size_t outSize, const char* def = "") {
        if (out == nullptr || outSize == 0) return;
        String value = getString(key, String(def));
        strncpy(out, value.c_str(), outSize - 1);
        out[outSize - 1] = '\0';
    }

    void putString(const char* key, const String& value) {
        if (!_opened) return;

        JsonDocument doc;

        // Load existing config from cache or file
        if (LoadCache()) {
            DeserializationError err = deserializeJson(doc, _cache);
            if (err) {
                Serial.printf("[FS] Cache parse error: %s, starting fresh\n", err.c_str());
            }
        }

        // Set value
        doc[key] = value.c_str();

        // Save atomically: write to temp, then rename
        File f = LittleFS.open(CONFIG_JSON_PATH, "w");
        if (!f) {
            Serial.printf("[FS] Failed to open %s for write\n", CONFIG_JSON_PATH);
            return;
        }

        if (serializeJson(doc, f) == 0) {
            Serial.println("[FS] Failed to serialize config");
        }
        f.close();

        // Update cache
        serializeJson(doc, _cache, sizeof(_cache));
        _cacheValid = true;

        Serial.printf("[FS] Saved %s = '%s'\n", key, value.c_str());
    }
};

// Static member definitions — inline for C++17 ODR compliance
inline char Preferences::_cache[512];
inline bool Preferences::_cacheValid = false;

#endif

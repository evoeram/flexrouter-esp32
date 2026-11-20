#include "config_store.h"
#include "../debug.h"
#include <Preferences.h>

ConfigStore& ConfigStore::instance() {
    static ConfigStore s;
    return s;
}

void ConfigStore::begin() {
    Preferences prefs;
    prefs.begin("arcc", true);
    String cfg = prefs.getString("cfg", "");
    prefs.end();

    bool ok = false;
    if (cfg.length() > 0) {
        DeserializationError err = deserializeJson(_doc, cfg);
        ok = !err;
        if (err)
            DbgPort.printf("[cfg] parse error: %s, fallback to defaults\n", err.c_str());
    }
    if (!ok) {
        deserializeJson(_doc, CONFIG_DEFAULT_JSON);
        save();
    }
}

bool ConfigStore::save() {
    String out;
    serializeJson(_doc, out);
    Preferences prefs;
    prefs.begin("arcc", false);
    prefs.putString("cfg", out);
    prefs.end();
    return true;
}

void ConfigStore::factoryReset() {
    Preferences prefs;
    prefs.begin("arcc", false);
    prefs.clear();
    prefs.end();
    deserializeJson(_doc, CONFIG_DEFAULT_JSON);
}

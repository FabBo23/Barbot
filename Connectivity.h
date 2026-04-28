// =============================================================================
// Connectivity.h – WiFi, Webserver, WebSocket und MQTT für BarBot
// =============================================================================

#ifndef CONNECTIVITY_H
#define CONNECTIVITY_H

#include <WiFi.h>
#include <ESPAsyncWebServer.h>     
#include <AsyncTCP.h>              
#include <DNSServer.h>             
#include <PubSubClient.h> 
#include <ArduinoJson.h>
#include <Preferences.h>
#include <LittleFS.h> 
#include <esp_wifi.h>
#include <Update.h>
#include <WiFiClientSecure.h> // Zwingend für HiveMQ Cloud
#include "DrinkMachine.h"

class Connectivity {
  private:
    AsyncWebServer server;
    AsyncWebSocket  ws;
    DNSServer dnsServer;   
    
    WiFiClient espClientPlain;         // Für lokale, offene Broker (Port 1883)
    WiFiClientSecure espClientSecure;  // Für HiveMQ Cloud (Port 8883)
    PubSubClient client;
    Preferences prefs;
    DrinkMachine* bot;
    
    SemaphoreHandle_t botMutex;
    File fsUploadFile; 
    
    // MQTT Config
    bool mqttEnabled = false;
    String mqttServer = "";
    int mqttPort = 1883;
    String mqttUser = "";
    String mqttPass = "";
    bool mqttTls = false;  // SSL/TLS aktivieren (Port 8883 oder manuell gesetzt)
    
    bool shouldStart = false;
    bool shouldStop = false;
    bool isOffline = false;
    bool inAPMode = false;
    
    struct StateCache {
        bool glasses[12];
        int slots[12];
        bool isBusy;
        int activeSlot;
        int activeDrink;
        char drinkNames[3][33]; 
    } cache;

    StateCache lastSentCache;

    bool lastStableGlassState[12];     
    bool lastReadingGlassState[12];     
    bool lastSentGlassState[12];        
    unsigned long lastDebounceTime[12]; 
    const unsigned long DEBOUNCE_DELAY = 50; 
    
    unsigned long lastMqttReconnectAttempt = 0;
    unsigned long mqttReconnectInterval = 5000; 
    unsigned long mqttWifiReadyTime = 0;
    const unsigned long MQTT_WIFI_SETTLE = 3000;
    unsigned long lastMqttLoop = 0;
    unsigned long lastDebugPrint = 0;
    unsigned long lastCacheUpdate = 0;
    int forceBroadcastCount = 0;

    struct MqttSentState {
        bool glasses[12];
        int  slots[12];
        bool isBusy;
        int activeSlot;
        int activeDrink;
        char drinkNames[3][33];
        bool initialized;
    } mqttSent;

  public:
    // --- Variablen für die Handy-Kopplung ---
    String slot_clients[12]; 
    bool isPairingMode = false;
    int pendingIdentifySlot = -1;

    // Funktion, die gerufen wird, wenn ein Glas abgestellt wurde
    void triggerPairingPending(int slot) {
        if (isPairingMode && mqttEnabled && client.connected()) {
            String msg = "{\"slot\":" + String(slot) + "}";
            client.publish("barbot/pairing/pending", msg.c_str());
            Serial.printf("[MQTT] Warte auf Bestaetigung von Handy fuer Slot %d...\n", slot);
        }
    }

    // Live-Update des MQTT Status auf dem Display (Seite 3)
    void updateMqttDisplay(bool connected) {
        if(bot && xSemaphoreTake(botMutex, pdMS_TO_TICKS(10))) {
            if(connected) {
                bot->sendHasp("p3b7.text_color=#44FF44");
                bot->sendHasp("p3b7.text=\"MQTT: Online\"");
            } else {
                bot->sendHasp("p3b7.text_color=#ef4444");
                bot->sendHasp("p3b7.text=\"MQTT: Offline\"");
            }
            xSemaphoreGive(botMutex);
        }
    }

    Connectivity(DrinkMachine* machineInstance) 
    : server(80), ws("/ws") {
      bot = machineInstance;
      botMutex = xSemaphoreCreateMutex();
      for(int i=0; i<12; i++) {
        cache.glasses[i] = false;     cache.slots[i] = -1;
        lastSentCache.glasses[i] = false; lastSentCache.slots[i] = -99;
        lastStableGlassState[i] = false;
        lastReadingGlassState[i] = false;
        lastSentGlassState[i] = false;
        lastDebounceTime[i] = 0;
      }
      cache.isBusy = false; cache.activeSlot = -1; cache.activeDrink = -1;
      lastSentCache.isBusy = false; lastSentCache.activeSlot = -99; lastSentCache.activeDrink = -99;
      memset(cache.drinkNames, 0, sizeof(cache.drinkNames));
      memset(lastSentCache.drinkNames, 0, sizeof(lastSentCache.drinkNames));
      memset(&mqttSent, 0, sizeof(mqttSent));
      mqttSent.initialized = false;
      mqttSent.activeSlot = -99;
      mqttSent.activeDrink = -99;
      for(int i=0;i<12;i++) mqttSent.slots[i] = -99;
      memset(mqttSent.drinkNames, 0, sizeof(mqttSent.drinkNames));
    }

    void buildStatusJson(char* buf, size_t len) {
        snprintf(buf, len,
          "{\"glasses\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],"
          "\"slots\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],"
          "\"busy\":%s,\"activeSlot\":%d,\"activeDrink\":%d}",
          cache.glasses[0],cache.glasses[1],cache.glasses[2],cache.glasses[3],
          cache.glasses[4],cache.glasses[5],cache.glasses[6],cache.glasses[7],
          cache.glasses[8],cache.glasses[9],cache.glasses[10],cache.glasses[11],
          cache.slots[0],cache.slots[1],cache.slots[2],cache.slots[3],
          cache.slots[4],cache.slots[5],cache.slots[6],cache.slots[7],
          cache.slots[8],cache.slots[9],cache.slots[10],cache.slots[11],
          cache.isBusy?"true":"false", cache.activeSlot, cache.activeDrink);
    }

    void wsBroadcastIfChanged() {
        if(ws.count() == 0) return;

        if(lastSentCache.isBusy && !cache.isBusy) {
            forceBroadcastCount = 8;
        }

        bool changed = (cache.isBusy     != lastSentCache.isBusy    ||
                        cache.activeSlot  != lastSentCache.activeSlot ||
                        cache.activeDrink != lastSentCache.activeDrink);
        if(!changed) for(int i=0;i<12;i++)
            if(cache.glasses[i]!=lastSentCache.glasses[i]||cache.slots[i]!=lastSentCache.slots[i])
                { changed=true; break; }

        if(!changed && forceBroadcastCount > 0) {
            changed = true;
            forceBroadcastCount--;
        }

        if(!changed) return;
        char json[512];
        buildStatusJson(json, sizeof(json));
        ws.textAll(json);
        lastSentCache = cache;
    }

    void resetWifiSettings() {
        Serial.println("[WIFI] Reset angefordert – alle Zugangsdaten werden geloescht");
        if(xSemaphoreTake(botMutex, pdMS_TO_TICKS(100))) {
            if(bot) {
                bot->leds.fill(bot->leds.Color(255, 0, 0)); 
                bot->leds.show();
            }
            xSemaphoreGive(botMutex);
        }
        prefs.clear(); 
        WiFi.disconnect(true);
        delay(1000);
        ESP.restart();
    }

    void setup() {
      WiFi.mode(WIFI_STA);
      esp_wifi_set_ps(WIFI_PS_NONE);
      WiFi.setAutoReconnect(true);   
      
      pinMode(PIN_BUTTON, INPUT_PULLUP);
      
      // Fehler behoben: Beide Clients separat konfigurieren
      espClientPlain.setTimeout(2);
      espClientSecure.setTimeout(2);
      client.setBufferSize(1024);
      
      if(!LittleFS.begin(true)) Serial.println("[FS] Mount Failed");
      else Serial.println("[FS] Mounted");

      prefs.begin("barbot_cfg", false); 
      
      if(digitalRead(PIN_BUTTON) == LOW) {
        resetWifiSettings();
      }

      // Konfiguration aus NVS laden
      mqttEnabled = prefs.getBool("mq_en",   false);
      mqttServer  = prefs.getString("mq_srv", "");
      mqttPort    = prefs.getInt("mq_prt",   1883);
      mqttUser    = prefs.getString("mq_usr", "");
      mqttPass    = prefs.getString("mq_pwd", "");
      mqttTls     = prefs.getBool("mq_tls",  false);

      String ssid = prefs.getString("w_ssid", "");
      String pass = prefs.getString("w_pass", "");
      
      bool connected = false;
      
      if(ssid.length() > 0) {
        Serial.print("Connecting to: "); Serial.println(ssid);
        WiFi.begin(ssid.c_str(), pass.c_str());
        
        unsigned long startAttempt = millis();
        while(millis() - startAttempt < 10000) {
          if(WiFi.status() == WL_CONNECTED) {
            connected = true;
            break;
          }
          delay(500);
          Serial.print(".");
        }
        Serial.println();
      }

      if(connected) {
        Serial.println("[WIFI] Connected!");
        Serial.print("[WIFI] IP: "); Serial.println(WiFi.localIP());

        if(bot) {
          bot->localIP = WiFi.localIP().toString();
          bot->sendHasp("p3b5.text=\"Bot: " + WiFi.localIP().toString() + "\"");
          if(bot->wifiSyncEnabled) {
            bot->sendHasp("ssid " + ssid);
            bot->sendHasp("pass " + pass);
            Serial.println("[CYD] WLAN-Daten ans Display gesendet");
            bot->sendHasp("page 1");
          }
        }

        inAPMode = false;
        setupAppRoutes(); 
        
        DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
        
        server.begin();
        Serial.println("[WEB] Async Server started (App Mode)");
        
        if(mqttEnabled && mqttServer.length() > 0) {
            if (mqttTls) {
                espClientSecure.setInsecure();  // Akzeptiert jedes Zertifikat (HiveMQ Cloud etc.)
                client.setClient(espClientSecure);
            } else {
                client.setClient(espClientPlain);
            }
            client.setServer(mqttServer.c_str(), mqttPort);
            client.setCallback([this](char* topic, byte* payload, unsigned int length) {
               this->mqttCallback(topic, payload, length);
            });
            Serial.println("[MQTT] Aktiviert und konfiguriert");
        } else {
            Serial.println("[MQTT] Disabled or not configured");
        }
      } else {
        Serial.println("[WIFI] Failed -> Starting AP");
        startAPMode();
      }
    }

    void startAPMode() {
      inAPMode = true;
      WiFi.disconnect();
      delay(100);
      WiFi.mode(WIFI_AP_STA);
      WiFi.softAP("Barbot", "barbot123");
      delay(200);
      Serial.print("[AP] IP: "); Serial.println(WiFi.softAPIP());
      dnsServer.start(53, "*", WiFi.softAPIP());
      setupPortalRoutes();   
      setupAppRoutes();      
      server.begin();
      Serial.println("[WEB] AP gestartet");
      WiFi.scanNetworks(true);
      if(bot) {
          bot->sendHasp("p3b5.text=\"Bot (AP): " + WiFi.softAPIP().toString() + "\"");
          bot->sendHasp("ssid Barbot");
          bot->sendHasp("pass barbot123");
          delay(1000);
          bot->sendHasp("reboot");
          Serial.println("[CYD] AP-WLAN-Daten + Neustart ans Display gesendet");
          delay(8000);
          bot->sendHasp("page 1");
      }
    }

    void loop() {
      delay(1);

      if(inAPMode) {
          dnsServer.processNextRequest();
      }

      if (millis() - lastCacheUpdate > 250) {
          lastCacheUpdate = millis();
          if(bot && xSemaphoreTake(botMutex, pdMS_TO_TICKS(50))) {
              for(int i=0; i<12; i++) {
                  cache.glasses[i] = bot->isGlassPresent(i);
                  cache.slots[i] = bot->slotConfig[i];
              }
              cache.isBusy = bot->isBusy;
              cache.activeSlot = bot->activeSlot;
              cache.activeDrink = bot->activeDrink;
              strncpy(cache.drinkNames[0], bot->drinkNames[0].c_str(), 32); cache.drinkNames[0][32] = 0;
              strncpy(cache.drinkNames[1], bot->drinkNames[1].c_str(), 32); cache.drinkNames[1][32] = 0;
              strncpy(cache.drinkNames[2], bot->drinkNames[2].c_str(), 32); cache.drinkNames[2][32] = 0;
              xSemaphoreGive(botMutex);
          }
      }

      if(!inAPMode) {
          if (WiFi.status() != WL_CONNECTED) {
              if(!isOffline) {
                  Serial.println("[WIFI] Connection lost... AutoReconnect active");
                  isOffline = true;
                  updateMqttDisplay(false);
                  mqttWifiReadyTime = 0;
                  if(client.connected()) client.disconnect();
                  espClientPlain.stop();
                  espClientSecure.stop();
              }
          } else {
              if(isOffline) {
                  Serial.println("[WIFI] Back Online!");
                  isOffline = false;
                  mqttWifiReadyTime = millis();
                  lastMqttReconnectAttempt = millis();
              }
          }

          if(mqttEnabled && mqttServer.length() > 0 && WiFi.status() == WL_CONNECTED) {
              unsigned long now = millis();
              if(mqttWifiReadyTime > 0 && (now - mqttWifiReadyTime) < MQTT_WIFI_SETTLE) {
                  // Wartezeit nach Reconnect
              } else if (!client.connected()) {
                  if (now - lastMqttReconnectAttempt > mqttReconnectInterval) {
                      updateMqttDisplay(false);
                      lastMqttReconnectAttempt = now;
                      if (attemptMqttConnect()) {
                          mqttReconnectInterval = 5000;
                          mqttSent.initialized = false;
                          Serial.println("[MQTT] Connected");
                      } else {
                          if(mqttReconnectInterval < 60000) mqttReconnectInterval *= 2;
                          Serial.printf("[MQTT] Connect failed, retry in %lus\n", mqttReconnectInterval/1000);
                      }
                  }
              } else {
                  mqttWifiReadyTime = 0;
                  client.loop();
                  mqttPublishIfChanged();
              }
          }
      }

      if(xSemaphoreTake(botMutex, pdMS_TO_TICKS(10))) {
          wsBroadcastIfChanged();
          xSemaphoreGive(botMutex);
      }
      ws.cleanupClients();

      if (millis() - lastDebugPrint > 5000) {
        lastDebugPrint = millis();
        Serial.print("[SYS] Heap: "); Serial.print(ESP.getFreeHeap());
        Serial.print(" | Modus: "); Serial.print(inAPMode ? "AP" : (WiFi.status() == WL_CONNECTED ? "STA OK" : "STA Lost"));
        Serial.print(" | MQTT: "); Serial.println(mqttEnabled ? (client.connected() ? "CONN" : "DISC") : "OFF");
      }
    }

    void setupPortalRoutes() {
        server.on("/generate_204",              HTTP_GET, [](AsyncWebServerRequest *r){ r->redirect("http://192.168.4.1/"); });
        server.on("/gen_204",                   HTTP_GET, [](AsyncWebServerRequest *r){ r->redirect("http://192.168.4.1/"); });
        server.on("/hotspot-detect.html",       HTTP_GET, [](AsyncWebServerRequest *r){ r->redirect("http://192.168.4.1/"); });
        server.on("/library/test/success.html", HTTP_GET, [](AsyncWebServerRequest *r){ r->redirect("http://192.168.4.1/"); });
        server.on("/ncsi.txt",                  HTTP_GET, [](AsyncWebServerRequest *r){ r->send(200,"text/plain","Microsoft NCSI"); });
        server.on("/connecttest.txt",           HTTP_GET, [](AsyncWebServerRequest *r){ r->send(200,"text/plain","Microsoft Connect Test"); });
        server.on("/redirect",                  HTTP_GET, [](AsyncWebServerRequest *r){ r->redirect("http://192.168.4.1/"); });

        server.on("/app", HTTP_GET, [](AsyncWebServerRequest *request){
            request->send(LittleFS, "/index.html", "text/html");
        });

        server.on("/", HTTP_GET, [this](AsyncWebServerRequest *request){
            if(LittleFS.exists("/portal.html")) {
                request->send(LittleFS, "/portal.html", "text/html");
            } else {
                request->send(200, "text/html",
                    "<h2>portal.html fehlt</h2><p>SSID: Barbot, Pass: barbot123<br>192.168.4.1</p>");
            }
        });

        server.on("/save", HTTP_POST, [this](AsyncWebServerRequest *request){
            if(request->hasParam("ssid", true)) {
                String newSsid = request->getParam("ssid", true)->value();
                String newPass = request->getParam("pass", true)->value();

                if(newSsid.length() > 0) {
                    prefs.putString("w_ssid", newSsid);
                    prefs.putString("w_pass", newPass);
                }

                if(request->hasParam("d0", true)) {
                    if(bot) {
                        bot->saveDrinkName(0, request->getParam("d0", true)->value());
                        bot->saveDrinkName(1, request->getParam("d1", true)->value());
                        bot->saveDrinkName(2, request->getParam("d2", true)->value());
                    }
                }

                if(request->hasParam("mq_srv", true)) {
                    String srv = request->getParam("mq_srv", true)->value();
                    prefs.putString("mq_srv", srv);
                    prefs.putBool("mq_en", srv.length() > 0);
                    if(request->hasParam("mq_prt", true)) prefs.putInt("mq_prt",    request->getParam("mq_prt", true)->value().toInt());
                    if(request->hasParam("mq_usr", true)) prefs.putString("mq_usr", request->getParam("mq_usr", true)->value());
                    if(request->hasParam("mq_pwd", true)) prefs.putString("mq_pwd", request->getParam("mq_pwd", true)->value());
                    
                    // TLS-Flag speichern (aus dem Portal)
                    bool tls = (request->getParam("mq_tls", true)->value() == "true");
                    prefs.putBool("mq_tls", tls);
                }

                request->send(200, "text/plain", "Gespeichert! BarBot startet neu...");
                delay(1000);
                ESP.restart();
            } else {
                request->send(400, "text/plain", "SSID fehlt!");
            }
        });

        server.on("/api/scan", HTTP_GET, [](AsyncWebServerRequest *request){
            int n = WiFi.scanComplete();
            if(n == WIFI_SCAN_RUNNING) {
                request->send(202, "application/json", "[]");
                return;
            }
            if(n <= 0) {
                WiFi.scanNetworks(true);
                request->send(202, "application/json", "[]");
                return;
            }
            String json = "[";
            for(int i = 0; i < n; i++) {
                if(i > 0) json += ",";
                json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + ",\"enc\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? 1 : 0) + "}";
            }
            json += "]";
            WiFi.scanDelete();
            request->send(200, "application/json", json);
        });

        server.on("/api/config", HTTP_GET, [this](AsyncWebServerRequest *request){
            StaticJsonDocument<512> doc;
            JsonArray drinks = doc.createNestedArray("drinks");
            drinks.add(bot ? bot->drinkNames[0] : "Getraenk 1");
            drinks.add(bot ? bot->drinkNames[1] : "Getraenk 2");
            drinks.add(bot ? bot->drinkNames[2] : "Getraenk 3");
            doc["cydMode"]   = bot ? bot->cydMode        : false;
            doc["wifiSync"]  = bot ? bot->wifiSyncEnabled : true;
            doc["pourTime"]  = bot ? bot->pourTimeMs      : 5000;
            doc["displayIP"] = bot ? bot->displayIP        : "";
            JsonObject mqtt = doc.createNestedObject("mqtt");
            mqtt["enabled"] = this->mqttEnabled;
            mqtt["server"]  = this->mqttServer;
            mqtt["port"]    = this->mqttPort;
            mqtt["user"]    = this->mqttUser;
            mqtt["tls"]     = this->mqttTls;
            String res; serializeJson(doc, res);
            request->send(200, "application/json", res);
        });

        server.onNotFound([](AsyncWebServerRequest *request){
            request->redirect("http://192.168.4.1/");
        });
    }

    void setupAppRoutes() {
      // WebSocket
      ws.onEvent([this](AsyncWebSocket* server, AsyncWebSocketClient* client,
                        AwsEventType type, void* arg, uint8_t* data, size_t len) {
          if(type == WS_EVT_CONNECT) {
              char json[512];
              if(xSemaphoreTake(botMutex, pdMS_TO_TICKS(30))) {
                  buildStatusJson(json, sizeof(json));
                  xSemaphoreGive(botMutex);
                  client->text(json);
              }
          } else if(type == WS_EVT_DISCONNECT) {
              ws.cleanupClients();
          }
      });
      server.addHandler(&ws);

      server.onNotFound([](AsyncWebServerRequest *request) {
        if (request->method() == HTTP_OPTIONS) request->send(200); else request->send(404);
      });

      server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
          if(LittleFS.exists("/index.html")) {
              AsyncWebServerResponse *r = request->beginResponse(LittleFS, "/index.html", "text/html");
              r->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
              r->addHeader("Pragma", "no-cache");
              request->send(r);
          } else {
              String html;
              html += "<!DOCTYPE html><html><head>";
              html += "<meta charset='UTF-8'>";
              html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
              html += "<style>";
              html += "body{font-family:sans-serif;padding:20px;background:#1a1a1a;color:#fff;max-width:480px;margin:0 auto}";
              html += "h1{color:#ff9800}h2{color:#aaa;font-size:1rem;border-top:1px solid #333;padding-top:14px}";
              html += "input[type=file]{width:100%;padding:10px;margin:8px 0;background:#2a2a2a;color:#fff;border:1px solid #444;border-radius:6px;box-sizing:border-box}";
              html += "button{width:100%;padding:12px;background:#ff9800;color:#000;border:none;border-radius:6px;font-size:1rem;font-weight:bold;cursor:pointer;margin-top:6px}";
              html += "button:disabled{background:#555;color:#888}";
              html += "#log{margin-top:16px;padding:12px;background:#111;border-radius:6px;font-size:.85rem;color:#aaa;min-height:60px;white-space:pre-wrap}";
              html += "#bar{width:100%;height:16px;background:#333;border-radius:8px;margin-top:8px;display:none}";
              html += "#fill{height:100%;width:0%;background:#ff9800;border-radius:8px;transition:width 0.2s}";
              html += "</style></head><body>";
              html += "<h1>&#127867; BarBot</h1>";
              html += "<p style='color:#ff4444'>&#9888; Webseiten-Dateien fehlen auf diesem Controller.</p>";
              html += "<p style='color:#aaa'>Lade die Dateien einzeln hoch (index.html, settings.html, filemanager.html usw.)</p>";
              html += "<h2>&#128196; Datei hochladen</h2>";
              html += "<input type='file' id='file' multiple>";
              html += "<button id='btn' onclick='uploadAll()'>Hochladen</button>";
              html += "<div id='bar'><div id='fill'></div></div>";
              html += "<div id='log'>Bereit.</div>";
              html += "<h2>&#9881; Firmware Update</h2>";
              html += "<a href='/ota' style='display:block;text-align:center;padding:12px;background:#333;color:#fff;border-radius:6px;text-decoration:none'>Firmware OTA &rarr;</a>";
              html += "<script>";
              html += "async function uploadAll(){";
              html += "  const files=[...document.getElementById('file').files];";
              html += "  if(!files.length){alert('Keine Datei ausgewaehlt');return;}";
              html += "  const btn=document.getElementById('btn');";
              html += "  const log=document.getElementById('log');";
              html += "  const bar=document.getElementById('bar');";
              html += "  const fill=document.getElementById('fill');";
              html += "  btn.disabled=true; bar.style.display='block'; log.textContent='';";
              html += "  for(let i=0;i<files.length;i++){";
              html += "    const f=files[i];";
              html += "    log.textContent+='Lade hoch: '+f.name+'...\\n';";
              html += "    fill.style.width=Math.round(i/files.length*100)+'%';";
              html += "    const fd=new FormData();";
              html += "    fd.append('file',f,f.name);";
              html += "    try{";
              html += "      const r=await fetch('/api/uploadAny',{method:'POST',body:fd});";
              html += "      log.textContent+=(r.ok?'  OK':'  FEHLER: '+r.status)+'\\n';";
              html += "    }catch(e){log.textContent+='  Verbindungsfehler\\n';}";
              html += "  }";
              html += "  fill.style.width='100%';";
              html += "  log.textContent+='\\nFertig! Seite wird neu geladen...';";
              html += "  setTimeout(()=>location.reload(),2000);";
              html += "}";
              html += "</script></body></html>";
              request->send(200, "text/html", html);
          }
      });
      
      server.on("/display", HTTP_GET, [](AsyncWebServerRequest *request){
          request->send(LittleFS, "/display_upload.html", "text/html");
      });

      server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *request){
          if(LittleFS.exists("/settings.html"))
              request->send(LittleFS, "/settings.html", "text/html");
          else
              request->redirect("/");
      });

      server.on("/files", HTTP_GET, [](AsyncWebServerRequest *request){
          if(LittleFS.exists("/filemanager.html"))
              request->send(LittleFS, "/filemanager.html", "text/html");
          else
              request->redirect("/");
      });
      server.on("/display_upload", HTTP_GET, [](AsyncWebServerRequest *request){
          if(LittleFS.exists("/display_upload.html"))
              request->send(LittleFS, "/display_upload.html", "text/html");
          else
              request->redirect("/");
      });
      
      auto imageHandler = [](AsyncWebServerRequest *request, String path){
          if(LittleFS.exists(path)) {
              AsyncWebServerResponse *response = request->beginResponse(LittleFS, path, "image/jpeg");
              response->addHeader("Cache-Control", "public, max-age=31536000");
              request->send(response);
          } else {
              request->send(404);
          }
      };

      server.on("/bg.jpg",     HTTP_GET, [imageHandler](AsyncWebServerRequest *r){ imageHandler(r, "/bg.jpg"); });
      server.on("/drink_0.jpg",HTTP_GET, [imageHandler](AsyncWebServerRequest *r){ imageHandler(r, "/drink_0.jpg"); });
      server.on("/drink_1.jpg",HTTP_GET, [imageHandler](AsyncWebServerRequest *r){ imageHandler(r, "/drink_1.jpg"); });
      server.on("/drink_2.jpg",HTTP_GET, [imageHandler](AsyncWebServerRequest *r){ imageHandler(r, "/drink_2.jpg"); });
      server.on("/drink_0.bin",HTTP_GET, [imageHandler](AsyncWebServerRequest *r){ imageHandler(r, "/drink_0.bin"); });
      server.on("/drink_1.bin",HTTP_GET, [imageHandler](AsyncWebServerRequest *r){ imageHandler(r, "/drink_1.bin"); });
      server.on("/drink_2.bin",HTTP_GET, [imageHandler](AsyncWebServerRequest *r){ imageHandler(r, "/drink_2.bin"); });

      server.on("/api/upload", HTTP_POST, [](AsyncWebServerRequest *request){
        request->send(200, "text/plain", "OK");
      }, [this](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){
        handleUpload(request, filename, index, data, len, final);
      });

      server.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest *request){
        char json[512];
        if(xSemaphoreTake(botMutex, pdMS_TO_TICKS(20))) {
            buildStatusJson(json, sizeof(json));
            xSemaphoreGive(botMutex);
        } else {
            strcpy(json, "{\"busy\":true}");
        }
        AsyncWebServerResponse *response = request->beginResponse(200, "application/json", json);
        response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
        request->send(response);
      });

      server.on("/api/config", HTTP_GET, [this](AsyncWebServerRequest *request){
         StaticJsonDocument<512> doc;
         if(xSemaphoreTake(botMutex, pdMS_TO_TICKS(50))) {
             JsonArray drinks = doc.createNestedArray("drinks");
             drinks.add(cache.drinkNames[0]); drinks.add(cache.drinkNames[1]); drinks.add(cache.drinkNames[2]);
             doc["cydMode"]   = bot ? bot->cydMode : false;
             doc["wifiSync"]  = bot ? bot->wifiSyncEnabled : true;
             doc["pourTime"]  = bot ? bot->pourTimeMs : 5000;
             doc["displayIP"] = bot ? bot->displayIP : "";
             xSemaphoreGive(botMutex);
         }
         JsonObject mqtt = doc.createNestedObject("mqtt");
         mqtt["enabled"] = this->mqttEnabled;
         mqtt["server"]  = this->mqttServer;
         mqtt["port"]    = this->mqttPort;
         mqtt["user"]    = this->mqttUser;
         mqtt["tls"]     = this->mqttTls;
         String res; serializeJson(doc, res);
         request->send(200, "application/json", res);
      });

      server.on("/api/saveConfig", HTTP_POST, [](AsyncWebServerRequest *request){
          request->send(200, "text/plain", "OK");
      }, NULL, [this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
          StaticJsonDocument<512> doc;
          deserializeJson(doc, (const char*)data, len);
          
          if(xSemaphoreTake(botMutex, pdMS_TO_TICKS(100))) {
            if(doc.containsKey("d1")) bot->saveDrinkName(0, doc["d1"].as<String>());
            if(doc.containsKey("d2")) bot->saveDrinkName(1, doc["d2"].as<String>());
            if(doc.containsKey("d3")) bot->saveDrinkName(2, doc["d3"].as<String>());
            if(doc.containsKey("cydMode")) {
              bot->cydMode = doc["cydMode"].as<bool>();
              prefs.begin("barbot", false);
              prefs.putBool("cydMode", bot->cydMode);
              prefs.end();
            }
            if(doc.containsKey("wifiSync")) {
              bot->wifiSyncEnabled = doc["wifiSync"].as<bool>();
              prefs.begin("barbot", false);
              prefs.putBool("wifiSync", bot->wifiSyncEnabled);
              prefs.end();
            }
            if(doc.containsKey("pourTime")) {
              bot->savePourTime(doc["pourTime"].as<int>());
            }
            xSemaphoreGive(botMutex);
          }
          
          if(doc.containsKey("mqtt_server")) {
               prefs.begin("barbot_cfg", false);  
               if(doc.containsKey("mqtt_enabled")) {
                 this->mqttEnabled = doc["mqtt_enabled"].as<bool>();
                 prefs.putBool("mq_en", this->mqttEnabled);
               }
               this->mqttServer = doc["mqtt_server"].as<String>();
               prefs.putString("mq_srv", this->mqttServer);
               if(doc.containsKey("mqtt_port")) { this->mqttPort = doc["mqtt_port"].as<int>(); prefs.putInt("mq_prt", this->mqttPort); }
               if(doc.containsKey("mqtt_user")) { this->mqttUser = doc["mqtt_user"].as<String>(); prefs.putString("mq_usr", this->mqttUser); }
               if(doc.containsKey("mqtt_pass")) { this->mqttPass = doc["mqtt_pass"].as<String>(); prefs.putString("mq_pwd", this->mqttPass); }
               if(doc.containsKey("mqtt_tls"))  { this->mqttTls  = doc["mqtt_tls"].as<bool>(); prefs.putBool("mq_tls", this->mqttTls); }
               prefs.end();
               
               if(client.connected()) client.disconnect();
               
               if(this->mqttEnabled && this->mqttServer.length() > 0) {
                   if (this->mqttTls) {
                       espClientSecure.setInsecure();
                       client.setClient(espClientSecure);
                   } else {
                       client.setClient(espClientPlain);
                   }
                   client.setServer(this->mqttServer.c_str(), this->mqttPort);
                   mqttSent.initialized = false;
                   lastMqttReconnectAttempt = 0;
               }
          }
      });

      server.on("/api/resetNames", HTTP_POST, [this](AsyncWebServerRequest *request){
          if(xSemaphoreTake(botMutex, pdMS_TO_TICKS(100))) {
              bot->resetDrinkNames();
              xSemaphoreGive(botMutex);
          }
          LittleFS.remove("/drink_0.jpg"); LittleFS.remove("/drink_1.jpg"); LittleFS.remove("/drink_2.jpg");
          request->send(200, "text/plain", "OK");
      });

      server.on("/api/start", HTTP_GET, [this](AsyncWebServerRequest *request){ 
          if(request->hasParam("id")) {
              int id = request->getParam("id")->value().toInt();
              if(xSemaphoreTake(botMutex, pdMS_TO_TICKS(200))) {
                  bot->activeDrink = id;
                  Serial.printf("[WEB] Start Drink ID: %d\n", id);
                  xSemaphoreGive(botMutex);
              }
          }
          shouldStart = true; 
          request->send(200, "text/plain", "OK"); 
      });

      server.on("/api/stop", HTTP_GET, [this](AsyncWebServerRequest *request){ shouldStop = true; request->send(200, "text/plain", "OK"); });

      // --- File Manager API ---
      server.on("/api/fsinfo", HTTP_GET, [](AsyncWebServerRequest *request){
          String json = "{\"total\":" + String(LittleFS.totalBytes()) + ",\"used\":" + String(LittleFS.usedBytes()) + "}";
          request->send(200, "application/json", json);
      });

      server.on("/api/files", HTTP_GET, [](AsyncWebServerRequest *request){
          String json = "["; bool first = true;
          File root = LittleFS.open("/");
          File f = root.openNextFile();
          while(f) {
              if(!first) json += ",";
              json += "{\"name\":\"" + String(f.name()) + "\",\"size\":" + String(f.size()) + "}";
              first = false; f = root.openNextFile();
          }
          json += "]";
          request->send(200, "application/json", json);
      });

      server.on("/api/deleteFile", HTTP_GET, [](AsyncWebServerRequest *request){
          if(request->hasParam("name")) {
              String name = request->getParam("name")->value();
              if(!name.startsWith("/")) name = "/" + name;
              if(LittleFS.remove(name)) request->send(200, "text/plain", "OK");
              else request->send(404, "text/plain", "Nicht gefunden");
          } else request->send(400, "text/plain", "Kein Name");
      });

      server.on("/api/uploadAny", HTTP_POST, [](AsyncWebServerRequest *request){
          request->send(200, "text/plain", "OK");
      }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){
          static File anyFile;
          if(!index) { if(anyFile) anyFile.close(); anyFile = LittleFS.open("/" + filename, "w"); }
          if(anyFile) anyFile.write(data, len);
          if(final && anyFile) anyFile.close();
      });
      
      server.on("/api/setSlot", HTTP_GET, [this](AsyncWebServerRequest *request){
        if(request->hasParam("id") && request->hasParam("val")) {
          if(xSemaphoreTake(botMutex, pdMS_TO_TICKS(100))) {
              bot->setSlot(request->getParam("id")->value().toInt(), request->getParam("val")->value().toInt());
              xSemaphoreGive(botMutex);
          }
          request->send(200, "text/plain", "OK");
        } else request->send(400, "text/plain", "Bad Args");
      });

      server.on("/api/setAll", HTTP_GET, [this](AsyncWebServerRequest *request){
        if(request->hasParam("val")) {
            int val = request->getParam("val")->value().toInt();
            if(xSemaphoreTake(botMutex, pdMS_TO_TICKS(100))) {
                bot->setAllSlots(val);
                if(val >= 0 && val <= 2) {
                    bot->updateDrinkScreen(bot->drinkNames[val], val);
                    bot->webSelectedDrink = val;
                }
                xSemaphoreGive(botMutex);
            }
            request->send(200, "text/plain", "OK");
        } else request->send(400, "text/plain", "Bad Args");
      });

      // --- Versions-Info ---
      server.on("/api/version", HTTP_GET, [](AsyncWebServerRequest *request){
          String json = "{\"version\":\"" FIRMWARE_VERSION "\","
                        "\"repo\":\"FabBo23/Barbot\"}";
          request->send(200, "application/json", json);
      });

      // --- OTA Dateisystem-Update (LittleFS) ---
      // LittleFS wird vor dem Flash-Vorgang ausgehängt und nach dem Neustart
      // automatisch neu gemountet.
      server.on("/api/ota-fs", HTTP_POST,
        [](AsyncWebServerRequest *request){
            if(Update.hasError()) {
                String err = Update.errorString();
                Serial.printf("[OTA-FS] FEHLER: %s\n", err.c_str());
                request->send(500, "text/plain", err);
            } else {
                Serial.println("[OTA-FS] Erfolgreich – starte neu");
                request->send(200, "text/plain", "OK");
                delay(500);
                ESP.restart();
            }
        },
        [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){
            if(index == 0) {
                Serial.printf("[OTA-FS] Start: %s\n", filename.c_str());
                LittleFS.end();  // Unmount vor dem Flashen zwingend nötig
                if(!Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS)) {
                    Serial.printf("[OTA-FS] begin() Fehler: %s\n", Update.errorString());
                    return;
                }
            }
            if(Update.isRunning()) {
                size_t written = Update.write(data, len);
                if(written != len) {
                    Serial.printf("[OTA-FS] write() Fehler: %s\n", Update.errorString());
                }
            }
            if(final) {
                if(Update.end(true)) {
                    Serial.printf("[OTA-FS] Fertig: %u Bytes\n", index + len);
                } else {
                    Serial.printf("[OTA-FS] end() Fehler: %s\n", Update.errorString());
                }
            }
        }
      );

      // --- OTA Firmware Update ---
      server.on("/ota", HTTP_GET, [](AsyncWebServerRequest *request){
          if(LittleFS.exists("/ota.html"))
              request->send(LittleFS, "/ota.html", "text/html");
          else
              request->send(200, "text/html", "<h2>ota.html fehlt – bitte hochladen</h2><a href='/files'>Datei-Manager</a>");
      });

      server.on("/api/ota", HTTP_POST,
        [](AsyncWebServerRequest *request){
            if(Update.hasError()) {
                String err = Update.errorString();
                Serial.printf("[OTA] FEHLER: %s\n", err.c_str());
                request->send(500, "text/plain", err);
            } else {
                Serial.println("[OTA] Erfolgreich – starte neu");
                request->send(200, "text/plain", "OK");
                delay(500);
                ESP.restart();
            }
        },
        [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){
            if(index == 0) {
                Serial.printf("[OTA] Start: %s\n", filename.c_str());
                if(!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
                    Serial.printf("[OTA] begin() Fehler: %s\n", Update.errorString());
                    return;
                }
            }
            if(Update.isRunning()) {
                size_t written = Update.write(data, len);
                if(written != len) {
                    Serial.printf("[OTA] write() Fehler nach %u Bytes: %s\n", index, Update.errorString());
                }
            }
            if(final) {
                if(Update.end(true)) {
                    Serial.printf("[OTA] Fertig: %u Bytes\n", index + len);
                } else {
                    Serial.printf("[OTA] end() Fehler: %s\n", Update.errorString());
                }
            }
        }
      );
    }
    
    bool attemptMqttConnect() {
        if(!this->mqttEnabled) return false;

        if(espClientPlain.connected()) espClientPlain.stop();
        if(espClientSecure.connected()) espClientSecure.stop();
        delay(50);

        espClientPlain.setTimeout(2);
        espClientSecure.setTimeout(3);

        String clientId = "BarBot-" + WiFi.macAddress().substring(9);
        clientId.replace(":", "");

        // TLS-Client setzen je nach Konfiguration
        if (this->mqttTls) {
            espClientSecure.setInsecure();  // Akzeptiert jedes Zertifikat (kein CA-Check)
            client.setClient(espClientSecure);
        } else {
            client.setClient(espClientPlain);
        }
        client.setServer(this->mqttServer.c_str(), this->mqttPort);

        bool result = false;
        if(this->mqttUser.length() > 0) {
            result = client.connect(clientId.c_str(), this->mqttUser.c_str(), this->mqttPass.c_str(),
                                    "barbot/status", 0, true, "OFFLINE");
        } else {
            result = client.connect(clientId.c_str(), nullptr, nullptr,
                                    "barbot/status", 0, true, "OFFLINE");
        }

        if (result) {
            client.publish("barbot/status", "IDLE", true);
            client.subscribe("barbot/cmd");
            client.subscribe("barbot/slot/+/set");
            client.subscribe("barbot/drink/+/set");
            client.subscribe("barbot/pairing/accept");
            client.subscribe("barbot/identify");

            // Getränkenamen sofort als retained publishen, damit frisch geladene
            // App-Clients sie ohne Wartezeit erhalten – unabhängig von der Settle-Zeit
            if(bot) {
                for(int i = 0; i < 3; i++) {
                    char topic[32];
                    snprintf(topic, sizeof(topic), "barbot/drink/%d/name", i);
                    client.publish(topic, bot->drinkNames[i].c_str(), true);
                }
            }

            updateMqttDisplay(true);
            return true;
        }
        Serial.printf("[MQTT] Connect failed, rc=%d\n", client.state());
        return false;
    }
    
    void mqttCallback(char* topic, byte* payload, unsigned int length) { 
        char msg[256];
        if(length > 255) length = 255;
        memcpy(msg, payload, length);
        msg[length] = '\0'; 
        
        String top = String(topic); 

        if(top == "barbot/cmd") { 
            if(strcmp(msg, "start") == 0) shouldStart = true; 
            if(strcmp(msg, "stop") == 0) shouldStop = true; 
        }
        else if(top.startsWith("barbot/slot/")) {
             int start = top.indexOf("/slot/") + 6;
             int end = top.lastIndexOf("/set");
             if(start > 0 && end > start && xSemaphoreTake(botMutex, pdMS_TO_TICKS(10))) {
                 bot->setSlot(top.substring(start, end).toInt(), atoi(msg));
                 xSemaphoreGive(botMutex);
             }
        }
        else if (top == "barbot/pairing/accept") {
            StaticJsonDocument<256> doc;
            DeserializationError err = deserializeJson(doc, msg);
            if(!err) {
                int slot = doc["slot"];
                String c_id = doc["client_id"] | ""; 
                
                Serial.printf("[MQTT] Kopplungs-Anfrage erhalten: Slot %d, Client %s\n", slot, c_id.c_str());
                
                if (slot >= 0 && slot < 12) {
                    slot_clients[slot] = c_id;
                    
                    String conf = "{\"slot\":" + String(slot) + ",\"client_id\":\"" + c_id + "\"}";
                    client.publish("barbot/pairing/success", conf.c_str());
                    Serial.println("[MQTT] Kopplung bestaetigt und Erfolg gemeldet!");
                    
                    isPairingMode = false;
                    
                    if(bot) {
                        bot->sendHasp("p3b99.bg_color=#0284c7");
                        bot->sendHasp("p3b99.text=\"Handy Koppeln\"");
                    }
                    publishStatus("Kopplung Slot " + String(slot) + " OK");
                } else {
                    Serial.println("[MQTT] Fehler: Slotnummer ungueltig!");
                }
            } else {
                Serial.printf("[MQTT] JSON Parse Error in Accept: %s\n", err.c_str());
            }
        }
        else if (top == "barbot/identify") {
            int slot = atoi(msg);
            if (slot >= 0 && slot < 12) {
                pendingIdentifySlot = slot;
                Serial.printf("[MQTT] Blinken angefordert fuer Slot %d\n", slot);
            }
        }
    }
    
    void mqttPublishIfChanged() {
        if(!mqttEnabled || !client.connected()) return;

        bool forceAll = !mqttSent.initialized;

        if(forceAll || cache.isBusy != mqttSent.isBusy) {
            mqttSent.isBusy = cache.isBusy;
            client.publish("barbot/status", cache.isBusy ? "BUSY" : "IDLE", true);
        }

        if(forceAll || cache.activeSlot != mqttSent.activeSlot) {
            mqttSent.activeSlot = cache.activeSlot;
            char val[8]; itoa(cache.activeSlot, val, 10);
            client.publish("barbot/activeSlot", val, true);
        }

        if(forceAll || cache.activeDrink != mqttSent.activeDrink) {
            mqttSent.activeDrink = cache.activeDrink;
            char val[8]; itoa(cache.activeDrink, val, 10);
            client.publish("barbot/activeDrink", val, true);
        }

        for(int i = 0; i < 12; i++) {
            if(forceAll || cache.glasses[i] != mqttSent.glasses[i]) {
                mqttSent.glasses[i] = cache.glasses[i];
                char topic[24]; snprintf(topic, 24, "barbot/glass/%d", i);
                client.publish(topic, cache.glasses[i] ? "ON" : "OFF", true);
            }
        }

        mqttSent.initialized = true;

        for(int i = 0; i < 12; i++) {
            if(forceAll || cache.slots[i] != mqttSent.slots[i]) {
                mqttSent.slots[i] = cache.slots[i];
                char topic[24]; snprintf(topic, 24, "barbot/slot/%d", i);
                char val[4];    itoa(cache.slots[i], val, 10);
                client.publish(topic, val, true);
            }
        }

        for(int i = 0; i < 3; i++) {
            if(forceAll || strncmp(cache.drinkNames[i], mqttSent.drinkNames[i], 32) != 0) {
                strncpy(mqttSent.drinkNames[i], cache.drinkNames[i], 32);
                mqttSent.drinkNames[i][32] = 0;
                char topic[32];
                snprintf(topic, 32, "barbot/drink/%d/name", i);
                client.publish(topic, cache.drinkNames[i], true);
                String imgUrl = "http://" + WiFi.localIP().toString() + "/drink_" + String(i) + ".jpg";
                snprintf(topic, 32, "barbot/drink/%d/image", i);
                client.publish(topic, imgUrl.c_str(), true);
            }
        }
    }

    void publishEvent(const char* event) {
        if(mqttEnabled && client.connected())
            client.publish("barbot/event", event, false);
    }

    void publishStatus(String status) {
        publishEvent(status.c_str());
    }
    
    bool getStartReq() { bool t = shouldStart; shouldStart = false; return t; }
    bool getStopReq() { bool t = shouldStop; shouldStop = false; return t; }

    void handleUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
        if(!index){
          if(fsUploadFile) fsUploadFile.close();
          String saveName = "/bg.jpg"; 
          if(request->hasParam("type")) { 
             String t = request->getParam("type")->value();
             if(t == "d0")    saveName = "/drink_0.jpg";
             else if(t == "d1")    saveName = "/drink_1.jpg";
             else if(t == "d2")    saveName = "/drink_2.jpg";
             else if(t == "d0bin") saveName = "/drink_0.bin";
             else if(t == "d1bin") saveName = "/drink_1.bin";
             else if(t == "d2bin") saveName = "/drink_2.bin";
          }
          fsUploadFile = LittleFS.open(saveName, "w");
        }
        if(fsUploadFile) fsUploadFile.write(data, len);
        if(final && fsUploadFile) fsUploadFile.close();
    }
};
#endif
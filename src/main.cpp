#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <ESPAsyncWebServer.h>
#include <ESP8266WiFi.h>
#include "FS.h"
#include "AsyncJson.h"
#include "ArduinoJson.h"
#include <SPIFFSEditor.h>
//#include <ArduinoOTA.h>

// To upload using curl:
// curl 10.7.10.23/edit --form 'data=@index.htm;filename=/index.htm' -v

#include "wificredentials.h"

#define PIN D1

#define NUM_LEDS 32
#define BRIGHTNESS 255

#define SCHEME_UPDATES 50 // 25 updates per second

#define WEB_PORT 80

Adafruit_NeoPixel strip = Adafruit_NeoPixel(NUM_LEDS, PIN, NEO_GRB + NEO_KHZ800);

struct RGBW
{
  byte r;
  byte g;
  byte b;
  byte w;
};

struct Scheme
{
  byte type;
  RGBW start;
  RGBW end;
  unsigned int dur;
  unsigned int altdur;
  unsigned long state;
};

Scheme schemes[NUM_LEDS];

// Schemes that are known/supported, also needs to be copied to JS code
#define NONE 0
#define FADE 1
#define FADE_ONE_WAY 2
#define FLICKER 3

AsyncWebServer server(WEB_PORT);

bool doledupdate = true;
unsigned long nextschemeupdate = SCHEME_UPDATES;
unsigned long nextupdate = 0;
unsigned long minnextupdate = 0;
#define UPDATE_FREQ 1000;
#define MAX_FREQ 50; // XXX

bool configchanged = false;
unsigned long nextsave = 0;
#define MAX_SAVE_FREQ 10000;

unsigned long hextolong(String hex)
{
  return strtoul(hex.c_str(), NULL, 16);
}

RGBW toRGBW(unsigned long src)
{
  RGBW ret;
  ret.r = (src >> 16) & 0xFF;
  ret.g = (src >> 8) & 0xFF;
  ret.b = (src >> 0) & 0xFF;
  ret.w = (src >> 24) & 0xFF;
  return ret;
}
RGBW toRGBW(String srcstr)
{
  return toRGBW(hextolong(srcstr));
}

unsigned long fromRGBW(RGBW src)
{
  unsigned long ret;
  ret = src.r;
  ret += src.g << 8;
  ret += src.b << 16;
  ret += src.w << 24;
  return ret;
}

void saveConfig()
{
  Serial.println("Saving Config...");
  char hex[10];
  File f = SPIFFS.open("/config.txt", "w");
  for (int i = 0; i < strip.numPixels(); i++)
  {
    sprintf(hex, "%06X", strip.getPixelColor(i));
    f.printf("%s\n", hex);
  }
  f.close();
}

void loadConfig()
{
  File f = SPIFFS.open("/config.txt", "r");
  if (!f)
  {
    Serial.println("file open failed");
  }
  else
  {
    int i = 0;
    while (true)
    {
      String s = f.readStringUntil('\n');
      Serial.printf("Got s: [%s]\n", s.c_str());
      if (!s.length() || i >= strip.numPixels())
      {
        break;
      }
      Serial.printf("Setting: %d to %lX\n", i, hextolong(s));

      strip.setPixelColor(i, hextolong(s));
      i++;
    }
    f.close();
  }
}

void handleUpdate(AsyncWebServerRequest *request)
{
  Serial.print("Starting: ");
  Serial.println(millis());

  String message = "Updated\n";

  Serial.print("Made Message: ");
  Serial.println(millis());
  uint16_t led;
  uint32_t color;
  int params = request->params();
  for (int i = 0; i < params; i++)
  {
    AsyncWebParameter *p = request->getParam(i);
    led = p->name().toInt();
    Serial.print("Setting LED :");
    Serial.print(led);
    Serial.print(" to:");
    color = hextolong(p->value().c_str());
    Serial.println(color);
    strip.setPixelColor(led, color);
  }

  doledupdate = true;
  request->send(200, "text/plain", message);
  configchanged = true;
}

void handleSetScheme(AsyncWebServerRequest *request)
{
  String message = "Saved\n";
  Serial.println("About to read data");
  int params = request->params();
  String jsonstr;
  for (int i = 0; i < params; i++)
  {
    AsyncWebParameter *p = request->getParam(i);
    if (p->name() == String("data"))
    {
      Serial.println("Match!");
      jsonstr = p->value();
    }
  }

  if (jsonstr.length())
  {
    Serial.println("Got data:");
    Serial.println(jsonstr);

    File f = SPIFFS.open("/schemes.json", "w");
    f.print(jsonstr);
    f.close();

    Serial.println("Saved:");
  }
  else
  {
    Serial.println("No DATA!");
  }

  doledupdate = true;
  request->send(200, "text/plain", message);
  configchanged = true;
}

void handleConfig(AsyncWebServerRequest *request)
{
  char hex[10];
  AsyncJsonResponse *response = new AsyncJsonResponse();
  response->addHeader("Server", "ESP Async Web Server");
  JsonObject &root = response->getRoot();
  root["heap"] = ESP.getFreeHeap();
  root["ssid"] = WiFi.SSID();
  JsonArray &colors = root.createNestedArray("colors");
  for (int i = 0; i < strip.numPixels(); i++)
  {
    Serial.printf("LED: %d color %08x\n", i, strip.getPixelColor(i));
    sprintf(hex, "%06X", strip.getPixelColor(i));
    colors.add(hex);
  }
  response->setLength();
  request->send(response);
}

// Loads the schemes from file
void loadSchemes()
{
  unsigned long et = millis();

  //DynamicJsonBuffer schemeJsonBuffer;
  StaticJsonBuffer<127 * NUM_LEDS> schemesJsonBuffer;

  File f = SPIFFS.open("/schemes.json", "r");
  if (!f)
  {
    Serial.println("file open failed");
    File f = SPIFFS.open("/schemes.json", "w");
    f.print("{}");
    f.close();
    return;
  }
  JsonObject &jschemes = schemesJsonBuffer.parseObject(f);
  Serial.print("Success: ");
  Serial.println(jschemes.success());
  String tmp;
  for (int i = 0; i < NUM_LEDS; i++)
  {
    if (jschemes.containsKey(String(i)))
    {
      JsonObject &jscheme = jschemes[String(i)];

      if (jscheme.containsKey("start"))
      {
        tmp = jscheme["start"].as<String>();
        schemes[i].start = toRGBW(tmp);
      }
      if (jscheme.containsKey("end"))
      {
        tmp = jscheme["end"].as<String>();
        schemes[i].end = toRGBW(tmp);
      }
      if (jscheme.containsKey("dur"))
      {
        schemes[i].dur = jscheme["dur"];
      }
      if (jscheme.containsKey("altdur"))
      {
        schemes[i].altdur = jscheme["altdur"];
      }

      String stype = jscheme["sc"];
      Serial.println(stype);
      if (stype.equals("fade"))
      {
        schemes[i].type = FADE;
      }
      else if (stype.equals("fadeoneway"))
      {
        schemes[i].type = FADE_ONE_WAY;
      }
      else if (stype.equals("flicker"))
      {
        schemes[i].type = FLICKER;
      }
      else
      {
        schemes[i].type = NONE;
      }
      schemes[i].state = 0;
    }
  }
  Serial.print("Total time:");
  Serial.println(millis() - et);
  f.close();
}

byte colprogress(byte start, byte end, float pcg)
{
  int total = end - start;
  byte ret = start + (total * pcg);
  return ret;
}

RGBW blend(RGBW a, RGBW b, float pcg)
{
  RGBW out;
  out.r = colprogress(a.r, b.r, pcg);
  out.g = colprogress(a.g, b.g, pcg);
  out.b = colprogress(a.b, b.b, pcg);
  out.w = colprogress(a.w, b.w, pcg);
  return out;
}

void updateLEDSchemes()
{
  unsigned long now = millis();
  for (int i = 0; i < NUM_LEDS; i++)
  {
    Scheme scheme = schemes[i];
    if (scheme.type == NONE)
    {
      continue;
    }
    RGBW out;
    bool updated = false;
    if (scheme.type == FADE)
    {
      unsigned long step = now % scheme.dur;
      float pcg = (float)step / (float)scheme.dur;
      if (pcg < 0.5)
        pcg = pcg * 2;
      else
        pcg = (1.0 - pcg) * 2;
      out = blend(scheme.start, scheme.end, pcg);
      updated = true;
    }
    else if (scheme.type == FADE_ONE_WAY)
    {
      unsigned long step = now % scheme.dur;
      float pcg = (float)step / (float)scheme.dur;
      out = blend(scheme.start, scheme.end, pcg);
      updated = true;
    }

    else if (scheme.type == FLICKER)
    {
      if (now >= scheme.state)
      {
        // Time to change colour
        unsigned long currentcol = strip.getPixelColor(i);
        if (currentcol == strip.Color(scheme.start.r, scheme.start.g, scheme.start.b))
        {
          // turn to end state, and wait alt duration
          out = scheme.end;
          unsigned long rand = random(scheme.altdur);
          schemes[i].state = now + rand + 1;
        }
        else
        {
          out = scheme.start;
          schemes[i].state = now + random(scheme.dur) + 1;
        }
        updated = true;
      }
    }
    if (updated)
    {
      strip.setPixelColor(i, strip.Color(out.r, out.g, out.b));
    }
    delay(0);
  }
}

void setup()
{
  Serial.begin(115200);

  Serial.println("\n\n");
  Serial.println("Starting SPIFFS...");
  SPIFFS.begin();
  Serial.println("Started SPIFFS");

  strip.setBrightness(BRIGHTNESS);
  strip.begin();
  /*
  for (uint16_t i = 0; i < strip.numPixels(); i++)
  {
    strip.setPixelColor(i, strip.Color(1, 1, 1));
  }
  */
  strip.show(); // Initialize all pixels to 'off'

  int counter = 0;

  WiFi.begin(ssid, password);
  // Wait for connection
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
    counter++;
    for (uint16_t i = 0; i < counter; i++)
    {
      strip.setPixelColor(i, strip.Color(100, 0, 0));
    }
    strip.show();
  }
  Serial.println("");
  Serial.print("Connected to ");
  Serial.println(ssid);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  for (uint16_t i = 0; i < 10; i++)
  {
    strip.setPixelColor(i, strip.Color(0, 1, 0));
  }
  strip.show();

  //Send OTA events to the browser
  /*
  ArduinoOTA.onStart([]() { events.send("Update Start", "ota"); });
  ArduinoOTA.onEnd([]() { events.send("Update End", "ota"); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    char p[32];
    sprintf(p, "Progress: %u%%\n", (progress / (total / 100)));
    events.send(p, "ota");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    if (error == OTA_AUTH_ERROR)
      events.send("Auth Failed", "ota");
    else if (error == OTA_BEGIN_ERROR)
      events.send("Begin Failed", "ota");
    else if (error == OTA_CONNECT_ERROR)
      events.send("Connect Failed", "ota");
    else if (error == OTA_RECEIVE_ERROR)
      events.send("Recieve Failed", "ota");
    else if (error == OTA_END_ERROR)
      events.send("End Failed", "ota");
  });
  */
  //ArduinoOTA.setHostname("what");
  //ArduinoOTA.begin();

  Serial.println("OTA Setup");

  // Web Server setup
  server.addHandler(new SPIFFSEditor());

  server.on("/heap", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", String(ESP.getFreeHeap()));
  });

  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.htm");

  server.on("/update", HTTP_GET + HTTP_POST, handleUpdate);
  server.on("/setscheme", HTTP_POST, handleSetScheme);

  server.on("/config", HTTP_GET, handleConfig);

  server.begin();
  Serial.println("HTTP server started");
  loadConfig();
  loadSchemes();
  randomSeed(micros());
}

void loop()
{
  unsigned long now = millis();
  if (doledupdate || now >= nextupdate)
  {
    if (minnextupdate > now)
    {
      Serial.println("Updating too fast, ignoring");
      doledupdate = false;
      nextupdate = minnextupdate;
    }
    else
    {
      Serial.printf("Doing update at %lu, Free Heap: %d\n", millis(), ESP.getFreeHeap());
      delay(0);
      strip.show();
      delay(0);
      doledupdate = false;
      nextupdate = now + UPDATE_FREQ;
      minnextupdate = now + MAX_FREQ;
      Serial.printf("That took %lu millis\n", millis() - now);
    }
  }
  if (configchanged && now > nextsave)
  {
    delay(0);
    saveConfig();
    delay(0);
    loadSchemes();
    nextsave = now + MAX_SAVE_FREQ;
    configchanged = false;
  }
  if (now >= nextschemeupdate)
  {
    updateLEDSchemes();
    nextschemeupdate = now + SCHEME_UPDATES - (now % SCHEME_UPDATES);
    delay(0);
    strip.show();
  }
  //ArduinoOTA.handle();
}
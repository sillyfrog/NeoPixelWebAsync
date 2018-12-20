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

#define NUM_LEDS 20
#define BRIGHTNESS 255

#define WEB_PORT 80
Adafruit_NeoPixel strip = Adafruit_NeoPixel(NUM_LEDS, PIN, NEO_GRB + NEO_KHZ800);

AsyncWebServer server(WEB_PORT);

bool doledupdate = true;
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
      Serial.printf("Doing update at %lu, Free Heap: %du\n", millis(), ESP.getFreeHeap());
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
    nextsave = now + MAX_SAVE_FREQ;
    configchanged = false;
  }
  //ArduinoOTA.handle();
}
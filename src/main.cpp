#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <ESPAsyncWebServer.h>
#include <ESP8266WiFi.h>
#include "FS.h"
#include "AsyncJson.h"
#include "ArduinoJson.h"
#include <SPIFFSEditor.h>
#include "Adafruit_TLC5947.h"
//#include <ArduinoOTA.h>

// To upload using curl:
// curl 10.7.10.23/edit --form 'data=@index.htm;filename=/index.htm' -v

#include "wificredentials.h"

#define NUM_TLC5974 1
#define DN D5
#define CLK D6
#define LAT D7
#define PWM_MAX 3750 // @12v

#define MAX_NUM_STRIPS 3
#define MAX_NUM_LEDS MAX_NUM_STRIPS * 30
#define BRIGHTNESS 255

#define SCHEME_UPDATES 50 // 25 updates per second

#define WEB_PORT 80

uint16_t strip_sizes[MAX_NUM_STRIPS];
uint16_t total_pixels = 0;

uint8_t pin_allocations[] = {D1, D2, D3};

bool dorainbow = false;

Adafruit_NeoPixel strips[MAX_NUM_STRIPS];
Adafruit_NeoPixel neofuncs = Adafruit_NeoPixel(0, -1, NEO_GRB + NEO_KHZ800);

Adafruit_TLC5947 tlc = Adafruit_TLC5947(NUM_TLC5974, CLK, DN, LAT);

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

Scheme schemes[MAX_NUM_LEDS];

// Schemes that are known/supported, also needs to be copied to JS code
#define NONE 0
#define FADE 1
#define FADE_ONE_WAY 2
#define FLICKER 3

AsyncWebServer server(WEB_PORT);
AsyncWebSocket ws("/ws");

unsigned long nextschemeupdate = SCHEME_UPDATES;
unsigned long nextupdate = 0;
#define UPDATE_FREQ 1000;

bool configchanged = false;
bool updatenonescheme = true;
unsigned long nextsave = 0;
#define MAX_SAVE_FREQ 10000;

extern void setScheme(int i, String jsondata);

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

/*
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
*/

void loadStripConfig()
{
  File f = SPIFFS.open("/stripdata.txt", "r");
  if (!f)
  {
    Serial.println("file open failed");
  }
  else
  {
    int i = 0;
    total_pixels = 0;
    while (true)
    {
      String s = f.readStringUntil('\n');
      Serial.printf("Got stripsize: [%s]\n", s.c_str());
      strip_sizes[i] = s.toInt();
      total_pixels += strip_sizes[i];
      Serial.printf("Setting size of strip %d to %d\n", i, strip_sizes[i]);

      // Actually configure the strip
      strips[i].updateLength(strip_sizes[i]);
      strips[i].updateType(NEO_GRB + NEO_KHZ800);
      strips[i].setPin(pin_allocations[i]);
      strips[i].setBrightness(BRIGHTNESS);
      strips[i].begin();
      delay(0);
      i++;
      if (i >= MAX_NUM_STRIPS)
      {
        break;
      }
    }
    f.close();
    // Ensure all unused strips are set to zero size
    while (i < MAX_NUM_STRIPS)
    {
      strip_sizes[i] = 0;
      i++;
    }
  }
}

uint32_t applyGamma(uint32_t c)
{
  uint8_t r = neofuncs.gamma8((uint8_t)(c >> 16));
  uint8_t g = neofuncs.gamma8((uint8_t)(c >> 8));
  uint8_t b = neofuncs.gamma8((uint8_t)c);
  uint32_t color = (r << 16) + (g << 8) + (b);
  return color;
}

void setPixelColor(uint16_t n, uint32_t c)
{
  if (n >= total_pixels)
  {
    return;
  }
  // First figure out the strip to apply this to
  int i = 0;
  while (i < MAX_NUM_STRIPS)
  {
    if (n >= strips[i].numPixels())
    {
      n -= strips[i].numPixels();
      i++;
    }
    else
    {
      break;
    }
  }
  // Apply gamma correction to each color component
  // XXX need to handle white should we support it down the track
  strips[i].setPixelColor(n, applyGamma(c));
}

uint32_t getPixelColor(uint16_t n)
{
  // First figure out the strip to apply this to
  int i = 0;
  while (i < MAX_NUM_STRIPS)
  {
    if (n >= strips[i].numPixels())
    {
      n -= strips[i].numPixels();
      i++;
    }
    else
    {
      break;
    }
  }
  return strips[i].getPixelColor(n);
}

void showAll()
{
  for (int i = 0; i < MAX_NUM_STRIPS; i++)
  {
    if (strip_sizes[i] > 0)
    {
      strips[i].show();
      delay(0);
    }
  }
}

// Types of incoming websocket messages - must match index.htm script
#define COLOR_UPDATE String("U\n")
#define SCHEME_UPDATE String("S\n")

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len)
{
  if (type == WS_EVT_CONNECT)
  {
    Serial.printf("ws[%s][%u] connect\n", server->url(), client->id());
    client->printf("Hello Client %u :)", client->id());
    client->ping();
  }
  else if (type == WS_EVT_DISCONNECT)
  {
    Serial.printf("ws[%s][%u] disconnect: %ul\n", server->url(), client->id());
  }
  else if (type == WS_EVT_ERROR)
  {
    Serial.printf("ws[%s][%u] error(%u): %s\n", server->url(), client->id(), *((uint16_t *)arg), (char *)data);
  }
  else if (type == WS_EVT_PONG)
  {
    Serial.printf("ws[%s][%u] pong[%u]: %s\n", server->url(), client->id(), len, (len) ? (char *)data : "");
  }
  else if (type == WS_EVT_DATA)
  {
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    String msg = "";
    if (info->final && info->index == 0 && info->len == len)
    {
      //the whole message is in a single frame and we got all of it's data
      Serial.printf("ws[%s][%u] %s-message[%llu]: ", server->url(), client->id(), (info->opcode == WS_TEXT) ? "text" : "binary", info->len);

      if (info->opcode == WS_TEXT)
      {
        for (size_t i = 0; i < info->len; i++)
        {
          msg += (char)data[i];
        }
      }
      else
      {
        char buff[3];
        for (size_t i = 0; i < info->len; i++)
        {
          sprintf(buff, "%02x ", (uint8_t)data[i]);
          msg += buff;
        }
      }
      Serial.printf("%s\n", msg.c_str());

      if (info->opcode == WS_TEXT)
      {
        String tmp;
        if (msg.startsWith(COLOR_UPDATE))
        {
          uint16_t led;
          uint32_t color;
          msg.remove(0, 2);
          Serial.print("JSON Data about to process: ");
          Serial.println(msg);
          // Calculated from https://arduinojson.org/v5/assistant/
          // plus a bunch of margin, global did not work
          StaticJsonBuffer<(500)> globalJsonBuffer;
          JsonObject &js = globalJsonBuffer.parseObject(msg);
          tmp = js["led"].as<String>();
          led = tmp.toInt();
          tmp = js["color"].as<String>();
          color = hextolong(tmp);
          setPixelColor(led, color);
        }
        else if (msg.startsWith(SCHEME_UPDATE))
        {
          uint16_t led;
          msg.remove(0, 2);
          led = msg.toInt();
          msg.remove(0, msg.indexOf('\n'));
          Serial.print("JSON Data about to set Scheme: ");
          Serial.println(msg);
          setScheme(led, msg);
        }
        client->text("OK");
      }
      else
        client->binary("I got your binary message");
    }
    else
    {
      //message is comprised of multiple frames or the frame is split into multiple packets
      if (info->index == 0)
      {
        if (info->num == 0)
          Serial.printf("ws[%s][%u] %s-message start\n", server->url(), client->id(), (info->message_opcode == WS_TEXT) ? "text" : "binary");
        Serial.printf("ws[%s][%u] frame[%u] start[%llu]\n", server->url(), client->id(), info->num, info->len);
      }

      Serial.printf("ws[%s][%u] frame[%u] %s[%llu - %llu]: ", server->url(), client->id(), info->num, (info->message_opcode == WS_TEXT) ? "text" : "binary", info->index, info->index + len);

      if (info->opcode == WS_TEXT)
      {
        for (size_t i = 0; i < info->len; i++)
        {
          msg += (char)data[i];
        }
      }
      else
      {
        char buff[3];
        for (size_t i = 0; i < info->len; i++)
        {
          sprintf(buff, "%02x ", (uint8_t)data[i]);
          msg += buff;
        }
      }
      Serial.printf("%s\n", msg.c_str());

      if ((info->index + len) == info->len)
      {
        Serial.printf("ws[%s][%u] frame[%u] end[%llu]\n", server->url(), client->id(), info->num, info->len);
        if (info->final)
        {
          Serial.printf("ws[%s][%u] %s-message end\n", server->url(), client->id(), (info->message_opcode == WS_TEXT) ? "text" : "binary");
          if (info->message_opcode == WS_TEXT)
            client->text("I have no idea... some part of the message...");
          else
            client->binary("I got your binary message");
        }
      }
    }
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
    setPixelColor(led, color);
  }

  request->send(200, "text/plain", message);
}

void handleLedCount(AsyncWebServerRequest *request)
{
  String message = String(total_pixels);
  request->send(200, "text/plain", message);
}

void handleRainbow(AsyncWebServerRequest *request)
{
  String message = "Set";
  request->send(200, "text/plain", message);
  dorainbow = true;
}
void handleUpdateScheme(AsyncWebServerRequest *request)
{
  String message = "Updating\n";
  request->send(200, "text/plain", message);
  configchanged = true;
  updatenonescheme = true;
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
  for (int i = 0; i < total_pixels; i++)
  {
    Serial.printf("LED: %d color %08x\n", i, getPixelColor(i));
    sprintf(hex, "%06X", getPixelColor(i));
    colors.add(hex);
  }
  response->setLength();
  request->send(response);
}

void setScheme(int i, String jsondata)
{
  // Calculated from https://arduinojson.org/v5/assistant/
  // plus a bunch of margin, global did not work
  StaticJsonBuffer<(500)> globalJsonBuffer;
  JsonObject &jscheme = globalJsonBuffer.parseObject(jsondata);
  Serial.print(jsondata);
  Serial.print(" - Success: ");
  Serial.println(jscheme.success());
  String tmp;

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

// Loads the schemes from file files
// Stored as 1 file per LED
void loadSchemes()
{
  unsigned long et = millis();

  File f = SPIFFS.open("/schemes.txt", "r");
  if (!f)
  {
    Serial.println("file open failed ");
    File f = SPIFFS.open("/schemes.txt", "w");
    f.print("");
  }
  else
  {
    while (1)
    {
      String ledstr = f.readStringUntil(':');
      String jsondata = f.readStringUntil('\n');
      if (ledstr.length() == 0)
      {
        break;
      }
      int i = ledstr.toInt();

      Serial.print("Free heap: ");
      Serial.println(ESP.getFreeHeap());
      setScheme(i, jsondata);
      delay(0);
    }
  }
  f.close();
  Serial.print("Total time:");
  Serial.println(millis() - et);
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
  for (int i = 0; i < total_pixels; i++)
  {
    Scheme scheme = schemes[i];
    RGBW out;
    bool updated = false;
    if (scheme.type == NONE)
    {
      if (updatenonescheme)
      {
        out = scheme.start;
        updated = true;
      }
    }
    else if (scheme.type == FADE)
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
        unsigned long currentcol = getPixelColor(i);
        if (currentcol == applyGamma(neofuncs.Color(scheme.start.r, scheme.start.g, scheme.start.b)))
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
      setPixelColor(i, neofuncs.Color(out.r, out.g, out.b));
    }
    delay(0);
  }
  updatenonescheme = false;
}

// Input a value 0 to 255 to get a color value.
// The colours are a transition r - g - b - back to r.
uint32_t Wheel(byte WheelPos)
{
  WheelPos = 255 - WheelPos;
  if (WheelPos < 85)
  {
    return neofuncs.Color(255 - WheelPos * 3, 0, WheelPos * 3, 0);
  }
  if (WheelPos < 170)
  {
    WheelPos -= 85;
    return neofuncs.Color(0, WheelPos * 3, 255 - WheelPos * 3, 0);
  }
  WheelPos -= 170;
  return neofuncs.Color(WheelPos * 3, 255 - WheelPos * 3, 0, 0);
}

// Slightly different, this makes the rainbow equally distributed throughout
void rainbowCycle(uint8_t wait)
{
  uint16_t i, j;

  for (j = 0; j < 256 * 5; j++)
  { // 5 cycles of all colors on wheel
    for (i = 0; i < total_pixels; i++)
    {
      setPixelColor(i, Wheel(((i * 256 / total_pixels) + j) & 255));
    }
    showAll();
    delay(wait);
  }
  //  configchanged = true;
  updatenonescheme = true;
}

void setup()
{
  Serial.begin(115200);

  Serial.println("\n\n");
  Serial.println("Starting...\n\n");
  tlc.begin();

  Serial.println("Starting SPIFFS...");
  SPIFFS.begin();
  Serial.println("Started SPIFFS");

  /*
  for (uint16_t i = 0; i < strip.numPixels(); i++)
  {
    strip.setPixelColor(i, strip.Color(1, 1, 1));
  }
  */
  loadStripConfig();
  showAll();

  int counter = 0;

  delay(0);
  WiFi.begin(ssid, password);
  // Wait for connection
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
    counter++;
    for (uint16_t i = 0; i < counter; i++)
    {
      setPixelColor(i, neofuncs.Color(100, 0, 0));
    }
    showAll();
  }
  Serial.println("");
  Serial.print("Connected to ");
  Serial.println(ssid);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  for (uint16_t i = 0; i < 10; i++)
  {
    setPixelColor(i, neofuncs.Color(0, 20, 0));
  }
  showAll();

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

  delay(0);
  Serial.println("OTA Setup");

  // Web Server setup
  server.addHandler(new SPIFFSEditor());

  server.on("/heap", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", String(ESP.getFreeHeap()));
  });

  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.htm");

  server.on("/update", HTTP_GET + HTTP_POST, handleUpdate);
  server.on("/updatescheme", HTTP_GET + HTTP_POST, handleUpdateScheme);
  server.on("/ledcount", HTTP_GET + HTTP_POST, handleLedCount);
  server.on("/rainbow", HTTP_GET + HTTP_POST, handleRainbow);

  server.on("/config", HTTP_GET, handleConfig);

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  delay(0);
  server.begin();
  Serial.println("HTTP server started");
  delay(0);
  loadSchemes();
  delay(0);
  randomSeed(micros());
  delay(0);
}

unsigned long jxxx = 0;
void loop()
{
  tlc.setPWM(14, 3750);
  /*
  for (uint16_t i = 0; i < 1096; i += 50)
  {
    //Serial.println(i);
    tlc.setPWM(0, i);
    tlc.write();
    delay(100);
    jxxx++;
    Serial.println(i);
  }
  return; //XXX
  */
  unsigned long now = millis();
  if (now >= nextupdate)
  {
    Serial.printf("Doing update at %lu, Free Heap: %d, configchanged: %d\n", millis(), ESP.getFreeHeap(), configchanged);
    delay(0);
    //showAll()
    nextupdate = now + UPDATE_FREQ;
    //Serial.printf("That took %lu millis\n", millis() - now);
  }
  if (configchanged && now > nextsave)
  {
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
    showAll();
  }
  if (dorainbow)
  {
    dorainbow = false;
    rainbowCycle(8);
  }
  //ArduinoOTA.handle();
}
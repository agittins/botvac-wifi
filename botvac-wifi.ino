#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WiFiClient.h>
#include <WebSocketsServer.h>
#include <WebSocketsClient.h>
#include <Hash.h>
#include <FS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266HTTPClient.h>
#include <TimedAction.h>
#include <rBase64.h>
#include <ArduinoJson.h>

#define SSID_FILE "/etc/ssid"
#define PASSWORD_FILE "/etc/pass"
#define HOSTNAME_FILE "/etc/hostname"
#define WSURL_FILE "/etc/wsurl"
#define SERIAL_FILE "/etc/serial"

#define CONNECT_TIMEOUT_SECS 30
#define SERIAL_NUMBER_ATTEMPTS 5

#define AP_SSID "neato"


#define MAX_BUFFER 8192

String HOSTNAME = "botvac-wifi";

String readString;
String incomingErr;
String batteryPercent = "";
boolean charging = false;
String lidarInfo;
String serialNumber = "Empty";
int lastBattRun = 0;
int lastErrRun = 0;
int lastTimeRun = 288;

WiFiClient client;
int bufferSize = 0;
uint8_t currentClient = 0;
uint8_t serialBuffer[8193];
ESP8266WebServer server (80);
WebSocketsServer webSocket = WebSocketsServer(81);
WebSocketsClient webSocketClient;
ESP8266WebServer updateServer(82);
ESP8266HTTPUpdateServer httpUpdater;

void getPage() {
  if (WiFi.status() == WL_CONNECTED) { //Check WiFi connection status

      // Only check battery once at the beginning, once every 60 seconds, or once when it got an empty response.
      if (batteryPercent == "" || batteryPercent == "-FAIL-" || lastBattRun > 11) {
        batteryPercent = getBattery("FuelPercent");
        lastBattRun = 0;
      } else {
        lastBattRun++;
      }
      if (serialNumber == "" || serialNumber == "-FAIL-" || lastBattRun > 11) {
        charging = getBattery("ChargingActive") == "1";
        lastBattRun = 0;
      } else {
        lastBattRun++;
      }
      
      if (incomingErr == "" || incomingErr == "-FAIL-" || lastErrRun > 11) {
        getError();
        lastErrRun = 0;
      } else {
        lastErrRun++;
      }

      if (batteryPercent != "" && batteryPercent != "-FAIL-" && serialNumber != "Empty") {
        webSocketClient.sendTXT(String() +
          "{" + 
            "\"header\": { " + 
              "\"ts\": " + String(millis()) + "," +
              "\"type\": \"neato\"," +
              "\"title\": \"neato\" " + 
            "}," + 
            "\"payload\": {" +
              "\"serial\": \"" + serialNumber + "\"," +
              "\"battery\": " + batteryPercent + "," +
              "\"charging\": " + (charging ? "true":"false") + "," +
              "\"errorMsg\": \"" + incomingErr + "\"" +
            "}" +
          "}");
      }
    }
}

String getSerial() {
  String serial;
  File serial_file = SPIFFS.open(SERIAL_FILE, "r");
  if(! serial_file || serial_file.readString() == "" ) {
    serial_file.close();
    Serial.setTimeout(250);
    String incomingSerial;
    int serialString;
    
    for(int i=0; i <= SERIAL_NUMBER_ATTEMPTS; i++) {
      Serial.println("GetVersion");
      incomingSerial = Serial.readString();
      serialString = incomingSerial.indexOf("Serial Number");
      if (serialString > -1)
        break;
      delay(50);
    }


    if (serialString == -1)
      serial = "Empty";
    else {
      int capUntil = serialString+50;
      String serialCap = incomingSerial.substring(serialString, capUntil);
      int commaIndex = serialCap.indexOf(',');
      int secondCommaIndex = serialCap.indexOf(',', commaIndex + 1);
      int thirdCommaIndex = serialCap.indexOf(',', secondCommaIndex + 1);
      String incomingSerialCheck = serialCap.substring(secondCommaIndex + 1, thirdCommaIndex); // To the end of the string
      if (incomingSerialCheck.indexOf("Welcome") > -1) {
        ESP.reset();
      } else {
        serial_file = SPIFFS.open(SERIAL_FILE, "w");
        serial_file.print(incomingSerialCheck);
        serial_file.close();
        serial = incomingSerialCheck;
      }
    }
  }
  else {
    serial_file.seek(0, SeekSet);
    serial = serial_file.readString();
    serial_file.close();
  }
  return serial;
}

void getError() {
    Serial.setTimeout(100);
    Serial.println("GetErr");
    String incomingErrTemp = Serial.readString();
    // Check for dash in error
    int serialString = incomingErrTemp.indexOf(" - ");
    int capUntil = serialString-4;
    if (serialString > -1){
      incomingErr = rbase64.encode(incomingErrTemp.substring(capUntil,serialString));
      incomingErr.replace('+', '-');
      incomingErr.replace('/', '_');
      incomingErr.replace('=', ',');
    } else {
      incomingErr = "";
    }
}

String getBattery(String value) {
    Serial.setTimeout(500);
    Serial.println("GetCharger");
    String chargerTemp = Serial.readString();
    String val = "";
    
    int serialString = chargerTemp.indexOf(value);
    if (serialString > -1){
      int checkLength = value.length()+4;
      int capUntil = serialString+checkLength;
      int commaIndex = chargerTemp.substring(serialString, capUntil).indexOf(',');
      String currentItem = chargerTemp.substring(serialString,capUntil);
      currentItem.trim();
      
      val = currentItem.substring(commaIndex+1,capUntil);
    }

    return val;
}

//create a couple timers that will fire repeatedly every x ms
TimedAction checkServer = TimedAction(5000,getPage);

void botDissconect() {
  // always disable testmode on disconnect
  Serial.println("TestMode off");
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      // always disable testmode on disconnect
      botDissconect();
      break;
    case WStype_CONNECTED:
      webSocket.sendTXT(num, "connected to Neato");
      // allow only one concurrent client connection
      currentClient = num;
      // all clients but the last connected client are disconnected
      for (uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++) {
        if (i != currentClient) {
          webSocket.disconnect(i);
        }
      }
      // reset serial buffer on new connection to prevent garbage
      serialBuffer[0] = '\0';
      bufferSize = 0;
      break;
    case WStype_TEXT:
      // send incoming data to bot
      Serial.printf("%s\n", payload);
      break;
    case WStype_BIN:
      webSocket.sendTXT(num, "binary transmission is not supported");
      break;
  }
}

void serverEvent() {
  // just a very simple websocket terminal, feel free to use a custom one
  server.send(200, "text/html", "<!DOCTYPE html><meta charset='utf-8' /><style>p{white-space:pre;word-wrap:break-word;font-family:monospace;}</style><title>Neato Console</title><script language='javascript' type='text/javascript'>var b='ws://'+location.hostname+':81/',c,d,e;function g(){d=new WebSocket(b);d.onopen=function(){h('[connected]')};d.onclose=function(){h('[disconnected]')};d.onmessage=function(a){h('<span style=\"color: blue;\">[response] '+a.data+'</span>')};d.onerror=function(a){h('<span style=\"color: red;\">[error] </span> '+a.data)}}\nfunction k(a){if(13==a.keyCode){a=e.value;if('/disconnect'==a)d.close();else if('/clear'==a)for(;c.firstChild;)c.removeChild(c.firstChild);else''!=a&&(h('[sent] '+a),d.send(a));e.value='';e.focus()}}function h(a){var f=document.createElement('p');f.innerHTML=a;c.appendChild(f);window.scrollTo(0,document.body.scrollHeight)}\nwindow.addEventListener('load',function(){c=document.getElementById('c');e=document.getElementById('i');g();document.getElementById('i').addEventListener('keyup',k,!1);e.focus()},!1);</script><h2>Neato Console</h2><div id='c'></div><input type='text' id='i' style=\"width:100%;font-family:monospace;\">\n");
}

void setupEvent() {
  char ssid[256];
  File ssid_file = SPIFFS.open(SSID_FILE, "r");
  if(!ssid_file) {
    strcpy(ssid, "XXX");
  }
  else {
    ssid_file.readString().toCharArray(ssid, 256);
    ssid_file.close();
  }

  char passwd[256];
  File passwd_file = SPIFFS.open(PASSWORD_FILE, "r");
  if(!passwd_file) {
    strcpy(passwd, "XXX");
  }
  else {
    passwd_file.readString().toCharArray(passwd, 256);
    passwd_file.close();
  }

  String host;
  File host_file = SPIFFS.open(HOSTNAME_FILE, "r");
  if(!host_file) {
    host = HOSTNAME;
  }
  else {
    host = host_file.readString();
    host_file.close();
  }

  char wsurl[256];
  File wsurl_file = SPIFFS.open(WSURL_FILE, "r");
  if(!wsurl_file) {
    strcpy(wsurl, "XXX");
  }
  else {
    wsurl_file.readString().toCharArray(wsurl, 256);
    wsurl_file.close();
  }

  server.send(200, "text/html", String() + 
  "<!DOCTYPE html><html> <body>" +
  "<p>Neato serial number: <b>" + serialNumber + "</b></p>" +
  "<form action=\"\" method=\"post\" style=\"display: inline;\">" +
  "Access Point SSID:<br />" +
  "<input type=\"text\" name=\"ssid\" value=\"" + ssid + "\"> <br />" +
  "WPA2 Password:<br />" +
  "<input type=\"text\" name=\"password\" value=\"" + passwd + "\"> <br />" +
  "<br />" +
  "Hostname:<br />" +
  "<input type=\"text\" name=\"host\" value=\"" + host + "\"> <br />" +
  "<br />" +
  "WebSocket server URL:<br />" +
  "<input type=\"text\" name=\"wsurl\" value=\"" + wsurl + "\"> <br />" +
  "<br />" +
  "<input type=\"submit\" value=\"Submit\"> </form>" +
  "<form action=\"http://" + HOSTNAME + ".local/reboot\" style=\"display: inline;\">" +
  "<input type=\"submit\" value=\"Reboot\" />" +
  "</form>" +
  "<p>Enter the details for your access point. After you submit, the controller will reboot to apply the settings.</p>" +
  "<p><a href=\"http://" + HOSTNAME + ".local:82/update\">Update Firmware</a></p>" +
  "<p><a href=\"http://" + HOSTNAME + ".local/console\">Neato Serial Console</a> - <a href=\"https://www.neatorobotics.com/resources/programmersmanual_20140305.pdf\">Command Documentation</a></p>" +
  "</body></html>\n");
}

void saveEvent() {
  String user_ssid = server.arg("ssid");
  String user_password = server.arg("password");
  String host = server.arg("host");
  String wsurl = server.arg("wsurl");

  SPIFFS.format();

  if(user_ssid != "" && user_password != "" && host != "" && wsurl.startsWith("ws://")) {
    SPIFFS.begin();
    File ssid_file = SPIFFS.open(SSID_FILE, "w");
    if (!ssid_file) {
      server.send(200, "text/html", "<!DOCTYPE html><html> <body> Setting Access Point SSID failed!</body> </html>");
      return;
    }
    ssid_file.print(user_ssid);
    ssid_file.close();

    File passwd_file = SPIFFS.open(PASSWORD_FILE, "w");
    if (!passwd_file) {
      server.send(200, "text/html", "<!DOCTYPE html><html> <body> Setting Access Point password failed!</body> </html>");
      return;
    }
    passwd_file.print(user_password);
    passwd_file.close();

    File hostname_file = SPIFFS.open(HOSTNAME_FILE, "w");
    if (!hostname_file) {
      server.send(200, "text/html", "<!DOCTYPE html><html> <body> Setting hostname failed!</body> </html>");
      return;
    }
    hostname_file.print(host);
    hostname_file.close();

    File wsurl_file = SPIFFS.open(WSURL_FILE, "w");
    if (!wsurl_file) {
      server.send(200, "text/html", "<!DOCTYPE html><html> <body> Setting WebSocket server URL failed!</body> </html>");
      return;
    }
    wsurl_file.print(wsurl);
    wsurl_file.close();

    server.send(200, "text/html", String() + 
    "<!DOCTYPE html><html> <body>" +
    "Setup was successful! <br />" +
    "<br />SSID was set to \"" + user_ssid + "\" with the password \"" + user_password + "\". <br />" +
    "<br />Hostname was set to \"" + host + "\". <br />" +
    "<br />WebSocket server URL was set to \"" + wsurl + "\". <br />" +
    "<br />The controller will now reboot. Please re-connect to your Wi-Fi network.<br />" +
    "If the SSID or password was incorrect, the controller will return to Access Point mode." +
    "</body> </html>");

    delay(1000);
    ESP.reset();
  }
}

void rebootEvent() {
  server.send(200, "text/html", String() + 
  "<!DOCTYPE html><html> <body>" +
  "The controller will now reboot.<br />" +
  "If the SSID or password is set but is incorrect, the controller will return to Access Point mode." +
  "</body> </html>");
  ESP.reset();
}

void serialEvent() {
  while (Serial.available() > 0) {
    char in = Serial.read();
    // there is no proper utf-8 support so replace all non-ascii
    // characters (<127) with underscores; this should have no
    // impact on normal operations and is only relevant for non-english
    // plain-text error messages
    if (in > 127) {
      in = '_';
    }
    serialBuffer[bufferSize] = in;
    bufferSize++;
    // fill up the serial buffer until its max size (8192 bytes, see MAX_BUFFER)
    // or unitl the end of file marker (ctrl-z; \x1A) is reached
    // a worst caste lidar result should be just under 8k, so that MAX_BUFFER
    // limit should not be reached under normal conditions
    if (bufferSize > MAX_BUFFER - 1 || in == '\x1A') {
      serialBuffer[bufferSize] = '\0';
      bool allSend = false;
      uint8_t localBuffer[1464];
      int localNum = 0;
      int bufferNum = 0;
      while (!allSend) {
        localBuffer[localNum] = serialBuffer[bufferNum];
        localNum++;
        // split the websocket packet in smaller (1300 + x) byte packets
        // this is a workaround for some issue that causes data corruption
        // if the payload is split by the wifi library into more than 2 tcp packets
        if (serialBuffer[bufferNum] == '\x1A' || (serialBuffer[bufferNum] == '\n' && localNum > 1300)) {
          localBuffer[localNum] = '\0';
          localNum = 0;
          webSocket.sendTXT(currentClient, localBuffer);
        }
        if (serialBuffer[bufferNum] == '\x1A') {
          allSend = true;
        }
        bufferNum++;
      }
      serialBuffer[0] = '\0';
      bufferSize = 0;
    }
  }
}

void setup(void) {
  Serial.begin(115200);

  //try to mount the filesystem. if that fails, format the filesystem and try again.
  if(!SPIFFS.begin()) {
    SPIFFS.format();
    SPIFFS.begin();
  }

  serialNumber = getSerial();

  if(SPIFFS.exists(SSID_FILE) && SPIFFS.exists(PASSWORD_FILE)) {
    File ssid_file = SPIFFS.open(SSID_FILE, "r");
    char ssid[256];
    ssid_file.readString().toCharArray(ssid, 256);
    ssid_file.close();
    File passwd_file = SPIFFS.open(PASSWORD_FILE, "r");
    char passwd[256];
    passwd_file.readString().toCharArray(passwd, 256);
    passwd_file.close();

    if(SPIFFS.exists(HOSTNAME_FILE)) {
      File hostname_file = SPIFFS.open(HOSTNAME_FILE, "r");
      HOSTNAME = hostname_file.readString();
      hostname_file.close();
    }

    // attempt station connection
    WiFi.disconnect();
    WiFi.mode(WIFI_STA);
    WiFi.hostname(HOSTNAME);
    WiFi.begin(ssid, passwd);
    for(int i = 0; i < CONNECT_TIMEOUT_SECS * 20 && WiFi.status() != WL_CONNECTED; i++) {
      delay(50);
    }
  }

  //start AP mode if either the AP / password do not exist, or cannot be connected to within CONNECT_TIMEOUT_SECS seconds.
  if(WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    if(! WiFi.softAP(AP_SSID)) {
      ESP.reset(); //reset because there's no good reason for setting up an AP to fail
    }
  }

  // start webserver
  server.on("/console", serverEvent);
  server.on("/", HTTP_POST, saveEvent);
  server.on("/", HTTP_GET, setupEvent);
  server.on("/reboot", HTTP_GET, rebootEvent);
  server.onNotFound(serverEvent);

  if (!MDNS.begin(HOSTNAME)) {
    while (1) {
      delay(1000);
    }
  }

  // Start TCP (HTTP) server
  server.begin();

  // start websocket
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);

  //OTA update hooks
  ArduinoOTA.onStart([]() {
    SPIFFS.end();
    webSocket.sendTXT(currentClient, "ESP-12x: OTA Update Starting\n");
  });

  ArduinoOTA.onEnd([]() {
    SPIFFS.begin();
    webSocket.sendTXT(currentClient, "ESP-12x: OTA Update Complete\n");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    webSocket.sendTXT(currentClient, "ESP-12x: OTA Progress: %u%%\r", (progress / (total / 100)));
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("ESP-12x: OTA Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("ESP-12x: OTA Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("ESP-12x: OTA Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("ESP-12x: OTA Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("ESP-12x: OTA End Failed");
  });

  ArduinoOTA.begin();
  httpUpdater.setup(&updateServer);
  updateServer.begin();

  // Add service to MDNS-SD
  MDNS.addService("http", "tcp", 80);
  MDNS.addService("ws", "tcp", 81);
  MDNS.addService("http", "tcp", 82);

  webSocket.sendTXT(currentClient, "ESP-12x: Ready\n");

  if(SPIFFS.exists(WSURL_FILE)) {
    File wsurl_file = SPIFFS.open(WSURL_FILE, "r");
    String wsurl = wsurl_file.readString();
    wsurl_file.close();

    int pos_host = 5;
    int pos_port = wsurl.indexOf(":", pos_host);
    int pos_path = wsurl.indexOf("/", pos_host);

    String wshost = wsurl.substring(pos_host, pos_port == -1 ? pos_path : pos_port);
    int wsport = pos_port == -1 ? 80 : wsurl.substring(pos_port+1, pos_path).toInt();
    String wspath = wsurl.substring(pos_path);

    // Handshake with the server
    webSocketClient.begin(wshost, wsport, wspath);
    webSocketClient.setReconnectInterval(5000);
  }
}

void loop(void) {
  checkServer.check();
  webSocket.loop();
  webSocketClient.loop();

  checkServer.check();
  server.handleClient();

  checkServer.check();
  ArduinoOTA.handle();

  checkServer.check();
  updateServer.handleClient();
  checkServer.check();
  serialEvent();
  MDNS.update();
}

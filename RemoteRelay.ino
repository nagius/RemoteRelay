/*************************************************************************
 *
 * This file is part of the Remoterelay Arduino sketch.
 * Copyleft 2018 Nicolas Agius <nicolas.agius@lps-it.fr>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * ***********************************************************************/


#include <ESP8266WiFi.h>         
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>         // See https://github.com/tzapu/WiFiManager for documentation
#include <EEPROM.h>
#include "Logger.h"

// Default value
#define DEFAULT_LOGIN ""        // AuthBasic credentials
#define DEFAULT_PASSWORD ""     // (default no auth)

// Internal constant
#define AUTHBASIC_LEN 21        // Login or password 20 char max
#define BUF_SIZE 256            // Used for string buffers
#define VERSION "1.1"
#define MODE_ON 1               // See LC-Relay board datasheet for open/close values
#define MODE_OFF 0

struct ST_SETTINGS {
  bool debug;
  bool serial;
  char login[AUTHBASIC_LEN];
  char password[AUTHBASIC_LEN];
};

struct ST_SETTINGS_FLAGS {
  bool debug;
  bool serial;
  bool login;
  bool password;
};

// Global variables
ESP8266WebServer server(80);
Logger logger = Logger();
ST_SETTINGS settings;
uint8_t channels[] = { MODE_OFF, MODE_OFF };
bool shouldSaveConfig = false;    // Flag for WifiManager custom parameters
char buffer[BUF_SIZE];            // Global char* to avoir multiple String concatenation which causes RAM fragmentation

/**
 * HTTP route handlers
 ********************************************************************************/

/**
 * GET /
 */
void handleGETRoot() 
{
  // I always loved this HTTP code
  server.send(418, F("text/plain"), F("\
            _           \r\n\
         _,(_)._            \r\n\
    ___,(_______).          \r\n\
  ,'__.           \\    /\\_  \r\n\
 /,' /             \\  /  /  \r\n\
| | |              |,'  /   \r\n\
 \\`.|                  /    \r\n\
  `. :           :    /     \r\n\
    `.            :.,'      \r\n\
      `-.________,-'        \r\n\
  \r\n"));
}

/**
 * GET /debug
 */
void handleGETDebug()
{
  if(!isAuthBasicOK())
    return;
 
  server.send(200, F("text/plain"), logger.getLog());
}

/**
 * GET /settings
 */
void handleGETSettings()
{
  if(!isAuthBasicOK())
    return;
 
  server.send(200, F("application/json"), getJSONSettings());
}

/**
 * POST /settings
 * Args :
 *   - debug = <bool>
 *   - login = <str>
 *   - password = <str>
 */
void handlePOSTSettings()
{
  ST_SETTINGS st;
  ST_SETTINGS_FLAGS isNew = { false, false, false, false };

  if(!isAuthBasicOK())
    return;

   // Check if args have been supplied
  if(server.args() == 0)
  {
    server.send(400, F("test/plain"), F("Invalid parameters\r\n"));
    return;
  }

  // Parse args   
  for(uint8_t i=0; i<server.args(); i++ ) 
  {
    String param = server.argName(i);
    if(param == "debug")
    {
      st.debug = server.arg(i).equalsIgnoreCase("true");
      isNew.debug = true;
    }
    else if(param == "serial")
    {
      st.serial = server.arg(i).equalsIgnoreCase("true");
      isNew.serial = true;
    }
    else if(param == "login")
    {
      server.arg(i).toCharArray(st.login, AUTHBASIC_LEN);
      isNew.login = true;
    }
    else if(param == "password")
    {
      server.arg(i).toCharArray(st.password, AUTHBASIC_LEN);
      isNew.password = true;
    }
    else
    {
      server.send(400, F("text/plain"), "Unknown parameter: " + param + "\r\n");
      return;
    }
  }

  // Save changes
  if(isNew.debug)
  {
    settings.debug = st.debug;
    logger.setDebug(st.debug);
    logger.info("Updated debug to %s.", st.debug ? "true" : "false");
  }

  if(isNew.serial)
  {
    settings.serial = st.serial;
    logger.setSerial(st.serial);
    logger.info("Updated serial to %s.", st.serial ? "true" : "false");
  }

  if(isNew.login)
  {
    strcpy(settings.login, st.login);
    logger.info("Updated login to \"%s\".", st.login);
  }

  if(isNew.password)
  {
    strcpy(settings.password, st.password);
    logger.info("Updated password.");
  }

  saveSettings();

  // Reply with current settings
  server.send(201, F("application/json"), getJSONSettings());
}

/**
 * POST /reset
 */
void handlePOSTReset()
{
  WiFiManager wifiManager;
  
  if(!isAuthBasicOK())
    return;

  logger.info("Reset settings to default");
    
  //reset saved settings
  wifiManager.resetSettings();
  setDefaultSettings();
  saveSettings();

  // Send response now
  server.send(200, F("text/plain"), F("Reset OK"));
  
  delay(3000);
  logger.info("Restarting...");
    
  ESP.restart();
}

/**
 * PUT /channel/:id
 * Args :
 *   - mode = "<on|off>"
 */
void handlePUTChannel(uint8_t channel)
{
  uint8_t requestedMode = MODE_OFF; // Default in case of error
  
  if(!isAuthBasicOK())
    return;

  // Check if args have been supplied
  if(server.args() != 1)
  {
    server.send(400, F("test/plain"), F("Invalid parameter\r\n"));
    return;
  }

  // Check if requested arg has been suplied
  if(server.argName(0) != "mode")
  {
    server.send(400, F("text/plain"), F("Invalid parameter\r\n"));
    return;
  } 

  String value = server.arg(0);
  if(value.equalsIgnoreCase("on"))
  {
    requestedMode = MODE_ON;
  }
  else if(value.equalsIgnoreCase("off"))
  {
    requestedMode = MODE_OFF;
  }
  else
  {
    server.send(400, F("text/plain"), "Invalid value: " + value + "\r\n");
    return;
  } 

  // Give some time to the watchdog
  ESP.wdtFeed();
  yield();

  setChannel(channel, requestedMode);
  server.send(200, F("application/json"), getJSONState(channel));
}

/**
 * GET /channel/:id
 */
void handleGETChannel(uint8_t channel)
{
  if(!isAuthBasicOK())
    return;

  server.send(200, F("application/json"), getJSONState(channel));
}

/**
 * WEB helpers 
 ********************************************************************************/

bool isAuthBasicOK()
{
  // Disable auth if not credential provided
  if(strlen(settings.login) > 0 && strlen(settings.password) > 0)
  {
    if(!server.authenticate(settings.login, settings.password))
    {
      server.requestAuthentication();
      return false;
    }
  }
  return true;
}

char* getJSONSettings()
{
  //Generate JSON 
  snprintf(buffer, BUF_SIZE, "{ \"login\": \"%s\", \"password\": \"<hidden>\", \"debug\": %s, \"serial\": %s }\r\n",
    settings.login,
    settings.debug ? "true" : "false",
    settings.serial ? "true" : "false"
  );

  return buffer;
}

char* getJSONState(uint8_t channel)
{
  //Generate JSON 
  snprintf(buffer, BUF_SIZE, "{ \"channel\": \"%d\", \"mode\": \"%s\" }\r\n",
    channel,
    channels[channel - 1] == MODE_ON ? "on" : "off"
  );

  return buffer;
}


/**
 * Flash memory helpers 
 ********************************************************************************/

// CRC8 simple calculation
// Based on https://github.com/PaulStoffregen/OneWire/blob/master/OneWire.cpp
uint8_t crc8(const uint8_t *addr, uint8_t len)
{
  uint8_t crc = 0;

  while (len--) {
    uint8_t inbyte = *addr++;
    for (uint8_t i = 8; i; i--) {
      uint8_t mix = (crc ^ inbyte) & 0x01;
      crc >>= 1;
      if (mix) crc ^= 0x8C;
      inbyte >>= 1;
    }
  }
  return crc;
}

void saveSettings()
{
  uint8_t buffer[sizeof(settings) + 1];  // Use the last byte for CRC

  memcpy(buffer, &settings, sizeof(settings));
  buffer[sizeof(settings)] = crc8(buffer, sizeof(settings));

  for(int i=0; i < sizeof(buffer); i++)
  {
    EEPROM.write(i, buffer[i]);
  }
  EEPROM.commit();
}

void loadSettings()
{
  uint8_t buffer[sizeof(settings) + 1];  // Use the last byte for CRC

  for(int i=0; i < sizeof(buffer); i++)
  {
    buffer[i] = uint8_t(EEPROM.read(i));
  }

  // Check CRC
  if(crc8(buffer, sizeof(settings)) == buffer[sizeof(settings)])
  {
    memcpy(&settings, buffer, sizeof(settings));
    logger.setDebug(settings.debug);
    logger.setSerial(settings.serial);
    logger.info("Loaded settings from flash");

    // Display loaded setting on debug
    logger.debug("FLASH: %s", getJSONSettings());
  }
  else
  {
    logger.info("Bad CRC, loading default settings.");
    setDefaultSettings();
    saveSettings();
  }
}

void setDefaultSettings()
{
    strcpy(settings.login, DEFAULT_LOGIN);
    strcpy(settings.password, DEFAULT_PASSWORD);
    settings.debug = false;
    settings.serial = false;
}


/**
 * General helpers 
 ********************************************************************************/

void setChannel(uint8_t channel, uint8_t mode)
{
  byte payload[4];

  // Header
  payload[0] = 0xA0;

  // Select the channel
  payload[1] = channel;

  // Set the mode
  //  * 0 = open (off)
  //  * 1 = close (on)
  payload[2] = mode;

  // Compute checksum
  payload[3] = payload[0] + payload[1] + payload[2];

  // Save status 
  channels[channel - 1] = mode;
  
  logger.info("Channel %i switched to %s", channel, (mode == MODE_ON) ? "on" : "off");
  logger.debug("Sending payload %02X%02X%02X%02X", payload[0], payload[1], payload[2], payload[3]);

  // Give some time to the watchdog
  ESP.wdtFeed();
  yield();

  // Send hex payload
  Serial.write(payload, sizeof(payload));

  if(settings.serial)
    Serial.println(""); // Clear the line for log output
}

void setup() 
{
  WiFiManager wifiManager;
  
  Serial.begin(115200);
  EEPROM.begin(512);
  logger.info("RemoteRelay version %s started.", VERSION);
  
  // Load settigns from flash
  loadSettings();

  // Be sure the relay are in the default state (off)
  for(uint8_t i=0; i<=1; i++)
  {
    setChannel(i+1, channels[i]);
  }

  // Configure custom parameters
  WiFiManagerParameter http_login("htlogin", "HTTP Login", settings.login, AUTHBASIC_LEN);
  WiFiManagerParameter http_password("htpassword", "HTTP Password", settings.password, AUTHBASIC_LEN, "type='password'");
  wifiManager.setSaveConfigCallback([](){
    shouldSaveConfig = true;
  });
  wifiManager.addParameter(&http_login);
  wifiManager.addParameter(&http_password);
  
  // Connect to Wifi or ask for SSID
  wifiManager.autoConnect("RemoteRelay");

  // Save new configuration set by captive portal
  if(shouldSaveConfig)
  {
    strncpy(settings.login, http_login.getValue(), AUTHBASIC_LEN);
    strncpy(settings.password, http_password.getValue(), AUTHBASIC_LEN);

    logger.info("Saving new config from portal web page");
    saveSettings();
  }

  // Display local ip
  logger.info("Connected. IP address: %s", WiFi.localIP().toString().c_str());
  
  // Setup HTTP handlers
  server.on("/", handleGETRoot );
  server.on("/debug", HTTP_GET, handleGETDebug);
  server.on("/settings", HTTP_GET, handleGETSettings);
  server.on("/settings", HTTP_POST, handlePOSTSettings);
  server.on("/reset", HTTP_POST, handlePOSTReset);
  server.on("/channel/1", HTTP_PUT, std::bind(&handlePUTChannel, 1));
  server.on("/channel/2", HTTP_PUT, std::bind(&handlePUTChannel, 2));
  server.on("/channel/1", HTTP_GET, std::bind(&handleGETChannel, 1));
  server.on("/channel/2", HTTP_GET, std::bind(&handleGETChannel, 2));
  server.onNotFound([]() {
    server.send(404, "text/plain", "Not found\r\n");
  });
  server.begin();
  
  logger.info("HTTP server started.");
}

void fakeATFirmware()
{
  // pretend to be an AT device here
  if(Serial.available())
  {
    String stringIn=Serial.readStringUntil('\r');
    Serial.flush();// flush what's left '\n'?

    if(stringIn!="")
    {
      logger.debug("Serial received: %s", stringIn);

      if(stringIn.indexOf("AT+")>-1)
        Serial.println("OK");

      if(stringIn.indexOf("AT+RST")>-1)
      {
        // pretend we reset (wait a bit then send the WiFi connected message)
        delay(1);
        Serial.println("WIFI CONNECTED\r\nWIFI GOT IP");
      }
    }
  }
}

void loop()
{
    server.handleClient();
    fakeATFirmware();
}

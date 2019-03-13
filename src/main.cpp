/*
*  Version:    2019-01-22 gpmaxx  NHL initial
*              2019-03-05 gpmaxx  mlb/nhl version
               2019-03-10 gpmaxx  OTA updating version
*
*  Desc:       NHL/MLB Scoreboard for Wemos D1 Mini and 128 & 160 TFT_eSPI
*
*  Libraries:  ArduinoJson:  https://github.com/bblanchon/ArduinoJson
*              TFT_eSPI:     https://github.com/Bodmer/TFT_eSPI
*              Bounce2:      https://github.com/thomasfredericks/Bounce2
*
*  Notes:      Pin assignments below are very particular. The D1 is finicky.
*              Changing pin assignments or adding additional connections is likely
*              to cause headaches.  The TFT_eSPI defaults are thus overridden
*              with compile options (see platformio.ini) to use the pins that are
*              most convienient. Using any other board beside there Wemos D1 Mini has never been tested.
*
*              Wemos D1 pin       connect to
*              ------------       ----------
*              RST                optionaly to GND via a button (external reset)
*              A0,D8,TX,RX,5V     nothing
*              D0                 TFT AO
*              D5                 TFT SCK
*              D6                 GND via Switch 1 (used for display mode)
*              D7                 TFT SDA
*              3V                 TFT VCC
*              D1                 GND via button (select button)
*              D2                 TFT CS
*              D3                 TFT RST
*              D4                 TFT LED
*              G                  Ground
*
*              Code assume 128 x 160 TFT display. The graphics functions assume
*              this size and use some hardcoded and magic values to get things
*              looking right. A different size screen the values will have to
*              be changed carefully.
*  ToDo:
*              - more testing of error scenarios
*              - more testing of all game scenarios
*              - testing during playoffs
               - test near end of season
               - test MLB double headers
*
*   Maybe:
               - figure out series record for mlb games - no API options?
               - exploer use of TFT datum for better and cleaner text positions
               - clean up use of global variables
*              - make graphics dynamic for display screens other than 128 x 160
*              - Add boxscore feature
*                  - the available NHL json is massive and goal scoring summary is now easily available
*                      - need better API for boxscore
*              - Figure out how to list playoff series record - not available in NHL API?
*/

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <Bounce2.h>
#include <Time.h>
#include <Timezone.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>

#define FS_NO_GLOBALS
#include "FS.h"
#include "BMP_functions.h"

////////////////// Global Constants //////////////////
const char* WIFI_CONFIG_AP = "ESP_WIFI_CONFIG";

const uint8_t SWITCH_PIN_1 = D6;          // change with caution
const uint8_t LED_BACKLIGHT_PIN = D4;     // change with caution
const uint8_t SELECT_BUTTON_PIN = D1;     // change with caution

const uint8_t DEBOUNCE_INTERVAL = 25;
const uint16_t LONG_PRESS_THRESHOLD = 1000;
const uint8_t TFT_ROTATION = 3;

const char* SPIFFS_DATAFILE = "/myteam.dat";
const char* TIME_HOST = "worldtimeapi.org";
const uint8_t TIME_PORT = 80;
const uint32_t TIME_UPDATE_INTERVAL_MS = 1000 * 60 * 60 * 24; // 24 hours

const uint16_t TFT_HALF_WIDTH = 80;
const uint16_t TFT_HALF_HEIGHT = 64;

const char* MONTHS_OF_YEAR[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
const char* DAYS_OF_WEEK[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
const uint32_t SECONDS_IN_A_DAY = 60 * 60 * 24;
const uint32_t SECONDS_IN_A_WEEK = SECONDS_IN_A_DAY * 7;

const char* NHL_HOST = "statsapi.web.nhl.com";
const uint16_t NHL_PORT = 80;
const uint8_t NHL_NUMTEAM_ICONS = 32;

const char* MLB_HOST = "statsapi.mlb.com";
const uint16_t MLB_PORT = 80;
const uint8_t MLB_NUMTEAM_ICONS = 31;

const uint16_t SPORTID_MARKER = 9999;

// Game status codes. These are dependant on the statsapi
// NHL uses numbers and MLB uses characters
const char* STATUSCODE_PREGAME = "Preview";
const char* STATUSCODE_LIVE = "Live";
const char* STATUSCODE_FINAL = "Final";

const uint32_t AFTER_GAME_RESULTS_DURATION_MS = 60 * 60 * 1 * 1000; // 1 hours
const uint32_t GAME_UPDATE_INTERVAL_NHL_S = 65;  // 65 seconds
const uint32_t GAME_UPDATE_INTERVAL_MLB_S = 60 * 5; // 5 mins
const uint32_t MAX_SLEEP_INTERVAL_S = 60 * 60; // 1 hour

const char* FIRMWARE_URL = "http://www.lipscomb.ca/IOT/firmware/";
const char* PROJECT_NAME = "TFT_SportsScores";
const char* BIN_VERSION_FILENAME = "/firmware_ver.txt";
const char* SPIFFS_VERSION_FILENAME = "/spiffs_ver.txt";
const char* BIN_PREFIX = "/firmware_";
const char* SPIFFS_PREFIX = "/spiffs_";
const char* BIN_EXT = ".bin";
const char* SPIFFS_EXT = ".bin";

// !!!!! Change version for each build !!!!!
const uint16_t CURRENT_BIN_VERSION = 1321;


////////////////// Data Structs ///////////
struct TeamInfo {
  uint16_t id = 0;
  char name[4] = "XXX";
};

struct NextGameData {
  uint8_t awayID = 0;
  uint8_t homeID = 0;
  uint32_t gameID = 0;
  char homeRecord[9];   // xx-xx-xx
  char awayRecord[9];
  bool isPlayoffs = false;
  time_t startTime = 0;
  bool isNHL = true;
};

struct CurrentGameData {
  uint32_t gameID = 0;
  uint8_t awayID = 0;
  uint8_t homeID = 0;
  uint8_t awayScore = 0;
  uint8_t homeScore = 0;
  char period[5];    // 1st, 2nd etc
  char timeRemaining[6];  // 12:34
  bool isNHL = true;
  uint8_t outs = 0;
};

enum GameStatus {SCHEDULED,STARTED,FINISHED,BUTTON_WAIT};

///////////// Global Variables ////////////
String queryString;
String tftString;
uint16_t myNHLTeamID = 24;   // Aniheim         // default - will be overridden by SPIFFS data
uint16_t myMLBTeamID = 109;  // Arizona      // default - will be overridden by SPIFFS data
char friendlyDate[12];             // date buffer format: Mon, Jan 23
char friendlyTime[6];              // time buffer format: 12:34
bool currentSportIsNHL = true;
bool otaSelected = false;

/////////// Global Object Variables ///////////
TimeChangeRule usEDT = {"EDT", Second, Sun, Mar, 2, -240};  //UTC - 4 hours
TimeChangeRule usEST = {"EST", First, Sun, Nov, 2, -300};   //UTC - 5 hours
Timezone tz(usEDT, usEST);
TFT_eSPI tft = TFT_eSPI();
TeamInfo nhlTeams[NHL_NUMTEAM_ICONS];
TeamInfo mlbTeams[MLB_NUMTEAM_ICONS];
Bounce debouncer = Bounce();
WiFiClient client;
WiFiManager wifiManager;
NextGameData nextGameData;
CurrentGameData currentGameData;
GameStatus gameStatus = SCHEDULED;


////////////////  Code //////////////////////
void tftMessage(char* theMessage) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE);
  tft.setCursor(0,0);
  tft.print(theMessage);
}

void tftMessage(String theMessage) {
  char buffer[theMessage.length()+1];
  theMessage.toCharArray(buffer,theMessage.length()+1);
  tftMessage(buffer);
}

void infiniteLoop() {
  Serial.println(F("infinite delay"));
  while (true) {
    delay(0xFFFFFFFF);
  }
}

// populate teams list
void teamListInit_NHL() {

  nhlTeams[0].id = 24;
  strncpy(nhlTeams[0].name,"ANA",3);
  nhlTeams[1].id = 53;
  strncpy(nhlTeams[1].name,"ARI",3);
  nhlTeams[2].id = 6;
  strncpy(nhlTeams[2].name,"BOS",3);
  nhlTeams[3].id = 7;
  strncpy(nhlTeams[3].name,"BUF",3);
  nhlTeams[4].id = 12;
  strncpy(nhlTeams[4].name,"CAR",3);
  nhlTeams[5].id = 29;
  strncpy(nhlTeams[5].name,"CBJ",3);
  nhlTeams[6].id = 20;
  strncpy(nhlTeams[6].name,"CGY",3);
  nhlTeams[7].id = 16;
  strncpy(nhlTeams[7].name,"CHI",3);
  nhlTeams[8].id = 21;
  strncpy(nhlTeams[8].name,"COL",3);
  nhlTeams[9].id = 25;
  strncpy(nhlTeams[9].name,"DAL",3);
  nhlTeams[10].id = 17;
  strncpy(nhlTeams[10].name,"DET",3);
  nhlTeams[11].id = 22;
  strncpy(nhlTeams[11].name,"EDM",3);
  nhlTeams[12].id = 13;
  strncpy(nhlTeams[12].name,"FLA",3);
  nhlTeams[13].id = 26;
  strncpy(nhlTeams[13].name,"LAK",3);
  nhlTeams[14].id = 30;
  strncpy(nhlTeams[14].name,"MIN",3);
  nhlTeams[15].id = 8;
  strncpy(nhlTeams[15].name,"MTL",3);
  nhlTeams[16].id = 1;
  strncpy(nhlTeams[16].name,"NJD",3);
  nhlTeams[17].id = 18;
  strncpy(nhlTeams[17].name,"NSH",3);
  nhlTeams[18].id = 2;
  strncpy(nhlTeams[18].name,"NYI",3);
  nhlTeams[19].id = 3;
  strncpy(nhlTeams[19].name,"NYR",3);
  nhlTeams[20].id = 9;
  strncpy(nhlTeams[20].name,"OTT",3);
  nhlTeams[21].id = 4;
  strncpy(nhlTeams[21].name,"PHI",3);
  nhlTeams[22].id = 5;
  strncpy(nhlTeams[22].name,"PIT",3);
  nhlTeams[23].id = 28;
  strncpy(nhlTeams[23].name,"SJS",3);
  nhlTeams[24].id = 19;
  strncpy(nhlTeams[24].name,"STL",3);
  nhlTeams[25].id = 14;
  strncpy(nhlTeams[25].name,"TBL",3);
  nhlTeams[26].id = 10;
  strncpy(nhlTeams[26].name,"TOR",3);
  nhlTeams[27].id = 23;
  strncpy(nhlTeams[27].name,"VAN",3);
  nhlTeams[28].id = 54;
  strncpy(nhlTeams[28].name,"VGK",3);
  nhlTeams[29].id = 52;
  strncpy(nhlTeams[29].name,"WPG",3);
  nhlTeams[30].id = 15;
  strncpy(nhlTeams[30].name,"WSH",3);
  nhlTeams[31].id = SPORTID_MARKER;
  strncpy(nhlTeams[31].name,"MLB",3);
}

// populate teams list
void teamListInit_MLB() {
  mlbTeams[0].id = 109;
  strncpy(mlbTeams[0].name,"ARI",3);
  mlbTeams[1].id = 144;
  strncpy(mlbTeams[1].name,"ATL",3);
  mlbTeams[2].id = 110;
  strncpy(mlbTeams[2].name,"BAL",3);
  mlbTeams[3].id = 111;
  strncpy(mlbTeams[3].name,"BOS",3);
  mlbTeams[4].id = 112;
  strncpy(mlbTeams[4].name,"CHC",3);
  mlbTeams[5].id = 145;
  strncpy(mlbTeams[5].name,"CWS",3);
  mlbTeams[6].id = 113;
  strncpy(mlbTeams[6].name,"CIN",3);
  mlbTeams[7].id = 114;
  strncpy(mlbTeams[7].name,"CLE",3);
  mlbTeams[8].id = 115;
  strncpy(mlbTeams[8].name,"COL",3);
  mlbTeams[9].id = 116;
  strncpy(mlbTeams[9].name,"DET",3);
  mlbTeams[10].id = 17;
  strncpy(mlbTeams[10].name,"HOU",3);
  mlbTeams[11].id = 118;
  strncpy(mlbTeams[11].name,"KCR",3);
  mlbTeams[12].id = 108;
  strncpy(mlbTeams[12].name,"LAA",3);
  mlbTeams[13].id = 119;
  strncpy(mlbTeams[13].name,"LAD",3);
  mlbTeams[14].id = 146;
  strncpy(mlbTeams[14].name,"MIA",3);
  mlbTeams[15].id = 158;
  strncpy(mlbTeams[15].name,"MIL",3);
  mlbTeams[16].id = 142;
  strncpy(mlbTeams[16].name,"MIN",3);
  mlbTeams[17].id = 121;
  strncpy(mlbTeams[17].name,"NYM",3);
  mlbTeams[18].id = 147;
  strncpy(mlbTeams[18].name,"NYY",3);
  mlbTeams[19].id = 133;
  strncpy(mlbTeams[19].name,"OAK",3);
  mlbTeams[20].id = 143;
  strncpy(mlbTeams[20].name,"PHI",3);
  mlbTeams[21].id = 134;
  strncpy(mlbTeams[21].name,"PIT",3);
  mlbTeams[22].id = 135;
  strncpy(mlbTeams[22].name,"SDP",3);
  mlbTeams[23].id = 137;
  strncpy(mlbTeams[23].name,"SFG",3);
  mlbTeams[24].id = 136;
  strncpy(mlbTeams[24].name,"SEA",3);
  mlbTeams[25].id = 138;
  strncpy(mlbTeams[25].name,"STL",3);
  mlbTeams[26].id = 139;
  strncpy(mlbTeams[26].name,"TBR",3);
  mlbTeams[27].id = 140;
  strncpy(mlbTeams[27].name,"TEX",3);
  mlbTeams[28].id = 141;
  strncpy(mlbTeams[28].name,"TOR",3);
  mlbTeams[29].id = 120;
  strncpy(mlbTeams[29].name,"WSH",3);
  mlbTeams[30].id = SPORTID_MARKER;
  strncpy(mlbTeams[30].name,"NHL",3);
}

void teamListInit() {
  teamListInit_NHL();
  teamListInit_MLB();
}



char* getTeamAbbreviation(const uint16_t teamID, const bool isNHLTeam) {

  if (isNHLTeam) {
    for (uint8_t i = 0; i < NHL_NUMTEAM_ICONS; i++) {
      if (nhlTeams[i].id == teamID) {
        return nhlTeams[i].name;
      }
    }
  }
  else {
    for (uint8_t i = 0; i < MLB_NUMTEAM_ICONS; i++) {
      if (mlbTeams[i].id == teamID) {
        return mlbTeams[i].name;
      }
    }
  }

  return "ERR";
}

void displaySingleLogo(const uint16_t teamID, const bool isNHL) {

  char filePath[19];
  char* sportPath;

  if (isNHL) {
    sportPath = "NHL/";
  }
  else {
    sportPath = "MLB/";
  }

  char* teamName = getTeamAbbreviation(teamID,isNHL);
  sprintf(filePath,"%s%s%s%s","/icons/",sportPath,teamName,".bmp");

  drawBmp(&tft,filePath,TFT_HALF_WIDTH-25,TFT_HALF_HEIGHT-25);
}

void displayTeamLogos(const uint8_t awayID, const uint8_t homeID, const bool isNHL) {

  char filePath[19];
  char* sportPath;

  if (isNHL) {
    sportPath = "NHL/";
  }
  else {
    sportPath = "MLB/";
  }

  sprintf(filePath,"%s%s%s%s","/icons/",sportPath,getTeamAbbreviation(awayID,isNHL),".bmp");
  drawBmp(&tft,filePath,10,10);

  sprintf(filePath,"%s%s%s%s","/icons/",sportPath,getTeamAbbreviation(homeID,isNHL),".bmp");
  drawBmp(&tft,filePath,100,10);

}



void loadTeams() {
  Serial.println(F("Loading data"));
  fs::File file = SPIFFS.open(SPIFFS_DATAFILE,"r");

  if (!file) {
    Serial.println(F("My Team config file error"));
    tftMessage("My team config error");
  }

  myNHLTeamID = file.parseInt();

  char* teamName = getTeamAbbreviation(myNHLTeamID,true);
  if (strcmp(teamName,"ERR") == 0) {
    Serial.print(F("Error NHL team not found: "));
    Serial.println(myNHLTeamID);
    tftMessage("Team error");
    infiniteLoop();
  }
  else {
    Serial.printf("NHL Team: %s (%d)\r\n",teamName,myNHLTeamID);
  }

  myMLBTeamID = file.parseInt();
  teamName = getTeamAbbreviation(myMLBTeamID,false);
  if (strcmp(teamName,"ERR") == 0) {
    Serial.print(F("Error MLB team not found: "));
    Serial.println(myMLBTeamID);
    tftMessage("Team error");
    infiniteLoop();
  }
  else {
    Serial.printf("MLB Team: %s (%d)\r\n",teamName,myMLBTeamID);
  }

  currentSportIsNHL = (bool)file.parseInt();
  Serial.printf("Display mode: %s\r\n",(currentSportIsNHL) ? "NHL" : "MLB");

  file.close();

}

uint16_t selectMLBTeam() {

  uint8_t teamIndex = 0;
  bool teamSelected = false;
  bool switchTeams = true;
  uint32_t buttonTimer = 0;
  bool alreadyFell = false;

  // get our current team's index
  for (uint8_t i = 0; i < MLB_NUMTEAM_ICONS; i++) {
    if (mlbTeams[i].id == myMLBTeamID) {
      teamIndex = i;
      break;
    }
  }

  tft.fillScreen(TFT_WHITE);
  while (!teamSelected) {
    if (switchTeams) {
      displaySingleLogo(mlbTeams[teamIndex].id,false);
      switchTeams = false;
    }
    debouncer.update();
    if (debouncer.fell()) {
      buttonTimer = millis();
      alreadyFell = true;
    }
    if (debouncer.rose() && alreadyFell) {
      if ((millis() - buttonTimer) > LONG_PRESS_THRESHOLD) {
        return mlbTeams[teamIndex].id;
      }
      else {
        switchTeams = true;
        teamIndex = (teamIndex + 1) % MLB_NUMTEAM_ICONS;
      }
    }
    yield();
  }
}

uint16_t selectNHLTeam() {

  uint8_t teamIndex = 0;
  bool teamSelected = false;
  bool switchTeams = true;
  uint32_t buttonTimer = 0;
  bool alreadyFell = false;

  // get our current team's index
  for (uint8_t i = 0; i < NHL_NUMTEAM_ICONS; i++) {
    if (nhlTeams[i].id == myNHLTeamID) {
      teamIndex = i;
      break;
    }
  }

  tft.fillScreen(TFT_WHITE);
  while (!teamSelected) {
    if (switchTeams) {
      displaySingleLogo(nhlTeams[teamIndex].id,true);
      switchTeams = false;
    }
    debouncer.update();
    if (debouncer.fell()) {
      buttonTimer = millis();
      alreadyFell = true;
    }
    if (debouncer.rose() && alreadyFell) {
      if ((millis() - buttonTimer) > LONG_PRESS_THRESHOLD) {
        return nhlTeams[teamIndex].id;
      }
      else {
        switchTeams = true;
        teamIndex = (teamIndex + 1) % NHL_NUMTEAM_ICONS;
      }
    }
    yield();
  }
}


void selectMenu() {

  uint16_t selectedTeamID = 0;

  while (true) {

    if (currentSportIsNHL) {
        selectedTeamID = selectNHLTeam();
        if (selectedTeamID == SPORTID_MARKER) {
          currentSportIsNHL = false;
        }
        else {
          myNHLTeamID = selectedTeamID;
          break;
        }
    }
    else {
      selectedTeamID = selectMLBTeam();
      if (selectedTeamID == SPORTID_MARKER) {
        currentSportIsNHL = true;
      }
      else {
          myMLBTeamID = selectedTeamID;
          break;
      }
    }

  }

}





// output out favourite team to SPIFFs
void saveTeams() {
  fs::File file = SPIFFS.open(SPIFFS_DATAFILE,"w");
  if (!file) {
    Serial.println(F("Error opening myTeam file for writing"));
  }
  file.println(myNHLTeamID);
  file.println(myMLBTeamID);
  file.println(currentSportIsNHL);
  file.close();

}

// String of the time expected in format YYYY-MM-DD HH-mm-ss GMT
time_t parseDateTime(const String timeStr) {

  tmElements_t tmSet;
  tmSet.Year = timeStr.substring(0,4).toInt() - 1970;
  tmSet.Month = timeStr.substring(5,7).toInt();
  tmSet.Day = timeStr.substring(8,10).toInt();
  tmSet.Hour = timeStr.substring(11,13).toInt();
  tmSet.Minute = timeStr.substring(14,16).toInt();
  tmSet.Second = timeStr.substring(17,19).toInt();
  time_t currentGMT = makeTime(tmSet);

  time_t currentLocal = tz.toLocal(currentGMT);

  return currentLocal;

}

// convert epoch time to YYYY-MM-DD format
String convertDate(const time_t epoch) {
  char dateChar[11];
  snprintf(dateChar,sizeof(dateChar),"%04d-%02d-%02d",year(epoch),month(epoch),day(epoch));
  String dateString = dateChar;
  return dateString;
}

void printDate(const time_t theTime) {
  Serial.println(convertDate(theTime));
}

void printTime(const time_t theTime) {
  Serial.printf("%02d:%02d:%02d\r\n",hour(theTime),minute(theTime),second(theTime));
}

void printDate() {
  printDate(now());
}

void printTime() {
  printTime(now());
}

//  Format:  Mon, Jan 23
void getFriendlyDate(char* formattedDate, const uint8_t buffSize, const time_t epoch) {
  String convertedDate = DAYS_OF_WEEK[weekday(epoch)-1];
  convertedDate += ", ";
  convertedDate += MONTHS_OF_YEAR[month(epoch)-1];
  convertedDate += " ";
  convertedDate += day(epoch);
  convertedDate.toCharArray(formattedDate,buffSize);
}

void getFriendlyTime(char* formattedTime, const uint8_t buffSize, const time_t epoch) {
  snprintf(formattedTime,buffSize,"%d:%02d",hour(epoch),minute(epoch));
}

void updateTime() {
  static uint32_t lastTimeUpdate = 0;

  if ((lastTimeUpdate == 0) || ((millis() - lastTimeUpdate) > TIME_UPDATE_INTERVAL_MS)) {
    client.setTimeout(10000);
    if (client.connect(TIME_HOST,TIME_PORT)) {
      queryString = "GET /api/ip HTTP/1.0";
      client.println(queryString);
      queryString = "Host: ";
      queryString += TIME_HOST;
      client.println(queryString);
      client.println(F("Connection: close"));
      if (client.println() == 0) {
        Serial.println(F("Failed to send request"));
      }

    }
    else {
      Serial.println(F("connection failed")); //error message if no client connect
      Serial.println();
    }

    while(client.connected() && !client.available()) delay(1);

    char endOfHeaders[] = "\r\n\r\n";
    if (!client.find(endOfHeaders)) {
      Serial.println(F("Invalid response"));
      return;
    }

    client.stop();

    DynamicJsonDocument doc(500);
    DeserializationError err = deserializeJson(doc,client);

    if (err) {
      Serial.print(F("Parsing error: "));
      Serial.println(err.c_str());
    }

    time_t epochTime = doc["unixtime"];

    setTime(tz.toLocal(epochTime));
    lastTimeUpdate = millis();

    getFriendlyDate(friendlyDate,sizeof(friendlyDate),now());
    getFriendlyTime(friendlyTime,sizeof(friendlyTime),now());

    Serial.print(F("Local Date: "));
    Serial.println(friendlyDate);
    Serial.print(F("Local time: "));
    Serial.println(friendlyTime);
  }
}

// sync system time with internet time
void updateTime_old() {

  static uint32_t lastTimeUpdate = 0;
  String timeStr = "";
  bool fetchedTime = false;
  uint32_t t;

  // only bother to sync once per interval
  if ((lastTimeUpdate == 0) || ((millis() - lastTimeUpdate) > TIME_UPDATE_INTERVAL_MS)) {

    while (timeStr.equals("")) {
      Serial.println(F("Fetching time from NIST"));
      if (!client.connect(TIME_HOST,TIME_PORT)) {
        Serial.println(F("connection failed"));
        tftMessage("Connection failed");
        delay(1000);
        ESP.restart();
      }
      // This will send the request to the server
      t = millis();
      client.print("HEAD / HTTP/1.1\r\nAccept: */*\r\nUser-Agent: Mozilla/4.0 (compatible; ESP8266 NodeMcu Lua;)");
      client.print(F("Connection: close\r\n"));
      Serial.printf("%d: %d\r\n",1,millis()-t);
      while(client.connected() && !client.available()) delay(1);
      Serial.printf("%d: %d\r\n",2,millis()-t);
      if (client.available()) {
        Serial.printf("%d: %d\r\n",3,millis()-t);
        timeStr += client.readStringUntil('\r');
        Serial.printf("%d: %d\r\n",4,millis()-t);
      }
      Serial.printf("%d: %d\r\n",5,millis()-t);
    }
    Serial.printf("%d: %d\r\n",6,millis()-t);
    client.stop();
    timeStr = timeStr.substring(7);
    timeStr = "20" + timeStr;

    setTime(parseDateTime(timeStr));

    lastTimeUpdate = millis();
    fetchedTime = true;

  }

}

void wifiConfigCallback(WiFiManager* myWiFiManager) {
  Serial.println(F("Entered WiFi Config Mode"));
  Serial.println(WiFi.softAPIP());
  Serial.println(myWiFiManager->getConfigPortalSSID());
  tftMessage(myWiFiManager->getConfigPortalSSID());
}

void wifiConnect() {

  Serial.println(F("Connecting to WiFi..."));
  if (!wifiManager.autoConnect(WIFI_CONFIG_AP)) {
    Serial.println(F("failed to connect timout"));
    tftMessage("WiFi connect timeout");
    delay(10000);
    ESP.reset();
    delay(10000);
  }

  Serial.println(F("WiFi Connected"));

}

void printNextGame(NextGameData* nextGame) {
  Serial.println(F("-----------------"));
  Serial.printf("Next GameID: %d (%s)\r\n",nextGame->gameID,(nextGame->isNHL) ? "NHL" : "MLB");
  Serial.printf("awayID: %d (%s)\r\n",nextGame->awayID,getTeamAbbreviation(nextGame->awayID,nextGame->isNHL));
  Serial.printf("homeID: %d (%s)\r\n",nextGame->homeID,getTeamAbbreviation(nextGame->homeID,nextGame->isNHL));

  Serial.print(F("StartTime: "));
  Serial.println(nextGame->startTime);
  Serial.print(F("Away record: "));
  Serial.println(nextGame->awayRecord);
  Serial.print(F("Home record: "));
  Serial.println(nextGame->homeRecord);

  Serial.print(F("Is Playoffs: "));
  Serial.println(nextGame->isPlayoffs ? "Yes" : "No");
  Serial.println(F("-----------------"));
}

void printCurrentGame (CurrentGameData* currentGame) {
  Serial.println(F("-----------------"));
  Serial.printf("Current GameID: %d (%s)\r\n",currentGame->gameID,(currentGame->isNHL) ? "NHL" : "MLB");
  Serial.printf("awayID: %d (%s)\r\n",currentGame->awayID,getTeamAbbreviation(currentGame->awayID,currentGame->isNHL));
  Serial.printf("homeID: %d (%s)\r\n",currentGame->homeID,getTeamAbbreviation(currentGame->homeID,currentGame->isNHL));
  Serial.printf("Away score: %d\r\n",currentGame->awayScore);
  Serial.printf("Home score: %d\r\n",currentGame->homeScore);
  Serial.print((currentGame->isNHL) ? "Period: " : "Inning: ");
  Serial.println(currentGame->period);
  Serial.print(F("Time: "));
  Serial.println(currentGame->timeRemaining);
  Serial.print(F("Outs: "));
  if (currentGame->isNHL) {
    Serial.println(F("NA"));
  }
  else {
    Serial.println(currentGame->outs);
  }
  Serial.println(F("-----------------"));
}

void getNextGame(const time_t today,const uint16_t teamID, const bool isNHLGame, NextGameData* nextGameData) {

  Serial.println(F("Geting next game data"));
  uint32_t excludeGameID = nextGameData->gameID;

  char* host;
  uint16_t port;

  if (isNHLGame) {
    host = (char*)NHL_HOST;
    port = NHL_PORT;
  }
  else {
    host = (char*)MLB_HOST;
    port = MLB_PORT;
  }

  Serial.printf("Host: %s\r\n",host);
  client.setTimeout(10000);
  if (client.connect(host,port)) {
    queryString = "GET /api/v1/schedule?";
    queryString += "sportId=1";
    queryString += "&teamId=";
    queryString += teamID;
    queryString += "&startDate=";
    queryString += convertDate(today - SECONDS_IN_A_DAY); // need to grab from yesterday
    queryString += "&endDate=";
    // get 10 days worth of data to in order to cover the all star break and playoff gaps
    queryString += convertDate(today + (SECONDS_IN_A_DAY * 7));
    Serial.print(F("queryString: "));
    Serial.println(queryString);
    queryString += " HTTP/1.0";
    client.println(queryString);
    queryString = "Host: ";
    queryString += host;
    client.println(queryString);
    client.println(F("Connection: close"));
    if (client.println() == 0) {
      Serial.println(F("Failed to send request"));
    }

  }
  else {
    Serial.println(F("connection failed")); //error message if no client connect
    Serial.println();
  }

  while(client.connected() && !client.available()) delay(1);

  char endOfHeaders[] = "\r\n\r\n";
  if (!client.find(endOfHeaders)) {
    Serial.println(F("Invalid response"));
    return;
  }

// too big or too small causes no memory error
DynamicJsonDocument doc(40000);
DeserializationError err = deserializeJson(doc,client);

if (err) {
  Serial.print(F("Parsing error:"));
  Serial.println(err.c_str());
  tftMessage("Parsing error");
  infiniteLoop();
}

  // this section is awkward because of the casting and assigment issues with the json library
  // it doesn't seem possible to assgin value to already declared variables so different variables
  // have to be declared inline for each for branch
  int8_t gameToExtract = -1;

  const char* gameDate = doc["dates"][0]["date"];
  const uint8_t gameCount = doc["totalGames"];

  String yesterdaysDate = convertDate(today - SECONDS_IN_A_DAY);


  // this code is dirty and confusing.  Should probably be rewritten
  // but it works for now.
  if (yesterdaysDate.compareTo(gameDate) == 0) {
    const char* gameStatus = doc["dates"][0]["games"][0]["status"]["abstractGameState"];
    if (strcmp(gameStatus,STATUSCODE_FINAL) != 0) {
        gameToExtract = 0;
    }
    else {
      uint32_t gameIDa = doc["dates"][1]["games"][0]["gamePk"];
      if (gameIDa == excludeGameID) {
        gameToExtract = 2;
      }
      else {
        gameToExtract = 1;
      }
    }
  }
  else {
    uint32_t gameIDb = doc["dates"][0]["games"][0]["gamePk"];
    if (gameIDb == excludeGameID) {
      gameToExtract = 1;
    }
    else {
      gameToExtract = 0;
    }
  }

  Serial.print(F("Extracting game: "));
  Serial.println(gameToExtract);
  if (gameCount <= gameToExtract) {
    tftMessage("gameCount error");
    infiniteLoop();
  }

  nextGameData->isNHL = isNHLGame;
  switch (gameToExtract) {
    case 0:
    {
      JsonObject game0 = doc["dates"][0]["games"][0];
      nextGameData->gameID = game0["gamePk"];
      const char* gameType0 = game0["gameType"];
      nextGameData->isPlayoffs = (strcmp(gameType0,"P") == 0);
      nextGameData->awayID = game0["teams"]["away"]["team"]["id"];
      nextGameData->homeID = game0["teams"]["home"]["team"]["id"];
      String nextGame0TS_str = game0["gameDate"];
      nextGameData->startTime = parseDateTime(nextGame0TS_str);
      JsonObject homeRecord0 = game0["teams"]["home"]["leagueRecord"];
      JsonObject awayRecord0 = game0["teams"]["away"]["leagueRecord"];
      uint8_t homeWins0 = homeRecord0["wins"];
      uint8_t awayWins0 = awayRecord0["wins"];
      uint8_t homeLosses0 = homeRecord0["losses"];
      uint8_t awayLosses0 = awayRecord0["losses"];
      if (isNHLGame) {
        uint8_t homeOT0 = homeRecord0["ot"];
        uint8_t awayOT0 = awayRecord0["ot"];
        sprintf(nextGameData->homeRecord,"%d-%d-%d",homeWins0,homeLosses0,homeOT0);
        sprintf(nextGameData->awayRecord,"%d-%d-%d",awayWins0,awayLosses0,awayOT0);
      }
      else {
          sprintf(nextGameData->homeRecord,"%d-%d",homeWins0,homeLosses0);
          sprintf(nextGameData->awayRecord,"%d-%d",awayWins0,awayLosses0);
      }

    }
    break;
  case 1:
    {
      JsonObject game1 = doc["dates"][1]["games"][0];
      nextGameData->gameID = game1["gamePk"];
      const char* gameType1 = game1["gameType"];
      nextGameData->isPlayoffs = (strcmp(gameType1,"P") == 0);
      nextGameData->awayID = game1["teams"]["away"]["team"]["id"];
      nextGameData->homeID = game1["teams"]["home"]["team"]["id"];
      String nextGame1TS_str = game1["gameDate"];
      nextGameData->startTime = parseDateTime(nextGame1TS_str);
      JsonObject homeRecord1 = game1["teams"]["home"]["leagueRecord"];
      JsonObject awayRecord1 = game1["teams"]["away"]["leagueRecord"];
      uint8_t homeWins1 = homeRecord1["wins"];
      uint8_t awayWins1 = awayRecord1["wins"];
      uint8_t homeLosses1 = homeRecord1["losses"];
      uint8_t awayLosses1 = awayRecord1["losses"];
      if (isNHLGame) {
        uint8_t homeOT1 = homeRecord1["ot"];
        uint8_t awayOT1 = awayRecord1["ot"];
        sprintf(nextGameData->homeRecord,"%d-%d-%d",homeWins1,homeLosses1,homeOT1);
        sprintf(nextGameData->awayRecord,"%d-%d-%d",awayWins1,awayLosses1,awayOT1);
      }
      else {
        sprintf(nextGameData->homeRecord,"%d-%d",homeWins1,homeLosses1);
        sprintf(nextGameData->awayRecord,"%d-%d",awayWins1,awayLosses1);
      }

    }
    break;
   case 2:
    {
      JsonObject game2 = doc["dates"][2]["games"][0];
      nextGameData->gameID = game2["gamePk"];
      const char* gameType1 = game2["gameType"];
      nextGameData->isPlayoffs = (strcmp(gameType1,"P") == 0);
      nextGameData->awayID = game2["teams"]["away"]["team"]["id"];
      nextGameData->homeID = game2["teams"]["home"]["team"]["id"];
      String nextGame2TS_str = game2["gameDate"];
      nextGameData->startTime = parseDateTime(nextGame2TS_str);
      JsonObject homeRecord2 = game2["teams"]["home"]["leagueRecord"];
      JsonObject awayRecord2 = game2["teams"]["away"]["leagueRecord"];
      uint8_t homeWins2 = homeRecord2["wins"];
      uint8_t awayWins2 = awayRecord2["wins"];
      uint8_t homeLosses2 = homeRecord2["losses"];
      uint8_t awayLosses2 = awayRecord2["losses"];
      if (isNHLGame) {
        uint8_t homeOT2 = homeRecord2["ot"];
        uint8_t awayOT2 = awayRecord2["ot"];
        sprintf(nextGameData->homeRecord,"%d-%d-%d",homeWins2,homeLosses2,homeOT2);
        sprintf(nextGameData->awayRecord,"%d-%d-%d",awayWins2,awayLosses2,awayOT2);
      }
      else {
        sprintf(nextGameData->homeRecord,"%d-%d",homeWins2,homeLosses2);
        sprintf(nextGameData->awayRecord,"%d-%d",awayWins2,awayLosses2);
      }

    }

    break;
  } // switch

  client.stop();

  printNextGame(nextGameData);


}

bool switchOneValue() {
  return digitalRead(SWITCH_PIN_1);
}

void getNextGame() {
  getNextGame(now(),(currentSportIsNHL ? myNHLTeamID : myMLBTeamID ),currentSportIsNHL,&nextGameData);
}

void tftOn() {
  digitalWrite(LED_BACKLIGHT_PIN,HIGH);
}

void tftSwitchControl() {
  digitalWrite(LED_BACKLIGHT_PIN,digitalRead(SWITCH_PIN_1));
}

void displayNextGame(NextGameData* nextGameData) {

  tftSwitchControl();

  tft.fillScreen(TFT_WHITE);
  tft.setTextColor(TFT_BLACK);

  if (nextGameData->gameID == 0) {  // this shouldn't really happen until the end of the season
    displaySingleLogo(myNHLTeamID,true);
    tft.drawString("No games scheduled",(tft.width() - tft.textWidth("No games scheduled",2))/2,95,2);
    infiniteLoop();
  }
  else {
    displayTeamLogos(nextGameData->awayID,nextGameData->homeID,nextGameData->isNHL);

    if (!(nextGameData->isPlayoffs)) {
      tft.drawString(nextGameData->awayRecord,35 - (tft.textWidth(nextGameData->awayRecord)/2),65);
      tft.drawString(nextGameData->homeRecord,125 - (tft.textWidth(nextGameData->homeRecord)/2),65);
    }


    getFriendlyDate(friendlyDate,sizeof(friendlyDate),nextGameData->startTime);
    getFriendlyTime(friendlyTime,sizeof(friendlyTime),nextGameData->startTime);

    tft.drawString(friendlyDate,TFT_HALF_WIDTH - (tft.textWidth(friendlyDate,4)/2),85,4);
    tft.drawString(friendlyTime,TFT_HALF_WIDTH - (tft.textWidth(friendlyTime,2)/2),110,2);
    tft.drawString("VS",72,20,2);
  }

}

void setGDStrings(CurrentGameData* gd, const char* period, const char* timeRemaining) {
  memset(gd->period,'\0',sizeof(gd->period));
  memset(gd->timeRemaining,'\0',sizeof(gd->timeRemaining));
  strcpy(gd->period,period);
  if (strcmp(period,"SO")==0) {
    strcpy(gd->timeRemaining,"");
  }
  else {
    strcpy(gd->timeRemaining,timeRemaining);
  }
}

bool gameStatsChanged(CurrentGameData* prev, CurrentGameData* curr) {

  bool result = false;

  if (!((prev->gameID == curr->gameID) && (prev->homeID == curr->homeID) && (prev->awayID == curr->awayID) && (prev->homeScore == curr->homeScore) && (prev->awayScore == curr->awayScore))) {
    return true;
  }

  if (strcmp(prev->period,curr->period) != 0) {
    return true;
  }

  if (strcmp(prev->timeRemaining,curr->timeRemaining) != 0) {
    return true;
  }

  return false;

}

void copyGameData(CurrentGameData* dest, CurrentGameData* source) {
  dest->gameID = source->gameID;
  dest->homeID = source->homeID;
  dest->awayID = source->awayID;
  dest->homeScore = source->homeScore;
  dest->awayScore = source->awayScore;
  strcpy(dest->period,source->period);
  strcpy(dest->timeRemaining,source->timeRemaining);
  dest->isNHL = source->isNHL;
  dest->outs = source->outs;
}


// i2s based sound code removed due to compile issues (on platformIO)
// and pin availability and functionality issues
// leaving the frame work for the sound code in place for the future
// perhaps add flashing screen functionality instead
void playHorn(const bool myTeamScored) {}

// hackey hardcoded postion.up
int16_t awayScorePosition(const uint8_t theScore) {
  int16_t position = 20;
  if (theScore == 1) {
    position = 10;
  }
  else {
    if (theScore >= 20) {
      position = 2;
    }
    else if (theScore >= 10) {
      position = -8;
    }
  }
  return position;
}

// hackey hardcoded position
int16_t homeScorePosition(const uint8_t theScore) {
  int16_t position = 110;
  if (theScore == 1) {
    position = 100;
  }
  else {
    if (theScore >= 20) {
      position = 92;
    }
    else if (theScore >= 10) {
      position = 82;
    }
  }
  return position;
}

// hacky code that assumed TFT screen size.  Won't look right on other size screens
void displayCurrentGame(CurrentGameData* gameData) {

  char score[3];

  Serial.println("Display Current");
  tftOn();

  tft.fillScreen(TFT_WHITE);
  tft.setTextColor(TFT_BLACK);

  displayTeamLogos(gameData->awayID,gameData->homeID,gameData->isNHL);

  memset(score,'\0',sizeof(score));
  snprintf(score,sizeof(score),"%d",gameData->awayScore);

  tft.drawString(score,awayScorePosition(gameData->awayScore),70,7);
  memset(score,'\0',sizeof(score));
  snprintf(score,sizeof(score),"%d",gameData->homeScore);

  tft.drawString(score,homeScorePosition(gameData->homeScore),70,7);

  tft.drawString("VS",TFT_HALF_WIDTH - (tft.textWidth("VS",2)/2),30,2);

  tft.drawString(gameData->period,TFT_HALF_WIDTH - (tft.textWidth(gameData->period,2)/2),90,2);
  if ((gameData->isNHL) || (strcmp(gameData->timeRemaining,"FINAL") == 0))  {
    tft.drawString(gameData->timeRemaining,TFT_HALF_WIDTH - (tft.textWidth(gameData->timeRemaining,2)/2),105,2);
  }
  else {
    const uint8_t basePosX = 75;
    const uint8_t basePosY = 90;
    if (strcmp(gameData->timeRemaining,"top") == 0) {
      tft.fillTriangle(basePosX-12,basePosY + 12,basePosX-2,basePosY + 12,basePosX-7,basePosY+2,TFT_BLACK);
    }
    else {
      tft.fillTriangle(basePosX-12,basePosY+2,basePosX-2,basePosY+2,basePosX-7,basePosY+12,TFT_BLACK);
    }

    for (uint8_t i = 1; i <= gameData->outs; i++) {
      tft.fillCircle(basePosX-7 + (8 * i),basePosY+20,3,TFT_BLACK);
    }
    for (uint8_t i = gameData->outs + 1; i <= 3; i++) {
      tft.drawCircle(basePosX-7 + (8 * i),basePosY+20,3,TFT_BLACK);
    }
  }

}

bool getMLBGameIsFinished(const uint32_t gameID) {

  Serial.println(F("Getting is finished"));
  client.setTimeout(10000);
  if (client.connect(MLB_HOST,MLB_PORT)) {
    queryString = "GET /api/v1/schedule?gamePk=";
    queryString += gameID;
    Serial.print(F("Querystring: "));
    Serial.println(queryString);
    queryString += " HTTP/1.0";
    client.println(queryString);
    queryString = "Host: ";
    queryString += MLB_HOST;
    client.println(queryString);
    client.println(F("Connection: close"));
    if (client.println() == 0) {
      Serial.println(F("Failed to send request"));
    }
  }
  else {
    Serial.println(F("connection failed")); //error message if no client connect
    Serial.println();
  }

  while(client.connected() && !client.available()) delay(1);

  char endOfHeaders[] = "\r\n\r\n";
  if (!client.find(endOfHeaders)) {
    Serial.println(F("Invalid response"));
    infiniteLoop();
  }

  DynamicJsonDocument doc(20000);
  DeserializationError err = deserializeJson(doc,client);

  if (err) {
    Serial.print(F("Parsing error:"));
    Serial.println(err.c_str());
    tftMessage("Parsing error");
    infiniteLoop();
  }

  const char* gameStatus = doc["dates"][0]["games"][0]["status"]["abstractGameState"];
  bool result = (strcmp(gameStatus,STATUSCODE_FINAL) == 0);

  client.stop();
  return result;
}

bool getAndDisplayCurrentMLBGame(NextGameData* gameSummary, CurrentGameData* prevUpdate) {
  CurrentGameData gameData;
  bool isGameOver = false;

  Serial.println(F("Getting current game data"));
  client.setTimeout(10000);
  if (client.connect(MLB_HOST,MLB_PORT)) {
    queryString = "GET /api/v1/game/";
    queryString += gameSummary->gameID;
    queryString += "/linescore";
    Serial.printf("Host: %s",MLB_HOST);
    Serial.print(F("Querystring: "));
    Serial.println(queryString);
    queryString += " HTTP/1.0";
    client.println(queryString);
    queryString = "Host: ";
    queryString += MLB_HOST;
    client.println(queryString);
    client.println(F("Connection: close"));
    if (client.println() == 0) {
      Serial.println(F("Failed to send request"));
    }

  }else {
    Serial.println(F("connection failed")); //error message if no client connect
    Serial.println();
  }

  while(client.connected() && !client.available()) delay(1);

  char endOfHeaders[] = "\r\n\r\n";
  if (!client.find(endOfHeaders)) {
    Serial.println(F("Invalid response"));
    tftMessage("Invalid response");
    infiniteLoop();

  }

  DynamicJsonDocument doc(10000);
  DeserializationError err = deserializeJson(doc,client);

  if (err) {
    Serial.print(F("Parsing error: "));
    Serial.println(err.c_str());
    tftMessage("parsing error");
    infiniteLoop();
  }


  // the MLB linescore doesn't include the teamIDs or game status so we have to be hackey
  // using the schedule data

  gameData.gameID = gameSummary->gameID;
  gameData.isNHL = false;
  gameData.homeID = gameSummary->homeID;
  gameData.awayID = gameSummary->awayID;
  uint8_t inning = doc["currentInning"];
  if (inning == 0) {
    setGDStrings(&gameData,"pre","");
  }
  else {
    gameData.homeScore = doc["teams"]["home"]["runs"];
    gameData.awayScore = doc["teams"]["away"]["runs"];
    gameData.outs = doc["outs"];

    if ((inning >= 9) && (getMLBGameIsFinished(gameData.gameID))) {
      setGDStrings(&gameData,"","FINAL");
      isGameOver = true;
    }
    else {
      const char* inningStr = doc["currentInningOrdinal"];
      bool isTopInning = doc["isTopInning"];
      if (isTopInning) {
        setGDStrings(&gameData,inningStr,"top");
      }
      else {
        setGDStrings(&gameData,inningStr,"bot");
      }

    }

  }

  client.stop();

  printCurrentGame(&gameData);

  if (gameStatsChanged(prevUpdate,&gameData)) {
    Serial.println(F("game stats changed"));
    copyGameData(prevUpdate,&gameData);
    displayCurrentGame(prevUpdate);
  }

  return isGameOver;
}

bool getAndDisplayCurrentNHLGame(const uint32_t gameID, CurrentGameData* prevUpdate) {
  CurrentGameData gameData;
  bool isGameOver = false;

  //Serial.println(F("Getting game data"));
  client.setTimeout(10000);
  if (client.connect(NHL_HOST,NHL_PORT)) {
    queryString = "GET /api/v1/game/";
    queryString += gameID;
    queryString += "/linescore";
    Serial.print(F("Querystring: "));
    Serial.println(queryString);
    queryString += " HTTP/1.0";
    client.println(queryString);
    queryString = "Host: ";
    queryString += NHL_HOST;
    client.println(queryString);
    client.println(F("Connection: close"));
    if (client.println() == 0) {
      Serial.println(F("Failed to send request"));
    }

  }else {
    Serial.println(F("connection failed")); //error message if no client connect
    Serial.println();
  }

  while(client.connected() && !client.available()) delay(1);

  char endOfHeaders[] = "\r\n\r\n";
  if (!client.find(endOfHeaders)) {
    Serial.println(F("Invalid response"));
    tftMessage("Invalid response");
    infiniteLoop();

  }

  DynamicJsonDocument doc(10000);
  DeserializationError err = deserializeJson(doc,client);

  if (err) {
    Serial.print(F("Parsing error: "));
    Serial.println(err.c_str());
    tftMessage("parsing error");
    infiniteLoop();
  }
  else {
    Serial.println(F("parsing success"));
  }


  gameData.gameID = gameID;
  gameData.isNHL = true;
  gameData.homeID = doc["teams"]["home"]["team"]["id"];
  gameData.homeScore = doc["teams"]["home"]["goals"];
  gameData.awayID = doc["teams"]["away"]["team"]["id"];
  gameData.awayScore = doc["teams"]["away"]["goals"];
  uint8_t period = doc["currentPeriod"];

  if (period == 0) {
        setGDStrings(&gameData,"pre","");
  }
  else {
    const char* p = doc["currentPeriodOrdinal"];   // Json template issues if declartion isn't on same line
    const char* tr = doc["currentPeriodTimeRemaining"];
    setGDStrings(&gameData,p,tr);
    if (strcmp(tr,STATUSCODE_FINAL) == 0) {
      isGameOver = true;
    }
  }

  client.stop();

  printCurrentGame(&gameData);


  if (gameStatsChanged(prevUpdate,&gameData)) {
    copyGameData(prevUpdate,&gameData);
    displayCurrentGame(prevUpdate);
  }

  return isGameOver;
}


// "sleep" function. Calls a delay which reduces the power consumption as the ESP is put into lightSleep mode automatically
void sleep(uint32_t timeInSeconds) {
  Serial.print(F("Sleeping for "));
  Serial.print(timeInSeconds);
  Serial.println(F(" seconds"));
  delay(timeInSeconds * 1000);
  Serial.println(F("Woke up"));
}

void ledSwitchInterrupt() {
    Serial.println(F("switch interrupt"));
    if (digitalRead(SWITCH_PIN_1)) {
      digitalWrite(LED_BACKLIGHT_PIN,HIGH);
    }
    else {
      if (gameStatus == SCHEDULED) {
          digitalWrite(LED_BACKLIGHT_PIN,LOW);
      }
    }
}

void checkForUpdates() {

  HTTPClient httpClient;
  String BINpath;
  String BINversionFilePath;
  String SPIFFSversionFilePath;
  String imageFile;
  uint32_t httpCode;
  uint32_t newBINversion;
  uint32_t currentSPIFFSversion;
  uint32_t newSPIFFSversion;
  bool binUpdateAvailable = false;
  bool spiffsUpdateAvailable = false;
  bool buttonPress = false;

  BINpath = FIRMWARE_URL;
  BINpath += PROJECT_NAME;
  BINversionFilePath = BINpath + BIN_VERSION_FILENAME;
  SPIFFSversionFilePath = BINpath + SPIFFS_VERSION_FILENAME;

  Serial.println(F("-----------------"));
  Serial.println(F("Firmware Checker"));
  httpClient.begin(BINversionFilePath);
  httpCode = httpClient.GET();
  newBINversion = httpClient.getString().toInt();
  if (httpCode == 200) {
      Serial.printf("BIN Version Current:   %d\r\n",CURRENT_BIN_VERSION);
      Serial.printf("BIN Version Available: %d\r\n",newBINversion);
      binUpdateAvailable = (newBINversion > CURRENT_BIN_VERSION);
      Serial.printf("BIN Update Available:  %s\r\n", (binUpdateAvailable) ? "Yes" : "No");
  }
  else {
    Serial.printf("BIN HTTP Error: %d\r\n",httpCode);
  }
  httpClient.end();

  fs::File file = SPIFFS.open(SPIFFS_VERSION_FILENAME,"r");

  if (!file) {
    Serial.println(F("SPIFFS Version read error"));
  }
  else {
    currentSPIFFSversion = file.parseInt();
    file.close();
    httpClient.begin(SPIFFSversionFilePath);
    httpCode = httpClient.GET();
    newSPIFFSversion = httpClient.getString().toInt();
    if (httpCode == 200) {
      Serial.printf("SPIFFS Version Current:   %d\r\n",currentSPIFFSversion);
      Serial.printf("SPIFFS Version Available: %d\r\n",newSPIFFSversion);
      spiffsUpdateAvailable = (newSPIFFSversion > currentSPIFFSversion);
      Serial.printf("SPIFFS Update available:  %s\r\n", (spiffsUpdateAvailable) ? "Yes" : "No");
    }
    else {
      Serial.printf("SPIFFS HTTP Error: %d\r\n",httpCode);
    }
    httpClient.end();

    file.close();

    Serial.println(F("-----------------"));

    bool proceedWithUpdate = false;

    if (binUpdateAvailable) {

      tftString  = "BIN update available\r\n\r\nPress button to continue";
      tftMessage(tftString);
      while (!buttonPress) {
        debouncer.update();
        buttonPress = debouncer.rose();
        yield();
      }


      imageFile = BINpath;
      imageFile += BIN_PREFIX;
      imageFile += newBINversion;
      imageFile += BIN_EXT;
      Serial.print(F("BIN Image to install: "));
      Serial.println(imageFile);
      Serial.println(F("Downloading... DON'T TURN OFF!"));
      tftMessage("Downloading...\r\nDON'T TURN OFF!");
      t_httpUpdate_return ret = ESPhttpUpdate.update(imageFile);
      switch(ret) {
        case HTTP_UPDATE_FAILED:
          Serial.printf("HTTP_UPDATE_FAILED Error (%d): %s\r\n", ESPhttpUpdate.getLastError(), ESPhttpUpdate.getLastErrorString().c_str());
          break;
        case HTTP_UPDATE_NO_UPDATES:
          Serial.println(F("HTTP_UPDATE_NO_UPDATES"));
          break;
      }
      ESP.reset();
    }
    else if (spiffsUpdateAvailable) {

      buttonPress = false;
      tftString = "SPIFFS update available\r\n\r\nPress button to continue";
      tftMessage(tftString);
      while (!buttonPress) {
        debouncer.update();
        buttonPress = debouncer.rose();
        yield();
      }

      imageFile = BINpath;
      imageFile += SPIFFS_PREFIX;
      imageFile += newSPIFFSversion;
      imageFile += SPIFFS_EXT;
      Serial.print(F("SPIFFS Image to install: "));
      Serial.println(imageFile);
      Serial.println(F("Downloading... DON'T TURN OFF!"));
      tftMessage("Downloading...\r\nDON'T TURN OFF!");
      t_httpUpdate_return ret = ESPhttpUpdate.updateSpiffs(imageFile);
      switch(ret) {
        case HTTP_UPDATE_FAILED:
          Serial.printf("HTTP_UPDATE_FAILED Error (%d): %s\r\n", ESPhttpUpdate.getLastError(), ESPhttpUpdate.getLastErrorString().c_str());
          break;
        case HTTP_UPDATE_NO_UPDATES:
          Serial.println(F("HTTP_UPDATE_NO_UPDATES"));
          break;
      }
      ESP.reset();
    }

  }
}

void setup() {
  queryString.reserve(100);
  tftString.reserve(50);

  Serial.begin(74880);

  tft.init(INITR_BLACKTAB);
  tft.setRotation(TFT_ROTATION);
  tft.fillScreen(TFT_BLACK);

  tftString = "TFT Sports Scoreboard\r\nVer: ";
  tftString += CURRENT_BIN_VERSION;
  Serial.println(tftString);
  tftMessage(tftString);

  pinMode(SWITCH_PIN_1,INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(SWITCH_PIN_1),ledSwitchInterrupt,CHANGE);

  pinMode(LED_BACKLIGHT_PIN,OUTPUT);
  digitalWrite(LED_BACKLIGHT_PIN,HIGH); // high to turn on

  debouncer.attach(SELECT_BUTTON_PIN,INPUT_PULLUP);
  debouncer.interval(DEBOUNCE_INTERVAL);

  if (!SPIFFS.begin()) {
    Serial.println(F("SPIFFS initialisation failed!"));
    tftMessage("SPIFFS Error");
    infiniteLoop();
  }
  Serial.println(F("\r\n\SPIFFS initialised."));

  //wifiManager.resetSettings();
  wifiManager.setAPCallback(wifiConfigCallback);
  wifiConnect();

  checkForUpdates();

  // load the list of NHL teams
  teamListInit();

  // get my favourite team from SPIFFS
  loadTeams();

  // GUI selection of favourite team iff button is being pressed
  debouncer.update();
  if (debouncer.read() == LOW) {
      selectMenu();
      saveTeams();
  }

  updateTime();
  getNextGame();
  displayNextGame(&nextGameData);

}

void loop() {

  static time_t waitUntil = 0;
  uint32_t gameFinished;

  if (gameStatus == STARTED) {
    if (currentSportIsNHL && (getAndDisplayCurrentNHLGame(nextGameData.gameID,&currentGameData))) {
      gameStatus = FINISHED;
      gameFinished = millis();
    }
    else if (!currentSportIsNHL && (getAndDisplayCurrentMLBGame(&nextGameData,&currentGameData))) {
       gameStatus = FINISHED;
       gameFinished = millis();
    }
    else {
      if (currentSportIsNHL) {
        sleep(GAME_UPDATE_INTERVAL_NHL_S);
      }
      else {
        sleep(GAME_UPDATE_INTERVAL_MLB_S);
      }
    }
  }

  else if (gameStatus == BUTTON_WAIT) {
    debouncer.update();
    if ((debouncer.rose()) || (now() > nextGameData.startTime) || ((millis() - gameFinished) > AFTER_GAME_RESULTS_DURATION_MS))   {
      displayNextGame(&nextGameData);
      gameStatus = SCHEDULED;
      ledSwitchInterrupt();
    }
  }
  else if (now() > nextGameData.startTime) {
    if (gameStatus == FINISHED) {
      getNextGame();
      gameStatus = BUTTON_WAIT;
    }
    else {
      gameStatus = STARTED;
    }
  }
  else if (nextGameData.startTime > now()) {
    sleep(min(MAX_SLEEP_INTERVAL_S,nextGameData.startTime - now()));
  }

  updateTime();
}

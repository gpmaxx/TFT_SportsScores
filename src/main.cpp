/*             Date       author  Ver   Description
*  Version:    2019-01-22 gpmaxx  1000  NHL initial
*              2019-03-05 gpmaxx        mlb/nhl version
               2019-03-10 gpmaxx        OTA updating version
            
               why didn't you track the changes properly?

               2021-03-31 gpmaxx  1600  Fixing various bugs  ver: 1600 
                                          - getnextgame bug
                                          - update time use ntp rather than worldtimeapi
                                          - switched queries to use httpClient
                                          - switch to use LittleFS
                                          - other minor fixes
               2021-04-03 gpmaxx  1601  Fixed buttonInterrupt bug
               2021-04-04 gpmaxx  2000  Implement NBA
               2021-04-09 gpmaxx  2001  NBA OT & Playoffs
               2021-04-17 gpmaxx  2010  Switch from linscore to feed/live api to gamefinished data for MLB
                                        fixed getNextGame finding doubleheaders for MLB
               2021-05-30 gpmaxx  2020  fix LED backlight and button bugs. Fix select team bugs
               2021-07-22 gpmaxx  2021  bug fix - mlb return code
                                                - suspended game picked up as current game bug
               2021-09-18 gpmaxx  2030  updated httpClient api calls for new 
          
                                           

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
*  
ToDo:           - Testing 
                    - NBA fully
                        - OT
                    - memory
                    - switching selected team
                    - other 

                - figure out way to how to speed up MLB query
                      - best of both worlds. single query. which is fast
                      - feed api contains lots of data which is slower to parse than linescore api
                      - but linescore api does not include abstractGameState
                - NBA Playoffs
                - handle PPD games
                      - need to capture Jsons for testing
                - investigate configTime and why it's so shit?
                - update icons for MLB
                - update icons for NHL
                - better documentation

*   Maybe:     - change millis() code to be rollover safe
               - change code to be league generic? so nhl, mlb, nba aren't listed in variable names
               - dark mode
               - secure and/or signed OTA updates
               - figure out series record for mlb games - no API options?
               - clean up use of global variables
*              - make graphics dynamic for display screens other than 128 x 160
*              - Figure out how to list playoff series record - not available in NHL API?
*/

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <Bounce2.h>
#include <Time.h>
#include <Timezone.h>
#include <LittleFS.h>
#include <WiFiManager.h>
#define ARDUINOJSON_USE_LONG_LONG 1
#define ARDUINOJSON_USE_DOUBLE 1
#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <TZ.h>

#define SIMPLEDEBUG_SERIAL Serial
#include "SimpleDebug.h"

#include "BMP_functions.h"

////////////////// Global Constants //////////////////
// !!!!! Change version for each build !!!!!
const uint16_t CURRENT_FW_VERSION = 2022;

const uint8_t SWITCH_PIN_1 = D6;          // change with caution
const uint8_t TFT_BACKLIGHT_PIN = D4;     // change with caution
const uint8_t SELECT_BUTTON_PIN = D1;     // change with caution

const uint8_t DEBOUNCE_INTERVAL = 25;
const uint16_t LONG_PRESS_THRESHOLD = 1000;
const uint8_t TFT_ROTATION = 3;

const char* TEAMS_DATAFILE = "/myteam.dat";
const uint32_t TIME_UPDATE_INTERVAL_MS = 1000 * 60 * 60 * 24; // 24 hours
const char* NTP_SERVER = "pool.ntp.org";
const uint16_t NTP_WAIT = 500;
// See https://github.com/esp8266/Arduino/blob/master/cores/esp8266/TZ.h
#define MY_TZ TZ_America_Toronto
 
const uint16_t TFT_HALF_WIDTH = 80;
const uint16_t TFT_HALF_HEIGHT = 64;

const uint32_t SECONDS_IN_A_DAY = 60 * 60 * 24;
const uint32_t SECONDS_IN_A_WEEK = SECONDS_IN_A_DAY * 7;
const char* HTTP_END_OF_HEADER = "\r\n\r\n";

const char* ORDINALS[4] = {"1st","2nd","3rd","4th"};


// Game status codes. These are dependant on the statsapi
// NHL uses numbers and MLB uses characters
const char* STATUSCODE_PREGAME = "Preview";
const char* STATUSCODE_LIVE = "Live";
const char* STATUSCODE_FINAL = "Final";
const char* STATUSCODE_SCHEDULED = "Scheduled";
const char* STATUSCODE_INPROGRESS = "In Progress";

//const char* STATUSCODE_NBA_SCHEDULED = "Scheduled";
//const char* STATUSCODE_NBA_INPROGRESS = "In Progress";
const char* STATUSCODE_NBA_FINAL = "STATUS_FINAL";
const char* STATUSCODE_NBA_HALFTIME = "STATUS_HALFTIME";
const char* STATUSCODE_NBA_ENDOFQUATER = "STATUS_END_PERIOD";
const char* STATUSCODE_NBA_SCHEDULED = "STATUS_SCHEDULED";

const uint32_t AFTER_GAME_RESULTS_DURATION_MS = 60 * 60 * 1 * 1000; // 1 hours
const uint32_t GAME_UPDATE_INTERVAL = 65;  // 65 seconds
const uint32_t MAX_SLEEP_INTERVAL_S = 60 * 60; // 1 hour

const char* FW_URL = "https://www.lipscomb.ca/IOT/firmware/";
const char* FW_HOST = "lipscomb.ca";
const char* PROJECT_NAME = "TFT_SportsScores/";
const char* FW_VERSION_FILENAME = "firmware_ver.txt";
const char* CFG_VERSION_FILENAME = "fs_ver.txt";
const char* FW_PREFIX = "firmware_";
const char* CFG_PREFIX = "littlefs_";
const char* FW_EXT = ".bin";
const char* CFG_EXT = ".bin";
const char* NBA_FILTER_JSON = "nba_filter.json";

const uint8_t TFT_BUFFER_SIZE = 80;

const uint8_t NUM_LEAGUES = 3;

const uint8_t NHL = 0;
const uint8_t NHL_ICON_ID = 253; // can't match any other team ids   
const char* NHL_HOST = "statsapi.web.nhl.com";
const uint16_t NHL_PORT = 80;
const uint8_t NHL_NUM_TEAMS = 31;
const uint8_t NHL_NUM_MENU_ITEMS = NHL_NUM_TEAMS + NUM_LEAGUES - 1;

const uint8_t MLB = 1; 
const uint8_t MLB_ICON_ID = 254; // can't match any other team ids
const char* MLB_HOST = "statsapi.mlb.com";
const uint16_t MLB_PORT = 80;
const uint8_t MLB_NUM_TEAMS = 30;
const uint8_t MLB_NUM_MENU_ITEMS = MLB_NUM_TEAMS + NUM_LEAGUES - 1;


const uint8_t NBA = 2;
const uint8_t NBA_ICON_ID = 255;   // can't match any other team ids
const char* NBA_HOST = "";
const uint16_t NBA_PORT = 443;
const uint8_t NBA_NUM_TEAMS = 30;
const uint8_t NBA_NUM_MENU_ITEMS = NBA_NUM_TEAMS + NUM_LEAGUES - 1;


const char* LEAGUE_NAMES[NUM_LEAGUES] = {"NHL","MLB","NBA"};

// handy for testing
#define ILOOP while(true){yield();}  


////////////////// Data Structs ///////////


typedef struct  {
  uint8_t id = 0;
  char name[4] = "XXX";
} TeamInfo;

typedef struct {
  uint8_t awayID = 0;
  uint8_t homeID = 0;
  uint32_t gameID = 0;
  char homeRecord[9];   // xx-xx-xx
  char awayRecord[9];
  bool isPlayoffs = false;
  time_t startTime = 0;
  uint8_t league = 0;
} NextGameData;

typedef struct  {
  uint32_t gameID = 0;
  uint8_t awayID = 0;
  uint8_t homeID = 0;
  uint8_t awayScore = 0;
  uint8_t homeScore = 0;
  uint8_t homeOther = 0;   // only used for NHL powerplay right now
  uint8_t awayOther = 0;   // only used for NHL powerplay right now
  char devision[5];    // 1st, 2nd etc
  char timeRemaining[6];  // 12:34
  uint8_t league = NHL;
  bool bases[3];
  uint8_t outs = 0;
} CurrentGameData;

enum GameStatus {NEW_TEAM,NO_GAMES,SCHEDULED,STARTED,FINISHED,AFTER_GAME};

///////////// Global Variables ////////////
uint16_t selectedTeam[NUM_LEAGUES] = {24,109,37};  // Aniheim, Arizona,     alphabetical first

char timeString[16 + 1];    // date buffer format: YYYY/MM/DD HH:mm 
uint8_t currentLeague = NHL;
bool otaSelected = false;
volatile bool switchTeamsFlag = false;   // used by button interrupt to signal to switch the selected team

/////////// Global Object Variables //////////
TFT_eSPI tft = TFT_eSPI();
TeamInfo nhlTeams[NHL_NUM_MENU_ITEMS];
TeamInfo mlbTeams[MLB_NUM_MENU_ITEMS];
TeamInfo nbaTeams[NBA_NUM_MENU_ITEMS];
Bounce debouncer = Bounce();
WiFiManager wifiManager;
NextGameData nextGameData;
CurrentGameData currentGameData;
GameStatus gameStatus = NEW_TEAM;

////////////////  Code //////////////////////

void tftMessage(const __FlashStringHelper *format, ...) {
  static char buffer[TFT_BUFFER_SIZE + 1];
  memset(buffer,sizeof(buffer),'\0');
  va_list ap;
  va_start(ap,format);
  vsnprintf(buffer,sizeof(buffer), (const char*) format, ap);
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE);
  tft.setCursor(0,0);  
  tft.print(buffer);
  va_end(ap);
}

void iLoop() {
  dPrintf(F("infinite delay"));
  while (true) {
    delay(0xFFFFFFFF);
  }
}

void permanentError(const __FlashStringHelper *format, ...) {
  static char buffer[80 + 1];
  memset(buffer,sizeof(buffer),'\0');
  va_list ap;
  va_start(ap,format);
  vsnprintf(buffer,sizeof(buffer), (const char*) format, ap);
  tftMessage(format,ap);
  dPrintf(F("Permanent Error!!!: %s\n"),buffer);
  va_end(ap);
  delay(5000);
  ESP.restart();

}

void tftSet(int8_t action); 

void ICACHE_RAM_ATTR buttonInterrupt() {

  dPrintln(F("select button pressed"));
  static uint32_t pressedTime = 0;

  if (digitalRead(SWITCH_PIN_1) == LOW) {  // screen off
    if (gameStatus != STARTED) {
        tftSet(!digitalRead(SELECT_BUTTON_PIN)); 
    }
  }
  else {
    if (digitalRead(SELECT_BUTTON_PIN) == LOW) {
      if (pressedTime == 0) {
        pressedTime = millis();
      }
    }
    else {
      if ((millis() - pressedTime) > LONG_PRESS_THRESHOLD) {
        switchTeamsFlag = true;    
      }
      pressedTime = 0;
    }
  }
 
}

void setInterrupt(const bool enable) {
  if (enable) {
    dPrintln(F("Enable select button interrupt"));
    attachInterrupt(digitalPinToInterrupt(SELECT_BUTTON_PIN),buttonInterrupt,CHANGE);
  }
  else {
    dPrintln(F("disable select button interrupt"));
    detachInterrupt(digitalPinToInterrupt(SELECT_BUTTON_PIN));
  }
}




// populate teams list
void teamListInit_NHL() {
  nhlTeams[0].id = 24;
  snprintf(nhlTeams[0].name,sizeof(nhlTeams[0].name),"%s",F("ANA"));
  nhlTeams[1].id = 53;
  snprintf(nhlTeams[1].name,sizeof(nhlTeams[1].name),"%s",F("ARI"));
  nhlTeams[2].id = 6;
  snprintf(nhlTeams[2].name,sizeof(nhlTeams[2].name),"%s",F("BOS"));
  nhlTeams[3].id = 7;
  snprintf(nhlTeams[3].name,sizeof(nhlTeams[3].name),"%s",F("BUF"));
  nhlTeams[4].id = 12;
  snprintf(nhlTeams[4].name,sizeof(nhlTeams[4].name),"%s",F("CAR"));
  nhlTeams[5].id = 29;
  snprintf(nhlTeams[5].name,sizeof(nhlTeams[5].name),"%s",F("CBJ"));
  nhlTeams[6].id = 20;
  snprintf(nhlTeams[6].name,sizeof(nhlTeams[6].name),"%s",F("CGY"));
  nhlTeams[7].id = 16;
  snprintf(nhlTeams[7].name,sizeof(nhlTeams[7].name),"%s",F("CHI"));
  nhlTeams[8].id = 21;
  snprintf(nhlTeams[8].name,sizeof(nhlTeams[8].name),"%s",F("COL"));
  nhlTeams[9].id = 25;
  snprintf(nhlTeams[9].name,sizeof(nhlTeams[9].name),"%s",F("DAL"));
  nhlTeams[10].id = 17;
  snprintf(nhlTeams[10].name,sizeof(nhlTeams[10].name),"%s",F("DET"));
  nhlTeams[11].id = 22;
  snprintf(nhlTeams[11].name,sizeof(nhlTeams[11].name),"%s",F("EDM"));
  nhlTeams[12].id = 13;
  snprintf(nhlTeams[12].name,sizeof(nhlTeams[12].name),"%s",F("FLA"));
  nhlTeams[13].id = 26;
  snprintf(nhlTeams[13].name,sizeof(nhlTeams[13].name),"%s",F("LAK"));
  nhlTeams[14].id = 30;
  snprintf(nhlTeams[14].name,sizeof(nhlTeams[14].name),"%s",F("MIN"));
  nhlTeams[15].id = 8;
  snprintf(nhlTeams[15].name,sizeof(nhlTeams[15].name),"%s",F("MTL"));
  nhlTeams[16].id = 1;
  snprintf(nhlTeams[16].name,sizeof(nhlTeams[16].name),"%s",F("NJD"));
  nhlTeams[17].id = 18;
  snprintf(nhlTeams[17].name,sizeof(nhlTeams[17].name),"%s",F("NSH"));
  nhlTeams[18].id = 2;
  snprintf(nhlTeams[18].name,sizeof(nhlTeams[18].name),"%s",F("NYI"));
  nhlTeams[19].id = 3;
  snprintf(nhlTeams[19].name,sizeof(nhlTeams[19].name),"%s",F("NYR"));
  nhlTeams[20].id = 9;
  snprintf(nhlTeams[20].name,sizeof(nhlTeams[20].name),"%s",F("OTT"));
  nhlTeams[21].id = 4;
  snprintf(nhlTeams[21].name,sizeof(nhlTeams[21].name),"%s",F("PHI"));
  nhlTeams[22].id = 5;
  snprintf(nhlTeams[22].name,sizeof(nhlTeams[22].name),"%s",F("PIT"));
  nhlTeams[23].id = 28;
  snprintf(nhlTeams[23].name,sizeof(nhlTeams[23].name),"%s",F("SJS"));
  nhlTeams[24].id = 19;
  snprintf(nhlTeams[24].name,sizeof(nhlTeams[24].name),"%s",F("STL"));
  nhlTeams[25].id = 14;
  snprintf(nhlTeams[25].name,sizeof(nhlTeams[25].name),"%s",F("TBL"));
  nhlTeams[26].id = 10;
  snprintf(nhlTeams[26].name,sizeof(nhlTeams[26].name),"%s",F("TOR"));
  nhlTeams[27].id = 23;
  snprintf(nhlTeams[27].name,sizeof(nhlTeams[27].name),"%s",F("VAN"));
  nhlTeams[28].id = 54;
  snprintf(nhlTeams[28].name,sizeof(nhlTeams[28].name),"%s",F("VGK"));
  nhlTeams[29].id = 52;
  snprintf(nhlTeams[29].name,sizeof(nhlTeams[29].name),"%s",F("WPG"));
  nhlTeams[30].id = 15;
  snprintf(nhlTeams[30].name,sizeof(nhlTeams[30].name),"%s",F("WSH"));
  nhlTeams[31].id = MLB_ICON_ID;
  snprintf(nhlTeams[31].name,sizeof(nhlTeams[31].name),"%s",F("MLB"));
  nhlTeams[32].id = NBA_ICON_ID;
  snprintf(nhlTeams[32].name,sizeof(nhlTeams[32].name),"%s",F("NBA"));
}

// populate teams list
void teamListInit_MLB() {
  mlbTeams[0].id = 109;
  snprintf(mlbTeams[0].name,sizeof(mlbTeams[0].name),"%s",F("ARI"));
  mlbTeams[1].id = 144;
  snprintf(mlbTeams[1].name,sizeof(mlbTeams[1].name),"%s",F("ATL"));
  mlbTeams[2].id = 110;
  snprintf(mlbTeams[2].name,sizeof(mlbTeams[2].name),"%s",F("BAL"));
  mlbTeams[3].id = 111;
  snprintf(mlbTeams[3].name,sizeof(mlbTeams[3].name),"%s",F("BOS"));
  mlbTeams[4].id = 112;
  snprintf(mlbTeams[4].name,sizeof(mlbTeams[4].name),"%s",F("CHC"));
  mlbTeams[5].id = 145;
  snprintf(mlbTeams[5].name,sizeof(mlbTeams[5].name),"%s",F("CWS"));
  mlbTeams[6].id = 113;
  snprintf(mlbTeams[6].name,sizeof(mlbTeams[6].name),"%s",F("CIN"));
  mlbTeams[7].id = 114;
  snprintf(mlbTeams[7].name,sizeof(mlbTeams[7].name),"%s",F("CLE"));
  mlbTeams[8].id = 115;
  snprintf(mlbTeams[8].name,sizeof(mlbTeams[8].name),"%s",F("COL"));
  mlbTeams[9].id = 116;
  snprintf(mlbTeams[9].name,sizeof(mlbTeams[9].name),"%s",F("DET"));
  mlbTeams[10].id = 117;
  snprintf(mlbTeams[10].name,sizeof(mlbTeams[10].name),"%s",F("HOU"));
  mlbTeams[11].id = 118;
  snprintf(mlbTeams[11].name,sizeof(mlbTeams[11].name),"%s",F("KCR"));
  mlbTeams[12].id = 108;
  snprintf(mlbTeams[12].name,sizeof(mlbTeams[12].name),"%s",F("LAA"));
  mlbTeams[13].id = 119;
  snprintf(mlbTeams[13].name,sizeof(mlbTeams[13].name),"%s",F("LAD"));
  mlbTeams[14].id = 146;
  snprintf(mlbTeams[14].name,sizeof(mlbTeams[14].name),"%s",F("MIA"));
  mlbTeams[15].id = 158;
  snprintf(mlbTeams[15].name,sizeof(mlbTeams[15].name),"%s",F("MIL"));
  mlbTeams[16].id = 142;
  snprintf(mlbTeams[16].name,sizeof(mlbTeams[16].name),"%s",F("MIN"));
  mlbTeams[17].id = 121;
  snprintf(mlbTeams[17].name,sizeof(mlbTeams[17].name),"%s",F("NYM"));
  mlbTeams[18].id = 147;
  snprintf(mlbTeams[18].name,sizeof(mlbTeams[18].name),"%s",F("NYY"));
  mlbTeams[19].id = 133;
  snprintf(mlbTeams[19].name,sizeof(mlbTeams[19].name),"%s",F("OAK"));
  mlbTeams[20].id = 143;
  snprintf(mlbTeams[20].name,sizeof(mlbTeams[20].name),"%s",F("PHI"));
  mlbTeams[21].id = 134;
  snprintf(mlbTeams[21].name,sizeof(mlbTeams[21].name),"%s",F("PIT"));
  mlbTeams[22].id = 135;
  snprintf(mlbTeams[22].name,sizeof(mlbTeams[22].name),"%s",F("SDP"));
  mlbTeams[23].id = 137;
  snprintf(mlbTeams[23].name,sizeof(mlbTeams[23].name),"%s",F("SFG"));
  mlbTeams[24].id = 136;
  snprintf(mlbTeams[24].name,sizeof(mlbTeams[24].name),"%s",F("SEA"));
  mlbTeams[25].id = 138;
  snprintf(mlbTeams[25].name,sizeof(mlbTeams[25].name),"%s",F("STL"));
  mlbTeams[26].id = 139;
  snprintf(mlbTeams[26].name,sizeof(mlbTeams[26].name),"%s",F("TBR"));
  mlbTeams[27].id = 140;
  snprintf(mlbTeams[27].name,sizeof(mlbTeams[27].name),"%s",F("TEX"));
  mlbTeams[28].id = 141;
  snprintf(mlbTeams[28].name,sizeof(mlbTeams[28].name),"%s",F("TOR"));
  mlbTeams[29].id = 120;
  snprintf(mlbTeams[29].name,sizeof(mlbTeams[29].name),"%s",F("WSH"));
  mlbTeams[30].id = NHL_ICON_ID;
  snprintf(mlbTeams[30].name,sizeof(mlbTeams[30].name),"%s",F("NHL"));
  mlbTeams[31].id = NBA_ICON_ID;
  snprintf(mlbTeams[31].name,sizeof(mlbTeams[31].name),"%s",F("NBA"));
}

void teamListInit_NBA() {
  nbaTeams[0].id = 1;
  snprintf(nbaTeams[0].name,sizeof(nbaTeams[0].name),"%s",F("ATL"));
  nbaTeams[1].id = 2;
  snprintf(nbaTeams[1].name,sizeof(nbaTeams[1].name),"%s",F("BOS"));
  nbaTeams[2].id = 17;
  snprintf(nbaTeams[2].name,sizeof(nbaTeams[2].name),"%s",F("BKN"));
  nbaTeams[3].id = 30;
  snprintf(nbaTeams[3].name,sizeof(nbaTeams[3].name),"%s",F("CHA"));
  nbaTeams[4].id = 4;
  snprintf(nbaTeams[4].name,sizeof(nbaTeams[4].name),"%s",F("CHI"));
  nbaTeams[5].id = 5;
  snprintf(nbaTeams[5].name,sizeof(nbaTeams[5].name),"%s",F("CLE"));
  nbaTeams[6].id = 6;
  snprintf(nbaTeams[6].name,sizeof(nbaTeams[6].name),"%s",F("DAL"));
  nbaTeams[7].id = 7;
  snprintf(nbaTeams[7].name,sizeof(nbaTeams[7].name),"%s",F("DEN"));
  nbaTeams[8].id = 8;
  snprintf(nbaTeams[8].name,sizeof(nbaTeams[8].name),"%s",F("DET"));
  nbaTeams[9].id = 9;
  snprintf(nbaTeams[9].name,sizeof(nbaTeams[9].name),"%s",F("GSW"));
  nbaTeams[10].id = 10;
  snprintf(nbaTeams[10].name,sizeof(nbaTeams[10].name),"%s",F("HOU"));
  nbaTeams[11].id = 11;
  snprintf(nbaTeams[11].name,sizeof(nbaTeams[11].name),"%s",F("IND"));
  nbaTeams[12].id = 12;
  snprintf(nbaTeams[12].name,sizeof(nbaTeams[12].name),"%s",F("LAC"));
  nbaTeams[13].id = 13;
  snprintf(nbaTeams[13].name,sizeof(nbaTeams[13].name),"%s",F("LAL"));
  nbaTeams[14].id = 29;
  snprintf(nbaTeams[14].name,sizeof(nbaTeams[14].name),"%s",F("MEM"));
  nbaTeams[15].id = 14;
  snprintf(nbaTeams[15].name,sizeof(nbaTeams[15].name),"%s",F("MIA"));
  nbaTeams[16].id = 15;
  snprintf(nbaTeams[16].name,sizeof(nbaTeams[16].name),"%s",F("MIL"));
  nbaTeams[17].id = 16;
  snprintf(nbaTeams[17].name,sizeof(nbaTeams[17].name),"%s",F("MIN"));
  nbaTeams[18].id = 3;
  snprintf(nbaTeams[18].name,sizeof(nbaTeams[18].name),"%s",F("NOP"));
  nbaTeams[19].id = 18;
  snprintf(nbaTeams[19].name,sizeof(nbaTeams[19].name),"%s",F("NYK"));
  nbaTeams[20].id = 25;
  snprintf(nbaTeams[20].name,sizeof(nbaTeams[20].name),"%s",F("OKC"));
  nbaTeams[21].id = 19;
  snprintf(nbaTeams[21].name,sizeof(nbaTeams[21].name),"%s",F("ORL"));
  nbaTeams[22].id = 20;
  snprintf(nbaTeams[22].name,sizeof(nbaTeams[22].name),"%s",F("PHI"));
  nbaTeams[23].id = 21;
  snprintf(nbaTeams[23].name,sizeof(nbaTeams[23].name),"%s",F("PHX"));
  nbaTeams[24].id = 22;
  snprintf(nbaTeams[24].name,sizeof(nbaTeams[24].name),"%s",F("POR"));
  nbaTeams[25].id = 23;
  snprintf(nbaTeams[25].name,sizeof(nbaTeams[25].name),"%s",F("SAC"));
  nbaTeams[26].id = 24;
  snprintf(nbaTeams[26].name,sizeof(nbaTeams[26].name),"%s",F("SAS"));
  nbaTeams[27].id = 28;
  snprintf(nbaTeams[27].name,sizeof(nbaTeams[27].name),"%s",F("TOR"));
  nbaTeams[28].id = 26;
  snprintf(nbaTeams[28].name,sizeof(nbaTeams[28].name),"%s",F("UTA"));
  nbaTeams[29].id = 27;
  snprintf(nbaTeams[29].name,sizeof(nbaTeams[29].name),"%s",F("WAS"));
  nbaTeams[30].id = NHL_ICON_ID;
  snprintf(nbaTeams[30].name,sizeof(nbaTeams[30].name),"%s",F("NHL"));
  nbaTeams[31].id = MLB_ICON_ID;
  snprintf(nbaTeams[31].name,sizeof(nbaTeams[31].name),"%s",F("MLB"));
}

void teamListInit() {
  teamListInit_NHL();
  teamListInit_MLB();
  teamListInit_NBA();
}

void drawIcon(const char *filename, int16_t x, int16_t y) {
  if (!drawBmp(&tft,filename,x,y)) {
    tft.fillRect(x,y,x+50,y+50,TFT_WHITE);
    tft.drawLine(x,y,x+50,y+50,TFT_BLACK);
    tft.drawLine(x,y+50,x+50,y,TFT_BLACK);
  }
}

char* getTeamAbbreviation(const uint16_t teamID, const uint8_t league) {

  if (league == NHL) {
    for (uint8_t i = 0; i < NHL_NUM_MENU_ITEMS; i++) {
      if (nhlTeams[i].id == teamID) {
        return nhlTeams[i].name;
      }
    }
  }
  else if (league == MLB) {
    for (uint8_t i = 0; i < MLB_NUM_MENU_ITEMS; i++) {
      if (mlbTeams[i].id == teamID) {
        return mlbTeams[i].name;
      }
    }
  }
  else if (league == NBA) {
    for (uint8_t i = 0; i < NBA_NUM_MENU_ITEMS; i++) {
      if (nbaTeams[i].id == teamID) {
        return nbaTeams[i].name;
      }
    }
  }

  return "ERR";
}

void displaySingleLogo(const uint8_t teamID, const uint8_t league) {

  char filePath[19];
  char* path;

  switch (teamID) {
    case NHL_ICON_ID:
      path = "NHL/";
      break;
    case MLB_ICON_ID:
      path = "MLB/";
      break;
    case NBA_ICON_ID:
      path = "NBA/";
      break;
    default: 
    {
      if (league == NHL) {
        path = "NHL/";
      }
      else if (league == MLB) {
        path = "MLB/";
      }
      else if (league == NBA) {
        path = "NBA/";
      }
      else {
        dPrintf(F("DisplaySingleLogo: Unrecognized logo: teamID %d  league: %d\n"),teamID,league);
        path = "XXX/";
      }
    }

  }

  char* teamName = getTeamAbbreviation(teamID,league);
  sprintf(filePath,"%s%s%s%s","/icons/",path,teamName,".bmp");

  drawIcon(filePath,TFT_HALF_WIDTH-25,TFT_HALF_HEIGHT-25);

}

void displayTeamLogos(const uint8_t awayID, const uint8_t homeID, const uint8_t league) {

  char filePath[19];
  char* sportPath;

  if (league == NHL) {
    sportPath = "NHL/";
  }
  else if (league == MLB) {
    sportPath = "MLB/";
  }
  else if (league == NBA) {
    sportPath = "NBA/";  
  }
  else {
    dPrintf(F("DisplayTeamLogos: Unrecognized league: %d\n"),league);
  }

  sprintf(filePath,"%s%s%s%s","/icons/",sportPath,getTeamAbbreviation(awayID,league),".bmp");
  drawIcon(filePath,10,10);

  sprintf(filePath,"%s%s%s%s","/icons/",sportPath,getTeamAbbreviation(homeID,league),".bmp");
  drawIcon(filePath,100,10);

}

uint16_t selectTeam(TeamInfo* teamList, const uint8_t numIcons, const uint8_t league)  {

  uint8_t teamIndex = 0;
  bool teamSelected = false;
  bool switchTeams = true;
  uint32_t buttonTimer = 0;
  bool alreadyFell = false;

  // get our current team's index
  for (uint8_t i = 0; i < numIcons; i++) {
    if (teamList[i].id == selectedTeam[league]) {
      teamIndex = i;
      break;
    }
  }

  tft.fillScreen(TFT_WHITE);
  while (!teamSelected) {
    if (switchTeams) {
      displaySingleLogo(teamList[teamIndex].id,league);
      switchTeams = false;
    }
    debouncer.update();
    if (debouncer.fell()) {
      buttonTimer = millis();
      alreadyFell = true;
    }
    if (debouncer.rose() && alreadyFell) {
      if ((millis() - buttonTimer) > LONG_PRESS_THRESHOLD) {
        return teamList[teamIndex].id;
      }
      else {
        switchTeams = true;
        teamIndex = (teamIndex + 1) % numIcons;
      }
    }
    yield();
  }
}


uint16_t selectNHLTeam() {
  return selectTeam(nhlTeams,NHL_NUM_MENU_ITEMS,NHL);
}

uint16_t selectMLBTeam() {
  return selectTeam(mlbTeams,MLB_NUM_MENU_ITEMS,MLB);
}

uint16_t selectNBATeam() {
  return selectTeam(nbaTeams,NBA_NUM_MENU_ITEMS,NBA);
}

void selectMenu() {

  setInterrupt(false);

  uint16_t selectedTeamID = 0;

  while (true) {

    if (currentLeague == NHL) {
        selectedTeamID = selectNHLTeam();
        if (selectedTeamID == MLB_ICON_ID) {
          currentLeague = MLB;
        }
        else if (selectedTeamID == NBA_ICON_ID) {
          currentLeague = NBA;
        }
        else {
          selectedTeam[NHL] = selectedTeamID;
          break;
        }
    }
    else if (currentLeague == MLB) {
        selectedTeamID = selectMLBTeam();
        if (selectedTeamID == NHL_ICON_ID) {
          currentLeague = NHL;
        }
        else if (selectedTeamID == NBA_ICON_ID) {
          currentLeague = NBA;
        }
        else {
          selectedTeam[MLB] = selectedTeamID;
          break;
        }
    }
    else if (currentLeague == NBA) {
        selectedTeamID = selectNBATeam();
        dPrintf(F("Selected NBA Team: %d\n"),selectedTeamID);
        if (selectedTeamID == MLB_ICON_ID) {
          currentLeague = MLB;
          dPrintf(F("changed league: %d\n"),currentLeague);
        }
        else if (selectedTeamID == NHL_ICON_ID) {
          currentLeague = NHL;
          dPrintf(F("changed league: %d\n"),currentLeague);
        }
        else {
          selectedTeam[NBA] = selectedTeamID;
          break;
        }
    }
    else {
      permanentError(F("Unrecognized league: %d\n"),currentLeague);
    }

  }

  setInterrupt(true);

}





// output out favourite team to config file
void saveTeams() {
  fs::File file = LittleFS.open(TEAMS_DATAFILE,"w");
  if (file) {
    for (uint8_t i = 0; i < NUM_LEAGUES; i++) {
      file.println(selectedTeam[i]);
    }
    file.println(currentLeague);
  }
  else {
    dPrintf(F("Error opening myTeam file for writing\n"));
  }
 
  file.close();
}

void selectTeam() {
  selectMenu();
  saveTeams();
}


bool loadTeams() {

  bool success = false;

  fs::File file = LittleFS.open(TEAMS_DATAFILE,"r");

  if (file) {
    tftMessage(F("Loading teams..."));
    uint8_t i = 0;
    for (i = 0; i < NUM_LEAGUES; i++) {
      selectedTeam[i] = file.parseInt();
      char* teamName = getTeamAbbreviation(selectedTeam[i],i);
      if (strcmp(teamName,"ERR") != 0) {
        dPrintf(F("%s Team: %s (%d)\n"), LEAGUE_NAMES[i], teamName, selectedTeam[i]);
      }
      else {
        dPrintf(F("%s Team not found: %d\n"),LEAGUE_NAMES[i],selectedTeam[i]);
      }
    }
    if (i == NUM_LEAGUES) {
      currentLeague = file.parseInt();
      if (currentLeague < NUM_LEAGUES) {
        dPrintf(F("League to display: %s (%d)\n"),LEAGUE_NAMES[currentLeague],currentLeague);
        success = true;
      }
    }
  }
   
    
  file.close();

  return success;

}

// String of the time expected in format YYYY-MM-DD HH-mm UTC
time_t parseDateTime(const String& timeStr) {

  tmElements_t tmSet;
  tmSet.Year = timeStr.substring(0,4).toInt() - 1970;
  tmSet.Month = timeStr.substring(5,7).toInt();
  tmSet.Day = timeStr.substring(8,10).toInt();
  tmSet.Hour = timeStr.substring(11,13).toInt();
  tmSet.Minute = timeStr.substring(14,16).toInt();
  tmSet.Second = 0;
  
  return makeTime(tmSet);

}

// convert epoch time to string format
String convertDate(const time_t epoch, const bool dashes) {
  char dateChar[11] = {'\0'};

  if (dashes) {
    snprintf(dateChar,sizeof(dateChar),"%04d-%02d-%02d",year(epoch),month(epoch),day(epoch));
  }
  else {
    snprintf(dateChar,sizeof(dateChar),"%04d%02d%02d",year(epoch),month(epoch),day(epoch));
  }
  String dateString = dateChar;
  return dateString;
}

void printDate(const time_t theTime) {
  dPrintf(F("%s\n"),convertDate(theTime,true).c_str());
}

void printTime(const time_t theTime) {
  dPrintf(F("%02d:%02d:%02d\n"),hour(theTime),minute(theTime),second(theTime));
}

time_t currentTime() {
  static time_t theTime = 0;
  time(&theTime);
  return theTime;
}

void printDate() {
  printDate(currentTime());
}

void printTime() {
  printTime(currentTime());
}

void updateTime() {
  static uint32_t lastTimeUpdate = 0;
  static time_t theTime = 0;
  if ((lastTimeUpdate == 0) || ((millis() - lastTimeUpdate) > TIME_UPDATE_INTERVAL_MS)) {
   
    dPrintf(F("Fetching time please wait\n"));
    lastTimeUpdate = millis();
    configTime(MY_TZ,NTP_SERVER);

    while (theTime < 1500000000) {
      delay(NTP_WAIT);
      theTime = time(nullptr);
      time(&theTime);
    }
    dPrintf(F("Time update took: %d\n"),millis()-lastTimeUpdate);
    dPrintf(F("Epoch time: %d\n"),theTime);

    char buffer[20];
    
    strftime(buffer,sizeof(buffer),"%Y/%m/%d %H:%M:%S",gmtime(&theTime));
    dPrintf(F("Epoch time: %s\n"),buffer);
    strftime(buffer,sizeof(buffer),"%Y/%m/%d %H:%M:%S",localtime(&theTime));
    dPrintf(F("Local time: %s\n"),buffer);

  }
}


void wifiConfigCallback(WiFiManager* myWiFiManager) {
  dPrint(F("Entered WiFi Config Mode\n"));
  dPrintf(F("%03d.%03d.%03d.%03d\n"),WiFi.softAPIP()[0],WiFi.softAPIP()[1],WiFi.softAPIP()[2],WiFi.softAPIP()[3]);
  dPrintf(F("%s\n"),myWiFiManager->getConfigPortalSSID().c_str());
  tftMessage(F("%s"),myWiFiManager->getConfigPortalSSID().c_str());
}

void wifiConnect() {
  dPrintf(F("Connecting to WiFi...\n"));
  String wifiAP = "TFT_SCORE_";
  wifiAP += ESP.getChipId();
  if (!wifiManager.autoConnect(wifiAP.c_str())) {
    dPrintf(F("failed to connect timout\n"));
    tftMessage(F("WiFi connect timeout"));
    delay(10000);
    ESP.reset();
    delay(10000);
  }

  dPrintf(F("WiFi Connected\n"));

}

void printNextGame(NextGameData& nextGame) {
  dPrintf(F("\n-----------------\n"));
  dPrintf(F("GameID: %d\n"),nextGame.gameID);
  dPrintf(F("League: %d\n"),LEAGUE_NAMES[nextGame.league]);
  dPrintf(F("awayID: %d (%s)\n"),nextGame.awayID,getTeamAbbreviation(nextGame.awayID,nextGame.league));
  dPrintf(F("homeID: %d (%s)\n"),nextGame.homeID,getTeamAbbreviation(nextGame.homeID,nextGame.league));
  dPrintf(F("StartTime: %d\n"),nextGame.startTime);
  strftime(timeString,sizeof(timeString),"%Y/%m/%d %H:%M",gmtime(&(nextGame.startTime)));
  dPrintf(F("StartTime GMT: %s\n"),timeString);
  strftime(timeString,sizeof(timeString),"%Y/%m/%d %H:%M",localtime(&(nextGame.startTime)));
  dPrintf(F("StartTime Local: %s\n"),timeString);
  dPrintf(F("Away Record: %s\n"),nextGame.awayRecord);
  dPrintf(F("Home Record: %s\n"),nextGame.homeRecord);
  dPrintf(F("Is Playoffs: %s\n"),(nextGame.isPlayoffs ? "Yes" : "No"));
  dPrintf(F("-----------------\n"));
}

void printCurrentGame (CurrentGameData& currentGame) {
  dPrintf(F("\n-----------------\n"));
  dPrintf(F("GameID: %d\n"),currentGame.gameID);
  dPrintf(F("League: %s\n"),LEAGUE_NAMES[currentGame.league]);
  dPrintf(F("awayID: %d (%s)\n"),currentGame.awayID,getTeamAbbreviation(currentGame.awayID,currentGame.league));
  dPrintf(F("homeID: %d (%s)\n"),currentGame.homeID,getTeamAbbreviation(currentGame.homeID,currentGame.league));
  dPrintf(F("Away score: %d\n"),currentGame.awayScore);
  dPrintf(F("Home score: %d\n"),currentGame.homeScore);
  switch (currentGame.league) {
    case NHL:
      dPrintf(F("Period:  %s\n"),currentGame.devision);
      dPrintf(F("Time: %s\n"),currentGame.timeRemaining);
      break;
    case MLB:
      dPrintf(F("Inning: %s\n"),currentGame.devision);
      dPrintf(F("Inning: %s\n"),currentGame.timeRemaining);
      break;
    case NBA:
      dPrintf(F("Quater: %s\n"),currentGame.devision);
      dPrintf(F("Time: %s\n"),currentGame.timeRemaining);
      break;
  }
  if (currentGame.league == NHL) {
    dPrintf(F("Home PP: %s\n"), currentGame.homeOther ? "Yes" : "No");
    dPrintf(F("Away PP: %s\n"), currentGame.awayOther ? "Yes" : "No");
  }
  else if (currentGame.league == MLB) {
    dPrintf(F("Outs: %d\n"),currentGame.outs);
    dPrintf(F("Bases:\n"));
    dPrintf(F(" Running on 1st: %s\n"),(currentGame.bases[0]) ? "yes" : "no");
    dPrintf(F(" Running on 2nd: %s\n"),(currentGame.bases[1]) ? "yes" : "no");
    dPrintf(F(" Running on 3rd: %s\n"),(currentGame.bases[2]) ? "yes" : "no");
  }
  dPrintf(F("-----------------\n"));
}

void setGDStrings(CurrentGameData& gd, const char* devision, const char* timeRemaining) {

  snprintf(gd.devision,sizeof(gd.devision),"%s",devision);
  snprintf(gd.timeRemaining,sizeof(gd.timeRemaining),"%s",timeRemaining);

}

bool extractCurrentGame_MLB(CurrentGameData& gameData, JsonDocument& doc) {

   // the MLB linescore doesn't include the teamIDs or game status so we have to be hackey
  // using the schedule data

  bool isGameOver = false;

  gameData.gameID = doc["gamePk"];
  gameData.league = MLB;
  gameData.homeID = doc["liveData"]["boxscore"]["teams"]["home"]["team"]["id"];
  gameData.awayID = doc["liveData"]["boxscore"]["teams"]["away"]["team"]["id"];
  JsonObject linescore = doc["liveData"]["linescore"];
  uint8_t inning = linescore["currentInning"];
  const char* gameState = doc["gameData"]["status"]["abstractGameState"];

  if (strcmp(gameState,"preview") == 0) {
    setGDStrings(gameData,"pre","");
  }
  else {
    gameData.homeScore = linescore["teams"]["home"]["runs"];
    gameData.awayScore = linescore["teams"]["away"]["runs"];
    gameData.outs = linescore["outs"];
    if (strcmp(gameState,"Final") == 0) {
      setGDStrings(gameData,"","FINAL");
      isGameOver = true;
    }
    else {
      const char* inningStr = linescore["currentInningOrdinal"];
      bool isTopInning = linescore["isTopInning"];
      if (isTopInning) {
        setGDStrings(gameData,inningStr,"top");
      }
      else {
        setGDStrings(gameData,inningStr,"bot");
      }

    }

  }

  JsonObject offense = linescore["offense"];
  gameData.bases[0] = offense.containsKey("first");
  gameData.bases[1] = offense.containsKey("second");
  gameData.bases[2] = offense.containsKey("third");

  return isGameOver;

}

bool extractCurrentGame_NHL(CurrentGameData& gameData,uint32_t gameID, JsonDocument& doc) {

  bool isGameOver = false;

  gameData.gameID = gameID;
  gameData.league = NHL;
  gameData.homeID = doc["teams"]["home"]["team"]["id"];
  gameData.homeScore = doc["teams"]["home"]["goals"];
  gameData.homeOther = doc["teams"]["home"]["powerPlay"];
  gameData.awayID = doc["teams"]["away"]["team"]["id"];
  gameData.awayScore = doc["teams"]["away"]["goals"];
  gameData.awayOther = doc["teams"]["away"]["powerPlay"];

  dPrintf(F("gameData.awayOther: %d\n"),gameData.awayOther);

  uint8_t period = doc["currentPeriod"];

  if (period == 0) {
        setGDStrings(gameData,"pre","");
  }
  else {
    const char* p = doc["currentPeriodOrdinal"];   // Json template issues if declartion isn't on same line
    const char* tr = doc["currentPeriodTimeRemaining"];

    if (strcmp(tr,STATUSCODE_FINAL) == 0) {
      setGDStrings(gameData,"","Final");
      isGameOver = true;
    }
    else {
      setGDStrings(gameData,p,tr);
    }
  }

  return isGameOver;

}

bool extractCurrentGame_NBA(CurrentGameData& currentGameData, JsonDocument& doc) {
  
  const char* id = doc["id"];
  currentGameData.gameID = atoi(id);
  currentGameData.league = NBA;

  const char* status = doc["status"]["type"]["name"];
  bool isFinal = (strcmp(status,STATUSCODE_NBA_FINAL) == 0);
  bool isStillScheduled = (strcmp(status,STATUSCODE_NBA_SCHEDULED) == 0);  // API doesn't show required field until game actually starts

  JsonArray competitors = doc["competitors"];
  for (JsonObject competitor: competitors) {   // there is aways 2
    const char* homeAway = competitor["homeAway"];
    if (strcmp(homeAway,"home") == 0) {
      currentGameData.homeID = competitor["id"];
      if (!isStillScheduled) {
        currentGameData.homeScore = competitor["score"];
      }
    }
    else {
      currentGameData.awayID = competitor["id"];
      if (!isStillScheduled) {
        currentGameData.awayScore = competitor["score"];
      }
    }
  }

  if (isFinal) {
    setGDStrings(currentGameData,"","FINAL");
  }
  else if (strcmp(status,STATUSCODE_NBA_HALFTIME) == 0) {
    setGDStrings(currentGameData,"","HALF");
  }
  else {
  
    if (!isStillScheduled) { 
    const int quater = doc["status"]["period"];
    
    char ordinalBuffer[4];
      switch (quater){
        case 1:
          snprintf(ordinalBuffer,sizeof(ordinalBuffer),"1st");
          break;
        case 2:
          snprintf(ordinalBuffer,sizeof(ordinalBuffer),"2nd");
          break;
        case 3:
          snprintf(ordinalBuffer,sizeof(ordinalBuffer),"3rd");
          break;
        case 4:
          snprintf(ordinalBuffer,sizeof(ordinalBuffer),"4th");
          break;
        default:
          snprintf(ordinalBuffer,sizeof(ordinalBuffer),"?");
      }

      snprintf(currentGameData.devision,sizeof(currentGameData.devision),"%s",ordinalBuffer);
      if (strcmp(status,STATUSCODE_NBA_ENDOFQUATER) == 0) {
        snprintf(currentGameData.timeRemaining,sizeof(currentGameData.timeRemaining),"END");  
      }
      else {
  
        const char* tr = doc["status"]["displayClock"];
        snprintf(currentGameData.timeRemaining,sizeof(currentGameData.timeRemaining),"%s",tr);  
      }
    }
    else {
      memset(currentGameData.timeRemaining,'\0',sizeof(currentGameData.timeRemaining));
      snprintf(currentGameData.devision,sizeof(currentGameData.devision),"pre");
    }
  } 

  return isFinal;

}

void extractNextGame_NBA(NextGameData& nextGameData, JsonDocument& doc) {

  nextGameData.gameID = doc["id"];
  JsonArray competitors = doc["competitions"][0]["competitors"];
  for (JsonObject competitor: competitors) {   // there is aways 2
    const char* homeAway = competitor["homeAway"];
    if (strcmp(homeAway,"home") == 0) {
      nextGameData.homeID = competitor["id"];
      JsonArray records = competitor["records"];
      for (JsonObject record : records) {
        const char* type = record["type"];
        if (strcmp(type,"total") == 0) {
          const char* r = record["summary"];
          snprintf(nextGameData.homeRecord,sizeof(nextGameData.homeRecord),"%s",r);
          break;
        }
      }
    }
    else {
      nextGameData.awayID = competitor["id"];
      JsonArray records = competitor["records"];
      for (JsonObject record : records) {
        const char* type = record["type"];
        if (strcmp(type,"total") == 0) {
          const char* r = record["summary"];
          snprintf(nextGameData.awayRecord,sizeof(nextGameData.homeRecord),"%s",r);
          break;
        }
      }
    }

    String nextGameTS_str = doc["date"];
    nextGameData.startTime = parseDateTime(nextGameTS_str);
    nextGameData.league = NBA;
   // int seasonType = doc["season"]["type"];
    //nextGameData.isPlayoffs = (seasonType == 3);
    nextGameData.isPlayoffs = false;

  }
  
}

void extractNextGame_NHLorMLB(NextGameData& nextGameData, JsonObject& game,const uint8_t league) {

  nextGameData.gameID = game["gamePk"];
  nextGameData.awayID = game["teams"]["away"]["team"]["id"];
  nextGameData.homeID = game["teams"]["home"]["team"]["id"];
  nextGameData.league = league;
  const char* gameType = game["gameType"];
  nextGameData.isPlayoffs = (strcmp(gameType,"P") == 0);
  String nextGameTS_str = game["gameDate"];
  nextGameData.startTime = parseDateTime(nextGameTS_str);
  JsonObject homeRecord = game["teams"]["home"]["leagueRecord"];
  JsonObject awayRecord = game["teams"]["away"]["leagueRecord"];
  uint8_t homeWins = homeRecord["wins"];
  uint8_t awayWins = awayRecord["wins"];
  uint8_t homeLosses = homeRecord["losses"];
  uint8_t awayLosses = awayRecord["losses"];

  if (league == NHL) {
    uint8_t homeOT = homeRecord["ot"];
    uint8_t awayOT = awayRecord["ot"];
    sprintf(nextGameData.homeRecord,"%d-%d-%d",homeWins,homeLosses,homeOT);
    sprintf(nextGameData.awayRecord,"%d-%d-%d",awayWins,awayLosses,awayOT);
  }
  else {
      sprintf(nextGameData.homeRecord,"%d-%d",homeWins,homeLosses);
      sprintf(nextGameData.awayRecord,"%d-%d",awayWins,awayLosses);
  }
}

// Awesomely the NHL & MLB use the same api schema
void getNextGame_NHLorMLB(const time_t today,const uint16_t teamID, const uint8_t league, NextGameData& nextGameData) {

  StaticJsonDocument<320> filter;
  DynamicJsonDocument doc(2048);
  JsonObject resultGame;

  String queryString;
  HTTPClient httpClient;
  WiFiClient wifiClient;
  int8_t gameCount = 0;
  uint32_t excludeGameID = nextGameData.gameID;

  nextGameData.gameID = 0;

  char* host;
  uint16_t port;

  // we just have to change the HOST everything else is the same between the two APIs
  if (league == NHL) {
    host = (char*)NHL_HOST;
    port = NHL_PORT;
  }
  else if (league == MLB) {
    host = (char*)MLB_HOST;
    port = MLB_PORT;
  }
  else {
    dPrintf(F("Invalid league: %d\n"),league);
    return;
  }

  queryString = "http://";
  queryString += host;
  queryString += "/api/v1/schedule?";
  queryString += "sportId=1";
  queryString += "&teamId=";
  queryString += teamID;
  queryString += "&startDate=";
  queryString += convertDate(today - SECONDS_IN_A_DAY,true); // need to grab from yesterday
  queryString += "&endDate=";
  // get 7 days worth of data to in order to cover the all star break and playoff gaps4
  queryString += convertDate(today + (SECONDS_IN_A_DAY * 7),true);

  dPrintf(F("Query - Type: Next %s Game\n"), LEAGUE_NAMES[league]);

  filter["games"][0]["gamePk"] = true;
  filter["games"][0]["gameType"] = true;
  filter["games"][0]["gameDate"] = true;
  filter["games"][0]["status"]["abstractGameState"] = true;
  filter["games"][0]["status"]["detailedState"] = true;
  
  filter["games"][0]["teams"]["home"]["team"]["id"] = true;
  filter["games"][0]["teams"]["home"]["leagueRecord"]["wins"] = true;
  filter["games"][0]["teams"]["home"]["leagueRecord"]["losses"] = true;
  filter["games"][0]["teams"]["away"]["team"]["id"] = true;
  filter["games"][0]["teams"]["away"]["leagueRecord"]["wins"] = true;
  filter["games"][0]["teams"]["away"]["leagueRecord"]["losses"] = true;
  if (league == NHL) {
    filter["games"][0]["teams"]["home"]["leagueRecord"]["ot"] = true;
    filter["games"][0]["teams"]["away"]["leagueRecord"]["ot"] = true;
  }

  serializeJsonPretty(filter,Serial);

  dPrintf(F("\nQuery URL: %s\n"),queryString.c_str());

  httpClient.useHTTP10(true); 
  httpClient.begin(wifiClient,queryString);
 
  int httpResult = httpClient.GET();
  if (httpResult != 200) {
    dPrintf(F("HTTP error: %d\n"),httpResult);
    httpClient.end();
    return;
  }

  bool found = false;

  // bug in NHL API that has spaces in the tag. If they use the same schema why are there spaces?
  if (league == NHL) {
    wifiClient.find("\"dates\" : [ ");
  }
  else {
    wifiClient.find("\"dates\":[");
  }
  do {
    DeserializationError err = deserializeJson(doc,wifiClient,DeserializationOption::Filter(filter));
    
    if (err) {
      dPrintf(F("Parse error: %s\n"),err.c_str());
      break;
    }
    
    JsonArray games = doc["games"];
    for (JsonObject game : games) {   // in case of doubleheaders 
      uint32_t gameID = game["gamePk"];
      const char* ags = game["status"]["abstractGameState"];
      const char* ds = game["status"]["detailedState"];
      dPrintf(F("GameID: %d Abstract: %s Detailed: %s"),gameID,ags,ds);
      gameCount++;
      if ((strcmp(ags,STATUSCODE_FINAL) != 0) && (gameID != excludeGameID)) {
        if ((strcmp(ds,STATUSCODE_INPROGRESS) == 0) || (strcmp(ds,STATUSCODE_SCHEDULED) == 0)) {
          dPrintln(F(" match"));
          found = true;
          resultGame = game;
          break;
        }
      }
      else {
        dPrintln(F(" no match"));
      }
    }
    if (found) {
      break;
    }
  } while (wifiClient.findUntil(",","]"));
  
  httpClient.end();

  if (found) {
    serializeJsonPretty(resultGame,Serial);
    
    extractNextGame_NHLorMLB(nextGameData,resultGame,league);
  }
  else {
    dPrintln(F("No next game found"));
  }

}

void getNextGame_NBA(const time_t today,const uint16_t teamID, NextGameData& nextGameData) {

  HTTPClient httpClient;
  WiFiClient wifiClient;
  StaticJsonDocument<368> filter;
  DynamicJsonDocument doc(2048);
 
  String queryString = "http://site.api.espn.com/apis/site/v2/sports/basketball/nba/scoreboard?limit=100&dates=";
  queryString += convertDate(today - SECONDS_IN_A_DAY,false);
  queryString += "-";
  queryString += convertDate(today + (SECONDS_IN_A_DAY * 3),false);
  
  dPrintln(F("Query - Type: Next NBA Game"));
  
  filter["id"] = true;
  filter["date"] = true;
  filter["competitions"][0]["competitors"][0]["id"] = true;
  filter["competitions"][0]["competitors"][0]["homeAway"] = true;
  filter["competitions"][0]["competitors"][0]["score"] = true;
  filter["competitions"][0]["competitors"][0]["records"][0]["type"] = true;
  filter["competitions"][0]["competitors"][0]["records"][0]["summary"] = true;
  filter["competitions"][0]["competitors"][1]["id"] = true;
  filter["competitions"][0]["competitors"][1]["homeAway"] = true;
  filter["competitions"][0]["competitors"][0]["score"] = true;
  filter["competitions"][0]["competitors"][1]["records"][0]["type"] = true;
  filter["competitions"][0]["competitors"][1]["records"][0]["summary"] = true;
  filter["status"]["type"]["name"] = true;

  serializeJsonPretty(filter,Serial);

  
  dPrintf(F("\nQuery URL: %s\n"),queryString.c_str());

  httpClient.useHTTP10(true);   // Very Important for NBA api to parse correctly
  httpClient.begin(wifiClient,queryString);

  int httpResult = httpClient.GET();
  if (httpResult != 200) {
    dPrintf(F("HTTP error: %d\n"),httpResult);
    httpClient.end();
    return;
  }
    
  bool found = false;

  wifiClient.find("\"events\":[");
  uint16_t event = 0;
  do {
    dPrintf(F("checking: %d\n"),event);
    DeserializationError err = deserializeJson(doc,wifiClient,DeserializationOption::Filter(filter),DeserializationOption::NestingLimit(15));
    
    if (err) {
      dPrintf(F("Parse error: %s"),err.c_str());
    }
    else {

      const char* status = doc["status"]["type"]["name"];
      
      if (strcmp(status,STATUSCODE_NBA_FINAL) != 0) {
        const int team1 = doc["competitions"][0]["competitors"][0]["id"];
        const int team2 = doc["competitions"][0]["competitors"][1]["id"];
        if ((team1 == teamID) || (team2 == teamID)) {
          const uint32_t id = doc["id"];
          if (id != nextGameData.gameID) {
            found = true;
            break;
          }  
        }
      }
    }
    event++;
  } while (wifiClient.findUntil(",","]"));

  httpClient.end();
  
  if (found) {
    serializeJsonPretty(doc,Serial);
    extractNextGame_NBA(nextGameData,doc);
  }
  else {
    Serial.println(F("No next game found"));
  }

}

void getNextGame(const time_t today,const uint16_t teamID, const uint8_t league, NextGameData& nextGameData) {

  if (league == NBA) {
    getNextGame_NBA(today,teamID,nextGameData);
  }
  else {
    getNextGame_NHLorMLB(today,teamID,league,nextGameData);   // NHL & MLB use the same api schema
  }
}

bool switchOneValue() {
  return digitalRead(SWITCH_PIN_1);
}

void getNextGame() {

  uint16_t tID = 0;
  getNextGame(currentTime(),selectedTeam[currentLeague],currentLeague,nextGameData);
  printNextGame(nextGameData);

}

// action
// 1 = turn on
// 0 = turn off
// -1 = toggle
void tftSet(int8_t action) {
  static bool ledState = false;

  if (action == -1) {
    ledState = !ledState;
  }
  else {
    ledState = action;
  }
  digitalWrite(TFT_BACKLIGHT_PIN,ledState);
  dPrintf(F("LED backlight: %s\n"),ledState ? "ON" : "OFF"); 
}

void displayNextGame(NextGameData& nextGameData) {

  tftSet(digitalRead(SWITCH_PIN_1));

  tft.fillScreen(TFT_WHITE);
  tft.setTextColor(TFT_BLACK);

  if (nextGameData.gameID == 0) {  
    displaySingleLogo(selectedTeam[currentLeague],currentLeague);
    tft.drawString("No games scheduled",(tft.width() - tft.textWidth("No games scheduled",2))/2,95,2);
  }
  else {
    displayTeamLogos(nextGameData.awayID,nextGameData.homeID,nextGameData.league);

    if (!(nextGameData.isPlayoffs)) {
      tft.drawString(nextGameData.awayRecord,35 - (tft.textWidth(nextGameData.awayRecord)/2),65);
      tft.drawString(nextGameData.homeRecord,125 - (tft.textWidth(nextGameData.homeRecord)/2),65);
    }

    char buffer[12];       // fit: Mon, Jan 23
    
    strftime(buffer,sizeof(buffer),"%a, %b %e",localtime(&(nextGameData.startTime)));
    tft.drawString(buffer,TFT_HALF_WIDTH - (tft.textWidth(buffer,4)/2),85,4);

    strftime(buffer,sizeof(buffer),"%H:%M",localtime(&(nextGameData.startTime)));
    tft.drawString(buffer,TFT_HALF_WIDTH - (tft.textWidth(buffer,2)/2),110,2);
    tft.drawString("VS",72,20,2);
  }

}



bool gameStatsChanged(CurrentGameData& prev, CurrentGameData& curr) {

  bool result = false;

  if (prev.gameID != curr.gameID) { return true; }
  if (prev.awayID != curr.awayID) { return true; }
  if (prev.homeID != curr.homeID) { return true; }
  if (prev.awayScore != curr.awayScore) { return true; }
  if (prev.homeScore != curr.homeScore) { return true; }
  if (strcmp(prev.devision,curr.devision) != 0) { return true; }
  if (strcmp(prev.timeRemaining,curr.timeRemaining) != 0) { return true; }
  if (curr.league == MLB) {
      if (prev.outs != curr.outs) { return true; }
      if (prev.bases[0] != curr.bases[0]) { return true; }
      if (prev.bases[1] != curr.bases[1]) { return true; }
      if (prev.bases[2] != curr.bases[2]) { return true; }
  }
  return false;
}

void copyGameData(CurrentGameData& dest, CurrentGameData& source) {
  dest.gameID = source.gameID;
  dest.homeID = source.homeID;
  dest.awayID = source.awayID;
  dest.homeScore = source.homeScore;
  dest.awayScore = source.awayScore;
  dest.homeOther = source.homeOther;
  dest.awayOther = source.awayOther;
  strcpy(dest.devision,source.devision);
  strcpy(dest.timeRemaining,source.timeRemaining);
  dest.league = source.league;
  dest.outs = source.outs;
  dest.bases[0] = source.bases[0];
  dest.bases[1] = source.bases[1];
  dest.bases[2] = source.bases[2];
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
  return awayScorePosition(theScore) + 90;
}

int16_t calculateScoreXPosition(const int16_t score, const bool bigFont) {

  int16_t position = 0;

  if (bigFont) {
    if (score == 1) {
      position = 8;
    }
    else if (score >= 100) {   // shouldn't happen
      position = 12;
    }
    else if (score >= 20) {
      position = 2;
    }
    else if (score >= 10) {
      position = -8;
    }
    else {
      position = 18;
    }
  }
  else {
    if (score == 1) {
      position = 28;
    }
    else if (score >= 100) {
      position = 13;
    }
    else if (score >= 20) {
      position = 2;
    }
    else if (score >= 10) {
      position = 20;
    }
    else {
      position = 20;
    }
  }


 
  return position;
}

void calculateScoreXPosition(const uint8_t awayScore, const uint8_t homeScore, int16_t& awayPosition, int16_t& homePosition, int16_t& fontNum) {

  bool bigFont = ((awayScore < 100) && (homeScore < 100));
  fontNum = bigFont ? 7 : 4;
  awayPosition = calculateScoreXPosition(awayScore,bigFont);
  homePosition = calculateScoreXPosition(homeScore,bigFont) + 90;
  
}

// hacky code that assumed TFT screen size and uses magic numbers
// Won't look right on other size screens
void displayCurrentGame(CurrentGameData& gameData) {

  char score[4];
  int16_t awayPosX; int16_t homePosX; int16_t font;

  tftSet(true);
  tft.fillScreen(TFT_WHITE);
  tft.setTextColor(TFT_BLACK);

  printCurrentGame(gameData);
  displayTeamLogos(gameData.awayID,gameData.homeID,gameData.league);

  calculateScoreXPosition(gameData.awayScore,gameData.homeScore,awayPosX,homePosX,font);

  memset(score,'\0',sizeof(score));
  snprintf(score,sizeof(score),"%d",gameData.awayScore);


  tft.drawString(score,awayPosX,70,font);
  memset(score,'\0',sizeof(score));
  snprintf(score,sizeof(score),"%d",gameData.homeScore);

  tft.drawString(score,homePosX,70,font);

  tft.drawString("VS",TFT_HALF_WIDTH - (tft.textWidth("VS",2)/2),30,2);

  const uint8_t basePosX = 70;
  const uint8_t basePosY = 70;

  if (gameData.league == NHL){
    if (gameData.awayOther == 1) {
      tft.drawString("PP",basePosX-21,basePosY,1);
    }
    
    if (gameData.homeOther == 1) {
      tft.drawString("PP",basePosX+30,basePosY,1);
    }
  }

  tft.drawString(gameData.devision,basePosX,basePosY,2);
  
  if ((gameData.league != MLB) || (strcmp(gameData.timeRemaining,"FINAL") == 0))  {
    tft.drawString(gameData.timeRemaining,TFT_HALF_WIDTH - (tft.textWidth(gameData.timeRemaining,2)/2),112,2);
  }
  else {
    // 3rd base
    if (gameData.bases[2]) {
      tft.fillTriangle(basePosX-5,basePosY+30,basePosX+5,basePosY+30,basePosX,basePosY+25,TFT_BLACK);
      tft.fillTriangle(basePosX-5,basePosY+30,basePosX+5,basePosY+30,basePosX,basePosY+35,TFT_BLACK);
    }
    else {
      tft.drawTriangle(basePosX-5,basePosY+30,basePosX+5,basePosY+30,basePosX,basePosY+25,TFT_BLACK);
      tft.drawTriangle(basePosX-5,basePosY+30,basePosX+5,basePosY+30,basePosX,basePosY+35,TFT_BLACK);
      tft.drawLine(basePosX-5+1,basePosY+30,basePosX+5-1,basePosY+30,TFT_WHITE);
    }

    // 1st base
    if (gameData.bases[0]) {
      tft.fillTriangle(basePosX+15,basePosY+30,basePosX+25,basePosY+30,basePosX+20,basePosY+25,TFT_BLACK);
      tft.fillTriangle(basePosX+15,basePosY+30,basePosX+25,basePosY+30,basePosX+20,basePosY+35,TFT_BLACK);
    }
    else {
      tft.drawTriangle(basePosX+15,basePosY+30,basePosX+25,basePosY+30,basePosX+20,basePosY+25,TFT_BLACK);
      tft.drawTriangle(basePosX+15,basePosY+30,basePosX+25,basePosY+30,basePosX+20,basePosY+35,TFT_BLACK);
      tft.drawLine(basePosX+15+1,basePosY+30,basePosX+25-1,basePosY+30,TFT_WHITE);
    }
    // 2nd base
    if (gameData.bases[1]) {
      tft.fillTriangle(basePosX+5,basePosY+22,basePosX+15,basePosY+22,basePosX+10,basePosY+17,TFT_BLACK);
      tft.fillTriangle(basePosX+5,basePosY+22,basePosX+15,basePosY+22,basePosX+10,basePosY+27,TFT_BLACK);
    }
    else {
      tft.drawTriangle(basePosX+5,basePosY+22,basePosX+15,basePosY+22,basePosX+10,basePosY+17,TFT_BLACK);
      tft.drawTriangle(basePosX+5,basePosY+22,basePosX+15,basePosY+22,basePosX+10,basePosY+27,TFT_BLACK);
      tft.drawLine(basePosX+5+1,basePosY+22,basePosX+15-1,basePosY+22,TFT_WHITE);
    }
    if (strcmp(gameData.timeRemaining,"top") == 0) {
      tft.fillTriangle(basePosX-9,basePosY+10,basePosX+1,basePosY+10,basePosX-4,basePosY+5,TFT_BLACK);
    }
    else {
      tft.fillTriangle(basePosX-9,basePosY+5,basePosX+1,basePosY+5,basePosX-4,basePosY+10,TFT_BLACK);
    }

    for (uint8_t i = 1; i <= gameData.outs; i++) {
      tft.fillCircle(basePosX-6 + (8 * i),basePosY+41,3,TFT_BLACK);
    }
    for (uint8_t i = gameData.outs + 1; i <= 3; i++) {
      tft.drawCircle(basePosX-6 + (8 * i),basePosY+41,3,TFT_BLACK);
    }

  }

}


bool getAndDisplayCurrentMLBGame(const uint32_t gameID, CurrentGameData& prevUpdate) {

  StaticJsonDocument<512> filter;
  DynamicJsonDocument doc(2048);

  HTTPClient httpClient;
  WiFiClient wifiClient;
  String queryString;

  CurrentGameData gameData;
  bool isGameOver = false;

  dPrintln(F("Query - Type: Current Game"));

  queryString = "http://";
  queryString += MLB_HOST;
  queryString += "/api/v1.1/game/";
  queryString += gameID;
  queryString += "/feed/live";

  filter["gamePk"] = true;
  filter["gameData"]["status"]["abstractGameState"] = true;
  filter["liveData"]["linescore"]["currentInning"] = true;
  filter["liveData"]["linescore"]["currentInningOrdinal"] = true;
  filter["liveData"]["linescore"]["isTopInning"] = true;
  filter["liveData"]["linescore"]["teams"]["home"]["runs"] = true;
  filter["liveData"]["linescore"]["teams"]["away"]["runs"] = true;
  filter["liveData"]["linescore"]["offense"] = true;
  filter["liveData"]["linescore"]["outs"] = true;
  filter["liveData"]["boxscore"]["teams"]["home"]["team"]["id"] = true;
  filter["liveData"]["boxscore"]["teams"]["away"]["team"]["id"] = true;

  serializeJsonPretty(filter,Serial);
  
  dPrintf(F("\nQuery URL: %s\n"),queryString.c_str());

  httpClient.useHTTP10(true);
  httpClient.begin(wifiClient,queryString);

  int httpResult = httpClient.GET();
  if (httpResult != 200) {
    dPrintf(F("HTTP error: %d\n"),httpResult);
    httpClient.end();
    permanentError(F("HTTP error: %s\n"),httpResult);
  }
  
  DeserializationError err = deserializeJson(doc,wifiClient,DeserializationOption::Filter(filter),DeserializationOption::NestingLimit(14));
 
  httpClient.end();
  
  if (err) {
    permanentError(F("%s"),err.c_str());
  }

  serializeJsonPretty(doc,Serial);

  isGameOver = extractCurrentGame_MLB(gameData,doc);

  printCurrentGame(gameData);

  if (gameStatsChanged(prevUpdate,gameData)) {
    copyGameData(prevUpdate,gameData);
    displayCurrentGame(prevUpdate);
  }

  return isGameOver;

}

bool getAndDisplayCurrentNHLGame(const uint32_t gameID, CurrentGameData& prevUpdate) {

  StaticJsonDocument<256> filter;
  StaticJsonDocument<512> doc;

  HTTPClient httpClient;
  WiFiClient wifiClient;
  String queryString;

  CurrentGameData gameData;
  bool isGameOver = false;

  dPrint(F("Query - Type: Current Game NHL\n"));

  queryString = "http://";
  queryString += NHL_HOST;
  queryString += "/api/v1/game/";
  queryString += gameID;
  queryString += "/linescore";

  filter["currentPeriod"] = true;
  filter["currentPeriodOrdinal"] = true;
  filter["currentPeriodTimeRemaining"] = true;
  filter["teams"]["home"]["team"]["id"] = true;
  filter["teams"]["home"]["goals"] = true;
  filter["teams"]["home"]["powerPlay"] = true;
  filter["teams"]["away"]["team"]["id"] = true;
  filter["teams"]["away"]["goals"] = true;
  filter["teams"]["away"]["powerPlay"] = true;
  
  serializeJsonPretty(filter,Serial);

  dPrintf(F("\nQuery URL: %s\n"),queryString.c_str());

  httpClient.useHTTP10(true);
  httpClient.begin(wifiClient,queryString);

  int httpResult = httpClient.GET();
  if (httpResult != 200) {
    dPrintf(F("HTTP error: %d\n"),httpResult);
    httpClient.end();
    permanentError(F("HTTP error: %d\n"),httpResult);
  }

  DeserializationError err = deserializeJson(doc,wifiClient,DeserializationOption::Filter(filter));
  httpClient.end();

  serializeJsonPretty(doc,Serial);

  if (err) {
    permanentError(F("%s"),err.c_str());
  }

  isGameOver = extractCurrentGame_NHL(gameData,gameID,doc);

  printCurrentGame(gameData);

  if (gameStatsChanged(prevUpdate,gameData)) {
    copyGameData(prevUpdate,gameData);
    displayCurrentGame(prevUpdate);
  }

  return isGameOver;
}

bool getAndDisplayCurrentNBAGame(const uint32_t gameID, CurrentGameData& prevUpdate) {

  HTTPClient httpClient;
  WiFiClient wifiClient;

  String queryString;
  StaticJsonDocument<224> filter;
  StaticJsonDocument<512> doc;
  
  CurrentGameData gameData;
  bool isGameOver = false;

  
  dPrint(F("Query - Type: Current Game NBA\n"));

  filter["id"] = true;
  filter["competitors"][0]["id"] = true;
  filter["competitors"][0]["homeAway"] = true;
  filter["competitors"][0]["score"] = true;
  
  filter["competitors"][1]["id"] = true;
  filter["competitors"][1]["homeAway"] = true;
  filter["competitors"][0]["score"] = true;
  
  filter["status"]["displayClock"] = true;
  filter["status"]["period"] = true;
  filter["status"]["type"]["name"] = true;

  serializeJsonPretty(filter,Serial);

  queryString = "http://site.api.espn.com/apis/site/v2/sports/basketball/nba/summary?event=";
  queryString += gameID;

  dPrintf(F("\nQuery URL: %s\n"),queryString.c_str());

  httpClient.useHTTP10(true);   // Very Important for NBA api to parse correctly
  httpClient.begin(wifiClient,queryString);

  int httpResult = httpClient.GET();
  if (httpResult != 200) {
    httpClient.end();
    permanentError(F("HTTP error: %d\n"),httpResult);
  }

  
  wifiClient.find("\"competitions\":[");

  DeserializationError err = deserializeJson(doc,wifiClient,DeserializationOption::Filter(filter),DeserializationOption::NestingLimit(11));

  if (err) {
    dPrintf(F("Parse Error: %s\n"),err.c_str());
  }
  else {
    serializeJsonPretty(doc,Serial);
  }

  isGameOver = extractCurrentGame_NBA(gameData,doc);
  printCurrentGame(gameData);

  if (gameStatsChanged(prevUpdate,gameData)) {
    copyGameData(prevUpdate,gameData);
    displayCurrentGame(prevUpdate);
  }

  return isGameOver;
  
}


// "sleep" function. We need to fake sleep to monitor for button interrupts
// this makes power consumption bad, but whatever

// return true iff time expired. Otherwise (button pressed) return false
bool sleep(uint32_t timeInSeconds) {

  uint32_t wakupTime = millis() + (timeInSeconds * 1000);
  dPrintf(F("Sleeping for %d seconds @ millis: %d\n"),timeInSeconds,millis());

  while (millis() < wakupTime) {
    if (switchTeamsFlag) {
      switchTeamsFlag = false;
      dPrintln(F("Select button interrupt\n"));
      nextGameData.gameID = 0;
      selectTeam();
      gameStatus = NEW_TEAM;
      return false;
    }
    delay(50);
  }

  char buffer[20];
  dPrintln(F("Woke up"));
  time_t t = currentTime();
  strftime(buffer,sizeof(buffer),"%Y/%m/%d %H:%M:%S",localtime(&t));
  dPrintf(F("Local time: %s\n"),buffer);

  return true;

}

void sleepForever() {
  
  while (sleep(60 * 60 * 24)) {}
   
}

void ICACHE_RAM_ATTR ledSwitchInterrupt() {

    if (digitalRead(SWITCH_PIN_1)) {
      tftSet(HIGH);
    }
    else {
      if (gameStatus != STARTED) {
        tftSet(LOW);
      }
    }
}

void cfgUpdate_onStart() {
  dPrintln(F("CFG update started"));
  tftMessage(F("CFG update started"));
}

void cfgUpdate_onEnd() {
  dPrintln(F("Data files download complete"));
  tftMessage(F("Downloading data\n\nprogress: complete\n\nrestarting..."));
  delay(1000);
  ESP.restart();
}

void cfgUpdate_onProgress(int cur, int total) {
  dPrintf(F("Data update progress %d of %d bytes\n"),cur,total);
  tftMessage(F("Downloading data\n\nprogress: %d%%"),(cur*100)/total);
}

void cfgUpdate_onError(int err) {
  dPrintf(F("Data update fatal error code: %d\n"),err);
}

void performCFGUpdate(const uint32_t version) {

  WiFiClientSecure wificlient;
  wificlient.setInsecure();

  String queryString = FW_URL;
  queryString += PROJECT_NAME;
  queryString += CFG_PREFIX;
  queryString += version;
  queryString += CFG_EXT;

  dPrintf(F("Data Image to install: %s\n"),queryString.c_str());
  dPrint(F("\nDownloading... DON'T TURN OFF!\n"));
  tftMessage(F("Downloading...\nDON'T TURN OFF!"));

  ESPhttpUpdate.onStart(cfgUpdate_onStart);
  ESPhttpUpdate.onEnd(cfgUpdate_onEnd);
  ESPhttpUpdate.onProgress(cfgUpdate_onProgress);
  ESPhttpUpdate.onError(cfgUpdate_onError);

  t_httpUpdate_return ret = ESPhttpUpdate.updateFS(wificlient,queryString);
  switch(ret) {
    case HTTP_UPDATE_FAILED:
      dPrintf(F("HTTP_UPDATE_FAILED Error (%d): %s\n"), ESPhttpUpdate.getLastError(), ESPhttpUpdate.getLastErrorString().c_str());
      break;
    case HTTP_UPDATE_NO_UPDATES:
      dPrint(F("HTTP_UPDATE_NO_UPDATES\n"));
      break;
    case HTTP_UPDATE_OK:
      dPrintf(F("HTTP Update OK"));
  }
  
}

uint32_t getFSVer() {
  File file = LittleFS.open(CFG_VERSION_FILENAME,"r");
  uint32_t result = 0;

  if (!file) {
    dPrint(F("CFG Version read error\n"));
  }
  else {
    result = file.parseInt();
  }

  file.close();
  return result;

}

uint32_t checkForCFGUpdate() {

  String queryString;
  WiFiClientSecure wificlient;
  wificlient.setInsecure();
  HTTPClient httpClient;
  uint32_t currentVersion = 0;
  
  currentVersion = getFSVer();

  queryString = FW_URL;
  queryString += PROJECT_NAME;
  queryString += CFG_VERSION_FILENAME;

  dPrint(F("Query Type: CFG files Check\n"));
  dPrintf(F("Query URL: %s\n"),queryString.c_str());

  httpClient.begin(wificlient, queryString);

  int httpCode = httpClient.GET();
  uint32_t availableVersion = 0;
  if (httpCode == 200) {
    availableVersion = httpClient.getString().toInt();
    dPrintf(F("Data files Version Current:  %d\n"),currentVersion);
    dPrintf(F("Data files Version Available: %d\n"),availableVersion);
    dPrintf(F("New data files available?:  %s\n"), (availableVersion > currentVersion) ? "Yes" : "No");
  }
   else {
    dPrintf(F("CFG HTTP Error: %d\n"),httpCode);
  }
  httpClient.end();

  return (availableVersion > currentVersion) ? availableVersion : 0;
  
}

void fwUpdate_onStart() {
  dPrintln(F("FW update started"));
  tftMessage(F("FW update started"));
}

void fwUpdate_onEnd() {
  dPrintln(F("FW download complete"));
  tftMessage(F("Downloading firmware\n\nprogress: complete\n\nrestarting..."));
  delay(1000);
  ESP.restart();
}

void fwUpdate_onProgress(int cur, int total) {
  dPrintf(F("FW update progress %d of %d bytes\n"),cur,total);
  tftMessage(F("Downloading firmware\n\nprogress: %d%%"),(cur*100)/total);
}

void fwUpdate_onError(int err) {
  dPrintf(F("FW update fatal error code: %d\n"),err);
}

void performFWUpdate(const uint32_t version) {

    WiFiClientSecure wificlient;
    wificlient.setInsecure();

    String queryString = FW_URL;
    queryString += PROJECT_NAME;
    queryString += FW_PREFIX;
    queryString += version;
    queryString += FW_EXT;

    dPrintf(F("FW Image to install: %s\n"),queryString.c_str());
    dPrint(F("\nDownloading... DON'T TURN OFF!\n"));
    tftMessage(F("Downloading...\nDON'T TURN OFF!"));

    ESPhttpUpdate.onStart(fwUpdate_onStart);
    ESPhttpUpdate.onEnd(fwUpdate_onEnd);
    ESPhttpUpdate.onProgress(fwUpdate_onProgress);
    ESPhttpUpdate.onError(fwUpdate_onError);
 
    t_httpUpdate_return ret = ESPhttpUpdate.update(wificlient,queryString);
    switch(ret) {
      case HTTP_UPDATE_FAILED:
        dPrintf(F("HTTP_UPDATE_FAILED Error (%d): %s\n"), ESPhttpUpdate.getLastError(), ESPhttpUpdate.getLastErrorString().c_str());
        break;
      case HTTP_UPDATE_NO_UPDATES:
        dPrint(F("HTTP_UPDATE_NO_UPDATES\n"));
        break;
      case HTTP_UPDATE_OK:
        dPrintf(F("HTTP Update OK"));
    }
    
}

uint32_t checkForFWUpdate() {

  // Github forces https which causes issue with redirect for httpClient
  // easiest way around is to just use setInsecure
  // 

  HTTPClient httpClient;
  WiFiClientSecure wificlient;
  wificlient.setInsecure();
  if (!wificlient.connect(FW_HOST,443)) {
    dPrint(F("FW check failed!"));
    wificlient.stop();
    return 0;
  }
  
  String queryString = FW_URL;
  queryString += PROJECT_NAME;
  queryString += FW_VERSION_FILENAME;
  dPrint(F("Query Type: Fireware Check\n"));
  dPrintf(F("Query URL: %s\n"),queryString.c_str());

  httpClient.begin(wificlient, queryString);

  int httpCode = httpClient.GET();
  uint32_t availableVersion = 0;
  if (httpCode == 200) {
    availableVersion = httpClient.getString().toInt();
    dPrintf(F("FW Version Current:   %d\n"),CURRENT_FW_VERSION);
    dPrintf(F("FW Version Available: %d\n"),availableVersion);
    dPrintf(F("FW Update Available:  %s\n"), (availableVersion > CURRENT_FW_VERSION) ? "Yes" : "No");
  }
  else {
    dPrintf(F("FW HTTP Error: %d\n"),httpCode);
  }
  httpClient.end();

  return (availableVersion > CURRENT_FW_VERSION) ? availableVersion : 0;

}

void checkForUpdates() {
  
  tftMessage(F("Checking for updates..."));
  uint32_t ver = checkForFWUpdate();
  if (ver) {
    tftMessage(F("FW update available\n\n\nPress button to continue"));
    while (!debouncer.rose()) {
      debouncer.update();
      yield();
    }
    performFWUpdate(ver);
  }
  ver = checkForCFGUpdate();
  if (ver) {
    tftMessage(F("New data files available\n\n\nPress buttton to continue"));
    while (!debouncer.rose()) {
      debouncer.update();
      yield();
    }
    performCFGUpdate(ver);
  }
}

void setup() {

  dBegin(115200);
  dPrint(F("TFT Sports Scoreboard\n"));

  tft.init(INITR_BLACKTAB);
  tft.setRotation(TFT_ROTATION);
  tft.fillScreen(TFT_BLACK);

  if (!LittleFS.begin()) {
    dPrint(F("FS initialisation failed!\n"));
    permanentError(F("LittleFS Error"));
  }
  dPrint(F("\n\LittleFS initialised.\n"));

  dPrintf(F("Firmware Version: %d\n"),CURRENT_FW_VERSION);
  uint32_t fsVer = getFSVer();
  dPrintf(F("Filesystem Version: %d\n"),fsVer);
  tftMessage(F("TFT Sports Scoreboard\n\nFW Ver: %d\nFS Ver: %d\n\nConnecting to WiFi..."),CURRENT_FW_VERSION,fsVer);

  pinMode(SWITCH_PIN_1,INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(SWITCH_PIN_1),ledSwitchInterrupt,CHANGE);
  
  pinMode(TFT_BACKLIGHT_PIN,OUTPUT);
  tftSet(1);

  debouncer.attach(SELECT_BUTTON_PIN,INPUT_PULLUP);
  debouncer.interval(DEBOUNCE_INTERVAL);

 // wifiManager.resetSettings();
  wifiManager.setAPCallback(wifiConfigCallback);
  wifiConnect();

  tftMessage(F("Fetching time..."));
  updateTime();

  checkForUpdates();

  // load the list of NHL teams
  teamListInit();

  // GUI selection of favourite team if button is being pressed
  debouncer.update();

  if (!loadTeams() || (debouncer.read() == LOW)) {
      selectTeam();
  }
  else {
    setInterrupt(true);
  }

}

void loop() {

  static time_t waitUntil = 0;
  static uint32_t gameFinishedTime = 0;

  //dPrintf(F("ESP Free Heap: %d Frag: %d%% Max Block: %d\n"),ESP.getFreeHeap(),ESP.getHeapFragmentation(),ESP.getMaxFreeBlockSize());

  if (gameStatus == NEW_TEAM) {
    tftMessage(F("Fetching next game..."));
    getNextGame();
      if (nextGameData.gameID == 0) {
        gameStatus = NO_GAMES;
      }
      else {
        gameStatus = SCHEDULED;
      }
    displayNextGame(nextGameData);
  }
  else if (gameStatus == NO_GAMES) {
    sleepForever();
  }
  else if (gameStatus == STARTED) {
    if ((currentLeague == NHL) && (getAndDisplayCurrentNHLGame(nextGameData.gameID,currentGameData))) {
      gameStatus = FINISHED;
      gameFinishedTime = millis();
    }
    else if ((currentLeague == MLB) && (getAndDisplayCurrentMLBGame(nextGameData.gameID,currentGameData))) {
       gameStatus = FINISHED;
       gameFinishedTime = millis();
    }
    else if ((currentLeague == NBA) && (getAndDisplayCurrentNBAGame(nextGameData.gameID,currentGameData))) {
      gameStatus = FINISHED;
      gameFinishedTime = millis();
    }
    else {
        sleep(GAME_UPDATE_INTERVAL);
    }
  }
  else if (gameStatus == AFTER_GAME) {
    debouncer.update();
    if (debouncer.rose() || (currentTime() > nextGameData.startTime) || ((millis() - gameFinishedTime) > AFTER_GAME_RESULTS_DURATION_MS))   {
      displayNextGame(nextGameData);
      if (nextGameData.gameID == 0) {
        gameStatus = NO_GAMES;
      }
      else {
        gameStatus = SCHEDULED;
      }
      //ledSwitchInterrupt();
    }
  }
  else if (currentTime() > nextGameData.startTime) {
    if (gameStatus == FINISHED) {
      getNextGame();
      gameStatus = AFTER_GAME;
    }
    else {
      gameStatus = STARTED;
    }
  }
  else if (nextGameData.startTime > currentTime()) {
    sleep((uint32_t)(nextGameData.startTime - currentTime()));
  }

  updateTime();
  yield();

}

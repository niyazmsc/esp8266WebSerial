/* 
  ESP_WebConfig 
  http://www.john-lassen.de/index.php/projects/esp-8266-arduino-ide-webconfig
  Copyright (c) 2015 John Lassen. All rights reserved.
  This is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.
  This software is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.
  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

  Latest version: 1.2.0  - 2016-09-xx by James Newton

Prior version had problems. First, the little ESP is doing good to deliver one page, but making every click load that 
page, and the styles, and the js, and the icon, and the values is just too much. 

It wouldn't be so bad if we could enable casheing of the pages, but that requires a nasty conversion of the time for 
the Last-Modified header value. Actually... maybe it doesn't... it would appear that a fixed, fake, Last-Modified with
a fixed Cache-Control: max-age=### works ok. At least in Chrome.
    server.sendHeader ( "Last-Modified", "Wed, 25 Feb 2015 12:00:00 GMT" );  
    server.sendHeader ( "Cache-Control", "max-age=86400" );  
Actually, the Last-Modified doesn't seem to be needed... Chrome gets by fine with just max-age. Good enough for now.

Putting each page all in one string would be more reliable, but wastes gobs of memory. 

Or we could make ONE page with everything and sections in tabs:
http://callmenick.com/post/simple-responsive-tabs-javascript-css
that still leaves us with an ajax request for all the data.
  
Building up a single page from multiple constant strings wastes ram and sending page parts is more complex with 
the server.send code because the total content length must be known. But perhaps we can do chunked content? By 
using CONTENT_LENGTH_UNKNOWN and then multiple calls to server.sendContent ala:
http://www.esp8266.com/viewtopic.php?p=34858&sid=e4749ea6b5cca73257f7829adb682f09#p34858
https://github.com/esp8266/Arduino/tree/master/libraries/ESP8266WebServer/examples/SDWebServer
This requires that we assume HTTP 1.1 clients... shouldn't be an issue.
Why doesn't that code have to close the connection? E.g. how does server know content is finished? Should it
use server.close()? Opened an issue about it. 
https://github.com/esp8266/Arduino/issues/2481

Added serial data flow from simple web form to serial port and from serial port to web via regular polling.
Debug msgs and startup garbage from bootloader are on the standard serial lines TX GPIO1, RX GPIO3. 
Need to keep actual serial data seperated. Use this serial.swap() to move to different pins, TX GPIO15, RX GPIO13 
and connect /those/ pins to the device.
http://arduino.esp8266.com/versions/1.6.5-1160-gef26c5f/doc/reference.html#serial
the original pins get connected to the bootloader for firmware updates only. 
TODO: Change debug(msg) and debugln(msg) in helpers.h file to check that no serial data is being sent, 
do the swap, serial.send, wait for completion serial.flush, and then swap back. 

DB9
2 BRN TX (driven by logger)
3 RED RX (listened to by logger)
4 ORG
5 YEL GND

  
  -----------------------------------------------------------------------------------------------
  History

  Latest Lassen version: 1.1.3  - 2015-07-20
  Changed the loading of the Javascript and CCS Files, so that they will successively loaded 
  and that only one request goes to the ESP.


  Version: 1.1.2  - 2015-07-17
  Added URLDECODE for some input-fields (SSID, PASSWORD...)

  Version  1.1.1 - 2015-07-12
  First initial version to the public
  
  */

#define SERIAL_ENABLE_PIN 5
#define WAS_BLINK 4
#define CLEAR_BLINK 2
#define TX2 15
#define RX2 13
#define RTS_OUT 14 //not used. May become cable disconnect detect?

//#define BAUD_RATE 9600
#define BAUD_RATE 38400

#define AdminTimeOut 0
//600
// Defines the Time in Seconds, when the Admin-Mode will be diabled
//set to 0 to never disable

#define ACCESS_POINT_NAME  "MassMind.org@192.168.4.1"
#define ACCESS_POINT_PASSWORD  "192.168.4.1" 
// ACCESS_POINT_IP 192.168.4.1
/*
Section 7.3.2.1 of the 802.11-2007 specification 
http://standards.ieee.org/getieee802/download/802.11-2007.pdf 
defines a valid SSID as 0-32 octets with arbitrary contents. 
*/


#include <ESP8266WiFi.h>
//#include <WiFiClient.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <Ticker.h>
#include <EEPROM.h>
#include <WiFiUdp.h>
//#include "ESP_Sleep.h"
#include "base64.h"
extern "C" {
#include <user_interface.h> //Required for getResetInfoPtr()
}

#include "helpers.h"
#include "global.h"
/*
Include the HTML, STYLE and Script "Pages"
*/
#include "Page_Root.h"
#include "Page_Admin.h"
#include "Page_Script.js.h"
#include "Page_Style.css.h"
#include "Page_NTPsettings.h"
#include "Page_Information.h"
#include "Page_General.h"
#include "PAGE_NetworkConfiguration.h"
#include "example.h"

#define STREAM_BUF_LINES 5
#define STREAM_MAX_CONFIDENCE 10

//extern "C" int __get_rf_mode(void); causes "unknown function error"
#define MICROSECONDS 1000000

/****** Global Variables *****/
//anything defined inside this struct will be saved to RTC memory on DeepSleep and should survive the restart
//these variables can change constantly and will not wear out the eeprom.
struct {
  float count;
  bool RFon;
} rtcmem;
static_assert (sizeof(rtcmem) < 512, "RTC can't hold more than 512 bytes");
rst_info *reset;

String DeviceID = "????????"; //compressed unique id for logging to server. 
//Will be made from MAC ID Base64 Encoded with URL safe character set

byte streaming=0;
String streamURL = "";
int streamLine = 1;
String streamBuf[STREAM_BUF_LINES];
int streamBufLine = 0;

boolean xoff=false; //flag to see if we need to hold off xmit until device is ready
boolean havedata = false; //flag to indicate data in rxbuf so we don't have to check length each loop.
String rxbuf = ""; //buffer for data recieved from device.
#define RFBUF_MAX 128 


//If Xoff, recieve bytes until Xon before sending byte.
void putc_x(byte b) {
  while (Serial.available() || xoff) {
    byte c = Serial.read(); 
    //can't peek, because xoff could be behind next char
    if (c == 0x13) { xoff=true; } // XOFF
    else if (c == 0x11) { xoff=false; } // XON
    else if (c > 0 && c < 0xFF) { //rx data
      rxbuf += c; //buffer it
      //TODO: may need to check rxbuf.length() and send an xoff if too big.
      havedata=true;
      }
    delay(1); //should this be more?
    //TODO: timeout?
    }
  Serial.write(b);
  }


//If Xoff, recieve bytes until Xon before sending string.
void writeStr_x(String msg) {
  for(int i=0;i<msg.length();i++) {
     putc_x(msg[i]);
     };
  }

boolean checkSerial(byte timout) {
  while (Serial.available()) { //note there is no timeout delay if there is no serial data waiting
    char c = Serial.read();  //gets one byte from serial buffer
    if (c == 0x13) { xoff=true; } // XOFF
    else if (c == 0x11) { xoff=false; } // XON
    else if (c>0 && c<0xFF) { rxbuf += c; havedata=true;} //filter out nulls and FF's.
    if (!Serial.available() && timout) { delay(timout); } //wait a tich if there isn't already more data available. Otherwise, timeout.
    }
  }

void setup ( void ) {
  //pinMode(TX2, OUTPUT); //TX line to device should always be an output
  //digitalWrite(TX2, HIGH); //when not used as serial, keep TX high or device will see nulls or FF's
  //pinMode(RX2,INPUT_PULLUP); // when not used as serial, keep RX high or we may see nulls or FF's
  Serial.begin(BAUD_RATE);
  //delay(500);
  //Serial.println("Up");
  //delay(500);
  Serial.swap(); //change to TX GPIO15, RX GPIO13 
  pinMode(SERIAL_ENABLE_PIN, OUTPUT);
  digitalWrite(SERIAL_ENABLE_PIN, LOW);
  pinMode(WAS_BLINK, INPUT);
  digitalWrite(CLEAR_BLINK, HIGH);
  pinMode(CLEAR_BLINK, OUTPUT);
  debug("\r","Starting ES8266");

  reset = ESP.getResetInfoPtr();
  switch (reset->reason) {
    case REASON_EXT_SYS_RST: //6
    case REASON_DEFAULT_RST: //0
    //standard power up or reset
      rtcmem.count=0;
      rtcmem.RFon=true; //TODO: Is this right? Will the radio always come on by default?
      debugln(" ","from poweroff");
      break;
    case REASON_DEEP_SLEEP_AWAKE: // 5
    //wake up from RTC
      ESP.rtcUserMemoryRead(0, (uint32_t*) &rtcmem, sizeof(rtcmem));
      rtcmem.count++;
      debugln(" for RTC Tick ",rtcmem.count);
      break;
    }
	EEPROM.begin(512); //EEPROM actually uses SPI_FLASH_SEC_SIZE which appears to be 4096

  ReadConfig(); //returns false and sets up default config if none found. See global.h
  //config.Interval = 0; //Temporary: Use to break out of sleep loop.
  config.ssid = "none";
  if (config.Interval > 0) config.sleepy=true; else config.sleepy=false;
  if (rtcmem.count >= config.WakeCount) {
    havedata = true; //fake having data so we will connect. 
    debugln("Check in",config.WakeCount);
    }
// Check if we really need to wake up, and if we don't, just go back to sleep
  if ( !digitalRead(WAS_BLINK)   //havent see a blink
    && config.Logging //and we are logging, but
    && config.sleepy  //we are setup to go to sleep //config.Interval>0 causes webserver issues here
    && !havedata      //not waiting on data to be logged to the server //rxbuf.length()==0 causes webserver issues here
    ) {               //then lets go to sleep
    debugln("Sleep for ",config.Interval);
    rtcmem.RFon = false;
    ESP.rtcUserMemoryWrite(0, (uint32_t*) &rtcmem, sizeof(rtcmem)); //write data to RTC memory so we don't loose it.
    ESP.deepSleep(config.Interval * MICROSECONDS, WAKE_RF_DISABLED);
    //WAKE_NO_RFCAL); //deep sleep, shut off RF, wake back up in setup.
    //https://github.com/esp8266/Arduino/issues/3072
    }
//If we get to here, we want to wake up and talk, but the radio might not be on. Only way to turn it on is to set a flag and go back to sleep.
  if (!rtcmem.RFon) {
    debugln("Reset for RF","");
    rtcmem.RFon = true;
    ESP.rtcUserMemoryWrite(0, (uint32_t*) &rtcmem, sizeof(rtcmem)); //write data to RTC memory so we don't loose it.
    ESP.deepSleep(1, WAKE_NO_RFCAL); //deep sleep, wake right away with RF on, assume calibration not needed
    }

  if (config.Connect) {
    digitalWrite(SERIAL_ENABLE_PIN, HIGH);
    debugln("Serial port enabled","");
    }

//uncomment the next line if you need the mac address for your router.
  debugln("MAC:",GetMacAddress());

  uint8_t mac[6];
  WiFi.macAddress(mac);
  DeviceID = base64::encode(mac,6);
  DeviceID.replace("+","-");
  DeviceID.replace("/","_");
  debugln("DeviceID:",DeviceID);


	if (AdminEnabled)	{
		//WiFi.mode(WIFI_AP_STA);
    WiFi.mode(WIFI_AP);
		WiFi.softAP( ACCESS_POINT_NAME , ACCESS_POINT_PASSWORD);
    IPAddress myIP = WiFi.softAPIP();
    debugln("Connect to SSID:",ACCESS_POINT_NAME);
    debugln("Password:",ACCESS_POINT_PASSWORD);
    debugln("http://",myIP);
	  }
	else	{
		WiFi.mode(WIFI_STA);
	}
 
	ConfigureWifi();
//https://github.com/esp8266/Arduino/blob/master/doc/esp8266wifi/readme.md#enable-wi-fi-diagnostic
	

//  server.on ( "/", processExample  );
//  server.on ( "/", sendRootPage  );
  server.on ( "/", handle_root  );
  
	server.on ( "/admin/filldynamicdata", filldynamicdata );
	
	server.on ( "/favicon.ico",   []() {
	  debugln("","favicon.ico"); 
    server.sendHeader ( "Cache-Control", "max-age=86400" );  
	  server.send ( 200, "text/html", "" );   //not actually sending a file? Why not 404?
	  }  );
	server.on ( "/admin.html", []() { 
	  debugln("","admin.html"); 
    server.sendHeader ( "Cache-Control", "max-age=86400" );  
	  server.send ( 200, "text/html", PAGE_AdminMainPage );   
	  }  );
	server.on ( "/config.html", send_network_configuration_html );
	server.on ( "/info.html", []() { debugln("","info.html"); server.send ( 200, "text/html", PAGE_Information );   }  );
	server.on ( "/ntp.html", send_NTP_configuration_html  );
	server.on ( "/general.html", send_general_html  );
	server.on ( "/example.html", []() { server.send ( 200, "text/html", PAGE_example );  } );
	server.on ( "/style.css", []() { 
	  debugln("","style.css"); 
    //server.sendHeader ( "Last-Modified", "Wed, 25 Feb 2015 12:00:00 GMT" );  
    server.sendHeader ( "Cache-Control", "max-age=86400" );  
    server.send ( 200, "text/css", PAGE_Style_css );  
	  } );
	server.on ( "/microajax.js", []() { 
	  debugln("","microajax.js"); 
    server.sendHeader ( "Cache-Control", "max-age=86400" );  
	  server.send ( 200, "text/plain", PAGE_microajax_js );  //in file Page_Scriptjs.h
	  } );
	server.on ( "/admin/values", send_network_configuration_values_html );
	server.on ( "/admin/connectionstate", send_connection_state_values_html );
	server.on ( "/admin/infovalues", send_information_values_html );
	server.on ( "/admin/ntpvalues", send_NTP_configuration_values_html );
	server.on ( "/admin/generalvalues", send_general_configuration_values_html);
	server.on ( "/admin/devicename",     send_devicename_value_html);
  server.on("/data", [](){  
    if (server.hasArg("text")) {
      writeStr_x(server.arg("text")); //pass on what was sent
      //TODO: Do we want a hard coded \n after the data or should the sender send that?
      Serial.flush(); //complete the send before going on
      rxbuf=""; havedata=false; //done with that data.
      delay(10); //give the device time to respond
      checkSerial(10); // get any text that comes back now
      }
    server.send(200, "text/html", rxbuf); //should be text/plain but for testing... 
    debugln(">",rxbuf);
    if (!config.Logging) {rxbuf=""; havedata=false;} //don't clear if logging so the server gets a copy
    });
  server.on("/file", [](){  
    if (server.hasArg("start")) { //have to specify a starting line to begin streaming.
      if (WL_CONNECTED != WiFi.status()) {
        debugln("file requested, but no net",get_wifi_status());
        server.send(400, "text/html", "No internet access, check network config"); 
        }
      else { //connected, start new file
        streamLine = server.arg("start").toInt(); // || 1; //the logical or 1 doesn't work for some reason!!!
        if (streamLine < 1) streamLine = 1;
        streamURL = config.streamServerURL + server.arg("name"); //include file name if specified
        http.begin(streamURL + "&line=" + streamLine);
        debugln("Streaming from \n",streamURL + "&line=" + streamLine);
        //TODO: Make the argument name for the line number configurable
        //maybe it should be post data?
        int httpCode = http.GET();
        if (HTTP_CODE_OK==httpCode) {
          if (streaming < STREAM_MAX_CONFIDENCE) streaming++; //flag it so the loop will keep running
          streamBuf[streamBufLine] = http.getString(); //first line returned on open
          server.send(200, "text/html", (String)"Streaming:"+streamBuf[streamBufLine]+"<a href=\"/file?stop=\">Stop</a> <a href=\"/file\">Status</a>"); 
          streamBufLine++;
          streamLine++;
          }
        else {
          server.send(400, "text/html", http.errorToString(httpCode)+"\nBad response from file stream server, check network config."); 
          }
        http.end();
        }
      } //done with new stream
    else if (server.hasArg("stop")) { 
      streaming = 0;  //stop the cha loop a
      server.send(200, "text/html", (String)"Streaming halted at line:" + streamLine + " <a href=\"/file?start=" + streamLine + "\">Continue</a>"); 
      }
    else { //got a request but no start
      if (streaming) { //already streaming, just provide a status update.
        server.sendHeader ( "Refresh", "5" );  
        // short version of <META HTTP-EQUIV="Refresh" CONTENT="600">
        server.send(200, "text/html", (String)"Streaming:" + streamLine + +"\n<br><a href=\"/file?stop=\">Stop</a>"); 
        }
      }
    });
	server.onNotFound ( []() { debugln("Page Not Found",""); server.send ( 404, "text/html", "Page not Found" );   }  );
	server.begin();
	debugln("HTTP server started on port:","80" );
	tkSecond.attach(1,Second_Tick);
	UDPNTPClient.begin(2390);  // Port for NTP receive
  } //Setup



void loop ( void ) {
	if (AdminEnabled && (AdminTimeOut>0))	{
		if (AdminTimeOutCounter > AdminTimeOut) {
			 AdminEnabled = false;
			 debugln("Admin Mode disabled!","");
			 WiFi.mode(WIFI_STA);
      //WiFi.disconnect();
      //WiFi.softAPdisconnect(true);
		  }
	  }
   
	if(DateTime.minute != Minute_Old)	{ //only check once a minute
		Minute_Old = DateTime.minute;
     //Serial.printf("FreeMem:%d %d:%d:%d %d.%d.%d \n",ESP.getFreeHeap() , DateTime.hour,DateTime.minute, DateTime.second, DateTime.year, DateTime.month, DateTime.day);
    debug("mem:",ESP.getFreeHeap());
    debug(" ",DateTime.year);
    debug("/",DateTime.month);
    debug("/",DateTime.day);
    debug(" ",DateTime.hour);
    debug(":",DateTime.minute);
    debug(":",DateTime.second);
    if (AdminEnabled) {debug(" admin ","")};
    if (config.Logging) {debug(" log ","")};
    if (config.sleepy) {
      debug(" sleep for:",config.Interval)
      debug(" checkin every:",config.WakeCount)
      };
    if (havedata) {debug(" data:",rxbuf)};
    debugln("","");
//    if (WL_CONNECTED != WiFi.status()) { ConfigureWifi(); };
    if (config.Update_Time_Via_NTP_Every  > 0 ) {
      if (cNTP_Update > 5 && firstStart) {
        NTPRefresh();
        cNTP_Update =0;
        firstStart = false;
        }
      else if ( cNTP_Update > (config.Update_Time_Via_NTP_Every * 60) ) {
        NTPRefresh();
        cNTP_Update =0;
        }
      }
    }


  if (streaming) {
    if (streamBufLine>0 && !xoff) {
      streamBufLine--;
      debugln("<",streamBuf[streamBufLine]);
      writeStr_x(streamBuf[streamBufLine]);
      }
    if (STREAM_BUF_LINES>streamBufLine) { //we have room to buffer more lines.
      if (WL_CONNECTED != WiFi.status()) {
        debugln("net lost while streaming:", get_wifi_status());
        streaming = 0; //stop the cha loop a
        }
      else {
        streamURL = config.streamServerURL + server.arg("name");
        //http.begin(streamURL + "&line=" + streamLine + "&lines=" + (STREAM_BUF_LINES-streamBufLine));
        //TODO: Deal with multiple line returns.
        http.begin(streamURL + "&line=" + streamLine + "&data=" + rxbuf );
        //TODO: Make the argument name for the line number configurable
        //maybe it should be post data?
        int httpCode = http.GET(); //blocking TODO: Timeout?
        if (HTTP_CODE_OK==httpCode) {
          //debugln(">",streamBuf[streamBufLine]);
          streamBuf[">",streamBufLine] = http.getString();
          streamBufLine++;
          streamLine++;
          if (streaming < STREAM_MAX_CONFIDENCE) streaming++;
          debugln(">",rxbuf);
          rxbuf="";
          havedata=false;
          }
        else { //TODO: Stop instantly on 404.
          debug(http.errorToString(httpCode),httpCode);
          debugln(" at line:",streamLine);
          streaming--; //loosen grip on the cha loop a
          }
        http.end();
        }
      }      
    }
    
  checkSerial(1); 
  // if we aren't connected to a browser, rxbuf will overflow
  // TODO: try to log rxbuf to a server
  // HACK: For now, just dump 
  
  if ( ( config.Logging && ( havedata || digitalRead(WAS_BLINK) ) )
  //TODO: OR... every so many wakeups.
  ) {
    streamURL = config.streamServerURL;// + server.arg("name");
    if (!http.begin(streamURL + "id=" + DeviceID + "&data=" + urlencode(rxbuf) + "&blink=" + (String) (digitalRead(WAS_BLINK)? "YES": "NO") ) ) {
      debug("Failed to open ",streamURL + "id=" + DeviceID + "&data=" + urlencode(rxbuf));
      }
    else {
      int httpCode = http.GET(); //blocking TODO: Timeout?
      if (HTTP_CODE_OK==httpCode) {
        //TODO: Server response can set sleep interval and send data to device.
        rxbuf=http.getString();
        debugln("logged. Response:",rxbuf);
        //TODO: If we got a command to send to the device, initiate the connection
        havedata = false; //assume 
        if ( rxbuf.length()>0 ) {
          havedata=true; //correct
          rxbuf = parseServer(rxbuf); //get out any settings and make those changes, leaving just the text to send to the device.
          debugln("send:",rxbuf);
          if ( rxbuf.length()>0 ) {
            digitalWrite(SERIAL_ENABLE_PIN, HIGH);
            delay(5); //give the RS232 transceiver / level converter time to respond
            writeStr_x(rxbuf); //pass on the servers response
            Serial.flush(); //complete the send before going on
            rxbuf=""; havedata=false; //done with that data.
            delay(10); //give the device time to respond
            checkSerial(10); // get any text that comes back now
            }
          }
        if (rtcmem.count >= config.WakeCount) {rtcmem.count = 0;} //reset checkin count.
        digitalWrite(CLEAR_BLINK, LOW); //disable any further blinks while we are awake.
        //TODO: Don't we want to see additional blinks?
        }
      else { 
        debugln("Logging failed to ",streamURL + "id=" + DeviceID + "&data=" + urlencode(rxbuf));
        debug("error:",http.errorToString(httpCode));
        debugln(" errorcode:",httpCode);
        debugln(" log:",http.getString());
        delay(1000);
        }
      http.end();
      }
    }


	if (Refresh)  { //Refresh gets set once a second by Second_Tick() in global.h
		Refresh = false; //service it here when we get a round toit.
    if (!AdminEnabled   //not in admin mode
      && config.Logging //and we are logging, but
      && config.sleepy  //we are setup to go to sleep //config.Interval>0 causes webserver issues here
      && !havedata      //not waiting on data to be logged to the server //rxbuf.length()==0 causes webserver issues here
      ) {               //then lets go to sleep
        debugln("Sleep for ",config.Interval);
        digitalWrite(CLEAR_BLINK, HIGH); //allow new blinks to be detected
        //TODO: Drop the connection to the device.
        ESP.rtcUserMemoryWrite(0, (uint32_t*) &rtcmem, sizeof(rtcmem)); //write data to RTC memory so we don't loose it.
        //TODO: Compensate for how long we have already been awake. e.g. (Interval - DateTime.Seconds)
        ESP.deepSleep(config.Interval * MICROSECONDS, WAKE_NO_RFCAL); //deep sleep, assume RF ok, wake back up in setup.
      }
//    else {
//      debugln(config.sleepy?"1":"0",havedata?"1":"0");
//      }
    }

  server.handleClient();

  } // main loop



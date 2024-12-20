#include <Arduino.h>
#include <Update.h>
#include <EEPROM.h>
#include <OneWire.h>
#include <DallasTemperature.h>
/*
This is simple emulator of mpp relay (on/off only,no momentary) and analog device for ESP32 chip family with Ethernet LAN8720 chip.
This particular configuration is expected to work on WT32-Eth board .
The IOs for relay is 14, for DS18B20 15.
The device starts simple web interface on 80 port and work interface on port 8898 for interacting with AM server and other AM devices
#include <ETH.h> - must follow after the PHY definitions accordingly v.3.0 Ethernet esp32 library cahnges.
The device keep all data etc relay pins configuration and their status in EEPROM after any changes.
It respond on UDP discovery from AM Server with current device configuration.
The device send broadcast UDP for detecting it by server right after the start and obtaining an IP.
Subscription service maintain up to 5 subscribers , the Subscribers  are being notifyed right after PUT/ query from service and changing the status and periodically every 10 min.
Every subscribers send query for resubscription after expires.
The device respond correctly to GET/survey query from server .
Last version support OTA from page 
 */
#define UDP_TX_PACKET_MAX_SIZE 2048

#undef ETH_CLK_MODE
#define ETH_CLK_MODE    ETH_CLOCK_GPIO0_IN      // WT01 version
// Pin# of the enable signal for the external crystal oscillator (-1 to disable for internal APLL source)
#define ETH_PHY_POWER 16
#define ETH_PHY_TYPE  ETH_PHY_LAN8720
#define ETH_PHY_ADDR  1
#define ETH_PHY_MDC   23
#define ETH_PHY_MDIO  18
#include <ETH.h> // important to set up after !
#include <NetworkUdp.h>
#include <WebServer.h>
#include <Update.h>
#define checkin 3000

const char* DeviceVersion = "Mpp32Relay+Analog 1.1.0"; 
const char* firmware="MppArduino 1.3.5/Mpp32Relay D18B20"; // Support full survey info, web part has changed to WebServer.h, firmware OTA update.

const char* serverIndex1 =
"<!DOCTYPE html><head><H1>Mpp Ethernet ESP32 relay + termo DS18B20</H1></head>"
"<body>"
"<br /><br />";

const char* serverIndex2 =
"<br /><br />"
"<h2>Update firmware</h2><p>Please upload the apporpriate firmware for this device.</p>"
"<form method='POST' action='/update' enctype='multipart/form-data' id='upload_form'>"
  "<label for=\"update\">Firmware:</label> <input type='file' name='update' accept='.bin'>"
  "<input type='submit' value='Update'>"
"<br /><br />"
"<br /><br />"
"<h2>Restart your device</h2>"
" &nbsp; <input type=button value='Restart' style='width:150px' onmousedown=location.href='/?RestartESP;'>"  
"</form>"
"</body></html>";

const char* updateOk = 
"<html><head><H1>Mpp Ethernet ESP32 relay + termo DS18B20</H1></head>"
"<body>"
"<br /><br />"
"<h1>Update success</h1><p>Device wil automatically reboot.</p>"
"<form action='./' method='get'>"
"<p><input type='submit' value='Back' style='width: 100px;'/></p>"
"</form></body></html>";


const char* updateFailed = 
"<html><head><H1>Mpp Ethernet ESP32 relay + termo DS18B20</H1></head>"
"<body>"
"<br /><br />"
"<h1>Update failed</h1>"
"<form action='./' method='get'>"
"<p><input type='submit' value='Back' style='width: 100px;'/></p>"
"</form></body></html>";


     
static bool eth_connected = false;

WebServer webserver(80);
NetworkServer server(8898);
NetworkUDP Udp;

String udn,location,group;
// String JSONReply;
String BroadcastIP="239.255.255.250";
String Subscriber[4];
unsigned int localPort = 8898;      // local port to listen on

boolean device_state=false;
String JsonDevice[2];
String DeviceName;
String AnalogDeviceName;
String RelTemp;
unsigned int PinDevice[2];
unsigned long lastnotify;
unsigned long next = millis();
static String UID;
unsigned long currentTime = millis();
unsigned long previousTime = 0; 
const long timeoutTime = 2000;
float avalue=0.1;
unsigned int RelayPin =14;
unsigned int AnalogPin =15;

OneWire oneWire(AnalogPin);
DallasTemperature sensors(&oneWire);

#define MaxProps 2048
#define MppMarkerLength 14
#define MppPropertiesLength MaxProps - MppMarkerLength
static const char MppMarker[MppMarkerLength] = "MppProperties";
static char propertiesString[MppPropertiesLength];


static bool writeProperties(String target) {
  if (target.length() < MppPropertiesLength) {
    for (unsigned i = 0; i < MppPropertiesLength && i < target.length() + 1;
        i++)
      EEPROM.put(i + MppMarkerLength, target.charAt(i));
    EEPROM.commit();
    Serial.printf("Saved properties (%d bytes).\n", target.length());
    return true;
  } else {
    Serial.println("Properties do not fit in reserved EEPROM space.");
    return false;
  }
}

void parseProperties(String newString, unsigned i, unsigned j, unsigned k)  {
  JsonDevice[k]=newString.substring(j,i+1);
  String name=JsonDevice[k].substring(JsonDevice[k].indexOf("name")+7,JsonDevice[k].indexOf("udn")-3);
      Serial.println("Name of device:"+ name);
      Serial.println("Full String:"+JsonDevice[k]);
   if(name.length()!=0) {
    if(JsonDevice[k].indexOf("MppAnalog")!=-1)  AnalogDeviceName=name;
    if(JsonDevice[k].indexOf("MppSwitch")!=-1)  DeviceName=name;
   }
      Serial.println("Properties loaded:"+ JsonDevice[k]);
}

void beginProperties() {
  EEPROM.begin(MaxProps);
  for (int i = 0; i < MppMarkerLength; i++) {
    if (MppMarker[i] != EEPROM.read(i)) {
      Serial.println("EEPROM initializing...");
      EEPROM.put(0, MppMarker); 
      if(writeProperties("")) {
           Serial.println(" EEPROM initialized");
            break;
    }
    else { break; Serial.print("Error EEPROM initialization"); return;} 
   }
  }

  for (unsigned i = 0; i < MppPropertiesLength; i++) {
    propertiesString[i] = EEPROM.read(i + MppMarkerLength);
    if (propertiesString[i] == 0)
      break;
  }
    unsigned k =0,j=0;  
    
    for (unsigned i = 0; i < MppPropertiesLength; i++) {
     if (propertiesString[i] == '{') j=i; // mark the very beginning of JSON {
     if (propertiesString[i] == '}' && propertiesString[i+1] == ';')  {
   String pin ="";
        pin.concat(propertiesString[i+2]);
        if(propertiesString[i+3]!=';') pin.concat(propertiesString[i+3]); // if the relay pin consist of 1 or 2 digits
          PinDevice[k]=pin.toInt();
          if(PinDevice[k]==0) break;
         parseProperties(propertiesString,i,j,k);
         Serial.printf(" for device pin:%d\n",PinDevice[k]);
          k++;
     }
  }
  SetInitRelayState(JsonDevice[1],RelayPin);
  
  return;
}


void SetInitRelayState( String JsonProperties, unsigned RelayPin) {
  String Rstate=JsonProperties.substring(JsonProperties.indexOf("state")+8,JsonProperties.indexOf("group")-3);
  if(strcmp(Rstate.c_str(),"on")==0)  { pinMode(RelayPin,OUTPUT);
                                          digitalWrite(RelayPin,true);}
  if(strcmp(Rstate.c_str(),"off")==0) { pinMode(RelayPin,OUTPUT);
                                          digitalWrite(RelayPin,false);}

  Rstate="";
}

const String& getUID() {
  if (UID.length() == 0) {
    UID = ETH.macAddress();
    while (UID.indexOf(':') > 0)
      UID.replace(":", "");
    UID.toLowerCase(); // compatible with V2
  }
  return UID;
}


String getDefaultSwitchUDN() {
  return String("MppSwitch") + "_" + getUID();
}

String getDefaultAnalogUDN() {
  return String("MppAnalog") + "_" + getUID();
}


class MppTokens {
public:
  MppTokens(const String& string, char delim)
  {
    this->string = string;
    this->delim = delim;
  }

  const String next() {
    String result;
    if (index < string.length()) {
      int i = index;
      int j = string.indexOf(delim, index);
      if (j == -1) {
        index = string.length();
        result = string.substring(i);
      } else {
        index = j + 1;
        result = string.substring(i, j);
      }
    } else
      result = String();
    return result;
  }
private:
  String string;
  unsigned index = 0;
  char delim;
};

void UpdateProperties( ) {

String Prop="";
if(JsonDevice[0]="" || JsonDevice[0].length()<10) JsonDevice[0]=makeAnalogJsonString(); // create JSON for Analog Device and store in JsonDevice[]
if(JsonDevice[1]="" || JsonDevice[1].length()<10) JsonDevice[1]=makeJsonString(); // create JSON for relay Pin and store 

Prop =JsonDevice[0]+";"+AnalogPin+";"+JsonDevice[1]+";"+RelayPin+";";

    Serial.println("Saving Current properties to EEPROM:"+Prop);
if(Prop.length()>10) writeProperties(Prop); // save current Json & pin in EEPROM
}


bool addSubscriber(String ip) {
  unsigned count =3; // restricted Subscriber size
  for (int i = 0; i <= count; i++) {
    if ((strcmp(Subscriber[i].c_str(), ip.c_str()) == 0) || Subscriber[i]=="")  {
      if(Subscriber[i]) { 
        Subscriber[i]= ip.c_str();  
         Serial.printf("%s subscribed as Subscriber[%d] \n", Subscriber[i].c_str(),i);
          return true;
        }
    } else  {
      if(i==3)  {
        count=0;
         Subscriber[i]= ip.c_str();  
         Serial.printf("%s subscribed as Subscriber[%d] \n", Subscriber[i].c_str(),i);
          return true;
      }
    }
  }
  return false;
}



void notifySubscribers(void) {  // the notification variant with Json objects 
    unsigned long now = millis();
        if (now > lastnotify+600000) {  // notifying every 10 min
          for (int i = 0; i <= 3; i++) {  //3 -is max subscribers
             if(Subscriber[i]==0) break;
             for (int j = 0;j<2; j++) {
               if (JsonDevice[j].length() == 0)  break;
                Serial.printf("Notifying: %s  ... with %s\n",Subscriber[i].c_str(),JsonDevice[j].c_str());
                  Udp.beginPacket(Subscriber[i].c_str(), localPort);
                  Udp.write((const uint8_t *)JsonDevice[j].c_str(),JsonDevice[j].length());
                  Udp.endPacket();
          Serial.println("Notifications sent.");
             }
         }
       lastnotify=millis();
     }      
  return;
}

void SendBroadcastUDP()  {

  if (makeJsonArray() == 0) return;  
  Serial.println("UDP broadcast started!");
  Udp.beginPacket(BroadcastIP.c_str(), localPort);
        Udp.write((const uint8_t *)makeJsonArray().c_str(),makeJsonArray().length());
        Udp.endPacket();
        return;
}



void onEvent(arduino_event_id_t event)
{


    switch (event)
    {
    case ARDUINO_EVENT_ETH_START:
        Serial.println("Ethernet service started!");
        // set eth hostname here
        ETH.setHostname("MppEsp32Ethernet");
        break;
    case ARDUINO_EVENT_ETH_CONNECTED:
        Serial.println("Cable is connected!");
        break;
    case ARDUINO_EVENT_ETH_GOT_IP:
    while ((ETH.localIP().toString()).startsWith("0.0") ) {}; /// Waiting IP 
        Serial.println("");
        Serial.println("IPv4 address has obtained!");
        Serial.print("ETH MAC: ");
        Serial.print(ETH.macAddress());
        Serial.print(", IPv4: ");
        Serial.print(ETH.localIP());
        
        if (ETH.fullDuplex())
        {
            Serial.print(", FULL_DUPLEX");
        }
        Serial.print(", ");
        Serial.print(ETH.linkSpeed());
        Serial.println("Mbps");
        eth_connected = true;
        break;
    case ARDUINO_EVENT_ETH_LOST_IP:
      Serial.println("ETH Lost IP");
      eth_connected = false;
      break;    
    case ARDUINO_EVENT_ETH_DISCONNECTED:
        Serial.println("ETH Disconnected");
        eth_connected = false;
    //     Serial.printf("Analog input 36: %d Voltage:%f \n",analogRead(36),(analogRead(36)*3.3)/4095 );
        break;
    case ARDUINO_EVENT_ETH_STOP:
        Serial.println("ETH Stopped");
        eth_connected = false;
        break;
    default:
        break;
    }
}

void UDP_UpdateStatus(NetworkClient& client)   {  // UDP notification to the client about status update

  for (int i = 0;i<2; i++) {
  if (JsonDevice[i].length() == 0)  break;
     Udp.beginPacket(client.remoteIP().toString().c_str(), localPort);
        Udp.write((const uint8_t *)JsonDevice[i].c_str(),JsonDevice[i].length());
        Udp.endPacket();
        Serial.println("UDP Update message with Json: "+JsonDevice[i]+" Has sent to :"+client.remoteIP().toString().c_str());
  }
}

boolean CheckRelayState() {
  return digitalRead(RelayPin);
}

void SetRelayState(bool state)  {
  pinMode(RelayPin,OUTPUT);
  digitalWrite(RelayPin,state);
  return;
}


String makeJsonArray() {
  String JSONReply="";
JSONReply+="[";
JSONReply+=makeJsonString();
JSONReply+=",";
JSONReply+= makeAnalogJsonString();
 JSONReply+="]";
      return JSONReply;
}

String makeJsonString() {
  device_state=CheckRelayState();
    if(DeviceName.length()<2 || DeviceName=="") DeviceName=String("MppSwitch") + "_" + getUID(); 
JsonDevice[1] = "{\"mac\":\""+ETH.macAddress()+"\",\"location\":\"" +location+"\",\"state\":\"" +( device_state ? "on" : "off") + "\",\"group\":\""+ group +"\",\"firmware\":\""+firmware+"\",\"name\":\""+DeviceName+"\",\"udn\":\"" + getDefaultSwitchUDN() +"\"}";
  return JsonDevice[1];  
}

String makeAnalogJsonString() {
  if(avalue!=-127) device_state=true;
  else device_state=false;
if(AnalogDeviceName.length()<2 || AnalogDeviceName=="" ) AnalogDeviceName=String("MppAnalog") + "_" + getUID()+"_"+"0"; // Set default name if no other is setup
 JsonDevice[0] = "{\"mac\":\""+ETH.macAddress()+"\",\"location\":\"" +location+"\",\"state\":\"" +( device_state ? "on" : "off") + "\",\"group\":\""+ group +"\",\"firmware\":\""+firmware+"\",\"value\":"+String(avalue,2)+",\"name\":\""+AnalogDeviceName+"\",\"udn\":\"" + getDefaultAnalogUDN()+"\"}";
   return JsonDevice[0];  
}


void sendDiscoveryResponse(IPAddress remoteIp, int remotePort) {
  Serial.printf("Responding to discovery request from %s:%d\n",remoteIp.toString().c_str(), remotePort);
  // send discovery reply
  Udp.beginPacket(remoteIp, remotePort);
 int result = Udp.write((const uint8_t *)makeJsonArray().c_str(),makeJsonArray().length());
  Udp.endPacket();
  Serial.printf("Sent discovery response to %s:%d (%d bytes sent)\n",
      remoteIp.toString().c_str(), remotePort, result);
}

bool handleIncomingUdp(NetworkUDP &Udp) {
  char incoming[16]; // only "discover" is accepted
// receive incoming UDP packets
int packetSize = Udp.parsePacket();
 if (packetSize) Serial.printf("Received UDP packet from %s, port %d\n", Udp.remoteIP().toString().c_str(), Udp.remotePort());
  else return false;
  int len = Udp.read(incoming, sizeof(incoming) - 1);
  if (len > 0) {
    incoming[len] = 0;
   Serial.printf("UDP packet contents: %s\n", incoming);
    if (String(incoming).startsWith("discover"))
      sendDiscoveryResponse(Udp.remoteIP(), Udp.remotePort());
    else return false;  
  }
  return true;
}


String ParseGet (String InputString)  {
  if(InputString.indexOf(getDefaultSwitchUDN())>0)  return JsonDevice[1];
  if(InputString.indexOf(getDefaultAnalogUDN())>0)  return JsonDevice[0];
  Serial.println("Non autorised device! from:"+InputString);  
  return ""; // return something that reach the limits
}


bool ParsePutName(String InString)   {    // parse changing name for device

  if(InString.indexOf("MppAnalog")!=-1) {
    String Newname=InString.substring(InString.lastIndexOf("?name=")+6,InString.indexOf("HTTP/1.1")-1);
    String Oldname=JsonDevice[0].substring(JsonDevice[0].indexOf("name")+7,JsonDevice[0].indexOf("udn")-3);
     if(Newname.compareTo(Oldname)!=0 && Newname.length()>1) {
      AnalogDeviceName=Newname;
      makeAnalogJsonString();
      return true;
    }
}
  
  if(InString.indexOf("MppSwitch")!=-1) {
    String Newname=InString.substring(InString.lastIndexOf("?name=")+6,InString.indexOf("HTTP/1.1")-1);
    String Oldname=JsonDevice[1].substring(JsonDevice[1].indexOf("name")+7,JsonDevice[1].indexOf("udn")-3);
     if(Newname.compareTo(Oldname)!=0 && Newname.length()>1) {
      DeviceName=Newname;
      makeJsonString();
      return true;
  }
} 

    Serial.println("No compatible device for change name!");
    return false;
}


 
void setup()
{
  Serial.begin(115200);
  sensors.begin();   // DS18B20 start
  Network.onEvent(onEvent);
  beginProperties(); // Check and load properties from EEPROM

  if( ETH.begin(ETH_PHY_TYPE,ETH_PHY_ADDR, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_POWER, ETH_CLK_MODE))
    {
      Serial.println(ETH.localIP());
        Serial.println(ETH.macAddress());
    }else { Serial.println("ETH Service not started!"); return;}
    Serial.print("Waiting for network ");
for (size_t i = 0; i < 1000 && !eth_connected; i++)
    {
        Serial.print(".");
        delay(100);
    }
 
 location+="http://"+(ETH.localIP().toString())+ ":" + "8898";
 group=getUID();
 

 sensors.requestTemperatures(); 
  avalue = sensors.getTempCByIndex(0);
  Serial.printf("Current Temperature : %.2f\n",avalue);
 
  UpdateProperties();  
  RelTemp="Current temperature:"+String(avalue)+"C"+"   Relay on pin:"+String(RelayPin)+" is :"+(digitalRead(RelayPin) ? "on" : "off");
  
  Udp.begin(localPort);
  Serial.println("UDP Service started");
  server.begin(localPort); // Server starts listening on port number 8898
  Serial.printf("Server started to listen at port %d\n",localPort);
  SendBroadcastUDP(); // Broadcast UDP

   webserver.on("/", HTTP_GET, []() {
      Serial.printf("HttpResponse: index.htm to %s\n",webserver.client().remoteIP().toString().c_str());
    webserver.sendHeader("Content-Length", String(String(serverIndex1+RelTemp+serverIndex2).length()));  
    webserver.sendHeader("Connection", "close");
    webserver.send(200, "text/html", serverIndex1+RelTemp+serverIndex2);
  });
  webserver.on("/update", HTTP_GET, []() {
    Serial.printf("HttpResponse to update for %s\n",webserver.client().remoteIP().toString().c_str());
    webserver.sendHeader("Connection", "close");
    webserver.send(200, "text/html", serverIndex1+RelTemp+serverIndex2);
  });
   /*handling reboot */
    webserver.on("/?RestartESP", HTTP_GET, []() {
      Serial.printf("HttpResponse to Restart  for %s\n",webserver.client().remoteIP().toString().c_str());
    webserver.sendHeader("Connection", "close");
    webserver.send(200, "text/html", serverIndex1+RelTemp+serverIndex2);
    delay(500);
    ESP.restart();
  });
  /*handling uploading firmware file */
  webserver.on("/update", HTTP_POST, []() {
    webserver.sendHeader("Connection", "close");
    webserver.send(200, "text/html", (Update.hasError()) ? updateFailed : updateOk);
    delay(500);
    ESP.restart();
  }, []() {
    HTTPUpload& upload = webserver.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("Update: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { //start with max available size
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      /* flashing firmware to ESP*/
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) { //true to set the size to the current progress
        Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  });
  webserver.begin(80); 
  Serial.println("Web Server started on port 80.");
}

void loop()
{
String InputString;
String OutputString;
unsigned long now = millis();
const char* Ok_Response1 ="HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ";
const char* Ok_Response2 ="\r\nConnection: close\r\n\r\n";
const char* NonOk_Response ="HTTP/1.1 404 ERROR\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n";


  if (now > next) {           // Refresh data and checkin
    Serial.printf("heap=%zu at %lus\n", ESP.getFreeHeap(), now / 1000);
    next = now + checkin;
    sensors.requestTemperatures();
    RelTemp="Current temperature:"+String(avalue)+"C"+"   Relay on pin:"+String(RelayPin)+" is :"+(digitalRead(RelayPin) ? "on" : "off");
  }


  if (eth_connected)
  {
  handleIncomingUdp(Udp);
  NetworkClient client = server.accept();
  webserver.handleClient();
 
    if (client) // If current customer is available
    {
        Serial.println("[Client connected]");
        Serial.printf("Remore client IP:%s\n",client.remoteIP().toString().c_str());
 
        
        while (client.connected()) // If the client is connected
        {
         if (client.available()) // If there is readable data /// if or while ??
            {
               char c = client.read(); // Read a byte
               if (c != '\n' && c != '\r') InputString += c;
               
               if (c == '\n' && InputString.startsWith("GET / HTTP/1.1") ) { // Reply to first device discovery
               Serial.println("Answering GET Response:"+InputString ); 
               avalue = sensors.getTempCByIndex(0);
               OutputString=makeJsonArray();
               client.println(String(Ok_Response1)+String(OutputString.length())+String(Ok_Response2));
               client.println(makeJsonArray());
          break;
            } 
               if (c == '\n' && InputString.startsWith("GET /state") ) { // Reply to Get/State
                avalue = sensors.getTempCByIndex(0);
               Serial.println("Answering GET/state Response:"+InputString ); 
             String answer= ParseGet(InputString);
      
  if(answer.length()>10) {
       client.print(String(Ok_Response1)+String(answer.length())+String(Ok_Response2));
       client.print(answer);
        Serial.println("JSON String for Get/state :"+answer);
          break;
          }
          
          else {          // Reporting ERROR 404 
            client.print(NonOk_Response);
            client.println(); 
            Serial.println("Undefined device or state!");
            break;
          }
         }     
          
           if (c == '\n' && InputString.startsWith("GET /survey") ) { // Reply to first survey discovery
           String Networks ="[{\"ssid\":\"Ethernet\",\"bssid\":\""+ETH.macAddress()+"\",\"channel\":0,\"rssi\":0,\"auth\":0}]";        
               Serial.println("Answering GET survey Response:"+InputString ); 
       client.print(String(Ok_Response1)+Networks.length()+String(Ok_Response2));        
       client.print(String(Networks));
          break;
           }
          if (c == '\n' && InputString.startsWith("PUT /state") ) {
                      Serial.println("Answering PUT Response:"+InputString );
                  if(InputString.indexOf("true")>0 && InputString.indexOf("MppSwitch")) SetRelayState(true);
                  if(InputString.indexOf("false")>0 && InputString.indexOf("MppSwitch")) SetRelayState(false);
                      OutputString=makeJsonString();
                      client.print(String(Ok_Response1)+String(OutputString.length())+String(Ok_Response2)); 
                      client.print(OutputString);
 Serial.println("JSON String for PUT/state:"+OutputString);
          UDP_UpdateStatus(client);  // UDP Notification to the client
              notifySubscribers();  // Notification to subscribers
               UpdateProperties(); // Save status of each relays  in EEPROM
          break;
            }
             if (c == '\n' && InputString.startsWith("PUT /subscribe") ) {      // Add subscriber Ip
                      Serial.println("Answering PUT/subscribe Response:"+InputString );
                       client.print(String(Ok_Response1)+String(Ok_Response2)); 
                      addSubscriber(client.remoteIP().toString().c_str());
                    break;
            }
               if (c == '\n' && InputString.startsWith("PUT /name") ) {
                      Serial.println("Answering PUT/name Response:"+InputString );
                  if(ParsePutName(InputString)) {
                    UpdateProperties(); 
               client.print(String(Ok_Response1)+String(Ok_Response2));   // ?? Response OK only , all further info in UDP  
                UDP_UpdateStatus(client);  // UDP Notification to the client
                  }
                  else Serial.println("Input names are not correct to be parsed, from:"+InputString );
                client.print(NonOk_Response);  
                client.println();        
                    break;
            }
            if (c == '\n' && InputString.startsWith("GET /favicon.ico HTTP/1.1")   ) {  // Accidental get from browser
                    client.print(String(Ok_Response1)+String(Ok_Response2));  
                    client.println();
                        break;
            }
          
        }
     }  
 client.stop(); //End the current connection
 Serial.println("[Client disconnected]" );
    }
  } 
  
  
}

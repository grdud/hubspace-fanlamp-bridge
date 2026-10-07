#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <sys/time.h>
#include <esp32c3/rom/miniz.h>
#include <functional>
#include "trust.h"
#include "project_config.h"
#include "codec.h"
#include "light_router.h"

Preferences prefs;
String ssid,password,refreshToken,accessToken,account;
WiFiClientSecure push;
String rx;
bool provisioned=false, enabled=true, loggedIn=false, initialCompressed=true, baseline=false;
uint32_t tokenDue=0,retryAt=0,lastRx=0,lastHeartbeat=0,lastPoll=0,lastStatus=0;
uint32_t heartbeatMs=30000,backoff=3000,authRetryAt=0;
uint8_t counter=0;
bool transmit(uint8_t cmd,uint8_t a0=0,uint8_t a1=0,uint8_t a2=0);
void report(const char *event);
LightRouter lights;
bool lastLightOn=false,lightKnown=false;
int lastBrightness=-1;
int lastTransmittedWarm=-1;
void serviceLiveControl();
void disconnectPush();
// HTTPS owns a separate task; only the main task touches push, routing and BLE.
bool cloudWork(const char *name,const std::function<void()> &fn){
 struct Job {const std::function<void()> *fn;SemaphoreHandle_t done;};
 auto done=xSemaphoreCreateBinary();if(!done)return false;
 Job job{&fn,done};uint32_t start=millis();
 auto entry=[](void *arg){auto *j=static_cast<Job*>(arg);(*j->fn)();xSemaphoreGive(j->done);vTaskDelete(nullptr);};
 if(xTaskCreate(entry,"cloud-http",8192,&job,1,nullptr)!=pdPASS){vSemaphoreDelete(done);return false;}
 while(xSemaphoreTake(done,0)!=pdTRUE){serviceLiveControl();delay(2);}
 vSemaphoreDelete(done);
 Serial.printf("STATUS CLOUD_JOB kind=%s duration_ms=%u heap=%u block=%u\n",name,millis()-start,ESP.getFreeHeap(),ESP.getMaxAllocHeap());
 return true;
}
struct SwitchState {const char *physical;String id;int power=-1,brightness=-1;uint64_t pt=0,bt=0;bool available=false,seeded=false;};
SwitchState switches[2];
void observeSwitch(int i,bool active){auto &v=switches[i];if(v.available&&v.power>=0&&v.brightness>=0){lights.observe(i,v.power,1+(constrain(v.brightness,i==0?BRIDGE_SWITCH_1_MIN:BRIDGE_SWITCH_2_MIN,100)-(i==0?BRIDGE_SWITCH_1_MIN:BRIDGE_SWITCH_2_MIN))*99/(100-(i==0?BRIDGE_SWITCH_1_MIN:BRIDGE_SWITCH_2_MIN)),v.pt,v.bt,active&&v.seeded&&enabled);v.seeded=true;}}
void flushLight(){
 if(!enabled||!loggedIn){lights.pending=false;return;}if(!lights.pending)return;
 Serial.printf("EVENT light_request source=%d power=%d brightness=%d revision=%llu ms=%u\n",lights.source+1,lights.power,lights.brightness,(unsigned long long)lights.winner,millis());
 bool ok=true;
 if(!lights.power){if(!lightKnown||lastLightOn)ok=transmit(0x11);}
 else {
  int warm=(lights.brightness*255+50)/100;
  // OFF retains the fan's last brightness. Repeating that brightness before
  // every ON added an unnecessary 350 ms burst. Unknown startup still sets it.
  if(lastTransmittedWarm!=warm)ok=transmit(0x21,0,0,warm);
  if(ok&&(!lightKnown||!lastLightOn))ok=transmit(0x10);
 }
 if(!ok){report("BLE_FAILURE_REBOOT");ESP.restart();}
 lastLightOn=lights.power;lastBrightness=lights.brightness;lightKnown=true;lights.pending=false;
}

void report(const char *event){Serial.printf("STATUS %s wifi=%d push=%d heap=%u\n",event,WiFi.status()==WL_CONNECTED,loggedIn,ESP.getFreeHeap());}
int cloudInt(JsonVariantConst v){
 if(v.is<int>())return v.as<int>();const char *p=v.as<const char*>();if(!p||!*p)return -1;
 char *end=nullptr;long n=strtol(p,&end,10);return !*end&&n>=0&&n<=65535?int(n):-1;
}
uint64_t timestamp(JsonVariant v){double n=v.as<double>(); return n>0&&n<100000000000.0?uint64_t(n*1000):uint64_t(n);}
bool transmit(uint8_t cmd,uint8_t a0,uint8_t a1,uint8_t a2){
 if(!BRIDGE_BLE_ID)return false;
 uint8_t raw[31];counter=(counter+1)%125;
 if(prefs.putUChar("counter",counter)!=1){report("COUNTER_SAVE_FAILED");return false;}
 encodeFan(cmd,a0,a1,a2,counter,1+esp_random()%0xfff5,raw);
 NimBLEAdvertisementData data;data.addData(std::string((char*)raw,sizeof(raw)));
 auto *adv=NimBLEDevice::getAdvertising();adv->setAdvertisementData(data);
 if(ble_gap_adv_set_data(raw,sizeof(raw))!=0)return false;
 adv->setAdvertisementType(BLE_GAP_CONN_MODE_NON);adv->setScanResponse(false);
 adv->setMinInterval(0x20);adv->setMaxInterval(0x20);
 if(!adv->start())return false;
 Serial.printf("EVENT TX_BEGIN cmd=%u count=%u warm=%u ms=%u\n",cmd,counter,a2,millis());
 delay(350);if(!adv->stop())return false;
 if(cmd==0x21&&a1==0)lastTransmittedWarm=a2;
 Serial.printf("TX cmd=%u count=%u warm=%u ms=%u radio-only\n",cmd,counter,a2,millis());return true;
}
void applyState(){
 if(!baseline&&loggedIn&&enabled){baseline=true;report("READY_SWITCH_ONLY");}
}
String formEncode(const String &s){String out;out.reserve(s.length()*2);const char *h="0123456789ABCDEF";for(size_t i=0;i<s.length();i++){uint8_t c=s[i];if(isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~')out+=char(c);else {out+='%';out+=h[c>>4];out+=h[c&15];}}return out;}
bool request(const String &url,DynamicJsonDocument &doc,const String &body="",bool auth=true,const char *content="application/json",JsonDocument *filter=nullptr){
 bool result=false;
 if(!cloudWork("request",[&](){
 WiFiClientSecure client;client.setCACert(url.indexOf(".afero.net/")>=0?API_CA:AUTH_CA);client.setHandshakeTimeout(15);
 HTTPClient http;http.setTimeout(15000);http.useHTTP10(true);
 if(!http.begin(client,url))return false;
 if(auth)http.addHeader("Authorization","Bearer "+accessToken);

 http.addHeader("Content-Type",content);http.addHeader("Accept","application/json");
 int code=body.length()?http.POST(body):http.GET();
 bool ok=false;
 if(code==200){
  // Token JSON is not needed until TLS has completed its peak allocations.
  if(doc.capacity()==0)doc=DynamicJsonDocument(12288);
  DeserializationError err=filter?deserializeJson(doc,http.getStream(),DeserializationOption::Filter(*filter),DeserializationOption::NestingLimit(32)):deserializeJson(doc,http.getStream());ok=!err;if(err)Serial.printf("JSON_ERROR %s\n",err.c_str());}
 else {char reason[120]={};int tls=client.lastError(reason,sizeof(reason));Serial.printf("HTTP_ERROR %d tls=%d heap=%u block=%u\n",code,tls,ESP.getFreeHeap(),ESP.getMaxAllocHeap());}
 if(code==401)tokenDue=0;
 http.end();result=ok;return true;
 }))return false;
 return result;
}
bool refreshAuth(){
 DynamicJsonDocument doc(0);
 String body="grant_type=refresh_token&client_id=hubspace_android&scope=openid%20email%20offline_access%20profile&refresh_token="+formEncode(refreshToken);
 if(!request("https://accounts.hubspaceconnect.com/auth/realms/thd/protocol/openid-connect/token",doc,body,false,"application/x-www-form-urlencoded"))return false;
 String next=doc["refresh_token"]|"";String access=doc["access_token"]|"";
 if(next.isEmpty()||access.isEmpty())return false;
 if(next!=refreshToken){if(prefs.putString("refresh",next)!=next.length())return false;refreshToken=next;}
 accessToken=access;int expires=doc["expires_in"]|300;tokenDue=millis()+uint32_t(max(30,expires-60))*1000;
 report("TOKEN_REFRESHED");return true;
}
bool discover(){
 DynamicJsonDocument filter(512),doc(8192);
 filter[0]["id"]=true;filter[0]["deviceId"]=true;
 filter[0]["description"]["device"]["deviceClass"]=true;
 if(!request("https://semantics2.afero.net/v1/accounts/"+account+"/metadevices?expansions=state,capabilities,semantics",doc,"",true,"application/json",&filter))return false;
 for(auto &v:switches)v.id="";
 for(JsonObject d:doc.as<JsonArray>()){
  for(auto &v:switches)if(String(d["deviceId"]|"")==v.physical&&String(d["description"]["device"]["deviceClass"]|"")=="switch")v.id=d["id"].as<String>();
 }
 if(switches[0].id.isEmpty()&&switches[1].id.isEmpty()){report("NO_SWITCHES_FOUND");return false;}
 report("DISCOVERED_SWITCH_ONLY");return true;
}
bool pollSwitches(bool active){
 for(int i=0;i<2;i++){
  auto &v=switches[i];if(v.id.isEmpty())continue;
  DynamicJsonDocument filter(512),d(8192);
  filter["values"][0]["functionClass"]=true;filter["values"][0]["functionInstance"]=true;
  filter["values"][0]["value"]=true;filter["values"][0]["lastUpdateTime"]=true;
  // Filter still includes nested settings; stream into a bounded document.
  if(!request("https://semantics2.afero.net/v1/accounts/"+account+"/metadevices/"+v.id+"/state",d,"",true,"application/json",&filter))return false;
  for(JsonObject a:d["values"].as<JsonArray>()){
   String c=a["functionClass"]|"";uint64_t t=timestamp(a["lastUpdateTime"]);
   if(c=="power"&&t>=v.pt){String p=a["value"]|"";if(p=="on"||p=="off"){v.power=p=="on";v.pt=t;}}
   if(c=="brightness"&&t>=v.bt){int b=a["value"]|-1;if(b>=0&&b<=100){v.brightness=b;v.bt=t;}}
   if(c=="available"&&a["value"].is<bool>())v.available=a["value"].as<bool>();
  }
  if(!active)Serial.printf("STATE switch=%d power=%d brightness=%d available=%d\n",i+1,v.power,v.brightness,v.available);
  observeSwitch(i,active);
 }
 return true;
}
void processFrame(const String &raw){
 StaticJsonDocument<512> filter;
 filter["welcome"]["heartbeat"]=true;filter["private"]["event"]=true;
 filter["private"]["data"]["id"]=true;filter["private"]["data"]["attribute"]=true;filter["private"]["data"]["status"]=true;
 DynamicJsonDocument d(2048);if(deserializeJson(d,raw,DeserializationOption::Filter(filter)))return;
 if(d.containsKey("welcome")){loggedIn=true;baseline=false;heartbeatMs=uint32_t(d["welcome"]["heartbeat"]|60)*500;heartbeatMs=constrain(heartbeatMs,1000,30000);report("PUSH_CONNECTED");return;}
 JsonObject priv=d["private"];if(priv.isNull())return;
 JsonObject data=priv["data"];String device=data["id"]|"";
 for(int i=0;i<2;i++)if(device.length()&&device==switches[i].physical){
  auto &v=switches[i];String kind=priv["event"]|"";
  if(kind=="status_change"&&data["status"]["available"].is<bool>()){v.available=data["status"]["available"];if(!v.available){v.seeded=false;lights.sources[i].seeded=false;}}
  if(kind=="attr_change"){
   JsonObject a=data["attribute"];int key=cloudInt(a["id"]);int n=cloudInt(a["value"]);uint64_t t=timestamp(a["updatedTimestamp"]);
   if(key==10&&n>=0&&n<=1&&t>=v.pt){v.power=n;v.pt=t;}
   else if(key==20&&n>=0&&n<=100&&t>=v.bt){v.brightness=n;v.bt=t;}else return;
   Serial.printf("EVENT switch=%d attr=%d value=%d ms=%u\n",i+1,key,n,millis());observeSwitch(i,true);
  }
  return;
 }
}
bool drainFrames(){
 if(initialCompressed&&rx.length()){
 if(uint8_t(rx[0])==0x78){
 auto *decomp=new tinfl_decompressor;tinfl_init(decomp);uint8_t out[4096];size_t inSize=rx.length(),outSize=sizeof(out);
 tinfl_status status=tinfl_decompress(decomp,(const uint8_t*)rx.c_str(),&inSize,out,out,&outSize,TINFL_FLAG_PARSE_ZLIB_HEADER|TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);delete decomp;
 if(status==TINFL_STATUS_DONE){rx.remove(0,inSize);initialCompressed=false;}
 else if(status<0){
 // Conclave may send an unfinished zlib prefix followed by a plain JSON hello.
 // Match aioafero's decoder fallback while retaining fragmented prefixes.
 int jsonStart=rx.indexOf('{');
 if(jsonStart>=0){rx.remove(0,jsonStart);initialCompressed=false;}
 else if(rx.length()>4096)return false;else return true;
 }else return true;
 }else initialCompressed=false;
 }
 while(rx.length()){
 if(rx[0]!='{'){rx.remove(0,1);continue;}
 int depth=0,end=-1;bool quoted=false,escaped=false;
 for(size_t i=0;i<rx.length();i++){char c=rx[i];if(quoted){if(escaped)escaped=false;else if(c=='\\')escaped=true;else if(c=='"')quoted=false;}else if(c=='"')quoted=true;else if(c=='{')depth++;else if(c=='}'&&!--depth){end=i;break;}}
 if(end<0)break;String frame=rx.substring(0,end+1);rx.remove(0,end+1);processFrame(frame);
 }
 return rx.length()<16384;
}
bool connectPush(){
 DynamicJsonDocument d(12288);
 if(!request("https://api2.afero.net/v1/accounts/"+account+"/conclaveAccess",d,"{\"user\":true,\"softHub\":false}"))return false;
 String host=d["conclave"]["host"]|"";int port=d["conclave"]["port"]|0;
 if(host.isEmpty()||port<1||!(d["conclave"]["ssl"]|true))return false;
 JsonObject token;for(JsonObject t:d["tokens"].as<JsonArray>())if(String(t["type"]|"")=="account"){token=t;break;}
 if(token.isNull())return false;
 push.setCACert(ROOT_CA);push.setHandshakeTimeout(15);push.setTimeout(1000);
 if(!push.connect(host.c_str(),port)){char reason[160]={};int e=push.lastError(reason,sizeof(reason));Serial.printf("ERR PUSH_TLS %d %s heap=%u block=%u\n",e,reason,ESP.getFreeHeap(),ESP.getMaxAllocHeap());return false;}
 report("PUSH_TLS_READY");
 rx="";initialCompressed=true;loggedIn=false;push.print("{}\n");
 uint32_t start=millis();while(millis()-start<10000){while(push.available()){rx+=char(push.read());lastRx=millis();}if(rx.length()){if(!drainFrames())return false;if(!initialCompressed)break;}delay(5);}
 if(initialCompressed){Serial.printf("ERR PUSH_HELLO bytes=%u\n",rx.length());return false;}
 DynamicJsonDocument frame(8192);frame["login"]["channelId"]=token["channelId"];frame["login"]["accessToken"]=token["token"];
 frame["login"]["type"]="aioafero";frame["login"]["version"]="1.0.0";frame["login"]["protocol"]=2;frame["login"]["trace"]=false;
 serializeJson(frame,push);push.print('\n');start=millis();
 while(millis()-start<10000&&!loggedIn){while(push.available()){rx+=char(push.read());lastRx=millis();}if(!drainFrames())return false;delay(5);}
 if(!loggedIn)Serial.printf("ERR PUSH_LOGIN bytes=%u connected=%d\n",rx.length(),push.connected());
 lastRx=lastHeartbeat=lastPoll=millis();return loggedIn;
}
void serviceLiveControl(){
 if(!loggedIn||!baseline||!push.connected())return;
 while(push.available()){rx+=char(push.read());lastRx=millis();if(rx.length()>16384){disconnectPush();return;}}
 if(!drainFrames()){disconnectPush();return;}
 applyState();flushLight();
 if(millis()-lastHeartbeat>=heartbeatMs){push.print('\n');lastHeartbeat=millis();}
}
void disconnectPush(){push.stop();loggedIn=false;baseline=false;rx="";lights.reset();for(auto &v:switches)v.seeded=false;}
void loadConfig(){
 ssid=prefs.getString("ssid","");password=prefs.getString("wifi","");refreshToken=prefs.getString("refresh","");account=prefs.getString("account","");
 counter=prefs.getUChar("counter",BRIDGE_INITIAL_COUNTER);enabled=prefs.getBool("enabled",true);
 provisioned=ssid.length()&&refreshToken.length()&&account.length()&&(strlen(BRIDGE_SWITCH_1_ID)==16||strlen(BRIDGE_SWITCH_2_ID)==16)&&BRIDGE_BLE_ID!=0;
}
void serialCommand(const String &line){
 if(line=="STATUS"||line=="PING"){report(baseline?"READY":"WAITING");Serial.printf("STATE enabled=%d light_known=%d light=%d brightness=%d\n",enabled,lightKnown,lastLightOn,lastBrightness);for(int i=0;i<2;i++)Serial.printf("STATE switch=%d power=%d brightness=%d available=%d\n",i+1,switches[i].power,switches[i].brightness,switches[i].available);return;}
 if(line=="PAUSE"){enabled=false;prefs.putBool("enabled",false);baseline=false;Serial.println("OK PAUSED");return;}
 if(line=="RESUME"){enabled=true;prefs.putBool("enabled",true);baseline=false;Serial.println("OK RESUMED");return;}
 if(line=="REBOOT"){ESP.restart();return;}
 if(line.startsWith("CONFIG ")){
 DynamicJsonDocument d(12288);if(deserializeJson(d,line.substring(7))){Serial.println("ERR CONFIG JSON");return;}
 String s=d["ssid"]|"",p=d["password"]|"",r=d["refresh_token"]|"",a=d["account"]|"";
 if(s.isEmpty()||s.length()>32||p.length()>63||r.length()<20||r.length()>7000||a.length()!=36){Serial.println("ERR CONFIG VALUES");return;}
 prefs.putBool("enabled",false);prefs.putString("ssid",s);prefs.putString("wifi",p);prefs.putString("refresh",r);prefs.putString("account",a);prefs.putUChar("counter",prefs.getUChar("counter",BRIDGE_INITIAL_COUNTER));prefs.putBool("enabled",true);
 Serial.println("OK CONFIG SAVED");Serial.flush();delay(200);ESP.restart();return;
 }
 Serial.println("ERR COMMAND");
}
void setup(){
 Serial.begin(115200);switches[0].physical=BRIDGE_SWITCH_1_ID;switches[1].physical=BRIDGE_SWITCH_2_ID;prefs.begin("fanstandalone",false);
 if(!lightRouterSelfTest()){Serial.println("FATAL LIGHT ROUTER SELFTEST");while(true)delay(1000);}Serial.println("OK LIGHT ROUTER SELFTEST");
 if(!codecSelfTest()){Serial.println("FATAL CODEC SELFTEST");while(true)delay(1000);}Serial.println("OK CODEC 14 SYNTHETIC VECTORS");
 NimBLEDevice::init("");
 if(!NimBLEDevice::getInitialized()){report("FATAL BLE");while(true)delay(1000);}
 loadConfig();WiFi.mode(WIFI_STA);WiFi.setSleep(true);WiFi.setAutoReconnect(true);if(provisioned)WiFi.begin(ssid.c_str(),password.c_str());
 configTime(0,0,"pool.ntp.org","time.google.com");report(provisioned?"BOOT":"NEEDS_CONFIG");
}
void loop(){
 static String line;static bool overflow=false;
 while(Serial.available()){char c=Serial.read();if(c=='\n'){if(!overflow){line.trim();if(line.length())serialCommand(line);}line="";overflow=false;}else if(line.length()<12288)line+=c;else overflow=true;}
 if(millis()-lastStatus>30000){report(baseline?"READY":"WAITING");lastStatus=millis();}
 if(!provisioned){delay(10);return;}
 if(WiFi.status()!=WL_CONNECTED){if(loggedIn)disconnectPush();delay(10);return;}
 if(time(nullptr)<1700000000){delay(10);return;}
 if(int32_t(millis()-retryAt)<0){delay(5);return;}
 // Refresh the REST token without interrupting the independent push session.
 if(loggedIn&&push.connected()&&int32_t(millis()-tokenDue)>=0&&int32_t(millis()-authRetryAt)>=0){
  if(refreshAuth())authRetryAt=0;else {report("AUTH_RETRY");authRetryAt=millis()+10000;}
 }
 if(!loggedIn||!push.connected()){
 disconnectPush();bool ok=true;
 if(accessToken.isEmpty()||int32_t(millis()-tokenDue)>=0)ok=refreshAuth();
 if(ok)ok=discover();if(ok)ok=pollSwitches(false);if(ok){report("STATE_LOADED");ok=connectPush();}
 if(!ok){disconnectPush();report("RETRY");retryAt=millis()+backoff;backoff=min(uint32_t(60000),backoff*2);return;}
 backoff=3000;applyState();
 }
 while(push.available()){rx+=char(push.read());lastRx=millis();if(rx.length()>16384){disconnectPush();return;}}
 if(!drainFrames()){disconnectPush();return;}applyState();flushLight();
 if(millis()-lastHeartbeat>=heartbeatMs){push.print('\n');lastHeartbeat=millis();}
 if(millis()-lastRx>90000){report("PUSH_TIMEOUT");disconnectPush();return;}
 // Live push supplies changes; REST snapshots are loaded on (re)connection.
 // Five redundant TLS polls every 30 seconds previously stalled this loop.
 delay(2);
}

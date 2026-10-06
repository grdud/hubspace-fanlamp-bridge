#pragma once
#include <stdint.h>

// Hubspace acknowledges a multi-field PUT as separate attribute events.
// Match each acknowledgement independently; never combine one fresh field
// with an old field to manufacture a new light command.
struct ReceiverLight {
 struct Seen {int value=-1;uint64_t stamp=0;} seen[3];
 struct Write {
  bool active=false,used[3]={false,false,false};
  int value[3]={0,0,0};uint64_t ack[3]={0,0,0};uint32_t at=0;
 } writes[8];
 unsigned next=0;
 struct Patch {bool powerChanged=false,brightnessChanged=false;int power=0,brightness=100;uint64_t pt=0,bt=0;};
 void seed(int primary,int power,int brightness,uint64_t t0,uint64_t t1,uint64_t t2){
  int v[]={primary,power,brightness};uint64_t t[]={t0,t1,t2};
  for(int i=0;i<3;i++){seen[i].value=v[i];seen[i].stamp=t[i];}
 }
 void expect(int primary,int power,int brightness,uint32_t now,bool writesBrightness=true){
  Write &w=writes[next++%8];w=Write();w.active=true;w.at=now;
  w.value[0]=primary;w.value[1]=power;w.value[2]=brightness;w.used[2]=!writesBrightness;
 }
 bool echo(int field,int value,uint64_t stamp,uint32_t now){
  for(auto &w:writes){
   if(!w.active||uint32_t(now-w.at)>15000||w.value[field]!=value)continue;
   if(w.used[field]&&w.ack[field]==stamp)return true;
  }
  // Consume the oldest outstanding matching acknowledgement first.
  for(unsigned n=0;n<8;n++){
   Write &w=writes[(next+n)%8];
   if(!w.active||uint32_t(now-w.at)>15000||w.used[field]||w.value[field]!=value)continue;
   w.used[field]=true;w.ack[field]=stamp;return true;
  }
  return false;
 }
 Patch update(int primary,int power,int brightness,uint64_t t0,uint64_t t1,uint64_t t2,
              int currentPower,int currentBrightness,uint32_t now){
  Patch p;p.power=currentPower;p.brightness=currentBrightness;
  int v[]={primary,power,brightness};uint64_t t[]={t0,t1,t2};
  for(int i=0;i<3;i++){
   if(t[i]<seen[i].stamp||(t[i]==seen[i].stamp&&v[i]==seen[i].value))continue;
   bool changed=v[i]!=seen[i].value;
   seen[i].value=v[i];seen[i].stamp=t[i];
   if(echo(i,v[i],t[i],now))continue;
   // Unchanged status refreshes aren't user commands.
   if(!changed)continue;
   if(i==0){if(v[i]==0){p.power=0;p.powerChanged=true;p.pt=t[i];}}
   else if(i==1){if(!p.powerChanged||t[i]>=p.pt){p.power=v[i];p.powerChanged=true;p.pt=t[i];}}
   else {p.brightness=v[i];p.brightnessChanged=true;p.bt=t[i];}
  }
  return p;
 }
};

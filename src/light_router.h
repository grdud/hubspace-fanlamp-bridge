#pragma once
#include <stdint.h>

// Compare cloud revisions, not continuously asserted device states. An unchanged
// REST snapshot must never undo a newer command from a different controller.
struct LightRouter {
 struct Source { bool seeded=false; int power=0,brightness=100; uint64_t pt=0,bt=0; } sources[2];
 bool pending=false, power=false, forcePower=false;
 int brightness=100,source=-1;
 uint64_t winner=0;
 void seed(int i,int p,int b,uint64_t pt,uint64_t bt){auto &s=sources[i];s.seeded=true;s.power=p;s.brightness=b;s.pt=pt;s.bt=bt;}
 void reset(){for(auto &s:sources)s=Source();pending=false;winner=0;}
 void observe(int index,int p,int b,uint64_t pt,uint64_t bt,bool active){
  if(index<0||index>1||p<0||p>1||b<0||b>100)return;
  auto &s=sources[index];
  if(s.seeded){if(pt<s.pt){p=s.power;pt=s.pt;}if(bt<s.bt){b=s.brightness;bt=s.bt;}}
  bool pe=s.seeded&&(p!=s.power||pt>s.pt);
  bool be=s.seeded&&(b!=s.brightness||bt>s.bt);
  bool seeded=s.seeded;s.seeded=true;s.power=p;s.brightness=b;s.pt=pt;s.bt=bt;
  uint64_t revision=pt>bt?pt:bt;
  if(!active||!seeded){if(revision>winner)winner=revision;return;}
  if(!(pe||be)||revision<winner)return;
  winner=revision;source=index;pending=true;power=p&&b>0;brightness=b;
  forcePower=pe;
 }
};

inline bool lightRouterSelfTest(){
 LightRouter r;
 r.observe(0,1,1,100,100,false);r.observe(1,0,100,110,110,false);
 if(r.pending)return false;
 r.observe(0,0,1,200,100,true);if(!r.pending||r.power)return false;r.pending=false;
 r.observe(1,1,100,300,110,true);if(!r.pending||!r.power||r.brightness!=100)return false;r.pending=false;
 r.observe(0,1,1,250,100,true);if(r.pending)return false;
 r.observe(1,1,100,300,110,true);if(r.pending)return false;
 r.observe(0,1,1,400,400,true);if(!r.pending||!r.power||r.brightness!=1)return false;
 r.reset();r.observe(0,1,1,400,400,false);return !r.pending;
}

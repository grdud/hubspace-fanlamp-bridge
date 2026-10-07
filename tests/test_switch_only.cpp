#include "../src/light_router.h"
#include <cassert>
#include <cstdio>
int main(){
 LightRouter r;
 assert(lightRouterSelfTest());
 r.observe(0,1,1,100,100,false);
 r.observe(1,0,100,110,110,false);
 assert(!r.pending);
 r.observe(0,0,1,200,100,true);
 assert(r.pending&&!r.power&&r.source==0);r.pending=false;
 r.observe(1,1,100,300,110,true);
 assert(r.pending&&r.power&&r.brightness==100&&r.source==1);r.pending=false;
 r.observe(0,1,1,250,100,true);assert(!r.pending);
 r.observe(1,1,100,300,110,true);assert(!r.pending);
 r.observe(0,1,1,400,400,true);
 assert(r.pending&&r.power&&r.brightness==1&&r.source==0);
 r.reset();r.observe(0,1,1,400,400,false);assert(!r.pending);
 puts("PASS: switch-only baseline, power, brightness, latest-change wins, duplicates and reconnect");
}

#include <cassert>
#include <cstdio>
#include "../src/receiver_light.h"
#include "../src/light_router.h"
static void quiet(const ReceiverLight::Patch &p){assert(!p.powerChanged&&!p.brightnessChanged);}
int main(){
 assert(lightRouterSelfTest());
 ReceiverLight r;r.seed(1,1,80,100,100,100);
 r.expect(0,0,1,1000);
 // Captured failure: brightness ACK arrives while old receiver power is on.
 quiet(r.update(1,1,1,100,100,200,0,1,1100));
 quiet(r.update(0,1,1,201,100,200,0,1,1200));
 quiet(r.update(0,0,1,201,202,200,0,1,1300));
 // Switch ON: primary ACK precedes the light-power ACK.
 r.expect(1,1,1,1400);
 quiet(r.update(1,0,1,300,202,200,1,1,1500));
 quiet(r.update(1,1,1,300,301,302,1,1,1600));
 // Real app brightness updates still work and preserve physical power.
 auto p=r.update(1,1,43,300,301,400,0,1,1700);
 assert(p.brightnessChanged&&p.brightness==43&&p.power==0&&!p.powerChanged);
 // Real app OFF/ON remains usable after acknowledged writes.
 p=r.update(1,0,43,300,401,400,1,43,1800);assert(p.powerChanged&&p.power==0);
 p=r.update(1,1,43,300,402,400,0,43,1900);assert(p.powerChanged&&p.power==1);
 // Ignore delayed snapshots and duplicate events.
 quiet(r.update(0,0,1,100,100,100,1,43,2000));
 quiet(r.update(1,1,43,300,402,400,1,43,2100));
 // Rapid OFF then ON: late OFF acknowledgements cannot reverse latest ON.
 ReceiverLight rapid;rapid.seed(1,1,43,100,100,100);
 rapid.expect(0,0,1,1000);rapid.expect(1,1,70,1200);
 quiet(rapid.update(0,0,1,200,201,202,1,70,1300));
 quiet(rapid.update(1,1,70,300,301,302,1,70,1400));
 // A new app command after those acknowledgements remains effective.
 p=rapid.update(1,0,70,300,400,302,1,70,1500);assert(p.powerChanged&&p.power==0);
 ReceiverLight offOnly;offOnly.seed(1,1,80,100,100,100);offOnly.expect(0,0,1,1000,false);
 quiet(offOnly.update(0,0,80,200,201,100,0,80,1100));
 p=offOnly.update(0,0,1,200,201,300,0,80,1200);assert(p.brightnessChanged&&p.power==0);
 puts("PASS: captured partial-ACK sequences, rapid toggles, real app commands, stale snapshots, and light routing");
}

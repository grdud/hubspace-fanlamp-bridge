#include "../src/fan_router.h"
#include <cassert>
#include <cstdio>
int main(){
 FanRouter r;r.seed(true,1,false);
 auto c=r.update(true,1,true);assert(c.opcode==0x33&&c.arg0==2);
 assert(!r.update(true,6,true).opcode); // receiver's breeze speed cycling
 assert(!r.update(true,6,true).opcode); // duplicate mode update
 c=r.update(true,6,false);assert(c.opcode==0x31&&c.arg1==6);
 c=r.update(true,3,false);assert(c.opcode==0x31&&c.arg1==3);
 r.update(true,3,true);c=r.update(false,3,true);assert(c.opcode==0x31&&c.arg1==0);
 assert(!r.update(false,6,true).opcode); // breeze cannot override power off
 c=r.update(true,6,true);assert(c.opcode==0x33&&c.arg0==2);
 r.seed(true,4,true);assert(!r.update(true,1,true).opcode); // reconnect baseline
 puts("PASS: breeze enter/exit, speed cycling, duplicates, OFF priority, restart baseline");
}

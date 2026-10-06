#pragma once
struct FanRouter {
 enum Mode {Off,Fixed,Breeze};
 struct Command {int opcode=0,arg0=0,arg1=0;};
 Mode mode=Off;int speed=0;
 void seed(bool on,int nextSpeed,bool breeze){mode=on?(breeze?Breeze:Fixed):Off;speed=nextSpeed;}
 Command update(bool on,int nextSpeed,bool breeze){
  Mode next=on?(breeze?Breeze:Fixed):Off;Command cmd;
  if(next!=mode||(next==Fixed&&nextSpeed!=speed)){
   if(next==Breeze){cmd.opcode=0x33;cmd.arg0=2;}
   else {cmd.opcode=0x31;cmd.arg0=32;cmd.arg1=next==Off?0:nextSpeed;}
  }
  mode=next;speed=nextSpeed;return cmd;
 }
};

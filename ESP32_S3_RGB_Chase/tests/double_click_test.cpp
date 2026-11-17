#include "step_control.h"
#include <cassert>
#include <iostream>
using namespace stepcontrol;
int main(){
 DoubleClickButton b;b.begin(false,0);
 auto edge=[&](bool pressed,uint32_t t){assert(b.update(pressed,t)==ClickEvent::None);return b.update(pressed,t+25);};
 assert(edge(true,100)==ClickEvent::None);assert(edge(false,160)==ClickEvent::None);
 assert(b.update(false,510)==ClickEvent::None);assert(b.update(false,511)==ClickEvent::Single);assert(b.update(false,900)==ClickEvent::None);
 b.begin(false,0);assert(edge(true,100)==ClickEvent::None);assert(edge(false,160)==ClickEvent::None);
 assert(edge(true,300)==ClickEvent::None);assert(edge(false,360)==ClickEvent::Double);assert(b.update(false,1200)==ClickEvent::None);
 // Release/press bounce never forms a second click.
 b.begin(false,0);assert(b.update(true,100)==ClickEvent::None);assert(b.update(false,105)==ClickEvent::None);
 assert(edge(true,110)==ClickEvent::None);assert(b.update(false,170)==ClickEvent::None);assert(b.update(true,175)==ClickEvent::None);
 assert(edge(false,180)==ClickEvent::None);assert(b.update(false,531)==ClickEvent::Single);
 // A button held during reset is unarmed until released; a long press does nothing.
 b.begin(true,0);assert(b.update(true,3000)==ClickEvent::None);assert(edge(false,3100)==ClickEvent::None);assert(b.update(false,4000)==ClickEvent::None);
 b.begin(false,0);assert(edge(true,100)==ClickEvent::None);assert(b.update(true,3000)==ClickEvent::None);
 assert(edge(false,3100)==ClickEvent::None);assert(b.update(false,4000)==ClickEvent::None);
 // Consecutive doubles consume four clicks and produce no extra single.
 b.begin(false,0);for(unsigned t:{100u,500u}){assert(edge(true,t)==ClickEvent::None);assert(edge(false,t+60)==ClickEvent::None);assert(edge(true,t+150)==ClickEvent::None);assert(edge(false,t+210)==ClickEvent::Double);}
 assert(b.update(false,1200)==ClickEvent::None);
 // Time wrap and a second press whose debounce crosses the deadline.
 b.begin(false,0xfffffe00u);assert(edge(true,0xffffff00u)==ClickEvent::None);assert(edge(false,0xffffff50u)==ClickEvent::None);
 assert(edge(true,0x90u)==ClickEvent::None);assert(edge(false,0xd0u)==ClickEvent::Double);
 b.begin(false,0);assert(edge(true,100)==ClickEvent::None);assert(edge(false,160)==ClickEvent::None);
 assert(edge(true,500)==ClickEvent::None);assert(edge(false,550)==ClickEvent::Double);
 std::cout<<"PASS: debounced exclusive single/double clicks, bounce, long/boot holds, repeated doubles and timer wrap.\n";
}

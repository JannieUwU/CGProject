#include "cg/ef.hpp"
#include <iostream>
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(false)
int main(){using namespace cg;
 CameraController c;c.orbit(0,100);CHECK(c.camera.pitch==1.55);c.zoom(10000);CHECK(c.camera.distance==.01);
 std::vector<Body> bodies={{1,{0,0,0},1},{2,{0,0,4},1}};
 CHECK(pick({0,0,10},{0,0,-1},bodies)==2);CHECK(!pick({10,0,10},{0,0,-1},bodies));CHECK(pick({0,0,0},{1,0,0},bodies)==1);
 c.follow(2);c.update(bodies);CHECK(c.camera.target.z==4);c.follow(std::nullopt);
 Snapshot s;s.time=42;RenderConfig cfg;cfg.width=2;cfg.height=2;cfg.trace=true;int calls=0;
 Renderer r=[&](const Snapshot& snap,const RenderConfig& conf){CHECK(snap.time==42);++calls;return Frame{conf.width,conf.height,std::vector<std::uint8_t>(16,255),{4,8,16}};};
 auto frames=compare(r,s,cfg);CHECK(frames.size()==3);CHECK(calls==3);
 Renderer timed=[&](const Snapshot& snap,const RenderConfig& conf){CHECK(!conf.trace);return r(snap,conf);};
 auto m=benchmark(timed,s,cfg,2,6);CHECK(calls==11);CHECK(m.milliseconds.size()==6);CHECK(m.p95>=m.median);CHECK(cfg.trace);
 bool threw=false;try{benchmark(r,s,cfg,0,0);}catch(const std::invalid_argument&){threw=true;}CHECK(threw);
 threw=false;try{compare([](const Snapshot&,const RenderConfig&){return Frame{};},s,cfg);}catch(const std::runtime_error&){threw=true;}CHECK(threw);
 std::cout<<"E/F foundation tests passed (synthetic renderer; no rasterizer implemented).\n";
}

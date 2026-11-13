#pragma once
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
namespace cg {
// Proposed integration types: A/B/C/D must review before adopting.
struct Vec3 { double x=0,y=0,z=0; };
inline Vec3 operator+(Vec3 a,Vec3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
inline Vec3 operator-(Vec3 a,Vec3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
inline Vec3 operator*(Vec3 a,double s){return {a.x*s,a.y*s,a.z*s};}
inline double dot(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
inline double length(Vec3 a){return std::sqrt(dot(a,a));}
inline Vec3 normalized(Vec3 a){double n=length(a);if(n<=0)throw std::invalid_argument("zero direction");return a*(1/n);}
struct Body { int id; Vec3 center; double radius; };
struct Camera { Vec3 target; double yaw=0,pitch=0,distance=10; };
class CameraController {
public:
 Camera camera;
 void orbit(double dx,double dy){camera.yaw+=dx;camera.pitch=std::clamp(camera.pitch+dy,-1.55,1.55);}
 void zoom(double steps){camera.distance=std::clamp(camera.distance*std::exp(-steps*.1),.01,1e8);}
 Vec3 eye()const{double c=std::cos(camera.pitch);return camera.target+Vec3{std::sin(camera.yaw)*c,std::sin(camera.pitch),std::cos(camera.yaw)*c}*camera.distance;}
 void follow(std::optional<int> id){following=id;}
 void update(const std::vector<Body>& bodies){if(following)for(auto& b:bodies)if(b.id==*following){camera.target=b.center;return;}}
private: std::optional<int> following;
};
// Ray uses world coordinates. Selection is geometric, never resolved image color.
inline std::optional<int> pick(Vec3 origin,Vec3 direction,const std::vector<Body>& bodies){
 direction=normalized(direction);double nearest=std::numeric_limits<double>::infinity();std::optional<int> id;
 for(auto& b:bodies){if(b.radius<=0)continue;auto oc=origin-b.center;double v=dot(oc,direction),d=v*v-dot(oc,oc)+b.radius*b.radius;if(d<0)continue;double t=-v-std::sqrt(d);if(t<0)t=-v+std::sqrt(d);if(t>=0&&t<nearest){nearest=t;id=b.id;}}
 return id;
}
enum class Sampling { Single, SSAA4, MSAA4 };
struct RenderConfig { int width=640,height=360; Sampling sampling=Sampling::Single; bool trace=false; };
struct Snapshot { double time=0; Camera camera; std::vector<Body> bodies; }; // Geometry/material payload still owned by A/C.
struct Stats { std::uint64_t shadingCalls=0,coverageTests=0,bufferBytes=0; };
struct Frame { int width=0,height=0; std::vector<std::uint8_t> rgba; Stats stats; }; // Display sRGB RGBA8, top row first.
using Renderer=std::function<Frame(const Snapshot&,const RenderConfig&)>;
inline void validate(const Frame& f,const RenderConfig& c){if(c.width<=0||c.height<=0||f.width!=c.width||f.height!=c.height||f.rgba.size()!=std::size_t(c.width)*std::size_t(c.height)*4)throw std::runtime_error("invalid renderer frame");}
inline std::vector<Frame> compare(const Renderer& render,const Snapshot& snapshot,RenderConfig config){
 std::vector<Frame> frames;for(auto mode:{Sampling::Single,Sampling::SSAA4,Sampling::MSAA4}){config.sampling=mode;auto f=render(snapshot,config);validate(f,config);frames.push_back(std::move(f));}return frames;
}
struct Measurement { std::vector<double> milliseconds; double median=0,p95=0; Stats stats; };
inline Measurement benchmark(const Renderer& render,const Snapshot& snapshot,RenderConfig config,int warmup=5,int repeats=30){
 if(warmup<0||repeats<1||config.width<=0||config.height<=0)throw std::invalid_argument("invalid benchmark settings");
 config.trace=false;Measurement m;
 for(int i=0;i<warmup+repeats;++i){auto start=std::chrono::steady_clock::now();auto frame=render(snapshot,config);auto stop=std::chrono::steady_clock::now();validate(frame,config);if(i>=warmup){m.milliseconds.push_back(std::chrono::duration<double,std::milli>(stop-start).count());m.stats=frame.stats;}}
 auto sorted=m.milliseconds;std::sort(sorted.begin(),sorted.end());auto n=sorted.size();m.median=n%2?sorted[n/2]:(sorted[n/2-1]+sorted[n/2])/2;m.p95=sorted[std::size_t(std::ceil(.95*n))-1];return m;
}
} // namespace cg

#include "planet_sandbox/impact_visual.h"
#include <algorithm>
#include <cmath>
namespace planet_sandbox {
namespace {
double surface_grain(VisualVector p,double frequency) {
    const double x=p.x*frequency+31,y=p.y*frequency+47,z=p.z*frequency+59;
    const int ix=static_cast<int>(std::floor(x)),iy=static_cast<int>(std::floor(y)),iz=static_cast<int>(std::floor(z));
    const auto smooth=[](double t){return t*t*(3-2*t);};
    const double tx=smooth(x-ix),ty=smooth(y-iy),tz=smooth(z-iz);
    double value=0;
    for(int dz=0;dz<2;++dz) for(int dy=0;dy<2;++dy) for(int dx=0;dx<2;++dx) {
        std::uint32_t seed=std::uint32_t(ix+dx)*374761393u+std::uint32_t(iy+dy)*668265263u+
                           std::uint32_t(iz+dz)*2246822519u;
        seed=(seed^(seed>>13))*1274126177u;
        value+=double(seed^(seed>>16))/4294967295.0*
            (dx?tx:1-tx)*(dy?ty:1-ty)*(dz?tz:1-tz);
    }
    return value;
}
}

std::vector<ImpactSurfacePatch> impact_surface_patches(const BodySpec& body,const SceneSnapshot& snapshot,
    std::optional<BodyKind> rendered_kind) {
    std::vector<ImpactSurfacePatch> patches;
    for(auto it=snapshot.events.rbegin();it!=snapshot.events.rend() && patches.size()<4;++it) {
        const double age=snapshot.time-it->time;
        if(it->survivor!=body.id || !it->contact || !std::isfinite(age) || age<0 || age>=impact_surface_lifetime) continue;
        const auto& c=*it->contact;
        const double length=std::sqrt(c.normal.x*c.normal.x+c.normal.y*c.normal.y+c.normal.z*c.normal.z);
        if(!std::isfinite(length) || length<1e-12 || !std::isfinite(c.survivor_radius) ||
           !std::isfinite(c.absorbed_radius) || c.survivor_radius<=0 || c.absorbed_radius<=0) continue;
        const auto rotation=body_surface_rotation(rendered_kind.value_or(body.kind),body.appearance.seed,
                                                   body.design.axis_tilt_offset,it->time);
        const auto local=rotation.to_local({c.normal.x/length,c.normal.y/length,c.normal.z/length});
        const double ratio=std::clamp(c.absorbed_radius/c.survivor_radius,.1,2.0);
        const double fade=1-age/impact_surface_lifetime;
        patches.push_back({local,std::cos(.28+.22*ratio),std::exp(-age/1.5)*fade,
                           .32*std::min(1.0,age)*fade});
    }
    return patches;
}

ImpactSurfaceResponse sample_impact_surface(const std::vector<ImpactSurfacePatch>& patches,double u,double v) {
    ImpactSurfaceResponse response;
    if(patches.empty()) return response;
    constexpr double pi=3.14159265358979323846;
    const double theta=pi*v,phi=2*pi*u;
    const VisualVector radial{std::sin(theta)*std::cos(phi),std::cos(theta),std::sin(theta)*std::sin(phi)};
    for(const auto& patch:patches) {
        const double alignment=radial.x*patch.local_direction.x+radial.y*patch.local_direction.y+radial.z*patch.local_direction.z;
        if(alignment<=patch.cosine_radius) continue;
        const double base=std::clamp((alignment-patch.cosine_radius)/(1-patch.cosine_radius),0.0,1.0);
        // Body-local 3D noise avoids a longitude seam and grid-like molten patterns.
        const double t=std::clamp(base+.28*(surface_grain(radial,11)-.5)*(1-base),0.0,1.0);
        const double mask=t*t*(3-2*t);
        const double grain=.38+.62*surface_grain(radial,43);
        const double hot=patch.heat*mask*grain;
        response.emission[0]+=hot*3.2;
        response.emission[1]+=hot*(.24+patch.heat*1.3);
        response.emission[2]+=hot*(.025+patch.heat*patch.heat*.65);
        response.darkening=std::min(.65,response.darkening+patch.darkening*mask*grain);
    }
    return response;
}

std::vector<ImpactSprite> impact_sprites(const Project& project, const SceneSnapshot& snapshot) {
    std::vector<ImpactSprite> result;
    int active=0;
    for(auto it=snapshot.events.rbegin();it!=snapshot.events.rend() && active<4;++it) {
        const double age=snapshot.time-it->time;
        if(!it->contact || !std::isfinite(age) || age<0 || age>=impact_visual_lifetime) continue;
        const auto& c=*it->contact;
        if(!std::isfinite(c.normal.z) || !std::isfinite(c.position.z) || !std::isfinite(c.velocity.z) ||
           !std::isfinite(c.normal.x) || !std::isfinite(c.normal.y) ||
           !std::isfinite(c.position.x) || !std::isfinite(c.position.y) ||
           !std::isfinite(c.velocity.x) || !std::isfinite(c.velocity.y) ||
           !std::isfinite(c.survivor_radius) || c.survivor_radius<=0 ||
           !std::isfinite(c.absorbed_radius) || c.absorbed_radius<=0 ||
           !std::isfinite(c.relative_speed)) continue;
        const auto spec=std::find_if(project.bodies.begin(),project.bodies.end(),
            [&](const auto& body){return body.id==it->survivor;});
        if(spec==project.bodies.end()) continue;
        const double nlength=std::sqrt(c.normal.x*c.normal.x+c.normal.y*c.normal.y+c.normal.z*c.normal.z);
        if(nlength<1e-12) continue;
        const double nx=c.normal.x/nlength,ny=c.normal.y/nlength,nz=c.normal.z/nlength;
        const double xy=std::hypot(nx,ny);
        const VisualVector tangent=xy>1e-12?VisualVector{-ny/xy,nx/xy,0}:VisualVector{1,0,0};
        const VisualVector across{ny*tangent.z-nz*tangent.y,nz*tangent.x-nx*tangent.z,nx*tangent.y-ny*tangent.x};
        WorldVector center{c.survivor_position.x+c.velocity.x*age,c.survivor_position.y+c.velocity.y*age,c.survivor_position.z+c.velocity.z*age};
        const auto live=std::find_if(snapshot.bodies.begin(),snapshot.bodies.end(),
            [&](const auto& body){return body.id==it->survivor;});
        if(live!=snapshot.bodies.end()) center=live->position;
        const double merged_radius=live!=snapshot.bodies.end()?live->radius:
            std::cbrt(c.survivor_radius*c.survivor_radius*c.survivor_radius+
                      c.absorbed_radius*c.absorbed_radius*c.absorbed_radius);
        const double radius=merged_radius*project.display_scale*
            (project.solar_system?spec->display_radius/spec->radius:1.0);
        if(!std::isfinite(radius) || radius<=0) continue;
        if(!std::isfinite(center.z) || !std::isfinite(center.x) || !std::isfinite(center.y)) continue;
        ++active;
        const int owner=static_cast<int>(spec-project.bodies.begin());
        const double x=center.x+nx*radius*.92,y=center.y+ny*radius*.92,z=center.z+nz*radius*.92;
        const double strength=std::clamp(.35+.2*std::log1p(std::max(0.0,c.relative_speed)),.35,1.0);
        const double fade=1-age/impact_visual_lifetime;
        result.push_back({x,y,z,radius*.56,.48*fade*fade,{1.9,.28,.025},owner});
        if(age<.32) result.push_back({x,y,z,radius*(.5+age*2),
            strength*std::exp(-age/.07),{5.0,3.0,1.1},owner});
        std::uint32_t identity=2166136261u;
        for(const auto& name:{it->survivor,it->absorbed})
            for(unsigned char byte:name) identity=(identity^byte)*16777619u;
        // The same recorded event and snapshot produce the same visual particles.
        for(int particle=0;particle<24;++particle) {
            std::uint32_t seed=identity^(static_cast<std::uint32_t>(particle+1)*374761393u);
            seed=(seed^(seed>>13))*1274126177u;
            const double random=double(seed^(seed>>16))/4294967295.0;
            const double angle=(random-.5)*2.2;
            const double nx2=nx*std::cos(angle)+tangent.x*std::sin(angle);
            const double ny2=ny*std::cos(angle)+tangent.y*std::sin(angle);
            const double nz2=nz*std::cos(angle)+tangent.z*std::sin(angle);
            const double scatter=radius*age*std::sin(particle*2.4)*1.5;
            const double travel=radius*age*(1.3+random*2.6);
            result.push_back({x+nx2*travel+across.x*scatter,y+ny2*travel+across.y*scatter,
                z+nz2*travel+across.z*scatter,
                radius*(.035+random*.03)*(1+age*.4),
                (.5+random*.3)*fade*fade,{2.8, .35+random*.5,.04},owner});
        }
    }
    return result;
}
}

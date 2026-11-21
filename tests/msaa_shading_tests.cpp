// Exercise the production rasterizer with controlled screen-space geometry.
#include "../src/raster_renderer.cpp"
#include "pixel_trace_checks.h"
#include <iostream>

#ifdef NDEBUG
#error MSAA shading tests require assertions
#endif

using namespace planet_sandbox;

static Vertex at(const Project& p,const RenderConfig& c,double x,double y,double depth=1) {
    const double scale=c.height/(2*half_fov_tangent),distance=p.view_extent*1.35/half_fov_tangent;
    return {{(x-c.width*.5)*depth/scale,-(y-c.height*.5)*depth/scale,distance-depth},
            {0,0,1},x/7,y/9};
}

static std::array<double,3> analytic_weights(const std::array<ScreenVertex,3>& p,Vec2 q) {
    const auto& a=p[0];const auto& b=p[1];const auto& c=p[2];
    const double denominator=(b.y-c.y)*(a.x-c.x)+(c.x-b.x)*(a.y-c.y);
    const double u=((b.y-c.y)*(q.x-c.x)+(c.x-b.x)*(q.y-c.y))/denominator;
    const double v=((c.y-a.y)*(q.x-c.x)+(a.x-c.x)*(q.y-c.y))/denominator;
    return {u,v,1-u-v};
}

static std::array<ScreenVertex,3> screen_triangle(std::array<Vec2,3> points) {
    std::array<ScreenVertex,3> out{};
    for(int i=0;i<3;++i) {
        auto& p=out[i];p.x=points[i].x;p.y=points[i].y;p.depth=1;
        p.u=(p.x+1)/4;p.v=(p.y+1)/4;p.normal={0,0,1};p.position={p.x,p.y,1};
    }
    return out;
}

static unsigned geometry(const raster_detail::TriangleCoverage& coverage,MsaaWeights& weights) {
    unsigned mask=0;constexpr auto scale=raster_detail::subpixel_scale;
    for(int s=0;s<4;++s) {
        const auto pos=sample_offset(4,s);
        if(coverage.weights(std::int64_t(pos.x*scale),std::int64_t(pos.y*scale),weights[s])) mask|=1u<<s;
    }
    return mask;
}

static void test_geometric_selection() {
    struct Case {std::array<Vec2,3> points;unsigned mask;TraceShadingLocation rule;Vec2 position;};
    const std::array<Case,3> cases{{
        {{{{-1,-1},{3,-1},{-1,3}}},15,TraceShadingLocation::PixelCenter,{.5,.5}},
        {{{{-1,.375},{3,.375},{1,-2}}},3,TraceShadingLocation::CoverageCentroid,{.5,.25}},
        {{{{0,0},{.625,0},{0,.625}}},1,TraceShadingLocation::CoverageCentroid,{.25,.25}}
    }};
    for(const auto& item:cases) for(int permutation=0;permutation<6;++permutation) {
        auto p=screen_triangle(item.points);
        if(permutation>=3) std::swap(p[1],p[2]);
        std::rotate(p.begin(),p.begin()+permutation%3,p.end());
        raster_detail::TriangleCoverage coverage({{{p[0].x,p[0].y},{p[1].x,p[1].y},{p[2].x,p[2].y}}});
        MsaaWeights weights{};const auto mask=geometry(coverage,weights);
        assert(mask==item.mask);MsaaPoint chosen;
        assert(choose_msaa_point(coverage,p,{0,0,2},false,0,0,mask,weights,chosen));
        assert(chosen.location==item.rule && chosen.position.x==item.position.x && chosen.position.y==item.position.y);
        assert(std::abs(chosen.attributes.u-(chosen.position.x+1)/4)<1e-14);
    }
    auto p=screen_triangle({{{-1,-1},{3,-1},{-1,3}}});
    p[0].depth=1;p[1].depth=3;p[2].depth=2;
    raster_detail::TriangleCoverage coverage({{{-1,-1},{3,-1},{-1,3}}});
    MsaaWeights weights{};assert(geometry(coverage,weights)==15);
    // Independently exercise the rational centroid, even though center-first
    // selection usually uses the center for a three-covered-sample triangle.
    const auto mean=coverage_centroid(7,weights);
    assert(mean.first.x==5./12 && mean.first.y==5./12);
    assert(mean.first.x*raster_detail::subpixel_scale!=std::round(mean.first.x*raster_detail::subpixel_scale));
    const auto expected=analytic_weights(p,mean.first);
    for(int k=0;k<3;++k) assert(std::abs(mean.second[k]-expected[k])<1e-14);
    double inverse=0,u=0,average_u=0;
    for(int k=0;k<3;++k) {inverse+=expected[k]/p[k].depth;u+=expected[k]*p[k].u/p[k].depth;}
    const auto point=interpolate_point(p,mean.second);
    assert(std::abs(point.depth-1/inverse)<1e-14 && std::abs(point.u-u/inverse)<1e-14);
    for(int s=0;s<3;++s) average_u+=interpolate_point(p,weights[s]).u/3;
    assert(std::abs(point.u-average_u)>1e-4); // Averaging corrected UVs is a different operation.
    MsaaPoint empty;assert(!choose_msaa_point(coverage,p,{0,0,2},false,0,0,0,weights,empty));
}

static void test_geometry_fallback() {
    // Three genuine coverage samples: the center is geometrically inside but
    // cloud-facing validation rejects it. The unsnapped 5/12 centroid is valid.
    {
        auto three=screen_triangle({{{0,0},{1.25,0},{0,1.25}}});
        for(auto& vertex:three) vertex.normal={0,0,.45-vertex.x};
        raster_detail::TriangleCoverage coverage({{{0,0},{1.25,0},{0,1.25}}});
        MsaaWeights weights{};assert(geometry(coverage,weights)==7);
        MsaaPoint chosen;
        assert(choose_msaa_point(coverage,three,{0,0,2},true,0,0,7,weights,chosen));
        assert(chosen.location==TraceShadingLocation::CoverageCentroid);
        assert(chosen.position.x==5./12 && chosen.position.y==5./12);
        assert(chosen.center_issue==TracePointIssue::BackFacing);
        assert(chosen.centroid_issue==TracePointIssue::None);
    }
    auto p=screen_triangle({{{-1,-1},{3,-1},{-1,3}}});
    raster_detail::TriangleCoverage coverage({{{-1,-1},{3,-1},{-1,3}}});
    MsaaWeights weights{};const auto mask=geometry(coverage,weights);
    for(double crossing:{.35,.5}) {
        for(auto& vertex:p) vertex.normal={0,0,crossing-vertex.x};
        MsaaPoint chosen;
        assert(choose_msaa_point(coverage,p,{0,0,2},true,0,0,mask,weights,chosen));
        assert(chosen.location==TraceShadingLocation::CoveredSample);
        assert(chosen.position.x==.25 && chosen.position.y==.25);
        const auto issue=crossing==.5?TracePointIssue::InvalidInterpolation:TracePointIssue::BackFacing;
        assert(chosen.center_issue==issue && chosen.centroid_issue==issue);
    }
    for(auto& vertex:p) vertex.normal={0,0,-1};
    MsaaPoint none;assert(!choose_msaa_point(coverage,p,{0,0,2},true,0,0,mask,weights,none));
    for(auto& vertex:p) vertex.u=std::numeric_limits<double>::infinity();
    assert(!choose_msaa_point(coverage,p,{0,0,2},false,0,0,mask,weights,none));
}

static void test_opaque_order() {
    Project p;p.view_extent=.2;
    RenderConfig c;c.width=c.height=12;c.sampling=SamplingMode::MSAA4;c.trace_x=c.trace_y=6;
    Appearance a{17,"#ff0000","#0000ff",12,1,0,0};
    Material back{a,{.9,.1,.1},{.1,.1,.9},true},front{a,{.1,.9,.1},{.1,.9,.1},true};
    std::array<Vertex,3> b{at(p,c,-4,-4),at(p,c,20,-4),at(p,c,-4,20)};
    std::array<Vertex,3> f{at(p,c,6,6,.5),at(p,c,6.6,6,.5),at(p,c,6,6.6,.5)};
    std::array<RenderFrame,2> frames;
    for(int order=0;order<2;++order) {
        Rasterizer r(p,c);
        // Procedural custom ring material is fully opaque and varies sharply
        // with u; it exposes location changes without transparent blending.
        if(order) {r.triangle(f,front,1,false);r.triangle(b,back,0,true);}
        else {r.triangle(b,back,0,true);r.triangle(f,front,1,false);}
        frames[order]=r.resolve();r.trace_pixel(frames[order]);
        check_pixel_process(*frames[order].trace);
    }
    int differences=0;
    for(std::size_t i=0;i<frames[0].image.rgb.size();i+=3)
        differences+=!std::equal(frames[0].image.rgb.begin()+i,frames[0].image.rgb.begin()+i+3,
                                 frames[1].image.rgb.begin()+i);
    std::cerr<<"Opaque submission-order differing pixels: "<<differences<<'\n';
    assert(differences==0);
    assert(frames[0].body_index==frames[1].body_index);
    for(const auto& frame:frames) {
        const auto event=std::find_if(frame.trace->events.begin(),frame.trace->events.end(),[](const auto& e){return e.body_index==0;});
        assert(event!=frame.trace->events.end() && event->shading.size()==1);
        assert(event->shading[0].location==TraceShadingLocation::PixelCenter);
        assert(event->shading[0].position.x==.5 && event->shading[0].position.y==.5);
    }
}

static void test_cloud_fallback_and_depth() {
    Project p;p.view_extent=.2;RenderConfig c;c.width=c.height=12;
    c.sampling=SamplingMode::MSAA4;c.trace_x=c.trace_y=6;
    Appearance a{1,"#ffffff","#ffffff",0,0,0,0};DesignOverrides design;
    Texture map;map.width=map.height=2;map.rgba.assign(16,255);
    Material cloud{a,{1,1,1},{1,1,1},true},front{a,{.1,.1,.1},{.1,.1,.1},true};
    cloud.cloud_pass=true;cloud.cloud_map=&map;cloud.design=&design;
    std::array<Vertex,3> mesh{at(p,c,-4,-4),at(p,c,20,-4),at(p,c,-4,20)};
    for(int i=0;i<3;++i) mesh[i].normal={0,0,6.35-std::array<double,3>{-4,20,-4}[i]};
    for(int blocked=0;blocked<3;++blocked) {
        RenderFrame traced;
        for(bool recording:{true,false}) {
            c.trace_process=recording;Rasterizer r(p,c);
            if(blocked==1) r.triangle({at(p,c,6,6,.5),at(p,c,6.6,6,.5),at(p,c,6,6.6,.5)},front,1,false);
            if(blocked==2) r.triangle({at(p,c,-4,-4,.5),at(p,c,20,-4,.5),at(p,c,-4,20,.5)},front,1,false);
            const auto before=r.resolve().shade_evaluations;
            r.begin_cloud_layer();r.triangle(mesh,cloud,0,false);
            auto frame=r.resolve();r.trace_pixel(frame);
            if(blocked==2) assert(frame.shade_evaluations==before);
            if(recording) {
                traced=frame;check_pixel_process(*frame.trace);
                const auto event=std::find_if(frame.trace->events.begin(),frame.trace->events.end(),[](const auto& e){return e.body_index==0;});
                assert(event!=frame.trace->events.end());
                if(blocked==2) {assert(event->shading.empty());continue;}
                assert(event->shading.size()==1);
                const auto& shade=event->shading[0];
                assert(shade.location==TraceShadingLocation::CoveredSample);
                assert(shade.position.x==.25 && shade.position.y==.25);
                assert(shade.center_issue==TracePointIssue::BackFacing && shade.centroid_issue==TracePointIssue::BackFacing);
                assert(event->samples[2].action==TraceAction::Written && event->samples[2].shading_index==0);
                if(blocked==1) assert(event->samples[0].action==TraceAction::DepthRejected);
                else assert(event->samples[0].action==TraceAction::Written);
                assert(event->samples[1].action==TraceAction::BackfaceRejected && event->samples[3].action==TraceAction::BackfaceRejected);
            } else {
                assert(frame.image.rgb==traced.image.rgb && frame.body_index==traced.body_index);
                assert(frame.shade_evaluations==traced.shade_evaluations);
            }
        }
    }
}

static void test_all_depth_masks_and_invalid_point() {
    Project p;p.view_extent=.2;RenderConfig c;c.width=c.height=12;
    c.sampling=SamplingMode::MSAA4;c.trace_x=c.trace_y=6;
    Appearance a{1,"#ffffff","#222222",0,0,0,0};
    Material back{a,{.8,.1,.2},{.1,.2,.8},true},front{a,{.1,.8,.1},{.1,.8,.1},true};
    std::array<Vertex,3> mesh{at(p,c,-4,-4),at(p,c,20,-4),at(p,c,-4,20)};
    std::array<double,3> shared{};
    for(unsigned blocked=0;blocked<16;++blocked) {
        Rasterizer r(p,c);
        for(int s=0;s<4;++s) if(blocked&(1u<<s)) {
            const auto q=sample_offset(4,s);const double x=6+q.x,y=6+q.y;
            r.triangle({at(p,c,x-.125,y-.125,.5),at(p,c,x+.25,y-.125,.5),at(p,c,x-.125,y+.25,.5)},front,1,false);
        }
        r.triangle(mesh,back,0,true);auto frame=r.resolve();r.trace_pixel(frame);check_pixel_process(*frame.trace);
        const auto e=std::find_if(frame.trace->events.begin(),frame.trace->events.end(),[](const auto& e){return e.body_index==0;});
        assert(e!=frame.trace->events.end());
        if(blocked==15) assert(e->shading.empty());
        else {
            assert(e->shading.size()==1 && e->shading[0].location==TraceShadingLocation::PixelCenter);
            if(!blocked) shared=e->shading[0].linear_rgb;
            else assert(e->shading[0].linear_rgb==shared);
        }
        for(int s=0;s<4;++s) {
            assert(e->samples[s].covered);
            assert(e->samples[s].action==((blocked&(1u<<s))?TraceAction::DepthRejected:TraceAction::Written));
        }
    }
    for(auto& v:mesh) v.normal={0,0,0};
    Rasterizer invalid(p,c);invalid.triangle(mesh,back,0,false);
    auto frame=invalid.resolve();invalid.trace_pixel(frame);check_pixel_process(*frame.trace);
    const auto& e=frame.trace->events[0];
    assert(e.shading.empty() && e.shading_failure==TracePointIssue::NoValidCoveredPoint);
    for(const auto& s:e.samples) assert(s.action==TraceAction::InvalidAttributes && s.after.body_index==-1);
}

static void test_sample_depth_uv_and_shading() {
    // Both surface and cloud receivers must keep their own perspective depth
    // and picking coordinates while sharing the independently checked color.
    for(bool cloud:{false,true}) {
    Project p;p.view_extent=.2;RenderConfig c;c.width=c.height=12;
    c.sampling=SamplingMode::MSAA4;c.trace_x=c.trace_y=6;
    Appearance a{11,"#ffffff","#111111",7,.6,0,0};Material material{a,{.8,.6,.1},{.1,.2,.8},true};
    DesignOverrides design;Texture map;map.width=map.height=2;map.rgba.assign(16,255);
    material.design=&design;material.cloud_map=cloud?&map:nullptr;material.cloud_pass=cloud;
    const std::array<Vec2,3> coords{{{-4,-4},{20,-4},{-4,20}}};
    const std::array<double,3> depths{{.6,1.4,2.1}};
    std::array<Vertex,3> vertices;
    auto projected=screen_triangle(coords);
    for(int i=0;i<3;++i) {
        vertices[i]=at(p,c,coords[i].x,coords[i].y,depths[i]);projected[i].depth=depths[i];
    }
    const auto expected_at=[&](Vec2 position) {
        const auto w=analytic_weights(projected,position);double inverse=0;
        for(int i=0;i<3;++i) inverse+=w[i]/depths[i];
        InterpolatedPoint result;result.depth=1/inverse;result.normal={0,0,1};
        for(int i=0;i<3;++i) {
            const double factor=w[i]/depths[i]/inverse;
            result.u+=factor*vertices[i].u;result.v+=factor*vertices[i].v;
            result.world=result.world+vertices[i].position*factor;
        }
        return result;
    };
    Rasterizer r(p,c);if(cloud)r.begin_cloud_layer();r.triangle(vertices,material,0,false);
    auto frame=r.resolve();r.trace_pixel(frame);check_pixel_process(*frame.trace);
    const auto& event=frame.trace->events[0];assert(event.shading.size()==1);
    double nearest=std::numeric_limits<double>::infinity();Vec2 expected_uv;
    for(int s=0;s<4;++s) {
        const auto offset=sample_offset(4,s);const auto expected=expected_at({6+offset.x,6+offset.y});
        assert(std::abs(event.samples[s].after.depth-expected.depth)<1e-12);
        if(expected.depth<nearest) {nearest=expected.depth;expected_uv={expected.u,expected.v};}
    }
    const auto uv=frame.surface_uv[6*c.width+6];
    assert(std::abs(uv.x-expected_uv.x)<1e-12 && std::abs(uv.y-expected_uv.y)<1e-12);
    const auto point=expected_at({6.5,6.5});
    assert(std::abs(uv.x-point.u)>1e-4); // Picking must not use the shared shading UV.
    const Vec3 camera{0,0,p.view_extent*1.35/half_fov_tangent};
    const auto expected=shade_fragment(material,point.u,point.v,point.normal,point.world,normalize(camera-point.world),false);
    const std::array<double,3> rgb{{expected.color.r,expected.color.g,expected.color.b}};
    for(int k=0;k<3;++k) assert(std::abs(event.shading[0].linear_rgb[k]-rgb[k])<1e-12);
    }
}

static void test_zero_alpha_and_blending() {
    Project p;p.view_extent=.2;RenderConfig c;c.width=c.height=12;
    c.sampling=SamplingMode::MSAA4;c.trace_x=c.trace_y=6;
    Appearance a{1,"#ffffff","#ffffff",0,0,0,0};DesignOverrides design;
    Material material{a,{1,1,1},{1,1,1},true};material.kind=BodyKind::Uranus;material.design=&design;
    for(double width:{0.,1.}) {
        design.ring_width=width;Rasterizer r(p,c);
        r.triangle({at(p,c,-4,-4),at(p,c,20,-4),at(p,c,-4,20)},material,0,true);
        auto frame=r.resolve();r.trace_pixel(frame);check_pixel_process(*frame.trace);
        const auto& event=frame.trace->events[0];assert(event.shading.size()==1);
        for(const auto& step:event.samples) {
            assert(step.covered && step.shading_index==0);
            assert(step.action==(width==0?TraceAction::Transparent:TraceAction::Written));
        }
        assert(event.shading[0].alpha==(width==0?0:event.samples[0].alpha));
    }
}

int main() {
    test_geometric_selection();test_geometry_fallback();
    test_opaque_order();
    test_all_depth_masks_and_invalid_point();
    test_cloud_fallback_and_depth();test_sample_depth_uv_and_shading();test_zero_alpha_and_blending();
    std::cout<<"MSAA shading location tests passed\n";
}

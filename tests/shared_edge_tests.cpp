// Compile the production rasterizer into this white-box test translation unit.
// This exercises its real projection, clipping, depth, shading and trace path
// with controlled meshes, without adding a test API to the application.
#include "../src/raster_renderer.cpp"
#include "pixel_trace_checks.h"
#include "planet_sandbox/aa_benchmark.h"
#include "planet_sandbox/solar_system.h"
#include <iostream>
#include <random>

#ifdef NDEBUG
#error Shared edge tests require assertions
#endif

using namespace planet_sandbox;
using raster_detail::TriangleCoverage;
using raster_detail::subpixel_scale;
using raster_detail::subpixel_step;
using Triangle2 = std::array<Vec2,3>;

static std::int64_t fixed(double x) { return std::llround(x*subpixel_scale); }
static bool covered(const TriangleCoverage& t, Vec2 p) {
    std::array<double,3> w;
    const bool hit = t.weights(fixed(p.x),fixed(p.y),w);
    if (hit) {
        for (double v : w) assert(std::isfinite(v) && v >= 0 && v <= 1.000000000000001);
        assert(std::abs(w[0]+w[1]+w[2]-1) < 1e-14);
    }
    return hit;
}

static Triangle2 permute(Triangle2 t, int permutation) {
    if (permutation >= 3) std::swap(t[1],t[2]);
    std::rotate(t.begin(),t.begin()+permutation%3,t.end());
    return t;
}

static void test_rectangles() {
    std::uint64_t checked = 0;
    for (Vec2 origin : {Vec2{0,0},Vec2{.5,.5},Vec2{.25,.75},Vec2{-.375,-1.125}}) {
        const Vec2 a=origin, b{a.x+4,a.y}, c{a.x+4,a.y+4}, d{a.x,a.y+4};
        for (int diagonal=0; diagonal<2; ++diagonal) {
            const Triangle2 first = diagonal ? Triangle2{a,b,d} : Triangle2{a,b,c};
            const Triangle2 second = diagonal ? Triangle2{b,c,d} : Triangle2{a,c,d};
            for (int p=0;p<6;++p) for (int q=0;q<6;++q) {
                const TriangleCoverage t0(permute(first,p)),t1(permute(second,q));
                assert(t0.valid() && t1.valid());
                for (auto mode : {SamplingMode::Single,SamplingMode::SSAA4,SamplingMode::MSAA4,SamplingMode::SSAA16}) {
                    const int count=sampling_count(mode);
                    for (int y=-3;y<7;++y) for (int x=-2;x<7;++x) for (int s=0;s<count;++s) {
                        const auto offset=sample_offset(count,s);
                        const Vec2 point{x+offset.x,y+offset.y};
                        const int hits=int(covered(t0,point))+int(covered(t1,point));
                        const bool expected=point.x>=a.x && point.x<b.x && point.y>=a.y && point.y<d.y;
                        assert(hits==int(expected));
                        ++checked;
                    }
                }
                // Exact outer vertices and edges: left/top included; right/bottom excluded.
                for (Vec2 v : {a,b,c,d,Vec2{a.x,a.y+2},Vec2{a.x+2,a.y},Vec2{b.x,b.y+2},Vec2{d.x+2,d.y}}) {
                    const bool expected=v.x>=a.x && v.x<b.x && v.y>=a.y && v.y<d.y;
                    assert(int(covered(t0,v))+int(covered(t1,v))==int(expected));
                }
            }
        }
    }
    std::cout << "Rectangle samples checked: " << checked << "; duplicate/missing owners: 0\n";
}

static void test_edges_and_numerics() {
    // Horizontal, vertical, sloping shared edges with unequal triangle areas.
    for (const auto& pair : std::array<std::pair<Triangle2,Triangle2>,3>{{
            {{{{0,.5},{4,.5},{1,-2}}},{{{4,.5},{0,.5},{3,6}}}},
            {{{{.5,0},{.5,4},{-2,1}}},{{{.5,4},{.5,0},{6,3}}}},
            {{{{0,0},{4,4},{-2,1}}},{{{4,4},{0,0},{7,2}}}}
        }}) {
        for (int p=0;p<6;++p) for (int q=0;q<6;++q) {
            const TriangleCoverage a(permute(pair.first,p)),b(permute(pair.second,q));
            for (double t : {.125,.25,.5,.75,.875}) {
                const auto u=pair.first[0],v=pair.first[1];
                Vec2 at{u.x+(v.x-u.x)*t,u.y+(v.y-u.y)*t};
                for (Vec2 delta : {Vec2{0,0},Vec2{subpixel_step,0},Vec2{-subpixel_step,0},
                                  Vec2{0,subpixel_step},Vec2{0,-subpixel_step}})
                    assert(int(covered(a,{at.x+delta.x,at.y+delta.y}))+int(covered(b,{at.x+delta.x,at.y+delta.y}))==1);
            }
        }
    }
    assert(!TriangleCoverage({{{0,0},{0,0},{1,1}}}).valid());
    assert(!TriangleCoverage({{{0,0},{1,1},{2,2}}}).valid());
    assert(!TriangleCoverage({{{0,0},{1,.1},{2,.2}}}).valid()); // snapping alone would create area
    assert(!TriangleCoverage({{{0,0},{subpixel_step/4,0},{0,subpixel_step/4}}}).valid());
    // Preserve the old 0.001-pixel fixture: finer precision now retains its area.
    assert(TriangleCoverage({{{0,0},{.001,0},{0,.001}}}).valid());
    const TriangleCoverage tiny({{{0,0},{0,subpixel_step},{subpixel_step,0}}});
    assert(tiny.valid() && covered(tiny,{0,0}));
    for (double bad : {std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
        assert(!TriangleCoverage({{{0,0},{1,1},{bad,0}}}).valid());
    // Different original coordinates rounding onto the same edge give identical masks.
    for (double epsilon : {-1e-9,0.,1e-9}) {
        const TriangleCoverage a({{{.5+epsilon,0},{.5+epsilon,4},{4,2}}});
        for (int y=0;y<4;++y) assert(covered(a,{.5,y+.5}));
    }
    // Both sides of the int64 fast-path boundary, and huge finite doubles whose
    // products would overflow even double. The viewport remains fully covered.
    const double fast_bound=double(raster_detail::fast_subpixel_limit)/subpixel_scale;
    const double compact_bound=double(std::uint64_t{1}<<52)/subpixel_scale;
    for (double extent : {fast_bound-subpixel_step,fast_bound,fast_bound+subpixel_step,
                           compact_bound-subpixel_step,compact_bound,compact_bound+subpixel_step,
                           2097151.,2097153.,0x1p45,0x1p100,0x1p500,0x1p1023,
                           std::numeric_limits<double>::max()}) {
        const Triangle2 a{{{-extent,-extent},{extent,-extent},{extent,extent}}};
        const Triangle2 b{{{-extent,-extent},{extent,extent},{-extent,extent}}};
        for (int p=0;p<6;++p) {
            const TriangleCoverage t0(permute(a,p)),t1(permute(b,5-p));
            assert(t0.valid() && t1.valid());
            assert(t0.uses_wide_arithmetic()==(extent>fast_bound));
            for (Vec2 v : {Vec2{0,0},Vec2{.5,.5},Vec2{.5,.25},Vec2{.25,.5},Vec2{-3.25,7.125}})
                assert(int(covered(t0,v))+int(covered(t1,v))==1);
        }
    }
    // Shared edge between a fast triangle and a wide triangle, no approximation
    // or change of precision may change the half-open ownership.
    const TriangleCoverage small({{{.5,-4},{.5,4},{-2,0}}});
    const TriangleCoverage huge({{{.5,4},{.5,-4},{0x1p100,0}}});
    assert(!small.uses_wide_arithmetic() && huge.uses_wide_arithmetic());
    for (double dx : {-subpixel_step,0.,subpixel_step})
        assert(int(covered(small,{.5+dx,.25}))+int(covered(huge,{.5+dx,.25}))==1);
    // Large collinear coordinates must not create an artificial skinny triangle.
    assert(!TriangleCoverage({{{0,0},{0x1p500,0x1p500},{0x1p501,0x1p501}}}).valid());
    // The binary64 conversion shortcut must use the selected grid, including
    // negative coordinates and the representable values just below its cutoff.
    for(double sign : {-1.,1.}) for(double delta : {-subpixel_step,0.,subpixel_step}) {
        const double origin=sign*(raster_detail::grid_aligned_min+delta);
        const TriangleCoverage t({{{origin,origin},{origin+4,origin},{origin,origin+4}}});
        assert(t.valid() && t.uses_wide_arithmetic());
        assert(covered(t,{origin+.5,origin+.5}));
        assert(!covered(t,{origin+3,origin+3}));
    }
    const TriangleCoverage compact({{{-100000,-100000},{100000,-100000},{0,100000}}});
    for(auto x:{std::numeric_limits<std::int64_t>::min(),std::numeric_limits<std::int64_t>::max()}) {
        std::array<double,3> weights;
        assert(!compact.weights(x,0,weights));
        assert(!compact.weights(0,x,weights));
    }
}

static void test_cloud_precision_regressions() {
    auto scene=make_aa_benchmark_scene(make_solar_system_project(),AAScene::Earth,320,240);
    struct Probe {
        SamplingMode mode;int x,y;std::uint64_t triangle;int sample;
        TraceAction action;double alpha;std::array<int,3> rgb;
    };
    // Fixed physical fixtures, independently measured with a 20-bit top-left
    // renderer. These bound visible interpolation errors, not all image changes.
    const std::array<Probe,6> probes{{
        {SamplingMode::Single,72,67,12170,0,TraceAction::Written,.273643,{135,137,141}},
        {SamplingMode::SSAA4,80,56,11786,0,TraceAction::Written,.183595,{124,126,129}},
        {SamplingMode::SSAA4,59,100,13131,3,TraceAction::BackfaceRejected,0,{21,27,35}},
        {SamplingMode::SSAA4,66,155,14466,3,TraceAction::Written,.005269,{38,40,44}},
        {SamplingMode::MSAA4,80,56,11786,0,TraceAction::Written,.183595,{124,126,130}},
        {SamplingMode::MSAA4,59,100,13131,3,TraceAction::BackfaceRejected,0,{20,26,33}}
    }};
    for(const auto& probe:probes) {
        auto c=scene.config;c.sampling=probe.mode;c.trace_x=probe.x;c.trace_y=probe.y;
        const auto frame=render_scene(scene.project,scene.snapshot,c);
        check_pixel_process(*frame.trace);
        const auto found=std::find_if(frame.trace->events.begin(),frame.trace->events.end(),
            [&](const auto& e){return e.primitive==TracePrimitive::Triangle &&
                e.surface==TraceSurface::Cloud && e.primitive_id==probe.triangle;});
        assert(found!=frame.trace->events.end());
        const auto& sample=found->samples[probe.sample];
        assert(sample.covered && sample.action==probe.action);
        assert(std::abs(sample.alpha-probe.alpha)<.001);
        for(int channel=0;channel<3;++channel)
            assert(std::abs(int(frame.image.rgb[(probe.y*c.width+probe.x)*3+channel])-probe.rgb[channel])<=1);
    }
    std::cout<<"Six cloud precision probes: alpha, culling, depth-dependent output and replay passed\n";
}

static void test_random_masks() {
    std::mt19937 random(147);
    for (int iteration=0;iteration<1000;++iteration) {
        Triangle2 triangle;
        // Keep the original physical test geometry on its 1/256 grid, even
        // when production precision changes. Convert sample units separately.
        for (auto& p : triangle) p={double(int(random()%4097)-2048)/256,double(int(random()%4097)-2048)/256};
        const TriangleCoverage original(triangle);
        // A power-of-two enlargement exercises the portable wide arithmetic,
        // while its local relative geometry and barycentric weights stay equal.
        Triangle2 large=triangle;
        for (auto& p : large) { p.x*=0x1p32; p.y*=0x1p32; }
        const TriangleCoverage wide(large), reversed(permute(triangle,4));
        for (int n=0;n<8;++n) {
            const std::int64_t x=fixed(double(int(random()%4097)-2048)/256),
                               y=fixed(double(int(random()%4097)-2048)/256);
            std::array<double,3> w,v,r;
            const bool hit=original.weights(x,y,w);
            assert(hit==reversed.weights(x,y,r));
            assert(hit==wide.weights(x*std::int64_t{4294967296},y*std::int64_t{4294967296},v));
            if (hit) for (int i=0;i<3;++i) assert(std::abs(w[i]-v[i])<1e-14);
            // Move this sample to the origin and enlarge further. This forces
            // the arbitrary-precision path, independently checking both the
            // int64 and the allocation-free two-word path at identical weights.
            Triangle2 enormous=triangle;
            Triangle2 shifted=triangle;
            for(auto& p:shifted) {p.x=(p.x-double(x)/subpixel_scale)*0x1p32;
                                 p.y=(p.y-double(y)/subpixel_scale)*0x1p32;}
            const TriangleCoverage compact_at_origin(shifted);
            assert(hit==compact_at_origin.weights(0,0,r));
            if(hit) for(int i=0;i<3;++i) assert(std::abs(w[i]-r[i])<1e-14);
            for(auto& p:enormous) {p.x=(p.x-double(x)/subpixel_scale)*0x1p64;
                                  p.y=(p.y-double(y)/subpixel_scale)*0x1p64;}
            const TriangleCoverage exact(enormous);
            assert(hit==exact.weights(0,0,r));
            if(hit) for(int i=0;i<3;++i) assert(std::abs(w[i]-r[i])<1e-14);
        }
    }
}

// Inverse projection for yaw=pitch=0, preserving a known screen-space mesh.
static Vertex vertex(const Project& project,const RenderConfig& c,Vec2 screen,double depth=1) {
    const double distance=project.view_extent*1.35/half_fov_tangent*c.camera.zoom;
    const double scale=c.height/(2*half_fov_tangent);
    return {{(screen.x-c.width*.5)*depth/scale,-(screen.y-c.height*.5)*depth/scale,distance-depth},
            {0,0,1},screen.x/16,screen.y/16};
}

static void test_renderer_meshes() {
    Project p; p.view_extent=.2;
    Appearance appearance{1,"#ffffff","#ffffff",0,0,0,0};
    DesignOverrides design;
    design.ring_width=1;
    Material material{appearance,{.3,.2,.1},{.3,.2,.1},true};
    material.design=&design;
    RenderConfig c; c.width=12; c.height=12;
    const Triangle2 first{{{2.5,2.5},{9.5,2.5},{9.5,9.5}}};
    const Triangle2 second{{{2.5,2.5},{9.5,9.5},{2.5,9.5}}};
    const TriangleCoverage expected0(first),expected1(second);
    for (bool transparent : {false,true}) {
        material.kind=transparent ? BodyKind::Uranus : BodyKind::Custom;
        for (auto mode : {SamplingMode::Single,SamplingMode::SSAA4,SamplingMode::MSAA4,SamplingMode::SSAA16}) {
            c.sampling=mode;
            for (int y=1;y<11;++y) for (int x=1;x<11;++x) {
                c.trace_x=x; c.trace_y=y;
                RenderFrame baseline;
                for (int order=0;order<2;++order) {
                    Rasterizer renderer(p,c);
                    for (int n=0;n<2;++n) {
                        const int index=n^order;
                        const auto source=permute(index ? second : first,index+order*3);
                        std::array<Vertex,3> mesh;
                        for (int k=0;k<3;++k) mesh[k]=vertex(p,c,source[k]);
                        renderer.triangle(mesh,material,index,transparent);
                    }
                    auto frame=renderer.resolve(); renderer.trace_pixel(frame);
                    check_pixel_process(*frame.trace);
                    for (int s=0;s<sampling_count(mode);++s) {
                        const auto offset=sample_offset(sampling_count(mode),s);
                        const Vec2 point{x+offset.x,y+offset.y};
                        const bool e0=covered(expected0,point),e1=covered(expected1,point);
                        int count=0;
                        for (const auto& event : frame.trace->events) {
                            const auto& step=event.samples[s];
                            assert(step.covered==(event.body_index==0 ? e0 : e1));
                            if (step.covered) {
                                ++count;
                                assert(step.action==TraceAction::Written);
                                assert(step.blend==(transparent ? TraceBlend::Alpha : TraceBlend::Replace));
                            }
                        }
                        assert(count==int(e0)+int(e1) && count<=1);
                        const auto& final=frame.trace->samples[s];
                        assert(final.body_index==(e0 ? 0 : e1 ? 1 : -1));
                        if (count) assert(std::abs(final.depth-1)<1e-14);
                    }
                    if (order==0) baseline=frame;
                    else {
                        assert(frame.image.rgb==baseline.image.rgb);
                        assert(frame.body_index==baseline.body_index);
                        assert(frame.shade_evaluations==baseline.shade_evaluations);
                    }
                }
            }
        }
    }
    std::cout << "Real renderer: opaque/alpha seams have exactly one coverage/write; submission order preserved\n";
}

static void test_clipped_mesh() {
    Project p; p.view_extent=.2;
    Appearance appearance{1,"#ffffff","#ffffff",0,0,0,0};
    Material material{appearance,{.3,.2,.1},{.3,.2,.1},true};
    RenderConfig c; c.width=c.height=12;
    // Two adjacent triangles on one tilted plane, two vertices behind near.
    const std::array<Vec2,4> screen{{{1,1},{11,1},{11,11},{1,11}}};
    std::array<Vertex,4> v;
    for (int i=0;i<4;++i) v[i]=vertex(p,c,screen[i],i==0 || i==3 ? .025 : .1);
    std::uint64_t visible=0;
    bool split=false;
    for (auto mode : {SamplingMode::Single,SamplingMode::SSAA4,SamplingMode::MSAA4,SamplingMode::SSAA16}) {
        c.sampling=mode;
        for (int y=0;y<12;++y) for (int x=0;x<12;++x) {
            c.trace_x=x;c.trace_y=y;
            std::vector<int> expected;
            for (int order=0;order<2;++order) {
                Rasterizer r(p,c);
                std::array<Vertex,3> a{v[0],v[1],v[2]},b{v[0],v[2],v[3]};
                if (order) { std::reverse(a.begin(),a.end());std::reverse(b.begin(),b.end()); }
                if (order) { r.triangle(b,material,1,false);r.triangle(a,material,0,false); }
                else { r.triangle(a,material,0,false);r.triangle(b,material,1,false); }
                auto frame=r.resolve();r.trace_pixel(frame);check_pixel_process(*frame.trace);
                std::vector<int> owners(sampling_count(mode),-1);
                for (const auto& e : frame.trace->events) {
                    split|=e.clipped_part==1;
                    for (int s=0;s<sampling_count(mode);++s) if (e.samples[s].covered) {
                        assert(owners[s]==-1); // Includes edges between fan pieces.
                        owners[s]=e.body_index;
                        assert(e.samples[s].action==TraceAction::Written);
                        assert(*e.samples[s].candidate_depth>=near_plane-1e-12);
                        ++visible;
                    }
                }
                for (int s=0;s<sampling_count(mode);++s) {
                    const auto offset=sample_offset(sampling_count(mode),s);
                    // Perspective projection of the clipped plane has a vertical
                    // cut at x=23/3. Avoid only that quantized outer boundary.
                    const bool interior=x+offset.x>7.67 && x+offset.x<11 && y+offset.y>1 && y+offset.y<11;
                    if (interior) assert(owners[s]>=0);
                }
                if (!order) expected=owners;else assert(expected==owners);
            }
        }
    }
    assert(visible>0 && split);
    std::cout << "Near-clipped shared/fan edges: no duplicate coverage, no interior holes\n";
}

static void test_interpolation_and_large_render() {
    Project p; p.view_extent=.2;
    Appearance appearance{1,"#ffffff","#ffffff",0,0,0,0};
    Material material{appearance,{.3,.2,.1},{.3,.2,.1},true};
    RenderConfig c; c.width=c.height=12; c.sampling=SamplingMode::Single;
    const Triangle2 screen{{{1.13,1.27},{10.31,2.42},{3.19,10.61}}};
    const TriangleCoverage expected(screen);
    std::array<Vertex,3> vertices;
    const std::array<double,3> depths{{.6,1.4,2.1}};
    for (int k=0;k<3;++k) vertices[k]=vertex(p,c,screen[k],depths[k]);
    for (int permutation=0;permutation<6;++permutation) {
        auto reordered=vertices;
        if (permutation>=3) std::swap(reordered[1],reordered[2]);
        std::rotate(reordered.begin(),reordered.begin()+permutation%3,reordered.end());
        for (int y=1;y<11;++y) for (int x=1;x<11;++x) {
            c.trace_x=x;c.trace_y=y;
            Rasterizer renderer(p,c);renderer.triangle(reordered,material,0,false);
            auto frame=renderer.resolve();renderer.trace_pixel(frame);check_pixel_process(*frame.trace);
            std::array<double,3> w;
            const bool hit=expected.weights(fixed(x+.5),fixed(y+.5),w);
            assert(hit==(frame.trace->samples[0].body_index==0));
            if (!hit) continue;
            const double inverse=w[0]/depths[0]+w[1]/depths[1]+w[2]/depths[2];
            const auto uv=frame.surface_uv[y*c.width+x];
            double u=0,v=0;
            for (int k=0;k<3;++k) { u+=w[k]*vertices[k].u/depths[k];v+=w[k]*vertices[k].v/depths[k]; }
            assert(std::abs(frame.trace->samples[0].depth-1/inverse)<1e-13);
            assert(std::abs(uv.x-u/inverse)<1e-13 && std::abs(uv.y-v/inverse)<1e-13);
        }
    }
    c.trace_x=c.trace_y=6;c.sampling=SamplingMode::SSAA16;
    const double extent=1e100;
    const Triangle2 huge{{{-extent,-extent},{extent,-extent},{0,extent}}};
    for (int k=0;k<3;++k) { vertices[k]=vertex(p,c,huge[k]);vertices[k].u=vertices[k].v=.5; }
    Rasterizer renderer(p,c);renderer.triangle(vertices,material,0,false);
    auto traced=renderer.resolve();renderer.trace_pixel(traced);check_pixel_process(*traced.trace);
    for (int body : traced.body_index) assert(body==0);
    for (auto& s : traced.trace->samples) assert(std::isfinite(s.depth) && std::abs(s.depth-1)<1e-14);
    c.trace_process=false;
    Rasterizer plain(p,c);plain.triangle(vertices,material,0,false);
    const auto untraced=plain.resolve();
    assert(traced.image.rgb==untraced.image.rgb && traced.body_index==untraced.body_index &&
           traced.shade_evaluations==untraced.shade_evaluations);
    std::cout << "Perspective interpolation, vertex attributes, huge viewport-crossing geometry and trace invariance passed\n";
}

int main() {
    test_rectangles();test_edges_and_numerics();test_random_masks();
    test_renderer_meshes();test_clipped_mesh();
    test_interpolation_and_large_render();
    test_cloud_precision_regressions();
    std::cout << "Shared-edge top-left checks passed\n";
}

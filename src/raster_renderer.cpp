#include "planet_sandbox/renderer.h"
#include "planet_sandbox/sculpt.h"
#include "planet_sandbox/texture.h"
#include "planet_sandbox/planet_visual.h"
#include "triangle_coverage.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace planet_sandbox {
namespace {

constexpr double pi = 3.14159265358979323846;
constexpr double half_fov_tangent = 0.41421356237309503;
constexpr double near_plane = 0.05;

int sampling_count(SamplingMode mode) {
    return mode == SamplingMode::Single ? 1 : mode == SamplingMode::SSAA16 ? 16 : 4;
}

Vec2 sample_offset(int sample_count, int sample) {
    return raster_detail::sample_offset(sample_count, sample);
}

TraceSurface geometry_surface(TraceSurface surface) {
    if (surface == TraceSurface::Atmosphere) return TraceSurface::Background;
    return surface == TraceSurface::Cloud ? TraceSurface::Body : surface;
}

using Vec3 = VisualVector;
struct Color { double r, g, b; };
struct Vertex { Vec3 position, normal; double u, v; };
struct ScreenVertex { double x, y, depth, u, v; Vec3 normal, position; };
struct Sample {
    Color color;
    double depth = std::numeric_limits<double>::infinity();
    int body = -1;
    Vec2 surface_uv{-1.0, -1.0};
    TraceSurface surface = TraceSurface::Background;
};
struct CloudBase {
    float r = 0.0f, g = 0.0f, b = 0.0f;
    std::uint32_t layer = 0;
};

Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, double scale) { return {a.x * scale, a.y * scale, a.z * scale}; }
Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 normalize(Vec3 value) {
    const double length = std::sqrt(dot(value, value));
    return length > 0.0 ? value * (1.0 / length) : Vec3{0.0, 0.0, 1.0};
}
Color mix(Color a, Color b, double fraction) {
    return {a.r + (b.r - a.r) * fraction, a.g + (b.g - a.g) * fraction,
            a.b + (b.b - a.b) * fraction};
}
Color scale(Color a, double amount) { return {a.r * amount, a.g * amount, a.b * amount}; }

Color warm_tint(BodyKind kind) {
    switch (kind) {
    case BodyKind::Sun: return {0.92, 0.35, 0.04};
    case BodyKind::Mercury: return {0.48, 0.38, 0.30};
    case BodyKind::Venus: return {0.78, 0.55, 0.24};
    case BodyKind::Earth: return {0.20, 0.62, 0.44};
    case BodyKind::Mars: return {0.78, 0.31, 0.12};
    case BodyKind::Jupiter: return {0.66, 0.37, 0.17};
    case BodyKind::Saturn: return {0.72, 0.55, 0.30};
    case BodyKind::Uranus: return {0.18, 0.67, 0.62};
    case BodyKind::Neptune: return {0.10, 0.34, 0.82};
    default: return {0.5, 0.5, 0.5};
    }
}

int hex_digit(char digit) {
    if (digit >= '0' && digit <= '9') return digit - '0';
    if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
    return digit - 'A' + 10;
}

Color parse_color(const std::string& hex) {
    const auto channel = [&](int offset) {
        const double srgb = (hex_digit(hex[offset]) * 16 + hex_digit(hex[offset + 1])) / 255.0;
        return std::pow(srgb, 2.2);
    };
    return {channel(1), channel(3), channel(5)};
}

std::uint8_t to_byte(double linear) {
    return static_cast<std::uint8_t>(std::round(std::pow(std::clamp(linear, 0.0, 1.0), 1.0 / 2.2) * 255.0));
}

double smoothstep(double low, double high, double value) {
    const double t = std::clamp((value - low) / (high - low), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

double hash(int x, int y, std::uint32_t seed) {
    std::uint32_t value = static_cast<std::uint32_t>(x) * 374761393u +
                          static_cast<std::uint32_t>(y) * 668265263u + seed * 2246822519u;
    value = (value ^ (value >> 13)) * 1274126177u;
    return static_cast<double>(value ^ (value >> 16)) / 4294967295.0;
}

double noise(double x, double y, std::uint32_t seed) {
    const int ix = static_cast<int>(std::floor(x));
    const int iy = static_cast<int>(std::floor(y));
    const double tx = smoothstep(0.0, 1.0, x - ix);
    const double ty = smoothstep(0.0, 1.0, y - iy);
    const double a = hash(ix, iy, seed) * (1.0 - tx) + hash(ix + 1, iy, seed) * tx;
    const double b = hash(ix, iy + 1, seed) * (1.0 - tx) + hash(ix + 1, iy + 1, seed) * tx;
    return a * (1.0 - ty) + b * ty;
}

struct Material {
    const Appearance& appearance;
    Color primary;
    Color secondary;
    bool emissive;
    BodyKind kind = BodyKind::Custom;
    const DesignOverrides* design = nullptr;
    const Texture* color_map = nullptr;
    const Texture* cloud_map = nullptr;
    const Texture* ring_map = nullptr;
    bool cloud_pass = false;
    bool venus_surface_pass = false;
    bool venus_cloud_pass = false;
    double time = 0.0;
    Vec3 center{}, light{-0.4, 0.55, 0.75}, pole{0, 1, 0}, ring_normal{0, .89, -.46};
    double radius = 1.0, envelope = 1.0, cloud_radius = 1.018;
    const VisualDetailMap* detail = nullptr;
};

Vec3 ring_normal_for(BodyKind kind, const DesignOverrides& design) {
    if (kind == BodyKind::Uranus) {
        const double tilt = (98.0 + design.axis_tilt_offset) * pi / 180.0;
        return {-std::sin(tilt) * std::cos(0.35), std::cos(tilt),
                 std::sin(tilt) * std::sin(0.35)};
    }
    const double tilt = std::atan2(0.46, 0.89) - design.ring_tilt_offset * pi / 180.0;
    return {0.0, std::cos(tilt), -std::sin(tilt)};
}

double shape_envelope(BodyKind kind, const DesignOverrides& d) {
    double sculpt = 0.0;
    for (float value : d.sculpt_visible) sculpt = std::max(sculpt, double(value));
    switch (kind) {
    case BodyKind::Sun: return 1 + .012 * d.relief + sculpt;
    case BodyKind::Mercury: return 1 + .026 * d.relief + sculpt;
    case BodyKind::Venus: return 1.055 + .006 * d.relief + sculpt;
    case BodyKind::Earth: return 1 + .012 * d.relief + sculpt;
    case BodyKind::Mars: return 1 + .0225 * d.relief + sculpt;
    case BodyKind::Jupiter: return 1 + .009 * d.relief + .006 * d.belt_relief + .008 * d.storm_relief + sculpt;
    case BodyKind::Saturn: return 1 + .013 * d.relief + sculpt;
    case BodyKind::Uranus: return 1 + .011 * d.relief + sculpt;
    case BodyKind::Neptune: return 1 + .015 * d.relief + .008 * d.storm_relief + sculpt;
    default: return 1;
    }
}

double ring_opacity(const Material& m, double radial) {
    if (radial < 0.0 || radial > 1.0) return 0.0;
    const double gap = smoothstep(m.design->cassini_gap * .008,
                                  m.design->cassini_gap * .013 + .001, std::abs(radial - .68));
    return sample_texture(*m.ring_map, radial, 0.5, false)[3] * gap *
           std::clamp(m.design->ring_width, 0.0, 1.0);
}

double cloud_mask(const Material& m, double u, double v) {
    const auto cloud = sample_texture(*m.cloud_map, u + m.design->cloud_motion * m.time * .006, v);
    return (cloud[0] + cloud[1] + cloud[2]) / 3.0;
}

double cloud_transmission(const Material& m, Vec3 world) {
    if (!m.cloud_map || m.design->cloud_opacity == 0 || m.design->cloud_shadow == 0) return 1.0;
    const Vec3 p = (world - m.center) * (1.0 / m.radius);
    const double b = dot(p, m.light);
    const double t = -b + std::sqrt(std::max(0.0, b * b + m.cloud_radius * m.cloud_radius - dot(p, p)));
    const Vec3 hit = normalize(p + m.light * t);
    const double spin = m.time * .45;
    const double u = std::atan2(hit.x * std::sin(spin) + hit.z * std::cos(spin),
                               hit.x * std::cos(spin) - hit.z * std::sin(spin)) / (2 * pi);
    const double v = std::acos(std::clamp(hit.y, -1.0, 1.0)) / pi;
    const double mask = (cloud_mask(m, u, v) * 2 + cloud_mask(m, u + .0015, v) +
                         cloud_mask(m, u - .0015, v)) * .25;
    return 1.0 - m.design->cloud_shadow * m.design->cloud_opacity * mask * .82;
}

Vec3 detailed_normal(const Material& m, double u, double v, Vec2 source, Vec3 normal, Vec3 world) {
    if (!m.detail || m.design->surface_detail == 0) return normal;
    Vec2 slope = sample_visual_detail(*m.detail, source.x, source.y);
    const auto& d = *m.design;
    if (m.kind == BodyKind::Jupiter || m.kind == BodyKind::Saturn ||
        m.kind == BodyKind::Uranus || m.kind == BodyKind::Neptune) {
        constexpr double h = 0.0002;
        const auto a = solar_visual_uv(m.kind, u - h, v, d), b = solar_visual_uv(m.kind, u + h, v, d);
        const auto c = solar_visual_uv(m.kind, u, v - h, d), e = solar_visual_uv(m.kind, u, v + h, d);
        slope = {(slope.x * std::remainder(b.x - a.x, 1.0) + slope.y * (b.y - a.y)) / (2 * h),
                 (slope.x * std::remainder(e.x - c.x, 1.0) + slope.y * (e.y - c.y)) / (2 * h)};
    }
    const Vec3 radial = normalize(world - m.center);
    const double latitude = std::sin(pi * v);
    const double fade = smoothstep(0.0, 0.15, latitude);
    double amplitude = m.kind == BodyKind::Earth ? .012 : m.kind == BodyKind::Mars ? .045 :
                       m.kind == BodyKind::Mercury ? .035 : .012;
    amplitude *= d.surface_detail * (m.venus_surface_pass ? d.radar_relief : d.relief) * fade;
    const double du = std::clamp(slope.x * amplitude / (2 * pi * std::max(.12, latitude)), -.65, .65);
    const double dv = std::clamp(slope.y * amplitude / pi, -.65, .65);
    return perturb_visual_normal(normal, radial, m.pole, du, dv);
}

Color surface_color(const Material& material, double u, double v, Vec3 normal, Vec3 world, Vec3 view) {
    const auto& a = material.appearance;
    if (material.color_map) {
        const auto source = solar_visual_uv(material.kind, u, v, *material.design);
        const auto texel = sample_texture(*material.color_map, source.x, source.y);
        Color color{std::pow(texel[0], 2.2), std::pow(texel[1], 2.2),
                    std::pow(texel[2], 2.2)};
        const DesignOverrides& design = *material.design;
        if (material.kind == BodyKind::Sun) {
            const double luma = (color.r + color.g + color.b) / 3.0;
            color = mix({0.65, 0.30, 0.055}, color, design.granulation);
            const double grains = noise(u * 150.0 * design.granule_scale,
                                        v * 75.0 * design.granule_scale, 19u);
            color = scale(color, 0.93 + 0.14 * grains);
            const double spot = 1.0 - smoothstep(0.10, 0.38, luma);
            color = scale(color, 1.0 - spot * (design.sunspot_strength - 1.0) * 0.25);
        }
        if (material.kind == BodyKind::Mercury) {
            color = mix({0.31, 0.30, 0.29}, color, design.band_contrast);
        }
        if (material.kind == BodyKind::Jupiter || material.kind == BodyKind::Saturn ||
            material.kind == BodyKind::Uranus || material.kind == BodyKind::Neptune) {
            color = mix({0.34, 0.32, 0.30}, color, design.band_contrast);
        }
        if (material.kind == BodyKind::Jupiter) {
            const double spot = solar_storm_mask(material.kind, source.x, source.y);
            const Color target = design.storm_scale < 1.0 ? Color{0.42, 0.35, 0.27} :
                                                            Color{0.67, 0.26, 0.095};
            color = mix(color, target,
                        std::clamp(std::abs(design.storm_scale - 1.0) * spot * 0.50, 0.0, 0.8));
        }
        if (material.kind == BodyKind::Neptune) {
            const double storm = solar_storm_mask(material.kind, source.x, source.y);
            const Color target = design.storm_scale < 1.0 ? Color{0.09, 0.16, 0.45} :
                                                            Color{0.025, 0.035, 0.16};
            color = mix(color, target,
                        std::clamp(std::abs(design.storm_scale - 1.0) * storm * 0.55, 0.0, 0.8));
        }
        if (design.color_shift > 0.0) {
            color = mix(color, warm_tint(material.kind), design.color_shift * 0.42);
        } else if (design.color_shift < 0.0) {
            color = mix(color, {0.18, 0.36, 0.72}, -design.color_shift * 0.42);
        }
        if (material.kind == BodyKind::Mars) {
            color = mix(color, {0.68, 0.30, 0.13}, design.dust_opacity * 0.78);
        }
        if (material.kind == BodyKind::Earth && design.ice_extent > 0.0) {
            const double polar = std::abs(0.5 - v) * 2.0;
            color = mix(color, {0.82, 0.88, 0.91},
                        design.ice_extent * smoothstep(0.72, 0.96, polar));
        }
        if (material.emissive) return scale(color, .94 + .06 * std::max(0.0, dot(normal, material.light)));
        normal = detailed_normal(material, u, v, source, normal, world);
        const double diffuse = std::max(0.0, dot(normal, material.light));
        double transmission = cloud_transmission(material, world);
        if (material.kind == BodyKind::Saturn && material.ring_map) {
            const double distance = ring_plane_radius((world - material.center) * (1.0 / material.radius),
                                                       material.light, material.ring_normal);
            if (distance >= 1.18 && distance <= 2.43)
                transmission *= 1.0 - .90 * ring_opacity(material, (distance - 1.18) / 1.25);
        }
        Color lit = scale(color, .025 + .975 * diffuse * transmission);
        if (material.kind == BodyKind::Earth) {
            const double ocean = smoothstep(texel[0] * 1.25, texel[0] * 1.25 + .12, texel[2]) *
                                 (1 - smoothstep(.18, .42, texel[1]));
            const Vec3 half = normalize(view + material.light);
            const double fresnel = .025 + .32 * std::pow(1 - std::max(0.0, dot(normal, view)), 5);
            const double specular = std::pow(std::max(0.0, dot(normal, half)), 100) * ocean * fresnel * 3 * transmission;
            lit.r += specular * .92; lit.g += specular * .96; lit.b += specular;
        }
        return lit;
    }
    const double latitude = 0.5 - v;
    const double terrain = noise(u * 12.0 + 20.0, v * 12.0 + 20.0, a.seed);
    const double detail = noise(u * 27.0 + 50.0, v * 27.0 + 50.0, a.seed + 17u);
    const double warp = (terrain - 0.5) * a.terrain * 2.3;
    const double bands = 0.5 + 0.5 * std::sin((latitude + warp) * a.bands * 2.0 * pi);
    Color color = mix(material.primary, material.secondary, smoothstep(0.25, 0.75, bands));
    color = mix(color, {0.72, 0.82, 0.8}, a.clouds * smoothstep(0.63, 0.84, detail));
    const Vec3 light = normalize({-0.4, 0.55, 0.75});
    const double diffuse = std::max(0.0, dot(normal, light));
    return scale(color, material.emissive ? 0.92 + 0.08 * diffuse : 0.22 + 0.78 * diffuse);
}

enum class FragmentBlend { Opaque, Alpha, Cloud };

struct Fragment {
    Color color{};
    double alpha = 1.0;
    FragmentBlend blend = FragmentBlend::Opaque;
};

Fragment shade_fragment(const Material& material, double u, double v, Vec3 normal, Vec3 world, Vec3 view, bool ring) {
    if (material.venus_cloud_pass) {
        Color color = surface_color(material, u, v, normal, world, view);
        const double rim = std::pow(1 - std::max(0.0, dot(normal, view)), 3) *
                           smoothstep(-.12, .25, dot(normal, material.light)) *
                           material.design->atmosphere_density * .18;
        // Venus's visible deck is its atmosphere; composite its opacity only once.
        color = mix(color, {.48, .35, .15}, std::clamp(rim, 0.0, .36));
        return {color, material.design->cloud_opacity, FragmentBlend::Cloud};
    }
    if (material.cloud_pass) {
        const double mask = cloud_mask(material, u, v);
        const double du = cloud_mask(material, u + .0015, v) - cloud_mask(material, u - .0015, v);
        const double dv = cloud_mask(material, u, v + .003) - cloud_mask(material, u, v - .003);
        const Vec3 cloud_normal = perturb_visual_normal(normal, normal, material.pole, du * .7, dv * .7);
        const double diffuse = std::max(0.0, dot(cloud_normal, material.light));
        const double alpha = 1 - std::exp(-material.design->cloud_opacity * mask * 2.0 /
                                           std::max(.35, dot(normal, view)));
        const double brightness = .025 + .975 * diffuse;
        return {scale({.93, .96, 1.0}, brightness), alpha < .005 ? 0.0 : alpha, FragmentBlend::Cloud};
    }
    if (ring && material.ring_map) {
        const auto texel = sample_texture(*material.ring_map, u, v, false);
        const double alpha = ring_opacity(material, u);
        const Vec3 relative = (world - material.center) * (1.0 / material.radius);
        const double visibility = planet_blocks_light(relative, material.light, 1.0) ? .025 : 1.0;
        const double light = .06 + .94 * std::sqrt(std::abs(dot(normal, material.light))) * visibility;
        return {scale({std::pow(texel[0], 2.2), std::pow(texel[1], 2.2),
                 std::pow(texel[2], 2.2)}, light), alpha < 0.03 ? 0.0 : alpha,
                FragmentBlend::Alpha};
    }
    if (ring && material.kind == BodyKind::Uranus) {
        const double alpha = material.design->ring_width *
                             (0.14 + 0.06 * std::sin(u * 8.0 * pi));
        return {{0.14, 0.32, 0.36}, alpha < 0.01 ? 0.0 : alpha, FragmentBlend::Alpha};
    }
    const double stripe = 0.5 + 0.5 * std::sin(u * 26.0 * pi);
    return {ring ? scale(mix(material.secondary, material.primary, stripe), 0.72)
                 : surface_color(material, u, v, normal, world, view), 1.0, FragmentBlend::Opaque};
}

struct InterpolatedPoint {
    double depth=0,u=0,v=0;
    Vec3 normal{},world{};
    bool valid=false;
};

// The receiving sample already has this exact perspective depth from its
// depth test. Reuse it without reassociating the attribute arithmetic.
InterpolatedPoint interpolate_point_at_depth(const std::array<ScreenVertex,3>& p,
                                             const std::array<double,3>& w,double depth) {
    InterpolatedPoint out;
    out.depth=depth;
    const auto interpolate=[&](double a,double b,double c) {
        return (w[0]*a/p[0].depth+w[1]*b/p[1].depth+w[2]*c/p[2].depth)*out.depth;
    };
    out.u=interpolate(p[0].u,p[1].u,p[2].u);out.v=interpolate(p[0].v,p[1].v,p[2].v);
    const Vec3 normal{interpolate(p[0].normal.x,p[1].normal.x,p[2].normal.x),
                      interpolate(p[0].normal.y,p[1].normal.y,p[2].normal.y),
                      interpolate(p[0].normal.z,p[1].normal.z,p[2].normal.z)};
    out.world={interpolate(p[0].position.x,p[1].position.x,p[2].position.x),
               interpolate(p[0].position.y,p[1].position.y,p[2].position.y),
               interpolate(p[0].position.z,p[1].position.z,p[2].position.z)};
    const auto finite=[](Vec3 v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);};
    out.valid=std::isfinite(out.depth)&&std::isfinite(out.u)&&std::isfinite(out.v)&&
              finite(normal)&&dot(normal,normal)>0&&std::isfinite(dot(normal,normal))&&finite(out.world);
    out.normal=normalize(normal);
    return out;
}

InterpolatedPoint interpolate_point(const std::array<ScreenVertex,3>& p,const std::array<double,3>& w) {
    const double inverse=w[0]/p[0].depth+w[1]/p[1].depth+w[2]/p[2].depth;
    if(!std::isfinite(inverse) || inverse<=0) return {};
    return interpolate_point_at_depth(p,w,1/inverse);
}

struct MsaaPoint {
    Vec2 position{};
    InterpolatedPoint attributes;
    TraceShadingLocation location=TraceShadingLocation::PixelCenter;
    TracePointIssue center_issue=TracePointIssue::None,centroid_issue=TracePointIssue::None;
};
using MsaaWeights=std::array<std::array<double,3>,4>;

// This selector deliberately has no depth buffer or depth-passed mask input.
// Averaging affine SCREEN weights evaluates the geometric mean point without
// snapping a rational centroid back to the integer coverage grid.
std::pair<Vec2,std::array<double,3>> coverage_centroid(unsigned mask,const MsaaWeights& weights) {
    Vec2 position{};std::array<double,3> w{};int count=0;
    for(int s=0;s<4;++s) if(mask&(1u<<s)) {
        const auto offset=sample_offset(4,s);position.x+=offset.x;position.y+=offset.y;
        for(int k=0;k<3;++k) w[k]+=weights[s][k];
        ++count;
    }
    if(count) {position.x/=count;position.y/=count;for(auto& value:w) value/=count;}
    return {position,w};
}

bool choose_msaa_point(const raster_detail::TriangleCoverage& coverage,const std::array<ScreenVertex,3>& p,
                       Vec3 camera,bool cloud,int x,int y,unsigned mask,const MsaaWeights& weights,MsaaPoint& out) {
    if(!mask) return false;
    const auto evaluate=[&](const std::array<double,3>& w,InterpolatedPoint& attributes) {
        for(double v:w) if(!std::isfinite(v)||v<0) return TracePointIssue::InvalidInterpolation;
        attributes=interpolate_point(p,w);
        if(!attributes.valid) return TracePointIssue::InvalidInterpolation;
        if(cloud && dot(attributes.normal,camera-attributes.world)<=0) return TracePointIssue::BackFacing;
        return TracePointIssue::None;
    };
    constexpr auto scale=raster_detail::subpixel_scale;
    std::array<double,3> center{};
    out.position={.5,.5};out.location=TraceShadingLocation::PixelCenter;
    // The four covered sample positions surround the center. Convexity proves
    // center ownership, and their affine weights average to center weights.
    // Avoid an extra arbitrary-precision edge query for large full-coverage faces.
    bool center_covered=mask==15;
    if(center_covered) {
        for(int s=0;s<4;++s) for(int k=0;k<3;++k) center[k]+=weights[s][k]*.25;
    } else center_covered=coverage.weights(std::int64_t(x)*scale+scale/2,std::int64_t(y)*scale+scale/2,center);
    out.center_issue=center_covered
        ? evaluate(center,out.attributes) : TracePointIssue::OutsideCoverage;
    if(out.center_issue==TracePointIssue::None) return true;
    const auto centroid=coverage_centroid(mask,weights);
    out.position=centroid.first;out.location=TraceShadingLocation::CoverageCentroid;
    out.centroid_issue=evaluate(centroid.second,out.attributes);
    if(out.centroid_issue==TracePointIssue::None) return true;
    // Distances tie for the current 2x2 layout; canonical sample order breaks
    // ties. Keep the explicit distance comparison if the layout later changes.
    double closest=std::numeric_limits<double>::infinity();bool found=false;
    for(int s=0;s<4;++s) if(mask&(1u<<s)) {
        InterpolatedPoint candidate;
        if(evaluate(weights[s],candidate)!=TracePointIssue::None) continue;
        const auto position=sample_offset(4,s);
        const double distance=(position.x-.5)*(position.x-.5)+(position.y-.5)*(position.y-.5);
        if(distance<closest) {
            closest=distance;found=true;out.position=position;out.attributes=candidate;
            out.location=TraceShadingLocation::CoveredSample;
        }
    }
    return found;
}

class Rasterizer {
public:
    Rasterizer(const Project& project, const RenderConfig& config)
        : config_(config), sample_count_(sampling_count(config.sampling)),
          samples_(static_cast<std::size_t>(config.width) * config.height * sample_count_) {
        const double yaw = config.camera.yaw, pitch = config.camera.pitch;
        view_direction_ = {std::sin(yaw) * std::cos(pitch), std::sin(pitch),
                           std::cos(yaw) * std::cos(pitch)};
        right_ = {std::cos(yaw), 0.0, -std::sin(yaw)};
        up_ = {-std::sin(yaw) * std::sin(pitch), std::cos(pitch),
               -std::cos(yaw) * std::sin(pitch)};
        distance_ = project.view_extent * 1.35 / half_fov_tangent * config.camera.zoom;
        camera_position_ = Vec3{config.camera.target.x, config.camera.target.y, 0.0} +
                           view_direction_ * distance_;
        image_scale_ = config.height / (2.0 * half_fov_tangent);
        for (int y = 0; y < config.height; ++y) {
            for (int x = 0; x < config.width; ++x) {
                const double vx = (x + 0.5 - config.width * 0.5) / config.width;
                const double vy = (y + 0.5 - config.height * 0.5) / config.height;
                const double vignette = 1.0 - 0.38 * std::min(1.0, vx * vx + vy * vy);
                Color background = scale({0.0036, 0.0054, 0.008}, vignette);
                if (hash(x, y, 97u) > 0.99988) background = {0.26, 0.29, 0.34};
                const std::size_t pixel = static_cast<std::size_t>(y) * config.width + x;
                for (int sample = 0; sample < sample_count_; ++sample) {
                    samples_[pixel * sample_count_ + sample].color = background;
                }
            }
        }
        if (config_.trace_process && config_.trace_x >= 0) {
            const auto pixel = static_cast<std::size_t>(config_.trace_y) * config_.width + config_.trace_x;
            for (int s = 0; s < sample_count_; ++s)
                initial_samples_.push_back(snapshot_sample(samples_[pixel * sample_count_ + s], s));
        }
    }

    double camera_depth(Vec3 world) const {
        const Vec3 relative = world - Vec3{config_.camera.target.x, config_.camera.target.y, 0.0};
        return distance_ - dot(relative, view_direction_);
    }

    bool project(Vec3 world, ScreenVertex& screen) const {
        const Vec3 relative = world - Vec3{config_.camera.target.x, config_.camera.target.y, 0.0};
        const double depth = camera_depth(world);
        if (depth < near_plane - 1e-9 || !std::isfinite(depth)) return false;
        const double projected_depth = std::max(depth, near_plane);
        screen.x = config_.width * 0.5 + dot(relative, right_) * image_scale_ / projected_depth;
        screen.y = config_.height * 0.5 - dot(relative, up_) * image_scale_ / projected_depth;
        screen.depth = projected_depth;
        return std::isfinite(screen.x) && std::isfinite(screen.y);
    }

    void begin_cloud_layer() {
        if (cloud_bases_.empty()) cloud_bases_.resize(samples_.size());
        ++cloud_layer_;
    }

    void triangle(const std::array<Vertex, 3>& vertices, const Material& material,
                  int body_index, bool ring) {
        const auto triangle_id = next_triangle_id_++;
        std::array<double, 3> depths{};
        int inside_count = 0;
        for (int i = 0; i < 3; ++i) {
            depths[i] = camera_depth(vertices[i].position);
            if (!std::isfinite(depths[i])) return;
            inside_count += depths[i] >= near_plane;
        }
        if (inside_count == 0) return;
        if (inside_count == 3) {
            rasterize_triangle(vertices, material, body_index, ring, triangle_id, 0);
            return;
        }
        std::array<Vertex, 4> clipped{};
        int count = 0;
        for (int i = 0; i < 3; ++i) {
            const int previous = (i + 2) % 3;
            const bool current_inside = depths[i] >= near_plane;
            const bool previous_inside = depths[previous] >= near_plane;
            if (current_inside != previous_inside) {
                // Always interpolate outside -> inside. Reversed shared edges
                // now produce bit-identical positions before screen snapping.
                const int outside = current_inside ? previous : i;
                const int inside = current_inside ? i : previous;
                const double t = std::clamp((near_plane - depths[outside]) /
                                                (depths[inside] - depths[outside]), 0.0, 1.0);
                const Vertex& a = vertices[outside];
                const Vertex& b = vertices[inside];
                clipped[count++] = {a.position + (b.position - a.position) * t,
                                    a.normal + (b.normal - a.normal) * t,
                                    a.u + (b.u - a.u) * t,
                                    a.v + (b.v - a.v) * t};
            }
            if (current_inside) clipped[count++] = vertices[i];
        }
        for (int i = 1; i + 1 < count; ++i) {
            rasterize_triangle({clipped[0], clipped[i], clipped[i + 1]},
                               material, body_index, ring, triangle_id, i - 1);
        }
    }

private:
    static std::array<double, 3> rgb(Color c) { return {c.r, c.g, c.b}; }

    TraceSample snapshot_sample(const Sample& sample, int s) const {
        return {sample_offset(sample_count_, s), sample.body, sample.depth,
                rgb(sample.color), sample.surface};
    }

    // Called only after geometric coverage succeeds. Allocate nothing for other
    // pixels or for timing renders. Each event snapshots all samples before its
    // first write so uncovered/depth-rejected samples remain explainable too.
    TraceSampleStep* record_coverage(TraceEvent*& event, int x, int y, int s,
                                    TracePrimitive primitive, std::uint64_t id,
                                    int body, TraceSurface surface, int part = 0) {
        if (!config_.trace_process || x != config_.trace_x || y != config_.trace_y) return nullptr;
        if (!event) {
            if (events_.size() >= PixelTrace::max_events) {
                process_truncated_ = true;
                return nullptr;
            }
            events_.emplace_back();
            event = &events_.back();
            event->primitive = primitive;
            event->primitive_id = id;
            event->clipped_part = part;
            event->body_index = body;
            event->surface = surface;
            event->samples.resize(sample_count_);
            const auto pixel = static_cast<std::size_t>(y) * config_.width + x;
            for (int n = 0; n < sample_count_; ++n) {
                auto& step = event->samples[n];
                step.before = step.after = snapshot_sample(samples_[pixel * sample_count_ + n], n);
                step.blend_base_rgb = step.before.linear_rgb;
            }
        }
        auto& step = event->samples[s];
        step.covered = true;
        return &step;
    }

    static void record_depth(TraceSampleStep* step, double depth, double previous) {
        if (!step) return;
        step->candidate_depth = depth;
        step->depth_test = depth >= previous ? TraceDepthTest::Rejected : TraceDepthTest::Passed;
        if (step->depth_test == TraceDepthTest::Rejected) step->action = TraceAction::DepthRejected;
    }

    void rasterize_msaa_pixel(const raster_detail::TriangleCoverage& coverage,const std::array<ScreenVertex,3>& p,
                              const Material& material,int body_index,bool ring,std::uint64_t triangle_id,
                              int part,int x,int y) {
        // Only covered entries are read. Defer the larger attribute storage
        // until at least one sample passes depth; most bounding-box pixels do not.
        MsaaWeights weights;
        std::array<double,4> depths;
        unsigned covered=0,depth_passed=0,receivers=0;
        TraceEvent* event=nullptr;
        const bool cloud=material.cloud_pass||material.venus_cloud_pass;
        const auto surface=ring?TraceSurface::Ring:cloud?TraceSurface::Cloud:TraceSurface::Body;
        const auto pixel=(static_cast<std::size_t>(y)*config_.width+x)*4;
        constexpr auto scale=raster_detail::subpixel_scale;
        for(int s=0;s<4;++s) {
            const auto offset=sample_offset(4,s);
            if(!coverage.weights(std::int64_t(x)*scale+std::int64_t(offset.x*scale),
                                 std::int64_t(y)*scale+std::int64_t(offset.y*scale),weights[s])) continue;
            covered|=1u<<s;
            auto* step=record_coverage(event,x,y,s,TracePrimitive::Triangle,triangle_id,body_index,surface,part);
            const auto& w=weights[s];
            const double depth=1/(w[0]/p[0].depth+w[1]/p[1].depth+w[2]/p[2].depth);
            if(!std::isfinite(depth)||depth<=0) {if(step) step->action=TraceAction::InvalidAttributes;continue;}
            depths[s]=depth;
            record_depth(step,depth,samples_[pixel+s].depth);
            if(depth>=samples_[pixel+s].depth) continue;
            depth_passed|=1u<<s;
        }
        if(!depth_passed) return;
        MsaaPoint point;
        const bool has_point=choose_msaa_point(coverage,p,camera_position_,cloud,x,y,covered,weights,point);
        std::array<InterpolatedPoint,4> attributes;
        for(int s=0;s<4;++s) if(depth_passed&(1u<<s)) {
            auto* step=event?&event->samples[s]:nullptr;
            auto& at=attributes[s];const auto offset=sample_offset(4,s);
            at.depth=depths[s];
            if(has_point && offset.x==point.position.x && offset.y==point.position.y) at=point.attributes;
            else if(cloud) at=interpolate_point_at_depth(p,weights[s],depths[s]);
            else {
                // Non-cloud sample writes need their own depth/UV, but normals
                // and world positions are needed only at the shared shader point.
                const auto& w=weights[s];
                at.u=(w[0]*p[0].u/p[0].depth+w[1]*p[1].u/p[1].depth+w[2]*p[2].u/p[2].depth)*at.depth;
                at.v=(w[0]*p[0].v/p[0].depth+w[1]*p[1].v/p[1].depth+w[2]*p[2].v/p[2].depth)*at.depth;
                at.valid=std::isfinite(at.u)&&std::isfinite(at.v);
            }
            if(!at.valid) {if(step) step->action=TraceAction::InvalidAttributes;continue;}
            if(cloud && dot(at.normal,camera_position_-at.world)<=0) {
                if(step) step->action=TraceAction::BackfaceRejected;
                continue;
            }
            receivers|=1u<<s;
        }
        if(!receivers) return;
        if(!has_point) {
            if(event) {
                event->shading_failure=TracePointIssue::NoValidCoveredPoint;
                for(int s=0;s<4;++s) if(receivers&(1u<<s)) event->samples[s].action=TraceAction::InvalidAttributes;
            }
            return;
        }
        const auto& at=point.attributes;
        const auto fragment=shade_fragment(material,at.u,at.v,at.normal,at.world,
                                           normalize(camera_position_-at.world),ring);
        ++shade_evaluations_;
        if(event) event->shading.push_back({point.position,rgb(fragment.color),fragment.alpha,
                                           point.location,point.center_issue,point.centroid_issue});
        for(int s=0;s<4;++s) if(receivers&(1u<<s)) {
            auto& sample=samples_[pixel+s];auto* step=event?&event->samples[s]:nullptr;
            if(step) {
                step->shading_index=0;step->source_rgb=rgb(fragment.color);step->alpha=fragment.alpha;
                step->blend=fragment.blend==FragmentBlend::Cloud?TraceBlend::CloudBase:
                    fragment.blend==FragmentBlend::Alpha?TraceBlend::Alpha:TraceBlend::Replace;
            }
            if(fragment.alpha<=0) {if(step) step->action=TraceAction::Transparent;continue;}
            if(fragment.blend==FragmentBlend::Cloud) {
                blend_cloud(pixel+s,sample,fragment.color,fragment.alpha);
                if(step) {const auto& base=cloud_bases_[pixel+s];step->blend_base_rgb={base.r,base.g,base.b};}
            } else if(fragment.blend==FragmentBlend::Alpha) sample.color=mix(sample.color,fragment.color,fragment.alpha);
            else sample.color=fragment.color;
            sample.depth=attributes[s].depth;sample.body=body_index;
            sample.surface_uv=ring?Vec2{-1,-1}:Vec2{attributes[s].u,attributes[s].v};sample.surface=surface;
            if(step) {step->action=TraceAction::Written;step->after=snapshot_sample(sample,s);}
        }
    }

    void rasterize_triangle(const std::array<Vertex, 3>& vertices, const Material& material,
                            int body_index, bool ring, std::uint64_t triangle_id, int part) {
        std::array<ScreenVertex, 3> p{};
        for (int i = 0; i < 3; ++i) {
            if (!project(vertices[i].position, p[i])) return;
            p[i].normal = vertices[i].normal;
            p[i].position = vertices[i].position;
            p[i].u = vertices[i].u;
            p[i].v = vertices[i].v;
        }
        const raster_detail::TriangleCoverage coverage({{{p[0].x,p[0].y},
                                                       {p[1].x,p[1].y}, {p[2].x,p[2].y}}});
        if (!coverage.valid()) return;
        const double left = coverage.minimum().x, right = coverage.maximum().x;
        const double top = coverage.minimum().y, bottom = coverage.maximum().y;
        if (right < 0.0 || bottom < 0.0 || left >= config_.width || top >= config_.height) return;
        const int min_x = static_cast<int>(std::floor(std::clamp(left, 0.0, double(config_.width - 1))));
        const int max_x = static_cast<int>(std::ceil(std::clamp(right, 0.0, double(config_.width - 1))));
        const int min_y = static_cast<int>(std::floor(std::clamp(top, 0.0, double(config_.height - 1))));
        const int max_y = static_cast<int>(std::ceil(std::clamp(bottom, 0.0, double(config_.height - 1))));
        for (int y = min_y; y <= max_y; ++y) {
            for (int x = min_x; x <= max_x; ++x) {
                if(config_.sampling==SamplingMode::MSAA4) {
                    rasterize_msaa_pixel(coverage,p,material,body_index,ring,triangle_id,part,x,y);
                    continue;
                }
                Fragment cached_fragment;
                bool cached = false;
                TraceEvent* event = nullptr;
                for (int s = 0; s < sample_count_; ++s) {
                    const Vec2 offset = sample_offset(sample_count_, s);
                    constexpr auto scale = raster_detail::subpixel_scale;
                    std::array<double, 3> weights;
                    if (!coverage.weights(std::int64_t(x)*scale + std::int64_t(offset.x*scale),
                                          std::int64_t(y)*scale + std::int64_t(offset.y*scale), weights)) continue;
                    const double w0 = weights[0], w1 = weights[1], w2 = weights[2];
                    auto* step = record_coverage(event, x, y, s, TracePrimitive::Triangle,
                        triangle_id, body_index, ring ? TraceSurface::Ring :
                        (material.cloud_pass || material.venus_cloud_pass)
                            ? TraceSurface::Cloud : TraceSurface::Body, part);
                    const double inverse_depth = w0 / p[0].depth + w1 / p[1].depth + w2 / p[2].depth;
                    const double depth = 1.0 / inverse_depth;
                    const std::size_t sample_index =
                        (static_cast<std::size_t>(y) * config_.width + x) * sample_count_ + s;
                    Sample& sample = samples_[sample_index];
                    record_depth(step, depth, sample.depth);
                    if (depth >= sample.depth) continue;
                    const auto interpolate = [&](double a, double b, double c) {
                        return (w0 * a / p[0].depth + w1 * b / p[1].depth +
                                w2 * c / p[2].depth) * depth;
                    };
                    const double u = interpolate(p[0].u, p[1].u, p[2].u);
                    const double v = interpolate(p[0].v, p[1].v, p[2].v);
                    const Vec3 normal = normalize({interpolate(p[0].normal.x, p[1].normal.x, p[2].normal.x),
                                                   interpolate(p[0].normal.y, p[1].normal.y, p[2].normal.y),
                                                   interpolate(p[0].normal.z, p[1].normal.z, p[2].normal.z)});
                    const Vec3 world{interpolate(p[0].position.x, p[1].position.x, p[2].position.x),
                                     interpolate(p[0].position.y, p[1].position.y, p[2].position.y),
                                     interpolate(p[0].position.z, p[1].position.z, p[2].position.z)};
                    if (material.cloud_pass || material.venus_cloud_pass) {
                        if (dot(normal, camera_position_ - world) <= 0.0) {
                            if (step) step->action = TraceAction::BackfaceRejected;
                            continue;
                        }
                    }
                    if (!cached || config_.sampling != SamplingMode::MSAA4) {
                        cached_fragment = shade_fragment(material, u, v, normal, world,
                                                          normalize(camera_position_ - world), ring);
                        ++shade_evaluations_;
                        cached = true;
                        if (event) event->shading.push_back({offset, rgb(cached_fragment.color),
                                                            cached_fragment.alpha});
                    }
                    if (step) {
                        step->shading_index = static_cast<int>(event->shading.size()) - 1;
                        step->source_rgb = rgb(cached_fragment.color);
                        step->alpha = cached_fragment.alpha;
                        step->blend = cached_fragment.blend == FragmentBlend::Cloud ? TraceBlend::CloudBase :
                            cached_fragment.blend == FragmentBlend::Alpha ? TraceBlend::Alpha : TraceBlend::Replace;
                    }
                    if (cached_fragment.alpha <= 0.0) {
                        if (step) step->action = TraceAction::Transparent;
                        continue;
                    }
                    if (cached_fragment.blend == FragmentBlend::Cloud) {
                        blend_cloud(sample_index, sample, cached_fragment.color, cached_fragment.alpha);
                        if (step) {
                            const auto& base = cloud_bases_[sample_index];
                            step->blend_base_rgb = {base.r, base.g, base.b};
                        }
                    } else if (cached_fragment.blend == FragmentBlend::Alpha) {
                        sample.color = mix(sample.color, cached_fragment.color, cached_fragment.alpha);
                    } else {
                        sample.color = cached_fragment.color;
                    }
                    sample.depth = depth;
                    sample.body = body_index;
                    sample.surface_uv = ring ? Vec2{-1.0, -1.0} : Vec2{u, v};
                    sample.surface = ring ? TraceSurface::Ring :
                        (material.cloud_pass || material.venus_cloud_pass)
                            ? TraceSurface::Cloud : TraceSurface::Body;
                    if (step) {
                        step->action = TraceAction::Written;
                        step->after = snapshot_sample(sample, s);
                    }
                }
            }
        }
    }

public:
    void trail(const BodyTrail& trail, Color color, int body_index,
               TraceSurface surface = TraceSurface::Trail) {
        for (std::size_t segment = 1; segment < trail.points.size(); ++segment) {
            const double plane = surface == TraceSurface::ReferenceTrail ? -0.07 : -0.05;
            Vec3 world_a{trail.points[segment - 1].x, trail.points[segment - 1].y, plane};
            Vec3 world_b{trail.points[segment].x, trail.points[segment].y, plane};
            const double depth_a = camera_depth(world_a);
            const double depth_b = camera_depth(world_b);
            if (!std::isfinite(depth_a) || !std::isfinite(depth_b) ||
                (depth_a < near_plane && depth_b < near_plane)) continue;
            if ((depth_a < near_plane) != (depth_b < near_plane)) {
                const double t = std::clamp((near_plane - depth_a) / (depth_b - depth_a),
                                            0.0, 1.0);
                const Vec3 intersection = world_a + (world_b - world_a) * t;
                if (depth_a < near_plane) world_a = intersection;
                else world_b = intersection;
            }
            ScreenVertex a{}, b{};
            if (!project(world_a, a) || !project(world_b, b)) continue;
            const double dx = b.x - a.x, dy = b.y - a.y;
            double start = 0.0, end = 1.0;
            const auto clip = [&](double p, double q) {
                if (p == 0.0) return q >= 0.0;
                const double t = q / p;
                if (p < 0.0) start = std::max(start, t);
                else end = std::min(end, t);
                return start <= end;
            };
            if (!clip(-dx, a.x + 1.0) || !clip(dx, config_.width + 1.0 - a.x) ||
                !clip(-dy, a.y + 1.0) || !clip(dy, config_.height + 1.0 - a.y)) continue;

            const double x0 = a.x + dx * start, x1 = a.x + dx * end;
            const double y0 = a.y + dy * start, y1 = a.y + dy * end;
            const int min_x = static_cast<int>(std::floor(std::clamp(std::min(x0, x1) - 1.0,
                                                                      0.0, double(config_.width - 1))));
            const int max_x = static_cast<int>(std::ceil(std::clamp(std::max(x0, x1) + 1.0,
                                                                     0.0, double(config_.width - 1))));
            const int min_y = static_cast<int>(std::floor(std::clamp(std::min(y0, y1) - 1.0,
                                                                      0.0, double(config_.height - 1))));
            const int max_y = static_cast<int>(std::ceil(std::clamp(std::max(y0, y1) + 1.0,
                                                                     0.0, double(config_.height - 1))));
            const double length_squared = dx * dx + dy * dy;
            for (int y = min_y; y <= max_y; ++y) {
                for (int x = min_x; x <= max_x; ++x) {
                    const std::size_t pixel = static_cast<std::size_t>(y) * config_.width + x;
                    TraceEvent* event = nullptr;
                    for (int s = 0; s < sample_count_; ++s) {
                        const Vec2 offset = sample_offset(sample_count_, s);
                        const double sx = x + offset.x;
                        const double sy = y + offset.y;
                        const double t = length_squared > 0.0
                            ? std::clamp(((sx - a.x) * dx + (sy - a.y) * dy) / length_squared, start, end)
                            : start;
                        const double ox = sx - (a.x + dx * t), oy = sy - (a.y + dy * t);
                        if (ox * ox + oy * oy > 0.36) continue;
                        auto* step = record_coverage(event, x, y, s, TracePrimitive::TrailSegment,
                                                     segment, body_index, surface);
                        const double depth = 1.0 / ((1.0 - t) / a.depth + t / b.depth);
                        Sample& sample = samples_[pixel * sample_count_ + s];
                        record_depth(step, depth, sample.depth);
                        if (depth >= sample.depth) continue;
                        sample.color = mix(sample.color, color,
                                           surface == TraceSurface::ReferenceTrail ? 0.5 : 0.65);
                        sample.depth = depth;
                        sample.body = body_index;
                        sample.surface_uv = {-1.0, -1.0};
                        sample.surface = surface;
                        if (step) {
                            step->source_rgb = rgb(color);
                            step->alpha = surface == TraceSurface::ReferenceTrail ? 0.5 : 0.65;
                            step->blend = TraceBlend::Alpha;
                            step->action = TraceAction::Written;
                            step->after = snapshot_sample(sample, s);
                        }
                    }
                }
            }
        }
    }

    void atmosphere(const Material& m, int body_index) {
        if (!m.design || m.design->atmosphere_density <= 0 || m.kind == BodyKind::Mercury ||
            m.kind == BodyKind::Sun || m.kind == BodyKind::Custom || m.kind == BodyKind::Venus) return;
        const double scale_height = m.kind == BodyKind::Mars ? .004 : m.kind == BodyKind::Earth ? .008 : .006;
        const double outer = m.radius * (std::max(m.envelope,
            m.kind == BodyKind::Earth ? m.cloud_radius : m.envelope) + .018);
        ScreenVertex projected{};
        const double center_depth = camera_depth(m.center);
        if (center_depth + outer < near_plane) return;
        int x0 = 0, y0 = 0, x1 = config_.width - 1, y1 = config_.height - 1;
        if (center_depth > outer + near_plane && project(m.center, projected)) {
            const double size = image_scale_ * outer / (center_depth - outer);
            if (projected.x + size < 0 || projected.x - size >= config_.width ||
                projected.y + size < 0 || projected.y - size >= config_.height) return;
            x0 = int(std::clamp(std::floor(projected.x - size), 0.0, double(x1)));
            x1 = int(std::clamp(std::ceil(projected.x + size), 0.0, double(x1)));
            y0 = int(std::clamp(std::floor(projected.y - size), 0.0, double(y1)));
            y1 = int(std::clamp(std::ceil(projected.y + size), 0.0, double(y1)));
        }
        const Vec3 offset = camera_position_ - m.center;
        const double c = dot(offset, offset) - outer * outer;
        const Color daylight = m.kind == BodyKind::Mars ? Color{.40, .19, .07} :
                               m.kind == BodyKind::Venus ? Color{.48, .35, .15} :
                               m.kind == BodyKind::Earth ? Color{.10, .32, .72} :
                               m.kind == BodyKind::Jupiter || m.kind == BodyKind::Saturn ? Color{.32, .28, .20} :
                               Color{.12, .30, .48};
        for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) {
            double cached_alpha = 0.0; Color cached_color{}; bool cached = false;
            TraceEvent* event = nullptr;
            for (int s = 0; s < sample_count_; ++s) {
                const Vec2 sub = sample_offset(sample_count_, s);
                const Vec3 ray = normalize(view_direction_ * -1.0 +
                    right_ * ((x + sub.x - config_.width * .5) / image_scale_) +
                    up_ * ((config_.height * .5 - y - sub.y) / image_scale_));
                const double b = dot(offset, ray), discriminant = b * b - c;
                if (discriminant <= 0) continue;
                auto* trace_step = record_coverage(event, x, y, s, TracePrimitive::Atmosphere,
                    static_cast<std::uint64_t>(body_index), body_index, TraceSurface::Atmosphere);
                const double root = std::sqrt(discriminant);
                const double forward = -dot(ray, view_direction_);
                const double entry = std::max(near_plane / forward, -b - root);
                Sample& sample = samples_[(static_cast<std::size_t>(y) * config_.width + x) * sample_count_ + s];
                const double exit = std::min(-b + root, sample.depth / forward);
                if (trace_step) { trace_step->ray_entry = entry; trace_step->ray_exit = exit; }
                if (exit <= entry) {
                    if (trace_step) trace_step->action = TraceAction::Occluded;
                    continue;
                }
                if (!cached || config_.sampling != SamplingMode::MSAA4) {
                    double column = 0, illumination = 0;
                    // Four short steps approximate a thin exponential scattering layer.
                    for (int step = 0; step < 4; ++step) {
                        const Vec3 p = offset + ray * (entry + (exit - entry) * (step + .5) / 4);
                        const double r = std::sqrt(dot(p, p));
                        const double altitude = std::max(0.0, r / m.radius - 1.0);
                        const double density = std::exp(-altitude / scale_height) *
                            smoothstep(0.0, m.radius * .004, outer - r);
                        column += density;
                        illumination += density * smoothstep(-.12, .25, dot(p * (1 / r), m.light));
                    }
                    const double sun = column > 0 ? illumination / column : 0;
                    const double phase = .75 * (1 + std::pow(dot(ray, m.light), 2));
                    const double depth = column * (exit - entry) / (4 * m.radius) * m.design->atmosphere_density *
                                         (m.kind == BodyKind::Venus ? m.design->cloud_opacity : 1.0);
                    cached_alpha = std::min(.32, 1 - std::exp(-depth * 1.6)) * sun;
                    cached_color = scale(daylight, .8 + .2 * phase);
                    ++shade_evaluations_; cached = true;
                    if (event) event->shading.push_back({sub, rgb(cached_color), cached_alpha});
                }
                if (trace_step) {
                    trace_step->shading_index = static_cast<int>(event->shading.size()) - 1;
                    trace_step->source_rgb = rgb(cached_color);
                    trace_step->alpha = cached_alpha;
                    trace_step->blend = TraceBlend::Alpha;
                }
                if (cached_alpha <= 0.00001) {
                    if (trace_step) trace_step->action = TraceAction::Transparent;
                    continue;
                }
                sample.color = mix(sample.color, cached_color, cached_alpha);
                // Scattering contributes color but cannot hide or select geometry.
                if (sample.surface == TraceSurface::Background) sample.surface = TraceSurface::Atmosphere;
                if (trace_step) {
                    trace_step->action = TraceAction::Written;
                    trace_step->after = snapshot_sample(sample, s);
                }
            }
        }
    }

    RenderFrame resolve() const {
        RenderFrame frame;
        frame.shade_evaluations = shade_evaluations_;
        frame.image = {config_.width, config_.height,
                       std::vector<std::uint8_t>(static_cast<std::size_t>(config_.width) * config_.height * 3)};
        frame.body_index.resize(static_cast<std::size_t>(config_.width) * config_.height, -1);
        frame.surface_uv.resize(frame.body_index.size(), {-1.0, -1.0});
        frame.sample_surface_variation.resize(frame.body_index.size(), 0);
        for (std::size_t pixel = 0; pixel < frame.body_index.size(); ++pixel) {
            Color resolved{0.0, 0.0, 0.0};
            double nearest = std::numeric_limits<double>::infinity();
            const Sample& first = samples_[pixel * sample_count_];
            for (int s = 0; s < sample_count_; ++s) {
                const Sample& sample = samples_[pixel * sample_count_ + s];
                resolved = mix(resolved, sample.color, 1.0 / (s + 1));
                if (sample.body != first.body ||
                    geometry_surface(sample.surface) != geometry_surface(first.surface)) {
                    frame.sample_surface_variation[pixel] = 1;
                }
                if (sample.depth < nearest) {
                    nearest = sample.depth;
                    frame.body_index[pixel] = sample.body;
                    frame.surface_uv[pixel] = sample.surface_uv;
                }
            }
            frame.image.rgb[pixel * 3] = to_byte(resolved.r);
            frame.image.rgb[pixel * 3 + 1] = to_byte(resolved.g);
            frame.image.rgb[pixel * 3 + 2] = to_byte(resolved.b);
        }
        return frame;
    }

    void trace_pixel(RenderFrame& frame) const {
        if (config_.trace_x >= 0 && config_.trace_y >= 0) {
            PixelTrace trace;
            trace.x = config_.trace_x;
            trace.y = config_.trace_y;
            trace.sampling = config_.sampling;
            trace.process_recorded = config_.trace_process;
            trace.process_truncated = process_truncated_;
            trace.initial_samples = initial_samples_;
            trace.events = events_;
            Color resolved{};
            const std::size_t pixel = static_cast<std::size_t>(trace.y) * config_.width + trace.x;
            for (int s = 0; s < sample_count_; ++s) {
                const Sample& sample = samples_[pixel * sample_count_ + s];
                resolved = mix(resolved, sample.color, 1.0 / (s + 1));
                trace.samples.push_back({sample_offset(sample_count_, s),
                                         sample.body, sample.depth,
                                         {sample.color.r, sample.color.g, sample.color.b},
                                         sample.surface});
            }
            trace.display_rgb = {frame.image.rgb[pixel * 3], frame.image.rgb[pixel * 3 + 1],
                                 frame.image.rgb[pixel * 3 + 2]};
            trace.resolved_linear_rgb = rgb(resolved);
            frame.trace = std::move(trace);
        }
    }

private:
    void blend_cloud(std::size_t index, Sample& sample, Color color, double alpha) {
        CloudBase& base = cloud_bases_[index];
        if (base.layer != cloud_layer_) {
            base = {static_cast<float>(sample.color.r), static_cast<float>(sample.color.g),
                    static_cast<float>(sample.color.b), cloud_layer_};
        }
        sample.color = mix({base.r, base.g, base.b}, color, alpha);
    }

    const RenderConfig& config_;
    int sample_count_;
    std::vector<Sample> samples_;
    std::vector<CloudBase> cloud_bases_;
    std::uint32_t cloud_layer_ = 0;
    std::uint64_t shade_evaluations_ = 0;
    std::uint64_t next_triangle_id_ = 0;
    std::vector<TraceSample> initial_samples_;
    std::vector<TraceEvent> events_;
    bool process_truncated_ = false;
    Vec3 view_direction_{}, right_{}, up_{};
    Vec3 camera_position_{};
    double distance_ = 0.0;
    double image_scale_ = 0.0;
};

void draw_sphere(Rasterizer& rasterizer, const BodyState& body, const Material& material,
                 int body_index, double scale_factor, double time) {
    const int latitude_steps = material.color_map ? 48 : 16;
    const int longitude_steps = material.color_map ? 96 : 32;
    const double spin = time * 0.45 +
                        (material.kind == BodyKind::Custom ? material.appearance.seed * 0.017 : 0.0);
    const double c = std::cos(spin), s = std::sin(spin);
    const double tilt = material.kind == BodyKind::Uranus
        ? (98.0 + material.design->axis_tilt_offset) * pi / 180.0 : 0.0;
    const double tilt_c = std::cos(tilt), tilt_s = std::sin(tilt);
    const double radius = body.radius * scale_factor;
    const Vec3 center{body.position.x, body.position.y, 0.0};
    std::vector<Vertex> grid((latitude_steps + 1) * (longitude_steps + 1));
    for (int row = 0; row <= latitude_steps; ++row) {
        const double theta = pi * row / latitude_steps;
        for (int col = 0; col <= longitude_steps; ++col) {
            const double phi = 2.0 * pi * col / longitude_steps;
            const Vec3 local{std::sin(theta) * std::cos(phi), std::cos(theta),
                             std::sin(theta) * std::sin(phi)};
            const Vec3 spun{local.x * c + local.z * s, local.y,
                            -local.x * s + local.z * c};
            const Vec3 tilted{spun.x * tilt_c - spun.y * tilt_s,
                              spun.x * tilt_s + spun.y * tilt_c, spun.z};
            const Vec3 normal = material.kind == BodyKind::Uranus
                ? Vec3{tilted.x * std::cos(0.35) + tilted.z * std::sin(0.35),
                       tilted.y,
                       -tilted.x * std::sin(0.35) + tilted.z * std::cos(0.35)}
                : tilted;
            double relief = 1.0;
            if (material.color_map && !material.cloud_pass) {
                relief = solar_shape_multiplier(
                    material.kind, static_cast<double>(col) / longitude_steps,
                    static_cast<double>(row) / latitude_steps, *material.design,
                    material.venus_surface_pass ? SolarShapeLayer::VenusRadar :
                                                  SolarShapeLayer::Visible);
            }
            if (material.cloud_pass) {
                relief = material.cloud_radius;
            }
            grid[row * (longitude_steps + 1) + col] =
                {center + normal * (radius * relief), normal,
                 static_cast<double>(col) / longitude_steps,
                 static_cast<double>(row) / latitude_steps};
        }
    }
    if (material.color_map && !material.cloud_pass) {
        for (int row = 1; row < latitude_steps; ++row) {
            for (int col = 0; col <= longitude_steps; ++col) {
                const int left = col == 0 ? longitude_steps - 1 : col - 1;
                const int right = col == longitude_steps ? 1 : col + 1;
                const int at = row * (longitude_steps + 1) + col;
                const Vec3 along = grid[row * (longitude_steps + 1) + right].position -
                                   grid[row * (longitude_steps + 1) + left].position;
                const Vec3 down = grid[(row + 1) * (longitude_steps + 1) + col].position -
                                  grid[(row - 1) * (longitude_steps + 1) + col].position;
                grid[at].normal = normalize(cross(along, down));
            }
        }
    }
    for (int row = 0; row < latitude_steps; ++row) {
        for (int col = 0; col < longitude_steps; ++col) {
            const int first = row * (longitude_steps + 1) + col;
            const int second = first + longitude_steps + 1;
            rasterizer.triangle({grid[first], grid[second], grid[second + 1]}, material, body_index, false);
            rasterizer.triangle({grid[first], grid[second + 1], grid[first + 1]}, material, body_index, false);
        }
    }
}

void draw_ring(Rasterizer& rasterizer, const BodyState& body, const Material& material,
               int body_index, double scale_factor) {
    const double ring_width = material.ring_map || material.kind == BodyKind::Uranus
        ? material.design->ring_width : material.appearance.ring_width;
    if (ring_width <= 0.0) return;
    constexpr int segments = 72;
    const double radius = body.radius * scale_factor;
    const double inner = radius * (material.kind == BodyKind::Uranus ? 1.32 : 1.18);
    const double outer = inner + radius * (material.kind == BodyKind::Uranus ? 0.12 :
                                           material.ring_map ? 1.25 : ring_width);
    const Vec3 center{body.position.x, body.position.y, 0.0};
    const auto vertex = [&](double angle, double distance, double radial) {
        if (material.kind == BodyKind::Uranus) {
            const double tilt = (98.0 + material.design->axis_tilt_offset) * pi / 180.0;
            const Vec3 local{std::cos(angle) * distance, 0.0,
                             std::sin(angle) * distance};
            const Vec3 tipped{local.x * std::cos(tilt), local.x * std::sin(tilt), local.z};
            return Vertex{center + Vec3{tipped.x * std::cos(0.35) + tipped.z * std::sin(0.35),
                                        tipped.y,
                                        -tipped.x * std::sin(0.35) + tipped.z * std::cos(0.35)},
                          material.ring_normal, radial, angle / (2.0 * pi)};
        }
        return Vertex{center + Vec3{std::cos(angle) * distance,
                                    std::sin(angle) * distance * -material.ring_normal.z,
                                    std::sin(angle) * distance * material.ring_normal.y},
                      material.ring_normal, radial, angle / (2.0 * pi)};
    };
    for (int segment = 0; segment < segments; ++segment) {
        const double a = 2.0 * pi * segment / segments;
        const double b = 2.0 * pi * (segment + 1) / segments;
        const Vertex ai = vertex(a, inner, 0.0), ao = vertex(a, outer, 1.0);
        const Vertex bi = vertex(b, inner, 0.0), bo = vertex(b, outer, 1.0);
        rasterizer.triangle({ai, ao, bo}, material, body_index, true);
        rasterizer.triangle({ai, bo, bi}, material, body_index, true);
    }
}

} // namespace

RenderFrame render_scene(const Project& project, const SceneSnapshot& snapshot,
                         const RenderConfig& config) {
    const auto start = std::chrono::steady_clock::now();
    validate_project(project);
    if (config.width <= 0 || config.height <= 0 || config.width > 8192 || config.height > 8192 ||
        !std::isfinite(config.camera.yaw) || !std::isfinite(config.camera.pitch) ||
        !std::isfinite(config.camera.zoom) || config.camera.zoom <= 0.0 ||
        !std::isfinite(config.camera.target.x) || !std::isfinite(config.camera.target.y) ||
        (config.sampling != SamplingMode::Single && config.sampling != SamplingMode::SSAA4 &&
         config.sampling != SamplingMode::MSAA4 && config.sampling != SamplingMode::SSAA16) ||
        ((config.trace_x != -1 || config.trace_y != -1) &&
         (config.trace_x < 0 || config.trace_x >= config.width ||
          config.trace_y < 0 || config.trace_y >= config.height))) {
        throw std::invalid_argument("Invalid render configuration");
    }
    const std::size_t sample_count = static_cast<std::size_t>(sampling_count(config.sampling));
    if (static_cast<std::size_t>(config.width) * config.height * sample_count > 8000000u ||
        !std::isfinite(project.view_extent * config.camera.zoom * 1.35 / half_fov_tangent)) {
        throw std::invalid_argument("Render size or camera distance is too large");
    }
    Rasterizer rasterizer(project, config);
    const auto sun = std::find_if(snapshot.bodies.begin(), snapshot.bodies.end(),
                                  [](const BodyState& b) { return b.id == "sun"; });
    const auto configure = [&](Material& m, const BodyState& body, double scale_factor, bool detail) {
        m.center = {body.position.x, body.position.y, 0};
        m.radius = body.radius * scale_factor;
        m.time = snapshot.time;
        m.light = project.solar_system && sun != snapshot.bodies.end() && body.id != "sun"
            ? normalize(Vec3{sun->position.x, sun->position.y, 0} - m.center)
            : normalize(Vec3{-.48, .62, .62});
        if (!m.design) return;
        m.ring_normal = ring_normal_for(m.kind, *m.design);
        if (m.kind == BodyKind::Uranus) m.pole = m.ring_normal;
        m.envelope = shape_envelope(m.kind, *m.design);
        m.cloud_radius = m.envelope + m.design->cloud_altitude;
        if (m.kind == BodyKind::Earth) m.cloud_map = &solar_texture(m.kind, AssetLayer::Clouds);
        const double projected_radius = config.height * m.radius /
            (2 * half_fov_tangent * std::max(near_plane, rasterizer.camera_depth(m.center) - m.radius));
        if (detail && projected_radius >= 20 && m.kind != BodyKind::Sun && m.design->surface_detail > 0) {
            m.detail = &solar_detail_map(m.kind, m.venus_surface_pass ? SolarShapeLayer::VenusRadar :
                                                                 SolarShapeLayer::Visible);
        }
    };
    struct CloudDraw {
        const BodyState* body;
        const BodySpec* spec;
        int index;
        double scale_factor;
        double view_position;
    };
    struct RingDraw {
        const BodyState* body;
        const BodySpec* spec;
        int index;
        double scale_factor;
        double view_position;
    };
    std::vector<CloudDraw> clouds;
    std::vector<RingDraw> rings;
    std::vector<CloudDraw> atmospheres;
    for (const auto& body : snapshot.bodies) {
        const auto found = std::find_if(project.bodies.begin(), project.bodies.end(),
                                        [&](const BodySpec& item) { return item.id == body.id; });
        if (found == project.bodies.end()) continue;
        const int index = static_cast<int>(found - project.bodies.begin());
        Material material{found->appearance, parse_color(found->appearance.primary),
                          parse_color(found->appearance.secondary), index == 0};
        material.time = snapshot.time;
        double scale_factor = project.display_scale;
        if (project.solar_system) {
            material.kind = found->kind;
            material.design = &found->design;
            if (found->kind == BodyKind::Venus) {
                material.color_map = &solar_texture(found->kind, AssetLayer::VenusSurface);
                material.venus_surface_pass = true;
            } else {
                material.color_map = &solar_texture(found->kind, AssetLayer::Color);
            }
            if (found->kind == BodyKind::Saturn) {
                material.ring_map = &solar_texture(found->kind, AssetLayer::Ring);
            }
            scale_factor = found->display_radius * project.display_scale / found->radius;
        }
        configure(material, body, scale_factor, true);
        draw_sphere(rasterizer, body, material, index, scale_factor, snapshot.time);
        const double view_position = body.position.x * std::sin(config.camera.yaw) *
                                         std::cos(config.camera.pitch) +
                                     body.position.y * std::sin(config.camera.pitch);
        rings.push_back({&body, &*found, index, scale_factor, view_position});
        if (project.solar_system) atmospheres.push_back({&body, &*found, index, scale_factor, view_position});
        if (project.solar_system && found->design.cloud_opacity > 0.0 &&
            (found->kind == BodyKind::Venus || found->kind == BodyKind::Earth)) {
            clouds.push_back({&body, &*found, index, scale_factor, view_position});
        }
    }
    for (const auto& trail : snapshot.reference_trails) {
        const auto found = std::find_if(project.bodies.begin(), project.bodies.end(),
                                        [&](const BodySpec& body) { return body.id == trail.id; });
        if (found != project.bodies.end()) {
            rasterizer.trail(trail, {0.95, 0.64, 0.24},
                             static_cast<int>(found - project.bodies.begin()),
                             TraceSurface::ReferenceTrail);
        }
    }
    for (const auto& trail : snapshot.trails) {
        const auto found = std::find_if(project.bodies.begin(), project.bodies.end(),
                                        [&](const BodySpec& body) { return body.id == trail.id; });
        if (found != project.bodies.end()) {
            rasterizer.trail(trail, parse_color(found->appearance.primary),
                             static_cast<int>(found - project.bodies.begin()));
        }
    }
    std::stable_sort(rings.begin(), rings.end(),
                     [](const RingDraw& a, const RingDraw& b) {
                         return a.view_position < b.view_position;
                     });
    for (const RingDraw& ring : rings) {
        const BodySpec& spec = *ring.spec;
        Material material{spec.appearance, parse_color(spec.appearance.primary),
                          parse_color(spec.appearance.secondary), ring.index == 0};
        if (project.solar_system) {
            material.kind = spec.kind;
            material.design = &spec.design;
            if (spec.kind == BodyKind::Saturn) {
                material.ring_map = &solar_texture(spec.kind, AssetLayer::Ring);
            }
        }
        configure(material, *ring.body, ring.scale_factor, false);
        draw_ring(rasterizer, *ring.body, material, ring.index, ring.scale_factor);
    }
    std::stable_sort(clouds.begin(), clouds.end(),
                     [](const CloudDraw& a, const CloudDraw& b) {
                         return a.view_position < b.view_position;
                     });
    for (const CloudDraw& cloud : clouds) {
        const BodySpec& spec = *cloud.spec;
        Material material{spec.appearance, parse_color(spec.appearance.primary),
                          parse_color(spec.appearance.secondary), cloud.index == 0};
        material.kind = spec.kind;
        material.design = &spec.design;
        material.color_map = &solar_texture(spec.kind, AssetLayer::Color);
        material.time = snapshot.time;
        if (spec.kind == BodyKind::Venus) material.venus_cloud_pass = true;
        else {
            material.cloud_pass = true;
            material.cloud_map = &solar_texture(spec.kind, AssetLayer::Clouds);
        }
        configure(material, *cloud.body, cloud.scale_factor, spec.kind == BodyKind::Venus);
        rasterizer.begin_cloud_layer();
        draw_sphere(rasterizer, *cloud.body, material, cloud.index,
                    cloud.scale_factor, snapshot.time);
    }
    std::stable_sort(atmospheres.begin(), atmospheres.end(),
                     [](const CloudDraw& a, const CloudDraw& b) { return a.view_position < b.view_position; });
    for (const auto& atmosphere : atmospheres) {
        const auto& spec = *atmosphere.spec;
        Material material{spec.appearance, {}, {}, false};
        material.kind = spec.kind; material.design = &spec.design;
        configure(material, *atmosphere.body, atmosphere.scale_factor, false);
        rasterizer.atmosphere(material, atmosphere.index);
    }
    RenderFrame frame = rasterizer.resolve();
    frame.render_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    frame.panel_render_ms[0] = frame.render_ms;
    frame.panel_sampling[0] = config.sampling;
    rasterizer.trace_pixel(frame);
    return frame;
}

} // namespace planet_sandbox

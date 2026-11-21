#include "triangle_coverage.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace planet_sandbox::raster_detail {
namespace {
static_assert(std::numeric_limits<double>::radix == 2 && std::numeric_limits<double>::digits == 53,
              "Screen coordinate conversion requires binary64 doubles");

// Only used for exceptionally large offscreen vertices. Signed base-2^32
// integers keep the very same grid and exact edge signs without __int128,
// overflow, or dropping triangles that cross the viewport. Ordinary rendering
// uses int64 exclusively. No general-purpose division is needed here.
struct Integer {
    std::vector<std::uint32_t> words; // little endian magnitude
    int sign = 0;

    void normalize() {
        while (!words.empty() && words.back() == 0) words.pop_back();
        if (words.empty()) sign = 0;
    }

    // value*2^shift must be an integer; all conversions below ensure this.
    static Integer scaled(double value, int shift) {
        Integer out;
        if (value == 0) return out;
        out.sign = value < 0 ? -1 : 1;
        int exponent = 0;
        const double fraction = std::frexp(std::abs(value), &exponent);
        auto mantissa = static_cast<std::uint64_t>(std::ldexp(fraction, 53));
        shift += exponent - 53;
        if (shift < 0) { mantissa >>= -shift; shift = 0; }
        out.words.resize(shift / 32, 0);
        const int bits = shift % 32;
        out.words.push_back(static_cast<std::uint32_t>(mantissa << bits));
        out.words.push_back(static_cast<std::uint32_t>(mantissa >> (32 - bits)));
        if (bits > 11) out.words.push_back(static_cast<std::uint32_t>(mantissa >> (64 - bits)));
        out.normalize();
        return out;
    }

    static Integer pixels(double value) {
        return std::abs(value) < grid_aligned_min ? scaled(std::round(value * subpixel_scale),0)
                                                  : scaled(value,subpixel_bits);
    }

    static Integer sample(std::int64_t value) {
        Integer out;
        if (!value) return out;
        out.sign = value < 0 ? -1 : 1;
        const auto magnitude = value < 0 ? std::uint64_t(-(value + 1)) + 1 : std::uint64_t(value);
        out.words = {static_cast<std::uint32_t>(magnitude), static_cast<std::uint32_t>(magnitude >> 32)};
        out.normalize();
        return out;
    }

    double leading() const {
        double value = words.back();
        if (words.size() > 1) value += std::ldexp(double(words[words.size()-2]), -32);
        if (words.size() > 2) value += std::ldexp(double(words[words.size()-3]), -64);
        return value;
    }
};

Integer operator-(Integer v) { v.sign = -v.sign; return v; }

Integer operator+(const Integer& a, const Integer& b) {
    if (!a.sign) return b;
    if (!b.sign) return a;
    Integer out;
    if (a.sign == b.sign) {
        out.sign = a.sign;
        out.words.resize(std::max(a.words.size(), b.words.size()) + 1);
        std::uint64_t carry = 0;
        for (std::size_t i = 0; i < out.words.size(); ++i) {
            const std::uint64_t sum = carry + (i < a.words.size() ? a.words[i] : 0ull) +
                                             (i < b.words.size() ? b.words[i] : 0ull);
            out.words[i] = static_cast<std::uint32_t>(sum);
            carry = sum >> 32;
        }
    } else {
        const bool a_less = a.words.size() != b.words.size() ? a.words.size() < b.words.size() :
            std::lexicographical_compare(a.words.rbegin(), a.words.rend(), b.words.rbegin(), b.words.rend());
        const Integer& large = a_less ? b : a;
        const Integer& small = a_less ? a : b;
        out.sign = large.sign;
        out.words.resize(large.words.size());
        std::uint64_t borrow = 0;
        for (std::size_t i = 0; i < large.words.size(); ++i) {
            const std::uint64_t sub = borrow + (i < small.words.size() ? small.words[i] : 0ull);
            out.words[i] = static_cast<std::uint32_t>(std::uint64_t(large.words[i]) - sub);
            borrow = std::uint64_t(large.words[i]) < sub;
        }
    }
    out.normalize();
    return out;
}
Integer operator-(const Integer& a, const Integer& b) { return a + -b; }

Integer operator*(const Integer& a, const Integer& b) {
    Integer out;
    if (!a.sign || !b.sign) return out;
    out.sign = a.sign * b.sign;
    out.words.resize(a.words.size() + b.words.size());
    for (std::size_t i = 0; i < a.words.size(); ++i) {
        std::uint64_t carry = 0;
        for (std::size_t j = 0; j < b.words.size(); ++j) {
            const std::uint64_t value = std::uint64_t(a.words[i]) * b.words[j] + out.words[i+j] + carry;
            out.words[i+j] = static_cast<std::uint32_t>(value);
            carry = value >> 32;
        }
        out.words[i+b.words.size()] = static_cast<std::uint32_t>(carry);
    }
    out.normalize();
    return out;
}

double ratio(const Integer& a, const Integer& b) {
    if (!a.sign) return 0;
    return std::ldexp(a.leading() / b.leading(),
                     32 * (int(a.words.size()) - int(b.words.size())));
}

double snap(double x) {
    return std::abs(x) < grid_aligned_min ? std::round(x * subpixel_scale) / subpixel_scale : x;
}

bool original_degenerate(const std::array<Vec2,3>& p) {
    const double left = (p[2].x-p[0].x)*(p[1].y-p[0].y);
    const double right = (p[2].y-p[0].y)*(p[1].x-p[0].x);
    const double error = 16*std::numeric_limits<double>::epsilon()*(std::abs(left)+std::abs(right));
    if (std::isfinite(error) && std::abs(left-right) > error) return false;
    // Cancellation, underflow or overflow: check exact input geometry before
    // snapping, so a truly collinear triangle cannot gain area by rounding.
    int lowest = std::numeric_limits<int>::max();
    for (auto v : p) for (double coordinate : {v.x,v.y}) if (coordinate != 0) {
        int exponent; std::frexp(coordinate,&exponent);
        lowest = std::min(lowest,exponent-53);
    }
    if (lowest == std::numeric_limits<int>::max()) return true;
    std::array<Integer,3> x,y;
    for (int i=0;i<3;++i) {
        x[i]=Integer::scaled(p[i].x,-lowest);y[i]=Integer::scaled(p[i].y,-lowest);
    }
    return ((x[2]-x[0])*(y[1]-y[0])-(y[2]-y[0])*(x[1]-x[0])).sign == 0;
}

// |coordinate| <= 2^29 => |difference| <= 2^30, |edge| <= 2^61.
std::int64_t edge(std::int64_t ax, std::int64_t ay, std::int64_t bx,
                  std::int64_t by, std::int64_t x, std::int64_t y) {
    return (x-ax)*(by-ay) - (y-ay)*(bx-ax);
}

// Two-word two's-complement arithmetic avoids per-sample allocations for
// moderately large projections. Unsigned operations are defined modulo 2^128;
// the bounds in CompactTriangle keep the signed mathematical result in range.
// This works on C++17 platforms without a compiler-specific __int128 type.
struct Int128 {
    std::uint64_t low=0,high=0;
    static Int128 from_signed(std::int64_t value) {return {std::uint64_t(value),value<0?~std::uint64_t{0}:0};}
    int sign() const { return high>>63 ? -1 : (low || high ? 1 : 0); }
    Int128 operator-() const { return {~low+1,~high+(low==0)}; }
    friend Int128 operator+(Int128 a,Int128 b) {
        const auto low=a.low+b.low;
        return {low,a.high+b.high+(low<a.low)};
    }
    friend Int128 operator-(Int128 a,Int128 b) { return a+-b; }
    static Int128 product(std::int64_t a,std::int64_t b) {
        const auto ua=a<0 ? std::uint64_t{0}-std::uint64_t(a) : std::uint64_t(a);
        const auto ub=b<0 ? std::uint64_t{0}-std::uint64_t(b) : std::uint64_t(b);
        const std::uint64_t a0=std::uint32_t(ua),a1=ua>>32,b0=std::uint32_t(ub),b1=ub>>32;
        const auto p00=a0*b0,p01=a0*b1,p10=a1*b0,p11=a1*b1;
        const auto middle=(p00>>32)+std::uint32_t(p01)+std::uint32_t(p10);
        Int128 out{(middle<<32)|std::uint32_t(p00),p11+(p01>>32)+(p10>>32)+(middle>>32)};
        return (a<0)!=(b<0) ? -out : out;
    }
    double positive_double() const { return double(high)*0x1p64+double(low); }
};

struct CompactTriangle {
    // Coordinates <=2^52, coefficients <=2^53, constants <=2^105.
    // Even arbitrary int64 sample coordinates give sums below 2^118.
    static constexpr std::int64_t limit=std::int64_t{1}<<52;
    struct Edge {
        std::int64_t x,y;Int128 constant;
        bool small_constant=false;
        std::int64_t constant64=0;
        double constant_double=0;
    };
    std::array<Edge,3> edges;
    int orientation=0;
    double inverse_area=0;
    std::int64_t sample_limit=0;
    explicit CompactTriangle(const std::array<Vec2,3>& p) {
        std::array<std::int64_t,3> x,y;
        for(int i=0;i<3;++i) {x[i]=std::int64_t(p[i].x*subpixel_scale);y[i]=std::int64_t(p[i].y*subpixel_scale);}
        for(int i=0;i<3;++i) {
            const int a=(i+1)%3,b=(i+2)%3;
            edges[i]={y[b]-y[a],x[a]-x[b],Int128::product(y[a],x[b])-Int128::product(x[a],y[b])};
        }
        auto area=at(edges[0],x[0],y[0]);orientation=area.sign();
        if(!orientation) return;
        if(orientation<0) {
            area=-area;
            for(auto& e:edges) {e.x=-e.x;e.y=-e.y;e.constant=-e.constant;}
        }
        inverse_area=1/area.positive_double();
        std::int64_t coefficient=0,max_constant=0;
        for(auto& e:edges) {
            coefficient=std::max({coefficient,std::abs(e.x),std::abs(e.y)});
            const auto magnitude=e.constant.sign()<0 ? -e.constant : e.constant;
            e.small_constant=!magnitude.high && magnitude.low<=std::uint64_t(std::numeric_limits<std::int64_t>::max());
            if(e.small_constant) {
                const auto value=std::int64_t(magnitude.low);
                e.constant64=e.constant.sign()<0 ? -value : value;
                max_constant=std::max(max_constant,value);
            } else if(e.constant.sign()>0) e.constant_double=e.constant.positive_double();
        }
        // Large vertices need wide constants, but viewport sample*coefficient
        // products often still fit int64. This guard bounds their sum as well.
        sample_limit=(std::numeric_limits<std::int64_t>::max()-max_constant)/(2*coefficient);
    }
    static Int128 at(const Edge& e,std::int64_t x,std::int64_t y) {
        return Int128::product(x,e.x)+Int128::product(y,e.y)+e.constant;
    }
    bool weights(std::int64_t x,std::int64_t y,const std::array<bool,3>& inclusive,
                 std::array<double,3>& w) const {
        const bool small=x>=-sample_limit && x<=sample_limit && y>=-sample_limit && y<=sample_limit;
        if(small) {
            for(int i=0;i<3;++i) {
                const auto& e=edges[i];
                const auto delta=x*e.x+y*e.y;
                if(e.small_constant) {
                    const auto value=delta+e.constant64;
                    if(value<0 || (!value&&!inclusive[i])) return false;
                    w[i]=double(value)*inverse_area;
                } else {
                    // |constant|>=2^63, |delta|<2^63: the exact sign cannot
                    // change. If constant>=2^64, subtracting delta cannot lose
                    // more than half its magnitude in the double conversion.
                    if(e.constant.sign()<0) return false;
                    const double value=e.constant.high ? e.constant_double+double(delta) :
                        (e.constant+Int128::from_signed(delta)).positive_double();
                    w[i]=value*inverse_area;
                }
            }
            return true;
        }
        std::array<Int128,3> values;
        for(int i=0;i<3;++i) {
            const auto& e=edges[i];
            values[i]=at(e,x,y);
            const int sign=values[i].sign();
            if(sign<0 || (!sign&&!inclusive[i])) return false;
        }
        for(int i=0;i<3;++i) w[i]=values[i].positive_double()*inverse_area;
        return true;
    }
};
} // namespace

struct TriangleCoverage::WideTriangle {
    struct Edge {
        Integer dx, dy, constant;
        Integer at(const Integer& x, const Integer& y) const { return x*dy - y*dx + constant; }
    };
    std::array<Edge, 3> edges;
    Integer area;
    std::unique_ptr<CompactTriangle> compact;
    int orientation() const { return compact ? compact->orientation : area.sign; }

    explicit WideTriangle(const std::array<Vec2, 3>& p) {
        const double limit=double(CompactTriangle::limit)/subpixel_scale;
        if(std::all_of(p.begin(),p.end(),[&](Vec2 v){return std::abs(v.x)<=limit && std::abs(v.y)<=limit;})) {
            compact=std::make_unique<CompactTriangle>(p);
            return;
        }
        std::array<Integer, 3> x, y;
        for (int i = 0; i < 3; ++i) { x[i] = Integer::pixels(p[i].x); y[i] = Integer::pixels(p[i].y); }
        for (int i = 0; i < 3; ++i) {
            const int a = (i+1)%3, b = (i+2)%3;
            edges[i] = {x[b]-x[a], y[b]-y[a], y[a]*x[b]-x[a]*y[b]};
        }
        area = edges[0].at(x[0], y[0]);
    }
};

TriangleCoverage::~TriangleCoverage() = default;

TriangleCoverage::TriangleCoverage(const std::array<Vec2, 3>& vertices) {
    std::array<Vec2, 3> p;
    bool fast = true;
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(vertices[i].x) || !std::isfinite(vertices[i].y)) return;
        p[i] = {snap(vertices[i].x), snap(vertices[i].y)};
        fast &= std::abs(p[i].x) <= double(fast_subpixel_limit)/subpixel_scale &&
                std::abs(p[i].y) <= double(fast_subpixel_limit)/subpixel_scale;
    }
    if (original_degenerate(vertices)) return;
    minimum_ = {std::min({p[0].x,p[1].x,p[2].x}), std::min({p[0].y,p[1].y,p[2].y})};
    maximum_ = {std::max({p[0].x,p[1].x,p[2].x}), std::max({p[0].y,p[1].y,p[2].y})};
    if (fast) {
        struct Point { std::int64_t x,y; };
        std::array<Point,3> points;
        for (int i = 0; i < 3; ++i)
            points[i] = {std::int64_t(p[i].x*subpixel_scale), std::int64_t(p[i].y*subpixel_scale)};
        const auto area = edge(points[0].x,points[0].y,points[1].x,points[1].y,points[2].x,points[2].y);
        if (!area) return;
        orientation_ = area > 0 ? 1 : -1;
        inverse_area_ = 1.0 / double(area*orientation_);
        for (int i=0;i<3;++i) {
            const auto a=points[(i+1)%3],b=points[(i+2)%3];
            edges_[i] = {(b.y-a.y)*orientation_,(a.x-b.x)*orientation_,
                         (a.y*b.x-a.x*b.y)*orientation_,0};
        }
    } else {
        wide_ = std::make_unique<WideTriangle>(p);
        orientation_ = wide_->orientation();
        if (!orientation_) return;
    }
    for (int i = 0; i < 3; ++i) {
        const Vec2 a = p[(i+1)%3], b = p[(i+2)%3];
        // Compare coordinates rather than subtracting potentially huge doubles.
        inclusive_[i] = a.y == b.y ? (orientation_ > 0 ? b.x < a.x : b.x > a.x) :
                                     (orientation_ > 0 ? b.y > a.y : b.y < a.y);
        edges_[i].threshold = inclusive_[i] ? 0 : 1;
    }
    valid_ = true;
}

bool TriangleCoverage::wide_weights(std::int64_t x, std::int64_t y, std::array<double, 3>& w) const {
    if(wide_->compact) return wide_->compact->weights(x,y,inclusive_,w);
    const auto sx = Integer::sample(x), sy = Integer::sample(y);
    std::array<Integer, 3> values;
    for (int i = 0; i < 3; ++i) {
        values[i] = wide_->edges[i].at(sx, sy);
        const int positive = values[i].sign*orientation_;
        if (positive < 0 || (positive == 0 && !inclusive_[i])) return false;
    }
    for (int i = 0; i < 3; ++i) w[i] = ratio(values[i],wide_->area);
    return true;
}

} // namespace planet_sandbox::raster_detail

// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "highlights/infrastructure/backdrop_shapes.hpp"

#include "highlights/infrastructure/svg_shape.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>

namespace ttrally::highlights {

namespace {

// All shape measures below are shares of the box's half height (hy) unless noted.

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegree = kPi / 180.0;
constexpr double kMaxUint32 = 4294967295.0;
constexpr double kOctaveShift = 17.31; ///< keeps octaves of noise from lining up
constexpr double kSideShift = 101.7;   ///< other noise for the other side of a shape
constexpr double kAntialiasPx = 1.0;   ///< width of an edge without softness

std::uint32_t mix32(std::uint32_t x) {
    // "lowbias32" integer hash (Chris Wellons)
    x ^= x >> 16U;
    x *= 0x7feb352dU;
    x ^= x >> 15U;
    x *= 0x846ca68bU;
    x ^= x >> 16U;
    return x;
}

/// Reproducible chance from a seed: smooth 1D value noise and plain random numbers.
class Noise {
  public:
    Noise(std::uint32_t seed, std::uint32_t salt)
        : key_(mix32(seed ^ mix32(salt + 0x9e3779b9U))) {}

    /// A random value in [-1, 1] for each whole number.
    [[nodiscard]] double lattice(std::int64_t i) const {
        const auto low = static_cast<std::uint32_t>(i);
        const auto high = static_cast<std::uint32_t>(static_cast<std::uint64_t>(i) >> 32U);
        return mix32(key_ ^ mix32(low ^ mix32(high))) / kMaxUint32 * 2.0 - 1.0;
    }

    /// A random value in [0, 1] for a number.
    [[nodiscard]] double uniform(std::int64_t i) const { return (lattice(i) + 1.0) / 2.0; }

    /// Smooth noise in [-1, 1], changing about once per unit of t.
    [[nodiscard]] double at(double t) const {
        const double cell = std::floor(t);
        const double f = t - cell;
        const double smooth = f * f * (3.0 - 2.0 * f);
        const auto i = static_cast<std::int64_t>(cell);
        const double a = lattice(i);
        return a + (lattice(i + 1) - a) * smooth;
    }

    /// Noise with finer detail: `octaves` layers, each half as large and half as strong.
    [[nodiscard]] double fractal(double t, int octaves) const {
        double sum = 0;
        double strength = 1;
        double total = 0;
        double frequency = 1;
        for (int k = 0; k < octaves; ++k) {
            sum += strength * at(t * frequency + k * kOctaveShift);
            total += strength;
            strength /= 2;
            frequency *= 2;
        }
        return sum / total;
    }

  private:
    std::uint32_t key_;
};

/// Signed distance in pixels (negative inside) from a point relative to the box's centre.
using Distance = std::function<double(double x, double y)>;

struct Shape {
    Distance front;
    Distance back; ///< may be empty
};

double box_distance(double x, double y, double bx, double by) {
    const double qx = std::abs(x) - bx;
    const double qy = std::abs(y) - by;
    return std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0);
}

double rounded_box(double x, double y, double bx, double by, double radius) {
    const double r = std::clamp(radius, 0.0, std::min(bx, by));
    return box_distance(x, y, bx - r, by - r) - r;
}

double circle(double x, double y, double cx, double cy, double r) {
    return std::hypot(x - cx, y - cy) - r;
}

struct Point {
    double x = 0;
    double y = 0;
};

double cross(Point a, Point b) { return a.x * b.y - a.y * b.x; }
double dot(Point a, Point b) { return a.x * b.x + a.y * b.y; }

/// Distance to a triangle (after Inigo Quilez).
double triangle(Point p, Point p0, Point p1, Point p2) {
    const Point e0{p1.x - p0.x, p1.y - p0.y};
    const Point e1{p2.x - p1.x, p2.y - p1.y};
    const Point e2{p0.x - p2.x, p0.y - p2.y};
    const Point v0{p.x - p0.x, p.y - p0.y};
    const Point v1{p.x - p1.x, p.y - p1.y};
    const Point v2{p.x - p2.x, p.y - p2.y};
    const auto nearest = [](Point v, Point e) {
        const double t = std::clamp(dot(v, e) / dot(e, e), 0.0, 1.0);
        return Point{v.x - e.x * t, v.y - e.y * t};
    };
    const Point q0 = nearest(v0, e0);
    const Point q1 = nearest(v1, e1);
    const Point q2 = nearest(v2, e2);
    const double sign = cross(e0, e2) < 0 ? -1.0 : 1.0;
    const double squared = std::min({dot(q0, q0), dot(q1, q1), dot(q2, q2)});
    const double side =
        std::min({sign * cross(v0, e0), sign * cross(v1, e1), sign * cross(v2, e2)});
    return -std::sqrt(squared) * (side < 0 ? -1.0 : 1.0);
}

double smoothstep(double edge0, double edge1, double x) {
    const double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

/// 0 at the middle of a period, 1 at its ends: a zigzag.
double zigzag(double t) { return std::abs(t - std::floor(t) - 0.5) * 2.0; }

/// The shape turned by `degrees` (clockwise on screen) and moved by (dx, dy).
Distance turned(Distance shape, double degrees, double dx = 0, double dy = 0) {
    const double c = std::cos(degrees * kDegree);
    const double s = std::sin(degrees * kDegree);
    return [shape = std::move(shape), c, s, dx, dy](double x, double y) {
        const double px = x - dx;
        const double py = y - dy;
        return shape(px * c + py * s, -px * s + py * c);
    };
}

Distance paper_strip(double hx, double hy, std::uint32_t seed) {
    const Noise edges(seed, 1);
    const Noise ends(seed, 2);
    const Noise lean(seed, 3);
    constexpr double kLean = 0.25;
    const double left_lean = kLean * lean.lattice(0);
    const double right_lean = kLean * lean.lattice(1);
    return [=](double x, double y) {
        const double edge =
            hy * (1.0 + 0.06 * edges.fractal(x / (0.8 * hy) + (y < 0 ? 0 : kSideShift), 3));
        const bool left = x < 0;
        const double tear = 0.4 * hy * ends.fractal(y / (0.3 * hy) + (left ? 0 : kSideShift), 5);
        const double end = hx + 0.35 * hy + (left ? -left_lean : right_lean) * y + tear;
        return std::max(std::abs(y) - edge, std::abs(x) - end);
    };
}

Shape torn_ends(double hx, double hy, std::uint32_t seed) {
    const Noise ends(seed, 2);
    return {[=](double x, double y) {
                const bool left = x < 0;
                const double tear =
                    0.42 * hy * ends.fractal(y / (0.3 * hy) + (left ? 0 : kSideShift), 5);
                return std::max(std::abs(y) - hy, std::abs(x) - (hx + 0.4 * hy + tear));
            },
            {}};
}

Shape tape(double hx, double hy, std::uint32_t seed) {
    const Noise chance(seed, 4);
    const double tilt = (1.5 + 1.5 * chance.uniform(0)) * (chance.uniform(1) < 0.5 ? -1 : 1);
    constexpr double kSlant = 0.35;
    const double left_slant = kSlant * chance.lattice(2);
    const double right_slant = kSlant * chance.lattice(3);
    const double left_phase = chance.uniform(4);
    const double right_phase = chance.uniform(5);
    const Noise rough(seed, 5);
    const Distance strip = [=](double x, double y) {
        const bool left = x < 0;
        const double teeth = 0.16 * hy *
                             (zigzag(y / (0.13 * hy) + (left ? left_phase : right_phase)) - 0.5);
        const double wobble =
            0.05 * hy * rough.fractal(y / (0.5 * hy) + (left ? 0 : kSideShift), 2);
        const double end =
            hx + 0.25 * hy + (left ? -left_slant : right_slant) * y + teeth + wobble;
        return std::max(std::abs(y) - hy, std::abs(x) - end);
    };
    return {turned(strip, tilt), {}};
}

Shape double_strip(double hx, double hy, std::uint32_t seed) {
    constexpr double kFrontTilt = -1.0;
    constexpr double kBackTilt = 2.5;
    constexpr double kBackGrowth = 1.04;
    return {turned(paper_strip(hx, hy, seed), kFrontTilt),
            turned(paper_strip(hx * kBackGrowth, hy * kBackGrowth, next_seed(seed)), kBackTilt,
                   0.12 * hy, 0.14 * hy)};
}

Shape cloud(double hx, double hy, std::uint32_t seed) {
    const Noise chance(seed, 6);
    struct Bump {
        double x;
        double y;
        double r;
    };
    std::vector<Bump> bumps;
    const double spacing = 0.9 * hy;
    const int count = std::max(1, static_cast<int>(std::lround(2 * hx / spacing)));
    const double step = 2 * hx / count;
    int n = 0;
    for (int k = 0; k <= count; ++k) {
        const double x = -hx + k * step;
        bumps.push_back({x + 0.15 * hy * chance.lattice(n++),
                         -0.45 * hy + 0.08 * hy * chance.lattice(n++),
                         hy * (0.55 + 0.25 * chance.uniform(n++))});
        bumps.push_back({x + 0.15 * hy * chance.lattice(n++),
                         0.45 * hy + 0.08 * hy * chance.lattice(n++),
                         hy * (0.5 + 0.2 * chance.uniform(n++))});
    }
    for (const double side : {-1.0, 1.0}) {
        bumps.push_back({side * hx, 0.05 * hy * chance.lattice(n++),
                         hy * (0.8 + 0.15 * chance.uniform(n++))});
    }
    return {[=](double x, double y) {
                double d = box_distance(x, y, hx, 0.75 * hy);
                for (const Bump& bump : bumps) {
                    d = std::min(d, circle(x, y, bump.x, bump.y, bump.r));
                }
                return d;
            },
            {}};
}

Shape brush(double hx, double hy, std::uint32_t seed) {
    const Noise edge(seed, 7);
    const Noise ends(seed, 8);
    const Noise bristles(seed, 9);
    return {[=](double x, double y) {
                const double ax = std::abs(x);
                const bool left = x < 0;
                const double taper = smoothstep(0.85 * hx, hx + 0.6 * hy, ax);
                const double half =
                    hy * (1.0 + 0.08 * edge.fractal(x / (0.7 * hy) + (y < 0 ? 0 : kSideShift), 3)) *
                    (1.0 - 0.3 * taper);
                const double base = hx + (left ? 0.15 : 0.45) * hy;
                const double bristle_reach = (left ? 0.25 : 0.55) * hy;
                const double side = left ? 0 : kSideShift;
                const double end = base + 0.2 * hy * ends.fractal(y / (0.4 * hy) + side, 3) +
                                   bristle_reach * bristles.at(y / (0.06 * hy) + side);
                return std::max(std::abs(y) - half, ax - end);
            },
            {}};
}

Shape speech_bubble(double hx, double hy, TitlePosition position) {
    const bool top = position == TitlePosition::TopLeft || position == TitlePosition::TopCenter ||
                     position == TitlePosition::TopRight;
    const bool left = position == TitlePosition::TopLeft || position == TitlePosition::BottomLeft;
    const bool right =
        position == TitlePosition::TopRight || position == TitlePosition::BottomRight;
    const double limit = std::max(0.0, hx - 0.6 * hy);
    const double wanted = left ? -0.55 * hx : right ? 0.55 * hx : -0.3 * hx;
    const double base = std::clamp(wanted, -limit, limit);
    const double lean = (right ? 0.25 : -0.25) * hy;
    const double toward = top ? -1.0 : 1.0; // the tip points to the picture's nearer edge
    const Point a{base - 0.3 * hy, toward * 0.6 * hy};
    const Point b{base + 0.3 * hy, toward * 0.6 * hy};
    const Point tip{base + lean, toward * 1.7 * hy};
    return {[=](double x, double y) {
                return std::min(rounded_box(x, y, hx, hy, 0.55 * hy),
                                triangle({x, y}, a, b, tip));
            },
            {}};
}

Shape ribbon(double hx, double hy) {
    const double drop = 0.4 * hy;  // the tails hang lower
    const double tail = 0.85 * hy; // half height of a tail
    const double inner = hx - 0.4 * hy;
    const double outer = hx + 0.9 * hy;
    const double notch = 0.45 * hy;
    const double slope = std::sqrt(1.0 + (notch / tail) * (notch / tail));
    return {[=](double x, double y) { return rounded_box(x, y, hx, hy, 0.04 * hy); },
            [=](double x, double y) {
                const double ty = std::abs(y - drop);
                const double ax = std::abs(x);
                const double end = outer - notch * (1.0 - std::min(1.0, ty / tail));
                const double cut = (ax - end) / slope;
                return std::max({ty - tail, inner - ax, cut});
            }};
}

Shape sport_bar(double hx, double hy) {
    constexpr double kSlant = 0.35;
    const double ex = hx + kSlant * hy;
    const double norm = std::sqrt(1.0 + kSlant * kSlant);
    return {[=](double x, double y) {
                return std::max(std::abs(y) - hy, (std::abs(x + kSlant * y) - ex) / norm);
            },
            {}};
}

Shape shape_of(const Backdrop& backdrop, double hx, double hy, double line_height,
               TitlePosition position, std::uint32_t seed) {
    switch (backdrop.shape) {
    case BackdropShape::RoundedBox:
        return {[=, r = backdrop.corner * line_height](double x, double y) {
                    return rounded_box(x, y, hx, hy, r);
                },
                {}};
    case BackdropShape::Pill:
        return {[=](double x, double y) { return rounded_box(x, y, hx + 0.5 * hy, hy, hy); }, {}};
    case BackdropShape::Cloud:
        return cloud(hx, hy, seed);
    case BackdropShape::TornEnds:
        return torn_ends(hx, hy, seed);
    case BackdropShape::PaperStrip:
        return {paper_strip(hx, hy, seed), {}};
    case BackdropShape::Tape:
        return tape(hx, hy, seed);
    case BackdropShape::DoubleStrip:
        return double_strip(hx, hy, seed);
    case BackdropShape::Brush:
        return brush(hx, hy, seed);
    case BackdropShape::SpeechBubble:
        return speech_bubble(hx, hy, position);
    case BackdropShape::Ribbon:
        return ribbon(hx, hy);
    case BackdropShape::SportBar:
        return sport_bar(hx, hy);
    default:
        return {};
    }
}

/// How far the shape reaches beyond its box (half sizes hx, hy), in pixels.
double overhang(BackdropShape shape, double hx, double hy) {
    constexpr double kTilt = 0.07; // sin of the largest tilt, with room
    switch (shape) {
    case BackdropShape::Pill:
        return 0.5 * hy;
    case BackdropShape::Cloud:
        return 1.1 * hy;
    case BackdropShape::TornEnds:
        return 0.9 * hy;
    case BackdropShape::PaperStrip:
        return 0.9 * hy;
    case BackdropShape::Tape:
        return 0.8 * hy + kTilt * hx;
    case BackdropShape::DoubleStrip:
        return 1.1 * hy + kTilt * hx;
    case BackdropShape::Brush:
        return 1.2 * hy;
    case BackdropShape::SpeechBubble:
        return 0.9 * hy;
    case BackdropShape::Ribbon:
        return 1.5 * hy;
    case BackdropShape::SportBar:
        return 0.8 * hy;
    default:
        return 0;
    }
}

/// Coverage at a signed distance: 1 inside, 0 outside, a ramp of `width` pixels at the edge.
float ramp(double distance, double width) {
    return static_cast<float>(std::clamp(0.5 - distance / width, 0.0, 1.0));
}

std::uint32_t seed_of_line(std::uint32_t seed, std::size_t line) {
    return line == 0 ? seed : mix32(seed + static_cast<std::uint32_t>(line) * 0x9e3779b9U);
}

struct Canvas {
    int width;
    int height;
    BackdropPlanes& planes;
};

void draw_shape(const Shape& shape, const ShapeBox& box, double reach, const Backdrop& backdrop,
                double line_height, Canvas canvas) {
    const double cx = (box.left + box.right) / 2;
    const double cy = (box.top + box.bottom) / 2;
    const double soft = std::max(kAntialiasPx, backdrop.softness * line_height);
    const double border = backdrop.border_width * line_height;
    const int x0 = std::max(0, static_cast<int>(std::floor(box.left - reach)));
    const int x1 = std::min(canvas.width, static_cast<int>(std::ceil(box.right + reach)));
    const int y0 = std::max(0, static_cast<int>(std::floor(box.top - reach)));
    const int y1 = std::min(canvas.height, static_cast<int>(std::ceil(box.bottom + reach)));
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const double px = x + 0.5 - cx;
            const double py = y + 0.5 - cy;
            const auto i = static_cast<std::size_t>(y) * canvas.width + x;
            const double d = shape.front(px, py);
            const float inside = ramp(d, soft);
            canvas.planes.front[i] = std::max(canvas.planes.front[i], inside);
            if (backdrop.bordered) {
                const float ring = std::max(0.0F, inside - ramp(d + border, soft));
                canvas.planes.border[i] = std::max(canvas.planes.border[i], ring);
            }
            if (shape.back) {
                canvas.planes.back[i] =
                    std::max(canvas.planes.back[i], ramp(shape.back(px, py), soft));
            }
        }
    }
}

/// Every pixel takes the smallest value within `radius` (a square).
void erode(std::vector<float>& plane, int width, int height, int radius) {
    if (radius <= 0) {
        return;
    }
    std::vector<float> row(plane.size());
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float least = 1.0F;
            for (int k = x - radius; k <= x + radius; ++k) {
                least = std::min(least, k < 0 || k >= width
                                            ? 0.0F
                                            : plane[static_cast<std::size_t>(y) * width + k]);
            }
            row[static_cast<std::size_t>(y) * width + x] = least;
        }
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float least = 1.0F;
            for (int k = y - radius; k <= y + radius; ++k) {
                least = std::min(least, k < 0 || k >= height
                                            ? 0.0F
                                            : row[static_cast<std::size_t>(k) * width + x]);
            }
            plane[static_cast<std::size_t>(y) * width + x] = least;
        }
    }
}

void draw_svg(const SvgShape& svg, const ShapeBox& box, const Backdrop& backdrop,
              double line_height, Canvas canvas) {
    const int left = static_cast<int>(std::lround(box.left));
    const int top = static_cast<int>(std::lround(box.top));
    const int width = std::max(1, static_cast<int>(std::lround(box.right - box.left)));
    const int height = std::max(1, static_cast<int>(std::lround(box.bottom - box.top)));
    const double soft = backdrop.softness * line_height;
    const int room = static_cast<int>(std::ceil(soft)) + 2;
    const int lw = width + 2 * room;
    const int lh = height + 2 * room;
    std::vector<float> local(static_cast<std::size_t>(lw) * lh, 0.0F);
    const std::vector<float> drawn = svg.coverage(width, height);
    for (int y = 0; y < height; ++y) {
        std::copy_n(drawn.begin() + static_cast<std::ptrdiff_t>(y) * width, width,
                    local.begin() + static_cast<std::ptrdiff_t>(y + room) * lw + room);
    }
    if (soft >= 2) {
        blur(local, lw, lh, static_cast<int>(std::lround(soft / 3)));
    }
    std::vector<float> ring;
    if (backdrop.bordered) {
        std::vector<float> inner = local;
        const int line = static_cast<int>(std::lround(backdrop.border_width * line_height));
        erode(inner, lw, lh, std::max(1, line));
        ring.resize(local.size());
        for (std::size_t i = 0; i < local.size(); ++i) {
            ring[i] = std::max(0.0F, local[i] - inner[i]);
        }
    }
    for (int y = 0; y < lh; ++y) {
        const int cy = top - room + y;
        if (cy < 0 || cy >= canvas.height) {
            continue;
        }
        for (int x = 0; x < lw; ++x) {
            const int cx = left - room + x;
            if (cx < 0 || cx >= canvas.width) {
                continue;
            }
            const auto i = static_cast<std::size_t>(cy) * canvas.width + cx;
            const auto j = static_cast<std::size_t>(y) * lw + x;
            canvas.planes.front[i] = std::max(canvas.planes.front[i], local[j]);
            if (!ring.empty()) {
                canvas.planes.border[i] = std::max(canvas.planes.border[i], ring[j]);
            }
        }
    }
}

} // namespace

double backdrop_reach(const Backdrop& backdrop, std::span<const ShapeBox> boxes,
                      double line_height) {
    double reach = 0;
    for (const ShapeBox& box : boxes) {
        reach = std::max(reach, overhang(backdrop.shape, (box.right - box.left) / 2,
                                         (box.bottom - box.top) / 2));
    }
    return reach + backdrop.softness * line_height + 2 * kAntialiasPx;
}

BackdropPlanes draw_backdrop(const Backdrop& backdrop, std::span<const ShapeBox> boxes, int width,
                             int height, double line_height, TitlePosition position,
                             const SvgShape* svg) {
    const auto size = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    BackdropPlanes planes{.back = std::vector<float>(size, 0.0F),
                          .front = std::vector<float>(size, 0.0F),
                          .border = std::vector<float>(size, 0.0F)};
    const Canvas canvas{width, height, planes};
    const double reach = backdrop_reach(backdrop, boxes, line_height);
    for (std::size_t line = 0; line < boxes.size(); ++line) {
        const ShapeBox& box = boxes[line];
        if (backdrop.shape == BackdropShape::Svg) {
            if (svg != nullptr) {
                draw_svg(*svg, box, backdrop, line_height, canvas);
            }
            continue;
        }
        const Shape shape =
            shape_of(backdrop, (box.right - box.left) / 2, (box.bottom - box.top) / 2,
                     line_height, position, seed_of_line(backdrop.seed, line));
        if (shape.front) {
            draw_shape(shape, box, reach, backdrop, line_height, canvas);
        }
    }
    return planes;
}

void blur(std::vector<float>& plane, int width, int height, int radius) {
    if (radius <= 0 || width <= 0 || height <= 0) {
        return;
    }
    constexpr int kPasses = 3;
    const float scale = 1.0F / static_cast<float>(2 * radius + 1);
    std::vector<float> line;
    const auto pass = [&](int count, int length, auto index) {
        line.resize(static_cast<std::size_t>(length));
        for (int c = 0; c < count; ++c) {
            float sum = 0;
            for (int k = 0; k <= std::min(radius, length - 1); ++k) {
                sum += plane[index(c, k)];
            }
            for (int k = 0; k < length; ++k) {
                line[static_cast<std::size_t>(k)] = sum * scale;
                const int enter = k + radius + 1;
                const int leave = k - radius;
                if (enter < length) {
                    sum += plane[index(c, enter)];
                }
                if (leave >= 0) {
                    sum -= plane[index(c, leave)];
                }
            }
            for (int k = 0; k < length; ++k) {
                plane[index(c, k)] = line[static_cast<std::size_t>(k)];
            }
        }
    };
    for (int p = 0; p < kPasses; ++p) {
        pass(height, width, [width](int row, int k) {
            return static_cast<std::size_t>(row) * width + k;
        });
        pass(width, height, [width](int column, int k) {
            return static_cast<std::size_t>(k) * width + column;
        });
    }
}

} // namespace ttrally::highlights

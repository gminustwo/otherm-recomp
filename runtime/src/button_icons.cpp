// The per-controller button prompts, drawn from the player's disc (button_icons.h). A port of the former
// tools/make-button-icons.py (in git history): the same sources, file names (aurora's tex1_<w>x<h>_<XXH64>_<fmt>.png
// keys), folders and drawing, with Pillow's pieces done here: shapes filled on a supersampled canvas, letters through
// stb_truetype (the copy ImGui ships) from a bold system font, Lanczos resampling as Pillow does it (premultiplied
// RGBA), a Gaussian glow, and a PNG writer (stored deflate blocks: no zlib in the runtime). Checked against the Python
// tool's output on 2026-11-23: the same 57 file names, mean differences of 0.1-3 out of 255 per picture.
#include "button_icons.h"

#include "runtime_config.h"
#include "runtime_log.h"
#include "virtual_remote.h"

#include <aurora/aurora.h>
#include <xxhash.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <imstb_truetype.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ButtonIcons {
namespace {
namespace fs = std::filesystem;

// Bump when the pictures change: every set is drawn again at the next start.
constexpr int kDrawingVersion = 1;

// ---- Pictures ----------------------------------------------------------------------------------------------------

// A picture of 1 (grey) or 4 (straight RGBA) channels, 0..255 as floats.
struct Image {
    int w = 0, h = 0, ch = 4;
    std::vector<float> px;
    Image() = default;
    Image(int width, int height, int channels, float fill = 0.f)
        : w(width), h(height), ch(channels), px(size_t(width) * size_t(height) * size_t(channels), fill) {}
    float* at(int x, int y) { return &px[(size_t(y) * size_t(w) + size_t(x)) * size_t(ch)]; }
    const float* at(int x, int y) const { return &px[(size_t(y) * size_t(w) + size_t(x)) * size_t(ch)]; }
};
using Colour = std::array<float, 4>;  // a grey picture uses [0]

// Pillow's drawing replaces pixels (no blending); its pixel centres are the integer coordinates.
template <class Inside>
void Fill(Image& img, double x0, double y0, double x1, double y1, const Colour& c, Inside inside) {
    const int ix0 = std::max(0, int(std::floor(x0)) - 1), iy0 = std::max(0, int(std::floor(y0)) - 1);
    const int ix1 = std::min(img.w - 1, int(std::ceil(x1)) + 1), iy1 = std::min(img.h - 1, int(std::ceil(y1)) + 1);
    for (int y = iy0; y <= iy1; ++y)
        for (int x = ix0; x <= ix1; ++x)
            if (inside(double(x), double(y))) std::copy_n(c.begin(), img.ch, img.at(x, y));
}

bool InEllipse(double x, double y, double x0, double y0, double x1, double y1) {
    const double rx = (x1 - x0) / 2, ry = (y1 - y0) / 2;
    if (rx <= 0 || ry <= 0) return false;
    const double dx = (x - (x0 + x1) / 2) / rx, dy = (y - (y0 + y1) / 2) / ry;
    return dx * dx + dy * dy <= 1.0;
}

void FillEllipse(Image& img, double x0, double y0, double x1, double y1, const Colour& c) {
    Fill(img, x0, y0, x1, y1, c, [&](double x, double y) { return InEllipse(x, y, x0, y0, x1, y1); });
}

// Outlines are `width` pixels inside the box, as Pillow draws them.
void EllipseOutline(Image& img, double x0, double y0, double x1, double y1, double width, const Colour& c) {
    Fill(img, x0, y0, x1, y1, c, [&](double x, double y) {
        return InEllipse(x, y, x0, y0, x1, y1) && !InEllipse(x, y, x0 + width, y0 + width, x1 - width, y1 - width);
    });
}

void FillRect(Image& img, double x0, double y0, double x1, double y1, const Colour& c) {
    Fill(img, x0, y0, x1, y1, c, [&](double x, double y) { return x >= x0 && x <= x1 && y >= y0 && y <= y1; });
}

void RectOutline(Image& img, double x0, double y0, double x1, double y1, double width, const Colour& c) {
    Fill(img, x0, y0, x1, y1, c, [&](double x, double y) {
        const bool outer = x >= x0 && x <= x1 && y >= y0 && y <= y1;
        const bool inner = x >= x0 + width && x <= x1 - width && y >= y0 + width && y <= y1 - width;
        return outer && !inner;
    });
}

bool InRoundedRect(double x, double y, double x0, double y0, double x1, double y1, double r) {
    if (x < x0 || x > x1 || y < y0 || y > y1) return false;
    r = std::max(0.0, std::min(r, std::min(x1 - x0, y1 - y0) / 2));
    const double cx = std::clamp(x, x0 + r, x1 - r), cy = std::clamp(y, y0 + r, y1 - r);
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
}

void RoundedRectOutline(Image& img, double x0, double y0, double x1, double y1, double radius, double width,
                        const Colour& c) {
    Fill(img, x0, y0, x1, y1, c, [&](double x, double y) {
        return InRoundedRect(x, y, x0, y0, x1, y1, radius) &&
               !InRoundedRect(x, y, x0 + width, y0 + width, x1 - width, y1 - width, radius - width);
    });
}

// A wide line with flat ends (Pillow's polygon around the segment).
void Line(Image& img, double x1, double y1, double x2, double y2, double width, const Colour& c) {
    const double dx = x2 - x1, dy = y2 - y1, len = std::hypot(dx, dy), half = width / 2;
    if (len <= 0) return;
    Fill(img, std::min(x1, x2) - half, std::min(y1, y2) - half, std::max(x1, x2) + half, std::max(y1, y2) + half, c,
         [&](double x, double y) {
             const double t = ((x - x1) * dx + (y - y1) * dy) / len;
             const double d = std::abs((x - x1) * dy - (y - y1) * dx) / len;
             return t >= 0 && t <= len && d <= half;
         });
}

void Disc(Image& img, double cx, double cy, double r, const Colour& c) {
    FillEllipse(img, cx - r, cy - r, cx + r, cy + r, c);
}

// ---- Letters (stb_truetype) ----------------------------------------------------------------------------------------

struct Font {
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
    bool ok = false;
    std::string path;
};

std::vector<unsigned char> ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)), {});
}

// A bold sans font: Windows, Debian / Ubuntu, Arch / SteamOS, Fedora, macOS, then whatever fontconfig offers.
const Font& BoldFont() {
    static Font font;
    static std::once_flag once;
    std::call_once(once, [] {
        std::vector<fs::path> candidates;
#if defined(_WIN32)
        const char* windir = std::getenv("WINDIR");
        const fs::path fonts = fs::path(windir != nullptr ? windir : "C:\\Windows") / "Fonts";
        candidates = {fonts / "arialbd.ttf", fonts / "segoeuib.ttf"};
#else
        candidates = {"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
                      "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf", "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
                      "/System/Library/Fonts/Supplemental/Arial Bold.ttf", "/Library/Fonts/Arial Bold.ttf"};
        if (FILE* pipe = popen("fc-match -f '%{file}' sans:bold 2>/dev/null", "r")) {
            char buffer[1024] = {};
            const size_t n = std::fread(buffer, 1, sizeof buffer - 1, pipe);
            pclose(pipe);
            if (n > 0) candidates.emplace_back(std::string(buffer, n));
        }
#endif
        std::error_code ec;
        for (const auto& path : candidates) {
            if (!fs::is_regular_file(path, ec)) continue;
            font.data = ReadAll(path);
            const int offset = font.data.empty() ? -1 : stbtt_GetFontOffsetForIndex(font.data.data(), 0);
            if (offset >= 0 && stbtt_InitFont(&font.info, font.data.data(), offset)) {
                font.ok = true;
                font.path = RuntimeConfigFile::PathToUtf8(path);
                return;
            }
        }
        font.data.clear();
    });
    return font;
}

// `text` centred on (cx, cy) as Pillow's anchor "mm" places it (half the advance width; the middle between ascender
// and descender), `stroke` pixels of outline around each letter in the same colour.
bool DrawText(Image& img, double cx, double cy, const std::string& text, int size, const Colour& c, int stroke) {
    const Font& font = BoldFont();
    if (!font.ok) return false;
    const float scale = stbtt_ScaleForMappingEmToPixels(&font.info, float(size));
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&font.info, &ascent, &descent, &gap);
    double advance = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&font.info, text[i], &adv, &lsb);
        advance += adv * scale;
        if (i + 1 < text.size()) advance += scale * stbtt_GetCodepointKernAdvance(&font.info, text[i], text[i + 1]);
    }
    double penX = cx - advance / 2;
    const double baseline = cy + (ascent + descent) * scale / 2.0;
    std::vector<uint8_t> mask(size_t(img.w) * size_t(img.h), 0);
    for (size_t i = 0; i < text.size(); ++i) {
        const int ox = int(std::floor(penX)), oy = int(std::floor(baseline));
        const float sx = float(penX - ox), sy = float(baseline - oy);
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetCodepointBitmapBoxSubpixel(&font.info, text[i], scale, scale, sx, sy, &x0, &y0, &x1, &y1);
        const int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            std::vector<unsigned char> glyph(static_cast<size_t>(gw) * size_t(gh));
            stbtt_MakeCodepointBitmapSubpixel(&font.info, glyph.data(), gw, gh, gw, scale, scale, sx, sy, text[i]);
            for (int y = 0; y < gh; ++y)
                for (int x = 0; x < gw; ++x) {
                    const int X = ox + x0 + x, Y = oy + y0 + y;
                    if (X >= 0 && Y >= 0 && X < img.w && Y < img.h && glyph[size_t(y) * gw + x] >= 128)
                        mask[size_t(Y) * img.w + X] = 1;
                }
        }
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&font.info, text[i], &adv, &lsb);
        penX += adv * scale;
        if (i + 1 < text.size()) penX += scale * stbtt_GetCodepointKernAdvance(&font.info, text[i], text[i + 1]);
    }
    if (stroke > 0) {  // a disc of `stroke` around every edge pixel
        std::vector<uint8_t> grown = mask;
        for (int y = 0; y < img.h; ++y)
            for (int x = 0; x < img.w; ++x) {
                if (!mask[size_t(y) * img.w + x]) continue;
                const bool edge = x == 0 || y == 0 || x == img.w - 1 || y == img.h - 1 || !mask[size_t(y) * img.w + x - 1] ||
                                  !mask[size_t(y) * img.w + x + 1] || !mask[size_t(y - 1) * img.w + x] ||
                                  !mask[size_t(y + 1) * img.w + x];
                if (!edge) continue;
                for (int dy = -stroke; dy <= stroke; ++dy)
                    for (int dx = -stroke; dx <= stroke; ++dx) {
                        const int X = x + dx, Y = y + dy;
                        if (dx * dx + dy * dy <= stroke * stroke && X >= 0 && Y >= 0 && X < img.w && Y < img.h)
                            grown[size_t(Y) * img.w + X] = 1;
                    }
            }
        mask.swap(grown);
    }
    for (int y = 0; y < img.h; ++y)
        for (int x = 0; x < img.w; ++x)
            if (mask[size_t(y) * img.w + x]) std::copy_n(c.begin(), img.ch, img.at(x, y));
    return true;
}

// A pad label centred at (cx, cy) inside radius r: letters, PlayStation shapes, or the menu / view / options / create
// and + / - marks. `outline` draws a halo first (the badges' white-edged black glyphs). False when the label needs
// letters and no font was found.
bool DrawSymbol(Image& d, const std::string& label, double cx, double cy, double r, const Colour& colour,
                const Colour* outline = nullptr, int outlineWidth = 0) {
    std::vector<std::pair<Colour, int>> passes;
    if (outline != nullptr) passes.emplace_back(*outline, outlineWidth);
    passes.emplace_back(colour, 0);
    for (const auto& [fill, extra] : passes) {
        const int w = std::max(2, int(r * 0.22)) + 2 * extra;
        if (label == "cross") {
            const double k = r * 0.62;
            Line(d, cx - k, cy - k, cx + k, cy + k, w, fill);
            Line(d, cx - k, cy + k, cx + k, cy - k, w, fill);
        } else if (label == "circle") {
            const double k = r * 0.62;
            EllipseOutline(d, cx - k - extra, cy - k - extra, cx + k + extra, cy + k + extra, w, fill);
        } else if (label == "square") {
            const double k = r * 0.56;
            RectOutline(d, cx - k - extra, cy - k - extra, cx + k + extra, cy + k + extra, w, fill);
        } else if (label == "triangle") {
            const double k = r * 0.68;
            const std::array<std::pair<double, double>, 3> pts{
                {{cx, cy - k}, {cx + k * 0.95, cy + k * 0.62}, {cx - k * 0.95, cy + k * 0.62}}};
            for (int i = 0; i < 3; ++i) {
                const auto& a = pts[i];
                const auto& b = pts[(i + 1) % 3];
                Line(d, a.first, a.second, b.first, b.second, w, fill);
                Disc(d, a.first, a.second, w / 2.0, fill);  // joint="curve"
            }
        } else if (label == "menu" || label == "options") {
            const double k = r * (label == "menu" ? 0.62 : 0.5);
            for (const double dy : {-0.42, 0.0, 0.42})
                Line(d, cx - k, cy + dy * r, cx + k, cy + dy * r, std::max(2, int(w * 0.8)), fill);
        } else if (label == "view" || label == "create") {
            const double k = r * 0.34;
            const int lw = std::max(2, w / 2 + extra);
            if (label == "view") {
                RectOutline(d, cx - k * 1.5, cy - k * 1.3, cx + k * 0.5, cy + k * 0.5, lw, fill);
                RectOutline(d, cx - k * 0.5, cy - k * 0.5, cx + k * 1.5, cy + k * 1.3, lw, fill);
            } else {  // Create: a capsule with three rays above it
                RoundedRectOutline(d, cx - k * 0.75, cy - k * 0.55, cx + k * 0.75, cy + k * 1.75, k * 0.75, lw, fill);
                for (const auto& [ax, ay, bx, by] : std::array<std::array<double, 4>, 3>{
                         {{0, -1.2, 0, -2.1}, {-0.8, -1.0, -1.45, -1.75}, {0.8, -1.0, 1.45, -1.75}}})
                    Line(d, cx + ax * k, cy + ay * k, cx + bx * k, cy + by * k, lw, fill);
            }
        } else if (label == "+" || label == "-") {
            const double k = r * 0.6;
            Line(d, cx - k, cy, cx + k, cy, w, fill);
            if (label == "+") Line(d, cx, cy - k, cx, cy + k, w, fill);
        } else {
            const int size = int(r * (label.size() == 1 ? 1.45 : 0.95));
            if (!DrawText(d, cx, cy + r * 0.04, label, size, fill, extra)) return false;
        }
    }
    return true;
}

// ---- Resampling, blur, compositing (as Pillow does them) ----------------------------------------------------------

double Lanczos(double x) {
    if (x == 0) return 1.0;
    if (x <= -3.0 || x >= 3.0) return 0.0;
    const double pi = 3.14159265358979323846;
    return 3.0 * std::sin(pi * x) * std::sin(pi * x / 3.0) / (pi * pi * x * x);
}

struct Taps {
    int first = 0;
    std::vector<double> weights;
};

// Pillow's precompute_coeffs for one axis.
std::vector<Taps> Coefficients(int inSize, int outSize) {
    const double scale = double(inSize) / outSize, filterScale = std::max(scale, 1.0), support = 3.0 * filterScale;
    std::vector<Taps> out(static_cast<size_t>(outSize));
    for (int i = 0; i < outSize; ++i) {
        const double centre = (i + 0.5) * scale;
        const int xmin = std::max(int(centre - support + 0.5), 0), xmax = std::min(int(centre + support + 0.5), inSize);
        Taps& t = out[size_t(i)];
        t.first = xmin;
        double sum = 0;
        for (int x = xmin; x < xmax; ++x) {
            const double w = Lanczos((x - centre + 0.5) / filterScale);
            t.weights.push_back(w);
            sum += w;
        }
        if (sum != 0)
            for (double& w : t.weights) w /= sum;
    }
    return out;
}

// Lanczos resize; RGBA goes through premultiplied alpha (Pillow converts RGBA to RGBa to resample).
Image Resize(const Image& src, int ow, int oh) {
    Image in = src;
    if (in.ch == 4)
        for (size_t i = 0; i < in.px.size(); i += 4)
            for (int k = 0; k < 3; ++k) in.px[i + k] = in.px[i + k] * in.px[i + 3] / 255.f;
    const auto hTaps = Coefficients(in.w, ow), vTaps = Coefficients(in.h, oh);
    Image mid(ow, in.h, in.ch);
    for (int y = 0; y < in.h; ++y)
        for (int x = 0; x < ow; ++x) {
            const Taps& t = hTaps[size_t(x)];
            float* o = mid.at(x, y);
            for (size_t j = 0; j < t.weights.size(); ++j) {
                const float* s = in.at(t.first + int(j), y);
                for (int k = 0; k < in.ch; ++k) o[k] += float(t.weights[j]) * s[k];
            }
        }
    Image out(ow, oh, in.ch);
    for (int y = 0; y < oh; ++y) {
        const Taps& t = vTaps[size_t(y)];
        for (int x = 0; x < ow; ++x) {
            float* o = out.at(x, y);
            for (size_t j = 0; j < t.weights.size(); ++j) {
                const float* s = mid.at(x, t.first + int(j));
                for (int k = 0; k < in.ch; ++k) o[k] += float(t.weights[j]) * s[k];
            }
        }
    }
    for (float& v : out.px) v = std::clamp(v, 0.f, 255.f);
    if (out.ch == 4)
        for (size_t i = 0; i < out.px.size(); i += 4) {
            const float a = out.px[i + 3];
            for (int k = 0; k < 3; ++k) out.px[i + k] = a > 0 ? std::min(255.f, out.px[i + k] * 255.f / a) : 0.f;
        }
    return out;
}

// Gaussian blur of every channel on its own (Pillow's GaussianBlur, sigma = radius), within the drawn part.
Image GaussianBlur(const Image& src, double sigma) {
    int x0 = src.w, y0 = src.h, x1 = -1, y1 = -1;
    for (int y = 0; y < src.h; ++y)
        for (int x = 0; x < src.w; ++x)
            if (src.at(x, y)[src.ch - 1] > 0) {
                x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
            }
    Image out(src.w, src.h, src.ch);
    if (x1 < 0) return out;
    const int radius = int(std::ceil(sigma * 3));
    std::vector<double> kernel(size_t(2 * radius + 1));
    double sum = 0;
    for (int i = -radius; i <= radius; ++i) sum += kernel[size_t(i + radius)] = std::exp(-0.5 * i * i / (sigma * sigma));
    for (double& k : kernel) k /= sum;
    x0 = std::max(0, x0 - radius), y0 = std::max(0, y0 - radius);
    x1 = std::min(src.w - 1, x1 + radius), y1 = std::min(src.h - 1, y1 + radius);
    Image mid(src.w, src.h, src.ch);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            float* o = mid.at(x, y);
            for (int i = -radius; i <= radius; ++i) {
                const int sx = std::clamp(x + i, 0, src.w - 1);
                const float* s = src.at(sx, y);
                for (int k = 0; k < src.ch; ++k) o[k] += float(kernel[size_t(i + radius)]) * s[k];
            }
        }
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            float* o = out.at(x, y);
            for (int i = -radius; i <= radius; ++i) {
                const int sy = std::clamp(y + i, 0, src.h - 1);
                const float* s = mid.at(x, sy);
                for (int k = 0; k < src.ch; ++k) o[k] += float(kernel[size_t(i + radius)]) * s[k];
            }
        }
    return out;
}

// Source over destination, both straight RGBA (Pillow's alpha_composite); `src` placed at (dx, dy).
void AlphaComposite(Image& dst, const Image& src, int dx = 0, int dy = 0) {
    for (int y = 0; y < src.h; ++y)
        for (int x = 0; x < src.w; ++x) {
            const int X = x + dx, Y = y + dy;
            if (X < 0 || Y < 0 || X >= dst.w || Y >= dst.h) continue;
            const float* s = src.at(x, y);
            float* d = dst.at(X, Y);
            const float sa = s[3] / 255.f, da = d[3] / 255.f, oa = sa + da * (1 - sa);
            for (int k = 0; k < 3; ++k) d[k] = oa > 0 ? (s[k] * sa + d[k] * da * (1 - sa)) / oa : 0.f;
            d[3] = oa * 255.f;
        }
}

// ---- The game's pictures (TPL, U8, GX texture formats) ------------------------------------------------------------

using Bytes = std::vector<uint8_t>;

uint32_t Be32(const Bytes& b, size_t at) {
    return at + 4 <= b.size() ? uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 | uint32_t(b[at + 2]) << 8 | b[at + 3] : 0;
}
uint16_t Be16(const Bytes& b, size_t at) { return at + 2 <= b.size() ? uint16_t(b[at] << 8 | b[at + 1]) : 0; }

struct Block {
    int bw, bh, bpp;
};
std::optional<Block> BlockOf(uint32_t fmt) {
    switch (fmt) {
    case 0: return Block{8, 8, 4};
    case 1: return Block{8, 4, 8};
    case 2: return Block{8, 4, 8};
    case 3: return Block{4, 4, 16};
    case 4: return Block{4, 4, 16};
    case 5: return Block{4, 4, 16};
    case 6: return Block{4, 4, 32};
    case 14: return Block{8, 8, 4};
    default: return std::nullopt;
    }
}

size_t BaseLevelSize(int w, int h, uint32_t fmt) {
    const Block b = *BlockOf(fmt);
    return size_t((w + b.bw - 1) / b.bw * b.bw) * size_t((h + b.bh - 1) / b.bh * b.bh) * size_t(b.bpp) / 8;
}

struct TplImage {
    int w, h;
    uint32_t fmt;
    size_t offset;
};

// The images of a TPL at `base`.
std::vector<TplImage> TplImages(const Bytes& blob, size_t base) {
    std::vector<TplImage> out;
    if (base + 12 > blob.size() || Be32(blob, base) != 0x0020AF30) return out;
    const uint32_t count = Be32(blob, base + 4), table = Be32(blob, base + 8);
    if (count == 0 || count > 256) return out;
    for (uint32_t i = 0; i < count; ++i) {
        const size_t entry = base + table + 8 * size_t(i);
        if (entry + 8 > blob.size()) return {};
        const size_t header = base + Be32(blob, entry);
        if (header + 12 > blob.size()) return {};
        const int h = Be16(blob, header), w = Be16(blob, header + 2);
        const uint32_t fmt = Be32(blob, header + 4);
        const size_t offset = base + Be32(blob, header + 8);
        if (BlockOf(fmt) && w > 0 && w <= 1024 && h > 0 && h <= 1024) out.push_back({w, h, fmt, offset});
    }
    return out;
}

// Every TPL in a file (layout archives keep theirs uncompressed): calls `each` with its images until it returns true.
template <class Each>
void ForEachTpl(const Bytes& blob, Each each) {
    static constexpr uint8_t kMagic[] = {0x00, 0x20, 0xAF, 0x30};
    for (auto it = blob.begin();;) {
        it = std::search(it, blob.end(), std::begin(kMagic), std::end(kMagic));
        if (it == blob.end()) return;
        const size_t at = size_t(it - blob.begin());
        for (const TplImage& img : TplImages(blob, at))
            if (each(img)) return;
        it += 4;
    }
}

bool BaseLevel(const Bytes& blob, const TplImage& img, const uint8_t*& data, size_t& size) {
    size = BaseLevelSize(img.w, img.h, img.fmt);
    if (img.offset + size > blob.size()) return false;
    data = blob.data() + img.offset;
    return true;
}

// aurora's replacement file name for a texture whose base level is `data` (texture_replacement.cpp).
std::string KeyName(const TplImage& img, const uint8_t* data, size_t size) {
    char name[96];
    std::snprintf(name, sizeof name, "tex1_%dx%d_%016llx_%u.png", img.w, img.h,
                  static_cast<unsigned long long>(XXH64(data, size, 0)), img.fmt);
    return name;
}

// GX texture -> RGBA. Formats 0 I4, 1 I8, 2 IA4, 3 IA8, 4 RGB565, 5 RGB5A3, 6 RGBA8 (others stay transparent).
Image Decode(const uint8_t* data, int w, int h, uint32_t fmt) {
    Image out(w, h, 4);
    const Block b = *BlockOf(fmt);
    size_t pos = 0;
    const auto put = [&](int x, int y, float r, float g, float bl, float a) {
        if (x < w && y < h) {
            float* p = out.at(x, y);
            p[0] = r, p[1] = g, p[2] = bl, p[3] = a;
        }
    };
    for (int by = 0; by < h; by += b.bh)
        for (int bx = 0; bx < w; bx += b.bw) {
            if (fmt == 6) {
                const uint8_t* blk = data + pos;
                pos += 64;
                for (int i = 0; i < 16; ++i)
                    put(bx + i % 4, by + i / 4, blk[2 * i + 1], blk[32 + 2 * i], blk[33 + 2 * i], blk[2 * i]);
                continue;
            }
            for (int i = 0; i < b.bw * b.bh; ++i) {
                const int x = bx + i % b.bw, y = by + i / b.bw;
                if (fmt == 0) {
                    int v = data[pos + size_t(i / 2)];
                    v = (i % 2 == 0 ? v >> 4 : v & 15) * 17;
                    put(x, y, v, v, v, v);
                } else if (fmt == 1) {
                    const int v = data[pos + size_t(i)];
                    put(x, y, v, v, v, v);
                } else if (fmt == 2) {
                    const int v = data[pos + size_t(i)], a = (v >> 4) * 17, l = (v & 15) * 17;
                    put(x, y, l, l, l, a);
                } else if (fmt == 3) {
                    const int a = data[pos + 2 * size_t(i)], l = data[pos + 2 * size_t(i) + 1];
                    put(x, y, l, l, l, a);
                } else if (fmt == 4) {
                    const int v = data[pos + 2 * size_t(i)] << 8 | data[pos + 2 * size_t(i) + 1];
                    put(x, y, ((v >> 11) & 31) * 255 / 31, ((v >> 5) & 63) * 255 / 63, (v & 31) * 255 / 31, 255);
                } else if (fmt == 5) {
                    const int v = data[pos + 2 * size_t(i)] << 8 | data[pos + 2 * size_t(i) + 1];
                    if (v & 0x8000)
                        put(x, y, ((v >> 10) & 31) * 255 / 31, ((v >> 5) & 31) * 255 / 31, (v & 31) * 255 / 31, 255);
                    else
                        put(x, y, ((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17, ((v >> 12) & 7) * 255 / 7);
                }
            }
            pos += size_t(b.bw * b.bh * b.bpp / 8);
        }
    return out;
}

// (name, data offset) of the files in a U8 archive.
std::vector<std::pair<std::string, size_t>> U8Entries(const Bytes& blob) {
    std::vector<std::pair<std::string, size_t>> out;
    const size_t root = Be32(blob, 4);
    const uint32_t count = Be32(blob, root + 8);
    const size_t names = root + size_t(count) * 12;
    if (count == 0 || names > blob.size()) return out;
    for (uint32_t i = 1; i < count; ++i) {
        const size_t entry = root + size_t(i) * 12;
        if (entry + 8 > blob.size()) break;
        const size_t nameAt = names + (size_t(blob[entry + 1]) << 16 | size_t(blob[entry + 2]) << 8 | blob[entry + 3]);
        if (blob[entry] != 0 || nameAt >= blob.size()) continue;
        const auto end = std::find(blob.begin() + long(nameAt), blob.end(), uint8_t(0));
        out.emplace_back(std::string(blob.begin() + long(nameAt), end), Be32(blob, entry + 4));
    }
    return out;
}

// ---- The three kinds of picture -----------------------------------------------------------------------------------

// The message-text icon: the game's white disc with a dark rim, carrying `label`.
std::optional<Image> TextIcon(const std::string& label, const Colour& colour, int size = 92) {
    const int s = 4;
    Image big(size * s, size * s, 4);
    const double c = size * s / 2.0, outer = 0.49 * size * s, inner = 0.41 * size * s;
    FillEllipse(big, c - outer, c - outer, c + outer, c + outer, {35, 35, 35, 255});
    FillEllipse(big, c - inner, c - inner, c + inner, c + inner, {255, 255, 255, 255});
    if (!DrawSymbol(big, label, c, c, 0.33 * size * s, colour)) return std::nullopt;
    return Resize(big, size, size);
}

// The 24x18 menu badge (intensity; the menus tint it): transparent edge, white frame, light grey inside, a black label
// with a white edge.
std::optional<Image> Badge(const std::string& label, int scale = 4) {
    const int w = 24 * scale, h = 18 * scale, s = 4, e = scale * s;
    Image big(w * s, h * s, 1);
    FillRect(big, e, e, w * s - e - 1, h * s - e - 1, {255});
    FillRect(big, 3 * e, 3 * e, w * s - 3 * e - 1, h * s - 3 * e - 1, {0xBF});
    const Colour white{255};
    if (!DrawSymbol(big, label, w * s / 2.0, h * s / 2.0, 8.4 * e, {0}, &white, int(e * 0.9))) return std::nullopt;
    return Resize(big, w, h);
}

// A pause-frame tab (I8) with its digit replaced by `label`: the digit is a transparent hole with a white edge inside
// the grey tab, so the holes (dark pixels not reached from the picture's border) and everything within 3 px of them,
// short of the frame, are painted the tab's grey, and the label is drawn there in the badges' style.
std::optional<Image> TabPicture(const Image& px, const std::string& label, int scale = 4) {
    const int w = px.w, h = px.h, n = w * h;
    std::vector<int> lum(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) lum[size_t(i)] = px.px[size_t(i) * 4 + 3] != 0 ? int(px.px[size_t(i) * 4]) : 0;
    std::vector<char> dark(static_cast<size_t>(n)), outside(static_cast<size_t>(n), 0);
    for (int i = 0; i < n; ++i) dark[size_t(i)] = lum[size_t(i)] < 120;
    std::deque<int> queue;
    for (int i = 0; i < n; ++i)
        if (dark[size_t(i)] && (i % w == 0 || i % w == w - 1 || i / w == 0 || i / w == h - 1)) {
            outside[size_t(i)] = 1;
            queue.push_back(i);
        }
    while (!queue.empty()) {
        const int i = queue.front();
        queue.pop_front();
        const int x = i % w, y = i / w;
        for (const auto& [nx, ny] : std::array<std::pair<int, int>, 4>{{{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}}}) {
            const int j = ny * w + nx;
            if (nx >= 0 && nx < w && ny >= 0 && ny < h && !outside[size_t(j)] && dark[size_t(j)]) {
                outside[size_t(j)] = 1;
                queue.push_back(j);
            }
        }
    }
    std::vector<int> holes;
    for (int i = 0; i < n; ++i)
        if (dark[size_t(i)] && !outside[size_t(i)]) holes.push_back(i);
    if (holes.empty()) return std::nullopt;
    const auto nearby = [&](const std::vector<int>& cells, int radius) {
        std::vector<char> out(static_cast<size_t>(n), 0);
        for (const int i : cells) {
            const int x = i % w, y = i / w;
            for (int dy = -radius; dy <= radius; ++dy)
                for (int dx = -radius; dx <= radius; ++dx)
                    if (x + dx >= 0 && x + dx < w && y + dy >= 0 && y + dy < h) out[size_t((y + dy) * w + x + dx)] = 1;
        }
        return out;
    };
    std::vector<int> outsideCells;
    for (int i = 0; i < n; ++i)
        if (outside[size_t(i)]) outsideCells.push_back(i);
    const auto frame = nearby(outsideCells, 2);
    // The tab's grey: the most common value strictly between 0 and 250 (the first seen wins a tie, as Counter does).
    std::array<int, 256> counts{};
    std::array<int, 256> firstSeen;
    firstSeen.fill(n);
    for (int i = 0; i < n; ++i) {
        const int v = lum[size_t(i)];
        if (v > 0 && v < 250) {
            ++counts[size_t(v)];
            firstSeen[size_t(v)] = std::min(firstSeen[size_t(v)], i);
        }
    }
    int grey = -1;
    for (int v = 1; v < 250; ++v)
        if (counts[size_t(v)] > 0 &&
            (grey < 0 || counts[size_t(v)] > counts[size_t(grey)] ||
             (counts[size_t(v)] == counts[size_t(grey)] && firstSeen[size_t(v)] < firstSeen[size_t(grey)])))
            grey = v;
    if (grey < 0) return std::nullopt;
    const auto erase = nearby(holes, 3);
    for (int i = 0; i < n; ++i)
        if (erase[size_t(i)] && !frame[size_t(i)]) lum[size_t(i)] = grey;
    int minX = w, maxX = -1, minY = h, maxY = -1;
    for (const int i : holes) {
        minX = std::min(minX, i % w), maxX = std::max(maxX, i % w);
        minY = std::min(minY, i / w), maxY = std::max(maxY, i / w);
    }
    const double cx = (minX + maxX + 1) / 2.0, cy = (minY + maxY + 1) / 2.0;
    Image grey8(w, h, 1);
    for (int i = 0; i < n; ++i) grey8.px[size_t(i)] = float(lum[size_t(i)]);
    const int s = 4, k = scale * s;
    Image big = Resize(grey8, w * k, h * k);
    const Colour white{255};
    if (!DrawSymbol(big, label, cx * k, cy * k, 7.0 * k, {0}, &white, int(k * 0.9))) return std::nullopt;
    return Resize(big, w * scale, h * scale);
}

int Lum100(const float* p) { return (int(p[0]) * 30 + int(p[1]) * 59 + int(p[2]) * 11) / 100; }
int Lum(const float* p) { return int(p[0]) * 30 + int(p[1]) * 59 + int(p[2]) * 11; }

// The (2) ring's centre and inner radius from its bright rim pixels near the estimate: the rim's centroid (each pass
// about the last), and the rim's nearest pixel minus a half pixel.
std::pair<std::pair<double, double>, double> RefineRing(const Image& px, std::pair<double, double> ring) {
    constexpr double kInnerFallback = 9.3;
    const int w = px.w, h = px.h;
    double cx = ring.first, cy = ring.second;
    int peak = 0;
    for (int y = std::max(0, int(cy) - 14); y < std::min(h, int(cy) + 15); ++y)
        for (int x = std::max(0, int(cx) - 14); x < std::min(w, int(cx) + 15); ++x) peak = std::max(peak, Lum100(px.at(x, y)));
    std::vector<std::pair<double, double>> pts;
    for (int pass = 0; pass < 3; ++pass) {
        pts.clear();
        for (int y = std::max(0, int(cy) - 14); y < std::min(h, int(cy) + 15); ++y)
            for (int x = std::max(0, int(cx) - 14); x < std::min(w, int(cx) + 15); ++x) {
                const double dist = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
                if (dist >= 8.5 && dist <= 13.5 && Lum100(px.at(x, y)) >= peak * 0.6) pts.emplace_back(x + 0.5, y + 0.5);
            }
        if (pts.empty()) break;
        double sx = 0, sy = 0;
        for (const auto& [x, y] : pts) sx += x, sy += y;
        cx = sx / double(pts.size()), cy = sy / double(pts.size());
    }
    double inner = kInnerFallback;
    if (!pts.empty()) {
        inner = 1e9;
        for (const auto& [x, y] : pts) inner = std::min(inner, std::hypot(x - cx, y - cy));
        inner -= 0.5;
    }
    return {{cx, cy}, inner};
}

// The title prompt with the 2 in its ring replaced by `label`, at `scale`x.
std::optional<Image> TitlePicture(const Image& px, std::pair<double, double> ringGuess, const std::string& label,
                                  int scale = 2) {
    const int w = px.w, h = px.h;
    Image out = Resize(px, w * scale, h * scale);
    const auto [centre, inner] = RefineRing(px, ringGuess);
    const auto [cx, cy] = centre;
    // The ring's own colour (its brightest pixels) and the dark fill inside it, sampled around the inner edge.
    std::vector<const float*> rim;
    for (int dy = -12; dy <= 12; ++dy)
        for (int dx = -12; dx <= 12; ++dx) {
            const double d = std::sqrt(double(dx * dx + dy * dy));
            const int x = int(cx + dx), y = int(cy + dy);
            if (d >= inner + 0.5 && d <= inner + 2.5 && x >= 0 && x < w && y >= 0 && y < h) rim.push_back(px.at(x, y));
        }
    std::vector<const float*> fill;
    for (int i = 0; i < 24; ++i) {
        const double a = i * 3.14159265358979323846 / 12;
        const int x = int(cx + (inner - 1.4) * std::cos(a)), y = int(cy + (inner - 1.4) * std::sin(a));
        if (x >= 0 && x < w && y >= 0 && y < h) fill.push_back(px.at(x, y));
    }
    if (rim.empty() || fill.empty()) return std::nullopt;
    const auto byLum = [](const float* a, const float* b) { return Lum(a) < Lum(b); };
    std::stable_sort(rim.begin(), rim.end(), byLum);
    std::stable_sort(fill.begin(), fill.end(), byLum);
    const float* ringPx = rim[rim.size() - std::max<size_t>(1, rim.size() / 8)];
    const float* fillPx = fill[fill.size() / 4];
    const Colour ringColour{ringPx[0], ringPx[1], ringPx[2], 255}, fillColour{fillPx[0], fillPx[1], fillPx[2], 255};
    // Drawn at 8x on the part around the ring only (the rest of the layer stays transparent), then shrunk onto `out`.
    const int s = 8, k = scale * s;
    const int margin = int(std::ceil((8.6 * k + 4 * 1.2 * k + 4 * s) / s));  // in `out` pixels: glyph, glow, filter
    const int ox0 = std::max(0, int(cx * scale) - margin), oy0 = std::max(0, int(cy * scale) - margin);
    const int ox1 = std::min(out.w, int(cx * scale) + margin), oy1 = std::min(out.h, int(cy * scale) + margin);
    const int lw = (ox1 - ox0) * s, lh = (oy1 - oy0) * s;
    const double lx = cx * k - ox0 * s, ly = cy * k - oy0 * s;  // the ring's centre on the layer
    Image layer(lw, lh, 4);
    const double r = (inner + 0.2) * k;
    FillEllipse(layer, lx - r, ly - r, lx + r, ly + r, fillColour);
    Image glyph(lw, lh, 4);
    if (!DrawSymbol(glyph, label, lx, ly, 8.6 * k, ringColour)) return std::nullopt;
    AlphaComposite(layer, GaussianBlur(glyph, 1.2 * k));
    AlphaComposite(layer, glyph);
    AlphaComposite(out, Resize(layer, ox1 - ox0, oy1 - oy0), ox0, oy0);
    return out;
}

// ---- PNG ------------------------------------------------------------------------------------------------------------

uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc = 0) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void PutBe32(Bytes& out, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(v >> s));
}

void Chunk(Bytes& out, const char* type, const Bytes& data) {
    PutBe32(out, uint32_t(data.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    PutBe32(out, Crc32(out.data() + start, out.size() - start));
}

// RGBA8 PNG (a grey picture is written as RGBA with every channel the grey, as Pillow merges the badges), deflate in
// stored blocks.
bool WritePng(const fs::path& path, const Image& img) {
    Bytes raw;
    raw.reserve(size_t(img.h) * (size_t(img.w) * 4 + 1));
    for (int y = 0; y < img.h; ++y) {
        raw.push_back(0);
        for (int x = 0; x < img.w; ++x) {
            const float* p = img.at(x, y);
            for (int k = 0; k < 4; ++k) {
                const float v = img.ch == 4 ? p[k] : p[0];
                raw.push_back(uint8_t(std::lround(std::clamp(v, 0.f, 255.f))));
            }
        }
    }
    Bytes z{0x78, 0x01};
    for (size_t at = 0; at < raw.size() || at == 0;) {
        const size_t n = std::min<size_t>(65535, raw.size() - at);
        z.push_back(at + n == raw.size() ? 1 : 0);
        z.push_back(uint8_t(n)), z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n)), z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), raw.begin() + long(at), raw.begin() + long(at + n));
        at += n;
        if (n == 0) break;
    }
    uint32_t a = 1, b = 0;
    for (const uint8_t v : raw) a = (a + v) % 65521, b = (b + a) % 65521;
    PutBe32(z, b << 16 | a);
    Bytes png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    Bytes ihdr;
    PutBe32(ihdr, uint32_t(img.w));
    PutBe32(ihdr, uint32_t(img.h));
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    Chunk(png, "IHDR", ihdr);
    Chunk(png, "IDAT", z);
    Chunk(png, "IEND", {});
    // Written beside it, then renamed: aurora may read the folder while this runs.
    fs::path temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
        if (!out) return false;
    }
    std::error_code ec;
    fs::rename(temp, path, ec);
    if (ec) fs::remove(temp, ec);
    return !ec;
}

// ---- The profile and the sets ---------------------------------------------------------------------------------------

using Roles = std::map<std::string, std::string>;  // Wii button -> pad role

// Pad inputs (Dolphin XInput names, as Metroid.ini writes them) -> the role of the button on any pad.
const std::map<std::string, std::string>& PadRoles() {
    static const std::map<std::string, std::string> roles{
        {"Button A", "south"}, {"Button B", "east"},  {"Button X", "west"},   {"Button Y", "north"},
        {"Back", "back"},      {"Start", "start"},    {"Shoulder L", "lb"},   {"Shoulder R", "rb"},
        {"Trigger L", "lt"},   {"Trigger R", "rt"}};
    return roles;
}

struct Label {
    std::string text;
    Colour colour;
};
using Style = std::map<std::string, Label>;  // role -> label

const std::vector<std::pair<std::string, Style>>& Styles() {
    static const Colour dark{40, 40, 40, 255}, black{30, 30, 30, 255};
    static const std::vector<std::pair<std::string, Style>> styles{
        {"xbox",
         {{"south", {"A", {16, 150, 40, 255}}}, {"east", {"B", {210, 30, 40, 255}}}, {"west", {"X", {20, 100, 210, 255}}},
          {"north", {"Y", {215, 160, 0, 255}}}, {"back", {"view", dark}}, {"start", {"menu", dark}},
          {"lb", {"LB", dark}}, {"rb", {"RB", dark}}, {"lt", {"LT", dark}}, {"rt", {"RT", dark}}}},
        {"playstation",
         {{"south", {"cross", {60, 110, 220, 255}}}, {"east", {"circle", {220, 50, 70, 255}}},
          {"west", {"square", {205, 80, 175, 255}}}, {"north", {"triangle", {20, 160, 130, 255}}},
          {"back", {"create", dark}}, {"start", {"options", dark}}, {"lb", {"L1", dark}}, {"rb", {"R1", dark}},
          {"lt", {"L2", dark}}, {"rt", {"R2", dark}}}},
        {"nintendo",
         {{"south", {"B", black}}, {"east", {"A", black}}, {"west", {"Y", black}}, {"north", {"X", black}},
          {"back", {"-", black}}, {"start", {"+", black}}, {"lb", {"L", black}}, {"rb", {"R", black}},
          {"lt", {"ZL", black}}, {"rt", {"ZR", black}}}},
    };
    return styles;
}

// The top-level `|` alternatives of a Dolphin expression (not inside parentheses or backticks).
std::vector<std::string> SplitAlternatives(const std::string& expr) {
    std::vector<std::string> parts;
    std::string buf;
    int depth = 0;
    bool tick = false;
    for (const char ch : expr) {
        if (ch == '`') tick = !tick;
        else if (!tick && ch == '(') ++depth;
        else if (!tick && ch == ')') --depth;
        else if (!tick && depth == 0 && ch == '|') {
            parts.push_back(buf);
            buf.clear();
            continue;
        }
        buf += ch;
    }
    parts.push_back(buf);
    return parts;
}

// Wii button -> role, in play and in the game's menus, from the profile's Buttons/* lines. `$menu` is 1 while one of
// the game's menus is up, so `Button X` & !$menu | `Button B` & $menu names X in play and B in menus: the first
// alternative whose $menu condition holds gives the button.
std::pair<Roles, Roles> ReadProfile(const std::string& text) {
    static const std::regex line(R"(^\s*Buttons/(\S+)\s*=\s*(.*?)\s*$)");
    static const std::regex notMenu(R"(!\s*\$menu\b)"), menu(R"(\$menu\b)");
    static const std::regex names(R"(`([^`]+)`|\b(Back|Start)\b)");
    Roles play, inMenu;
    std::istringstream in(text);
    for (std::string row; std::getline(in, row);) {
        if (!row.empty() && row.back() == '\r') row.pop_back();
        std::smatch m;
        if (!std::regex_match(row, m, line)) continue;
        const std::string button = m[1];
        for (const std::string& term : SplitAlternatives(m[2])) {
            const int needs = std::regex_search(term, notMenu) ? 0 : std::regex_search(term, menu) ? 1 : -1;
            std::string name;
            for (auto it = std::sregex_iterator(term.begin(), term.end(), names); it != std::sregex_iterator(); ++it) {
                const std::string found = (*it)[1].matched ? (*it)[1].str() : (*it)[2].str();
                if (PadRoles().count(found)) {
                    name = found;
                    break;
                }
            }
            if (name.empty()) continue;
            if (needs != 1) play.emplace(button, PadRoles().at(name));
            if (needs != 0) inMenu.emplace(button, PadRoles().at(name));
        }
    }
    return {play, inMenu};
}

enum class Kind { Text, Badge, Tab, Title };
struct Source {
    std::string key;
    Kind kind;
    std::string button;
    Image picture;                    // tabs and titles: the game's own picture
    std::pair<double, double> ring{};  // titles: the rough centre of the (2) ring
};

// Finds the game's pictures that name a Wii Remote button (the files are named in the comments below).
std::vector<Source> FindSources(const fs::path& files, std::string& problems) {
    std::vector<Source> sources;
    const auto read = [&](const fs::path& path) {
        Bytes blob = ReadAll(path);
        if (blob.empty()) problems += " missing " + RuntimeConfigFile::PathToUtf8(path.filename()) + ";";
        return blob;
    };
    // Message-text icons: files/layout/icon/<file>.
    static const std::array<std::pair<const char*, const char*>, 6> kTextIcons{
        {{"one.tpl", "1"}, {"two.tpl", "2"}, {"a.tpl", "A"}, {"b.tpl", "B"}, {"plus.tpl", "+"}, {"minus.tpl", "-"}}};
    for (const auto& [file, button] : kTextIcons) {
        const Bytes blob = read(files / "layout" / "icon" / file);
        const auto images = TplImages(blob, 0);
        const uint8_t* data = nullptr;
        size_t size = 0;
        if (!images.empty() && BaseLevel(blob, images[0], data, size))
            sources.push_back({KeyName(images[0], data, size), Kind::Text, button, {}, {}});
    }
    // Menu badges (24x18 I8) by key, found in the menu layout archives.
    static const std::map<std::string, std::string> kBadges{{"tex1_24x18_1d0f9811dc109db8_1.png", "1"},
                                                            {"tex1_24x18_194e7c1d2c44c12a_1.png", "2"},
                                                            {"tex1_24x18_408928752dd4687b_1.png", "A"},
                                                            {"tex1_24x18_af6f6b6b3d8ed1a8_1.png", "B"},
                                                            {"tex1_24x18_ff8e6849309ba3d1_1.png", "-"}};
    std::map<std::string, std::string> foundBadges;
    for (const char* hint : {"3175", "3384", "3458"}) {
        const Bytes blob = read(files / hint);
        ForEachTpl(blob, [&](const TplImage& img) {
            const uint8_t* data = nullptr;
            size_t size = 0;
            if (img.w == 24 && img.h == 18 && img.fmt == 1 && BaseLevel(blob, img, data, size)) {
                const std::string key = KeyName(img, data, size);
                if (const auto it = kBadges.find(key); it != kBadges.end()) foundBadges.emplace(key, it->second);
            }
            return false;
        });
    }
    for (const auto& [key, button] : kBadges)
        if (!foundBadges.count(key)) problems += " badge " + key + " not on this disc;";
    for (const auto& [key, button] : foundBadges) sources.push_back({key, Kind::Badge, button, {}, {}});
    // The pause frame's tabs (menu_back.brlyt's archive, file 3548).
    {
        static const std::map<std::string, std::string> kTabs{
            {"button_1.tpl", "1"}, {"button_2.tpl", "2"}, {"button_a.tpl", "A"}, {"button_b.tpl", "B"}};
        const Bytes blob = read(files / "3548");
        int tabs = 0;
        for (const auto& [name, offset] : blob.empty() ? std::vector<std::pair<std::string, size_t>>{} : U8Entries(blob)) {
            const auto it = kTabs.find(name);
            if (it == kTabs.end()) continue;
            const auto images = TplImages(blob, offset);
            const uint8_t* data = nullptr;
            size_t size = 0;
            if (images.empty() || !BaseLevel(blob, images[0], data, size)) continue;
            sources.push_back({KeyName(images[0], data, size), Kind::Tab, it->second,
                               Decode(data, images[0].w, images[0].h, images[0].fmt), {}});
            ++tabs;
        }
        if (tabs != int(kTabs.size())) problems += " " + std::to_string(tabs) + " of 4 pause-frame tabs;";
    }
    // The title's "PLEASE PRESS (2)" in English: 268x44 RGB565 (file 3492) and the widescreen title's 256x34 (3676).
    struct TitleSource {
        const char* file;
        int w, h;
        std::pair<double, double> ring;
    };
    for (const TitleSource& t : {TitleSource{"3492", 268, 44, {241.5, 21.0}}, TitleSource{"3676", 256, 34, {238.5, 17.0}}}) {
        const Bytes blob = read(files / t.file);
        bool found = false;
        ForEachTpl(blob, [&](const TplImage& img) {
            const uint8_t* data = nullptr;
            size_t size = 0;
            if (img.w != t.w || img.h != t.h || img.fmt != 4 || !BaseLevel(blob, img, data, size)) return false;
            sources.push_back({KeyName(img, data, size), Kind::Title, "2", Decode(data, img.w, img.h, img.fmt), t.ring});
            found = true;
            return true;
        });
        if (!found) problems += std::string(" no title prompt in file ") + t.file + ";";
    }
    return sources;
}

std::string Stamp(const Roles& play, const Roles& menu, bool font) {
    std::string stamp = "button_icons " + std::to_string(kDrawingVersion) + "\nplay";
    for (const auto& [button, role] : play) stamp += " " + button + "=" + role;
    stamp += "\nmenu";
    for (const auto& [button, role] : menu) stamp += " " + button + "=" + role;
    stamp += font ? "\nletters\n" : "\nno letters\n";
    return stamp;
}

void Draw(bool force) {
    const auto started = std::chrono::steady_clock::now();
    const fs::path files = RuntimeConfigFile::ResolvedDvdRoot() / "files";
    std::error_code ec;
    if (!fs::is_regular_file(files / "layout" / "icon" / "one.tpl", ec)) {
        RT_LOGF(RT_TAG_RUNTIME, "[icons] not drawn: the game's files are not set up\n");
        return;
    }
    const auto [play, menu] = ReadProfile(VirtualRemote::ProfileText());
    const Font& font = BoldFont();
    const fs::path out = RuntimeConfigFile::ApplicationDataDirectory() / "button_icons";
    const std::string stamp = Stamp(play, menu, font.ok);
    if (!force) {
        std::ifstream in(out / "drawn.txt", std::ios::binary);
        const std::string old((std::istreambuf_iterator<char>(in)), {});
        bool complete = old == stamp;
        for (const auto& [style, labels] : Styles()) complete = complete && fs::is_directory(out / style, ec);
        if (complete) {
            RT_LOGF(RT_TAG_RUNTIME, "[icons] up to date for these bindings\n");
            return;
        }
    }
    if (!font.ok) RT_LOGF(RT_TAG_RUNTIME, "[icons] no bold sans font found: prompts with letters keep the Wii's\n");
    std::string problems;
    const std::vector<Source> sources = FindSources(files, problems);
    if (!problems.empty()) RT_LOGF(RT_TAG_RUNTIME, "[icons] warning:%s\n", problems.c_str());
    static const std::array<const char*, 2> kMenuBadges{"1", "B"};  // a menu's back legend: marker7, "a menu is up"
    std::string summary;
    int total = 0;
    for (const auto& [style, labels] : Styles()) {
        const fs::path dir = out / style, menuDir = dir / "menu", markerDir = dir / "marker7";
        for (const fs::path& d : {dir, menuDir, markerDir}) {
            fs::create_directories(d, ec);
            std::vector<fs::path> old;
            for (const auto& entry : fs::directory_iterator(d, ec)) {
                const std::string name = RuntimeConfigFile::PathToUtf8(entry.path().filename());
                if (name.rfind("tex1_", 0) == 0 && name.size() > 4 && name.compare(name.size() - 4, 4, ".png") == 0)
                    old.push_back(entry.path());
            }
            for (const fs::path& path : old) fs::remove(path, ec);
            ec.clear();
        }
        int written = 0;
        const auto draw = [&](const Source& src, const Label& label) -> std::optional<Image> {
            switch (src.kind) {
            case Kind::Text: return TextIcon(label.text, label.colour);
            case Kind::Badge: return Badge(label.text);
            case Kind::Tab: return TabPicture(src.picture, label.text);
            case Kind::Title: return TitlePicture(src.picture, src.ring, label.text);
            }
            return std::nullopt;
        };
        for (const Source& src : sources) {
            // Badges and tabs exist only in the menus, so they show the menu buttons; message icons appear in both and
            // get a menu/ picture where the button differs there; the title is not a menu.
            const bool menuOnly = src.kind == Kind::Badge || src.kind == Kind::Tab;
            const Roles& roles = menuOnly ? menu : play;
            const auto role = roles.find(src.button);
            const Label* label = nullptr;
            if (role != roles.end())
                if (const auto it = labels.find(role->second); it != labels.end()) label = &it->second;
            if (label != nullptr) {
                const bool marker = src.kind == Kind::Tab ||
                                    (src.kind == Kind::Badge &&
                                     std::find(kMenuBadges.begin(), kMenuBadges.end(), src.button) != kMenuBadges.end());
                if (const auto img = draw(src, *label); img && WritePng((marker ? markerDir : dir) / src.key, *img))
                    ++written;
            }
            const auto menuRole = menu.find(src.button);
            if (src.kind == Kind::Text && menuRole != menu.end() && (role == roles.end() || menuRole->second != role->second))
                if (const auto it = labels.find(menuRole->second); it != labels.end())
                    if (const auto img = draw(src, it->second); img && WritePng(menuDir / src.key, *img)) ++written;
        }
        summary += " " + style + " " + std::to_string(written);
        total += written;
    }
    {
        std::ofstream stampFile(out / "drawn.txt", std::ios::binary | std::ios::trunc);
        stampFile << stamp;
    }
    aurora_reload_button_icons();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    RT_LOGF(RT_TAG_RUNTIME, "[icons] drew %d pictures from the disc (%s) in %lld ms;%s\n", total,
            font.ok ? font.path.c_str() : "no font", static_cast<long long>(ms), summary.c_str());
}

std::mutex g_mutex;
bool g_running = false, g_again = false, g_againForce = false;

void Worker(bool force) {
    for (;;) {
        try {
            Draw(force);
        } catch (const std::exception& e) {
            RT_LOGF(RT_TAG_RUNTIME, "[icons] failed: %s\n", e.what());
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_again) {
            g_running = false;
            return;
        }
        force = g_againForce;
        g_again = g_againForce = false;
    }
}

} // namespace

void Refresh(bool force) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_running) {
        g_again = true;
        g_againForce = g_againForce || force;
        return;
    }
    g_running = true;
    std::thread(Worker, force).detach();
}

} // namespace ButtonIcons

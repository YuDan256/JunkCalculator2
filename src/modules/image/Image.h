#ifndef JC2_IMAGE_H
#define JC2_IMAGE_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdio>

namespace jc {

    struct Color {
        uint8_t r, g, b;

        static Color fromHex(const std::string& hex) {
            std::string s = hex;
            if (!s.empty() && s[0] == '#') s = s.substr(1);
            if (s.size() != 6) return { 0, 0, 0 };
            auto h = [](char c) -> uint8_t {
                if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
                if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
                if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
                return 0;
                };
            return {
                static_cast<uint8_t>(h(s[0]) * 16 + h(s[1])),
                static_cast<uint8_t>(h(s[2]) * 16 + h(s[3])),
                static_cast<uint8_t>(h(s[4]) * 16 + h(s[5]))
            };
        }

        static Color fromName(const std::string& name) {
            static const std::unordered_map<std::string, Color> colors = {
                {"black", {0,0,0}}, {"white", {255,255,255}}, {"red", {255,0,0}},
                {"green", {0,128,0}}, {"blue", {0,0,255}}, {"yellow", {255,255,0}},
                {"cyan", {0,255,255}}, {"magenta", {255,0,255}}, {"orange", {255,165,0}},
                {"purple", {128,0,128}}, {"pink", {255,192,203}}, {"gray", {128,128,128}},
                {"lime", {0,255,0}}, {"navy", {0,0,128}}, {"teal", {0,128,128}}
            };
            auto it = colors.find(name);
            if (it != colors.end()) return it->second;
            return fromHex(name);
        }

        static Color parse(const std::string& s) {
            if (!s.empty() && s[0] == '#') return fromHex(s);
            return fromName(s);
        }

        std::string toHex() const {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", r, g, b);
            return std::string(buf);
        }
    };

    // ★ IBM VGA 8x8 ASCII 硬编码基础字库 (ASCII 32~126)
    static const uint8_t font8x8[96][8] = {
        {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, {0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00}, {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00},
        {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00}, {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00}, {0x38,0x6C,0x68,0x76,0xDC,0xCC,0x76,0x00}, {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00},
        {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00},
        {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00},
        {0x3C,0x66,0xCE,0xD6,0xE6,0x66,0x3C,0x00}, {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, {0x3C,0x66,0x06,0x1C,0x30,0x60,0xFE,0x00}, {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00},
        {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x00}, {0xFE,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00}, {0x3C,0x60,0x7C,0x66,0x66,0x66,0x3C,0x00}, {0xFE,0x06,0x0C,0x18,0x30,0x30,0x30,0x00},
        {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00}, {0x3C,0x66,0x66,0x66,0x3E,0x06,0x3C,0x00}, {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30},
        {0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00}, {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00}, {0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x00}, {0x3C,0x66,0x06,0x0C,0x18,0x00,0x18,0x00},
        {0x3C,0x66,0xDE,0xDE,0xDE,0x60,0x3C,0x00}, {0x3C,0x66,0x66,0x7E,0x66,0x66,0x66,0x00}, {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00}, {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00},
        {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00}, {0xFE,0x60,0x60,0x7C,0x60,0x60,0xFE,0x00}, {0xFE,0x60,0x60,0x7C,0x60,0x60,0x60,0x00}, {0x3C,0x66,0x60,0x6E,0x66,0x66,0x3E,0x00},
        {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00}, {0x7E,0x18,0x18,0x18,0x18,0x18,0x7E,0x00}, {0x06,0x06,0x06,0x06,0x06,0x66,0x3C,0x00}, {0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00},
        {0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00}, {0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00}, {0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00}, {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00},
        {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00}, {0x3C,0x66,0x66,0x66,0x6A,0x6C,0x36,0x00}, {0x7C,0x66,0x66,0x7C,0x6C,0x66,0x66,0x00}, {0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00},
        {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, {0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, {0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00}, {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00},
        {0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00}, {0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00}, {0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00}, {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00},
        {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, {0x18,0x3C,0x66,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00},
        {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x3C,0x06,0x3E,0x66,0x3E,0x00}, {0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00}, {0x00,0x00,0x3C,0x60,0x60,0x60,0x3C,0x00},
        {0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00}, {0x00,0x00,0x3C,0x66,0x7E,0x60,0x3C,0x00}, {0x1C,0x30,0x7C,0x30,0x30,0x30,0x30,0x00}, {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x3C},
        {0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x00}, {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00}, {0x0C,0x00,0x1C,0x0C,0x0C,0x0C,0x0C,0x38}, {0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0x00},
        {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, {0x00,0x00,0x66,0x7F,0x7F,0x6B,0x63,0x00}, {0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x00}, {0x00,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00},
        {0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60}, {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x06}, {0x00,0x00,0x7C,0x60,0x60,0x60,0x60,0x00}, {0x00,0x00,0x3E,0x60,0x3C,0x06,0x7C,0x00},
        {0x30,0x30,0x7C,0x30,0x30,0x30,0x1C,0x00}, {0x00,0x00,0x66,0x66,0x66,0x66,0x3E,0x00}, {0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x00}, {0x00,0x00,0x63,0x6B,0x7F,0x3E,0x36,0x00},
        {0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00}, {0x00,0x00,0x66,0x66,0x66,0x3E,0x06,0x3C}, {0x00,0x00,0x7E,0x0C,0x18,0x30,0x7E,0x00}, {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00},
        {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18}, {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00}, {0x3A,0x6C,0x00,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}
    };

    class Image {
    private:
        int w, h;
        std::vector<uint8_t> pixels; // RGB

    public:
        Image() : w(0), h(0) {}
        Image(int width, int height, Color bg = { 255,255,255 })
            : w(width), h(height), pixels(width* height * 3) {
            if (w <= 0 || h <= 0) throw std::runtime_error("Image Error: Dimensions must be positive.");
            for (int i = 0; i < w * h; ++i) {
                pixels[i * 3] = bg.r;
                pixels[i * 3 + 1] = bg.g;
                pixels[i * 3 + 2] = bg.b;
            }
        }

        int width() const { return w; }
        int height() const { return h; }
        const std::vector<uint8_t>& getRawPixels() const { return pixels; }

        void setPixel(int x, int y, Color c) {
            if (x < 0 || x >= w || y < 0 || y >= h) return;
            int idx = (y * w + x) * 3;
            pixels[idx] = c.r; pixels[idx + 1] = c.g; pixels[idx + 2] = c.b;
        }

        // ★ 核心平滑混合器：用于实现抗锯齿 Alpha Blending
        void blendPixel(int x, int y, Color c, double alpha) {
            if (x < 0 || x >= w || y < 0 || y >= h) return;
            if (alpha <= 0.0) return;
            if (alpha >= 1.0) { setPixel(x, y, c); return; }
            int idx = (y * w + x) * 3;
            double inv = 1.0 - alpha;
            pixels[idx] = static_cast<uint8_t>(std::min(255.0, c.r * alpha + pixels[idx] * inv));
            pixels[idx + 1] = static_cast<uint8_t>(std::min(255.0, c.g * alpha + pixels[idx + 1] * inv));
            pixels[idx + 2] = static_cast<uint8_t>(std::min(255.0, c.b * alpha + pixels[idx + 2] * inv));
        }

        Color getPixel(int x, int y) const {
            if (x < 0 || x >= w || y < 0 || y >= h) return { 0,0,0 };
            int idx = (y * w + x) * 3;
            return { pixels[idx], pixels[idx + 1], pixels[idx + 2] };
        }

        void clear(Color c) {
            for (int i = 0; i < w * h; ++i) {
                pixels[i * 3] = c.r; pixels[i * 3 + 1] = c.g; pixels[i * 3 + 2] = c.b;
            }
        }

        // ★ 升级版抗锯齿连线 (SDF Signed Distance Field 算法)
        void line(double x0, double y0, double x1, double y1, Color c, double thickness = 1.0) {
            double rad = thickness / 2.0;
            int minX = std::max(0, static_cast<int>(std::min(x0, x1) - rad - 2));
            int maxX = std::min(w - 1, static_cast<int>(std::max(x0, x1) + rad + 2));
            int minY = std::max(0, static_cast<int>(std::min(y0, y1) - rad - 2));
            int maxY = std::min(h - 1, static_cast<int>(std::max(y0, y1) + rad + 2));

            double dx = x1 - x0;
            double dy = y1 - y0;
            double l2 = dx * dx + dy * dy;

            for (int y = minY; y <= maxY; ++y) {
                for (int x = minX; x <= maxX; ++x) {
                    double px = x - x0;
                    double py = y - y0;
                    double dist = 0.0;
                    if (l2 == 0.0) {
                        dist = std::sqrt(px * px + py * py);
                    }
                    else {
                        double t = std::max(0.0, std::min(1.0, (px * dx + py * dy) / l2));
                        double projX = x0 + t * dx;
                        double projY = y0 + t * dy;
                        dist = std::sqrt((x - projX) * (x - projX) + (y - projY) * (y - projY));
                    }
                    // 控制抗锯齿边缘羽化 1 像素
                    double alpha = std::max(0.0, std::min(1.0, rad + 0.5 - dist));
                    blendPixel(x, y, c, alpha);
                }
            }
        }

        void rect(int x0, int y0, int rw, int rh, Color c, double thickness = 1.0) {
            line(x0, y0, x0 + rw - 1, y0, c, thickness);
            line(x0 + rw - 1, y0, x0 + rw - 1, y0 + rh - 1, c, thickness);
            line(x0 + rw - 1, y0 + rh - 1, x0, y0 + rh - 1, c, thickness);
            line(x0, y0 + rh - 1, x0, y0, c, thickness);
        }

        void fillRect(int x0, int y0, int rw, int rh, Color c) {
            for (int y = std::max(0, y0); y < std::min(h, y0 + rh); ++y)
                for (int x = std::max(0, x0); x < std::min(w, x0 + rw); ++x)
                    setPixel(x, y, c);
        }

        // ★ 升级版抗锯齿圆圈 (SDF) 边缘丝滑细腻
        void circle(double cx, double cy, double radius, Color c, double thickness = 1.0) {
            double r_out = radius + thickness / 2.0;
            int minX = std::max(0, static_cast<int>(cx - r_out - 2));
            int maxX = std::min(w - 1, static_cast<int>(cx + r_out + 2));
            int minY = std::max(0, static_cast<int>(cy - r_out - 2));
            int maxY = std::min(h - 1, static_cast<int>(cy + r_out + 2));

            for (int y = minY; y <= maxY; ++y) {
                for (int x = minX; x <= maxX; ++x) {
                    double dist = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
                    double d_ring = std::abs(dist - radius);
                    double alpha = std::max(0.0, std::min(1.0, (thickness / 2.0) + 0.5 - d_ring));
                    blendPixel(x, y, c, alpha);
                }
            }
        }

        // ★ 升级版抗锯齿实心圆 (SDF)
        void fillCircle(double cx, double cy, double radius, Color c) {
            int minX = std::max(0, static_cast<int>(cx - radius - 2));
            int maxX = std::min(w - 1, static_cast<int>(cx + radius + 2));
            int minY = std::max(0, static_cast<int>(cy - radius - 2));
            int maxY = std::min(h - 1, static_cast<int>(cy + radius + 2));

            for (int y = minY; y <= maxY; ++y) {
                for (int x = minX; x <= maxX; ++x) {
                    double dist = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
                    double alpha = std::max(0.0, std::min(1.0, radius + 0.5 - dist));
                    blendPixel(x, y, c, alpha);
                }
            }
        }

        // ★ 全新：内置点阵字体渲染引擎！
        void drawText(const std::string& txt, int startX, int startY, Color c, int scale = 1) {
            int currX = startX;
            for (char ch : txt) {
                if (ch < 32 || ch > 126) ch = '?'; // Fallback
                int idx = ch - 32;
                for (int row = 0; row < 8; ++row) {
                    uint8_t bits = font8x8[idx][row];
                    for (int col = 0; col < 8; ++col) {
                        if (bits & (1 << (7 - col))) {
                            if (scale <= 1) setPixel(currX + col, startY + row, c);
                            else fillRect(currX + col * scale, startY + row * scale, scale, scale, c);
                        }
                    }
                }
                currX += 8 * scale; // 每一个字符占 8 点宽
            }
        }

        void drawAxes(double xMin, double xMax, double yMin, double yMax, Color c, int marginL = 40, int marginR = 10, int marginT = 10, int marginB = 30) {
            int plotW = w - marginL - marginR;
            int plotH = h - marginT - marginB;
            auto mapX = [&](double v) -> int { return marginL + static_cast<int>((v - xMin) / (xMax - xMin) * plotW); };
            auto mapY = [&](double v) -> int { return marginT + static_cast<int>((1.0 - (v - yMin) / (yMax - yMin)) * plotH); };
            if (yMin <= 0 && yMax >= 0) line(marginL, mapY(0), w - marginR, mapY(0), c);
            if (xMin <= 0 && xMax >= 0) line(mapX(0), marginT, mapX(0), h - marginB, c);
            rect(marginL, marginT, plotW, plotH, { 200,200,200 });
        }

        int mapPlotX(double v, double xMin, double xMax, int marginL = 40, int marginR = 10) const {
            return marginL + static_cast<int>((v - xMin) / (xMax - xMin) * (w - marginL - marginR));
        }
        int mapPlotY(double v, double yMin, double yMax, int marginT = 10, int marginB = 30) const {
            return marginT + static_cast<int>((1.0 - (v - yMin) / (yMax - yMin)) * (h - marginT - marginB));
        }

        bool saveBMP(const std::string& path) const {
            int rowBytes = w * 3;
            int rowPad = (4 - rowBytes % 4) % 4;
            int dataSize = (rowBytes + rowPad) * h;
            int fileSize = 54 + dataSize;
            std::ofstream f(path, std::ios::binary);
            if (!f) return false;
            auto writeLE16 = [&](uint16_t v) { f.write(reinterpret_cast<char*>(&v), 2); };
            auto writeLE32 = [&](uint32_t v) { f.write(reinterpret_cast<char*>(&v), 4); };
            f.write("BM", 2); writeLE32(fileSize); writeLE32(0); writeLE32(54);
            writeLE32(40); writeLE32(w); writeLE32(h); writeLE16(1); writeLE16(24);
            writeLE32(0); writeLE32(dataSize); writeLE32(2835); writeLE32(2835);
            writeLE32(0); writeLE32(0);
            uint8_t pad[3] = { 0 };
            for (int y = h - 1; y >= 0; --y) {
                for (int x = 0; x < w; ++x) {
                    int idx = (y * w + x) * 3;
                    uint8_t bgr[3] = { pixels[idx + 2], pixels[idx + 1], pixels[idx] };
                    f.write(reinterpret_cast<char*>(bgr), 3);
                }
                if (rowPad > 0) f.write(reinterpret_cast<char*>(pad), rowPad);
            }
            f.close();
            return true;
        }
    };
}
#endif

#include "../jc2_extension_cpp.h"
#include "Image.h"
#include <sstream>

using namespace jc2;

jc::Color parseColor(const Value& v) {
    if (v.is_string()) return jc::Color::parse(v.as_string());
    throw_error("Type Error: Expected a color string.");
    return jc::Color{0,0,0};
}

jc::Image* getImg(JC2_ValueHandle* argv) {
    Value self(argv[0]);
    jc::Image* img = self.get_native_data<jc::Image>();
    if (!img) throw_error("Type Error: Expected an Image instance.");
    return img;
}

JC2_ValueHandle img_width(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    return Value(getImg(argv)->width()).get_handle();
}
JC2_ValueHandle img_height(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    return Value(getImg(argv)->height()).get_handle();
}
JC2_ValueHandle img_setPixel(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    getImg(argv)->setPixel(Value(argv[1]).as_int(), Value(argv[2]).as_int(), parseColor(Value(argv[3])));
    return argv[0];
}
JC2_ValueHandle img_getPixel(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    return Value(getImg(argv)->getPixel(Value(argv[1]).as_int(), Value(argv[2]).as_int()).toHex()).get_handle();
}
JC2_ValueHandle img_clear(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    getImg(argv)->clear(parseColor(Value(argv[1])));
    return argv[0];
}
JC2_ValueHandle img_line(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    double thickness = (argc == 7 && !Value(argv[6]).is_none()) ? Value(argv[6]).as_double() : 1.0;
    getImg(argv)->line(Value(argv[1]).as_double(), Value(argv[2]).as_double(), Value(argv[3]).as_double(), Value(argv[4]).as_double(), parseColor(Value(argv[5])), thickness);
    return argv[0];
}
JC2_ValueHandle img_rect(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    double thickness = (argc == 7 && !Value(argv[6]).is_none()) ? Value(argv[6]).as_double() : 1.0;
    getImg(argv)->rect(Value(argv[1]).as_int(), Value(argv[2]).as_int(), Value(argv[3]).as_int(), Value(argv[4]).as_int(), parseColor(Value(argv[5])), thickness);
    return argv[0];
}
JC2_ValueHandle img_fillRect(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    getImg(argv)->fillRect(Value(argv[1]).as_int(), Value(argv[2]).as_int(), Value(argv[3]).as_int(), Value(argv[4]).as_int(), parseColor(Value(argv[5])));
    return argv[0];
}
JC2_ValueHandle img_circle(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    double thickness = (argc == 6 && !Value(argv[5]).is_none()) ? Value(argv[5]).as_double() : 1.0;
    getImg(argv)->circle(Value(argv[1]).as_double(), Value(argv[2]).as_double(), Value(argv[3]).as_double(), parseColor(Value(argv[4])), thickness);
    return argv[0];
}
JC2_ValueHandle img_fillCircle(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    getImg(argv)->fillCircle(Value(argv[1]).as_double(), Value(argv[2]).as_double(), Value(argv[3]).as_double(), parseColor(Value(argv[4])));
    return argv[0];
}
JC2_ValueHandle img_text(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    std::string txt = Value(argv[1]).as_string();
    int x = Value(argv[2]).as_int();
    int y = Value(argv[3]).as_int();
    double scale = (argc == 6 && !Value(argv[5]).is_none()) ? Value(argv[5]).as_double() : 1.0;
    getImg(argv)->drawText(txt, x, y, parseColor(Value(argv[4])), scale);
    return argv[0];
}
JC2_ValueHandle img_axes(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    jc::Color c = (argc == 6 && !Value(argv[5]).is_none()) ? parseColor(Value(argv[5])) : jc::Color{100, 100, 100};
    getImg(argv)->drawAxes(Value(argv[1]).as_double(), Value(argv[2]).as_double(), Value(argv[3]).as_double(), Value(argv[4]).as_double(), c);
    return argv[0];
}
JC2_ValueHandle img_save(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    if (!getImg(argv)->saveBMP(Value(argv[1]).as_string())) {
        throw_error("IO Error: Failed to save image.");
    }
    return argv[0];
}

Class* g_imageClass = nullptr;

JC2_ValueHandle create_image(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    if (argc < 2) throw_error("TypeError: Image() takes at least 2 arguments (width, height).");
    int w = Value(argv[0]).as_int();
    int h = Value(argv[1]).as_int();
    if (w <= 0 || h <= 0) throw_error("Dimensions must be positive.");
    jc::Color bg = (argc >= 3) ? parseColor(Value(argv[2])) : jc::Color{255, 255, 255};
    
    Instance inst(*g_imageClass);
    inst.set_native_data(new jc::Image(w, h, bg), [](void* ptr) {
        delete static_cast<jc::Image*>(ptr);
    });
    return inst.get_handle();
}

int jc2_init(Module& mod) {
    mod.register_help("image", 
        "═══ Image Processing — Native Module ═══\n"
        "\n"
        "  Requires: import image\n"
        "\n"
        "  The `image` module provides a high-performance 2D rasterization API backed \n"
        "  by C++. It allows rendering geometric shapes, statistical plots, and manipulating \n"
        "  pixels directly in memory, which can be encoded to BMP formats.\n"
        "\n"
        "  Initialization\n"
        "  ──────────────────────\n"
        "    im = image.Image(width, height [, background_color])\n"
        "    im = image.img(...)  // Legacy alias\n"
        "        Allocates a new image surface in RAM. Returns an Image object.\n"
        "        Colors are passed as hex strings (e.g., \"#282C34\") or standard names.\n"
        "\n"
        "  SDF Anti-Aliasing (HD Graphics)\n"
        "  ──────────────────────\n"
        "    The JC2 drawing engine natively utilizes Signed Distance Fields (SDF) \n"
        "    to provide GPU-grade, sub-pixel anti-aliasing entirely in software.\n"
        "    \n"
        "    To activate buttery-smooth edges, simply pass floating-point values \n"
        "    for thickness or coordinates. The engine will automatically perform \n"
        "    Alpha-blending on the sub-pixel boundaries!\n"
        "\n"
        "      im.line(10, 10, 90, 90, \"red\", 1.0)      // Crisp, aliased 1px line\n"
        "      im.line(10, 10, 90, 90, \"red\", 1.5)      // Smooth, anti-aliased 1.5px line!\n"
        "      im.circle(400, 300, 150, \"blue\", 5.8)    // HD Circle with feathered edges\n"
        "\n"
        "  Drawing Primitives (Chainable)\n"
        "  ──────────────────────\n"
        "    Most methods return `self` to allow fluent chaining.\n"
        "\n"
        "    im.width()  /  im.height()\n"
        "    im.clear(color)\n"
        "    im.setPixel(x, y, color)\n"
        "    im.getPixel(x, y)                Returns the color as a hex string \"#RRGGBB\"\n"
        "    \n"
        "    im.line(x0, y0, x1, y1, color [, thick=1.0])\n"
        "    im.rect(x, y, w, h, color [, thick=1.0])\n"
        "    im.fillRect(x, y, w, h, color)\n"
        "    im.circle(cx, cy, radius, color [, thick=1.0])\n"
        "    im.fillCircle(cx, cy, radius, color)\n"
        "\n"
        "  Text Rendering (Native Hardware Font)\n"
        "  ──────────────────────\n"
        "    im.text(text, x, y, color [, scale=1])\n"
        "        Renders strings or numbers using a zero-dependency, hardcoded IBM VGA \n"
        "        8x8 ASCII hardware font. Perfect for HUDs, labels, and retro games.\n"
        "\n"
        "  Data Visualization (Plotting)\n"
        "  ──────────────────────\n"
        "    im.axes(xMin, xMax, yMin, yMax [, color])\n"
        "        Draws cartesian coordinate axes mapped to the specified range.\n"
        "    im.scatter(x_matrix, y_matrix, xMin, xMax, yMin, yMax [, color])\n"
        "        Projects data points from two matrices onto the image canvas.\n"
        "    imgPlot(im, f, xMin, xMax, yMin, yMax, color [, thick=2])\n"
        "        Plot a function f(x) onto the image canvas.\n"
        "\n"
        "  I/O & Network Streaming\n"
        "  ──────────────────────\n"
        "    im.save(\"filepath.bmp\")\n"
        "        Encodes the memory surface and flushes it to a valid Windows BMP file.\n"
        "    \n"
        "    data = imgReadBytes(\"filepath.bmp\")\n"
        "        (Global) Reads a binary file's EXACT byte sequence into a String buffer.\n"
    );

    g_imageClass = new Class("Image");
    mod.register_value("Image", *g_imageClass);
    
    g_imageClass->bind_method("width", img_width, 0, 0, false);
    g_imageClass->bind_method("height", img_height, 0, 0, false);
    g_imageClass->bind_method("setPixel", img_setPixel, 3, 3, false);
    g_imageClass->bind_method("getPixel", img_getPixel, 2, 2, false);
    g_imageClass->bind_method("clear", img_clear, 1, 1, false);
    g_imageClass->bind_method("line", img_line, 5, 6, false);
    g_imageClass->bind_method("rect", img_rect, 5, 6, false);
    g_imageClass->bind_method("fillRect", img_fillRect, 5, 5, false);
    g_imageClass->bind_method("circle", img_circle, 4, 5, false);
    g_imageClass->bind_method("fillCircle", img_fillCircle, 4, 4, false);
    g_imageClass->bind_method("text", img_text, 4, 5, false);
    g_imageClass->bind_method("axes", img_axes, 4, 5, false);
    g_imageClass->bind_method("save", img_save, 1, 1, false);

    g_imageClass->set_allocator(create_image);

    mod.register_function("img", create_image, 2, 3, false);

    mod.register_function_help("image.Image", "image.Image(width, height, [bg_color])", "Allocates a new image surface in RAM. Colors can be hex strings (e.g., \"#FF0000\") or names (e.g., \"red\").", "im = image.Image(800, 600, \"black\")");
    mod.register_function_help("image.img", "image.img(width, height, [bg_color])", "Legacy alias for image.Image.", "im = image.img(800, 600, \"black\")");
    mod.register_function_help("im.width", "im.width()", "Returns the width of the image in pixels.", "w = im.width()");
    mod.register_function_help("im.height", "im.height()", "Returns the height of the image in pixels.", "h = im.height()");
    mod.register_function_help("im.setPixel", "im.setPixel(x, y, color)", "Sets the color of a single pixel at (x, y).", "im.setPixel(10, 10, \"red\")");
    mod.register_function_help("im.getPixel", "im.getPixel(x, y)", "Returns the color of the pixel at (x, y) as a hex string.", "c = im.getPixel(10, 10)");
    mod.register_function_help("im.clear", "im.clear(color)", "Fills the entire image with the specified color.", "im.clear(\"white\")");
    mod.register_function_help("im.line", "im.line(x0, y0, x1, y1, color, [thick])", "Draws an anti-aliased line between two points.", "im.line(10, 10, 100, 100, \"blue\", 2.5)");
    mod.register_function_help("im.rect", "im.rect(x, y, w, h, color, [thick])", "Draws the outline of a rectangle.", "im.rect(50, 50, 200, 100, \"green\")");
    mod.register_function_help("im.fillRect", "im.fillRect(x, y, w, h, color)", "Draws a filled rectangle.", "im.fillRect(50, 50, 200, 100, \"#00FF00\")");
    mod.register_function_help("im.circle", "im.circle(cx, cy, radius, color, [thick])", "Draws an anti-aliased circle outline.", "im.circle(400, 300, 50, \"red\", 1.5)");
    mod.register_function_help("im.fillCircle", "im.fillCircle(cx, cy, radius, color)", "Draws a filled anti-aliased circle.", "im.fillCircle(400, 300, 50, \"red\")");
    mod.register_function_help("im.text", "im.text(txt, x, y, color, [scale])", "Renders text using a built-in 8x8 hardware font.", "im.text(\"Hello\", 10, 10, \"white\", 2)");
    mod.register_function_help("im.axes", "im.axes(xMin, xMax, yMin, yMax, [color])", "Draws Cartesian coordinate axes mapped to the specified range.", "im.axes(-10, 10, -5, 5, \"gray\")");
    mod.register_function_help("im.save", "im.save(path)", "Encodes and saves the image to a BMP file.", "im.save(\"output.bmp\")");

    return 0;
}
JC2_EXTENSION_INIT

#include "jc2_extension_cpp.h"
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
    int scale = (argc == 6 && !Value(argv[5]).is_none()) ? Value(argv[5]).as_int() : 1;
    getImg(argv)->drawText(txt, Value(argv[2]).as_int(), Value(argv[3]).as_int(), parseColor(Value(argv[4])), scale);
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
    int w = Value(argv[0]).as_int();
    int h = Value(argv[1]).as_int();
    jc::Color bg = (argc == 3) ? parseColor(Value(argv[2])) : jc::Color{255, 255, 255};
    
    Instance inst(*g_imageClass);
    inst.set_native_data(new jc::Image(w, h, bg), [](void* ptr) {
        delete static_cast<jc::Image*>(ptr);
    });
    return inst.get_handle();
}

int jc2_init(Module& mod) {
    mod.register_help("image", 
        "Image Module (Native C ABI)\n"
        "Provides a high-performance 2D drawing API with anti-aliasing.\n\n"
        "Usage:\n"
        "  import image\n"
        "  let img = image.img(800, 600, \"#FFFFFF\")\n"
        "  img.line(0, 0, 800, 600, \"red\", 2.5)\n"
        "  img.save(\"output.bmp\")\n"
    );

    g_imageClass = new Class("Image");
    mod.register_value("Image", *g_imageClass);
    
    g_imageClass->bind_method("width", img_width, 1, 1, false);
    g_imageClass->bind_method("height", img_height, 1, 1, false);
    g_imageClass->bind_method("setPixel", img_setPixel, 4, 4, false);
    g_imageClass->bind_method("getPixel", img_getPixel, 3, 3, false);
    g_imageClass->bind_method("clear", img_clear, 2, 2, false);
    g_imageClass->bind_method("line", img_line, 6, 7, false);
    g_imageClass->bind_method("rect", img_rect, 6, 7, false);
    g_imageClass->bind_method("fillRect", img_fillRect, 6, 6, false);
    g_imageClass->bind_method("circle", img_circle, 5, 6, false);
    g_imageClass->bind_method("fillCircle", img_fillCircle, 5, 5, false);
    g_imageClass->bind_method("text", img_text, 5, 6, false);
    g_imageClass->bind_method("axes", img_axes, 5, 6, false);
    g_imageClass->bind_method("save", img_save, 2, 2, false);

    mod.register_function("img", create_image, 2, 3, false);
    return 0;
}
JC2_EXTENSION_INIT

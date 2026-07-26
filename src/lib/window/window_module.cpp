#include "../jc2_extension_cpp.h"
#include "../image/Image.h"
#include <string>
#include <vector>
#include <stdexcept>
#include <algorithm>
#include <cctype>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <imm.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <deque>

struct WinEvent {
    std::string type;
    int x = 0;
    int y = 0;
    int key = 0;
    int button = 0; // 0: Left, 1: Right
};

class NativeWindow {
private:
    HWND hwnd = NULL;
    HIMC defaultImc = NULL;
    std::atomic<bool> running{ true };
    std::atomic<bool> cursorVisible{ true };
    std::thread winThread;

    int width, height;
    std::vector<uint8_t> displayBuffer;
    std::mutex bufMutex;

    // 线程安全事件队列
    std::deque<WinEvent> eventQueue;
    std::mutex eventMutex;

    void pushEvent(const WinEvent& ev) {
        std::lock_guard<std::mutex> lock(eventMutex);
        eventQueue.push_back(ev);
        // 限制队列上限，防止脚本不读取导致内存溢出
        if (eventQueue.size() > 256) eventQueue.pop_front();
    }

    void threadFunc(std::string title) {
        WNDCLASS wc = { 0 };
        wc.lpfnWndProc = staticWndProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = "JC2WindowMT";
        RegisterClass(&wc); // 忽略重复注册错误

        // ★ 修复最大化乱码：使用定死样式的窗口，砍掉最大化和边缘调整大小功能！
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

        RECT rect = { 0, 0, width, height };
        AdjustWindowRect(&rect, style, FALSE);

        this->hwnd = CreateWindow("JC2WindowMT", title.c_str(),
            style | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
            rect.right - rect.left, rect.bottom - rect.top,
            NULL, NULL, GetModuleHandle(NULL), this);

        if (!this->hwnd) { running = false; return; }
        ImmAssociateContext(this->hwnd, NULL);

        MSG msg;
        while (GetMessage(&msg, NULL, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        running = false;
    }

    static LRESULT CALLBACK staticWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        NativeWindow* win = NULL;
        if (msg == WM_NCCREATE) {
            CREATESTRUCT* pCreate = (CREATESTRUCT*)lParam;
            win = (NativeWindow*)pCreate->lpCreateParams;
            SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)win);
        }
        else {
            win = (NativeWindow*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
        }
        if (win) return win->wndProc(hWnd, msg, wParam, lParam);
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }

    LRESULT wndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_APP + 1) {
            InvalidateRect(hWnd, NULL, FALSE);
            return 0;
        }
        if (msg == WM_CLOSE) {
            pushEvent({ "close", 0, 0, 0, 0 });
            PostQuitMessage(0);
            return 0;
        }
        if (msg == WM_PAINT) {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            std::lock_guard<std::mutex> lock(bufMutex);
            if (!displayBuffer.empty()) {
                BITMAPINFO bmi = { 0 };
                bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bmi.bmiHeader.biWidth = width;
                bmi.bmiHeader.biHeight = -height;
                bmi.bmiHeader.biPlanes = 1;
                bmi.bmiHeader.biBitCount = 24;
                bmi.bmiHeader.biCompression = BI_RGB;
                SetDIBitsToDevice(hdc, 0, 0, width, height, 0, 0, 0, height,
                    displayBuffer.data(), &bmi, DIB_RGB_COLORS);
            }
            EndPaint(hWnd, &ps);
            return 0;
        }

        if (msg == WM_SETCURSOR) {
            if (!cursorVisible) {
                SetCursor(NULL); // 隐藏光标，设为空白
                return TRUE;     // 告诉系统已处理
            }
            else {
                // 当需要显示时，必须强制向系统申请标准的“白箭头”并重新绘制！
                SetCursor(LoadCursor(NULL, IDC_ARROW));
                return TRUE;
            }
        }

        // ─── 拦截交互事件 ───
        if (msg == WM_KEYDOWN) { pushEvent({ "keydown", 0, 0, (int)wParam, 0 }); }
        else if (msg == WM_KEYUP) { pushEvent({ "keyup", 0, 0, (int)wParam, 0 }); }
        else if (msg == WM_MOUSEMOVE) { pushEvent({ "mousemove", (short)LOWORD(lParam), (short)HIWORD(lParam), 0, 0 }); }
        else if (msg == WM_LBUTTONDOWN) { pushEvent({ "mousedown", (short)LOWORD(lParam), (short)HIWORD(lParam), 0, 0 }); }
        else if (msg == WM_LBUTTONUP) { pushEvent({ "mouseup", (short)LOWORD(lParam), (short)HIWORD(lParam), 0, 0 }); }
        else if (msg == WM_RBUTTONDOWN) { pushEvent({ "mousedown", (short)LOWORD(lParam), (short)HIWORD(lParam), 0, 1 }); }
        else if (msg == WM_RBUTTONUP) { pushEvent({ "mouseup", (short)LOWORD(lParam), (short)HIWORD(lParam), 0, 1 }); }

        return DefWindowProc(hWnd, msg, wParam, lParam);
    }

public:
    NativeWindow(const std::string& title, int w, int h) : width(w), height(h) {
        int rowBytes = w * 3;
        int rowPad = (4 - rowBytes % 4) % 4;
        displayBuffer.resize((rowBytes + rowPad) * h, 0);
        winThread = std::thread(&NativeWindow::threadFunc, this, title);
        while (running && hwnd == NULL) std::this_thread::yield();
    }

    ~NativeWindow() {
        if (running && hwnd) PostMessage(hwnd, WM_CLOSE, 0, 0);
        if (winThread.joinable()) winThread.join();
    }

    void setImeEnabled(bool enabled) {
        if (!hwnd) return;
        if (enabled) {
            // 恢复被冷藏的输入法
            if (defaultImc) {
                ImmAssociateContext(hwnd, defaultImc);
                defaultImc = NULL;
            }
        }
        else {
            // 剥夺当前输入法并冷藏起来
            HIMC currentImc = ImmGetContext(hwnd);
            if (currentImc) {
                defaultImc = ImmAssociateContext(hwnd, NULL);
                ImmReleaseContext(hwnd, currentImc);
            }
        }
    }

    void showCursor(bool show) {
        cursorVisible = show;
    }

    // ★ 刺透系统限制：强行将操作系统的鼠标指针设定到窗口内的指定坐标
    void setCursorPos(int x, int y) {
        if (!hwnd) return;
        POINT pt = { x, y };
        ClientToScreen(hwnd, &pt); // 必须将客户区相对坐标转为屏幕绝对坐标！
        SetCursorPos(pt.x, pt.y);
    }

    bool isOpen() const { return running; }

    bool pollEvent(WinEvent& outEv) {
        std::lock_guard<std::mutex> lock(eventMutex);
        if (eventQueue.empty()) return false;
        outEv = eventQueue.front();
        eventQueue.pop_front();
        return true;
    }

    void show(const jc::Image* img) {
        if (!running || !hwnd || !img) return;
        const auto& src = img->getRawPixels();
        
        // 严格使用图像自身的尺寸，防止越界
        int imgW = img->width();
        int imgH = img->height();
        int copyW = std::min(width, imgW);
        int copyH = std::min(height, imgH);
        
        int rowBytes = width * 3;
        int rowPad = (4 - rowBytes % 4) % 4;
        int stride = rowBytes + rowPad;
        
        {
            std::lock_guard<std::mutex> lock(bufMutex);
            if (displayBuffer.size() < stride * height) {
                displayBuffer.resize(stride * height, 0);
            }
            for (int y = 0; y < copyH; ++y) {
                for (int x = 0; x < copyW; ++x) {
                    int srcIdx = (y * imgW + x) * 3;
                    int dstIdx = y * stride + x * 3;
                    
                    // 确保不会越界读取 src
                    if (srcIdx + 2 < src.size()) {
                        displayBuffer[dstIdx] = src[srcIdx + 2];     // B
                        displayBuffer[dstIdx + 1] = src[srcIdx + 1]; // G
                        displayBuffer[dstIdx + 2] = src[srcIdx];     // R
                    }
                }
            }
        }
        PostMessage(hwnd, WM_APP + 1, 0, 0);
    }
};
#else
// Linux/macOS 兼容占位
struct WinEvent { std::string type; int x = 0, y = 0, key = 0, button = 0; };
class NativeWindow {
public:
    NativeWindow(const std::string&, int, int) { jc2::throw_error("Window module is strictly Win32 currently."); }
    bool isOpen() { return false; }
    bool pollEvent(WinEvent&) { return false; }
    void show(const jc::Image*) {}
    void setImeEnabled(bool) {}
    void showCursor(bool) {}
    void setCursorPos(int, int) {}
};
#endif

static jc2::Class* g_windowClass = nullptr;

JC2_ValueHandle win_allocator(JC2_VMContext, int argc, JC2_ValueHandle* argv, void*) {
    if (argc < 3) jc2::throw_error("TypeError: Window() takes exactly 3 arguments (title, width, height).");
    std::string title = jc2::Value(argv[0]).as_string();
    int w = jc2::Value(argv[1]).as_int();
    int h = jc2::Value(argv[2]).as_int();
    auto win = new NativeWindow(title, w, h);
    jc2::Instance inst(*g_windowClass);
    inst.set_native_data(win, [](void* ptr) { delete static_cast<NativeWindow*>(ptr); });
    return inst.get_handle();
}

JC2_ValueHandle win_isOpen(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    jc2::Instance self(argv[0]);
    auto win = self.get_native_data<NativeWindow>();
    if (!win) return jc2::Value(false).get_handle();
    return jc2::Value(win->isOpen()).get_handle();
}

JC2_ValueHandle win_pollEvent(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    jc2::Instance self(argv[0]);
    auto win = self.get_native_data<NativeWindow>();
    if (!win) return jc2::Value().get_handle();
    WinEvent ev;
    if (win->pollEvent(ev)) {
        jc2::Dict d;
        d.set(jc2::Value("type"), jc2::Value(ev.type));
        if (ev.type == "mousemove" || ev.type == "mousedown" || ev.type == "mouseup") {
            d.set(jc2::Value("x"), jc2::Value((double)ev.x));
            d.set(jc2::Value("y"), jc2::Value((double)ev.y));
            if (ev.type != "mousemove") d.set(jc2::Value("button"), jc2::Value((double)ev.button));
        }
        if (ev.type == "keydown" || ev.type == "keyup") {
            std::string keyStr;
#ifdef _WIN32
            if (ev.key >= 'A' && ev.key <= 'Z') keyStr = std::string(1, static_cast<char>(ev.key));
            else if (ev.key >= '0' && ev.key <= '9') keyStr = std::string(1, static_cast<char>(ev.key));
            else {
                switch (ev.key) {
                case VK_SPACE:   keyStr = "SPACE"; break;
                case VK_RETURN:  keyStr = "ENTER"; break;
                case VK_ESCAPE:  keyStr = "ESC"; break;
                case VK_LEFT:    keyStr = "LEFT"; break;
                case VK_UP:      keyStr = "UP"; break;
                case VK_RIGHT:   keyStr = "RIGHT"; break;
                case VK_DOWN:    keyStr = "DOWN"; break;
                case VK_SHIFT:   keyStr = "SHIFT"; break;
                case VK_CONTROL: keyStr = "CTRL"; break;
                case VK_MENU:    keyStr = "ALT"; break;
                case VK_TAB:     keyStr = "TAB"; break;
                case VK_BACK:    keyStr = "BACKSPACE"; break;
                default:         keyStr = "UNKNOWN"; break;
                }
            }
#else
            keyStr = "UNKNOWN";
#endif
            d.set(jc2::Value("key"), jc2::Value(keyStr));
            d.set(jc2::Value("keycode"), jc2::Value((double)ev.key));
        }
        return d.get_handle();
    }
    return jc2::Value().get_handle();
}

JC2_ValueHandle win_isKeyDown(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
#ifdef _WIN32
    int key = 0;
    jc2::Value arg(argv[1]);
    if (arg.is_string()) {
        std::string s = arg.as_string();
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) -> char {
            return static_cast<char>(std::toupper(c));
        });
        if (s.length() == 1) {
            char c = s[0];
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
                key = static_cast<int>(c);
            }
        } else {
            if (s == "UP")         key = VK_UP;
            else if (s == "DOWN")  key = VK_DOWN;
            else if (s == "LEFT")  key = VK_LEFT;
            else if (s == "RIGHT") key = VK_RIGHT;
            else if (s == "SPACE") key = VK_SPACE;
            else if (s == "ENTER" || s == "RETURN") key = VK_RETURN;
            else if (s == "ESC" || s == "ESCAPE")   key = VK_ESCAPE;
            else if (s == "SHIFT") key = VK_SHIFT;
            else if (s == "CTRL" || s == "CONTROL") key = VK_CONTROL;
            else if (s == "ALT")   key = VK_MENU;
            else if (s == "TAB")   key = VK_TAB;
            else if (s == "BACKSPACE" || s == "BACK") key = VK_BACK;
        }
    } else {
        key = arg.as_int();
    }
    if (key == 0) return jc2::Value(false).get_handle();
    bool isDown = (GetAsyncKeyState(key) & 0x8000) != 0;
    return jc2::Value(isDown).get_handle();
#else
    return jc2::Value(false).get_handle();
#endif
}

JC2_ValueHandle win_show(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    jc2::Instance self(argv[0]);
    auto win = self.get_native_data<NativeWindow>();
    if (!win) return jc2::Value().get_handle();
    jc2::Value arg(argv[1]);
    if (!arg.is_instance()) jc2::throw_error("Type Error: Expected an Image instance.");
    jc2::Instance imgInst(arg.get_handle());
    auto im = imgInst.get_native_data<jc::Image>();
    if (!im) jc2::throw_error("Type Error: Invalid Image instance.");
    win->show(im);
    return jc2::Value().get_handle();
}

JC2_ValueHandle win_setImeEnabled(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    jc2::Instance self(argv[0]);
    auto win = self.get_native_data<NativeWindow>();
    if (win) win->setImeEnabled(jc2::Value(argv[1]).as_double() != 0.0);
    return jc2::Value().get_handle();
}

JC2_ValueHandle win_showCursor(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    jc2::Instance self(argv[0]);
    auto win = self.get_native_data<NativeWindow>();
    if (win) win->showCursor(jc2::Value(argv[1]).as_double() != 0.0);
    return jc2::Value().get_handle();
}

JC2_ValueHandle win_setCursorPos(JC2_VMContext, int, JC2_ValueHandle* argv, void*) {
    jc2::Instance self(argv[0]);
    auto win = self.get_native_data<NativeWindow>();
    if (win) win->setCursorPos(jc2::Value(argv[1]).as_int(), jc2::Value(argv[2]).as_int());
    return jc2::Value().get_handle();
}

int jc2_init(jc2::Module& mod) {
    g_windowClass = new jc2::Class("Window");
    mod.register_value("Window", *g_windowClass);

    g_windowClass->set_allocator(win_allocator);
    g_windowClass->bind_method("isOpen", win_isOpen, 0, 0, false);
    g_windowClass->bind_method("pollEvent", win_pollEvent, 0, 0, false);
    g_windowClass->bind_method("isKeyDown", win_isKeyDown, 1, 1, false);
    g_windowClass->bind_method("show", win_show, 1, 1, false);
    g_windowClass->bind_method("setImeEnabled", win_setImeEnabled, 1, 1, false);
    g_windowClass->bind_method("showCursor", win_showCursor, 1, 1, false);
    g_windowClass->bind_method("setCursorPos", win_setCursorPos, 2, 2, false);

    mod.register_help("window",
        "═══ Native Window Engine — Native Module ═══\n\n"
        "  Requires: import window\n"
        "  (Note: Currently restricted to Win32 architectures. Requires User32 / Gdi32 / imm32)\n\n"
        "  The `window` module pierces the OS layer to spawn a hardware-accelerated \n"
        "  GUI window, complete with an asynchronous, thread-safe Event Queue for \n"
        "  zero-latency keyboard and mouse polling.\n\n"
        "  Spawning a Window & Basic State\n"
        "  ──────────────────────\n"
        "    win = window.Window(title, width, height)\n"
        "        Requests the OS to create a fixed-size trackable window.\n"
        "    win.isOpen()\n"
        "        Returns true if the window is alive, false if closed by the user.\n"
        "    win.show(image_obj)\n"
        "        Bit-block transfers (Blits) a JC2 `image` object's memory buffer \n"
        "        directly onto the window's device context (HDC) instantly.\n\n"
        "  Mouse & Cursor Control (3D/FPS Mechanics)\n"
        "  ──────────────────────\n"
        "    win.showCursor(boolean)\n"
        "        Dynamically hides (false) or shows (true) the Windows mouse pointer.\n"
        "        Essential for creating immersive games and custom UI crosshairs.\n"
        "        \n"
        "    win.setCursorPos(x, y)\n"
        "        Forcibly teleports the operating system's mouse pointer to the \n"
        "        specified coordinates within the window. Used to create \"infinite\" \n"
        "        mouse-look mechanics in 3D games by continually resetting the \n"
        "        mouse to the center of the screen.\n\n"
        "  IME (Input Method Editor) Control\n"
        "  ──────────────────────\n"
        "    win.setImeEnabled(boolean)\n"
        "        Dynamically enables or disables the OS Input Method (e.g., Pinyin).\n"
        "        • Pass `false` for action games to prevent the IME from intercepting WASD.\n"
        "        • Pass `true` when expecting text input from the user.\n\n"
        "  Real-Time Input Polling\n"
        "  ──────────────────────\n"
        "    win.isKeyDown(key)\n"
        "        Provides instantaneous, zero-latency physical key state.\n"
        "        Accepts human-readable strings (case-insensitive):\n"
        "          \"W\", \"A\", \"S\", \"D\", \"0\"-\"9\"\n"
        "          \"UP\", \"DOWN\", \"LEFT\", \"RIGHT\"\n"
        "          \"SPACE\", \"ENTER\", \"ESC\", \"SHIFT\", \"CTRL\", \"ALT\", \"TAB\"\n"
        "        Returns true if currently pressed, false otherwise.\n\n"
        "  Event Queue (win.pollEvent)\n"
        "  ──────────────────────\n"
        "    ev = win.pollEvent()\n"
        "        Non-blocking pop from the OS message queue. Returns `none` if empty.\n"
        "        If an event exists, returns a Dict with a \"type\" string:\n\n"
        "        • \"keydown\" / \"keyup\"\n"
        "           ev.key       → string (\"W\", \"SPACE\", \"LEFT\", etc.)\n"
        "           ev.keycode   → number (Underlying Win32 Virtual-Key code)\n"
        "        \n"
        "        • \"mousedown\" / \"mouseup\"\n"
        "           ev.x, ev.y   → number (Mouse cursor coordinates)\n"
        "           ev.button    → number (0 = Left Click, 1 = Right Click)\n"
        "        \n"
        "        • \"mousemove\"\n"
        "           ev.x, ev.y   → number (Current coordinates)\n"
        "        \n"
        "        • \"close\"\n"
        "           The user clicked the 'X' button on the window."
    );

    mod.register_function_help("window.Window", "window.Window(title, width, height)", "Creates a new hardware-accelerated OS window.", "win = window.Window(\"My Game\", 800, 600)");
    mod.register_function_help("isOpen", "win.isOpen()", "Returns true if the window is still open, false if closed.", "while (win.isOpen()) { ... }");
    mod.register_function_help("pollEvent", "win.pollEvent()", "Pops the next input event from the window's queue. Returns a Dict or none.", "ev = win.pollEvent()");
    mod.register_function_help("isKeyDown", "win.isKeyDown(key)", "Returns true if the specified physical key is currently pressed.", "if (win.isKeyDown(\"W\")) moveUp()");
    mod.register_function_help("show", "win.show(img)", "Blits an image object's buffer directly to the window.", "win.show(im)");
    mod.register_function_help("setImeEnabled", "win.setImeEnabled(enable)", "Enables or disables the OS Input Method Editor (IME) for the window.", "win.setImeEnabled(0)");
    mod.register_function_help("showCursor", "win.showCursor(show)", "Hides or shows the OS mouse cursor over the window.", "win.showCursor(0)");
    mod.register_function_help("setCursorPos", "win.setCursorPos(x, y)", "Teleports the OS mouse cursor to the specified window coordinates.", "win.setCursorPos(400, 300)");

    return 0;
}

JC2_EXTENSION_INIT

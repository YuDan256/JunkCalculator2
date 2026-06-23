#ifndef JC2_MODULE_WINDOW_H
#define JC2_MODULE_WINDOW_H

#include "Module.h"
#include "../vm/BuiltinRegistry.h"
#include "../vm/VM.h"
#include "Image.h"

#ifdef _WIN32
#include <windows.h>
#include <imm.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <deque>
#include <algorithm>
#endif

namespace jc_window {
    using namespace jc;

#ifdef _WIN32
    // 操作系统事件封包
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
            displayBuffer.resize(w * h * 3, 0);
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
            {
                std::lock_guard<std::mutex> lock(bufMutex);
                for (size_t i = 0; i < src.size(); i += 3) {
                    displayBuffer[i] = src[i + 2]; // B
                    displayBuffer[i + 1] = src[i + 1]; // G
                    displayBuffer[i + 2] = src[i];     // R
                }
            }
            InvalidateRect(hwnd, NULL, FALSE);
        }
    };
#else
    // Linux/macOS 兼容占位
    struct WinEvent { std::string type; int x = 0, y = 0, key = 0, button = 0; };
    class NativeWindow {
    public:
        NativeWindow(const std::string&, int, int) { throw std::runtime_error("Window module is strictly Win32 currently."); }
        bool isOpen() { return false; }
        bool pollEvent(WinEvent&) { return false; }
        void show(const jc::Image*) {}
        void setImeEnabled(bool) {}
        void showCursor(bool) {}
        void setCursorPos(int, int) {}
    };
#endif

    inline ObjClass* windowClass = nullptr;
}

JC2_MODULE(window) {
    using namespace jc_window;
    jc::ModuleReg R(env, builtins, arity);

    windowClass = GcHeap::get().allocate<ObjClass>();
    jc::Value winClassVal(windowClass);

    windowClass->name = "Window";
    R.set("Window", winClassVal);

    auto addWinMethod = [&](const std::string& name, jc::NativeCallable fn) {
        auto fc = GcHeap::get().allocate<ObjClosure>(std::vector<std::string>{}, std::vector<bool>{}, name, nullptr);
        fc->nativeFn = jc::VM::makeNativeFn(fn);
        windowClass->methods[name] = fc;
        };

    addWinMethod("init", [](const std::vector<jc::Value>& args) -> jc::Value {
        if (args.size() != 3) { throw std::runtime_error("TypeError: Window takes exactly 3 arguments (title, width, height)."); }
        std::string title = args[0].asString();
        int w = static_cast<int>(args[1].asDouble());
        int h = static_cast<int>(args[2].asDouble());

        auto selfVal = jc::helpers::getGlobalCallback("self");
        auto inst = selfVal.asInstance();
        inst->nativeData = std::make_shared<NativeWindow>(title, w, h);
        return jc::Value::none();
        });

    addWinMethod("isOpen", [](const std::vector<jc::Value>&) -> jc::Value {
        auto inst = jc::helpers::getGlobalCallback("self").asInstance();
        auto win = std::any_cast<std::shared_ptr<NativeWindow>&>(inst->nativeData);
        return jc::Value(win->isOpen());
        });

    // ─── 暴露 pollEvent：无阻塞抓取单次键鼠事件（带智能字符串按键！） ───
    addWinMethod("pollEvent", [](const std::vector<jc::Value>&) -> jc::Value {
        auto inst = jc::helpers::getGlobalCallback("self").asInstance();
        auto win = std::any_cast<std::shared_ptr<NativeWindow>&>(inst->nativeData);

        WinEvent ev;
        if (win->pollEvent(ev)) {
            ObjDict* d = GcHeap::get().allocate<ObjDict>();
            jc::Value dVal(d);
            auto setDict = [&](const std::string& k, jc::Value v) {
                jc::Value keyVal(k);
                d->elements.push_back({keyVal, v});
                d->keyMap[keyVal] = d->elements.size() - 1;
            };
            setDict("type", jc::Value(ev.type));

            // 处理鼠标移动与点击
            if (ev.type == "mousemove" || ev.type == "mousedown" || ev.type == "mouseup") {
                setDict("x", jc::Value((double)ev.x));
                setDict("y", jc::Value((double)ev.y));
                if (ev.type != "mousemove") setDict("button", jc::Value((double)ev.button));
            }

            // 处理键盘按键
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
#endif
                setDict("key", jc::Value(keyStr));              // 人类可读的直观字符串!
                setDict("keycode", jc::Value((double)ev.key));  // 依然保留底层硬核数字供骨灰级玩家查阅
            }
            return jc::Value(d);
        }
        return jc::Value::none();
        });

    // ─── 暴露 isKeyDown：支持智能字符串映射 ───
    addWinMethod("isKeyDown", [](const std::vector<jc::Value>& args) -> jc::Value {
#ifdef _WIN32
        int key = 0;

        // 智能解析：如果传入的是字符串
        if (args[0].isString()) {
            std::string s = args[0].asString();

            // 强行大写化，显式转换为 char 消除警告
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) -> char {
                return static_cast<char>(std::toupper(c));
                });

            if (s.length() == 1) {
                char c = s[0];
                if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
                    key = static_cast<int>(c); // 直接对应 ASCII 码
                }
            }
            else {
                // 常用控制键字典映射
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
        }
        // 兼容降级：保留直接传入数字键码的能力
        else {
            try { key = static_cast<int>(args[0].asDouble()); }
            catch (...) {}
        }

        if (key == 0) return jc::Value(false);

        // 0x8000 表示最高位为1（正在按下）
        bool isDown = (GetAsyncKeyState(key) & 0x8000) != 0;
        return jc::Value(isDown);
#else
        return jc::Value(false);
#endif
        });

    addWinMethod("show", [](const std::vector<jc::Value>& args) -> jc::Value {
        auto inst = jc::helpers::getGlobalCallback("self").asInstance();
        auto win = std::any_cast<std::shared_ptr<NativeWindow>&>(inst->nativeData);
        if (!args[0].isInstance()) throw std::runtime_error("Type Error: Expected an Image instance.");
        jc::Image* im = static_cast<jc::Image*>(args[0].asInstance()->c_nativeData);
        if (!im) throw std::runtime_error("Type Error: Invalid Image instance.");
        win->show(im);
        return jc::Value::none();
        });

    addWinMethod("setImeEnabled", [](const std::vector<jc::Value>& args) -> jc::Value {
        if (args.empty()) return jc::Value::none();
        auto inst = jc::helpers::getGlobalCallback("self").asInstance();
        auto win = std::any_cast<std::shared_ptr<NativeWindow>&>(inst->nativeData);

        bool enable = args[0].asDouble() != 0.0;
        win->setImeEnabled(enable);
        return jc::Value::none();
        });

    // ★ 暴露强行接管鼠标渲染与位置权限的系统级 API
    addWinMethod("showCursor", [](const std::vector<jc::Value>& args) -> jc::Value {
        if (args.empty()) return jc::Value::none();
        auto inst = jc::helpers::getGlobalCallback("self").asInstance();
        auto win = std::any_cast<std::shared_ptr<NativeWindow>&>(inst->nativeData);
        win->showCursor(args[0].asDouble() != 0.0);
        return jc::Value::none();
        });

    addWinMethod("setCursorPos", [](const std::vector<jc::Value>& args) -> jc::Value {
        if (args.size() < 2) return jc::Value::none();
        auto inst = jc::helpers::getGlobalCallback("self").asInstance();
        auto win = std::any_cast<std::shared_ptr<NativeWindow>&>(inst->nativeData);
        win->setCursorPos(
            static_cast<int>(args[0].asDouble()),
            static_cast<int>(args[1].asDouble())
        );
        return jc::Value::none();
        });
}
#endif

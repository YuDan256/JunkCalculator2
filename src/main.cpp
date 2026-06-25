#include <iostream>
#include <string>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <algorithm>
#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "memory/Value.h"
#include "memory/SipHash.h"
#include "vm/HelpRouter.h"
#include "frontend/Highlight.h"
#include "frontend/Compiler.h"
#include "vm/VM.h"
#include "vm/BuiltinRegistry.h"
#include "regvm/compiler/IRBuilder.h"
#include "regvm/compiler/IROptimizer.h"
#include "regvm/compiler/RegisterAllocator.h"
#include "regvm/compiler/Emitter.h"
#include "regvm/vm/VM.h"
#include <csignal>
#include <atomic>
#include <random>
#include <array>
#include <string_view>

namespace jc {
    std::atomic<bool> g_isWaitingForInput{ false };
}

// 信号处理
static std::atomic<int> g_sigintCount{ 0 };
static auto g_lastSigintTime = std::chrono::steady_clock::now();

void sigintHandler(int signum) {
    (void)signum;
    std::signal(SIGINT, sigintHandler); // 重新注册，防止某些平台恢复默认处理

    if (jc::g_isWaitingForInput.load(std::memory_order_relaxed)) {
        extern bool g_quiet;
        if (!g_quiet) std::cout << "\nGoodbye!" << std::endl;
        std::exit(0);
    }

    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastSigintTime).count() < 1000) {
        g_sigintCount++;
    }
    else {
        g_sigintCount = 1;
    }
    g_lastSigintTime = now;

    if (g_sigintCount >= 3) {
        std::cerr << "\n[Hard Kill] Multiple Ctrl+C detected. Exiting immediately.\n";
        std::exit(1);
    }

    jc::g_interruptRequested.store(true, std::memory_order_relaxed);
}

// 延续字符串判定
static bool endsWithContinuation(const std::string& line) {
    size_t e = line.find_last_not_of(" \t\r\n");
    if (e == std::string::npos) return false;

    char c = line[e];
    if (c == '+' || c == '-' || c == '*' || c == '/' || c == '\\' ||
        c == '%' || c == '^' || c == ',' || c == '=' || c == '.' ||
        c == ':' || c == '?' || c == '|' || c == '&' || c == '<' ||
        c == '>' || c == '!') {
        return true;
    }

    auto endsWithWord = [&](const std::string& word) {
        if (e + 1 < word.length()) return false;
        size_t start = e + 1 - word.length();
        if (line.substr(start, word.length()) != word) return false;
        if (start > 0 && (std::isalnum(line[start - 1]) || line[start - 1] == '_')) return false;
        return true;
        };

    if (endsWithWord("in")) {
        return true;
    }

    return false;
}

std::string getExecutableDir() {
    namespace fs = std::filesystem;
#ifdef _WIN32
    char buf[2048];
    if (GetModuleFileNameA(nullptr, buf, sizeof(buf))) {
        return fs::path(buf).parent_path().string();
    }
#endif
    return fs::current_path().string();
}

void printHelp() {
    jc::HelpRouter::printMainHelp();
}

void printHelpTopic(const std::string& topic) {
    jc::HelpRouter::printHelpTopic(topic);
}

// 核心 VM 实例和全局上下文
jc::VM vm;
jc::regvm::VM regvm_inst;
bool g_useRegVM = false;
bool g_showDisasm = false;  // ★ 新增：字节码反汇编开关
bool g_autoDebug = false;
bool g_profile = false;
bool g_quiet = false;
bool g_showNone = false;
bool g_silentRepl = false;

// ★ 执行一段任意多行/单行代码的统一接口
jc::Value evalCode(const std::string& code, const std::string& sourceFile, bool isFile = false) {
    jc::Lexer lexer(code, sourceFile);                       // ★
    auto tokens = lexer.tokenize();
    jc::Parser parser(tokens, sourceFile);                   // ★
    auto ast = parser.parse();
    
    if (g_useRegVM) {
        jc::regvm::IRGraph graph;
        jc::regvm::IRBuilder builder(&graph);
        builder.build(ast.get());
        
        jc::regvm::IROptimizer::optimize(&graph);
        jc::regvm::RegisterAllocator::allocate(&graph);
        
        jc::regvm::Chunk chunk;
        int localCount = jc::regvm::Emitter::emit(&graph, chunk);
        
        if (g_showDisasm) {
            chunk.disassemble(isFile ? "Script Chunk (RegVM)" : "REPL Chunk (RegVM)");
        }
        
        if (g_autoDebug) {
            std::cout << "Warning: Debugger is not yet supported in RegVM.\n";
        }
        
        return regvm_inst.execute(chunk, localCount);
    }

    jc::Compiler compiler;
    // ★ 核心修复：每次编译前，从 VM 获取最新、最权威的函数列表！
    auto currentFns = vm.getCompiledFunctions();
    compiler.setCompiledFunctions(currentFns);
    compiler.setFunctionIndexOffset(0);
    
    jc::Chunk chunk = compiler.compile(ast.get(), sourceFile); // ★

    auto evalFn = std::make_shared<jc::CompiledFunction>();
    evalFn->name = isFile ? "<script>" : "<eval>";
    evalFn->arity = 0;
    evalFn->maxArity = 0;
    evalFn->localCount = compiler.getTopLevelLocalCount();
    evalFn->chunk = chunk;
    evalFn->sourceFile = sourceFile;

    auto fns = compiler.getCompiledFunctions();
    fns.push_back(evalFn);
    int evalIdx = static_cast<int>(fns.size()) - 1;
    
    vm.setCompiledFunctions(fns);

    if (g_showDisasm) {
        for (size_t i = currentFns.size(); i < fns.size(); ++i) {
            std::string chunkName = fns[i]->name;
            if (chunkName == "<eval>" || chunkName == "<script>") {
                chunkName = isFile ? "Script Chunk" : "REPL Chunk";
            } else {
                chunkName = "Function: " + chunkName;
            }
            fns[i]->chunk.disassemble(chunkName);
        }
    }

    if (g_autoDebug) {
        if (jc::VM::activeVM) {
            jc::VM::activeVM->triggerDebugger(); // ★ 一进虚拟机立刻触发下一行暂停！
        }
    }

    // ★ 核心修复：执行完毕后，将顶层 evalFn 从 VM 的函数列表中移除，避免 REPL 历史堆积导致内存泄漏
    struct EvalFnCleanup {
        jc::VM& vmRef;
        std::shared_ptr<jc::CompiledFunction> fn;
        ~EvalFnCleanup() {
            auto currFns = vmRef.getCompiledFunctions();
            if (!currFns.empty() && currFns.back() == fn) {
                currFns.pop_back();
                vmRef.setCompiledFunctions(currFns);
            }
        }
    } cleanup{vm, evalFn};

    return vm.callVMFunction(evalIdx, nullptr, 0);
}

void runScript(const std::string& filepath, bool isImport = false) {
    std::string resolvedPath = jc::helpers::safeResolvePath(filepath);
    if (!std::filesystem::exists(resolvedPath))
        resolvedPath = jc::helpers::safeResolvePath(filepath + ".jc2");
    if (!std::filesystem::exists(resolvedPath)) {
        std::cerr << "   IO Error: Cannot open script '" << filepath << "'." << std::endl;
        return;
    }
    std::ifstream file(resolvedPath);
    if (!file.is_open()) {
        std::cerr << "   IO Error: Cannot open script '" << filepath << "'." << std::endl;
        return;
    }

    // ★ 一次性读取整个文件
    std::string code, line;
    while (std::getline(file, line)) code += line + "\n";
    file.close();

    jc::helpers::g_scriptDirStack.push_back(
        std::filesystem::path(resolvedPath).parent_path().string());

    try {
        // ★ 将 resolvedPath 传进虚拟机
        jc::Value result = evalCode(code, resolvedPath, true);
        if (!result.isNone()) {
            vm.setGlobal("ANS", result);
            regvm_inst.setGlobal("ANS", result);
        }
    }
    catch (const jc::EngineInterruptError&) {
        if (isImport) throw;
        std::cerr << "\n^C KeyboardInterrupt in script '" << resolvedPath << "'" << std::endl;
    }
    catch (const std::exception& ex) {
        if (isImport) throw;
        // 注意这里不在最后加 RESET，交给底层传递回来的字符串本身控制
        std::cerr << "\n" << jc::col(jc::Ansi::BRIGHT_RED)
            << "Error in '" << resolvedPath << "':\n"
            << ex.what() << std::endl;
    }
    if (g_profile && jc::VM::activeVM) {
        jc::VM::activeVM->printProfileReport();
    }

    jc::helpers::g_scriptDirStack.pop_back();
}

void saveWorkspace(const std::string& filename) {
    namespace fs = std::filesystem;

    // 正确调用 C++ 原生的 getWorkspace
    jc::BuiltinRegistry reg; reg.registerAll();
    std::string wp = reg.getBuiltins()["getWorkspace"]({}).asString();

    fs::path dir(wp);
    if (!fs::exists(dir)) fs::create_directories(dir);
    std::ofstream out((dir / (filename + ".jc2")).string());

    int count = 0;
    auto globals = g_useRegVM ? regvm_inst.getGlobals() : vm.getGlobals();
    for (const auto& [name, value] : globals) {
        if (name == "PI" || name == "E" || name == "i" || name == "I" || name == "ANS") continue;
        out << name << " = " << value.toJC2Expression() << "\n";
        count++;
    }
    out.close();
    std::cout << "   Saved " << count << " variables to " << (dir / (filename + ".jc2")).string() << std::endl;
}

void loadWorkspace(const std::string& filename) {
    namespace fs = std::filesystem;

    jc::BuiltinRegistry reg; reg.registerAll();
    std::string wp = reg.getBuiltins()["getWorkspace"]({}).asString();

    std::string path = (fs::path(wp) / (filename + ".jc2")).string();
    if (!fs::exists(path)) { std::cerr << "   IO Error: Workspace not found.\n"; return; }

    vm.clearGlobals();
    regvm_inst.clearGlobals();
    runScript(path);
}

int runTestSuite(const std::string& testPath, const std::string& exeDir) {
    namespace fs = std::filesystem;
    fs::path targetPath = testPath.empty() ? fs::path(exeDir) / "tests" : fs::path(testPath);

    if (!fs::exists(targetPath) || !fs::is_directory(targetPath)) {
        std::cerr << jc::col(jc::Ansi::BRIGHT_RED) << "Test Error: Directory not found -> " << targetPath.string() << jc::col(jc::Ansi::RESET) << std::endl;
        return 1;
    }

    std::cout << jc::col(jc::Ansi::BRIGHT_CYAN) << "=========================================\n"
              << "JC2 Test Suite Started\n"
              << "Target: " << targetPath.string() << "\n"
              << "=========================================\n" << jc::col(jc::Ansi::RESET);

    int total = 0;
    int passed = 0;
    int failed = 0;
    std::vector<std::string> failedFiles;

    // 收集所有测试文件以保证顺序稳定
    std::vector<fs::path> testFiles;
    for (const auto& entry : fs::recursive_directory_iterator(targetPath)) {
        if (entry.is_regular_file() && entry.path().extension() == ".jc2") {
            testFiles.push_back(entry.path());
        }
    }
    std::sort(testFiles.begin(), testFiles.end());

    for (const auto& path : testFiles) {
        total++;
        std::string filepath = path.string();
        std::string filename = path.filename().string();

        // Reset Environment
        vm.clearGlobals();
        vm.setGlobal("PI", jc::Value(3.14159265358979323846));
        vm.setGlobal("E", jc::Value(2.71828182845904523536));
        vm.setGlobal("i", jc::Value(jc::Complex(0.0, 1.0)));
        vm.setGlobal("I", jc::Value(jc::Complex(0.0, 1.0)));
        vm.setGlobal("ANS", jc::Value::none());
        
        regvm_inst.clearGlobals();
        regvm_inst.setGlobal("PI", jc::Value(3.14159265358979323846));
        regvm_inst.setGlobal("E", jc::Value(2.71828182845904523536));
        regvm_inst.setGlobal("i", jc::Value(jc::Complex(0.0, 1.0)));
        regvm_inst.setGlobal("I", jc::Value(jc::Complex(0.0, 1.0)));
        regvm_inst.setGlobal("ANS", jc::Value::none());
        jc::helpers::g_scriptDirStack.clear();

        std::cout << jc::col(jc::Ansi::BRIGHT_BLUE) << "\n[TEST] Running " << filename << "..." << jc::col(jc::Ansi::RESET) << std::endl;

        std::ifstream file(filepath);
        if (!file.is_open()) {
            std::cout << jc::col(jc::Ansi::BRIGHT_RED) << "  -> [FAIL] (IO Error)" << jc::col(jc::Ansi::RESET) << std::endl;
            failed++;
            failedFiles.push_back(filename + " (IO Error)");
            continue;
        }

        std::string code, line;
        while (std::getline(file, line)) code += line + "\n";
        file.close();

        jc::helpers::g_scriptDirStack.push_back(path.parent_path().string());

        try {
            evalCode(code, filepath, true);
            std::cout << jc::col(jc::Ansi::BRIGHT_GREEN) << "  -> [PASS] " << filename << jc::col(jc::Ansi::RESET) << std::endl;
            passed++;
        } catch (const std::exception& ex) {
            std::cout << jc::col(jc::Ansi::BRIGHT_RED) << "  -> [FAIL] " << filename << jc::col(jc::Ansi::RESET) << std::endl;
            std::cerr << "     Error: " << ex.what() << std::endl;
            failed++;
            failedFiles.push_back(filename);
        } catch (...) {
            std::cout << jc::col(jc::Ansi::BRIGHT_RED) << "  -> [FAIL] " << filename << jc::col(jc::Ansi::RESET) << std::endl;
            std::cerr << "     Error: Unknown Exception" << std::endl;
            failed++;
            failedFiles.push_back(filename);
        }
        jc::helpers::g_scriptDirStack.clear();
    }

    std::cout << jc::col(jc::Ansi::BRIGHT_CYAN) << "\n=========================================\n"
              << "JC2 Test Suite Summary\n"
              << "=========================================\n" << jc::col(jc::Ansi::RESET);
    std::cout << "Total Tests : " << total << "\n";
    std::cout << jc::col(jc::Ansi::BRIGHT_GREEN) << "Passed      : " << passed << jc::col(jc::Ansi::RESET) << "\n";
    if (failed > 0) {
        std::cout << jc::col(jc::Ansi::BRIGHT_RED) << "Failed      : " << failed << jc::col(jc::Ansi::RESET) << "\n\n";
        std::cout << "Failed Tests:\n";
        for (const auto& f : failedFiles) {
            std::cout << "  - " << f << "\n";
        }
    } else {
        std::cout << jc::col(jc::Ansi::BRIGHT_GREEN) << "\nAll tests passed successfully! 🎉" << jc::col(jc::Ansi::RESET) << "\n";
    }
    std::cout << jc::col(jc::Ansi::BRIGHT_CYAN) << "=========================================\n" << jc::col(jc::Ansi::RESET);

    return failed > 0 ? 1 : 0;
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    std::system("chcp 65001 > nul");
#endif
    jc::initSipHashSeed();
    jc::enableAnsiColors();
    std::signal(SIGINT, sigintHandler);
    std::string exeDir = getExecutableDir();
    // ===== 初始化超级虚拟机 =====
    jc::BuiltinRegistry registry;
    registry.registerAll();
    for (const auto& [name, fn] : registry.getBuiltins()) {
        const auto& arities = registry.getArity().find(name)->second;
        vm.registerBuiltin(name, fn, arities);
        // ★ 我们把内置方法只留给原生表处理！彻底释放 Globals 字典空间供用户自由重载调用！
    }

    // 初始化系统常量 (普通赋予即可！它无法通过系统的 delete 指令销毁，但你能将 PI 暂时盖为别的值)
    vm.setGlobal("PI", jc::Value(3.14159265358979323846));
    vm.setGlobal("E", jc::Value(2.71828182845904523536));
    vm.setGlobal("i", jc::Value(jc::Complex(0.0, 1.0)));
    vm.setGlobal("I", jc::Value(jc::Complex(0.0, 1.0)));
    vm.setGlobal("ANS", jc::Value::none());
    
    regvm_inst.setGlobal("PI", jc::Value(3.14159265358979323846));
    regvm_inst.setGlobal("E", jc::Value(2.71828182845904523536));
    regvm_inst.setGlobal("i", jc::Value(jc::Complex(0.0, 1.0)));
    regvm_inst.setGlobal("I", jc::Value(jc::Complex(0.0, 1.0)));
    regvm_inst.setGlobal("ANS", jc::Value::none());

    // 绑定虚拟机外包服务给系统级运行时回调！
    jc::helpers::setGlobalCallback = [](const std::string& name, const jc::Value& val) { 
        vm.setGlobal(name, val); 
        regvm_inst.setGlobal(name, val);
    };
    jc::helpers::evalCallback = [](const std::string& code) -> jc::Value { return evalCode(code, "<eval>", false); };
    jc::helpers::runFileCallback = [](const std::string& path) { runScript(path, true); };
    jc::helpers::callFunctionCallback = [](jc::ObjClosure* closure, const std::vector<jc::Value>& args) -> jc::Value {
        if (closure->isNative() && !closure->isBytecode()) {
            jc::helpers::nativeSelfStack.push_back(closure->boundSelf);
            jc::helpers::nativeClassStack.push_back(closure->boundClass);
            jc::Value result;
            try {
                auto& fn = std::any_cast<jc::NativeCallable&>(closure->nativeFn);
                result = fn(args);
            } catch (...) {
                jc::helpers::nativeSelfStack.pop_back();
                jc::helpers::nativeClassStack.pop_back();
                throw;
            }
            jc::helpers::nativeSelfStack.pop_back();
            jc::helpers::nativeClassStack.pop_back();
            return result;
        } else if (closure->isBytecode()) {
            return vm.callVMFunction(closure->compiledFnIndex, args, closure, closure->boundSelf, closure->boundClass);
        }
        throw std::runtime_error("Runtime Error: Invalid closure.");
    };
    jc::helpers::resolvePathCallback = [exeDir](const std::string& path) -> std::string {
        namespace fs = std::filesystem;
        fs::path p(path);
        if (p.is_absolute()) return p.string();
        std::vector<std::string> c = {
            fs::weakly_canonical(fs::current_path() / p).string(),
            (fs::current_path() / "data" / p).string(),
            (fs::path(exeDir) / p).string(),
            (fs::path(exeDir) / "data" / p).string(),
            (fs::path(exeDir) / "lib" / p).string()
        };
        for (const auto& cp : c) if (fs::exists(cp)) return cp;
        return fs::weakly_canonical(fs::current_path() / p).string();
        };

    // ★ 清洁版命令行参数解析
    std::string scriptPath = "";
    std::string evalStr = "";
    bool runTests = false;
    std::string testPath = "";
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-e" || arg == "--eval") {
            if (i + 1 < argc) {
                evalStr = argv[++i];
            }
            else {
                std::cerr << "Error: --eval requires an argument.\n";
                return 1;
            }
        }
        else if (arg == "-q" || arg == "--quiet") {
            g_quiet = true;
        }
        else if (arg == "-d") {
            g_showDisasm = true;
        }
        else if (arg == "--debug") {    // ★ 拦截 --debug 启动项
            g_autoDebug = true;
        }
        else if (arg == "--regvm") {
            g_useRegVM = true;
        }
        else if (arg == "--run") {
            continue; // 跳过 --run 标记
        }
        else if (arg == "--help" || arg == "-h") {
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                printHelpTopic(argv[i + 1]);
                i++; // 消耗掉 topic 参数
            }
            else {
                printHelp();
            }
            return 0;
        }
        else if (arg == "--version" || arg == "-v") {
            std::cout << "Junk Calculator 2.4.5.0\n";
            return 0;
        }
        else if (arg == "--profile") {
            g_profile = true;
            vm.enableProfiler(true);
        }
        else if (arg == "--test") {
            runTests = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                testPath = argv[++i];
            }
        }
        else {
            if (scriptPath.empty()) {
                scriptPath = arg;
            }
            else {
                std::cerr << "Unknown argument or multiple scripts provided: " << arg << std::endl;
                return 1;
            }
        }
    }

    // 如果有 --test 参数，则执行测试套件并退出
    if (runTests) {
        return runTestSuite(testPath, exeDir);
    }

    // 如果有 --eval 参数，则直接执行并退出
    if (!evalStr.empty()) {
        try {
            jc::Value result = evalCode(evalStr, "<command-line>", false);
            if (!result.isNone()) {
                std::cout << result << std::endl;
            }
        }
        catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
            return 1;
        }
        return 0;
    }

    // 有脚本路径则执行脚本并退出
    if (!scriptPath.empty()) {
        runScript(scriptPath);
        return 0;
    }

    auto printBanner = []() {
        std::cout << jc::col(jc::Ansi::BRIGHT_CYAN)
            << "=================================================\n"
            << "   Junk Calculator 2.4.5.0\n"
            << "   Developed by Yu Liangyang, Tsinghua University\n"
            << "=================================================\n" << jc::col(jc::Ansi::RESET)
            << "Type " << jc::col(jc::Ansi::BRIGHT_YELLOW) << "'/help'" << jc::col(jc::Ansi::RESET) << " for a list of commands." << std::endl;
        };

    if (!g_quiet) printBanner();

    while (true) {
        jc::g_interruptRequested.store(false, std::memory_order_relaxed);
        g_sigintCount = 0;

        std::string input;
        if (!g_quiet) std::cout << "\n" << jc::col(jc::Ansi::BOLD) << jc::col(jc::Ansi::BRIGHT_CYAN) << "JC2> " << jc::col(jc::Ansi::RESET);

        jc::g_isWaitingForInput.store(true, std::memory_order_relaxed);
        bool getlineResult = (bool)std::getline(std::cin, input);
        jc::g_isWaitingForInput.store(false, std::memory_order_relaxed);

        if (!getlineResult) {
            bool isInterrupt = jc::g_interruptRequested.load(std::memory_order_relaxed);
            bool isEof = std::cin.eof();
            std::cin.clear(); // 清除错误状态

            if (isInterrupt) {
                std::cout << "\n";
                continue;
            }
            if (isEof) {
                if (!g_quiet) std::cout << "\nGoodbye!" << std::endl;
                std::exit(1);
            }

            std::cout << "\n";
            continue;
        }

        size_t start = input.find_first_not_of(" \t");
        size_t end = input.find_last_not_of(" \t");
        if (start == std::string::npos) continue;
        input = input.substr(start, end - start + 1);

        auto checkInputState = [](const std::string& s, int& braces, int& parens, int& brackets, bool& inStr, bool& isMulti, int& commentNesting) {
            braces = 0; parens = 0; brackets = 0;
            inStr = false; isMulti = false; commentNesting = 0;
            char strQuote = '\0';
            for (size_t i = 0; i < s.length(); ++i) {
                char c = s[i];
                if (commentNesting > 0) {
                    if (c == '/' && i + 1 < s.length() && s[i + 1] == '*') {
                        commentNesting++;
                        i++;
                    } else if (c == '*' && i + 1 < s.length() && s[i + 1] == '/') {
                        commentNesting--;
                        i++;
                    }
                }
                else if (inStr) {
                    if (c == '\\' && i + 1 < s.length()) {
                        i++;
                    }
                    else if (c == strQuote) {
                        if (isMulti) {
                            if (i + 2 < s.length() && s[i + 1] == strQuote && s[i + 2] == strQuote) {
                                inStr = false;
                                isMulti = false;
                                i += 2;
                            }
                        }
                        else {
                            inStr = false;
                        }
                    }
                }
                else {
                    if (c == '/' && i + 1 < s.length() && s[i + 1] == '/') {
                        while (i < s.length() && s[i] != '\n') i++;
                    }
                    else if (c == '/' && i + 1 < s.length() && s[i + 1] == '*') {
                        commentNesting++;
                        i++;
                    }
                    else if (c == '"' || c == '\'') {
                        inStr = true;
                        strQuote = c;
                        if (i + 2 < s.length() && s[i + 1] == c && s[i + 2] == c) {
                            isMulti = true;
                            i += 2;
                        }
                        else {
                            isMulti = false;
                        }
                    }
                    else if (c == '{') braces++;
                    else if (c == '}') braces--;
                    else if (c == '(') parens++;
                    else if (c == ')') parens--;
                    else if (c == '[') brackets++;
                    else if (c == ']') brackets--;
                }
            }
            };

        int braces = 0, parens = 0, brackets = 0, commentNesting = 0;
        bool inStr = false, isMulti = false;
        checkInputState(input, braces, parens, brackets, inStr, isMulti, commentNesting);

        bool inputAborted = false;
        bool isEof = false;
        while (braces > 0 || parens > 0 || brackets > 0 || (inStr && isMulti) || commentNesting > 0 || (!inStr && commentNesting == 0 && endsWithContinuation(input))) {
            std::string line;
            if (!g_quiet) std::cout << jc::col(jc::Ansi::BRIGHT_CYAN) << "...  " << jc::col(jc::Ansi::RESET);
            if (!std::getline(std::cin, line)) {
                bool isInterrupt = jc::g_interruptRequested.load(std::memory_order_relaxed);
                isEof = std::cin.eof();
                std::cin.clear();

                if (isInterrupt) {
                    std::cout << "\n";
                    isEof = false; // 忽略由 Ctrl+C 引起的 EOF
                    inputAborted = true;
                    break;
                }
                if (isEof) {
                    inputAborted = true;
                    break;
                }
                std::cout << "\n";
                inputAborted = true;
                break;
            }
            input += "\n" + line;
            checkInputState(input, braces, parens, brackets, inStr, isMulti, commentNesting);
        }
        if (inputAborted && isEof) {
            if (!g_quiet) std::cout << "\nGoodbye!" << std::endl;
            std::exit(1);
        }
        if (inputAborted) continue;

        if (input.length() >= 2 && input[0] == '/' && input[1] == '/') continue;

        if (!input.empty() && input[0] == '/' && (input.length() == 1 || (input[1] != '/' && input[1] != '*'))) {
            if (input == "/color on") { jc::colorsEnabled = true; continue; }
            if (input == "/color off") { jc::colorsEnabled = false; continue; }

            // ★ 随时开关 Disassembly 打印
            if (input == "/d on") {
                g_showDisasm = true;
                std::cout << "Bytecode disassembly enabled.\n";
                continue;
            }
            if (input == "/d off") {
                g_showDisasm = false;
                std::cout << "Bytecode disassembly disabled.\n";
                continue;
            }

            // ★ 随时开关全局单步 Debugger
            if (input == "/debug on") {
                g_autoDebug = true;
                std::cout << "Interactive Step-Debugger enabled. (Will break on next evaluated line)\n";
                continue;
            }
            if (input == "/debug off") {
                g_autoDebug = false;
                if (jc::VM::activeVM) jc::VM::activeVM->disableDebugger(); // ★ 强制拉闸
                std::cout << "Interactive Step-Debugger disabled.\n";
                continue;
            }
            if (input == "/regvm on") {
                g_useRegVM = true;
                std::cout << "Register VM backend enabled.\n";
                continue;
            }
            if (input == "/regvm off") {
                g_useRegVM = false;
                std::cout << "Register VM backend disabled (using Stack VM).\n";
                continue;
            }
            if (input == "/profile on") {
                g_profile = true;
                if (jc::VM::activeVM) jc::VM::activeVM->enableProfiler(true);
                std::cout << "Profiler enabled. Will print report after execution.\n";
                continue;
            }
            if (input == "/profile off") {
                g_profile = false;
                if (jc::VM::activeVM) jc::VM::activeVM->enableProfiler(false);
                std::cout << "Profiler disabled.\n";
                continue;
            }
            if (input == "/show_none on") {
                g_showNone = true;
                std::cout << "Show 'none' enabled.\n";
                continue;
            }
            if (input == "/show_none off") {
                g_showNone = false;
                std::cout << "Show 'none' disabled.\n";
                continue;
            }
            if (input == "/silent on") {
                g_silentRepl = true;
                std::cout << "Silent REPL enabled. Return values will not be printed.\n";
                continue;
            }
            if (input == "/silent off") {
                g_silentRepl = false;
                std::cout << "Silent REPL disabled.\n";
                continue;
            }
            if (input == "/exit" || input == "/quit") break;
            if (input == "/help") { printHelp(); continue; }
            if (input == "/version") { std::cout << "Junk Calculator 2.4.5.0\n"; continue; }
            if (input.substr(0, 6) == "/help ") { printHelpTopic(input.substr(6)); continue; }
            if (input == "/clear") { 
                vm.clearGlobals(); 
                regvm_inst.clearGlobals();
                std::cout << "All variables cleared.\n"; 
                continue; 
            }
            if (input == "/cls") {
#ifdef _WIN32
                std::system("cls");
#else
                std::system("clear");
#endif
                printBanner();
                continue;
            }
            if (input.substr(0, 6) == "/save ") { saveWorkspace(input.substr(6)); continue; }
            if (input.substr(0, 6) == "/load ") { loadWorkspace(input.substr(6)); continue; }
            if (input.substr(0, 8) == "/delete ") {
                std::string varName = input.substr(8);
                size_t s = varName.find_first_not_of(" \t");
                size_t e = varName.find_last_not_of(" \t");
                if (s != std::string::npos) varName = varName.substr(s, e - s + 1);
                else varName = "";
                vm.removeGlobal(varName);
                regvm_inst.removeGlobal(varName);
                std::cout << "Variable '" << varName << "' forcefully deleted.\n";
                continue;
            }
            if (input == "/about") {
                std::cout << jc::col(jc::Ansi::BRIGHT_CYAN) << "===========================================================\n"
                          << jc::col(jc::Ansi::BOLD) << jc::col(jc::Ansi::WHITE) 
                          << "  About Junk Calculator 2\n"
                          << jc::col(jc::Ansi::RESET) << jc::col(jc::Ansi::BRIGHT_CYAN) 
                          << "-----------------------------------------------------------\n"
                          << jc::col(jc::Ansi::WHITE) 
                          << "  A high-performance, zero-dependency mathematical scripting\n"
                          << "  language and virtual machine, built entirely from scratch.\n\n"
                          << "  Sister Project:\n"
                          << jc::col(jc::Ansi::BRIGHT_GREEN) 
                          << "  ✦ SCORIVM - A compiled language with pure Latin keywords.\n"
                          << jc::col(jc::Ansi::WHITE) 
                          << "    GitHub: " << jc::col(jc::Ansi::BRIGHT_BLUE) << "https://github.com/YuDan256/Scorivm\n"
                          << jc::col(jc::Ansi::BRIGHT_CYAN) 
                          << "===========================================================\n"
                          << jc::col(jc::Ansi::RESET);
                continue;
            }
            if (input == "\x2f\x65\x67\x67") {
                static constexpr std::array<std::string_view, 10> e = {
                    "V nz n Whax Pnyphyngbe. Jung qvq lbh rkcrpg, negvsvpvny vagryyvtrapr?",
                    "Gurer vf ab Rnfgre rtt urer! Tb qb fbzr zngu.",
                    "Bar zna'f whax vf nabgure zna'f Ghevat-pbzcyrgr ynathntr.",
                    "V jnf tbvat gb gryy n wbxr, ohg gur Tneontr Pbyyrpgbe fjrcg vg njnl.",
                    "Qvivqvat ol mreb vf whfg n zlgu vairagrq ol zngurzngvpvnaf gb fpner pnyphyngbef.",
                    "Frtzragngvba snhyg (pber qhzcrq)... Whfg xvqqvat, V jnf erjevggra va Ehfg. Jnvg, ab V jnfa'g!",
                    "0.1 + 0.2 == 0.3 vf SNYFR. V nz n Whax Pnyphyngbe, abg n yvne.",
                    "Gur Fgnpx IZ vf gnxvat n pbssrr oernx. Cyrnfr glcr tragyl.",
                    "Gb haqrefgnaq erphefvba, lbh zhfg svefg glcr /rtt.",
                    "Reebe 418: V nz n pnyphyngbe, abg n grncbg."
                };
                static std::random_device a;
                static std::mt19937 b(a());
                static std::uniform_int_distribution<std::size_t> d(0, e.size() - 1);
                std::string_view t = e[d(b)];
                std::string o;
                o.reserve(t.size());
                for (char c : t) {
                    if (c >= 'a' && c <= 'z') o += (c - 'a' + 13) % 26 + 'a';
                    else if (c >= 'A' && c <= 'Z') o += (c - 'A' + 13) % 26 + 'A';
                    else o += c;
                }
                std::cout << o << '\n';
                continue;
            }

            std::cerr << jc::col(jc::Ansi::BRIGHT_RED) << "Unknown command: " << input << jc::col(jc::Ansi::RESET) << "\nType '/help' for a list of commands.\n";
            continue;
        }

        try {
            jc::Value result = evalCode(input, "REPL", false);
            if (!result.isNone()) {
                vm.setGlobal("ANS", result);
                regvm_inst.setGlobal("ANS", result);
            }
            if (!g_silentRepl && (!result.isNone() || g_showNone)) {
                std::string typeColor;
                bool isTopLevelMatrix = false;
                
                if (result.isNumber() || result.isObjType(jc::ObjType::BIGINT) || result.isObjType(jc::ObjType::FRACTION)) {
                    typeColor = jc::col(jc::Ansi::BRIGHT_YELLOW);
                } else if (result.isObjType(jc::ObjType::COMPLEX)) {
                    typeColor = jc::col(jc::Ansi::BRIGHT_MAGENTA);
                } else if (result.isObjType(jc::ObjType::STRING)) {
                    typeColor = jc::col(jc::Ansi::BRIGHT_GREEN);
                } else if (result.isObjType(jc::ObjType::REAL_MATRIX)) {
                    typeColor = jc::col(jc::Ansi::BRIGHT_YELLOW);
                    isTopLevelMatrix = true;
                } else if (result.isObjType(jc::ObjType::COMPLEX_MATRIX)) {
                    typeColor = jc::col(jc::Ansi::BRIGHT_MAGENTA);
                    isTopLevelMatrix = true;
                } else if (result.isObjType(jc::ObjType::STRING_MATRIX)) {
                    typeColor = jc::col(jc::Ansi::BRIGHT_GREEN);
                    isTopLevelMatrix = true;
                } else if (result.isObjType(jc::ObjType::BASENUM)) {
                    typeColor = jc::col(jc::Ansi::BRIGHT_CYAN);
                } else if (result.isObjType(jc::ObjType::CLOSURE) || result.isObjType(jc::ObjType::CLASS)) {
                    typeColor = jc::col(jc::Ansi::BRIGHT_BLUE);
                } else if (result.isObjType(jc::ObjType::INSTANCE)) {
                    typeColor = jc::col(jc::Ansi::BRIGHT_CYAN);
                } else if (result.isObjType(jc::ObjType::DICT) || result.isObjType(jc::ObjType::LIST) || result.isObjType(jc::ObjType::SET)) {
                    typeColor = jc::col(jc::Ansi::CYAN);
                } else if (result.isNone()) {
                    typeColor = jc::col(jc::Ansi::GRAY);
                } else {
                    typeColor = jc::col(jc::Ansi::WHITE); // SymExpr 等
                }

                struct MatrixPrintGuard {
                    ~MatrixPrintGuard() { jc::g_printMatrix2D = false; }
                } _guard;
                
                jc::g_printMatrix2D = isTopLevelMatrix;
                std::cout << typeColor << result << jc::col(jc::Ansi::RESET) << std::endl;
            }
            if (g_profile && jc::VM::activeVM) {
                jc::VM::activeVM->printProfileReport();
            }
        }
        catch (const jc::EngineInterruptError&) {
            std::cerr << "^C KeyboardInterrupt" << std::endl;
        }
        catch (const std::exception& e) {
            std::cerr << jc::col(jc::Ansi::BRIGHT_RED)
                << "Error: " << e.what() << jc::col(jc::Ansi::RESET) << std::endl;
        }
    }

    if (!g_quiet) std::cout << "\nGoodbye!" << std::endl;
    return 0;
}

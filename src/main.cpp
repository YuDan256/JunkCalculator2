#include <iostream>
#include <string>
#include <cstdlib>
#include <cctype>
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
#include "utils/fmt/Formatter.h"
#include "vm/BuiltinRegistry.h"
#include "compiler/Resolver.h"
#include "compiler/IRBuilder.h"
#include "compiler/IROptimizer.h"
#include "compiler/RegisterAllocator.h"
#include "compiler/Emitter.h"
#include "vm/VM.h"
#include "vm/BytecodeSerializer.h"
#include "utils/lsp/LspServer.h"
#include <csignal>
#include <atomic>
#include <random>
#include <array>
#include <string_view>

namespace jc {
    std::atomic<bool> g_isWaitingForInput{ false };
    extern std::string g_workspacePath;
    extern std::string g_cwd();

    inline std::filesystem::path to_path(const std::string& utf8_str) {
        return std::filesystem::path(reinterpret_cast<const char8_t*>(utf8_str.c_str()));
    }

    inline std::string from_path(const std::filesystem::path& p) {
        auto u8str = p.u8string();
        return std::string(u8str.begin(), u8str.end());
    }
}

// 信号处理
static std::atomic<int> g_sigintCount{ 0 };
static auto g_lastSigintTime = std::chrono::steady_clock::now();

extern jc::VM vm;

void saveWorkspace(const std::string& filename, bool silent = false);

void sigintHandler(int signum) {
    (void)signum;
    std::signal(SIGINT, sigintHandler); // 重新注册，防止某些平台恢复默认处理

    if (jc::g_isWaitingForInput.load(std::memory_order_relaxed)) {
        extern bool g_quiet;
        if (!g_quiet) std::cout << "\nGoodbye!" << std::endl;
        vm.shutdown();
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
        vm.shutdown();
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
    wchar_t buf[2048];
    if (GetModuleFileNameW(nullptr, buf, 2048)) {
        return jc::from_path(fs::path(buf).parent_path());
    }
#endif
    return jc::from_path(fs::current_path());
}

void printHelp() {
    jc::HelpRouter::printMainHelp();
}

void printHelpTopic(const std::string& topic) {
    jc::HelpRouter::printHelpTopic(topic);
}

// 核心 VM 实例和全局上下文
jc::VM vm;
bool g_showDisasm = false;  // ★ 新增：字节码反汇编开关
bool g_showIR = false;      // ★ 新增：IR 图打印开关
bool g_showHIR = false;     // ★ 新增：JIT HIR 图打印开关
bool g_showMachineCode = false; // ★ 新增：JIT 机器码打印开关
bool g_autoDebug = false;
bool g_profile = false;
bool g_quiet = false;
bool g_showNone = false;
bool g_silentRepl = false;
bool g_enableJit = false;

// ★ 消费编译器/VM 指令（行首 # 开头）。注册表：'!' 是 Shebang，no-op。
static void processDirectives(const std::vector<jc::Directive>& directives) {
    for (const auto& d : directives) {
        if (d.name == "!") continue;  // Shebang，no-op
        throw std::runtime_error("Compile Error: Unknown directive '#" + d.name + "'.");
    }
}

// ★ 执行一段任意多行/单行代码的统一接口
jc::Value evalCode(const std::string& code, const std::string& sourceFile, bool isFile = false) {
    jc::Lexer lexer(code, sourceFile);                       // ★
    auto tokens = lexer.tokenize();
    processDirectives(lexer.directives);
    jc::Parser parser(tokens, sourceFile);                   // ★
    auto ast = parser.parse();
    
    auto& fns = vm.getCompiledFunctions();
    size_t currentFnsSize = fns.size();
    
    jc::Resolver resolver;
    resolver.setKnownConstGlobals(&vm.getConstGlobals());
    resolver.resolve(ast.get());

    jc::IRGraph graph;
    jc::IRBuilder builder(&graph, &fns, nullptr, nullptr, &resolver.exprSymbols, &resolver.patternSymbols);
    builder.build(ast.get());
    
    if (g_showIR) graph.print(isFile ? "Script Unoptimized" : "REPL Unoptimized");
    
    jc::IROptimizer::optimize(&graph);
    if (g_showIR) graph.print(isFile ? "Script Optimized" : "REPL Optimized");
    
    jc::RegisterAllocator::allocate(&graph);
    if (g_showIR) graph.print(isFile ? "Script Allocated" : "REPL Allocated");
    
    jc::Chunk chunk;
    int localCount = jc::Emitter::emit(&graph, chunk);
    
    if (g_showDisasm) {
        for (size_t i = currentFnsSize; i < fns.size(); ++i) {
            std::string chunkName = fns[i]->name;
            chunkName = "Function: " + chunkName;
            fns[i]->chunk.disassemble(chunkName);
        }
        chunk.disassemble(isFile ? "Script Chunk" : "REPL Chunk");
    }
    
    return vm.execute(chunk, localCount);
}

void runScript(const std::string& filepath, bool isImport = false) {
    std::string resolvedPath = jc::helpers::safeResolvePath(filepath);
    if (!std::filesystem::exists(jc::to_path(resolvedPath))) {
        std::string jcbPath = jc::helpers::safeResolvePath(filepath + ".jcb");
        if (std::filesystem::exists(jc::to_path(jcbPath))) {
            resolvedPath = jcbPath;
        } else {
            resolvedPath = jc::helpers::safeResolvePath(filepath + ".jc2");
        }
    }
    if (!std::filesystem::exists(jc::to_path(resolvedPath))) {
        std::cerr << "   IO Error: Cannot open script '" << filepath << "'." << std::endl;
        return;
    }

    std::string ext = jc::from_path(jc::to_path(resolvedPath).extension());
    if (ext == ".jcb") {
        bool fallbackToSource = false;
        jc::helpers::g_scriptDirStack.push_back(jc::from_path(jc::to_path(resolvedPath).parent_path()));
        try {
            auto modFn = jc::BytecodeSerializer::loadJCB(resolvedPath, &vm);
            int fnIdx = static_cast<int>(vm.getCompiledFunctions().size()) - 1;
            jc::Value result = vm.callVMFunction(fnIdx, {});
            if (!result.isNone()) {
                vm.setGlobal("ANS", result);
            }
        } catch (const jc::EngineInterruptError&) {
            if (isImport) throw;
            std::cerr << "\n^C KeyboardInterrupt in script '" << resolvedPath << "'" << std::endl;
        } catch (const std::runtime_error& ex) {
            std::string msg = ex.what();
            if (msg == "JCB_MAGIC_MISMATCH" || msg == "JCB_VERSION_MISMATCH") {
                fallbackToSource = true;
            } else {
                if (isImport) throw;
                std::cerr << "\n" << jc::col(jc::Ansi::BRIGHT_RED)
                    << "In '" << resolvedPath << "':\n"
                    << ex.what() << std::endl;
            }
        } catch (const std::exception& ex) {
            if (isImport) throw;
            std::cerr << "\n" << jc::col(jc::Ansi::BRIGHT_RED)
                << "In '" << resolvedPath << "':\n"
                << ex.what() << std::endl;
        }
        jc::helpers::g_scriptDirStack.pop_back();
        
        if (fallbackToSource) {
            std::string jc2Path = jc::from_path(jc::to_path(resolvedPath).replace_extension(".jc2"));
            if (std::filesystem::exists(jc::to_path(jc2Path))) {
                resolvedPath = jc2Path;
            } else {
                std::cerr << "   VM Error: Bytecode version mismatch and source file not found for '" << resolvedPath << "'." << std::endl;
                return;
            }
        } else {
            return;
        }
    }

    std::ifstream file(jc::to_path(resolvedPath));
    if (!file.is_open()) {
        std::cerr << "   IO Error: Cannot open script '" << filepath << "'." << std::endl;
        return;
    }

    // ★ 一次性读取整个文件
    std::string code, line;
    while (std::getline(file, line)) code += line + "\n";
    file.close();

    jc::helpers::g_scriptDirStack.push_back(
        jc::from_path(jc::to_path(resolvedPath).parent_path()));

    try {
        // ★ 将 resolvedPath 传进虚拟机
        jc::Value result = evalCode(code, resolvedPath, true);
        if (!result.isNone()) {
            vm.setGlobal("ANS", result);
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
            << "In '" << resolvedPath << "':\n"
            << ex.what() << std::endl;
    }

    jc::helpers::g_scriptDirStack.pop_back();
}

std::string getWorkspaceDir() {
    if (jc::g_workspacePath.empty()) {
        auto u8str = std::filesystem::current_path().u8string();
        return std::string(u8str.begin(), u8str.end());
    }
    return jc::g_workspacePath;
}

void saveWorkspace(const std::string& filename, bool silent) {
    namespace fs = std::filesystem;

    std::string wp = getWorkspaceDir();

    fs::path dir = jc::to_path(wp);
    if (!fs::exists(dir)) fs::create_directories(dir);
    std::string path = jc::from_path(dir / jc::to_path(filename + ".jcw"));

    try {
        jc::BytecodeSerializer::saveJCW(path, &vm);
        if (!silent) std::cout << "Saved workspace snapshot to " << path << std::endl;
    } catch (const std::exception& e) {
        if (!silent) std::cerr << "Failed to save workspace: " << e.what() << std::endl;
    }
}

void listWorkspaces() {
    namespace fs = std::filesystem;
    std::string wp = getWorkspaceDir();
    fs::path dir = jc::to_path(wp);
    
    if (!fs::exists(dir)) {
        std::cout << "No workspaces found.\n";
        return;
    }
    
    std::cout << "Available workspaces:\n";
    int count = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".jcw") {
            std::cout << "  - " << jc::from_path(entry.path().stem()) << "\n";
            count++;
        }
    }
    if (count == 0) std::cout << "  (none)\n";
}

void loadWorkspace(const std::string& arg, bool silent = false, bool merge = false, bool infoOnly = false) {
    namespace fs = std::filesystem;
    fs::path targetPath;
    bool isExplicitPath = false;

    if (arg.find('/') != std::string::npos || arg.find('\\') != std::string::npos || (arg.length() >= 4 && arg.substr(arg.length() - 4) == ".jcw")) {
        isExplicitPath = true;
        targetPath = jc::to_path(arg);
        if (!targetPath.is_absolute()) {
            targetPath = jc::to_path(jc::g_cwd()) / targetPath;
        }
        if (targetPath.extension() != ".jcw") {
            targetPath += ".jcw";
        }
    } else {
        std::string wp = getWorkspaceDir();
        targetPath = jc::to_path(wp) / jc::to_path(arg + ".jcw");
    }

    if (!fs::exists(targetPath)) { 
        if (!silent) std::cerr << "IO Error: Workspace not found at " << jc::from_path(targetPath) << ".\n"; 
        return; 
    }

    try {
        jc::BytecodeSerializer::loadJCW(jc::from_path(targetPath), &vm, merge, infoOnly);
        if (infoOnly) return;
        if (!silent) {
            if (merge) std::cout << "Workspace merged from " << jc::from_path(targetPath) << std::endl;
            else std::cout << "Workspace loaded from " << jc::from_path(targetPath) << std::endl;
        }
        
        if (isExplicitPath) {
            auto u8str = fs::weakly_canonical(targetPath.parent_path()).u8string();
            jc::g_workspacePath = std::string(u8str.begin(), u8str.end());
            if (!silent) std::cout << "Workspace directory changed to " << jc::g_workspacePath << std::endl;
        }
    } catch (const std::exception& e) {
        if (!silent) std::cerr << "Failed to load workspace: " << e.what() << std::endl;
    }
}

void deleteWorkspace(const std::string& arg, bool silent = false) {
    namespace fs = std::filesystem;
    fs::path targetPath;

    if (arg.find('/') != std::string::npos || arg.find('\\') != std::string::npos || (arg.length() >= 4 && arg.substr(arg.length() - 4) == ".jcw")) {
        targetPath = jc::to_path(arg);
        if (!targetPath.is_absolute()) {
            targetPath = jc::to_path(jc::g_cwd()) / targetPath;
        }
        if (targetPath.extension() != ".jcw") {
            targetPath += ".jcw";
        }
    } else {
        std::string wp = getWorkspaceDir();
        targetPath = jc::to_path(wp) / jc::to_path(arg + ".jcw");
    }

    if (!fs::exists(targetPath)) { 
        if (!silent) std::cerr << "IO Error: Workspace not found at " << jc::from_path(targetPath) << ".\n"; 
        return; 
    }

    try {
        fs::remove(targetPath);
        if (!silent) std::cout << "Workspace deleted: " << jc::from_path(targetPath) << std::endl;
    } catch (const std::exception& e) {
        if (!silent) std::cerr << "Failed to delete workspace: " << e.what() << std::endl;
    }
}

int runTestSuite(const std::string& testPath, const std::string& exeDir) {
    namespace fs = std::filesystem;
    fs::path targetPath = testPath.empty() ? jc::to_path(exeDir) / "tests" : jc::to_path(testPath);

    if (!fs::exists(targetPath) || !fs::is_directory(targetPath)) {
        std::cerr << jc::col(jc::Ansi::BRIGHT_RED) << "Test Error: Directory not found -> " << jc::from_path(targetPath) << jc::col(jc::Ansi::RESET) << std::endl;
        return 1;
    }

    std::cout << jc::col(jc::Ansi::BRIGHT_CYAN) << "=========================================\n"
              << "JC2 Test Suite Started\n"
              << "Target: " << jc::from_path(targetPath) << "\n"
              << "=========================================\n" << jc::col(jc::Ansi::RESET);

    int total = 0;
    int passed = 0;
    int failed = 0;
    std::vector<std::string> failedFiles;

    // 收集所有测试文件以保证顺序稳定
    std::vector<fs::path> testFiles;
    for (const auto& entry : fs::recursive_directory_iterator(targetPath)) {
        if (entry.is_regular_file() && jc::from_path(entry.path().extension()) == ".jc2") {
            testFiles.push_back(entry.path());
        }
    }
    std::sort(testFiles.begin(), testFiles.end());

    for (const auto& path : testFiles) {
        total++;
        std::string filepath = jc::from_path(path);
        std::string filename = jc::from_path(path.filename());

        // Reset Environment
        vm.clearGlobals();
        vm.setGlobal("PI", jc::Value(3.14159265358979323846));
        vm.setGlobal("E", jc::Value(2.71828182845904523536));
        vm.setGlobal("i", jc::Value(jc::Complex(0.0, 1.0)));
        vm.setGlobal("I", jc::Value(jc::Complex(0.0, 1.0)));
        vm.setGlobal("ANS", jc::Value::none());
        jc::helpers::g_scriptDirStack.clear();

        std::cout << jc::col(jc::Ansi::BRIGHT_BLUE) << "\n[TEST] Running " << filename << "..." << jc::col(jc::Ansi::RESET) << std::endl;

        std::ifstream file(path);
        if (!file.is_open()) {
            std::cout << jc::col(jc::Ansi::BRIGHT_RED) << "  -> [FAIL] (IO Error)" << jc::col(jc::Ansi::RESET) << std::endl;
            failed++;
            failedFiles.push_back(filename + " (IO Error)");
            continue;
        }

        std::string code, line;
        while (std::getline(file, line)) code += line + "\n";
        file.close();

        jc::helpers::g_scriptDirStack.push_back(jc::from_path(path.parent_path()));

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
    jc::VM::activeVM = &vm;
    jc::BuiltinRegistry registry;
    registry.registerAll();
    for (const auto& [name, fn] : registry.getBuiltins()) {
        const auto& arities = registry.getArity().find(name)->second;
        const auto& paramNames = registry.getParamNames().find(name)->second;
        const auto& restName = registry.getRestName().find(name)->second;
        const auto& kwargNames = registry.getKwargNames().find(name)->second;
        const auto& kwargsName = registry.getKwargsName().find(name)->second;
        int kwargDefaultCount = registry.getKwargDefaultCount().find(name)->second;
        const auto& kwargDefaultValueTexts = registry.getKwargDefaultValueTexts().find(name)->second;
        vm.registerBuiltin(name, fn, arities, paramNames, restName, kwargNames, kwargsName, kwargDefaultCount, kwargDefaultValueTexts);
        // ★ 我们把内置方法只留给原生表处理！彻底释放 Globals 字典空间供用户自由重载调用！
    }

    // 初始化系统常量 (普通赋予即可！它无法通过系统的 delete 指令销毁，但你能将 PI 暂时盖为别的值)
    vm.setGlobal("PI", jc::Value(3.14159265358979323846));
    vm.setGlobal("E", jc::Value(2.71828182845904523536));
    vm.setGlobal("i", jc::Value(jc::Complex(0.0, 1.0)));
    vm.setGlobal("I", jc::Value(jc::Complex(0.0, 1.0)));
    vm.setGlobal("ANS", jc::Value::none());

    // 绑定虚拟机外包服务给系统级运行时回调！
    jc::helpers::setGlobalCallback = [](const std::string& name, const jc::Value& val) { 
        vm.setGlobal(name, val); 
    };
    jc::helpers::evalCallback = [](const std::string& code) -> jc::Value { return evalCode(code, "<eval>", false); };
    jc::helpers::runFileCallback = [](const std::string& path) { runScript(path, true); };
    jc::helpers::callFunctionCallback = [](jc::ObjClosure* closure, const std::vector<jc::Value>& args) -> jc::Value {
        if (closure->isBytecode()) {
            jc::Value s = !jc::helpers::nativeSelfStack.empty() ? jc::helpers::nativeSelfStack.back() : closure->boundSelf;
            jc::Value c = !jc::helpers::nativeClassStack.empty() ? jc::helpers::nativeClassStack.back() : closure->boundClass;
            return vm.callVMFunction(closure->compiledFnIndex, args, closure, s, c);
        }
        if (closure->nativeFn.has_value()) {
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
        }
        throw std::runtime_error("VM Error: Invalid closure in callback.");
    };
    jc::helpers::callValueCallback = [](const jc::Value& callee, const std::vector<jc::Value>& args) -> jc::Value {
        if (callee.isFunctionClosure()) {
            return jc::helpers::callFunctionCallback(callee.asFunction(), args);
        }
        if (callee.isType()) {
            jc::ObjTypeDef* td = static_cast<jc::ObjTypeDef*>(callee.asObj());
            if (td->converter) return td->converter(args);
            throw std::runtime_error("TypeError: This type object is not callable.");
        }
        if (callee.isClass()) {
            jc::ObjClass* cls = static_cast<jc::ObjClass*>(callee.asObj());
            if (cls->native_allocator) return cls->native_allocator(args);
            auto instance = jc::GcHeap::get().allocate<jc::ObjInstance>();
            jc::Value res(instance);
            jc::GcValueGuard guard(res);
            instance->classDef = cls;
            jc::ObjClosure* initMethod = nullptr;
            auto c = cls;
            while (c) {
                auto it = c->properties.find("<init>");
                if (it != c->properties.end() && it->second.val.isFunctionClosure()) {
                    initMethod = it->second.val.asFunction();
                    break;
                }
                c = c->parent;
            }
            if (initMethod) {
                if (initMethod->isBytecode()) {
                    vm.callVMFunction(initMethod->compiledFnIndex, args, initMethod, res, jc::Value(cls));
                } else if (initMethod->isNative()) {
                    jc::helpers::nativeSelfStack.push_back(res);
                    jc::helpers::nativeClassStack.push_back(jc::Value(cls));
                    auto& fn = std::any_cast<jc::NativeCallable&>(initMethod->nativeFn);
                    fn(args);
                    jc::helpers::nativeSelfStack.pop_back();
                    jc::helpers::nativeClassStack.pop_back();
                }
            }
            return res;
        }
        if (callee.isInstance()) {
            auto [method, owner] = vm.findDunder(callee, "__call__");
            if (method) return vm.callDunder(callee, method, owner, args);
            throw std::runtime_error("TypeError: Instance is not callable (no __call__).");
        }
        throw std::runtime_error("VM Error: Value is not callable.");
    };
    jc::helpers::resolvePathCallback = [exeDir](const std::string& path) -> std::string {
        namespace fs = std::filesystem;
        fs::path p(path);
        if (p.is_absolute()) return p.string();
        std::vector<std::string> c = {
            fs::weakly_canonical(fs::current_path() / p).string(),
            (fs::path(exeDir) / p).string(),
            (fs::path(exeDir) / "modules" / p).string(),
            (fs::path(exeDir) / "lib" / p).string()
        };
        for (const auto& cp : c) if (fs::exists(cp)) return cp;
        return fs::weakly_canonical(fs::current_path() / p).string();
        };

    // ★ 清洁版命令行参数解析 (Subcommand 架构)
    std::string command = "";
    std::string scriptPath = "";
    std::string evalStr = "";
    bool runTests = false;
    std::string testPath = "";
    bool compileMode = false;
    std::string compileInput = "";
    std::string compileOutput = "";
    bool stripDebug = false;
    bool compileAsModule = false;
    bool fmtMode = false;
    bool fmtCheck = false;
    std::string fmtPath = "";
    bool lspMode = false;
    bool loadMode = false;
    std::string loadTarget = "";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        // 1. 全局修饰符 (Flags)
        if (arg == "-q" || arg == "--quiet") { g_quiet = true; continue; }
        if (arg == "--stdio") { continue; } // 兼容 VSCode LanguageClient 自动追加的参数
        if (arg == "-d") { g_showDisasm = true; continue; }
        if (arg == "--ir") { g_showIR = true; continue; }
        if (arg == "--hir") { g_showHIR = true; continue; }
        if (arg == "--mc") { g_showMachineCode = true; continue; }
        if (arg == "--debug") { g_autoDebug = true; continue; }
        if (arg == "--profile") { g_profile = true; std::cout << "Profiler enabled.\n"; continue; }
        if (arg == "--jit") { g_enableJit = true; continue; }

        // 2. 兼容旧版横杠主命令 (转换为子命令)
        if (arg == "--help" || arg == "-h") { command = "help"; continue; }
        if (arg == "--version" || arg == "-v") { command = "version"; continue; }
        if (arg == "--compile" || arg == "-c") { command = "compile"; compileMode = true; continue; }
        if (arg == "--test") { command = "test"; runTests = true; continue; }
        if (arg == "-e" || arg == "--eval") { command = "eval"; continue; }
        if (arg == "--run") { command = "run"; continue; }
        if (arg == "fmt") { command = "fmt"; fmtMode = true; continue; }

        // 3. 识别子命令 (如果尚未确定 command 且当前参数不是横杠开头)
        if (command.empty() && arg[0] != '-') {
            if (arg == "compile" || arg == "test" || arg == "eval" || arg == "help" || arg == "version" || arg == "run" || arg == "repl" || arg == "fmt" || arg == "lsp" || arg == "load") {
                command = arg;
                if (command == "compile") compileMode = true;
                if (command == "test") runTests = true;
                if (command == "fmt") fmtMode = true;
                if (command == "lsp") lspMode = true;
                if (command == "load") loadMode = true;
                continue;
            } else {
                // 智能 Fallback：根据后缀判断是 load 工作区还是 run 脚本
                if (arg.length() >= 4 && arg.substr(arg.length() - 4) == ".jcw") {
                    command = "load";
                    loadMode = true;
                    loadTarget = arg;
                } else {
                    command = "run";
                    scriptPath = arg;
                }
                continue;
            }
        }

        // 4. 处理子命令的特定参数
        if (command == "compile") {
            if (arg == "-o") {
                if (i + 1 < argc) compileOutput = argv[++i];
                else { std::cerr << "Error: -o requires an output file.\n"; return 1; }
            }
            else if (arg == "--strip" || arg == "-s") stripDebug = true;
            else if (arg == "--module" || arg == "-m") compileAsModule = true;
            else if (compileInput.empty() && arg[0] != '-') compileInput = arg;
            else if (compileOutput.empty() && arg[0] != '-') compileOutput = arg;
            else { std::cerr << "Unknown argument for compile: " << arg << "\n"; return 1; }
        }
        else if (command == "test") {
            if (testPath.empty() && arg[0] != '-') testPath = arg;
            else { std::cerr << "Unknown argument for test: " << arg << "\n"; return 1; }
        }
        else if (command == "eval") {
            if (evalStr.empty()) evalStr = arg;
            else { std::cerr << "Unknown argument for eval: " << arg << "\n"; return 1; }
        }
        else if (command == "help") {
            if (arg[0] != '-') {
                printHelpTopic(arg);
                return 0;
            }
        }
        else if (command == "run") {
            if (scriptPath.empty() && arg[0] != '-') scriptPath = arg;
            else { std::cerr << "Unknown argument for run: " << arg << "\n"; return 1; }
        }
        else if (command == "fmt") {
            if (arg == "--check") fmtCheck = true;
            else if (fmtPath.empty() && arg[0] != '-') fmtPath = arg;
            else { std::cerr << "Unknown argument for fmt: " << arg << "\n"; return 1; }
        }
        else if (command == "load") {
            if (loadTarget.empty() && arg[0] != '-') loadTarget = arg;
            else { std::cerr << "Unknown argument for load: " << arg << "\n"; return 1; }
        }
        else if (command == "repl") {
            std::cerr << "Unknown argument for repl: " << arg << "\n"; return 1;
        }
        else {
            std::cerr << "Unknown argument: " << arg << "\n"; return 1;
        }
    }

    // 5. 执行无参数的立即命令
    if (command == "help") { printHelp(); return 0; }
    if (command == "version") { std::cout << "Junk Calculator 2.6.2.0\n"; return 0; }
    if (command == "compile" && compileInput.empty()) { std::cerr << "Error: compile requires an input file.\n"; return 1; }
    if (command == "eval" && evalStr.empty()) { std::cerr << "Error: eval requires an argument.\n"; return 1; }
    if (command == "load" && loadTarget.empty()) { std::cerr << "Error: load requires a workspace name or path.\n"; return 1; }

    // 如果有 lsp 参数，则启动 LSP 服务器并退出
    if (lspMode) {
        jc::lsp::LspServer server;
        server.run();
        return 0;
    }

    // 如果有 fmt 参数，则执行格式化并退出
    if (fmtMode) {
        if (fmtPath.empty()) fmtPath = ".";
        namespace fs = std::filesystem;
        fs::path targetPath = jc::to_path(fmtPath);
        if (!fs::exists(targetPath)) {
            std::cerr << "Error: Path not found -> " << fmtPath << "\n";
            return 1;
        }

        std::vector<fs::path> filesToFormat;
        if (fs::is_directory(targetPath)) {
            for (const auto& entry : fs::recursive_directory_iterator(targetPath)) {
                if (entry.is_regular_file() && jc::from_path(entry.path().extension()) == ".jc2") {
                    filesToFormat.push_back(entry.path());
                }
            }
        } else {
            filesToFormat.push_back(targetPath);
        }

        int unformattedCount = 0;
        for (const auto& path : filesToFormat) {
            std::ifstream file(path);
            if (!file.is_open()) continue;
            std::string code((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            file.close();

            std::string formatted = jc::Formatter::format(code);
            if (code != formatted) {
                unformattedCount++;
                if (!fmtCheck) {
                    std::ofstream out(path);
                    out << formatted;
                    out.close();
                    std::cout << "Formatted: " << jc::from_path(path) << "\n";
                } else {
                    std::cout << "Needs formatting: " << jc::from_path(path) << "\n";
                }
            }
        }

        if (fmtCheck) {
            if (unformattedCount > 0) {
                std::cerr << unformattedCount << " file(s) need formatting.\n";
                return 1;
            } else {
                std::cout << "All files are formatted correctly.\n";
                return 0;
            }
        } else {
            std::cout << "Formatting complete. " << unformattedCount << " file(s) changed.\n";
            return 0;
        }
    }

    // 如果有 --compile 参数，则执行编译并退出
    if (compileMode) {
        if (compileOutput.empty()) {
            std::filesystem::path p = jc::to_path(compileInput);
            p.replace_extension(".jcb");
            compileOutput = jc::from_path(p);
        }
        std::string resolvedPath = jc::helpers::safeResolvePath(compileInput);
        if (!std::filesystem::exists(jc::to_path(resolvedPath)))
            resolvedPath = jc::helpers::safeResolvePath(compileInput + ".jc2");
        if (!std::filesystem::exists(jc::to_path(resolvedPath))) {
            std::cerr << "IO Error: Cannot open script '" << compileInput << "'." << std::endl;
            return 1;
        }
        std::ifstream file(jc::to_path(resolvedPath));
        if (!file.is_open()) {
            std::cerr << "IO Error: Cannot open script '" << compileInput << "'." << std::endl;
            return 1;
        }
        std::string code, line;
        while (std::getline(file, line)) code += line + "\n";
        file.close();

        try {
            jc::Lexer lexer(code, resolvedPath);
            auto tokens = lexer.tokenize();
            processDirectives(lexer.directives);
            jc::Parser parser(tokens, resolvedPath);
            auto ast = parser.parse();
            
            auto& fns = vm.getCompiledFunctions();
            int startIndex = static_cast<int>(fns.size());
            
            std::string baseName = jc::from_path(jc::to_path(resolvedPath).stem());
            std::unique_ptr<jc::NamespaceDecl> nsDecl;
            jc::Expr* targetAst = ast.get();
            
            if (compileAsModule) {
                nsDecl = std::make_unique<jc::NamespaceDecl>(jc::Token(jc::TokenType::IDENTIFIER, baseName, 0), std::move(ast));
                targetAst = nsDecl.get();
            }

            jc::Resolver resolver;
            resolver.resolve(targetAst);

            auto modFn = std::make_shared<jc::CompiledFunction>();
            modFn->name = compileAsModule ? ("<module " + baseName + ">") : "<script>";
            modFn->sourceFile = resolvedPath;
            modFn->arity = 0;
            modFn->maxArity = 0;
            modFn->restName = "";

            jc::IRGraph graph;
            jc::IRBuilder builder(&graph, &fns, nullptr, modFn.get(), &resolver.exprSymbols, &resolver.patternSymbols);
            builder.build(targetAst);
            
            jc::IROptimizer::optimize(&graph);
            jc::RegisterAllocator::allocate(&graph);
            
            for (auto& target : builder.upvalueTargets) {
                if (target.isLocal && target.localNode) {
                    jc::IRNode* localNode = target.localNode;
                    int upvalIdx = target.index;
                    jc::CompiledFunction* childFn = modFn.get();
                    graph.postAllocCallbacks.push_back([childFn, upvalIdx, localNode]() {
                        childFn->upvalues[upvalIdx].index = localNode->physicalReg;
                    });
                }
            }

            modFn->localCount = jc::Emitter::emit(&graph, modFn->chunk);
            fns.push_back(modFn);
            
            int count = static_cast<int>(fns.size()) - startIndex;
            jc::BytecodeSerializer::saveJCB(compileOutput, &vm, startIndex, count, stripDebug);
            std::cout << "Successfully compiled '" << compileInput << "' to '" << compileOutput << "'." << std::endl;
            if (stripDebug) std::cout << "Debug information (line numbers, source paths) was stripped." << std::endl;
        } catch (const std::exception& ex) {
            std::cerr << "Compilation Error:\n" << ex.what() << std::endl;
            vm.shutdown();
            return 1;
        }
        vm.shutdown();
        return 0;
    }

    // 如果有 --test 参数，则执行测试套件并退出
    if (runTests) {
        int res = runTestSuite(testPath, exeDir);
        vm.shutdown();
        return res;
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
            std::cerr << e.what() << std::endl;
            if (g_profile) vm.printProfileInfo();
            vm.shutdown();
            return 1;
        }
        if (g_profile) vm.printProfileInfo();
        vm.shutdown();
        return 0;
    }

    // 有脚本路径则执行脚本并退出
    if (!scriptPath.empty()) {
        runScript(scriptPath);
        if (g_profile) vm.printProfileInfo();
        vm.shutdown();
        return 0;
    }

    auto printBannerTop = []() {
        std::cout << jc::col(jc::Ansi::BRIGHT_CYAN)
            << "=================================================\n"
            << "   Junk Calculator 2.6.2.0\n"
            << "   Developed by Yu Liangyang, Tsinghua University\n"
            << "=================================================\n" << jc::col(jc::Ansi::RESET);
    };

    auto printBannerBottom = []() {
        std::cout << "Type " << jc::col(jc::Ansi::BRIGHT_YELLOW) << "'/help'" << jc::col(jc::Ansi::RESET) << " for a list of commands." << std::endl;
    };

    auto printBanner = [&]() {
        printBannerTop();
        printBannerBottom();
    };

    if (!g_quiet) printBannerTop();

    if (loadMode) {
        loadWorkspace(loadTarget, g_quiet);
    }

    if (!g_quiet) printBannerBottom();

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
                vm.shutdown();
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
                    else if (c == 'r' && i + 1 < s.length() && (s[i + 1] == '"' || s[i + 1] == '\'')) {
                        // raw string：r"..." / r'...' / r"TAG(...)TAG"，不能把内容里的引号当字符串分隔符
                        char rq = s[i + 1];
                        i += 2;
                        size_t tagStart = i;
                        while (i < s.length() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_')) i++;
                        if (i < s.length() && s[i] == '(') {
                            std::string delimiter = s.substr(tagStart, i - tagStart);
                            std::string endMarker = ")" + delimiter + rq;
                            size_t pos = s.find(endMarker, i);
                            if (pos != std::string::npos) i = pos + endMarker.length() - 1;
                            else { inStr = true; strQuote = rq; break; }
                        }
                        else {
                            size_t pos = s.find(rq, i);
                            if (pos != std::string::npos) i = pos;
                            else { inStr = true; strQuote = rq; break; }
                        }
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

        // 以 / 或 // 开头（命令/单行注释）且不在任何括号里时，不开启续行模式（否则 /help in 会被 "in" 误判为续行）。
        // 注意：/* 是多行注释开头，仍需续行等待 */。
        bool slashInput = !input.empty() && input[0] == '/' && !(input.length() >= 2 && input[1] == '*');
        bool openBrackets = braces > 0 || parens > 0 || brackets > 0;

        bool inputAborted = false;
        bool isEof = false;
        while (!(slashInput && !openBrackets) && (braces > 0 || parens > 0 || brackets > 0 || (inStr && isMulti) || commentNesting > 0 || (!inStr && commentNesting == 0 && endsWithContinuation(input)))) {
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
            vm.shutdown();
            std::exit(1);
        }
        if (inputAborted) continue;

        if (input.length() >= 2 && input[0] == '/' && input[1] == '/') continue;

        if (!input.empty() && input[0] == '/' && (input.length() == 1 || (input[1] != '/' && input[1] != '*'))) {
            // ★ 统一开关命令簇 /set <switch> on|off
            auto applySwitch = [&](const std::string& name, bool on) -> bool {
                if (name == "jit") { g_enableJit = on; }
                else if (name == "debug") { g_autoDebug = on; }
                else if (name == "ir") { g_showIR = on; }
                else if (name == "hir") { g_showHIR = on; }
                else if (name == "mc") { g_showMachineCode = on; }
                else if (name == "disasm") { g_showDisasm = on; }
                else if (name == "profile") { g_profile = on; }
                else if (name == "color") { jc::colorsEnabled = on; }
                else if (name == "none") { g_showNone = on; }
                else if (name == "silent") { g_silentRepl = on; }
                else return false;
                return true;
            };

            if (input.substr(0, 5) == "/set ") {
                std::string rest = input.substr(5);
                size_t sp = rest.find(' ');
                if (sp == std::string::npos) { std::cout << "Usage: /set <switch> on|off\n"; continue; }
                std::string name = rest.substr(0, sp);
                std::string val = rest.substr(sp + 1);
                size_t vs = val.find_first_not_of(" \t");
                if (vs != std::string::npos) val = val.substr(vs);
                bool on;
                if (val == "on") on = true;
                else if (val == "off") on = false;
                else { std::cout << "Usage: /set <switch> on|off\n"; continue; }
                if (!applySwitch(name, on)) {
                    std::cout << "Unknown switch '" << name << "'. Available: jit, debug, ir, hir, mc, disasm, profile, color, none, silent\n";
                } else {
                    std::cout << "Switch '" << name << "' " << (on ? "enabled" : "disabled") << ".\n";
                }
                continue;
            }
            // 高频别名
            if (input == "/jit on") { applySwitch("jit", true); std::cout << "JIT Compilation enabled.\n"; continue; }
            if (input == "/jit off") { applySwitch("jit", false); std::cout << "JIT Compilation disabled.\n"; continue; }
            if (input == "/debug on") { applySwitch("debug", true); std::cout << "Interactive Step-Debugger enabled.\n"; continue; }
            if (input == "/debug off") { applySwitch("debug", false); std::cout << "Interactive Step-Debugger disabled.\n"; continue; }
            if (input == "/exit" || input == "/quit") break;
            if (input == "/help") { printHelp(); continue; }
            if (input == "/version") { std::cout << "Junk Calculator 2.6.2.0\n"; continue; }
            if (input.substr(0, 6) == "/help ") { printHelpTopic(input.substr(6)); continue; }
            if (input == "/clear") { 
                vm.clearGlobals(); 
                vm.setGlobal("PI", jc::Value(3.14159265358979323846));
                vm.setGlobal("E", jc::Value(2.71828182845904523536));
                vm.setGlobal("i", jc::Value(jc::Complex(0.0, 1.0)));
                vm.setGlobal("I", jc::Value(jc::Complex(0.0, 1.0)));
                vm.setGlobal("ANS", jc::Value::none());
                std::cout << "All variables cleared (system constants and types restored).\n"; 
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
            if (input == "/pwd") {
                std::cout << "  Script dir:    " << jc::g_cwd() << std::endl;
                std::cout << "  Workspace dir: " << getWorkspaceDir() << std::endl;
                continue;
            }
            if (input.substr(0, 4) == "/ws ") {
                std::string cmd = input.substr(4);
                size_t s = cmd.find_first_not_of(" \t");
                if (s != std::string::npos) cmd = cmd.substr(s);
                else cmd = "";

                if (cmd == "list") {
                    listWorkspaces();
                } else if (cmd == "pwd") {
                    std::cout << "  Script dir:    " << jc::g_cwd() << std::endl;
                    std::cout << "  Workspace dir: " << getWorkspaceDir() << std::endl;
                } else if (cmd.substr(0, 4) == "set ") {
                    std::string path = cmd.substr(4);
                    size_t ps = path.find_first_not_of(" \t");
                    if (ps != std::string::npos) path = path.substr(ps);
                    
                    if (path == "default") {
                        jc::g_workspacePath = "";
                    } else {
                        namespace fs = std::filesystem;
                        fs::path dir = jc::to_path(path);
                        if (!dir.is_absolute()) dir = jc::to_path(jc::g_cwd()) / dir;
                        if (!fs::exists(dir)) fs::create_directories(dir);
                        auto u8str = fs::weakly_canonical(dir).u8string();
                        jc::g_workspacePath = std::string(u8str.begin(), u8str.end());
                    }
                    std::cout << "  Script dir:    " << jc::g_cwd() << std::endl;
                    std::cout << "  Workspace dir: " << getWorkspaceDir() << std::endl;
                } else if (cmd.substr(0, 5) == "save ") {
                    std::string name = cmd.substr(5);
                    s = name.find_first_not_of(" \t");
                    if (s != std::string::npos) name = name.substr(s);
                    saveWorkspace(name.empty() ? "default" : name);
                } else if (cmd == "save") {
                    saveWorkspace("default");
                } else if (cmd.substr(0, 5) == "load ") {
                    std::string name = cmd.substr(5);
                    s = name.find_first_not_of(" \t");
                    if (s != std::string::npos) name = name.substr(s);
                    loadWorkspace(name.empty() ? "default" : name);
                } else if (cmd == "load") {
                    loadWorkspace("default");
                } else if (cmd.substr(0, 6) == "merge ") {
                    std::string name = cmd.substr(6);
                    s = name.find_first_not_of(" \t");
                    if (s != std::string::npos) name = name.substr(s);
                    loadWorkspace(name.empty() ? "default" : name, false, true, false);
                } else if (cmd == "merge") {
                    loadWorkspace("default", false, true, false);
                } else if (cmd.substr(0, 5) == "info ") {
                    std::string name = cmd.substr(5);
                    s = name.find_first_not_of(" \t");
                    if (s != std::string::npos) name = name.substr(s);
                    loadWorkspace(name.empty() ? "default" : name, false, false, true);
                } else if (cmd == "info") {
                    loadWorkspace("default", false, false, true);
                } else if (cmd.substr(0, 7) == "delete ") {
                    std::string name = cmd.substr(7);
                    s = name.find_first_not_of(" \t");
                    if (s != std::string::npos) name = name.substr(s);
                    if (!name.empty()) deleteWorkspace(name);
                    else std::cerr << "Error: /ws delete requires a workspace name.\n";
                } else {
                    std::cout << "Unknown /ws command. Use: /ws save, /ws load, /ws merge, /ws info, /ws list, /ws delete, /ws pwd, /ws set\n";
                }
                continue;
            }
            if (input.substr(0, 8) == "/delete ") {
                std::string varName = input.substr(8);
                size_t s = varName.find_first_not_of(" \t");
                size_t e = varName.find_last_not_of(" \t");
                if (s != std::string::npos) varName = varName.substr(s, e - s + 1);
                else varName = "";
                
                if (!varName.empty() && varName[0] == '@') {
                    std::string mName = "<macro_" + varName.substr(1) + ">";
                    vm.removeGlobal(mName);
                    std::cout << "Macro '" << varName.substr(1) << "' forcefully deleted.\n";
                } else {
                    vm.removeGlobal(varName);
                    std::cout << "Variable '" << varName << "' forcefully deleted.\n";
                }
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
                          << "  language and virtual machine, built entirely from scratch.\n"
                          << "  GitHub: " << jc::col(jc::Ansi::BRIGHT_BLUE) << "https://github.com/YuDan256/JunkCalculator2\n\n"
                          << jc::col(jc::Ansi::WHITE) 
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
            if (input == "/gc") {
                jc::SymExpr::cleanupPool();
                if (jc::VM::activeVM) jc::VM::activeVM->setGlobal("ANS", jc::Value::none());
                int freed = jc::GcHeap::get().collectGarbage();
                std::cout << "Garbage collected: " << freed << " object(s) freed.\n";
                continue;
            }
            if (input == "/gcinfo") {
                auto& heap = jc::GcHeap::get();
                std::cout << "--- GC Status ---\n"
                          << "  Tracked objects:     " << heap.trackedCount() << "\n"
                          << "  Allocs since GC:     " << heap.allocsSinceGc() << "\n"
                          << "  Next GC threshold:   " << heap.threshold() << "\n"
                          << "-----------------\n";
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
                    "Gur Ertvfgre IZ vf gnxvat n pbssrr oernx. Cyrnfr glcr tragyl.",
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
                } else if (result.isObjType(jc::ObjType::SYM_MATRIX)) {
                    typeColor = jc::col(jc::Ansi::WHITE);
                    isTopLevelMatrix = true;
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
            if (g_profile) {
                vm.printProfileInfo();
                vm.clearProfileData();
            }
        }
        catch (const jc::EngineInterruptError&) {
            std::cerr << "^C KeyboardInterrupt" << std::endl;
            if (g_profile) {
                vm.printProfileInfo();
                vm.clearProfileData();
            }
        }
        catch (const std::exception& e) {
            std::cerr << jc::col(jc::Ansi::BRIGHT_RED)
                << e.what() << jc::col(jc::Ansi::RESET) << std::endl;
            if (g_profile) {
                vm.printProfileInfo();
                vm.clearProfileData();
            }
        }
    }

    if (!g_quiet) std::cout << "\nGoodbye!" << std::endl;
    if (g_profile) vm.printProfileInfo();
    vm.shutdown();
    return 0;
}

# JC2 C/C++ 原生扩展 API 文档

JC2 提供了一套高性能、零依赖、ABI 稳定的 C 语言接口 (`jc2_extension_api.h`)，以及一套现代且易用的 C++ 包装层 (`jc2_extension_cpp.h`)。通过这套 API，开发者可以编写动态链接库 (.dll / .so / .dylib) 或静态链接模块，将 C/C++ 的高性能代码、第三方库或系统级 API 暴露给 JC2 脚本。

## 1. 核心设计理念

*   **ABI 稳定**：底层完全基于纯 C 函数指针表 (`JC2_HostAPI`) 和不透明句柄 (`JC2_ValueHandle`)，跨编译器、跨版本兼容。
*   **零开销传值**：`JC2_ValueHandle` 完美契合 JC2 内部的 NaN-Boxing (64-bit) 机制，传递数字、布尔值和短字符串时没有任何堆分配开销。
*   **生命周期托管**：支持将 C++ 原生对象的指针绑定到 JC2 的类实例上，并注册析构回调，由 JC2 的垃圾回收器 (GC) 自动管理内存。

## 2. 快速入门 (C++ 包装层)

编写一个 JC2 扩展非常简单。你只需要包含 `jc2_extension_cpp.h`，实现你的原生函数，并在 `jc2_init` 中注册它们。

```cpp
#include "jc2_extension_cpp.h"

// 1. 定义原生函数
// 签名必须是: JC2_ValueHandle func(JC2_VMContext ctx, int argc, JC2_ValueHandle* argv, void* user_data)
JC2_ValueHandle my_add(JC2_VMContext ctx, int argc, JC2_ValueHandle* argv, void* user_data) {
    // 将 C 句柄包装为 C++ jc2::Value 对象
    jc2::Value a(argv[0]);
    jc2::Value b(argv[1]);

    // 类型检查
    if (!a.is_double() || !b.is_double()) {
        jc2::throw_error("TypeError: my_add expects two numbers.");
        return jc2::Value().get_handle(); // 返回 none
    }

    // 计算并返回新的句柄
    double result = a.as_double() + b.as_double();
    return jc2::Value(result).get_handle();
}

// 2. 模块初始化入口
int jc2_init(jc2::Module& mod) {
    // 注册函数: 名称, 函数指针, 最小参数, 最大参数, 是否接受变长参数
    mod.register_function("add", my_add, 2, 2, false);
    
    // 注册常量
    mod.register_double("PI_APPROX", 3.14);
    mod.register_string("AUTHOR", "JC2 Modder");

    // 注册 REPL 帮助文档
    mod.register_help("my_mod", "This is a custom math module.");
    
    return 0; // 返回 0 表示加载成功
}

// 3. 注入导出宏 (必须放在文件末尾或全局作用域)
JC2_EXTENSION_INIT
```

## 3. 核心 C++ 类参考

### `jc2::Value`
包装了底层的 `JC2_ValueHandle`，提供了便捷的类型检查和转换方法。

*   **构造函数**：
    *   `Value()`: 创建 `none`。
    *   `Value(bool)`, `Value(int32_t)`, `Value(double)`: 创建基本类型。
    *   `Value(const char*)`, `Value(const std::string&)`: 创建字符串。
*   **类型检查**：
    *   `is_none()`, `is_bool()`, `is_int()`, `is_double()`, `is_string()`, `is_instance()`
*   **值提取**：
    *   `as_bool()`, `as_int()`, `as_double()`, `as_string()`

### 高级内置类型 (`Complex`, `List`, `Dict`, `Set`, `Matrix`, `Function`)
除了基本类型，C++ 包装层还提供了对 JC2 高级内置类型的直接操作支持：

*   **`jc2::Complex`**：对应 JC2 的复数。支持 `real()`, `imag()`。
*   **`jc2::List`**：对应 JC2 的 `list`。支持 `push_back(val)`, `get(index)`, `size()`。
*   **`jc2::Dict`**：对应 JC2 的 `dict`。支持 `set(key, val)`, `get(key)`, `has(key)`, `size()`。
*   **`jc2::Set`**：对应 JC2 的 `set`。支持 `add(val)`, `remove(val)`, `has(val)`, `size()`。
*   **`jc2::RealMatrix`**：对应 JC2 的 `realmatrix`。支持 `get(row, col)`, `set(row, col, val)`, `rows()`, `cols()`。
*   **`jc2::ComplexMatrix`**：对应 JC2 的 `complexmatrix`。支持 `get_real(row, col)`, `get_imag(row, col)`, `set(row, col, r, i)`, `rows()`, `cols()`。
*   **`jc2::StringMatrix`**：对应 JC2 的 `stringmatrix`。支持 `get(row, col)`, `set(row, col, str)`, `rows()`, `cols()`。
*   **`jc2::Function`**：对应 JC2 的函数闭包。支持 `call(args_vector)`，允许你从 C++ 侧直接回调 JC2 脚本中的函数！

```cpp
// 示例：从 C++ 调用 JC2 传入的回调函数
JC2_ValueHandle map_array(JC2_VMContext ctx, int argc, JC2_ValueHandle* argv, void* user_data) {
    jc2::RealMatrix mat(argv[0]);
    jc2::Function callback(argv[1]);
    
    jc2::RealMatrix result(mat.rows(), mat.cols());
    for (int i = 0; i < mat.rows(); ++i) {
        for (int j = 0; j < mat.cols(); ++j) {
            // 调用 JC2 脚本函数
            jc2::Value ret = callback.call({ jc2::Value(mat.get(i, j)) });
            result.set(i, j, ret.as_double());
        }
    }
    return result.get_handle();
}
```

### `jc2::Module`
用于在模块加载时向 JC2 引擎注册内容。

*   `register_function(name, fn, min_arity, max_arity, has_rest, user_data)`: 注册全局函数。
*   `register_int(name, val)`, `register_double(...)`, `register_string(...)`: 注册全局变量/常量。
*   `register_help(topic, text)`: 注册 `/help <topic>` 的文档。
*   `register_function_help(name, signature, desc, example)`: 注册特定函数的帮助信息。

## 4. 面向对象与原生指针绑定 (OOP & Native Data)

你可以创建 JC2 类，设置继承关系，读写实例字段，并将 C++ 的原生对象（如文件句柄、网络套接字、图形窗口等）绑定到 JC2 的实例上。当 JC2 的实例被垃圾回收时，会自动调用你提供的 C++ 析构函数。

### 类继承与字段操作示例

```cpp
// 1. 创建父类和子类
jc2::Class animalClass("Animal");
jc2::Class dogClass("Dog");

// 2. 设置继承关系 (Dog extends Animal)
dogClass.set_parent(animalClass);

// 3. 在 C++ 中操作实例字段
JC2_ValueHandle dog_init(JC2_VMContext ctx, int argc, JC2_ValueHandle* argv, void* user_data) {
    jc2::Instance self(argv[0]);
    
    // 动态设置 JC2 实例的字段
    self.set("legs", jc2::Value(4));
    self.set("sound", jc2::Value("Woof!"));
    
    return jc2::Value().get_handle();
}
```

### 原生指针绑定示例

```cpp
#include "jc2_extension_cpp.h"
#include <iostream>

// 你的 C++ 原生类
class NativeFile {
public:
    NativeFile() { std::cout << "File opened.\n"; }
    ~NativeFile() { std::cout << "File closed.\n"; }
    void write(const std::string& text) { std::cout << "Writing: " << text << "\n"; }
};

// 析构回调函数 (供 JC2 GC 调用)
void native_file_dtor(void* ptr) {
    delete static_cast<NativeFile*>(ptr);
}

// JC2 构造函数包装
JC2_ValueHandle file_init(JC2_VMContext ctx, int argc, JC2_ValueHandle* argv, void* user_data) {
    jc2::Value self(argv[0]); // 方法调用的第一个参数通常是 self (实例本身)
    
    // 创建 C++ 对象
    NativeFile* file = new NativeFile();
    
    // 绑定到 JC2 实例，并注册析构器
    self.set_native_data(file, native_file_dtor);
    
    return jc2::Value().get_handle();
}

// JC2 方法包装
JC2_ValueHandle file_write(JC2_VMContext ctx, int argc, JC2_ValueHandle* argv, void* user_data) {
    jc2::Value self(argv[0]);
    jc2::Value text(argv[1]);
    
    // 提取绑定的 C++ 对象
    NativeFile* file = self.get_native_data<NativeFile>();
    if (file && text.is_string()) {
        file->write(text.as_string());
    }
    
    return jc2::Value().get_handle();
}

int jc2_init(jc2::Module& mod) {
    // 1. 创建类
    jc2::Class fileClass("File");
    
    // 2. 绑定方法
    fileClass.bind_method("init", file_init, 1, 1, false); // argc=1 (self)
    fileClass.bind_method("write", file_write, 2, 2, false); // argc=2 (self, text)
    
    // 3. 将类注册到模块中
    mod.register_value("File", fileClass);
    
    return 0;
}

JC2_EXTENSION_INIT
```

## 5. 异常处理

在原生函数中，如果遇到错误，应使用 `jc2::throw_error(msg)`。
**注意**：`throw_error` 内部会抛出 C++ 异常 (`std::runtime_error`)，该异常会被 JC2 引擎的 VM 捕获并转换为 JC2 脚本层的 Traceback 报错。因此，调用 `throw_error` 之后的 C++ 代码将不会被执行。

```cpp
if (!argv[0].is_string()) {
    jc2::throw_error("Type Error: Expected a string.");
    // 下面的代码不会执行
}
```

## 6. 纯 C API 结构 (`JC2_HostAPI`)

如果你使用纯 C 语言或其他语言（如 Rust, Zig, Go）编写扩展，你需要直接与 `JC2_HostAPI` 结构体交互。

引擎会在加载 DLL 时寻找并调用 `jc2_extension_init` 函数，并将 `JC2_HostAPI` 的指针传递给你。你需要保存这个指针，并通过它来调用引擎的功能（如 `api->make_string(ctx, "hello", 5)`）。具体函数签名请参考 `jc2_extension_api.h`。

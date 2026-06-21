# JC2 Test Suite Specification (测试规范)

为了保证 Junk Calculator 2 (JC2) 的稳定性和可维护性，所有位于 `tests/` 目录下的测试脚本应遵循以下统一规范。

## 1. 文件命名 (File Naming)
* 所有测试文件必须以 `test_` 开头，并以 `.jc2` 作为扩展名。
* 命名应简明扼要地反映测试的核心特性，例如：`test_match.jc2`, `test_matrix.jc2`。

## 2. 测试结构与隔离 (Structure & Isolation)
虽然测试运行器（Test Runner）会在每个文件执行前调用 `vm.clearGlobals()` 清空全局环境，但在**同一个文件内**的多个测试用例之间，应尽量避免全局变量污染。

**最佳实践**：将每个独立的测试用例封装在一个局部函数中，并在定义后立即调用。

```jc2
println("=== Testing Feature Name ===")

// 1. 测试子特性 A
test_feature_a() = {
    // 局部变量，不会污染后续测试
    a = 10
    b = 20
    assert(a + b == 30, "Feature A addition failed")
}
test_feature_a()

// 2. 测试子特性 B
test_feature_b() = {
    ...
}
test_feature_b()
```

## 3. 断言规范 (Assertions)
* **必须**使用内置的 `assert(condition, "Error message")` 函数进行状态验证。
* 错误信息（Error message）必须清晰、具体，指出是哪个具体的逻辑分支失败了。
* **不要**使用 `if (!cond) throw "error"`，统一使用 `assert`。

## 4. 异常与失败测试 (Testing Exceptions)
当需要验证某段代码**必须抛出异常**（例如语法错误、类型不匹配、越界等）时，请使用标准的 `try-catch` 拦截模式：

```jc2
test_expected_error() = {
    try {
        // 预期会失败的代码
        invalid_op = 1 / 0
        
        // 如果执行到了这里，说明没有抛出异常，测试失败！
        assert(false, "Should throw division by zero error")
    } catch (e) {
        // 可选：验证具体的错误信息内容
        assert("Division by zero" in e, "Wrong error message thrown")
    }
}
```

## 5. 控制台输出 (Console Output) — 测试套件必须静默

**核心原则：所有测试脚本应以完全静默模式运行。**

* **严格要求**：测试脚本内部**不允许任何 `println()` 输出**，包括开头的测试标题、进度信息等。
  - ❌ 禁止：`println("=== Testing XXX ===")`
  - ❌ 禁止：`println("[1/5] XXX OK")`
  - ❌ 禁止：任何中间调试打印

* **为什么**：测试运行器（Test Runner）会自动为每个测试文件打印统一格式的结果汇总：
  ```
  [TEST] Running test_xxx.jc2...
    -> [PASS] test_xxx.jc2
  ```
  任何内部输出都会破坏这个整洁的面板，降低可读性。

* **唯一例外**：仅在测试脚本故意验证输出功能时（如 `print()` 函数的测试），才允许使用 `println()`。此时应明确注释说明。

* **最佳实践**：
  ```jc2
  // ✓ 好的做法：静默执行，仅用 assert 验证
  test_feature_a() = {
      result = some_computation()
      assert(result == expected, "Feature A computation failed")
  }
  test_feature_a()
  
  // ✓ 好的做法：复杂测试可分段，使用描述性错误消息
  test_complex_feature() = {
      // 第 1 阶段
      x = 10
      assert(x > 0, "Setup: x should be positive")
      
      // 第 2 阶段
      y = transform(x)
      assert(y == 20, "Transform: should double the value")
  }
  test_complex_feature()
  
  // ✗ 坏的做法：打印进度信息
  test_bad() = {
      println("Testing feature...")  // ← 污染输出！
      assert(...)
      println("OK")                  // ← 污染输出！
  }
  ```

## 6. 自动化对比函数 (可选)
对于需要大量对比的测试（如模式匹配、解析器测试），可以在文件顶部定义一个辅助的 `check` 函数：

```jc2
check(name, actual, expected) = {
    assert(str(actual) == str(expected), f"[{name}] Expected {expected}, got {actual}")
}

check("Literal Match", match_func(200), "OK")
```

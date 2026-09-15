# JC2 命名规则 (Naming Rules)

适用于**内置名**：全局函数、模块函数（`math.` / `cas.` / `sys.` / `random.`）、
原生原型方法（list / string / dict / set / matrix）。用户自己的类与方法不受约束。

## 一、两条主规则

| 名字形态 | 拼法 | 例子 |
|---|---|---|
| **单字** | 全小写 | `len` `det` `rank` `expand` `factor` `sin` `hash` |
| **多字** | 驼峰 | `countIf` `groupBy` `getElement` `setSlice` `startsWith` `polyGCD` `nextPrime` `compileCode` |

数学词汇本身就是小写，所以矩阵/数学那一层绝大多数名字天然落在"单字全小写"里；
真正需要判断的是多字复合名，它们一律驼峰。

## 二、谓词（`is*`）：全小写，与 C/C++ 一致

`is*` 是一个**独立族**，跨层统一为全小写（`<ctype.h>` / `<math.h>` / C99 的拼法）：

```
全局    isnan isinf isfinite isalpha isdigit isalnum isspace isupper islower
        isprime iseven isodd ispositive isnegative iszero isapprox
        isiterable iscallable isindexable ishashable isempty isfrozen
        isinstance isperp isparallel
原型    a.issubset(b)   a.issuperset(b)   a.isdisjoint(b)
模块    math.isprime(n) math.isperfect(n)
```

理由：这些名字与 C/C++ 标准库同名，用户有肌肉记忆；且谓词不分层——`a.issubset(b)`
与 `math.isprime(n)` 属于同一族，例外只会制造新的不一致。

## 三、C/C++ 同名/同形：从标准库

C99/C++ 的浮点精度后缀是小写 `f`，JC2 跟随：

```
sqrtf  cbrtf  rootf      ← 取 float 精度版本（对应双精度 sqrt / cbrt / root）
isnan  isinf  isfinite   ← <math.h>
isalpha isdigit ...      ← <ctype.h>
```

## 四、第三类：转换与反射（保留驼峰）

这两个家族是**动作**而不是"多字复合名"，按 C++ 自身惯例（`std::to_string`、
`has_extension`）保留驼峰：

```
转换   toArray  toList  toMatrix  toSet  toFrac
反射   getClass  getParent
```

它们虽然是"单字 + 前缀"，但**不写成** `tomatrix` / `getclass`——那样词界消失、可读性变差，
而全小写下划线（`to_matrix`）JC2 又不用。

## 五、加新名字时的判断顺序

1. 是谓词（返回 bool 的判定）？ → `is` + **全小写**（`isempty`，不是 `isEmpty`）
2. 与 C/C++ 标准库同名或同形？ → **从标准库**（`sqrtf`，不是 `sqrtF`）
3. 是转换 / 反射？ → 驼峰（`toMatrix` / `getClass`）
4. 否则：**单字全小写、多字驼峰**

## 六、不受此规则约束的名字

- **用户代码**：类、方法、变量随作者习惯（`examples/` 里的 `isValid`、`modules/collections.jc2`
  的 `isEmpty`、`modules/discrete.jc2` 的 `isTautology` 等都是用户方法，不参与统一）。
- **C++ 内部标识符**：`ObjClass::isTrait`、`Closure::isUFCS`、`BigInt::isPrime()` 等是 C++
  成员/字段，与语言内置名无关，**不要跟着改**（替换时注意区分，别把 `.isPerfect()` 一起改掉）。
- **扩展模块自带的方法**：如 `io.File` 的 `readLine` / `writeStr`，随模块作者；新增时建议按本规则。

## 七、已落地的改名（历史）

| 提交 | 内容 |
|---|---|
| `ab2b67f` | 21 个谓词统一成驼峰（**已撤销**） |
| `3f7c3f3` | 撤销上一条，谓词改回全小写（对齐 C/C++） |
| `2f6c8a3` | 跨层统一：`isFrozen`→`isfrozen`、set 的 `isSubset/isSuperset/isDisjoint`→小写、`math.isPerfect`→`isperfect`，并修正文档与代码不一致的 `math.isPrime` |
| `*` | `sqrtF/cbrtF/rootF` → `sqrtf/cbrtf/rootf`（规则三） |

## 八、已知的存量不一致（未处理）

- `matrix` 原型里 73 个小写名中有 `Acof`（大写 A 开头，来自数学记号 `A` 的余子式），
  属数学惯例，暂留。
- `math` 模块的 `A` / `C` 是数学常数记号（大写单字母），规则一不适用，暂留。

# 方案留档：`datetime` 日期时间库（未实施）

> **状态：未实施，仅留档。** 2026-09-20 决定「都不做」，本文件用于避免下次重复讨论。
> **性质：内部设计记录**，不是用户侧帮助。这里**故意**写决策理由与被否方案，
> 与 `data/documentation.json`（只写契约）的规则不同，勿混。
> 实施前须重新确认：本轮之后需求可能已变。

---

## 1. 需求

用户提问原文：

> 是不是可以提供一个日期/时间转换的库？

指的是**时间戳/时区**这一类能力（拿"现在"、时间戳 ↔ 年月日、时区、年月日加减、
格式化/解析）。与 [`calendar-plan.md`](calendar-plan.md) 的历法转换是**两件不同的事**，
两者不可互相替代。

### 现状（已实测）

**JC2 当前没有任何日期/时间能力。** 全仓（排除 `out/`）搜
`20\d\d-\d{1,2}-\d{1,2}` / `epoch` / `unix time` / `strftime` / `时间戳`，
只命中 `data/documentation.json` 两处，且都是错的：

```
data/documentation.json:583   "clock()  High-resolution timer (seconds since epoch)"
data/documentation.json:6817  "Returns the high-resolution time in seconds since the epoch."
```

`modules/`（含 `http.jc2`）、`examples/`、`tests/` 里**一处日期处理都没有**。

**`sys.clock()` 的文档是错的**（实测）：

| 量 | 值 |
|---|---|
| `sys.clock()` | `607709.0` |
| 真实 epoch | `1790049936.975` |
| 系统开机时长 | `607655` 秒 |

`sys.clock()` 返回的是**开机以来的秒数**，不是文档说的 "seconds since epoch"
（MSVC 下 `high_resolution_clock` 即 `steady_clock`）。拿它当时间戳会得到 1970-01-08 附近的数。
**该 bug 至今未修，本方案也不动它（独立事项）。**

**一个硬约束**：纯 JC2 脚本模块（`modules/*.jc2`）**拿不到墙钟时间** ——
除 `sys.clock()`（开机时长）外没有任何时间原语。所以纯脚本做不出 `now()`。

---

## 2. 已锁定的决策

用户在 2026-09-20 分两轮问答中确认（括号内为当时选项）：

| # | 决策 | 选择 | 备注 |
|---|---|---|---|
| 1 | 实现形态 | **原生 DLL**（原生 DLL / 纯 JC2 / 混合） | 纯脚本拿不到墙钟 |
| 2 | 表示法 | **`DateTime` 类实例 + `TimeDelta` 类**（epoch 数字 / 类+timedelta / Dict） | 与推荐相反，以用户选择为准 |
| 3 | 时区范围 | **只做 UTC + 本机偏移**（不做 IANA 任意时区） | |
| 4 | 功能范围 | **完整面**（不做裁剪） | |
| 5 | 字段读取 | **裸属性 `dt.year`**（裸属性 / 方法 / 两者） | 靠绑 `__getattr__` |
| 6 | 相等性 | **按绝对时刻**（偏移不参与） | |
| 7 | 命名 | **`import datetime` → `datetime.DateTime` / `datetime.TimeDelta`** | 对齐 `decimal.Decimal` |

**内部模型**（已定）：
- `DateTime` = (绝对时刻 epoch 秒, 显示偏移秒)。运算只动绝对时刻，字段按 `时刻 + 偏移` 派生。
- `TimeDelta` = 总秒数。
- 不可变：**不绑 `__setattr__`**，`dt.year = 5` 直接报错。

---

## 3. API 面（已定，已按 `docs/NAMING.md` 校正）

```jc2
import datetime

// 构造
datetime.now()                                          -> DateTime（本机偏移）
datetime.DateTime(y, mo, d, [h], [mi], [s], [offset])   -> DateTime   offset 默认 0
datetime.fromEpoch(sec, [offset])                       -> DateTime
datetime.parse(s, [fmt], [offset])                      -> DateTime
datetime.TimeDelta([days], [hours], [minutes], [seconds]) -> TimeDelta

// 字段（裸属性，走 __getattr__）
dt.year  dt.month  dt.day  dt.hour  dt.minute  dt.second
dt.weekday    // 0=周一 .. 6=周日
dt.yearday    // 1..366
dt.offset     // 显示偏移秒
dt.epoch      // 绝对时刻（UTC 秒）

// 方法（动词）
dt.format([fmt])   // 默认 ISO 8601 带偏移，如 2026-09-20T15:04:05+08:00
dt.toUTC()   dt.toLocal()
dt.addMonths(n)    // 月末 clamp：1/31 +1月 = 2/28；闰年 1/31 +1月 = 2/29
dt.isleapyear()

// 运算符
dt + td  /  dt - td  -> DateTime
dt - dt              -> TimeDelta
dt 间 == < <= > >=    -> Bool（按绝对时刻）
str(dt)              -> ISO 8601
hash(dt)             -> 只由 epoch 决定

// TimeDelta 成员
td.days  td.seconds  td.totalSeconds()

// 模块级纯函数
datetime.isleapyear(y)
datetime.daysInMonth(y, m)
datetime.monthName(m)   datetime.weekdayName(w)   // 固定英文，不随系统语言变
datetime.localOffset([ts])                        // 本机 UTC 偏移秒（含夏令时）
```

### 命名校正（依 `docs/NAMING.md`）

`NAMING.md` §二 规定谓词一律 **`is` + 全小写**（先例：`isFrozen`→`isfrozen`，
`math.isPerfect`→`isperfect`）。我早先稿子里的 `isLeapYear` **不合规**，
定稿改为 **`isleapyear`**（方法名同）。其余名字自检通过：
单字全小写（`now` `parse` `format`）、多字驼峰（`fromEpoch` `addMonths`
`localOffset` `daysInMonth` `monthName` `weekdayName` `totalSeconds`）、
转换族驼峰（`toUTC` `toLocal`）、类名 PascalCase（对齐 `Decimal`）。

---

## 4. 已实测的能力边界（实施依据，勿重新验证）

### 4.1 原生类可以重载运算符

原生模块**实际绑定过**的 dunder 共 **28 个**（`src/lib/*/*_module.cpp` 全量 grep）：
`__add__ __sub__ __mul__ __div__ __radd__ __rsub__ __rmul__ __rdiv__ __eq__ __neq__
__lt__ __le__ __gt__ __ge__ __hash__ __bool__ __neg__ __abs__ __pow__ __getitem__
__setitem__ __len__ __iter__ __next__ __call__ __str__ __getattr__ __setattr__`

> VM 侧另识别 `__repr__`（`src/vm` 的 dunder 名单里有），但**没有任何原生模块绑过它**；
> 它能否绑在原生类上**未验证**。

先例：`decimal_module.cpp`（`Decimal`）与 `tensor_module.cpp`（`Tensor`）都绑了
`__add__`/`__sub__`/`__hash__`/`__str__`。`Decimal` 是照抄模板
（`set_allocator(global_Decimal)` 让 `Decimal(...)` 成为构造函数）。

### 4.2 裸属性可行，但有硬条件

```
import io;  f = io.open("data\\documentation.json")
f.tell      -> <function tell()>       ← 绑了方法，裸访问拿到的是闭包，不是值
f.tell()    -> 0.0
```

```
Point = ffi.Struct({x: "i32", y: "i32"});  p = Point();  p.x = 100
p.x         -> 100                     ← 未绑同名方法，__getattr__ 被调用，返回字段值
p.zzz       -> RuntimeError: Property 'zzz' not found.
```

**结论**：`dt.year` 靠绑 `__getattr__` 实现（`ffi.Struct` 已验证此路可行）。
**字段名与所有方法名不得重名** —— 一旦绑了 `year` 方法，`dt.year` 就只返回闭包。
上表已满足此约束（字段全是名词，方法全是动词/转换）。

### 4.3 整数算术陷阱（epoch ↔ 年月日换算必须避开）

原本是为评估"纯 JC2 脚本实现"是否可行而测的；既然定为原生 DLL，这条的适用面变成
**C++ 实现内部的整数除法/取模**（C++ 的 `/` 和 `%` 同样是向零截断，与 JC2 一致），
以及**用 JC2 写的测试代码**：

```
floor(-1/2) = -1     ✓ 可用
int(-7/3)   = -2     ✗ 向零截断，换算不能用 int()
-7 % 3      = -1     ✗ 截断取模，不是 floored mod
```

换算必须用 **floor 除法**，取模自写 `x - floor(x/n)*n`。
（`%` 的截断语义是语言设计决定，本方案不改。）

### 4.4 名字空闲

`datetime` / `date` / `time` / `timestamp` / `calendar` / `iso` 全部未占用
（`print(type(X))` 报 `Undefined global variable`；`import datetime` 报
`Cannot find library or module 'datetime'`）。

### 4.5 项目不用 `std::format`

`src/` 下 `#include <format>` / `std::format` **零处命中**。模块风格是字符串 `+` 拼接
（见 `io_module.cpp`）。实施时沿用拼接，不引入 `<format>`。

### 4.6 `documentation.json` 结构（帮助走的是另一条路）

顶层键：`topics global_functions keywords matrix_methods list_methods string_methods
dict_methods set_methods sys_methods math_methods cas_methods random_methods`。
**原生模块的帮助不在其中** —— 它由模块自己 `register_help` /
`register_function_help` 注册，构建时由 `collect_<mod>.exe` 产出
`out/build/x64-release/lib/<mod>.json` sidecar（现存 12 个）。

---

## 5. 未验证 / 待定（实施前必须先解决）

| 项 | 状态 | 处理 |
|---|---|---|
| `std::chrono::current_zone()->get_info(now)` 在本工具链（MSVC 19.44 + `/W4 /WX`）是否可用 | **未验证** | 实施前单独写探针编译验证；不可用则退回 Win32 `GetTimeZoneInformation`。**不许直接写进代码** |
| 本机偏移是否含夏令时、边界是否正确 | 未验证 | 同上，需探针 |
| `parse` 的格式串语法细节（哪些记号、错误如何报） | 未定 | 定稿时需补 |
| `dt.format` 默认 ISO 8601 的确切形态 | 未定 | 建议 `2026-09-20T15:04:05+08:00` |

---

## 6. 被否掉的选项（**勿再提**）

| 选项 | 否掉的理由 |
|---|---|
| 纯 JC2 脚本模块（`modules/datetime.jc2`） | 拿不到墙钟时间；无时区数据 |
| 混合（原生只给原语、逻辑在 JC2） | 跨语言边界多一层，调试与性能都更差 |
| 表示法用 epoch 秒（普通数字） | 我推荐，**用户选了类**。已定，勿翻案 |
| 表示法用 Dict 字段 | 同上，用户选了类 |
| 任意 IANA 时区（`"Asia/Shanghai"`） | 需 `std::chrono::tzdb`，可用性未验证；用户选只做 UTC + 本机偏移 |
| 用 `strftime` 做格式化 | `%a/%b/%p` 输出**随系统语言变**，与 `io` 模块刚修掉的 `ec.message()` 同类问题（本地化文本不可断言）。故自写格式化器 |
| 引入 `<format>` | 项目零处使用；为 3 处用途不值得 |
| 谓词写成 `isLeapYear` | 违反 `NAMING.md` §二，见 §3 |

---

## 7. 落地工序（实施时照做）

1. 先写独立 C++ 探针验证 `current_zone()`（决定本机偏移走哪条路）—— **先做，别跳**
2. `src/lib/datetime/datetime_module.cpp`；**epoch ↔ 民用日期**换算自己实现
   （Howard Hinnant 的 `days_from_civil` / `civil_from_days`，纯整数、无查表、
   支持 1970 以前的负数年份；注意 §4.3 的截断陷阱）
3. `CMakeLists.txt` **三处**注册：`add_library(datetime SHARED ...)`、
   `foreach(mod IN ITEMS ...)`、`set(JC2_LIBS ...)`
4. 模块内自注册 help（**只写契约**，依 `LESSONS.md` 闸门 25–27）
5. `tests/modules/test_datetime.jc2`：闰年 2000/1900/2024、月末 clamp、负 epoch（1969）、
   格式与解析往返、`%s` 往返、`dt - dt`、跨偏移相等性、`hash` 一致性、已知参考值
6. 三套测试：`test tests` / `--jit test tests` / `--jit test jit_tests`
7. **打桩反向验证**：临时破坏实现，确认新测试真会失败（不许以"跑绿了"当证据）
8. 提交按 `docs/COMMIT_CONVENTION.md`：`feat(datetime): ...` + 【动机】/【原理】

---

## 8. 明确不做

- 不改 `sys.clock()` 的文档错误（"seconds since epoch" 实为开机时长）—— 独立事项，需用户指令
- 不动 `io` 剩下 4 个无 `error_code` 的报错点（`io_module.cpp` 约 351/458/530/549 行）
- 不动 `%` 的截断语义（语言设计决定）
- 不做 IANA 时区、不做 `TimeDelta` 的月份（长度不固定，月运算只走 `addMonths`）
- 不做 locale 本地化的月名/星期名（固定英文）

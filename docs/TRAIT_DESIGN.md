# JC2 Trait 设计

## 一、定位

Trait 是 **契约 + mixin 两态**：

- **抽象方法**（无实现的签名）：类 `with` trait 必须实现，签名需匹配（契约）。
- **默认方法**（带实现）：类可覆盖、可用默认实现（mixin 复用）。
- 组合契约：**单继承 `extends` + 组合 trait `with`**，trait 之间也用 `with` 组合。
- Trait **不可实例化**、**定义后冻结**（不可修改）。

## 二、语法

```jc2
trait Drawable {
    draw(ctx)                     // 抽象方法：无实现，类必须实现
    describe() = "a drawable"     // 默认方法：有实现，可选覆盖
    local helper() = self.x       // 私有默认方法
}

// trait 组合 trait：也用 with，后覆盖先
trait Renderable with Drawable, Serializable {
    render() = { print("rendering") }
}

// 类：extends 单继承 + with 组合 trait
class Circle extends Shape with Drawable, Other {
    draw(ctx) = { print("circle", ctx) }   // 实现抽象方法
    // describe 用默认实现
}
```

语法与 `class` 对齐：`trait Name {}` 是 `Name = trait {}` 的糖，支持匿名、`local` scope。

- **`extends`**：永远是单继承（类继承类）。
- **`with`**：永远是组合（类组合 trait、trait 组合 trait）。

## 三、成员修饰符矩阵

方法/字段复用 class 修饰符，复制时原样带进类：

- **方法**：`local`（私有）、`static`（静态）、`const`（只读/不可覆盖），可组合。
- **字段**：`const`（只读）、`local`（私有）、`static`（静态），可组合。

底层由 `PropertyDescriptor` 的 `{val, is_const, is_local}` 承载。

## 四、签名校验

抽象方法带完整签名，类实现时**严格相等**校验：

| 维度 | 校验 |
|---|---|
| 方法名 | 一致 |
| 位置参数个数、参数名 | 一致 |
| `rest` 名 | 一致 |
| 仅关键字参数名 | 一致 |
| `kwargs` 名 | 一致 |
| 参数类型（`paramTypeRegs`） | 逐个相等 |
| 返回类型（`returnTypeReg`） | 相等 |

类型比较第一版用**相等**（后续可增强为子类型兼容）。

```jc2
trait I { run(x, y, ...rest, *, mode: int) -> string }

class C with I {
    run(x, y, ...rest, *, mode: int) -> string = { ... }   // ✅
    // 漏参数、改参数名、改类型 → 类定义时抛 TypeError
}
```

## 五、语义

1. **抽象方法必须实现**：类 `with` trait 时，trait 的抽象方法若类自身/父类/其他 trait 默认方法都没实现，或签名不匹配，**类定义时抛 `TypeError`**。抽象方法**不允许默认值参数、不允许 `const`/`static`**（`local` 允许）。
2. **默认方法复制**：复制进类方法表，可选覆盖。
3. **覆盖规则（后覆盖先）**：`with A, C`，后列出的 C 覆盖先列出的 A；自身覆盖一切。
4. **trait 不可实例化**：`Drawable()` 抛 `TypeError`。
5. **trait 方法 self 是实例**：鸭子式信任，不校验字段。
6. **trait 冻结**：定义完成即 `is_frozen = true`，不可加/改/删成员，复制快照与 trait 身份一致，`isinstance` 可靠。
7. **类型检查**：`isinstance(obj, T)` / `e: T` / `x: T` 判「obj 的类（或任意祖先）的 trait 表里含 T」。

## 六、方法查找顺序

复制语义下，查找与普通类一致（类袋子分两张表，见 `OOP_MODEL_DESIGN.md` §2.6：
`classDef->members` 是实例成员模板，`classDef->properties` 是 static 域）：

```
obj.draw() 查找顺序：
1. obj 的类成员模板表（classDef->members）
   └─ 类自身方法（先写，最高）
   └─ trait 复制进来的默认方法（补缺，后覆盖先）
2. 找不到 → 沿 parent 链查父类（父类自己的 members，再父类的父类…）
```

优先级：**类自身 > trait 默认 > 父类**。运行时零额外开销。

- **trait 默认能把父类的同名方法盖掉**：trait 的语义是"复制进当前的类定义"，所以它算这个类
  自己写的方法，而"自己写的"高于"继承来的"。`class D extends A with J {}` 里 A 有 `n`、J 也有 `n`
  时，`D().n()` 走 J 那份；A 自己的 `A().n()` 不受影响。
- 复制阶段跳过的是**类体里自己定义的**同名成员（`ownMembers` 只装类体里写过的名字），
  父类继承来的名字不在其中，所以父类挡不住 trait。

## 七、底层实现

### 1. 数据结构

`PropertyDescriptor` 增字段：

```cpp
struct PropertyDescriptor {
    Value val;
    bool is_const = false;
    bool is_local = false;
    bool is_abstract = false;   // ★ 抽象方法标记
};
```

`ObjClass` 增字段：

```cpp
bool isTrait = false;
bool is_frozen = false;              // trait 冻结（复用/新增）
std::vector<ObjClass*> traits;       // 组合的 trait，含继承、已平铺
```

抽象方法存法：编译成 `CompiledFunction`（签名字段完整、`chunk` 为空），存入 `properties[name] = {Value(fn), false, false, true}`。签名校验直接比对两个 `CompiledFunction` 的 `paramNames`/`restName`/`kwargNames`/`kwargsName`/`paramTypeRegs`/`returnTypeReg`。

### 2. VM 语句执行

**`trait` 语句**：`allocate<ObjClass>()`，`isTrait = true`；若带 `with` 复制父 trait 成员；定义自身成员（抽象方法标记 is_abstract、默认方法正常）；最后 `is_frozen = true`。

**`class ... with X, Y`**：建类、填自身成员；遍历 trait，默认方法与字段复制（自身已存在的不覆盖）；抽象方法校验「已实现 + 签名相等」，否则抛 `TypeError`；平铺 trait 及祖先 trait 进 `traits`；合并父类 `parent->traits`。

### 2.1 字段默认值初始化（`<fieldinit>`）

字段默认值**不能**由 `init` 承担。`<init>` 是按**名字**存进类成员表的，而 trait 组合时成员按名复制，若字段初始化器也叫 `<init>`：

- `with F1, F2`（两者都有同名字段）时后者覆盖前者，字段收敛成同一个值；
- 类自身写了 `init` 时，trait 的字段初始化器被整个覆盖，trait 字段默认值丢失（读字段报「未找到」）。

因此每个类/trait 各自持有一份**保留名**初始化器 `<fieldinit>`（`Bytecode.h` 的 `JC2_FIELD_INIT_NAME`），实例化时按以下顺序全部执行：

1. 沿 `parent` 链**派生优先**（最派生的类先跑，与原先「沿 parent 链取第一个 `<init>`」的覆盖语义一致）；
2. 每级内部先跑该级 `traits`（trait 声明顺序，后者覆盖先者），再跑该级自身的 `<fieldinit>`。

每个初始化器以**声明它的类**同时作为 `self` 的类上下文与词法类，因此私有字段按各自的 classId 落键、互不覆盖，`self.x = v` 也能写回正确的 key。

配套约定：
- 字段默认值只由 `<fieldinit>` 负责，用户 `init` 只写用户逻辑（否则两处都 `DEFINE_PROP` 会重复定义）。`init` 在字段就绪后执行。
- 用户没写 `init` 且该类有字段默认值时，仍补一个**空的** `<init>`，保持子类 `super.init()` 可解析。
- `DEFINE_PROP` 命中已存在字段时不再是「重复定义」错误，而是覆盖写入（同名多次初始化时以最后写入者为准，配合派生优先即正确的覆盖语义）。真·重复声明仍由编译期重定义检查拦截。


### 3. 类型检查整合

`isinstance(obj, T)`：沿 `classDef` 的 `parent` 链，查每个类的 `traits` 表是否含 `T`。trait 表极短，线性比对；无 bitmask、无上限。

## 八、边界与规则汇总

| 场景 | 行为 |
|---|---|
| trait 实例化 | 抛 `TypeError` |
| 抽象方法未实现/签名不符 | 类定义时抛 `TypeError` |
| 抽象方法带默认值参数 | 不允许（纯签名契约） |
| 抽象方法 + `const`/`static` | 不允许（`local` 允许） |
| 默认方法 | 复制进类，可选覆盖 |
| trait 定义后修改 | 冻结，禁止 |
| trait 定义 `init` | 不允许 |
| 同名成员冲突（多 trait） | 后列出的覆盖先列出的 |
| 菱形组合同名抽象方法 | 后覆盖先（继承后列 trait 的签名） |
| 父类已实现抽象方法 | 沿 parent 链算「已实现」 |
| 签名类型相等 | 类型对象指针相等（interned） |
| 循环组合 | 定义时检测，抛 `TypeError` |
| trait `parent` | `nullptr`（`with` 只用 `traits` 表） |
| trait 字段与类字段冲突 | 后覆盖先（类自身覆盖 trait） |
| trait 字段默认值求值 | 每个类/trait 各自持有 `<fieldinit>`，实例化时按「派生优先 → trait 声明顺序」全部执行 |
| trait 的 local 字段初始化 | 以**声明它的 trait** 为类上下文落键，同名私有字段各归其主、每实例独立 |
| 消费者类代码访问 trait 的 local 字段 | 不可见（需由 trait 自己暴露公开方法），与词法私有解析一致 |
| 方法查找顺序 | 类自身 > trait 默认 > 父类 |
| 父类 with 的 trait | 子类继承（traits 合并） |
| 修饰符 local/const/static | 复制时原样带 |

## 九、验证点

1. `trait I { run(x) }` 定义成功，`I()` 抛 `TypeError`。
2. `class C with I` 不实现 `run` → 类定义时抛 `TypeError`。
3. `class C with I { run(x) = {...} }` 实现成功。
4. 签名不符（参数个数/名/类型/返回类型不同）→ 抛 `TypeError`。
5. 抽象方法带默认值、带 `const`/`static` → trait 定义时抛错。
6. 默认方法 `describe() = "..."` 不写也可用，写了覆盖。
7. 多 trait 同名默认方法：后列出的覆盖先列出的。
8. 父类已实现抽象方法，子类不实现也算「已实现」。
9. 循环组合 `A with B`、`B with A` → 定义时抛错。
10. `isinstance(obj, T)` / `e: T` / `x: T` 命中；父类 with 的 trait 子类也命中。
11. trait 表平铺：`class C with B`（B with A），`isinstance(c, A)` 命中。
12. trait 定义后修改成员抛错（冻结）。
13. 修饰符 local/const/static 复制后语义与类原生一致。
14. 字段默认值：`with F1, F2` 两 trait 同名字段各归其主（1 与 2）；类自身 `init` 不吞掉 trait 字段默认值；类继承链逐级初始化；`super.init()` 仍可用；每实例独立持有自己的那份。
15. trait 的 local 私有成员按词法作用域解析：trait 方法只见自己 trait 的私有；类自身的同名私有互不干扰。
16. 组合链上的私有不可跨作用域取用：`trait Deep with LA` 的方法调 LA 的 local → 报错（词法作用域的正确结果）。

## 十、明确不做（第一版）

- trait_mask 位掩码（固定上限）。
- trait「委托」查找（用复制语义，traits 表只用于类型检查）。
- 签名校验的子类型兼容（第一版类型相等）。

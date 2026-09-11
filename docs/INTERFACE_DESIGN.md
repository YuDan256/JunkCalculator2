# JC2 接口（Interface）设计

## 一、定位

接口是 **契约 + mixin 两态**：

- **抽象方法**（无实现的签名）：类 `implements` 必须实现，签名需匹配（契约）。
- **默认方法**（带实现）：类可覆盖、可用默认实现（mixin 复用）。
- 组合契约：**单继承 `extends` + 多接口 `implements`**，接口之间也支持 `extends`（多继承）。
- 接口**不可实例化**、**定义后冻结**（不可修改）。

## 二、语法

```jc2
interface Drawable {
    draw(ctx)                     // 抽象方法：无实现，类必须实现
    describe() = "a drawable"     // 默认方法：有实现，可选覆盖
    local helper() = self.x       // 私有默认方法
}

interface Renderable extends Drawable, Serializable {
    render() = { print("rendering") }
}

class Circle extends Shape implements Drawable {
    draw(ctx) = { print("circle", ctx) }   // 实现抽象方法
    // describe 用默认实现
}
```

语法与 `class` 对齐：`interface Name {}` 是 `Name = interface {}` 的糖，支持匿名、`local` scope。差异仅 `extends` 多继承。

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
interface I { run(x, y, ...rest, *, mode: int) -> string }

class C implements I {
    run(x, y, ...rest, *, mode: int) -> string = { ... }   // ✅
    // 漏参数、改参数名、改类型 → 类定义时抛 TypeError
}
```

## 五、语义

1. **抽象方法必须实现**：类 implements 接口时，接口的抽象方法若类自身/父类/其他接口默认方法都没实现，或签名不匹配，**类定义时抛 `TypeError`**。抽象方法**不允许默认值参数、不允许 `const`/`static`**（`local` 允许）。
2. **默认方法复制**：复制进类方法表，可选覆盖。
3. **覆盖规则（后覆盖先）**：`extends A, C` / `implements A, C`，后列出的 C 覆盖先列出的 A；自身覆盖一切。
4. **接口不可实例化**：`Drawable()` 抛 `TypeError`。
5. **接口方法 self 是实例**：鸭子式信任，不校验字段。
6. **接口冻结**：定义完成即 `is_frozen = true`，不可加/改/删成员，复制快照与接口身份一致，`isinstance` 可靠。
7. **类型检查**：`isinstance(obj, I)` / `e: I` / `x: I` 判「obj 的类（或任意祖先）的接口表里含 I」。

## 六、方法查找顺序

复制语义下，查找与普通类一致：

```
obj.draw() 查找顺序：
1. obj 的类方法表（classDef->properties）
   └─ 类自身方法（先写，最高）
   └─ 接口复制进来的默认方法（补缺，后覆盖先）
2. 找不到 → 沿 parent 链查父类
```

优先级：**类自身 > 接口默认 > 父类**。运行时零额外开销。

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
bool isInterface = false;
bool is_frozen = false;              // 接口冻结（复用/新增）
std::vector<ObjClass*> interfaces;   // 实现的接口，含继承、已平铺
```

抽象方法存法：编译成 `CompiledFunction`（签名字段完整、`chunk` 为空），存入 `properties[name] = {Value(fn), false, false, true}`。签名校验直接比对两个 `CompiledFunction` 的 `paramNames`/`restName`/`kwargNames`/`kwargsName`/`paramTypeRegs`/`returnTypeReg`。

### 2. VM 语句执行

**`interface` 语句**：`allocate<ObjClass>()`，`isInterface = true`；若带 `extends` 复制父接口成员；定义自身成员（抽象方法标记 is_abstract、默认方法正常）；最后 `is_frozen = true`。

**`class ... implements X, Y`**：建类、填自身成员；遍历接口，默认方法与字段复制（自身已存在的不覆盖）；抽象方法校验「已实现 + 签名相等」，否则抛 `TypeError`；平铺接口及祖先接口进 `interfaces`；合并父类 `parent->interfaces`。

### 3. 类型检查整合

`isinstance(obj, I)`：沿 `classDef` 的 `parent` 链，查每个类的 `interfaces` 表是否含 `I`。接口表极短，线性比对；无 bitmask、无上限。

## 八、边界与规则汇总

| 场景 | 行为 |
|---|---|
| 接口实例化 | 抛 `TypeError` |
| 抽象方法未实现/签名不符 | 类定义时抛 `TypeError` |
| 抽象方法带默认值参数 | 不允许（纯签名契约） |
| 抽象方法 + `const`/`static` | 不允许（`local` 允许） |
| 默认方法 | 复制进类，可选覆盖 |
| 接口定义后修改 | 冻结，禁止 |
| 接口定义 `init` | 不允许 |
| 同名成员冲突（多接口） | 后列出的覆盖先列出的 |
| 菱形继承同名抽象方法 | 后覆盖先（继承后列接口的签名） |
| 父类已实现抽象方法 | 沿 parent 链算「已实现」 |
| 签名类型相等 | 类型对象指针相等（interned） |
| 循环继承 | 定义时检测，抛 `TypeError` |
| 接口 `parent` | `nullptr`（`extends` 只用 `interfaces`） |
| 接口字段与类字段冲突 | 后覆盖先（类自身覆盖接口） |
| 接口字段默认值求值 | 接口定义时求值一次（与普通类一致） |
| 方法查找顺序 | 类自身 > 接口默认 > 父类 |
| 父类 implements 的接口 | 子类继承（interfaces 合并） |
| 修饰符 local/const/static | 复制时原样带 |

## 九、验证点

1. `interface I { run(x) }` 定义成功，`I()` 抛 `TypeError`。
2. `class C implements I` 不实现 `run` → 类定义时抛 `TypeError`。
3. `class C implements I { run(x) = {...} }` 实现成功。
4. 签名不符（参数个数/名/类型/返回类型不同）→ 抛 `TypeError`。
5. 抽象方法带默认值、带 `const`/`static` → 接口定义时抛错。
6. 默认方法 `describe() = "..."` 不写也可用，写了覆盖。
7. 多接口同名默认方法：后列出的覆盖先列出的。
8. 父类已实现抽象方法，子类不实现也算「已实现」。
9. 循环继承 `A extends B`、`B extends A` → 定义时抛错。
10. `isinstance(obj, I)` / `e: I` / `x: I` 命中；父类 implements 子类也命中。
11. 接口表平铺：`class C implements B`（B extends A），`isinstance(c, A)` 命中。
12. 接口定义后修改成员抛错（冻结）；接口字段默认值只在定义时求值一次。
13. 修饰符 local/const/static 复制后语义与类原生一致。

## 十、明确不做（第一版）

- trait_mask 位掩码（固定上限）。
- 接口「委托」查找（用复制语义，接口表只用于类型检查）。
- 签名校验的子类型兼容（第一版类型相等）。

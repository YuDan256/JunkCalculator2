# JC2 spawn / yield 语法定稿

本文只定义 **spawn 执行体 + yield**。`thread` 暂不进入语法。

## 1. 关键字

新增：

```text
spawn
yield
```

不新增：

```text
await
async
thread
```

## 2. 执行模型

- 普通调用 `f()` 在当前执行体里同步执行。
- `spawn` 是唯一创建新执行体的语法。
- `yield` 挂起的是**当前执行体**，不是当前函数。
- 顶层脚本是一个隐式主执行体。
- 顶层 `yield` 让出主执行体，由调度器以 `none` 恢复。

```jc2
inner() = {
    yield 1
}

outer() = {
    inner()
    return 2
}

job = spawn outer()

job.resume()   // 1
job.join()     // 2
```

## 3. spawn 语法

```text
spawn 调用
spawn 调用标签 调用
spawn 块
spawn 调用标签 块
```

示例：

```jc2
job = spawn fetch(url)
job = spawn 'fetcher' fetch(url)

job = spawn {
    ...
}
job = spawn 'worker' {
    ...
}
```

### 标签

只接受普通字符串或 r 字符串：

```jc2
spawn 'fetcher' fetch(url)
spawn "fetcher" fetch(url)
spawn r'worker\io' fetch(url)
```

不接受：

```jc2
spawn f'{name}' fetch(url)
spawn nameVar fetch(url)
spawn '' fetch(url)
```

标签规则：

```text
必须是字符串字面量
必须非空
不创建变量
不进入作用域
不参与遮蔽
只作为句柄的调试元数据
```

## 4. yield 语法

```text
yield
yield 表达式
```

```jc2
yield
yield 1
local x = yield 1
```

```text
yield expr
    产出 expr
    恢复时，yield 表达式得到 resume(value) 的 value
    没有传值时得到 none

```

`yield` 可以出现在：

```text
普通函数
块
defer
catch
顶层
```

## 5. task 句柄

`spawn` 返回 `task`。

```text
PENDING
RUNNING
SUSPENDED
DONE
FAILED
CANCELLED
```

### 方法

```jc2
job.resume(value)
job.join()
job.cancel()
job.isAlive()
```

### resume

```text
PENDING/SUSPENDED -> 推进
value 成为上次 yield 表达式的结果
返回本次 yield 值
如果本次推进中结束，返回最终结果
已结束 -> 报错
```

### join

```text
等待执行体完成
返回最终结果
未捕获异常在此重抛
取消时抛错误
```

### cancel

```text
PENDING   -> CANCELLED
SUSPENDED -> 下一次恢复注入取消
RUNNING   -> 下一个让出点生效
DONE/FAILED/CANCELLED -> 无操作
```

### isAlive

```text
PENDING / RUNNING / SUSPENDED -> true
DONE / FAILED / CANCELLED     -> false
```

### 显示与迭代

```text
<task (PENDING)>
<task 'calc' (SUSPENDED)>
<task 'calc' (DONE)>
```

标签只用于显示。`task` 可直接用于 `for-in`：

```jc2
for (step in spawn hanoi(3)) {
    print(step)
}
```

每轮调用 `resume(none)`，产出 `yield` 值；`return` 值不进入循环；
`FAILED` 重抛，`CANCELLED` 安静结束。

## 6. 失败

```text
未捕获异常 -> FAILED
done == true
failed == true
error 保存原异常
join 重抛
再次 join 重抛同一异常
```

## 7. defer

```text
正常 return    -> 执行 defer
未捕获异常     -> 执行 defer
cancel         -> 按 LIFO 执行 defer
yield          -> 不执行 defer
```

正常作用域退出时，defer 内可以 yield；任务挂起，恢复后继续执行该 defer。

异常展开时，defer 内也可以 yield；defer 完成后继续恢复 catch handler。
未捕获异常时，先执行 yielding defer，再让 task 进入 FAILED。

取消展开期间，defer 内再次 yield 不再挂起：

```text
抛 CancelledError
继续按 LIFO 执行剩余 defer
最终 task 进入 CANCELLED
```

defer 内 yield 不影响已经保存的 return 值。

## 8. 示例

```jc2
producer() = {
    local x = yield 1
    return x + 1
}

job = spawn 'producer' producer()

job.resume(none)   // 1
job.resume(10)     // 11
job.join()         // 11
```

```jc2
rangeLike(n) = {
    local i = 0
    while (i < n) {
        yield i
        i += 1
    }
}

job = spawn rangeLike(3)
job.resume()   // 0
job.resume()   // 1
job.resume()   // 2
job.join()     // none
```

## 9. 实现状态

已实现：

```text
spawn 调用/块
可选字符串/r 字符串标签
task resume/join/cancel/isAlive
yield 执行体帧栈
嵌套调用整体挂起
顶层隐式 main task
顶层 yield
无 await
```

仍需收口：

```text
join/resume 目前是同步等待，不是调度器让出点
GC 压力下的多帧任务测试
文档/帮助同步
```

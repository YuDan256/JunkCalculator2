# JC2 LSP (Language Server Protocol) 设计与实现方案

为 JC2 语言实现完整的 LSP 支持，是一项系统性的工程。为了最大程度复用现有的编译器前端（Lexer、Parser、AST），建议将 LSP Server 作为 JC2 主程序的一个子命令（例如 `jc2 lsp`）来运行。

以下是为 JC2 构建 LSP 功能的详细架构设计与实现方案。

## 1. 总体架构设计

LSP Server 的核心是一个**事件驱动的常驻进程**。它的整体架构可以分为四个主要层级：

1. **通讯层 (Transport Layer)**：负责通过标准输入/输出（stdin/stdout）与编辑器（如 VSCode）进行 JSON-RPC 2.0 消息的收发。
2. **协议层 (Protocol Layer)**：负责解析 JSON 数据，将其映射为 LSP 标准的数据结构（如 `InitializeParams`, `TextDocumentPositionParams` 等），并进行路由分发。
3. **文档与状态管理层 (Workspace & VFS)**：在内存中维护一个虚拟文件系统（Virtual File System），实时跟踪用户正在编辑的代码内容，并处理行列坐标与绝对偏移量的转换。
4. **语言特性层 (Language Features)**：调用 JC2 的 Lexer、Parser 和 Resolver，对代码进行词法、语法和语义分析，最终生成补全、悬停、跳转等响应数据。

---

## 2. 详细实现步骤

### 步骤一：构建 JSON-RPC 通讯框架
LSP 规定使用基于 HTTP 报文头的 JSON-RPC 协议。
* **消息读取**：Server 需要在一个死循环中不断读取 `stdin`。首先读取 HTTP 头部（如 `Content-Length: ...\r\n\r\n`），解析出内容长度，然后读取对应长度的 JSON 字符串。
* **消息解析**：引入一个轻量级的 C++ JSON 库（如果 JC2 已有内置的 JSON 模块，可直接复用）。将 JSON 解析为请求（Request，需要回复）、通知（Notification，不需要回复）或响应（Response）。
* **消息发送**：将结果序列化为 JSON，拼接上 `Content-Length` 头部，写入 `stdout` 并立即 `flush`。

### 步骤二：实现虚拟文件系统 (VFS) 与坐标映射
编辑器中的代码可能尚未保存到磁盘，因此 Server 必须在内存中维护代码状态。
* **生命周期管理**：
  * 监听 `textDocument/didOpen`：将文档内容加载到内存的哈希表中（Key 为文件 URI）。
  * 监听 `textDocument/didChange`：更新内存中的文档内容（初期可实现全量更新 Full Sync，后期优化为增量更新 Incremental Sync）。
  * 监听 `textDocument/didClose`：从内存中移除文档。
* **坐标转换引擎**：
  * LSP 传递的位置是 `(line, character)`（基于 0 的索引，character 通常是 UTF-16 编码单元）。
  * JC2 的 AST 节点使用的是绝对字节偏移量 `position`。
  * **方案**：每次文档更新时，扫描一遍文本，记录每一行开头的绝对字节偏移量（构建一个 `std::vector<int> lineOffsets`）。
  * 提供两个核心转换接口：`PositionToOffset(line, char)` 和 `OffsetToPosition(offset)`。

### 步骤三：改造 Parser 实现容错解析 (Error Recovery)
这是最关键的一步。当前的 JC2 Parser 遇到错误会直接抛出异常并终止，这在 LSP 环境下是不可接受的。
* **错误收集**：在 Parser 中新增一个 `std::vector<Diagnostic> diagnostics` 列表。当遇到语法错误时，不再 `throw`，而是将错误信息、错误发生的 `startPos` 和 `endPos` 记录到列表中。
* **恐慌模式恢复 (Panic Mode)**：记录错误后，进入恢复模式。不断调用 `advance()` 丢弃 Token，直到遇到“同步标记”（Synchronization Tokens）。
  * 同步标记通常是语句的结束符（如 `SEMICOLON`, `NEWLINE`, `RBRACE`）或控制流关键字（如 `IF`, `WHILE`, `FOR`, `CLASS`, `RETURN`）。
* **生成部分 AST**：恢复同步后，继续解析下一条语句。确保即使代码有错，也能返回一个包含正确部分的 `Block` 节点（Partial AST）。

### 步骤四：构建持久化符号表与语义分析 (Semantic Analysis)
为了支持跳转和补全，需要对 AST 进行遍历，提取语义信息。
* **作用域树 (Scope Tree)**：构建一个树状结构，每个节点代表一个作用域（全局、文件、类、函数、块）。
* **符号定义 (Symbol)**：遍历 AST（如 `Assign`, `ClassDefExpr`, `LambdaExpr`），将变量名、函数名、类名注册到当前作用域中。
  * 记录符号的类型（TypeHint 或推导类型）。
  * 记录符号的定义位置（AST 节点的 `startPos` 和 `endPos`）。
  * **提取文档注释**：利用 Lexer 的 `keepComments` 特性，如果一个声明节点上方紧挨着 `COMMENT` Token，将其提取为该符号的 Docstring。
* **引用消解 (Reference Resolution)**：遍历所有 `Variable` 和 `DotAccess` 节点，将其链接到作用域树中对应的符号定义上。

### 步骤五：实现核心 LSP 请求处理

#### 1. 诊断提示 (Diagnostics - 语法检查)
* **触发时机**：每次 `didChange` 或 `didOpen` 后。
* **流程**：调用 Lexer 和容错 Parser。将收集到的词法错误和语法错误，通过坐标转换引擎转为 `(line, character)` 范围。
* **响应**：主动向客户端发送 `textDocument/publishDiagnostics` 通知，编辑器会显示红波浪线。

#### 2. 悬停提示 (Hover)
* **触发时机**：客户端发送 `textDocument/hover` 请求。
* **流程**：
  1. 将请求的 `(line, character)` 转为绝对偏移量 `offset`。
  2. 在 AST 中进行深度优先搜索，找到包含该 `offset` 的最深层节点（通常是 `Variable` 或 `Call`）。
  3. 在符号表中查找该节点对应的定义。
  4. 提取其类型信息和 Docstring。
* **响应**：返回包含 Markdown 格式文本的 Hover 对象。

#### 3. 跳转到定义 (Go to Definition)
* **触发时机**：客户端发送 `textDocument/definition` 请求。
* **流程**：与 Hover 类似，找到光标下的 AST 节点，查符号表找到其原始定义节点。
* **响应**：返回定义节点所在的文件 URI 和位置范围（Range）。

#### 4. 自动补全 (Completion)
* **触发时机**：客户端发送 `textDocument/completion` 请求。
* **流程**：
  1. 确定光标位置的上下文。
  2. **如果是普通输入**：从当前作用域开始，沿着作用域树向上查找，收集所有可见的变量、函数、类名，以及 JC2 的全局关键字（`if`, `while`, `class` 等）和内置函数（`sin`, `cos`, `print` 等）。
  3. **如果是属性访问 (`DotAccess`)**：例如输入 `obj.`。尝试推导 `obj` 的类型。如果 `obj` 是已知类或命名空间，则只返回该类/命名空间的成员属性和方法。
* **响应**：返回一个 `CompletionItem` 列表，包含补全文本、类型图标（如 Method, Variable, Class）和简短说明。

#### 5. 代码格式化 (Formatting)
* **触发时机**：客户端发送 `textDocument/formatting` 请求。
* **流程**：JC2 已经内置了 `jc2 fmt` 功能。直接将 VFS 中的当前代码文本传递给现有的 AST 格式化引擎。
* **响应**：返回一个 `TextEdit` 列表，通常可以直接返回一个替换整个文档的全局 Edit，或者计算差异返回局部 Edit。

---

## 3. 部署与集成方案

1. **命令行入口**：在 JC2 的 `main.cpp` 中新增一个参数解析。当执行 `jc2 lsp` 时，不执行脚本，而是启动上述的 JSON-RPC 消息循环，进入 Server 模式。
2. **VSCode 插件开发**：
   * 创建一个简单的 VSCode 插件项目（使用 TypeScript 和 `vscode-languageclient` 库）。
   * 在插件的 `extension.ts` 中配置 Language Client，将其执行路径指向用户的 `jc2.exe lsp`。
   * 配置 `package.json`，声明 JC2 语言的语法高亮（TextMate Grammar）和语言配置（括号匹配、注释符号 `//` 等）。

---

## 4. 文件结构设计 (File Structure)

LSP 相关的源代码将统一存放在 `src\utils\lsp\` 目录下，以实现高内聚低耦合。具体文件划分如下：

* **`src\utils\lsp\LspServer.h / .cpp`**
  * **核心控制层**：LSP 服务器的主入口。负责管理基于 `stdin/stdout` 的 JSON-RPC 消息循环，维护服务器生命周期（Initialize, Shutdown, Exit），并分发各类请求。
* **`src\utils\lsp\LspProtocol.h / .cpp`**
  * **协议与序列化层**：定义 LSP 标准数据结构（如 `Position`, `Range`, `CompletionItem`, `Hover` 等），并提供 JSON 与 C++ 结构体之间的相互转换（序列化/反序列化）。
* **`src\utils\lsp\Workspace.h / .cpp`**
  * **文档与状态管理层 (VFS)**：实现虚拟文件系统。处理 `didOpen`, `didChange`, `didClose` 通知，在内存中维护最新的代码文本，并提供 `(line, character)` 与绝对字节偏移量 `offset` 之间的双向转换引擎。
* **`src\utils\lsp\SemanticAnalyzer.h / .cpp`**
  * **语义分析层**：负责遍历容错解析后生成的 AST，构建持久化的作用域树（Scope Tree）和符号表（Symbol Table），提取变量类型、定义位置及文档注释（Docstrings）。
* **`src\utils\lsp\LspHandlers.h / .cpp`**
  * **语言特性处理层**：实现具体的 LSP 请求处理逻辑。包括悬停提示（Hover）、自动补全（Completion）、跳转定义（Go to Definition）以及语法诊断（Diagnostics）的生成。

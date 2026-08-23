const vscode = require('vscode');
const path = require('path');
const fs = require('fs');
const os = require('os');

let outputChannel = null;

function getExePath() {
    const config = vscode.workspace.getConfiguration('jc2');
    let exePath = config.get('executablePath', '');
    if (exePath && fs.existsSync(exePath)) return exePath;

    const isWin = process.platform === 'win32';
    const name = isWin ? 'JunkCalculator2.exe' : 'JunkCalculator2';
    const pathDirs = (process.env.PATH || '').split(isWin ? ';' : ':');
    for (const dir of pathDirs) {
        const full = path.join(dir, name);
        if (fs.existsSync(full)) return full;
    }
    return null;
}

function runFile(filePath, cwd, extraFlags = "") {
    const exePath = getExePath();
    if (!exePath) {
        vscode.window.showErrorMessage(
            'JC2 executable not found. Use "JC2: Set Executable Path" to configure.',
            'Set Path'
        ).then(choice => {
            if (choice === 'Set Path') vscode.commands.executeCommand('jc2.setExecutable');
        });
        return;
    }

    let terminal = vscode.window.terminals.find(t => t.name === 'JC2');
    if (terminal) { terminal.dispose(); }

    terminal = vscode.window.createTerminal({ name: 'JC2', cwd: cwd });
    terminal.show();

    const isWin = process.platform === 'win32';
    const defaultShell = vscode.env.shell || '';
    const isPowerShell = defaultShell.toLowerCase().includes('powershell') || 
                         defaultShell.toLowerCase().includes('pwsh');

    if (isPowerShell) {
        terminal.sendText(`& "${exePath}" ${extraFlags} --run "${filePath}"`);
    } else if (isWin) {
        terminal.sendText(`chcp 65001 >nul`);
        terminal.sendText(`"${exePath}" ${extraFlags} --run "${filePath}"`);
    } else {
        terminal.sendText(`"${exePath}" ${extraFlags} --run "${filePath}"`);
    }
}

function activate(context) {
    // ★ 新增：自动补全 (从 documentation.json 加载函数列表)
    let functions = {};
    let docKeywords = {};
    try {
        const possiblePaths = [
            path.join(__dirname, '../data/documentation.json'), // 源码仓库相对路径
            path.join(__dirname, 'documentation.json')          // 插件打包后的内部路径
        ];
        
        if (vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders.length > 0) {
            possiblePaths.push(path.join(vscode.workspace.workspaceFolders[0].uri.fsPath, 'data/documentation.json'));
        }

        let loaded = false;
        for (const docPath of possiblePaths) {
            if (fs.existsSync(docPath)) {
                const docData = JSON.parse(fs.readFileSync(docPath, 'utf-8'));
                if (docData && docData.functions) {
                    functions = docData.functions;
                    if (docData.keywords) docKeywords = docData.keywords;
                    loaded = true;
                    break;
                }
            }
        }
        
        if (!loaded) {
            vscode.window.showWarningMessage("JC2 Extension: Could not find documentation.json. Function autocompletion will be disabled.");
        }
    } catch (e) {
        console.error("Failed to load documentation.json for autocompletion", e);
    }

    const provider = vscode.languages.registerCompletionItemProvider('jc2', {
        provideCompletionItems(document, position) {
            const completionItems = [];

            // 添加关键字补全
            const keywords = [
                'if', 'else', 'while', 'for', 'in', 'is', 'break', 'continue', 'return',
                'switch', 'case', 'default', 'throw', 'try', 'catch', 'match', 'defer',
                'class', 'extends', 'const', 'static', 'state', 'delete', 'ref', 'import', 'local', 'namespace', 'macro', 'syntax', 'quote', 'enum',
                'true', 'false', 'none', 'PI', 'E', 'ANS', 'self', 'super'
            ];
            for (const kw of keywords) {
                completionItems.push(new vscode.CompletionItem(kw, vscode.CompletionItemKind.Keyword));
            }

            // 添加常用代码片段 (Snippets)
            const snippets = [
                { label: 'for', detail: 'for loop', insertText: 'for (${1:i} = ${2:0}; ${1:i} < ${3:10}; ${1:i} += 1) {\n\t$0\n}' },
                { label: 'forin', detail: 'for..in loop', insertText: 'for (${1:item} in ${2:collection}) {\n\t$0\n}' },
                { label: 'while', detail: 'while loop', insertText: 'while (${1:condition}) {\n\t$0\n}' },
                { label: 'if', detail: 'if statement', insertText: 'if (${1:condition}) {\n\t$0\n}' },
                { label: 'ifelse', detail: 'if..else statement', insertText: 'if (${1:condition}) {\n\t$2\n} else {\n\t$0\n}' },
                { label: 'class', detail: 'class definition', insertText: 'class ${1:ClassName} {\n\tinit() {\n\t\t$0\n\t}\n}' },
                { label: 'namespace', detail: 'namespace definition', insertText: 'namespace ${1:Name} {\n\t$0\n}' },
                { label: 'enum', detail: 'enum definition', insertText: 'enum ${1:Name} {\n\t$0\n}' },
                { label: 'func', detail: 'function definition', insertText: '${1:functionName}(${2:args}) = {\n\t$0\n}' },
                { label: 'match', detail: 'match expression', insertText: 'match (${1:expr}) {\n\t${2:pattern} => ${3:body},\n\t_ => ${0:fallback}\n}' },
                { label: 'macro', detail: 'macro definition', insertText: 'macro ${1:macroName}(${2:args}) = {\n\treturn quote {\n\t\t$0\n\t}\n}' },
                { label: 'syntax', detail: 'syntax macro definition', insertText: 'syntax ${1:macroName}(${2:tokens}) = {\n\t$0\n\treturn parseExpr(${2:tokens})\n}' },
                { label: 'quote', detail: 'quote block', insertText: 'quote {\n\t$0\n}' }
            ];
            for (const snip of snippets) {
                const item = new vscode.CompletionItem(snip.label, vscode.CompletionItemKind.Snippet);
                item.detail = snip.detail;
                item.insertText = new vscode.SnippetString(snip.insertText);
                completionItems.push(item);
            }

            for (const [funcName, funcData] of Object.entries(functions)) {
                // 忽略 dunder methods (如 __add__)
                if (funcName.startsWith('__') && funcName.endsWith('__')) continue;

                const item = new vscode.CompletionItem(funcName, vscode.CompletionItemKind.Function);
                if (funcData.signature) item.detail = funcData.signature;
                if (funcData.desc) {
                    item.documentation = new vscode.MarkdownString(funcData.desc);
                    if (funcData.examples && funcData.examples.length > 0) {
                        item.documentation.appendCodeblock(funcData.examples.join('\n'), 'jc2');
                    }
                }
                completionItems.push(item);

                // 处理别名
                if (funcData.aliases) {
                    for (const alias of funcData.aliases) {
                        const aliasItem = new vscode.CompletionItem(alias, vscode.CompletionItemKind.Function);
                        aliasItem.detail = (funcData.signature || alias) + ` (alias for ${funcName})`;
                        if (funcData.desc) {
                            aliasItem.documentation = new vscode.MarkdownString(funcData.desc);
                            if (funcData.examples && funcData.examples.length > 0) {
                                aliasItem.documentation.appendCodeblock(funcData.examples.join('\n'), 'jc2');
                            }
                        }
                        completionItems.push(aliasItem);
                    }
                }
            }
            return completionItems;
        }
    });
    context.subscriptions.push(provider);

    // ★ 新增：参数悬浮提示 (Signature Help)
    const signatureProvider = vscode.languages.registerSignatureHelpProvider('jc2', {
        provideSignatureHelp(document, position, token, context) {
            const linePrefix = document.lineAt(position).text.substring(0, position.character);
            
            // 向前查找未闭合的左括号 '('
            let openParenIndex = -1;
            let parenCount = 0;
            for (let i = position.character - 1; i >= 0; i--) {
                const char = linePrefix[i];
                if (char === ')') parenCount++;
                else if (char === '(') {
                    if (parenCount === 0) {
                        openParenIndex = i;
                        break;
                    }
                    parenCount--;
                }
            }
            if (openParenIndex === -1) return null;

            // 提取函数名
            const beforeParen = linePrefix.substring(0, openParenIndex).trimEnd();
            const match = beforeParen.match(/([a-zA-Z_][a-zA-Z0-9_]*)$/);
            if (!match) return null;

            const funcName = match[1];
            const funcData = functions[funcName];
            if (!funcData || !funcData.signature) return null;

            const signatureHelp = new vscode.SignatureHelp();
            const sigInfo = new vscode.SignatureInformation(funcData.signature);
            
            // 解析签名中的参数列表以实现高亮
            const paramMatch = funcData.signature.match(/\((.*)\)/);
            if (paramMatch && paramMatch[1]) {
                // 简单按逗号分割签名字符串
                const params = paramMatch[1].split(',').map(p => p.trim());
                sigInfo.parameters = params.map(p => new vscode.ParameterInformation(p));
            }

            // 计算当前处于第几个参数（忽略嵌套括号内的逗号）
            const argsString = linePrefix.substring(openParenIndex + 1);
            let activeParam = 0;
            let nested = 0;
            for (let i = 0; i < argsString.length; i++) {
                if (argsString[i] === '(') nested++;
                else if (argsString[i] === ')') nested--;
                else if (argsString[i] === ',' && nested === 0) activeParam++;
            }

            signatureHelp.signatures = [sigInfo];
            signatureHelp.activeSignature = 0;
            signatureHelp.activeParameter = activeParam;

            return signatureHelp;
        }
    }, '(', ',');
    context.subscriptions.push(signatureProvider);

    // ★ 新增：悬停提示 (Hover Provider)
    const hoverProvider = vscode.languages.registerHoverProvider('jc2', {
        provideHover(document, position, token) {
            const range = document.getWordRangeAtPosition(position, /[a-zA-Z_][a-zA-Z0-9_]*/);
            if (!range) return null;
            const word = document.getText(range);
            
            const funcData = functions[word];
            if (funcData) {
                const md = new vscode.MarkdownString();
                md.appendCodeblock(funcData.signature || word, 'jc2');
                if (funcData.desc) {
                    md.appendMarkdown(`\n\n${funcData.desc}`);
                }
                if (funcData.examples && funcData.examples.length > 0) {
                    md.appendMarkdown(`\n\n**Examples:**\n`);
                    md.appendCodeblock(funcData.examples.join('\n'), 'jc2');
                }
                return new vscode.Hover(md, range);
            }
            
            const kwData = docKeywords[word];
            if (kwData) {
                const md = new vscode.MarkdownString();
                md.appendCodeblock(kwData.signature || word, 'jc2');
                if (kwData.desc) {
                    const descText = Array.isArray(kwData.desc) ? kwData.desc.join('\n') : kwData.desc;
                    md.appendMarkdown(`\n\n${descText}`);
                }
                if (kwData.examples && kwData.examples.length > 0) {
                    md.appendMarkdown(`\n\n**Examples:**\n`);
                    md.appendCodeblock(kwData.examples.join('\n'), 'jc2');
                }
                return new vscode.Hover(md, range);
            }
            return null;
        }
    });
    context.subscriptions.push(hoverProvider);

    // ★ 新增：大纲视图/文档符号 (Document Symbol Provider)
    const symbolProvider = vscode.languages.registerDocumentSymbolProvider('jc2', {
        provideDocumentSymbols(document, token) {
            const symbols = [];
            const text = document.getText();
            const lines = text.split(/\r?\n/);
            
            for (let i = 0; i < lines.length; i++) {
                const line = lines[i];
                
                // 匹配类定义: class MyClass, namespace MyNamespace, 或 enum MyEnum
                const classMatch = line.match(/^\s*(?:class|namespace|enum)\s+([a-zA-Z_][a-zA-Z0-9_]*)/);
                if (classMatch) {
                    const isNamespace = line.includes('namespace');
                    const isEnum = line.includes('enum');
                    const range = new vscode.Range(i, 0, i, line.length);
                    const selectionRange = new vscode.Range(i, line.indexOf(classMatch[1]), i, line.indexOf(classMatch[1]) + classMatch[1].length);
                    
                    let kind = vscode.SymbolKind.Class;
                    let detail = 'class';
                    if (isNamespace) { kind = vscode.SymbolKind.Namespace; detail = 'namespace'; }
                    else if (isEnum) { kind = vscode.SymbolKind.Enum; detail = 'enum'; }

                    symbols.push(new vscode.DocumentSymbol(
                        classMatch[1],
                        detail,
                        kind,
                        range,
                        selectionRange
                    ));
                    continue;
                }
                
                // 匹配宏定义: macro/syntax macroName(args) =
                const macroMatch = line.match(/^\s*(?:macro|syntax)\s+([a-zA-Z_][a-zA-Z0-9_]*)\s*\([^)]*\)\s*(?:\{|=)/);
                if (macroMatch) {
                    const range = new vscode.Range(i, 0, i, line.length);
                    const selectionRange = new vscode.Range(i, line.indexOf(macroMatch[1]), i, line.indexOf(macroMatch[1]) + macroMatch[1].length);
                    symbols.push(new vscode.DocumentSymbol(
                        macroMatch[1],
                        'macro',
                        vscode.SymbolKind.Function,
                        range,
                        selectionRange
                    ));
                    continue;
                }

                // 匹配函数定义: funcName(args) = 或 [const] [local/ref/state] funcName(args) -> type =
                const funcMatch = line.match(/^\s*(?:const\s+)?(?:(?:local|ref|state)\s+)?([a-zA-Z_][a-zA-Z0-9_]*)\s*\([^)]*\)(?:\s*->\s*[a-zA-Z_][a-zA-Z0-9_]*)?\s*(?:\{|=)/);
                if (funcMatch) {
                    const range = new vscode.Range(i, 0, i, line.length);
                    const selectionRange = new vscode.Range(i, line.indexOf(funcMatch[1]), i, line.indexOf(funcMatch[1]) + funcMatch[1].length);
                    symbols.push(new vscode.DocumentSymbol(
                        funcMatch[1],
                        'function',
                        vscode.SymbolKind.Function,
                        range,
                        selectionRange
                    ));
                }
            }
            return symbols;
        }
    });
    context.subscriptions.push(symbolProvider);

    context.subscriptions.push(
        vscode.commands.registerCommand('jc2.run', () => {
            const editor = vscode.window.activeTextEditor;
            if (!editor || !editor.document.fileName.endsWith('.jc2')) return;
            editor.document.save().then(() => {
                runFile(editor.document.fileName, path.dirname(editor.document.fileName));
            });
        })
    );

    // ★ 新增：带探针执行
    context.subscriptions.push(
        vscode.commands.registerCommand('jc2.runProfile', () => {
            const editor = vscode.window.activeTextEditor;
            if (!editor || !editor.document.fileName.endsWith('.jc2')) return;
            editor.document.save().then(() => {
                runFile(editor.document.fileName, path.dirname(editor.document.fileName), "--profile");
            });
        })
    );

    // ★ 新增：JIT 编译执行
    context.subscriptions.push(
        vscode.commands.registerCommand('jc2.runJit', () => {
            const editor = vscode.window.activeTextEditor;
            if (!editor || !editor.document.fileName.endsWith('.jc2')) return;
            editor.document.save().then(() => {
                runFile(editor.document.fileName, path.dirname(editor.document.fileName), "--jit");
            });
        })
    );

    context.subscriptions.push(
        vscode.commands.registerCommand('jc2.runSelection', () => {
            const editor = vscode.window.activeTextEditor;
            if (!editor) return;
            const selection = editor.document.getText(editor.selection);
            if (!selection.trim()) return;
            const tmpFile = path.join(os.tmpdir(), '_jc2_selection.jc2');
            fs.writeFileSync(tmpFile, selection, 'utf-8');
            runFile(tmpFile, path.dirname(editor.document.fileName));
        })
    );

    context.subscriptions.push(
        vscode.commands.registerCommand('jc2.setExecutable', async () => {
            const result = await vscode.window.showOpenDialog({
                canSelectFiles: true, canSelectFolders: false,
                filters: { 'Executable': process.platform === 'win32' ? ['exe'] : ['*'] },
                title: 'Select JunkCalculator2 executable'
            });
            if (result && result.length > 0) {
                const config = vscode.workspace.getConfiguration('jc2');
                await config.update('executablePath', result[0].fsPath, vscode.ConfigurationTarget.Global);
                vscode.window.showInformationMessage(`JC2 executable updated.`);
            }
        })
    );
}

function deactivate() { if (outputChannel) outputChannel.dispose(); }
module.exports = { activate, deactivate };

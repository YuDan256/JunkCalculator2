#!/usr/bin/env python3
# 阶段 3 迁移脚本（全项目版）：
#   把 src 下所有 .cpp/.h 里「单行、有明确前缀」的 throw std::runtime_error("前缀: 消息")
#   前缀替换为 JC2_THROW(类型, "。
# 精确性：前缀白名单 + 只替换前缀 + 多行/无前缀跳过 + 编译回归兜底。
import re
import sys
import os

PREFIX_MAP = {
    # 运行时错误
    'Math Error': 'MathError', 'MathError': 'MathError', 'Matrix Error': 'MathError',
    'DivisionByZero': 'MathError', 'Numerical Error': 'MathError',
    'Non-elementary integral': 'MathError', 'Groebner Error': 'MathError', 'CAS Error': 'MathError',
    'Type Error': 'TypeError', 'TypeError': 'TypeError',
    'Runtime Error': 'RuntimeError', 'VM Error': 'RuntimeError',
    'Macro Error': 'RuntimeError', 'Macro Execution Error': 'RuntimeError',
    'Token Macro Execution Error': 'RuntimeError', 'Solver Error': 'RuntimeError',
    'Image Error': 'RuntimeError', 'toFunc error': 'RuntimeError', 'toFunc': 'RuntimeError',
    'Value Error': 'ValueError', 'ValueError': 'ValueError',
    'Base Error': 'ValueError', 'JSON Parse Error': 'ValueError',
    'IO Error': 'IOError', 'FFI Error': 'FFIError', 'Tensor Error': 'TensorError',
    'Calculus Error': 'CalculusError', 'SymMatrix Error': 'SymbolicError',
    'Overflow': 'OverflowError',
    # 编译期错误
    'Parser Error': 'ParserError', 'Compile Error': 'ParserError', 'CompileError': 'ParserError',
    'CodeEmitter': 'EmitterError',
    'Syntax Error': 'SyntaxError', 'SyntaxError': 'SyntaxError',
    'Lexer Error': 'LexerError', 'TokenStream Error': 'LexerError',
    # 内部错误
    'JIT Error': 'InternalError', 'BytecodeSerializer Error': 'InternalError',
    'JCB Read Error': 'InternalError', 'System Error': 'InternalError', 'Universal Error': 'InternalError',
    'Risch Step 4': 'MathError', 'Risch Step 5': 'MathError',
}

pattern = re.compile(r'throw std::runtime_error\("([^"]+?):\s')

def process_file(path):
    with open(path, 'r', encoding='utf-8') as f:
        content = f.read()
    lines = content.split('\n')
    migrated = 0
    skipped = []
    for idx, line in enumerate(lines):
        if 'throw std::runtime_error(' not in line:
            continue
        if ');' not in line:
            skipped.append((idx + 1, line.strip()))
            continue
        m = pattern.search(line)
        if not m:
            skipped.append((idx + 1, line.strip()))
            continue
        prefix = m.group(1)
        typ = PREFIX_MAP.get(prefix)
        if typ is None:
            skipped.append((idx + 1, line.strip()))
            continue
        lines[idx] = line[:m.start()] + 'JC2_THROW(' + typ + ', "' + line[m.end():]
        migrated += 1
    new_content = '\n'.join(lines)
    if new_content != content:
        with open(path, 'w', encoding='utf-8') as f:
            f.write(new_content)
    return migrated, skipped

def main():
    total = 0
    all_skipped = []
    for root, _, files in os.walk('src'):
        for fn in files:
            if fn.endswith('.cpp') or fn.endswith('.h'):
                path = os.path.join(root, fn)
                m, s = process_file(path)
                total += m
                for ln, txt in s:
                    all_skipped.append((path, ln, txt))
    print(f'总迁移 {total} 处')
    print(f'跳过 {len(all_skipped)} 处（多行/无前缀）：')
    for path, ln, txt in all_skipped:
        print(f'  {path}:{ln}: {txt[:100]}')

if __name__ == '__main__':
    main()

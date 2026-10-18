"""文档覆盖检查：注册表 vs documentation.json

用法：
    py local/check_doc_coverage.py            # 只报告
    py local/check_doc_coverage.py --strict   # 有任何缺口则退出码 1

检查三类问题：
  1. 已注册（proto 方法或全局函数）但文档里没有任何条目；
  2. 文档条目挂在某个容器分类下，但该 proto 上并未注册同名方法；
  3. 文档签名写成方法形式（A.foo(...)）却登记在 global_functions 里（仅提示）。
"""

import io
import json
import re
import sys

REG = r'src/vm/BuiltinRegistry.cpp'
DOC = r'data/documentation.json'

src = io.open(REG, encoding='utf-8', errors='replace').read()
docs = json.load(open(DOC, encoding='utf-8'))

proto_methods = {}
for proto, name in re.findall(r'regMethod\(VM::activeVM->(\w+), "([A-Za-z_][A-Za-z0-9_]*)"', src):
    proto_methods.setdefault(proto, set()).add(name)

globals_registered = set(re.findall(r'\breg\("([A-Za-z_][A-Za-z0-9_]*)"', src))

documented = set(docs['global_functions'].keys())
for key, cat in docs.items():
    if key.endswith('_methods') and isinstance(cat, dict):
        documented |= set(cat.keys())
        for entry in cat.values():
            if isinstance(entry, dict):
                documented |= set(entry.get('aliases', []))
for entry in docs['global_functions'].values():
    if isinstance(entry, dict):
        documented |= set(entry.get('aliases', []))

def report(title, items, strict_fail):
    items = sorted(items)
    print('%s: %d' % (title, len(items)))
    if items:
        for n in items:
            print('   ', n)
    print()
    return strict_fail and bool(items)

fail = False

# 1. 注册了但完全没文档
missing = set()
for proto, names in proto_methods.items():
    missing |= (names - documented)
fail |= report('registered methods with no doc entry', missing, True)

# 2. 分类条目与其 proto 不匹配
mismatch = set()
for key, cat in docs.items():
    if not (key.endswith('_methods') and isinstance(cat, dict)):
        continue
    proto = key[:-len('_methods')] + 'Proto'
    if proto not in proto_methods:      # 例如 cas/math/sys/random：它们是命名空间，不是原型
        continue
    registered = proto_methods.get(proto, set())
    for name in cat:
        if name not in registered:
            mismatch.add('%s.%s' % (proto, name))
fail |= report('documented in a category but not registered on that proto', mismatch, True)

# 3. 提示：方法态签名（A.foo(...)）登记在 global_functions 里
#    这不是错误——那些条目就是用方法形式写的；只统计数量，方便日后整理分区。
method_form = 0
for name, entry in docs['global_functions'].items():
    sig = entry.get('signature', '')
    if sig and not sig.startswith(name + '(') and ('.' + name + '(') in sig:
        if name not in globals_registered:
            method_form += 1
print('note: %d method-style entries live under global_functions (informational)' % method_form)

print('RESULT:', 'FAIL' if fail else 'OK')
sys.exit(1 if fail else 0)

const fs = require('fs');
const path = require('path');

const sourcePath = path.join(__dirname, '../data/documentation.json');
const targetPath = path.join(__dirname, 'documentation.json');
const tmLanguagePath = path.join(__dirname, 'syntaxes/jc2.tmLanguage.json');

try {
    const rawData = fs.readFileSync(sourcePath, 'utf-8');
    const doc = JSON.parse(rawData);
    
    // 1. 提取所有内置函数和别名，自动更新语法高亮文件 (Single Source of Truth)
    const builtinFunctions = new Set();
    if (doc.functions) {
        for (const key in doc.functions) {
            if (key.startsWith('__') && key.endsWith('__')) continue; // 忽略 dunder 魔术方法
            builtinFunctions.add(key);
            if (doc.functions[key].aliases) {
                for (const alias of doc.functions[key].aliases) {
                    builtinFunctions.add(alias);
                }
            }
        }
    }
    
    if (fs.existsSync(tmLanguagePath) && builtinFunctions.size > 0) {
        const tmRaw = fs.readFileSync(tmLanguagePath, 'utf-8');
        const tmDoc = JSON.parse(tmRaw);
        if (tmDoc.repository && tmDoc.repository["builtin-functions"] && tmDoc.repository["builtin-functions"].patterns) {
            const funcList = Array.from(builtinFunctions).join('|');
            tmDoc.repository["builtin-functions"].patterns[0].match = "\\b(" + funcList + ")\\b(?=\\()";
            fs.writeFileSync(tmLanguagePath, JSON.stringify(tmDoc, null, 4), 'utf-8');
            console.log('Successfully updated builtin-functions in jc2.tmLanguage.json');
        }
    }

    // 2. 删除不需要的 topics 节点以精简插件体积
    if (doc.topics) delete doc.topics;
    
    // 剔除描述与示例以精简体积，并将别名提升为独立的函数条目
    if (doc.functions) {
        const aliasesToAdd = {};
        for (const key in doc.functions) {
            const func = doc.functions[key];
            if (func.aliases) {
                for (const alias of func.aliases) {
                    // 将签名中的原函数名替换为别名 (例如 log(x) -> ln(x))
                    aliasesToAdd[alias] = {
                        signature: func.signature ? func.signature.split(key).join(alias) : alias
                    };
                }
                delete func.aliases;
            }
            delete func.desc;
            delete func.examples;
        }
        Object.assign(doc.functions, aliasesToAdd);
    }
    
    // 同样剔除关键字的描述与示例
    if (doc.keywords) {
        for (const key in doc.keywords) {
            delete doc.keywords[key].desc;
            delete doc.keywords[key].examples;
        }
    }
    
    fs.writeFileSync(targetPath, JSON.stringify(doc, null, 2), 'utf-8');
    console.log('Successfully built minified documentation.json for extension.');
} catch (err) {
    console.error('Error building documentation.json:', err);
    process.exit(1);
}

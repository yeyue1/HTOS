# -*- coding: utf-8 -*-
"""注释规范自检：确认一次改动只改了注释，代码逐字节未变。

用法（在仓库根目录执行）：

    python scripts/check_comments.py include/httask.h kernel/httask.c

对每个文件：
  1. 剥离 /* */ 与 // 注释（正确跳过字符串与字符常量），与 HEAD 版本比较代码文本；
  2. 检查文件是 UTF-8 且没有 BOM；
  3. 检查没有残留 // 行注释；
  4. 检查文件头有 @file 与 @brief；
  5. 检查注释里有中文。
全部通过时退出码为 0，否则为 1。
"""
import io
import re
import subprocess
import sys


def strip_comments(text):
    """去掉注释，保留字符串/字符字面量原样。"""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            i += 2
            while i + 1 < n and not (text[i] == '*' and text[i + 1] == '/'):
                i += 1
            i += 2
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            i += 2
            while i < n and text[i] != '\n':
                i += 1
            continue
        if c == '"' or c == "'":
            quote = c
            out.append(c)
            i += 1
            while i < n:
                out.append(text[i])
                if text[i] == '\\' and i + 1 < n:
                    out.append(text[i + 1])
                    i += 2
                    continue
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def normalize(code):
    """把空白折叠成单个空格，避免注释行删除后残留空行的干扰。"""
    return re.sub(r'\s+', ' ', code).strip()


def git_head(path):
    result = subprocess.run(['git', 'show', 'HEAD:' + path], capture_output=True)
    if result.returncode != 0:
        return None
    return result.stdout.decode('utf-8')


def check(path):
    raw = io.open(path, 'rb').read()
    problems = []
    if raw.startswith(b'\xef\xbb\xbf'):
        problems.append('文件带 BOM')
    try:
        text = raw.decode('utf-8')
    except UnicodeDecodeError as exc:
        return ['不是 UTF-8: %s' % exc], 0
    old = git_head(path)
    if old is None:
        problems.append('不在 git HEAD 中')
    elif normalize(strip_comments(old)) != normalize(strip_comments(text)):
        problems.append('代码被改动（应当只改注释）')
    if '//' in strip_comments(text):
        problems.append('残留 // 行注释')
    if '@file' not in text:
        problems.append('缺少 @file 文件头')
    if '@brief' not in text:
        problems.append('缺少 @brief')
    cjk = sum(1 for ch in text if '\u4e00' <= ch <= '\u9fff')
    if cjk == 0:
        problems.append('注释里没有中文')
    return problems, cjk


def main(paths):
    if not paths:
        print(__doc__)
        return 1
    failed = 0
    for path in paths:
        problems, cjk = check(path)
        if problems:
            print('%-34s FAIL  %s' % (path, '; '.join(problems)))
            failed += 1
        else:
            print('%-34s PASS  cjk=%d' % (path, cjk))
    print('----')
    print('FILES=%d FAILED=%d' % (len(paths), failed))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

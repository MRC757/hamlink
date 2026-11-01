#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Syntax-only check of the Kotlin and C# sources with tree-sitter. No compiler is involved, so this catches
unbalanced braces and malformed statements, NOT type errors or missing symbols."""
import glob, os, sys
import tree_sitter_kotlin as tk, tree_sitter_c_sharp as tc
from tree_sitter import Language, Parser

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
JOBS = [(Language(tk.language()), "android/app/src/main/java/**/*.kt"), (Language(tc.language()), "windows/src/**/*.cs")]

def errors(node, out):
    if node.type == "ERROR" or node.is_missing: out.append(node)
    for c in node.children: errors(c, out)

bad = 0
for lang, pat in JOBS:
    p = Parser(lang)
    for f in sorted(glob.glob(os.path.join(ROOT, pat), recursive=True)):
        src = open(f, "rb").read()
        errs = []
        errors(p.parse(src).root_node, errs)
        rel = os.path.relpath(f, ROOT)
        if errs:
            bad += 1
            for e in errs[:3]:
                print(f"{rel}:{e.start_point[0]+1}: syntax error near {src[e.start_byte:e.start_byte+60]!r}")
        else:
            print("ok ", rel)
sys.exit(1 if bad else 0)

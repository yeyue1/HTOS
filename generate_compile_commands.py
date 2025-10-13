#!/usr/bin/env python3
"""
生成compile_commands.json用于clang-tidy分析
"""
import json
import os
from pathlib import Path

def generate_compile_commands():
    """生成编译命令数据库"""
    root_dir = Path(__file__).parent
    include_dir = root_dir / "include"
    
    commands = []
    
    # 查找所有C源文件
    c_files = list((root_dir / "kernel").rglob("*.c"))
    
    for c_file in c_files:
        command = {
            "directory": str(root_dir),
            "command": f"gcc -std=c11 -I{include_dir} -DSTM32F103xB -c {c_file}",
            "file": str(c_file)
        }
        commands.append(command)
    
    # 写入compile_commands.json
    output_file = root_dir / "compile_commands.json"
    with open(output_file, 'w') as f:
        json.dump(commands, f, indent=2)
    
    print(f"Generated {output_file} with {len(commands)} entries")

if __name__ == "__main__":
    generate_compile_commands()

#!/usr/bin/env python3
"""Memoize immutable ISA opcode sets to reduce host allocations, preserving all checks."""
from pathlib import Path
import re


def patch(runtime):
    path = runtime / 'src/device/linker/kernel/instruction/instruction.go'
    source = path.read_text()
    if 'reproOpcodeSetCache' in source:
        return
    source = source.replace('"math"', '"math"\n\t"sync"', 1)
    pattern = r'func \(this \*Instruction\) (\w+OpCodes)\(\) map\[OpCode\]bool \{'
    def replacement(match):
        name = match.group(1)
        return f'''var reproOpcodeSetCache{name} struct {{
    once sync.Once
    codes map[OpCode]bool
}}
func (this *Instruction) {name}() map[OpCode]bool {{
    reproOpcodeSetCache{name}.once.Do(func() {{
        reproOpcodeSetCache{name}.codes = this.reproBuild{name}()
    }})
    return reproOpcodeSetCache{name}.codes
}}
func (this *Instruction) reproBuild{name}() map[OpCode]bool {{'''
    source, count = re.subn(pattern, replacement, source)
    if count < 30:
        raise RuntimeError(f'Unexpected instruction interface: {count} opcode set methods')
    path.write_text(source)
    print(f'Memoized {count} immutable opcode-set builders.')


if __name__ == '__main__':
    patch(Path(__file__).resolve().parent / 'runtime')

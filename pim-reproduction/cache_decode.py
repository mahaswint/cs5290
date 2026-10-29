#!/usr/bin/env python3
"""Enable an optional host-side decode cache; simulated instructions are unchanged."""
from pathlib import Path


def patch(runtime):
    path = runtime / 'src/device/simulator/dpu/sram/iram.go'
    source = path.read_text()
    if 'decodeCacheEnabled' in source:
        return
    source = source.replace('"errors"', '"errors"\n\t"os"')
    source = source.replace('type Iram struct {', 'type Iram struct {\n\tdecodeCacheEnabled bool\n\tdecoded map[int64]*instruction.Instruction\n')
    source = source.replace('func (this *Iram) Init() {', '''func (this *Iram) Init() {
    this.decodeCacheEnabled = os.Getenv("REPRO_IRAM_CACHE") == "1"
    this.decoded = make(map[int64]*instruction.Instruction)
''', 1)
    source = source.replace('func (this *Iram) Read(address int64) *instruction.Instruction {', '''func (this *Iram) Read(address int64) *instruction.Instruction {
    if this.decodeCacheEnabled {
        if cached, found := this.decoded[address]; found {
            // Instructions/descriptors are immutable; retain a distinct issue identity.
            issued := *cached
            return &issued
        }
    }
''', 1)
    source = source.replace('instruction_.Decode(byte_stream)', '''instruction_.Decode(byte_stream)
    if this.decodeCacheEnabled { this.decoded[address] = instruction_ }''', 1)
    source = source.replace('func (this *Iram) Write(address int64, size int64, byte_stream *encoding.ByteStream) {', '''func (this *Iram) Write(address int64, size int64, byte_stream *encoding.ByteStream) {
    // Any code upload invalidates decoded host objects, preserving self-modifying loads.
    this.decoded = make(map[int64]*instruction.Instruction)
''', 1)
    path.write_text(source)


if __name__ == '__main__':
    patch(Path(__file__).resolve().parent / 'runtime')

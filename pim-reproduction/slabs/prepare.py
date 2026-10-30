#!/usr/bin/env python3
"""Prepare isolated size-dependent slab runtime; preserve other experiments."""
from pathlib import Path
import hashlib
import json
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
BASE = HERE.parent
ROOT = BASE.parent
AUTHOR = ROOT / 'PIM-malloc/simulator/uPIMulator/golang_vm/uPIMulator'
RUNTIME = HERE / 'runtime'


def replace_function(code, name, replacement):
    import re
    match = re.search(r'(?m)^[^\n;{}]+\b' + re.escape(name) + r'\([^;{}]*\)\s*\{', code)
    if not match:
        raise RuntimeError(f'Function not found: {name}')
    start, pos, depth = match.start(), match.end(), 1
    while depth:
        depth += (code[pos] == '{') - (code[pos] == '}')
        pos += 1
    return code[:start] + replacement.strip() + code[pos:]


def backend_source():
    original = (AUTHOR / 'benchmark/microbenchmark/dpu/task.c').read_text()
    declarations = original[:original.index('struct ThreadCache;')]
    body = original[original.index('// NOTE(bongjoon.hyun@gmail.com): metadata meanings'):
                    original.index('struct ThreadCache *initThreadCache(')]
    code = declarations + '\nMUTEX_INIT(buddyAllocatorMutex);\n' + body
    code = replace_function(code, 'safeBuddyAllocate', '''
__mram_ptr void *safeBuddyAllocate(struct BuddyAllocator *b, int size) {
  return buddyAllocateForward(b, 0, 0, size);
}''')
    code = replace_function(code, 'safeBuddyFree', '''
void safeBuddyFree(struct BuddyAllocator *b, __mram_ptr void *pointer) {
  /* Directly allocated large nodes leave their descendants empty. The lowest
     fully occupied node on this path identifies the actual allocation. */
  for (int level = b->numBuddyLevels - 1; level >= 0; --level) {
    int index = getLeafIndex(b, level, pointer);
    if (getMetadata(b, level, index) == 0) {
      assert(pointer == b->mramMallocableRegionBase + index * getLeafSize(b, level));
      buddyFreeBackward(b, level, pointer);
      return;
    }
  }
  assert(0 && "invalid or double backend free");
}''')
    # Round storage, not level offsets, to the SDK's eight-byte DMA granularity.
    start = code.index('int getMetadataSize(struct BuddyAllocator *buddyAllocator) {')
    end = code.index('int getMetadataSizePerLevel(', start)
    section = code[start:end].replace('return metadataSize;', 'return (metadataSize + 7) & ~7;')
    code = code[:start] + section + code[end:]
    # The original tail-bound expression adds byte counts to an int pointer.
    start_marker = '    // NOTE(bongjoon.hyun@gmail.com): MRAM address is assumed to be aligned with'
    for name in ('getMetadata', 'setMetadata'):
        import re
        match = re.search(r'(?m)^(?:int|void) ' + name + r'\([^;{}]*\)\s*\{', code)
        start = code.index(start_marker, match.start())
        end = code.index('    mram_read(mramAddress, buddyAllocator->wramBuffer, dmaTransferSize);', start)
        replacement = '''    __mram_ptr void *mramAddress = (__mram_ptr void *)
        ((uint32_t)getMramAddress(buddyAllocator, level, leafIndex) & ~7U);
    int available = (uint32_t)buddyAllocator->mramMetadataBase +
                    buddyAllocator->metadataSize - (uint32_t)mramAddress;
    int dmaTransferSize = available < buddyAllocator->bufferSize ?
                         available : buddyAllocator->bufferSize;
    assert(dmaTransferSize >= 8 && dmaTransferSize % 8 == 0);

'''
        code = code[:start] + replacement + code[end:]
    code = code.replace('for (int i = buddyAllocator->numBuddyLevels - 1; i > 0; i--)',
                        'for (int i = buddyAllocator->numBuddyLevels - 1; i >= 0; i--)')
    code = code.replace('0b11 <<', '0b11U <<').replace('0b1 <<', '0b1U <<')
    return ('/* Derived from the authors\' buddy backend. Shared correctness repairs\n'
            ' * are documented in README.md and source_manifest.json. */\n' + code), original


def prepare_backend():
    code, original = backend_source()
    path = HERE / 'buddy_backend.inc'
    if path.exists() and path.read_text() != code:
        manifest_path = HERE / 'source_manifest.json'
        previous = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}
        if hashlib.sha256(path.read_bytes()).hexdigest() != previous.get('backend_sha256'):
            raise RuntimeError('buddy_backend.inc has local edits; review before regenerating')
    path.write_text(code)
    manifest = {
        'author_commit': subprocess.check_output(['git', '-C', ROOT / 'PIM-malloc', 'rev-parse', 'HEAD'], text=True).strip(),
        'author_source_sha256': hashlib.sha256(original.encode()).hexdigest(),
        'backend_sha256': hashlib.sha256(code.encode()).hexdigest(),
        'shared_repairs': ['nonfatal allocation failure', 'large-block free traversal',
                           'DMA-aligned metadata storage and byte-address tail bounds',
                           'allow root-sized allocations'],
        'frontend': 'Common maintained bitmap/page-list frontend in allocator.c; all policies share it.',
        'scope': 'Same-tasklet frees; 4 KiB pages for 16..2048 B, optional larger slab for 2049..4096 B. No adaptive allocation.',
    }
    (HERE / 'source_manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


def install_reporting(code):
    if 'policyReportPC' in code:
        return code
    code = code.replace('type Logic struct {', '''type Logic struct {
    policyReportPC map[int64]bool
    policySymbols map[string]int64
''', 1)
    code = code.replace('// Passive probes;', '''this.policyReportPC = make(map[int64]bool)
    this.policySymbols = make(map[string]int64)
    // Passive probes;''', 1)
    needle = 'address, err := strconv.ParseInt(strings.TrimSpace(fields[1]), 10, 64)'
    code = code.replace(needle, '''if strings.HasPrefix(fields[0], "policy_report_ready_") {
            value, err := strconv.ParseInt(strings.TrimSpace(fields[1]), 10, 64)
            if err != nil { panic(err) }
            this.policyReportPC[value] = true
        }
        for _, name := range []string{"policy_stats", "policy_backend", "snapshots", "policy_report_meta"} {
            if fields[0] == name {
                value, err := strconv.ParseInt(strings.TrimSpace(fields[1]), 10, 64)
                if err != nil { panic(err) }
                this.policySymbols[name] = value
            }
        }
        ''' + needle, 1)
    needle = 'tid := thread.ThreadId()'
    code = code.replace(needle, needle + '''
            if this.policyReportPC[pc] {
                read := func(name string, index int64) int64 {
                    address, ok := this.policySymbols[name]
                    if !ok { panic("missing report symbol: "+name) }
                    return this.operand_collector.Lw(address+4*index)
                }
                count, tasklets := read("policy_report_meta", 3), read("policy_report_meta", 4)
                if count > 514 || tasklets > 16 { panic("invalid report shape") }
                for i := int64(0); i < count; i++ {
                    fmt.Print("POLICY_SNAPSHOT")
                    for j := int64(0); j < 6; j++ { fmt.Printf(",%d", read("snapshots", 6*i+j)) }
                    fmt.Print("\\n")
                }
                for t := int64(0); t < tasklets; t++ {
                    fmt.Printf("POLICY_STATS,%d", t)
                    for j := int64(0); j < 12; j++ { fmt.Printf(",%d", read("policy_stats", 12*t+j)) }
                    fmt.Print("\\n")
                }
                fmt.Print("POLICY_BACKEND")
                for j := int64(0); j < 6; j++ { fmt.Printf(",%d", read("policy_backend", j)) }
                fmt.Print("\\nPOLICY_WRAM")
                for j := int64(0); j < 3; j++ { fmt.Printf(",%d", read("policy_report_meta", j)) }
                fmt.Print("\\nPOLICY_CHECK,PASS\\n")
            }
''', 1)
    return code


def prepare_runtime():
    if RUNTIME.exists():
        return
    shutil.copytree(BASE / 'runtime', RUNTIME, symlinks=True,
                    ignore=shutil.ignore_patterns('build', 'bin', '__pycache__', '*.pprof'))
    config = RUNTIME / 'src/misc/config_loader.go'
    code = config.read_text()
    code = code.replace('func (this *ConfigLoader) WramSize() int64 {\n\treturn 256 * 1024',
                        'func (this *ConfigLoader) WramSize() int64 {\n\treturn 64 * 1024 - this.WramOffset()')
    code = code.replace('func (this *ConfigLoader) StackSize() int64 {\n\treturn 4 * 1024',
                        'func (this *ConfigLoader) StackSize() int64 {\n\treturn 1536')
    config.write_text(code)
    path = RUNTIME / 'src/device/simulator/dpu/logic/logic.go'
    code = path.read_text()
    fields_start = code.index('\n\treproStartPC, reproEndPC')
    fields_end = code.index('\n', code.index('reproCounts ', fields_start))
    code = code[:fields_start] + '''
    policyBegin, policyEnd map[int64][]string
    policyStart map[string]int64
    policyCount map[string]int
''' + code[fields_end:]
    start = code.index('// Passive PC probes:')
    end = code.index('\n', code.index('panic("allocation probes missing")', start))
    # Include the enclosing closing brace after the original panic line.
    end = code.index('\n\t}', end) + len('\n\t}')
    code = code[:start] + '''// Passive probes; do not issue instructions or alter scheduling.
    this.policyBegin, this.policyEnd = make(map[int64][]string), make(map[int64][]string)
    this.policyStart, this.policyCount = make(map[string]int64), make(map[string]int)
    addressBytes, err := os.ReadFile(filepath.Join(command_line_parser.StringParameter("bin_dirpath"), "addresses.txt"))
    if err != nil { panic(err) }
    for _, line := range strings.Split(string(addressBytes), "\\n") {
        fields := strings.SplitN(line, ":", 2)
        if len(fields) != 2 { continue }
        address, err := strconv.ParseInt(strings.TrimSpace(fields[1]), 10, 64)
        if err != nil { panic(err) }
        for _, kind := range []string{"alloc", "free", "init", "work", "drain", "total"} {
            if strings.HasPrefix(fields[0], "policy_begin_"+kind+"_") { this.policyBegin[address] = append(this.policyBegin[address], kind) }
            if strings.HasPrefix(fields[0], "policy_end_"+kind+"_") { this.policyEnd[address] = append(this.policyEnd[address], kind) }
        }
    }
    if len(this.policyBegin) == 0 || len(this.policyEnd) == 0 { panic("policy probes missing") }
''' + code[end:]
    start = code.index('\n\t\t\ttid := thread.ThreadId()')
    end = code.index('\n\t\t\t}', code.index('delete(this.reproStarts, tid)', start)) + len('\n\t\t\t}')
    code = code[:start] + '''
            tid := thread.ThreadId()
            for _, kind := range this.policyBegin[pc] {
                key := strconv.Itoa(tid)+":"+kind
                if _, active := this.policyStart[key]; active { panic("nested policy probe: "+key) }
                this.policyStart[key] = this.stat_factory.Value("logic_cycle")
            }
            for _, kind := range this.policyEnd[pc] {
                key := strconv.Itoa(tid)+":"+kind
                start, active := this.policyStart[key]
                if !active { panic("missing policy start: "+key) }
                end := this.stat_factory.Value("logic_cycle")
                fmt.Printf("POLICY_EVENT,%s,%d,%d,%d,%d,%d\\n", kind, tid, this.policyCount[key], start, end, end-start)
                this.policyCount[key]++
                delete(this.policyStart, key)
            }
''' + code[end:]
    path.write_text(install_reporting(code))
    subprocess.run(['gofmt', '-w', config, path], check=True)
    (RUNTIME / 'src/.policy-runtime-version').write_text('1\n')


if __name__ == '__main__':
    prepare_backend()
    prepare_runtime()
    print(f'Contribution runtime ready: {RUNTIME}')

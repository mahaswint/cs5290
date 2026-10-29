#!/usr/bin/env python3
"""Prepare an isolated, instrumented copy of the authors' software simulator."""
from pathlib import Path
import json
import shutil
import subprocess
from cache_decode import patch as patch_decode_cache
from cache_opcodes import patch as patch_opcode_cache

BASE = Path(__file__).resolve().parent
WORKSPACE = BASE.parent
SOURCE = WORKSPACE / 'PIM-malloc/simulator/uPIMulator/golang_vm/uPIMulator'
RUNTIME = BASE / 'runtime'


def main():
    if RUNTIME.exists():
        raise SystemExit('runtime already exists; reuse it rather than overwriting experiments')
    shutil.copytree(SOURCE, RUNTIME, ignore=shutil.ignore_patterns('build', 'bin', '__pycache__'))
    # Reuse the authors' staged main program; no hardware-cache implementation is copied.
    staged = WORKSPACE / 'PIM-malloc/simulator/uPIMulatorLevelCache/golang_vm/uPIMulator/src/main.go'
    main_text = staged.read_text()
    # Compiler execution is performed by run.py using the SDK directly, outside Docker.
    main_text = main_text.replace('\n\t"uPIMulator/src/device/compiler"', '')
    main_text = main_text.replace('compiler_ := new(compiler.Compiler)\n\t\t\tcompiler_.Init(command_line_parser)\n\t\t\tcompiler_.Compile()', '')
    # Keep every architectural default from the baseline main.go.
    baseline_main = (SOURCE / 'src/main.go').read_text()
    main_text = main_text[:main_text.index('func InitCommandLineParser()')] + baseline_main[baseline_main.index('func InitCommandLineParser()'):]
    main_text = main_text.replace('command_line_parser.Init()\n', 'command_line_parser.Init()\n\tcommand_line_parser.AddOption(misc.STRING, "mode", "simulate", "link or simulate")\n', 1)
    main_text = main_text.replace('"os"', '"os"\n\t"runtime/pprof"')
    main_text = main_text.replace('func main() {', '''func main() {
    if profilePath := os.Getenv("REPRO_CPU_PROFILE"); profilePath != "" {
        profile, err := os.Create(profilePath)
        if err != nil { panic(err) }
        if err := pprof.StartCPUProfile(profile); err != nil { panic(err) }
        defer profile.Close()
        defer pprof.StopCPUProfile()
    }
''', 1)
    (RUNTIME / 'src/main.go').write_text(main_text)

    logic_path = RUNTIME / 'src/device/simulator/dpu/logic/logic.go'
    code = logic_path.read_text()
    code = code.replace('"math"', '"math"\n\t"os"\n\t"path/filepath"\n\t"strconv"\n\t"strings"')
    code = code.replace('type Logic struct {', 'type Logic struct {\n\treproStartPC, reproEndPC map[int64]bool\n\treproStarts map[int]int64\n\treproCounts map[int]int\n')
    needle = 'this.stat_factory.Init(name)'
    addition = '''
    // Passive PC probes: these do not issue instructions or change simulated timing.
    this.reproStartPC, this.reproEndPC = make(map[int64]bool), make(map[int64]bool)
    this.reproStarts = make(map[int]int64)
    this.reproCounts = make(map[int]int)
    addressBytes, addressErr := os.ReadFile(filepath.Join(command_line_parser.StringParameter("bin_dirpath"), "addresses.txt"))
    if addressErr != nil { panic(addressErr) }
    for _, line := range strings.Split(string(addressBytes), "\\n") {
        words := strings.SplitN(line, ":", 2)
        if len(words) != 2 { continue }
        address, err := strconv.ParseInt(strings.TrimSpace(words[1]), 10, 64)
        if err != nil { panic(err) }
        if strings.HasPrefix(words[0], "repro_begin_") { this.reproStartPC[address] = true }
        if strings.HasPrefix(words[0], "repro_end_") { this.reproEndPC[address] = true }
    }
    if len(this.reproStartPC) == 0 || len(this.reproEndPC) == 0 { panic("allocation probes missing") }
'''
    code = code.replace(needle, needle + addition, 1)
    needle = 'pc := thread.RegFile().ReadPcReg()'
    addition = '''
            tid := thread.ThreadId()
            if this.reproStartPC[pc] {
                if _, active := this.reproStarts[tid]; active { panic("nested allocation probe") }
                this.reproStarts[tid] = this.stat_factory.Value("logic_cycle")
            }
            if this.reproEndPC[pc] {
                start, active := this.reproStarts[tid]
                if !active { panic("allocation probe has no start") }
                end := this.stat_factory.Value("logic_cycle")
                elapsed := end - start
                this.stat_factory.Increment("repro_allocation_cycles", elapsed)
                this.stat_factory.Increment("repro_allocation_count", 1)
                fmt.Printf("REPRO_ALLOC,%d,%d,%d,%d,%d\\n", tid, this.reproCounts[tid], start, end, elapsed)
                this.reproCounts[tid]++
                delete(this.reproStarts, tid)
            }
'''
    code = code.replace(needle, needle + addition, 1)
    logic_path.write_text(code)
    patch_decode_cache(RUNTIME)
    patch_opcode_cache(RUNTIME)
    subprocess.run(['gofmt', '-w', str(RUNTIME / 'src/main.go'), str(logic_path),
                    str(RUNTIME / 'src/device/simulator/dpu/sram/iram.go'),
                    str(RUNTIME / 'src/device/linker/kernel/instruction/instruction.go')], check=True)
    (BASE / 'source_manifest.json').write_text(json.dumps({
        'paper_artifact_commit': subprocess.check_output(['git', '-C', str(WORKSPACE / 'PIM-malloc'), 'rev-parse', 'HEAD'], text=True).strip(),
        'baseline_source': str(SOURCE),
        'runtime': str(RUNTIME),
        'measurement': 'Per-thread issue-cycle timestamps at zero-instruction assembly labels immediately before and after each allocation; includes lock waiting, excludes allocator initialization.',
        'architectural_defaults': 'Unchanged from the authors software simulator; 350 MHz DPU, one channel/rank/DPU for these experiments.'
    }, indent=2) + '\n')


if __name__ == '__main__':
    main()

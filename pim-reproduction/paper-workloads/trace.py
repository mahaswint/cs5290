"""Paper-derived deterministic inputs, with independent semantic references."""
import functools
import hashlib
import json
import random
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DATA = ROOT / 'prim-benchmarks/BFS/data/loc-gowalla_edges.txt'
LARSON_REF = 'https://github.com/daanx/mimalloc-bench/blob/c65a68834c8fbd619f3ef449f72b9ec659cb551e/bench/larson/larson.cpp'


class LRan2:
    """Numerical Recipes shuffle generator used in the published Larson driver."""
    def __init__(self, seed):
        x = abs(150889 - seed) % 714025
        self.table = []
        for _ in range(97):
            x = (1366*x + 150889) % 714025
            self.table.append(x)
        self.x = self.y = (1366*x + 150889) % 714025

    def next(self):
        i = self.y % 97
        self.y = self.table[i]
        self.x = (1366*self.x + 150889) % 714025
        self.table[i] = self.x
        return self.y


def array(name, values):
    return f'static const unsigned {name}[{len(values)}] = {{' + ','.join(f'{v}U' for v in values) + '};\n'


def operations(name, records):
    return f'static PAPER_INPUT struct PaperOp {name}[{len(records)}] = {{\n' + ''.join(
        '  {' + ','.join(f'{v}U' for v in row) + '},\n' for row in records) + '};\n'


def larson(a):
    all_ops, offsets, starts, sums, live = [], [0], [], [], []
    upper = getattr(a, 'larson_max', 1000)
    # Independent worker streams preserve owner-local frees. The single-worker
    # warmup/permutation/replacement sequence follows the original driver.
    for t in range(a.threads):
        rng = LRan2(a.seed + t)
        sizes = [8 + rng.next() % (upper - 8) for _ in range(a.objects)]
        ops = [(0, i, size, 0) for i, size in enumerate(sizes)]
        model = [(size, i + 1) for i, size in enumerate(sizes)]
        for n in range(a.objects, 0, -1):
            victim = rng.next() % n
            ops.append((1, victim, n - 1, 0))
            model[victim], model[n - 1] = model[n - 1], model[victim]
        for _ in range(4 * a.objects):
            victim, size = rng.next() % a.objects, 8 + rng.next() % (upper - 8)
            ops.append((2, victim, size, len(ops) + 1))
            model[victim] = (size, len(ops))
        starts.append(len(all_ops) + len(ops))
        # Original driver initializes a worker generator from the warmup RNG.
        worker = LRan2(rng.next())
        for _ in range(a.operations):
            victim, size = worker.next() % a.objects, 8 + worker.next() % (upper - 8)
            ops.append((2, victim, size, len(ops) + 1))
            model[victim] = (size, len(ops))
        sums.append(sum(tag for _, tag in model) & 0xffffffff)
        live.append(sum(size for size, _ in model))
        all_ops.extend(ops); offsets.append(len(all_ops))
    header = operations('paper_ops', all_ops) + array('paper_offsets', offsets)
    header += array('paper_work_starts', starts) + array('paper_expected_sums', sums) + array('paper_expected_live', live)
    return header, dict(source=LARSON_REF, operation_records=len(all_ops), pool_per_tasklet=a.objects,
                        warmup_replacements_per_tasklet=4*a.objects, work_replacements_per_tasklet=a.operations,
                        requested_size_range=('8..999 B, uniform integer; same as mimalloc-bench command' if upper==1000 else
                                              f'8..{upper-1} B, uniform integer; explicit larger-size Larson parameterization'),
                        limitations='Scaled fixed operation count; independent tasklets, no thread respawn/object bleeding.',
                        expected_sums=sums, expected_live=live)


@functools.lru_cache(maxsize=16)
def sample_edges(count, seed):
    rng = random.Random(seed); sample = []; seen = 0
    raw = DATA.read_bytes()
    for line in raw.decode().splitlines()[1:]:
        u, v = map(int, line.split())
        if u >= v: continue
        seen += 1
        if len(sample) < count: sample.append((u, v))
        else:
            j = rng.randrange(seen)
            if j < count: sample[j] = (u, v)
    assert seen == 950327 and len(sample) == count and len(set(sample)) == count
    rng.shuffle(sample)
    return sample, hashlib.sha256(raw).hexdigest()


def signature(u, v):
    return (((u+1)*0x9e3779b1) ^ ((v+1)*0x85ebca6b)) & 0xffffffff


def graph(a):
    if getattr(a, 'fixture', False):
        edges = [(0, i) for i in range(1, 81)] + [(i, i+1) for i in range(1, 41)]
        dataset_sha = None
    else: edges, dataset_sha = sample_edges(a.graph_edges, a.seed)
    vertices = sorted({v for e in edges for v in e}); ids = {v:i for i,v in enumerate(vertices)}
    edges = [(ids[u], ids[v]) for u,v in edges]
    nnew = len(edges)//3
    new, base = edges[:nnew], edges[nnew:]
    rng = random.Random(a.seed ^ 0x5290)
    shards = [[] for _ in range(a.threads)]; work = [[] for _ in range(a.threads)]
    model = [set() for _ in vertices]
    def emit(target, kind, pair):
        u,v = pair
        for x,y in [(u,v),(v,u)]: target[x%a.threads].append((kind,x,y,0))
    if a.workload == 'faimgraph':
        for e in edges: emit(shards, 3, e)
        for _ in range(a.rounds):
            chosen = rng.sample(edges, nnew)
            for e in chosen: emit(work, 4, e)
            rng.shuffle(chosen)
            for e in chosen: emit(work, 3, e)
        final = edges
    else:
        for e in base: emit(shards, 3, e)
        for e in new: emit(work, 3, e)
        final = base + new
    for u,v in final: model[u].add(v); model[v].add(u)
    all_ops=[];offsets=[0];starts=[]
    for t in range(a.threads):
        all_ops += shards[t];starts.append(len(all_ops));all_ops += work[t];offsets.append(len(all_ops))
    sums=[sum(signature(u,v) for u in range(t,len(vertices),a.threads) for v in model[u]) & 0xffffffff for t in range(a.threads)]
    counts=[sum(len(model[u]) for u in range(t,len(vertices),a.threads)) for t in range(a.threads)]
    # Independent adjacency reference in Python, plus per-source degree/count.
    header = f'#define PAPER_VERTICES {len(vertices)}\n'
    header += operations('paper_ops', all_ops) + array('paper_offsets', offsets) + array('paper_work_starts', starts)
    header += array('paper_expected_sums', sums) + array('paper_expected_edges', counts)
    degrees = [len(s) for s in model]
    header += f'static PAPER_INPUT uint64_t paper_expected_degrees[{len(degrees)}] = {{' + ','.join(map(str,degrees)) + '};\n'
    exact = [(u,v) for u,s in enumerate(model) for v in sorted(s)]
    header += '#ifdef HOST_TEST\n' + operations('paper_exact_edges', [(0,u,v,0) for u,v in exact]) + '#endif\n'
    manifest=dict(dataset=str(DATA), dataset_sha256=dataset_sha, sampled_undirected_edges=len(edges),
                  vertices=len(vertices), base_edges=len(base), new_edges=nnew, split='1:2 new/existing',
                  operation_records=len(all_ops), original_vertex_ids=vertices, compact_edges=edges,
                  expected_sums=sums, expected_edges=counts, expected_degrees=degrees,
                  schedule='Uniform-over-existing-edge deletion/reinsertion' if a.workload=='faimgraph' else 'One insertion batch, paper-style append',
                  limitations='Single-DPU reduced graph; no GPU code/vertex updates; sampled static edges, synthetic updates.')
    return header, manifest


def make_trace(a):
    if a.workload == 'larson': header, manifest = larson(a)
    elif a.workload == 'prim-sel':
        header='';manifest=dict(source='CMU-SAFARI/prim-benchmarks/SEL',
            input_elements=a.items, block_bytes=256, predicate='retain odd uint64 values (!pred)',
            limitations='Single-DPU kernel port with per-tasklet static output partitions; no allocator calls in work.')
    else: header, manifest = graph(a)
    manifest |= dict(workload=a.workload, seed=a.seed, threads=a.threads,
                     trace_sha256=hashlib.sha256(header.encode()).hexdigest())
    return header, manifest

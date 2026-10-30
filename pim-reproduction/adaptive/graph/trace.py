"""Deterministic edge-sampled Gowalla input and an explicit synthetic schedule."""
import functools
import hashlib
import json
from pathlib import Path
import random
import csv
import math
import statistics

HERE = Path(__file__).resolve().parent
DATA = HERE.parents[2] / 'prim-benchmarks/BFS/data/loc-gowalla_edges.txt'
PHASE_NAMES = ['low', 'high', 'low-again', 'rebound']


def signature(source, destination):
    return (((source + 1) * 0x9e3779b1) ^ ((destination + 1) * 0x85ebca6b)) & 0xffffffff


@functools.lru_cache(maxsize=16)
def make_trace(edges=384, seed=5290, threads=4):
    if edges < 48 or edges % 3 or edges > 3072:
        raise ValueError('graph-edges must be a multiple of 3 in 48..3072')
    # Reservoir sampling scans the whole input; it does not select the first,
    # unusually high-degree users in PrIM's source-sorted COO file.
    rng = random.Random(seed)
    reservoir, seen = [], 0
    raw = DATA.read_bytes()
    lines = raw.decode().splitlines()
    rows, columns, directed_count = map(int, lines[0].split())
    assert rows == columns == 196591 and directed_count == len(lines) - 1
    for line in lines[1:]:
        source, destination = map(int, line.split())
        if source >= destination:
            continue
        seen += 1
        if len(reservoir) < edges:
            reservoir.append((source, destination))
        else:
            index = rng.randrange(seen)
            if index < edges:
                reservoir[index] = (source, destination)
    assert seen == 950327 and len(set(reservoir)) == edges
    rng.shuffle(reservoir)
    new_count = edges // 3
    new, base = reservoir[:new_count], reservoir[new_count:]
    original_vertices = sorted({v for edge in reservoir for v in edge})
    remap = {v: i for i, v in enumerate(original_vertices)}
    limits = [max(1, new_count // 16), new_count,
              max(1, new_count // 8), max(1, new_count // 2)]
    base_shards, update_shards = [[] for _ in range(threads)], [[] for _ in range(threads)]
    for source, destination in base:
        for u, v in [(source, destination), (destination, source)]:
            base_shards[remap[u] % threads].append((remap[u], v))
    for rank, (source, destination) in enumerate(new):
        for u, v in [(source, destination), (destination, source)]:
            update_shards[remap[u] % threads].append((remap[u], v | (rank << 20)))
    base_sums = [sum(signature(u, v) for u, v in shard) & 0xffffffff for shard in base_shards]
    base_counts = list(map(len, base_shards))
    expected_sums, expected_counts = [base_sums], [base_counts]
    for limit in limits:
        selected = [[(u, packed & 0xfffff) for u, packed in shard if packed >> 20 < limit]
                    for shard in update_shards]
        expected_sums.append([(base_sums[t] + sum(signature(u, v) for u, v in selected[t])) & 0xffffffff
                              for t in range(threads)])
        expected_counts.append([base_counts[t] + len(selected[t]) for t in range(threads)])

    def array(name, values):
        return f'static const unsigned {name}[{len(values)}] = {{' + ','.join(map(str, values)) + '};\n'

    def edge_array(name, shards):
        values, offsets = [], [0]
        for shard in shards:
            values.extend(shard); offsets.append(len(values))
        text = f'static const struct GraphEdge {name}[{len(values)}] = {{\n'
        text += '\n'.join(f'  {{{u}U,{v}U}},' for u, v in values) + '\n};\n'
        return text + array(name + '_offsets', offsets)

    header = f'#define GRAPH_VERTICES {len(original_vertices)}\n#define GRAPH_NEW_EDGES {new_count}\n'
    header += f'#define GRAPH_BASE_ARCS {2 * len(base)}\n#define GRAPH_UPDATE_ARCS {2 * len(new)}\n'
    header += 'struct GraphEdge { unsigned source, destination; };\n'
    header += array('graph_limits', limits)
    header += edge_array('graph_base', base_shards) + edge_array('graph_updates', update_shards)
    for name, values in [('graph_expected_sums', expected_sums), ('graph_expected_counts', expected_counts)]:
        header += f'static const unsigned {name}[5][NR_TASKLETS] = {{\n'
        header += '\n'.join('  {' + ','.join(f'{v}U' for v in row) + '},' for row in values) + '\n};\n'
    manifest = {
        'dataset': str(DATA), 'dataset_sha256': hashlib.sha256(raw).hexdigest(),
        'dataset_url': 'https://snap.stanford.edu/data/loc-gowalla.html',
        'input_format': 'PrIM COO; both directions; reservoir samples only source < destination',
        'full_nodes': rows, 'full_undirected_edges': seen, 'sampled_undirected_edges': edges,
        'sampled_vertices': len(original_vertices), 'base_undirected_edges': len(base),
        'update_undirected_edges': len(new), 'seed': seed, 'threads': threads,
        'base_arcs_per_tasklet': base_counts,
        'update_arcs_per_tasklet': list(map(len, update_shards)),
        'phase_names': PHASE_NAMES, 'new_undirected_edges_per_phase': limits,
        'new_adjacency_arcs_per_phase': [2 * n for n in limits],
        'read_passes_per_phase': [4, 1, 4, 1],
        'schedule_kind': 'synthetic closed-loop batch schedule; not measured arrival timestamps',
        'sampling': 'uniform reservoir of undirected edges, seed-fixed 1:2 new/existing split',
        'source_vertices_original_ids': original_vertices,
        'undirected_edges_new': new, 'undirected_edges_base': base,
        'expected_checksums': expected_sums, 'expected_arc_counts': expected_counts,
        'trace_sha256': hashlib.sha256(header.encode()).hexdigest(),
    }
    return header, manifest


def add_measurements(result, events, out):
    """Separate measured graph updates from graph preload and final teardown."""
    manifest = json.loads((out / 'trace.json').read_text())
    c = result['configuration']
    work = [e for e in events if e['kind'] == 'work']
    assert len(work) == 4 * c['rounds'] * c['threads']
    rows, all_alloc, all_free = [], [], []

    def metrics(values):
        return {'samples': len(values), 'mean_us': statistics.mean(values) if values else None,
                'p99_us': sorted(values)[max(0, math.ceil(.99 * len(values)) - 1)] if values else None}

    for index in range(4 * c['rounds']):
        phase = index % 4
        windows = {w['thread']: w for w in work if w['index'] == index}
        assert len(windows) == c['threads']
        selected = [e for e in events if e['kind'] in ('alloc', 'free') and
                    windows[e['thread']]['begin_cycle'] <= e['begin_cycle'] and
                    e['end_cycle'] <= windows[e['thread']]['end_cycle']]
        allocations = [e['cycles'] / 350 for e in selected if e['kind'] == 'alloc']
        frees = [e['cycles'] / 350 for e in selected if e['kind'] == 'free']
        all_alloc.extend(allocations); all_free.extend(frees)
        begin = min(w['begin_cycle'] for w in windows.values())
        end = max(w['end_cycle'] for w in windows.values())
        am, fm = metrics(allocations), metrics(frees)
        rows.append({'index': index, 'cycle': index // 4, 'phase': PHASE_NAMES[phase],
                     'begin_cycle': begin, 'end_cycle': end, 'duration_us': (end - begin) / 350,
                     'attempted_insert_arcs': manifest['new_adjacency_arcs_per_phase'][phase],
                     'read_passes': manifest['read_passes_per_phase'][phase],
                     'allocations': am['samples'], 'alloc_mean_us': am['mean_us'], 'alloc_p99_us': am['p99_us'],
                     'frees': fm['samples'], 'free_mean_us': fm['mean_us']})
    with (out / 'graph_phases.csv').open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0])); writer.writeheader(); writer.writerows(rows)
    total = result['totals']
    result['graph'] = {
        'update_alloc': metrics(all_alloc), 'update_free': metrics(all_free), 'phases': rows,
        'update_phase_makespan_us': sum(r['duration_us'] for r in rows),
        'attempted_insert_arcs': sum(r['attempted_insert_arcs'] for r in rows),
        'frontend_hit_fraction_all_requests': total['cache_hits'] / total['alloc_calls'] if total['alloc_calls'] else None,
        'counter_scope': 'Allocator counters include preload and teardown; update latency uses work windows only.',
        'trace_sha256': manifest['trace_sha256'], 'dataset_sha256': manifest['dataset_sha256'],
        'sampled_vertices': manifest['sampled_vertices'],
        'block_bytes': c['graph_block_bytes'], 'schedule_kind': manifest['schedule_kind']}


if __name__ == '__main__':
    _, manifest = make_trace()
    print(json.dumps({k: v for k, v in manifest.items() if not isinstance(v, list)}, indent=2))

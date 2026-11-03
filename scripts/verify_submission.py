#!/usr/bin/env python3
"""Check the saved final matrix and deliverables without rerunning simulations."""
from collections import Counter, defaultdict
import csv
import hashlib
import json
import math
from pathlib import Path
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT / 'pim-reproduction'
SUBMISSION = PROJECT / 'submission'
VARIANTS = {
    'baseline', 'indexed-adaptive64', 'indexed-fixed2', 'slab-adaptive64',
    'slab-fixed2', 'slab-pinned-adaptive64', 'slab-pinned-fixed2',
}


def check(condition, message):
    if not condition:
        raise ValueError(message)


def verify():
    with (SUBMISSION / 'evidence/final-98-runs.csv').open(newline='') as stream:
        rows = list(csv.DictReader(stream))
    check(len(rows) == 98, f'Expected 98 final rows, found {len(rows)}')
    groups = defaultdict(list)
    for row in rows:
        groups[row['family'], row['workload'], row['threads']].append(row['variant'])
        path = (ROOT / row['raw_summary']).resolve()
        check(path.is_relative_to(PROJECT), f'Summary outside project: {path}')
        data = json.loads(path.read_text())
        check(data.get('correctness_passed') is True, f'Correctness failed: {path}')
        check(data.get('outcome') == 'completed', f'Incomplete run: {path}')
        for key in ['source_sha256', 'simulator_sha256']:
            check(row[key] == data[key], f'{key} mismatch: {path}')
        check(data['configuration']['variant'] == row['variant'], f'Variant mismatch: {path}')
        check(data['configuration']['threads'] == int(row['threads']), f'Tasklet mismatch: {path}')
        check(data['configuration']['workload'] == row['workload'], f'Workload mismatch: {path}')
        check(math.isclose(float(row['work_ms']), data['work_makespan_us'] / 1000,
                           rel_tol=1e-12), f'Work timing mismatch: {path}')
        check(math.isclose(float(row['total_ms']), data['metrics']['total']['mean_us'] / 1000,
                           rel_tol=1e-12), f'Total timing mismatch: {path}')
        check(0 < int(row['wram_heap_end']) <= 65536, f'WRAM endpoint exceeds limit: {path}')
    check(len(groups) == 14, f'Expected 14 workload settings, found {len(groups)}')
    for key, variants in groups.items():
        check(Counter(variants) == Counter(VARIANTS), f'Variant matrix mismatch: {key}')
    check(Counter(row['family'] for row in rows) == {'main-stack': 35, 'paper-cases': 63},
          'Unexpected family sizes')
    print('PASS: 98 completed summaries; 14 settings with all seven variants; timing and source pins match')
    output = SUBMISSION / 'output'
    for name in ['pdf/report.pdf', 'pdf/baseline-results.pdf', 'pdf/contribution-results.pdf',
                 'presentation.pptx', 'speaker-notes.md', 'tables/final-progression.csv',
                 'tables/traffic-and-memory.csv']:
        check((output / name).is_file() and (output / name).stat().st_size > 0,
              f'Missing deliverable: {name}')
    archives = ['report-latex-source.zip', 'latex-source.zip', 'baseline-results.zip',
                'contribution-results.zip', 'source-code.zip', 'submission.zip', 'presentation.pptx']
    for name in archives:
        with zipfile.ZipFile(output / name) as archive:
            check(archive.testzip() is None, f'Corrupt archive: {name}')
    with zipfile.ZipFile(output / 'source-code.zip') as archive:
        manifest = json.loads(archive.read('SOURCE_MANIFEST.json'))
        for entry in manifest:
            data = archive.read(entry['path'])
            check(hashlib.sha256(data).hexdigest() == entry['sha256'],
                  f"Source archive checksum mismatch: {entry['path']}")
    with zipfile.ZipFile(output / 'submission.zip') as archive:
        for line in archive.read('SHA256SUMS').decode().splitlines():
            digest, name = line.split('  ', 1)
            check(hashlib.sha256(archive.read(name)).hexdigest() == digest,
                  f'Bundle checksum mismatch: {name}')
    print('PASS: deliverables, archive integrity, source manifest, and bundle checksums')


def main():
    try:
        verify()
    except (ValueError, KeyError, OSError, zipfile.BadZipFile) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())

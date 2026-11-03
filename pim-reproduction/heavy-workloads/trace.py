"""Unmodified CloudPhysics request sizes/order, with a separate LRU oracle."""
import csv
import hashlib
import json
from collections import OrderedDict, Counter
from pathlib import Path

HERE=Path(__file__).resolve().parent
DATA=HERE/'data/cloudPhysicsIO.csv'
SHA256='987ff2213050e47d24e8ba6e010d4b3127e51aafef6a76a8a6d43d13b9156fa1'
COMMIT='c6ffd976c83d9e731b37fc91e1ce930039f5ddb9'


def owner(key,threads):
    return (key ^ (key>>16))%threads


def array(name,values):
    return f'static const unsigned {name}[{len(values)}]={{'+','.join(str(v)+'U' for v in values)+'};\n'


def make_trace(a):
    raw=DATA.read_bytes();assert hashlib.sha256(raw).hexdigest()==SHA256
    rows=list(csv.DictReader(raw.decode().splitlines()))
    selected=rows[a.trace_offset:a.trace_offset+a.warmup+a.operations]
    assert len(selected)==a.warmup+a.operations
    shards=[[] for _ in range(a.threads)];work=[[] for _ in range(a.threads)]
    model=[OrderedDict() for _ in range(a.threads)];latest={}
    counts=[dict(allocations=0,frees=0,hits=0,work_allocations=0,work_frees=0,work_hits=0) for _ in range(a.threads)]
    sizes=Counter();work_sizes=Counter()
    for i,r in enumerate(selected):
        key,size=int(r['lbn']),int(r['size']);command=int(r['op'],16)
        assert 0<=key<2**32 and size>0 and size%8==0
        assert command in [0x28,0x2a]
        t=owner(key,a.threads);cache=model[t];timed=i>=a.warmup
        write=command==0x2a;tag=(a.trace_offset+i+1) if write else latest.get(key,0)
        if write:latest[key]=tag
        hit=key in cache and cache[key][0]==size
        count=counts[t]
        if key in cache and not hit:
            del cache[key];count['frees']+=1
            if timed:count['work_frees']+=1
        if hit:
            count['hits']+=1
            if timed:count['work_hits']+=1
            del cache[key]
        else:
            if len(cache)==a.objects:
                cache.popitem(last=False);count['frees']+=1
                if timed:count['work_frees']+=1
            count['allocations']+=1;sizes[size]+=1
            if timed:count['work_allocations']+=1;work_sizes[size]+=1
        cache[key]=(size,tag)
        (work if timed else shards)[t].append((int(write),key,size,tag))
    ops=[];offsets=[0];starts=[]
    for t in range(a.threads):
        ops+=shards[t];starts.append(len(ops));ops+=work[t];offsets.append(len(ops))
    header='static PAPER_INPUT struct PaperOp paper_ops['+str(len(ops))+']={\n'
    header+=''.join('  {'+','.join(str(v)+'U' for v in op)+'},\n' for op in ops)+'};\n'
    header+=array('paper_offsets',offsets)+array('paper_work_starts',starts)
    for field in ['allocations','frees','hits','work_allocations','work_frees','work_hits']:
        header+=array('cache_expected_'+field,[c[field] for c in counts])
    header+=array('cache_expected_live',[len(c) for c in model])
    expected=[];expected_offsets=[0]
    for cache in model:
        expected.extend((key,size,tag) for key,(size,tag) in cache.items());expected_offsets.append(len(expected))
    header+='static const struct PaperOp cache_expected_final['+str(max(1,len(expected)))+']={\n'
    header+=''.join('  {0,'+','.join(str(v)+'U' for v in row)+'},\n' for row in expected)+'};\n'
    header+=array('cache_expected_offsets',expected_offsets)
    manifest=dict(workload=a.workload,source_commit=COMMIT,dataset_sha256=SHA256,
        dataset_url=f'https://github.com/1a1a11a/libCacheSim/blob/{COMMIT}/data/cloudPhysicsIO.csv',
        original_records=len(rows),trace_offset=a.trace_offset,selected_records=len(selected),
        warmup_records=a.warmup,work_records=a.operations,threads=a.threads,cache_entries_per_tasklet=a.objects,
        counts=counts,request_sizes=dict(sorted(sizes.items())),work_request_sizes=dict(sorted(work_sizes.items())),
        final_requested_bytes=sum(size for cache in model for size,tag in cache.values()),
        trace_sha256=hashlib.sha256(header.encode()).hexdigest(),
        limitations='Reduced sharded LRU write-cache kernel, original I/O lengths and per-owner order; no storage latency, timestamps, original application, cache-policy comparison or production miss-ratio claim.')
    return header,manifest

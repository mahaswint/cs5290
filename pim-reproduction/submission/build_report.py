#!/usr/bin/env python3
"""Generate data tables from the saved measurement CSV files."""
import csv,json,re
from pathlib import Path
HERE=Path(__file__).resolve().parent;ROOT=HERE.parent;LATEX=HERE/'latex'
TABLES=LATEX/'tables';TABLES.mkdir(exist_ok=True)
def rows(p):return list(csv.DictReader(p.open()))
def f(r,k):return float(r[k])
def inline_tables(doc):
 return re.sub(r'\\input\{tables/([^}]+)\}',lambda m:(TABLES/m[1]).read_text(),doc)
def esc(s):
 s=str(s)
 return ''.join({'&':r'\&','%':r'\%','_':r'\_','#':r'\#','$':r'\$'}.get(c,c) for c in s).replace('–','--').replace('→',r'$\rightarrow$').replace('µ',r'$\mu$')
FINAL=rows(HERE/'evidence/final-98-runs.csv')
VARIANTS=['baseline','indexed-adaptive64','indexed-fixed2','slab-adaptive64','slab-fixed2','slab-pinned-adaptive64','slab-pinned-fixed2']
LABEL={'larson':'Larson 8--4095 B','cloud-cache':'CloudPhysics cache','larson-wide':'Larson 8--8191 B','graph-churn':'Gowalla churn','prim-sel':'PrIM selection',
 'pim-list':'Gowalla linked blocks','pim-array':'Gowalla growing arrays','kv-growth':'KV growth replay','micro-32':'32 B microbenchmark','micro-256':'256 B microbenchmark','micro-4096':'4 KiB microbenchmark'}
CASES=list(dict.fromkeys((r['family'],r['workload'],int(r['threads'])) for r in FINAL))
def group(c):return {r['variant']:r for r in FINAL if (r['family'],r['workload'],int(r['threads']))==c}
def case_label(c):return LABEL[c[1]]+f' ({c[2]}T)'
def table(filename,caption,head,data,align,label,wide=True,note=''):
 env='table*' if wide else 'table'
 text='\\begin{'+env+'}[t]\n\\caption{'+caption+'}\\label{'+label+'}\n\\centering\\small\n'
 if filename=='traffic.tex':text+='\\setlength{\\tabcolsep}{4.5pt}\n'
 text+='\\begin{tabular}{'+align+'}\\toprule\n'+' & '.join(head)+r' \\ \midrule'+'\n'
 text+='\n'.join(' & '.join(map(str,r))+r' \\' for r in data)+'\n\\bottomrule\\end{tabular}\n'
 if note:text+='\\par\\vspace{2pt}{\\footnotesize '+note+'}\n'
 text+='\\end{'+env+'}\n'
 (TABLES/filename).write_text(text)
 return '\\input{tables/'+filename+'}\n'

def make_tables():
 base=rows(ROOT/'analysis/figure15_comparison.csv');data=[]
 for t in [1,16]:
  for s in [32,256,4096]:
   b=next(r for r in base if int(r['threads'])==t and int(r['size_bytes'])==s and r['allocator']=='buddy')
   sw=next(r for r in base if int(r['threads'])==t and int(r['size_bytes'])==s and r['allocator']=='sw')
   data.append([t,s,f"{f(b,'paper_latency_us'):g}",f"{f(b,'measured_latency_us'):.2f}",f"{f(sw,'paper_latency_us'):g}",f"{f(sw,'measured_latency_us'):.2f}",f"{f(b,'measured_latency_us')/f(sw,'measured_latency_us'):.2f}"])
 table('baseline.tex','Original Figure 15 artifact reproduction: batch time divided by 128 requests per tasklet, in microseconds.',
   ['Tasklets','Bytes','Paper buddy','Measured buddy','Paper SW','Measured SW','Buddy/SW'],data,'rrrrrrr','tab:baseline',
   note='The original simulator layout is preserved here. These latencies are separate from the repaired allocator and 64 KiB WRAM configuration used for contributions.')
 ideas=[['Adaptive empty retention','Implemented','Retain/release wholly empty pages; pressure and changing demand'],
 ['Sharded buddy arenas','Deferred','Partitioning, arena ownership and fallback require a separate study'],
 ['Scalable allocation/free','Implemented','Partial/full/empty lists plus direct MRAM ownership map'],
 ['Lifetime regions','Deferred','Requires explicit application lifetime boundaries and API changes'],
 ['Cross-DPU balancing','Deferred','Host placement, data movement and pointer relocation exceed this scope'],
 ['Pinned buddy metadata','Implemented','Keep upper tree in shared WRAM; stream lower metadata'],
 ['Adaptive classes/slabs','Partly implemented','Static 16/32/64 KiB geometry for a new 4 KiB class; no online migration'],
 ['Long-running workloads','Implemented as evaluation','Persistent pools, churn, phase changes and trace replay']]
 table('ideas.tex','Design directions considered and the implemented scope.', ['Direction','Decision','Reason or realization'],
  [[esc(x) for x in r] for r in ideas],r'@{}p{.23\textwidth}p{.15\textwidth}p{.53\textwidth}@{}','tab:ideas')
 stage=[['Reference reproduction','Original buddy / PIM-malloc-SW','Released layout','128 allocations/tasklet'],
 ['Retention and indexing','Maintained scan controls','16 KiB/tasklet','Normally 64 alloc+free operations'],
 ['Check-frequency study','Indexed adaptive-64','16 KiB/tasklet','Paired 64 versus 128'],
 ['Stacked slabs / heavy requests','Indexed, same retention policy','256 KiB/tasklet','128 alloc+free operations'],
 ['Final complete stack','Scan fixed-2','256 KiB/tasklet','64 alloc+free operations']]
 table('stages.tex','Comparison contracts across the progressive evaluation. Ratios are formed only within a stage.',
  ['Stage','Baseline','Empty-byte budget','Adaptive interval / workload'],stage,r'@{}p{.22\textwidth}p{.26\textwidth}p{.16\textwidth}p{.27\textwidth}@{}','tab:stages')
 adaptive=[['Regular phases, 1T','fixed-2',5.48,32],['Regular phases, 1T','adaptive',6.21,32],
  ['Regular phases, 4T','fixed-2',16.25,128],['Regular phases, 4T','adaptive',18.52,128],
  ['Regular phases, 4T','eager',29.66,192],['Regular phases, 4T','lazy',23.82,96]]
 table('adaptive.tex','Early retention experiment: regular phases do not favor adaptive bookkeeping.',
  ['Case','Policy','Total (ms)','Peak (KiB)'],[[r[0],r[1],f'{r[2]:.2f}',r[3]] for r in adaptive],'llrr','tab:adaptive',wide=False)
 modes=rows(ROOT/'paper-workloads/analysis/all-runs.csv');data=[]
 for w,l in [('larson','Larson 8--999 B'),('faimgraph','Gowalla churn')]:
  g={r['mode']:r for r in modes if r['preset']=='ablation' and r['workload']==w}
  for m in ['scan','lists','lookup','indexed']:
   r=g[m];data.append([l,m,f"{f(r,'work_ms'):.3f}",f"{f(g['scan'],'work_ms')/f(r,'work_ms'):.3f}",
    f"{f(r,'work_alloc_mean_us'):.2f}",f"{f(r,'work_free_mean_us'):.2f}"])
 table('ablation.tex','Allocation/free mechanisms isolated at one tasklet with fixed-2 retention and unchanged input.',
  ['Workload','Mode','Work (ms)','Speedup',r'Alloc. mean ($\mu$s)',r'Free mean ($\mu$s)'],data,'llrrrr','tab:ablation')
 data=[]
 for c in CASES:
  if c[0]!='main-stack':continue
  g=group(c);data.append([case_label(c)]+[f"{f(g[v],'work_ms'):.3f}" for v in VARIANTS])
 table('final-main.tex','Final work runtime (milliseconds) for every stage on five complementary cases.',
  ['Case','B','IA','IF','SA','SF','PA','PF'],data,'lrrrrrrr','tab:final-main',
  note='B: scan fixed-2; I: indexed; S: indexed plus 64 KiB slabs; P: indexed plus slabs plus pinned metadata. A uses adaptive interval 64; F retains at most two empty spans/class. All rows use four tasklets.')
 data=[]
 for c in CASES:
  if c[0]!='paper-cases':continue
  g=group(c);b=g['baseline'];pa=g['slab-pinned-adaptive64'];pf=g['slab-pinned-fixed2']
  data.append([case_label(c),f"{f(b,'work_ms'):.3f}",f"{f(pa,'work_ms'):.3f}",f"{f(pf,'work_ms'):.3f}",
    f"{f(b,'work_ms')/f(pa,'work_ms'):.3f}",f"{f(b,'work_ms')/f(pf,'work_ms'):.3f}",f"{f(b,'total_ms')/f(pf,'total_ms'):.3f}"])
 table('paper-final.tex','Final paper-derived cases: the baseline and both full-stack policies.',
  ['Case','B work (ms)','PA work (ms)','PF work (ms)','PA speedup','PF speedup','PF total speedup'],data,'lrrrrrr','tab:paper-final',
  note='Graph cases are reduced ports; KV is an allocation-only replay and excludes attention/serving. Original request sizes and 128 microbenchmark requests/tasklet are retained. Values below one are regressions.')
 data=[]
 for c in CASES:
  g=group(c);b=g['baseline'];pf=g['slab-pinned-fixed2']
  data.append([case_label(c),f"{int(f(b,'work_allocations')):,}",f"{int(f(b,'backend_allocations')):,}",f"{int(f(pf,'backend_allocations')):,}",
   f"{f(b,'dram_bytes')/2**20:.3f}",f"{f(pf,'dram_bytes')/2**20:.3f}",f"{f(b,'peak_payload_bytes')/1024:.0f}",f"{f(pf,'peak_payload_bytes')/1024:.0f}"])
 table('traffic.tex','Allocation demand, full-execution backend/DRAM activity and peak payload reservation.',
  ['Case','Timed allocs','B buddy','PF buddy','B DRAM MiB','PF DRAM MiB','B peak KiB','PF peak KiB'],data,'lrrrrrrr','tab:traffic',
  note='Buddy counts and DRAM bytes cover the full execution; they are not timed-work counts. Peak excludes the extra 64 KiB owner map, buddy metadata and identical application tables. DRAM counters exclude WRAM and instruction fetches.')
 data=[]
 for c in CASES:
  if c[0]!='main-stack':continue
  g=group(c)
  for policy,pin,slab in [('A','slab-pinned-adaptive64','slab-adaptive64'),('F','slab-pinned-fixed2','slab-fixed2')]:
   a=g[pin];b=g[slab];data.append([LABEL[c[1]],policy,f"{f(b,'work_ms')/f(a,'work_ms'):.3f}",
    f"{f(b,'total_ms')/f(a,'total_ms'):.3f}",f"{100*(1-f(a,'dram_bytes')/f(b,'dram_bytes')):.2f}"])
 table('pinning.tex','Pinning isolated from the slab and retention policy.',
  ['Workload (4T)','Policy','Work speedup','Total speedup',r'DRAM-byte reduction (\%)'],data,'llrrr','tab:pinning',
  note='The pinned variant uses a measured 540 B additional WRAM endpoint in each pair. It is an added-storage comparison, not an equal-WRAM-budget replacement experiment.')
 variants=[['B','Scan + fixed-2','4 KiB pages, classes through 2 KiB','None'],
 ['IA / IF','Indexed + adaptive-64 / fixed-2','Same small classes','64 KiB owner map (MRAM)'],
 ['SA / SF','Indexed + slabs + A / F','New 4 KiB class, 64 KiB MRAM spans','Ninth class descriptors'],
 ['PA / PF','Indexed + slabs + pinning + A / F','Same geometry and policy','528 B WRAM prefix; 540 B endpoint delta']]
 table('variants.tex','Final implementation progression and additional storage.', ['Variants','Mechanism','Geometry','Additional storage'],variants,
  r'@{}p{.09\textwidth}p{.29\textwidth}p{.27\textwidth}p{.27\textwidth}@{}','tab:variants')
 table('variants-compact.tex','The seven allocator variants.', ['Labels','Changes from the common baseline'],
  [['B','Scan + fixed-2, classes through 2 KiB'],
   ['IA / IF','Indexed lists and lookup, adaptive-64 / fixed-2'],
   ['SA / SF','Indexed + 64 KiB slabs, adaptive-64 / fixed-2'],
   ['PA / PF','Indexed + slabs + pinned metadata, adaptive-64 / fixed-2']],
  r'@{}lp{.68\columnwidth}@{}','tab:variants',wide=False)
 # The main paper uses short summaries; the result extracts keep full tables.
 compact=[]
 for t in [1,16]:
  for s in [32,256,4096]:
   r=next(r for r in base if int(r['threads'])==t and int(r['size_bytes'])==s and r['allocator']=='sw')
   compact.append([t,'4 KiB' if s==4096 else f'{s} B',f"{f(r,'paper_latency_us'):g}",f"{f(r,'measured_latency_us'):.2f}"])
 table('baseline-summary.tex','Software allocator reproduction. Latency is batch time / 128, in microseconds.',
  ['Tasklets','Request','Paper','Reproduced'],compact,'rlrr','tab:baseline',wide=False)
 compact=[]
 selected=[('main-stack','larson',4,'Larson (4T)'),('main-stack','cloud-cache',4,'Cache (4T)'),
           ('main-stack','graph-churn',4,'Churn (4T)'),('paper-cases','micro-4096',16,'4 KiB (16T)'),
           ('paper-cases','micro-32',1,'32 B (1T)')]
 for family,workload,threads,label in selected:
  g=group((family,workload,threads));b=g['baseline'];p=g['slab-pinned-fixed2']
  precision=3 if workload=='micro-32' else 2
  peak=(f(p,'peak_payload_bytes')-f(b,'peak_payload_bytes'))/1024
  compact.append([label,f"{f(b,'dram_bytes')/2**20:.{precision}f}",
                  f"{f(p,'dram_bytes')/2**20:.{precision}f}",f'{peak:+.0f}' if peak else '0'])
 table('memory-summary.tex','Selected memory costs. B and PF give full-execution DRAM traffic in MiB; the last column is extra peak payload in KiB.',
  ['Case','B MiB','PF MiB','Peak +KiB'],compact,'lrrr','tab:traffic',wide=False,
  note='PF is the full fixed stack. The separate 64 KiB ownership map is excluded from payload peaks.')

def main():
 make_tables()
 print("Updated data tables. The report source is latex/report.tex.")

if __name__=="__main__":main()

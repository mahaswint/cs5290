#!/usr/bin/env python3
"""Package the paper, editable deck, result tables and canonical source snapshot."""
from pathlib import Path
import csv,hashlib,json,re,shutil,zipfile

HERE=Path(__file__).resolve().parent
ROOT=HERE.parent
REPO=ROOT.parent
OUT=HERE/'output'
BUILD=HERE/'build/package'
VARIANTS=['baseline','indexed-adaptive64','indexed-fixed2','slab-adaptive64','slab-fixed2','slab-pinned-adaptive64','slab-pinned-fixed2']
FIG_INPUTS=['analysis/figure15_comparison.csv','paper-workloads/analysis/all-runs.csv',
 'paper-workloads/analysis/interval128-comparisons.csv','slabs/analysis/slabs-full-v1/comparison.csv',
 'stacked/analysis/comparisons.csv']

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def rows(p):return list(csv.DictReader(p.open()))
def writecsv(p,data):
 p.parent.mkdir(parents=True,exist_ok=True)
 with p.open('w',newline='') as f:
  w=csv.DictWriter(f,fieldnames=list(data[0]));w.writeheader();w.writerows(data)
def copy(src,dst):
 dst.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(src,dst)
def zipdir(src,dst):
 with zipfile.ZipFile(dst,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as z:
  for p in sorted(src.rglob('*')):
   if p.is_file():z.write(p,p.relative_to(src))
 with zipfile.ZipFile(dst) as z:
  assert z.testzip() is None
 print(dst.name, len(z.namelist()), dst.stat().st_size,flush=True)

def main():
 if BUILD.exists():shutil.rmtree(BUILD)
 BUILD.mkdir(parents=True,exist_ok=True)
 final=rows(HERE/'evidence/final-98-runs.csv')
 cases=list(dict.fromkeys((r['family'],r['workload'],r['threads']) for r in final))
 groups={c:{r['variant']:r for r in final if (r['family'],r['workload'],r['threads'])==c} for c in cases}
 tables=OUT/'tables';tables.mkdir(exist_ok=True)
 wide=[];traffic=[]
 for c in cases:
  g=groups[c];b=g['baseline'];f=g['slab-pinned-fixed2']
  r=dict(family=c[0],workload=c[1],tasklets=c[2])
  for v in VARIANTS:
   for key in ['work_ms','total_ms']:
    r[v+'_'+key]=g[v][key]
   r[v+'_work_speedup']=float(b['work_ms'])/float(g[v]['work_ms'])
   r[v+'_total_speedup']=float(b['total_ms'])/float(g[v]['total_ms'])
  wide.append(r)
  t=dict(family=c[0],workload=c[1],tasklets=c[2])
  for v in ['baseline','slab-pinned-adaptive64','slab-pinned-fixed2']:
   for key in ['work_allocations','backend_allocations','dram_commands','dram_bytes','peak_payload_bytes','wram_heap_end','pinned_metadata_bytes']:
    if key in g[v]:t[v+'_'+key]=g[v][key]
  traffic.append(t)
 writecsv(tables/'final-progression.csv',wide)
 writecsv(tables/'traffic-and-memory.csv',traffic)
 writecsv(tables/'baseline-final.csv',[r for r in final if r['variant']=='baseline'])
 copy(ROOT/'analysis/figure15_comparison.csv',tables/'original-paper-baseline.csv')
 (tables/'README.md').write_text('Units and scope\n\nRuntime columns are milliseconds. Speedups divide the matching baseline phase by the variant phase; values below one are regressions. Work is the concurrent application phase. Total includes initialization/preload/checking/drain. DRAM commands and bytes cover full execution including final accounting, and exclude WRAM and instruction fetch. Payload peaks exclude the 64 KiB ownership map and shared buddy metadata. IA/SA/PA use interval 64 over allocations plus frees. Fixed-2 caps empty retention. The original paper baseline uses batch/128 microseconds and a different runtime layout; it must remain separate.\n')
 # A self-contained Overleaf project, including figure regeneration inputs.
 latex=BUILD/'latex-source';latex.mkdir(exist_ok=True)
 for p in (HERE/'latex').rglob('*'):
  if p.is_file():copy(p,latex/p.relative_to(HERE/'latex'))
 for name in FIG_INPUTS:copy(ROOT/name,latex/'data/history'/name)
 copy(HERE/'evidence/final-98-runs.csv',latex/'evidence/final-98-runs.csv')
 for name in ['final-progression.csv','traffic-and-memory.csv','README.md']:copy(tables/name,latex/'data'/name)
 plotting=(HERE/'figures.py').read_text().replace("HERE=Path(__file__).resolve().parent;ROOT=HERE.parent", "HERE=Path(__file__).resolve().parent.parent;ROOT=HERE/'data/history'")
 plotting=plotting.replace("OUT=HERE/'latex/figures'","OUT=HERE/'figures'").replace("inventory=json.loads((HERE/'evidence/inventory.json').read_text())","inventory={}")
 plotting=plotting.replace("HERE/'latex/artifacts'","HERE/'artifacts'")
 plotting=plotting.replace("OUT.mkdir(exist_ok=True)","OUT.mkdir(exist_ok=True);(HERE/'build').mkdir(exist_ok=True)")
 (latex/'tools').mkdir(exist_ok=True)
 (latex/'tools/figures.py').write_text(plotting)
 copy(ROOT/'requirements-lock.txt',latex/'tools/requirements.txt')
 (latex/'README.md').write_text('''# Complete IEEE LaTeX project

Upload this directory to Overleaf and select report.tex as the main document, with pdfLaTeX. Alternatively run `tectonic report.tex` or `latexmk -pdf report.tex`. The requested template uses IEEEtran conference mode, standard margins and two columns. IEEEtran.cls and all external figures/tables are included. The main report preserves the Group-11 author block and inline tables from the uploaded source, with its supporting files in artifacts/.

Select baseline-results.tex or contribution-results.tex for the separate results documents. Those extracts use one column for wide tables. All figures are vector PDFs, with editable SVG and 400 dpi PNG exports. No figure downloads are needed during compilation.

Optional graph regeneration: install tools/requirements.txt in a Python environment and run `python tools/figures.py` from this directory. Its inputs are included in data/history/ and evidence/final-98-runs.csv. Graph values retain full CSV precision; report tables round for display. data/README.md defines units and scopes. The main report keeps the results in its Evaluation section.
''')
 zipdir(latex,OUT/'latex-source.zip')
 # The report-only source ZIP opens directly as an Overleaf project.
 report_source=BUILD/'report-latex-source';report_source.mkdir(exist_ok=True)
 for name in ['report.tex','README.md']:
  copy(latex/name,report_source/name)
 report_text=(latex/'report.tex').read_text()
 if (latex/'artifacts/IEEEtran.cls').exists():
  copy(latex/'artifacts/IEEEtran.cls',report_source/'artifacts/IEEEtran.cls')
 else:copy(latex/'IEEEtran.cls',report_source/'IEEEtran.cls')
 for name in re.findall(r'\\input\{([^}]+)\}',report_text):copy(latex/name,report_source/name)
 graphic_dirs=[Path('.')]
 for group in re.findall(r'\\graphicspath\{((?:\{[^{}]*\})+)\}',report_text):
  graphic_dirs += [Path(p) for p in re.findall(r'\{([^{}]*)\}',group)]
 for name in re.findall(r'\\includegraphics\[[^]]+\]\{([^}]+)\}',report_text):
  requested=Path(name)
  assert not requested.is_absolute() and '..' not in requested.parts
  relative=next(folder/requested for folder in graphic_dirs if (latex/folder/requested).is_file())
  for ext in ['pdf','svg','png']:
   asset=relative.with_suffix('.'+ext)
   if (latex/asset).exists():copy(latex/asset,report_source/asset)
 (report_source/'README.md').write_text('''# Report LaTeX source

Upload the contents of this ZIP to Overleaf. Select report.tex as the main file and use pdfLaTeX. Alternatively run `tectonic report.tex` or `latexmk -pdf report.tex`.

The main file includes all report text, both tables, equations, references and the TikZ baseline diagram. The Group-11 author block and artifacts/ layout follow the uploaded current source. The bundled IEEEtran.cls and all five results plots are in artifacts/; keep that directory next to report.tex. Figure and table captions define their abbreviations locally. The report uses the standard IEEE conference layout and is seven pages.

The complete data and plot-generation scripts are in the accompanying latex-source.zip and source-code.zip packages.
''')
 zipdir(report_source,OUT/'report-latex-source.zip')
 # Separate baseline and contribution packages.
 for kind in ['baseline','contribution']:
  dest=BUILD/(kind+'-results');dest.mkdir(exist_ok=True)
  copy(OUT/'pdf'/f'{kind}-results.pdf',dest/f'{kind}-results.pdf')
  for name in ['README.md','final-progression.csv','traffic-and-memory.csv']:
   if kind=='contribution' or name=='README.md':copy(tables/name,dest/'tables'/name)
  if kind=='baseline':
   for name in ['baseline-final.csv','original-paper-baseline.csv']:copy(tables/name,dest/'tables'/name)
  for p in (HERE/'latex/figures').iterdir():
   if (p.stem=='baseline-reproduction')==(kind=='baseline'):copy(p,dest/'figures'/p.name)
  for p in (HERE/'latex/tables').glob('*.tex'):
   if (p.stem in ['baseline','final-main','paper-final']) or kind=='contribution':copy(p,dest/'latex-tables'/p.name)
  zipdir(dest,OUT/f'{kind}-results.zip')
 # Canonical code, source runtimes, inputs and compact measured evidence.
 source=BUILD/'source-code';source.mkdir(exist_ok=True)
 allowed={'.py','.c','.h','.inc','.go','.md','.txt','.csv','.json','.sh','.S','.ld','.cmake'}
 roots=['adaptive','combined','slabs','paper-workloads','stacked','heavy-workloads','pinned','research','reference','runtime']
 inputs=list(ROOT.iterdir())
 for name in roots:inputs.extend((ROOT/name).rglob('*'))
 selected=[]
 for p in inputs:
  if not p.is_file() or p.is_symlink():continue
  rel=p.relative_to(ROOT)
  if any(part.startswith('.') for part in rel.parts):continue
  if any(x in rel.parts for x in ['results','validation','build','__pycache__','.venv','.git']):continue
  if p.suffix not in allowed and p.name not in ['go.mod','go.sum','Dockerfile','Makefile','CMakeLists.txt','AUTHORS_LICENSE']:continue
  if 'runtime' in rel.parts and 'benchmark' in rel.parts:continue
  selected.append(p)
 for p in selected:copy(p,source/'pim-reproduction'/p.relative_to(ROOT))
 for base in [ROOT,ROOT/'adaptive',ROOT/'combined',ROOT/'paper-workloads',ROOT/'paper-workloads/validation/interval-128',ROOT/'stacked',ROOT/'heavy-workloads',ROOT/'pinned',ROOT/'pinned/paper']:
  for name in ['checks.json','native-evidence.json']:
   p=base/'validation'/name if 'validation' not in base.parts else base/name
   if p.exists():copy(p,source/'pim-reproduction'/p.relative_to(ROOT))
 for r in final:
  p=REPO/r['raw_summary'];copy(p,source/p.relative_to(REPO))
 for base,tag in [(ROOT/'pinned','pinned-v1'),(ROOT/'pinned/paper','paper-v1')]:
  for p in (base/'results'/tag).glob('matrix-*.json'):copy(p,source/p.relative_to(REPO))
 for name in ['PIM-malloc/simulator/uPIMulator/golang_vm/uPIMulator/benchmark/microbenchmark/dpu/task.c','prim-benchmarks/BFS/data/loc-gowalla_edges.txt']:
  copy(REPO/name,source/name)
 for name in ['AUTHORS_LICENSE','README.md']:
  p=ROOT/name
  if p.exists():copy(p,source/'pim-reproduction'/name)
 copy(HERE/'README.md',source/'pim-reproduction/submission/README.md')
 for name in ['figures.py','build_report.py','package.py']:copy(HERE/name,source/'pim-reproduction/submission'/name)
 copy(HERE/'evidence/final-98-runs.csv',source/'pim-reproduction/submission/evidence/final-98-runs.csv')
 for name in FIG_INPUTS:copy(ROOT/name,source/'pim-reproduction'/name)
 # Include the repository entry points and verification workflow in the offline snapshot.
 for name in ['README.md','Makefile','PIM_SETUP.md','.gitignore','.gitattributes']:
  copy(REPO/name,source/name)
 for folder in ['docs','scripts']:
  for p in (REPO/folder).rglob('*'):
   if p.is_file() and '__pycache__' not in p.parts:copy(p,source/p.relative_to(REPO))
 manifest=[dict(path=str(p.relative_to(source)),sha256=sha(p),bytes=p.stat().st_size) for p in sorted(source.rglob('*')) if p.is_file()]
 (source/'SOURCE_MANIFEST.json').write_text(json.dumps(manifest,indent=2)+'\n')
 zipdir(source,OUT/'source-code.zip')
 # Upload bundle; do not include private authoring or validation records.
 bundle=BUILD/'submission';bundle.mkdir(exist_ok=True)
 for name in ['report-latex-source.zip','latex-source.zip','baseline-results.zip','contribution-results.zip','source-code.zip','presentation.pptx','speaker-notes.md']:
  copy(OUT/name,bundle/name)
 for p in (OUT/'pdf').glob('*.pdf'):copy(p,bundle/p.name)
 bundle_readme=(HERE/'README.md').read_text().replace('output/pdf/','').replace('output/','')
 bundle_readme=bundle_readme.replace('`../SNAPSHOT.md`','`pim-reproduction/SNAPSHOT.md` inside `source-code.zip`')
 (bundle/'README.md').write_text(bundle_readme)
 (bundle/'SOURCE_CODE.md').write_text('Source code: https://github.com/mahaswint/cs5290/tree/main/pim-reproduction\n\nOffline source: source-code.zip.\n')
 checksum=''.join(sha(p)+'  '+p.name+'\n' for p in sorted(bundle.iterdir()) if p.is_file())
 (bundle/'SHA256SUMS').write_text(checksum)
 zipdir(bundle,OUT/'submission.zip')
 # Explicit publication paths keep build products/caches out of Git.
 publication=[p.relative_to(REPO).as_posix() for p in selected if p.relative_to(ROOT).parts[0] in ['paper-workloads','stacked','heavy-workloads','pinned']]
 publication += ['.gitignore','pim-reproduction/submission/.gitignore','pim-reproduction/README.md','pim-reproduction/SNAPSHOT.md']
 for p in HERE.rglob('*'):
  if not p.is_file() or p.is_symlink():continue
  rel=p.relative_to(HERE)
  if any(part.startswith('.') for part in rel.parts):continue
  if rel.parts[0] in ['build','evidence']:continue
  if p.name in ['collect.py','compile.py']:continue
  if p.suffix in ['.pdf','.pptx','.zip','.tex','.cls','.svg','.png','.md','.py','.csv','.txt']:publication.append(p.relative_to(REPO).as_posix())
 publication += ['pim-reproduction/submission/evidence/final-98-runs.csv']
 (HERE/'build/publication-paths.json').write_text(json.dumps(sorted(set(publication)),indent=2)+'\n')

if __name__=='__main__':main()

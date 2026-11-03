#!/usr/bin/env python3
"""Publication-sized vector plots from frozen, audited measurement tables."""
import os,csv,json
from pathlib import Path
os.environ.setdefault('MPLCONFIGDIR',str(Path(__file__).parent/'build/mpl'))
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter

HERE=Path(__file__).resolve().parent;ROOT=HERE.parent
OUT=HERE/'latex/figures';OUT.mkdir(exist_ok=True)
COLORS=['#777777','#0072B2','#56B4E9','#009E73','#75B89E','#9B59A6','#D55E00']
VARIANTS=['baseline','indexed-adaptive64','indexed-fixed2','slab-adaptive64','slab-fixed2','slab-pinned-adaptive64','slab-pinned-fixed2']
SHORT=['B','IA','IF','SA','SF','PA','PF']
LABEL={'larson':'Larson 8–4095 B','cloud-cache':'CloudPhysics','larson-wide':'Larson 8–8191 B','graph-churn':'Graph churn','prim-sel':'PrIM SEL',
 'pim-list':'Linked blocks','pim-array':'Growing arrays','kv-growth':'KV replay','micro-32':'32 B','micro-256':'256 B','micro-4096':'4 KiB'}

def rows(path):return list(csv.DictReader(path.open()))
def f(row,key):return float(row[key])
FINAL=rows(HERE/'evidence/final-98-runs.csv')
CASES=list(dict.fromkeys((r['family'],r['workload'],int(r['threads'])) for r in FINAL))
def group(case):return {r['variant']:r for r in FINAL if (r['family'],r['workload'],int(r['threads']))==case}
def name(case):return LABEL[case[1]]+f' ({case[2]}T)'

plt.rcParams.update({'font.family':'serif','font.serif':['STIXGeneral'],'mathtext.fontset':'stix','font.size':8,
 'axes.labelsize':8,'axes.titlesize':9,'xtick.labelsize':7,'ytick.labelsize':7,'legend.fontsize':7,
 'axes.spines.top':False,'axes.spines.right':False,'axes.linewidth':.6,'lines.linewidth':1.2,
 'pdf.fonttype':42,'ps.fonttype':42,'savefig.facecolor':'white'})

def save(fig,stem):
 for ext in ['pdf','svg','png']:
  path=OUT/f'{stem}.{ext}'
  fig.savefig(path,dpi=400,bbox_inches='tight',pad_inches=.025)
  if ext=='svg':path.write_text('\n'.join(line.rstrip() for line in path.read_text().splitlines())+'\n')
 plt.close(fig)

def synthetic_reuse(stacked):
 # A separate attribution study: every bar has the same policy as its control.
 cases=[('burst',1,'Dense burst\n1T'),('burst',4,'Dense burst\n4T'),
        ('pool',4,'Persistent pool\n4T'),('sparse',1,'Sparse survivors\n1T')]
 fig,axs=plt.subplots(1,2,figsize=(7.16,2.65),sharey=True)
 x=np.arange(len(cases))
 for ax,key,title in zip(axs,['work_speedup','total_speedup'],['Work phase','Checked total']):
  for j,(policy,col) in enumerate([('adaptive',COLORS[5]),('fixed',COLORS[6])]):
   vals=[]
   for workload,threads,_ in cases:
    r=next(r for r in stacked if r['workload']==workload and int(r['threads'])==threads
           and int(r['size'])==4096 and r['policy']==policy and int(r['slab_kib'])==64)
    vals.append(f(r,key))
   bars=ax.bar(x+(j-.5)*.35,vals,.35,color=col,label=policy)
   for bar,val in zip(bars,vals):
    ax.text(bar.get_x()+bar.get_width()/2,val*1.08,f'{val:.2f}',ha='center',va='bottom',fontsize=6.5)
  ax.set_xticks(x,[c[2] for c in cases]);ax.set_title(title)
  ax.set_yscale('log',base=2);ax.set_ylim(1,90);ax.set_yticks([1,2,4,8,16,32,64])
  ax.yaxis.set_major_formatter(FuncFormatter(lambda v,p:f'{v:g}'))
  ax.axhline(1,color='black',lw=.6);ax.grid(axis='y',alpha=.2);ax.set_axisbelow(True)
 axs[0].set_ylabel('64 KiB slab speedup vs indexed direct (×)')
 axs[1].legend(frameon=False,loc='upper right')
 fig.tight_layout();save(fig,'synthetic-reuse')

def reuse_summary(stacked):
 # Compact main-paper view; the complete work/total plot remains available.
 fig,ax=plt.subplots(figsize=(3.5,2.2));x=np.arange(2)
 for j,(policy,col,label) in enumerate([('adaptive',COLORS[5],'Adaptive'),('fixed',COLORS[6],'Fixed-2')]):
  vals=[f(next(r for r in stacked if r['workload']==workload and r['threads']=='4'
        and r['size']=='4096' and r['slab_kib']=='64' and r['policy']==policy),'work_speedup')
        for workload in ['burst','pool']]
  bars=ax.bar(x+(j-.5)*.3,vals,.3,color=col,label=label)
  for bar,val in zip(bars,vals):
   ax.text(bar.get_x()+bar.get_width()/2,val+1,f'{val:.2f}×',ha='center',va='bottom',fontsize=7)
 ax.set_xticks(x,['Dense bursts','Persistent pool']);ax.set_ylim(0,68)
 ax.set_yticks([0,20,40,60]);ax.set_ylabel('Work speedup (×)')
 ax.legend(frameon=False,loc='upper left');ax.grid(axis='y',alpha=.2);ax.set_axisbelow(True)
 fig.tight_layout();save(fig,'reuse-summary')

def main():
 # Original paper-artifact reproduction, with its own aggregation and runtime.
 base=rows(ROOT/'analysis/figure15_comparison.csv')
 fig,axs=plt.subplots(1,2,figsize=(7.16,2.55),sharey=True)
 for ax,t in zip(axs,[1,16]):
  selected=[next(r for r in base if int(r['threads'])==t and int(r['size_bytes'])==s and r['allocator']==a) for s in [32,256,4096] for a in ['buddy','sw']]
  x=np.arange(6)
  ax.bar(x-.18,[f(r,'paper_latency_us') for r in selected],.36,color='#B8B8B8',label='Paper label')
  ax.bar(x+.18,[f(r,'measured_latency_us') for r in selected],.36,color=COLORS[1],label='Reproduced')
  ax.set_xticks(x,['32 B\nBuddy','32 B\nSW','256 B\nBuddy','256 B\nSW','4 KiB\nBuddy','4 KiB\nSW'])
  ax.set_yscale('log');ax.set_ylim(3,25000);ax.set_title(f'{t} tasklet'+('s' if t>1 else ''))
  ax.grid(axis='y',alpha=.2);ax.set_axisbelow(True)
 axs[0].set_ylabel('Batch time / 128 (µs)');fig.legend(*axs[1].get_legend_handles_labels(),frameon=False,loc='upper center',bbox_to_anchor=(.5,1.09),ncol=2)
 fig.tight_layout();save(fig,'baseline-reproduction')
 # Lists and owner lookup, fixed retention throughout.
 a=rows(ROOT/'paper-workloads/analysis/all-runs.csv')
 fig,ax=plt.subplots(figsize=(3.5,2.30));x=np.arange(4)
 for j,w in enumerate(['larson','faimgraph']):
  selected={r['mode']:r for r in a if r['preset']=='ablation' and r['workload']==w}
  vals=[f(selected['scan'],'work_ms')/f(selected[m],'work_ms') for m in ['scan','lists','lookup','indexed']]
  ax.bar(x+(j-.5)*.36,vals,.36,label='Larson 8–999 B' if w=='larson' else 'Graph churn',color=[COLORS[1],COLORS[6]][j])
  ax.text(x[-1]+(j-.5)*.36,vals[-1]*1.08,f'{vals[-1]:.2f}×',ha='center',va='bottom',fontsize=7)
 ax.set_xticks(x,['Scan','Lists','Lookup','Both']);ax.set_ylabel('Work speedup (×)');ax.set_yscale('log',base=2)
 ax.set_yticks([1,2,4,8,16,32]);ax.yaxis.set_major_formatter(FuncFormatter(lambda v,p:f'{v:g}'))
 ax.axhline(1,color='black',lw=.6);ax.legend(frameon=False);fig.tight_layout();save(fig,'index-ablation')
 # All 14 interval pairs: signs and regressions preserved.
 interval=rows(ROOT/'paper-workloads/analysis/interval128-comparisons.csv')
 fig,ax=plt.subplots(figsize=(7.16,2.6));x=np.arange(len(interval))
 vals=[f(r,'work_runtime_reduction_percent') for r in interval]
 ax.bar(x,vals,color=[COLORS[3] if v>=0 else COLORS[6] for v in vals])
 ln={'larson':'Larson','pim-list':'List','pim-array':'Array','faimgraph':'Churn','prim-sel':'SEL'}
 ax.set_xticks(x,[ln[r['workload']]+f"\n{r['threads']}T/{r['seed']}" for r in interval]);ax.axhline(0,color='black',lw=.6)
 ax.set_ylabel('Work-time reduction, 64 → 128 (%)');ax.grid(axis='y',alpha=.2);ax.set_axisbelow(True)
 fig.tight_layout();save(fig,'interval-sweep')
 # Standalone slab exploration spans 1,2,4,8,16 tasklets.
 slab=rows(ROOT/'slabs/analysis/slabs-full-v1/comparison.csv')
 fig,ax=plt.subplots(figsize=(3.5,2.35))
 for k,col in zip([0,16,32,64],[COLORS[0],COLORS[1],COLORS[3],COLORS[6]]):
  selected=sorted([r for r in slab if r['workload']=='burst' and int(r['slab_kib'])==k],key=lambda r:int(r['threads']))
  ax.plot([int(r['threads']) for r in selected],[f(r,'total_us')/1000 for r in selected],marker='o',ms=3,color=col,label='Direct' if k==0 else f'{k} KiB')
 ax.set_xticks([1,2,4,8,16]);ax.set_yscale('log');ax.set_xlabel('Tasklets');ax.set_ylabel('Harness total (ms)');ax.legend(frameon=False,ncol=2)
 ax.grid(axis='y',alpha=.2);fig.tight_layout();save(fig,'standalone-slabs')
 # Combined slab geometry and sparse reservation cost, equal budgets.
 stacked=rows(ROOT/'stacked/analysis/comparisons.csv')
 synthetic_reuse(stacked)
 reuse_summary(stacked)
 fig,axs=plt.subplots(1,2,figsize=(7.16,2.45))
 for policy,col in [('fixed',COLORS[1]),('adaptive',COLORS[6])]:
  sel=sorted([r for r in stacked if r['workload']=='burst' and r['threads']=='4' and r['size']=='4096' and r['policy']==policy],key=lambda r:int(r['slab_kib']))
  axs[0].plot([int(r['slab_kib']) for r in sel],[f(r,'work_ms') for r in sel],marker='o',ms=3,label=policy,color=col)
 axs[0].set_xticks([0,16,32,64]);axs[0].set_yscale('log');axs[0].set_xlabel('Slab size (KiB), 0 = direct');axs[0].set_ylabel('Dense work (ms), 4T');axs[0].legend(frameon=False)
 axs[1].bar([0,1],[8,128],color=[COLORS[0],COLORS[6]],width=.55)
 axs[1].set_xticks([0,1],['Two direct objects','Two slab survivors']);axs[1].set_ylabel('Reservation after trim (KiB)');axs[1].set_ylim(0,150)
 for x,y in enumerate([8,128]):axs[1].text(x,y+3,str(y),ha='center')
 for ax in axs:ax.grid(axis='y',alpha=.2);ax.set_axisbelow(True)
 fig.tight_layout();save(fig,'slab-tradeoff')
 # Complete final parent progression, all stages.
 cases=[c for c in CASES if c[0]=='main-stack'];fig,ax=plt.subplots(figsize=(7.16,2.7));x=np.arange(len(cases));width=.105
 for j,v in enumerate(VARIANTS):
  vals=[f(group(c)['baseline'],'work_ms')/f(group(c)[v],'work_ms') for c in cases]
  ax.bar(x+(j-3)*width,vals,width,color=COLORS[j],label=SHORT[j])
 ax.set_xticks(x,['Larson\n8–4095 B','CloudPhysics','Larson\n8–8191 B','Graph churn','PrIM SEL']);ax.set_ylabel('Work speedup vs scan fixed-2 (×)')
 ax.set_yscale('log',base=2);ax.set_yticks([.5,1,2,4,8,16]);ax.yaxis.set_major_formatter(FuncFormatter(lambda v,p:f'{v:g}'))
 ax.set_ylim(.65,14);ax.axhline(1,color='black',lw=.6);ax.legend(ncol=7,frameon=False,loc='upper center',bbox_to_anchor=(.5,1.19))
 ax.grid(axis='y',alpha=.2);ax.set_axisbelow(True);fig.tight_layout();save(fig,'final-progression')
 # Final paper cases: both full policies, every case, unchanged object sizes.
 cases=[c for c in CASES if c[0]=='paper-cases'];fig,ax=plt.subplots(figsize=(7.16,2.8));x=np.arange(len(cases))
 for j,v in enumerate(['slab-pinned-adaptive64','slab-pinned-fixed2']):
  vals=[f(group(c)['baseline'],'work_ms')/f(group(c)[v],'work_ms') for c in cases]
  bars=ax.bar(x+(j-.5)*.36,vals,.36,color=[COLORS[5],COLORS[6]][j],label=['Full adaptive-64','Full fixed-2'][j])
  for bar,val in zip(bars,vals):
   if val>4:ax.text(bar.get_x()+bar.get_width()/2,val*1.07,f'{val:.2f}',ha='center',va='bottom',fontsize=6.5)
 ax.set_xticks(x,[LABEL[c[1]]+f'\n{c[2]}T' for c in cases]);ax.set_ylabel('Work speedup vs scan fixed-2 (×)');ax.set_yscale('log',base=2)
 ax.set_yticks([.5,1,2,4,8,16,32]);ax.yaxis.set_major_formatter(FuncFormatter(lambda v,p:f'{v:g}'));ax.set_ylim(.6,32)
 ax.axhline(1,color='black',lw=.6);ax.legend(frameon=False,loc='upper left');ax.grid(axis='y',alpha=.2);ax.set_axisbelow(True)
 fig.tight_layout();save(fig,'paper-workloads')
 # Isolate pinning after holding policy/slab geometry fixed.
 cases=[c for c in CASES if c[1] in ['larson','larson-wide','cloud-cache','graph-churn','pim-list','pim-array','micro-4096']]
 fig,axs=plt.subplots(1,2,figsize=(7.16,3.1),sharey=True);y=np.arange(len(cases))
 for j,(pin,slab,col) in enumerate([('slab-pinned-adaptive64','slab-adaptive64',COLORS[5]),('slab-pinned-fixed2','slab-fixed2',COLORS[6])]):
  vals=[f(group(c)[slab],'work_ms')/f(group(c)[pin],'work_ms') for c in cases]
  byt=[100*(1-f(group(c)[pin],'dram_bytes')/f(group(c)[slab],'dram_bytes')) for c in cases]
  axs[0].barh(y+(j-.5)*.32,vals,.32,color=col,label=['adaptive','fixed'][j]);axs[1].barh(y+(j-.5)*.32,byt,.32,color=col)
 axs[0].set_yticks(y,[name(c) for c in cases]);axs[0].invert_yaxis();axs[0].set_xlim(.9,1.4);axs[0].axvline(1,color='black',lw=.6)
 axs[0].set_xlabel('Pinning work speedup (×)');axs[1].set_xlabel('Full-execution DRAM-byte reduction (%)');fig.legend(*axs[0].get_legend_handles_labels(),frameon=False,loc='upper center',bbox_to_anchor=(.5,1.065),ncol=2)
 for ax in axs:ax.grid(axis='x',alpha=.2);ax.set_axisbelow(True)
 fig.tight_layout();save(fig,'pinning-isolation')
 # Work versus checked lifecycle, all 14 final cases.
 fig,axs=plt.subplots(1,2,figsize=(7.16,2.8))
 for ax,family,title in zip(axs,['main-stack','paper-cases'],['Mixed workloads','Paper-derived settings']):
  cases=[c for c in CASES if c[0]==family];y=np.arange(len(cases))
  for j,key in enumerate(['work_ms','total_ms']):
   vals=[f(group(c)['baseline'],key)/f(group(c)['slab-pinned-fixed2'],key) for c in cases]
   ax.barh(y+(j-.5)*.34,vals,.34,color=[COLORS[6],COLORS[1]][j],label=['Work','Checked total'][j])
  ax.set_yticks(y,[name(c) for c in cases]);ax.invert_yaxis();ax.set_xscale('log',base=2)
  ax.set_xticks([1,2,4,8,16,32]);ax.xaxis.set_major_formatter(FuncFormatter(lambda v,p:f'{v:g}'))
  ax.set_xlim(.65,32);ax.set_title(title);ax.set_xlabel('Full fixed-stack speedup vs scan fixed-2 (×)')
  ax.axvline(1,color='black',lw=.6);ax.grid(axis='x',alpha=.2);ax.set_axisbelow(True)
 axs[0].legend(frameon=False,loc='lower right',ncol=1)
 fig.tight_layout();save(fig,'work-versus-total')
 # Machine-readable values drive both editable slide charts and tables.
 slide_data=dict(final=FINAL,cases=[list(c) for c in CASES],baseline=base,interval=interval,stacked=stacked,
  ablation=[r for r in a if r['preset']=='ablation'])
 (HERE/'build/deck-data.json').write_text(json.dumps(slide_data,indent=2)+'\n')
 print('Exported eleven publication-sized figures as PDF/SVG/400 dpi PNG.')

if __name__=='__main__':main()

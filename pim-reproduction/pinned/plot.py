#!/usr/bin/env python3
"""Render scientific comparison figures from the audited CSV artifacts."""
import csv
import os
from pathlib import Path

HERE=Path(__file__).resolve().parent
os.environ.setdefault('MPLCONFIGDIR',str(HERE/'.mplconfig'))
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.colors import LogNorm
import numpy as np


def main():
    folder=HERE/'analysis'
    rows=list(csv.DictReader((folder/'all-runs.csv').open()))
    pairs=list(csv.DictReader((folder/'incremental-pinning.csv').open()))
    workloads=['larson','cloud-cache','larson-wide','graph-churn','prim-sel']
    labels=['Larson 8–4095 B','CloudPhysics write cache','Larson 8–8191 B','Gowalla graph churn','PrIM SEL control']
    variants=['indexed-adaptive64','indexed-fixed2','slab-adaptive64','slab-fixed2','slab-pinned-adaptive64','slab-pinned-fixed2']
    names=['Indexed\nadaptive','Indexed\nfixed-2','+ slab\nadaptive','+ slab\nfixed-2','+ slab + pin\nadaptive','+ slab + pin\nfixed-2']
    values=[]
    for w in workloads:
        group={r['variant']:r for r in rows if r['preset']=='main' and r['workload']==w}
        base=float(group['baseline']['work_ms'])
        values.append([base/float(group[v]['work_ms']) for v in variants])
    values=np.asarray(values)
    fig,ax=plt.subplots(figsize=(11.7,5.1))
    norm=LogNorm(vmin=min(.95,float(values.min())),vmax=max(2,float(values.max())))
    im=ax.imshow(values,norm=norm,cmap='Blues',aspect='auto')
    ax.set_xticks(range(6),names,fontsize=10);ax.set_yticks(range(5),labels,fontsize=10)
    for y in range(5):
        for x in range(6):
            ax.text(x,y,f'{values[y,x]:.2f}×',ha='center',va='center',fontsize=11,
                    color='white' if norm(values[y,x])>.65 else '#17212f')
    for x in [1.5,3.5]:ax.axvline(x,color='white',linewidth=3)
    ax.set_title('Full-stack work speedup relative to scan fixed-2',pad=14,fontsize=13)
    bar=fig.colorbar(im,ax=ax,pad=.025,shrink=.8);bar.set_label('Speedup (log color scale)')
    fig.text(.53,.015,'4 tasklets · 64 KiB slabs · adaptive interval 64 alloc/free operations · equal 256 KiB retention budgets',ha='center',fontsize=9)
    fig.tight_layout(rect=(0,.05,1,1));fig.savefig(folder/'full-stack.png',dpi=180);fig.savefig(folder/'full-stack.svg');plt.close(fig)

    fig,ax=plt.subplots(figsize=(10.5,5.3));y=np.arange(5)
    maximum=1.0;minimum=1.0
    for i,(policy,color) in enumerate([('fixed','#2563a6'),('adaptive','#d48228')]):
        ratios=[float(next(p for p in pairs if (p['preset'],p['workload'],p['policy'])==('main',w,policy))['incremental_work_speedup']) for w in workloads]
        pos=y+(i-.5)*.30;maximum=max(maximum,max(ratios));minimum=min(minimum,min(ratios))
        ax.scatter(ratios,pos,color=color,label='Fixed-2' if policy=='fixed' else 'Adaptive, interval 64',s=46)
        for v,p in zip(ratios,pos):ax.annotate(f'{v:.3f}×',(v,p),xytext=(7,0),textcoords='offset points',va='center',fontsize=10,color=color)
    ax.axvline(1,color='#333333',linestyle='--',linewidth=1)
    ax.set_yticks(y,labels);ax.set_ylim(4.6,-.6)
    left=min(.97,minimum-.03);ax.set_xlim(left,maximum+(maximum-left)*.28+.03)
    ax.grid(axis='x',alpha=.2);ax.set_xlabel('Slab runtime / slab + pinned runtime (higher is faster)')
    ax.set_title('Incremental gain from pinning upper buddy metadata',pad=15,fontsize=13)
    ax.spines[['top','right']].set_visible(False);ax.legend(loc='lower right',frameon=False)
    fig.text(.53,.015,'Same input, policy and 64 KiB slab geometry · 528 B shared metadata prefix · 4 tasklets',ha='center',fontsize=9)
    fig.tight_layout(rect=(0,.05,1,1));fig.savefig(folder/'incremental-pinning.png',dpi=180);fig.savefig(folder/'incremental-pinning.svg');plt.close(fig)
    print('Saved full-stack and incremental-pinning PNG/SVG figures')


if __name__=='__main__':main()

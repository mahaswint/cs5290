#!/usr/bin/env python3
"""Export scientific figures from the complete audited CSV, without filtering."""
import csv
import os
from pathlib import Path
os.environ.setdefault('MPLCONFIGDIR',str(Path(__file__).resolve().parent/'.mplconfig'))
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import run

HERE=run.HERE
VARIANTS=list(run.VARIANTS)
LABELS=['Baseline','Indexed A64','Indexed F2','+ slab A64','+ slab F2','+ slab + pin A64','+ slab + pin F2']
COLORS=['#8b929b','#2e669e','#8ab5db','#33826c','#91c3ae','#8c4b9f','#e18b36']


def main():
    rows=list(csv.DictReader((HERE/'analysis/comparisons.csv').open()))
    cases=list(dict.fromkeys((r['workload'],int(r['threads'])) for r in rows))
    data={(r['workload'],int(r['threads']),r['variant']):float(r['work_speedup']) for r in rows}
    fig,axes=plt.subplots(2,1,figsize=(14,8),gridspec_kw={'height_ratios':[1,1.2]})
    fig.suptitle('PIM-malloc paper workload shapes · all seven allocator stages',fontsize=17,y=.99)
    for ax,selection in zip(axes,[[c for c in cases if not c[0].startswith('micro-')],
                               [c for c in cases if c[0].startswith('micro-')]]):
        x=np.arange(len(selection));width=.105
        for j,(v,label,color) in enumerate(zip(VARIANTS,LABELS,COLORS)):
            vals=[data[w,t,v] for w,t in selection]
            ax.bar(x+(j-3)*width,vals,width=width,label=label,color=color,edgecolor='white',linewidth=.4,zorder=3)
        def name(c):
            w,t=c
            names={'pim-list':'Gowalla linked blocks','pim-array':'Gowalla growing arrays',
                'kv-growth':'KV growth replay\n(allocator only)','micro-32':'32 B','micro-256':'256 B','micro-4096':'4 KiB'}
            return names[w]+f'\n{t} tasklet'+('s' if t!=1 else '')
        ax.set_xticks(x,[name(c) for c in selection],fontsize=10)
        ax.set_ylabel('Work speedup vs scan fixed-2')
        ax.axhline(1,color='#333333',lw=1,zorder=2)
        ax.set_yscale('log',base=2)
        upper=max(2,max(data[w,t,v] for w,t in selection for v in VARIANTS)*1.35)
        ax.set_yticks([v for v in [.5,1,2,4,8,16,32,64] if v<=upper])
        ax.set_ylim(.5,upper)
        ax.yaxis.set_major_formatter(matplotlib.ticker.FormatStrFormatter('%g×'))
        ax.grid(axis='y',alpha=.2,zorder=0)
        ax.spines[['top','right']].set_visible(False)
    axes[0].set_title('Scaled graph insertion ports and separately labelled KV allocation replay',fontsize=12,pad=12)
    axes[1].set_title('Paper microbenchmarks: 128 requests per tasklet, unchanged sizes and tasklet counts',fontsize=12,pad=12)
    handles,labels=axes[0].get_legend_handles_labels()
    fig.legend(handles,labels,ncol=4,loc='upper center',bbox_to_anchor=(.5,.925),frameon=False,fontsize=10)
    fig.text(.02,.015,'350 MHz single-DPU simulation · 32 MiB payload heap · higher is faster; below 1 is a regression\n'
        'Maintained scan fixed-2 baseline; not the paper’s straw-man/static baseline. KV replay excludes attention and serving.',fontsize=10,color='#444444')
    fig.subplots_adjust(top=.80,bottom=.13,hspace=.62,left=.09,right=.99)
    for ext in ['png','svg']:fig.savefig(HERE/'analysis'/('paper-full-stack.'+ext),dpi=180,facecolor='white')
    plt.close(fig)


if __name__=='__main__':main()

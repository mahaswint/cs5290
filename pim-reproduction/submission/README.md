# CS5290 project submission

The report introduces DPUs and the PIM-malloc baseline, explains our allocator changes, describes the setup, and presents the results in a separate Evaluation section.

| Required item | File |
| --- | --- |
| Report PDF | `output/pdf/report.pdf` |
| Complete LaTeX project | `output/report-latex-source.zip` (report only), `output/latex-source.zip` (all result sources) |
| Presentation | `output/presentation.pptx` |
| Baseline results | `output/pdf/baseline-results.pdf` and `output/baseline-results.zip` |
| Contribution results | `output/pdf/contribution-results.pdf` and `output/contribution-results.zip` |
| Source code | [GitHub source branch](https://github.com/mahaswint/cs5290/tree/main/pim-reproduction), also `output/source-code.zip` |

`output/submission.zip` collects all six items. `output/speaker-notes.md` provides the presentation script, source references and timing. Present slides 1-20 in 16 minutes; slides 21-24 are backup material. Tables and charts in the PowerPoint remain editable.

The seven-page report uses IEEEtran, the standard two-column layout and the author label **CS5290 Project Group**. Four results plots cover indexing, synthetic reuse, the final full-stack comparison and the original paper settings. Two short tables summarize software-baseline reproduction and selected memory costs. The plots sit beside their discussion rather than collecting at the end. Full intermediate plots and numerical tables remain in the result packages. The result extracts use one column for wide tables. Figures are supplied as vector PDF/SVG and 400 dpi PNG.

The report separates the original paper reproduction from the corrected controls with a 64 KiB WRAM limit. The final results cover fourteen workload settings and all seven requested stages. Work runtime, checked lifecycle runtime and full-execution memory traffic have distinct definitions. Reduced graph ports, configured Larson sizes and the allocation-only KV replay are identified explicitly in the paper.

For LaTeX compilation, upload the contents of `latex-source.zip` to Overleaf and choose `report.tex` as the main file with pdfLaTeX, or run Tectonic locally. The ZIP includes IEEEtran.cls, all figures and tables, plotting code and its input CSV files. No network access is needed for the figures during compilation; normal TeX packages must be available.

For implementation dependencies and experiment commands, see `../SNAPSHOT.md` and the individual stage READMEs. Full simulator builds, toolchain caches and individual binary/log directories remain outside the submission archives. The source archive includes the simulator source, public input data and selected measured summary evidence.

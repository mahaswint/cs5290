PYTHON ?= python3
PLOT_PYTHON ?= pim-reproduction/.venv/bin/python

.PHONY: verify test analyze figures package

verify:
	$(PYTHON) scripts/verify_submission.py

test:
	$(PYTHON) pim-reproduction/pinned/test.py
	$(PYTHON) pim-reproduction/pinned/paper/test.py

analyze:
	$(PYTHON) pim-reproduction/pinned/analyze.py
	$(PYTHON) pim-reproduction/pinned/paper/analyze.py --tag paper-v1

figures:
	$(PLOT_PYTHON) pim-reproduction/pinned/plot.py
	$(PLOT_PYTHON) pim-reproduction/pinned/paper/plot.py

package:
	$(PYTHON) pim-reproduction/submission/package.py

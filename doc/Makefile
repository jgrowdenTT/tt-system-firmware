# Minimal makefile for Sphinx documentation
#

# You can set these variables from the command line, and also
# from the environment for the first two.
SPHINXOPTS    ?= -W --conf-dir .
SPHINXBUILD   ?= sphinx-build
SPHINXWATCH   ?= sphinx-autobuild
SOURCEDIR     = .
BUILDDIR      = _build_sphinx

# Put it first so that "make" without argument is like "make help".
help:
	@$(SPHINXBUILD) -M help "$(SOURCEDIR)" "$(BUILDDIR)" $(SPHINXOPTS) $(O)

# Development (watch): build the documentation and watch for changes.
# Served on port 3000 (http://localhost:3000)
watch:
	@$(SPHINXWATCH) "$(SOURCEDIR)" "$(BUILDDIR)" $(SPHINXOPTS) $(O) --port 3000 --open-browser

.PHONY: help doxygen board-configs ci-status test-platforms zephyr-tag Makefile

# Zephyr checkout used by twister to work out which platforms each test runs on.
ZEPHYR_BASE ?= $(abspath $(CURDIR)/../../zephyr)

# Generate auto-generated board configuration documentation
board-configs:
	@echo "Generating board configuration documentation..."
	@cd .. && python scripts/generate_board_docs_rst.py

# Last passing run on main for each test (never fails the build; see fetch_ci_status.py)
ci-status:
	@mkdir -p _generated
	@python3 ../scripts/fetch_ci_status.py --output _generated/ci_status.json

# Generate the per-test-group platform list (_generated/ is gitignored)
test-platforms: ci-status
	@python3 ../scripts/gen_test_platforms.py --zephyr-base "$(ZEPHYR_BASE)" \
		--output _generated/test_platforms.dox --ci-status _generated/ci_status.json

# Fetch Zephyr's published doxygen tag file so names such as zassert_equal link to the
# Zephyr docs. Fetched once; delete _generated/zephyr.tag to refresh it.
zephyr-tag:
	@mkdir -p _generated
	@test -s _generated/zephyr.tag || \
		(curl -fsSL -o _generated/zephyr.tag.part \
			https://docs.zephyrproject.org/latest/doxygen/html/zephyr.tag && \
		 mv _generated/zephyr.tag.part _generated/zephyr.tag) || \
		(rm -f _generated/zephyr.tag.part; \
		 echo "zephyr-tag: could not download zephyr.tag" >&2; exit 1)

doxygen: test-platforms zephyr-tag
	doxygen Doxyfile
	@mkdir -p "$(BUILDDIR)/html/doxygen"
	cp -r _build_doxygen/html/* "$(BUILDDIR)/html/doxygen/"

# Catch-all target: route all unknown targets to Sphinx using the new
# "make mode" option.  $(O) is meant as a shortcut for $(SPHINXOPTS).
%: Makefile board-configs
	@$(SPHINXBUILD) -M $@ "$(SOURCEDIR)" "$(BUILDDIR)" $(SPHINXOPTS) $(O)

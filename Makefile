EXTENSION    = pg_relusage
EXTVERSION   = $(shell grep default_version $(EXTENSION).control | sed -e "s/default_version[[:space:]]*=[[:space:]]*'\([^']*\)'/\1/")
MODULES      = pg_relusage
PGFILEDESC   = "pg_relusage - log relations used by SQL statements"
DATA         = $(wildcard $(EXTENSION)--*.sql)
TESTS        = $(wildcard sql/*.sql)
REGRESS      = $(patsubst sql/%.sql,%,$(TESTS))
PG_CONFIG   ?= pg_config

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# PostgreSQL 10 does not prune partitions at executor startup
ifeq ($(MAJORVERSION),10)
REGRESS := $(filter-out runtime_pruning,$(REGRESS))
endif

# Defined after including PGXS, so that "all" from PGXS remains the default target

# Source archive for PGXN, built from the committed HEAD
release-zip:
	git archive --format zip --prefix=$(EXTENSION)-$(EXTVERSION)/ --output $(EXTENSION)-$(EXTVERSION).zip HEAD

.PHONY: release-zip

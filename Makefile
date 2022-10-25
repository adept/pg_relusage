EXTENSION    = pg_relusage
EXTVERSION   = $(shell grep default_version $(EXTENSION).control | sed -e "s/default_version[[:space:]]*=[[:space:]]*'\([^']*\)'/\1/")
TESTS        = $(wildcard sql/*.sql)
REGRESS      = $(patsubst sql/%.sql,%,$(TESTS))
# REGRESS_OPTS = --inputdir=test
MODULES      = pg_relusage
PG_CONFIG    ?= pg_config

all:

release-zip: all
	git archive --format zip --prefix=pg_relusage-$(EXTVERSION)/ --output ./pg_relusage-$(EXTVERSION).zip HEAD
	unzip ./pg_relusage-$(EXTVERSION).zip
	rm ./pg_relusage-$(EXTVERSION).zip
	sed -i -e "s/__VERSION__/$(EXTVERSION)/g"  ./pg_relusage-$(EXTVERSION)/META.json
	zip -r ./pg_relusage-$(EXTVERSION).zip ./pg_relusage-$(EXTVERSION)/
	rm ./pg_relusage-$(EXTVERSION) -rf


DATA = $(wildcard *--*.sql)
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

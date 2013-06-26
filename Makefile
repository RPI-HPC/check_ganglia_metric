CC ?= cc

ALL_CFLAGS := $(CFLAGS) $(shell xml2-config --cflags)
ALL_LDFLAGS := $(LDFLAGS) -lrt $(shell xml2-config --libs)

all:
	$(CC) $(ALL_CFLAGS) -O2 -Wall check_ganglia_metric.c -o check_ganglia_metric $(ALL_LDFLAGS)

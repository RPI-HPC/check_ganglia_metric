all:
	gcc -O2 -Wall check_ganglia_metric.c `xml2-config --cflags` -o check_ganglia_metric `xml2-config --libs` -lrt

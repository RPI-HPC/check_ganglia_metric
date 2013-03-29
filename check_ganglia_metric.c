#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <math.h>
#include <sys/time.h>
#include <getopt.h>

#define MAX_RETRY 4
#define CHUNK 1048576
#define MINI_CHUNK 65536

struct {
        int max_age;
        char metric[256];
        char host[256];
        char gmetad_host[256];
        int gmetad_port;
        char cachepath[4096];
        char cachename[256];

	char short_name; // bool

	char warning[64];
	char critical[64];

	int heartbeat;

	int debug;
	int dump;
} config;

static void debug(const char *fmt, ...);

char *get_shortname(const char *longname) {
	char *shortname = malloc(strlen(longname) + 1);

	strcpy(shortname, longname);
	strtok(shortname, ".");

	return shortname;
}

/*
 * Create global cache file
 */

int create_cachefile(char *cachefile)
{
	int ret;
	ret = creat(cachefile, S_IRUSR | S_IWUSR);
	if (ret < 0) {
		return -1;
	} else {
		close(ret);
		return config.max_age;
	}
}

/*
 * Check global cache file age
 */

int check_cache_age(char *cachefile)
{
        struct stat f;
        int ret;

        ret = stat(cachefile, &f);
        if (ret < 0) {
		if (errno == ENOENT) {
			return create_cachefile(cachefile);
		} else {
			debug("DEBUG: Unable to stat cache file.\n");
	                return -1;
		}
	}

        return time(NULL) - f.st_mtime;
}

/*
 * Connect to gmetad
 */

int gmetad_connect(char *host, int port)
{
	int sockfd;
	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0)
		return -1;

	struct hostent *gmetad;
	struct sockaddr_in addr;
	gmetad = gethostbyname(host);

	if (gmetad == NULL)
		return -2;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	memcpy(&addr.sin_addr.s_addr, gmetad->h_addr, gmetad->h_length);
	addr.sin_port = htons(port);

	if (connect(sockfd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		// TODO: make errors more helpful
		printf("Connection error: %d\n", errno);
		return -3;
	}

	debug("Connected\n");
	
	return sockfd;
}

/*
 * Fetch XML from gmetad
 */

int fetch_xml(char *host, int port, char **dest)
{
	int sockfd;
	sockfd = gmetad_connect(host, port);
	if (sockfd < 0) {
		return sockfd;
	}

	int ret, offset = 0;
	int buffer_size = CHUNK;

	debug("%d kB chunk size\n", buffer_size / 1024);

	char *buffer = malloc(buffer_size);
	char *buffer2 = NULL;

	debug("Fetching...\n");

	ret = recv(sockfd, buffer, MINI_CHUNK, 0);

	while (ret > 0) {
		offset += ret;

		if (offset + MINI_CHUNK > buffer_size) {
			buffer_size += CHUNK;
			buffer2 = (char *) realloc(buffer, buffer_size);

			debug("Grow buffer to %d kB\n", buffer_size / 1024);

			if (buffer2 == NULL) {
				return -4;
			} else {
				buffer = buffer2;
			}
		}

		ret = recv(sockfd, buffer + offset, MINI_CHUNK, 0);
	}

	if (ret < 0) {
		printf("Error receiving %d\n", errno);
		return -4;
	}

	offset += ret;

	debug("Receive used approx %d kB of %d kB allocated.\n", offset / 1024, buffer_size / 1024);

	*dest = buffer;

	close(sockfd);

	return offset;
}

/*
 * Ensure a path exists, create if it does not
 */

int ensure_path(const char *path)
{
	struct stat f;
	int ret = stat(path, &f);
	if (ret < 0) {
		if (errno == ENOENT) {
			ret = mkdir(path, S_IRWXU | S_IXOTH | S_IXGRP | S_IROTH | S_IRGRP);
			if (ret < 0) {
				return -1;
			}
		} else {
			return -1;
		}
	}

	return 0;
}

/*
 * Lock the global cache file
 */

int get_cache_lock(char *cachefile, int *cachefd)
{
	int ret;

	if (*cachefd < 0) {
	        *cachefd = open(cachefile, O_RDWR);
        	if (*cachefd < 0) {
                	printf("Unable to get cache FD\n");
	                return -1;
	        }
	}

        struct flock l;
        l.l_type = F_WRLCK;
        l.l_whence = SEEK_SET;
        l.l_start = 0;
        l.l_len = 0;

	ret = fcntl(*cachefd, F_SETLK, &l);
        if (ret < 0) {
        	return -1;
        }

	return 0;
}

/*
 * Release the lock on the global cache file
 */

int release_cache_lock (char *cachefile, int *cachefd)
{
	int ret;

        // touch global cache
        ret = utimes(cachefile, NULL);
        if (ret < 0) {
                // TODO: probably not fatal ?
        }

	struct flock l;
        l.l_type = F_UNLCK;
        l.l_whence = SEEK_SET;
        l.l_start = 0;
        l.l_len = 0;

        ret = fcntl(*cachefd, F_SETLK, &l);
        if (ret < 0) {
                printf("Failed to remove lock\n");
		// TODO: will this be fatal ?
        }

        close(*cachefd);
	*cachefd = -1;

	return 0;
}

/*
 * Parse the XML out to per-host cache files
 */

int parse_xml_to_cache(char *xml, int xlen, char *cachepath, char *cachefile)
{
	int retc = 0;

	int ret;

	xmlDoc *doc = NULL;
	xmlNode *root = NULL;

	// suggested ABI check
	LIBXML_TEST_VERSION

	doc = xmlReadMemory(xml, xlen, "xml", NULL, 0);

	if (doc == NULL) {
		xmlError *pErr = xmlGetLastError();
		if (pErr == NULL) {
			printf("panic!\n");
			exit(100);
		}

		printf("Error parsing #%d (%d,%d)\n", pErr->code, pErr->line,pErr->int2);

		retc = -1;
		goto cleanup;
	}

	root = xmlDocGetRootElement(doc);

	xmlNode *node, *node2, *node3, *node4 = NULL;
	if (strcmp((char *) root->name, "GANGLIA_XML") != 0) {
		retc = -1;
		goto cleanup;
	}

	char *name, *units, *value, *grid, *cluster, *host;

	FILE *f;

	char filenamebuf[4096];

	int count;

	for (node = root->children; node; node = node->next) {
		if (node->type == XML_ELEMENT_NODE && strcmp((const char *) node->name, "GRID") == 0) {
			grid = (char *) xmlGetProp(node, (const xmlChar *) "NAME");
           		debug("Found new grid: %s\n", grid);

			sprintf(filenamebuf, "%s/%s", cachepath, grid);
			ret = ensure_path(filenamebuf);
			if (ret < 0) {
				retc = -1;
				goto cleanup;
			}

			for (node2 = node->children; node2; node2 = node2->next) {
				if (node2->type == XML_ELEMENT_NODE && strcmp((const char *) node2->name, "CLUSTER") == 0) {
					cluster = (char *) xmlGetProp(node2, (const xmlChar *) "NAME");
					debug("\tFound new cluster: %s\n", cluster);

					sprintf(filenamebuf, "%s/%s/%s", cachepath, grid, cluster);
					ret = ensure_path(filenamebuf);
					if (ret < 0) {
		                                retc = -1;
                		                goto cleanup;
					}

					for (node3 = node2->children; node3; node3 = node3->next) {
               		                        if (node3->type == XML_ELEMENT_NODE && strcmp((const char *) node3->name, "HOST") == 0) {
							host = (char *) xmlGetProp(node3, (const xmlChar *) "NAME");
       	                	                        debug("\t\tFound new host: %s\n", host);

							sprintf(filenamebuf, "%s/%s/%s/%s", cachepath, grid, cluster, host);
							if (ret < 0) {
				                                retc = -1;
				                                goto cleanup;
				                        }

							count = 0;

							f = fopen(filenamebuf, "w");
							if (f == NULL) {
								retc = -1;
								goto cleanup;
							}

							value = (char *) xmlGetProp(node3, (const xmlChar *) "REPORTED");
							fprintf(f, "#REPORTED, ,%s\n", value);

							for (node4 = node3->children; node4; node4 = node4->next) {
								if (node4->type == XML_ELEMENT_NODE && strcmp((const char *) node4->name, "METRIC") == 0) {
									name = (char *) xmlGetProp(node4, (const xmlChar *) "NAME");
									units = (char *) xmlGetProp(node4, (const xmlChar *) "UNITS");
									value = (char *) xmlGetProp(node4, (const xmlChar *) "VAL");

									debug("\t\t\tFound new metric: %s\n", name);

									fprintf(f, "%s,%s,%s\n", name, units, value);
									count++;
								}
							}

							fclose(f);

							debug("\t\t\tWrote %d metrics to %s\n", count, filenamebuf);
                                        	}
					}
				}
			}
		}
	}

cleanup:
	xmlFreeDoc(doc);

	xmlCleanupParser();

	return retc;
}

/*
 * Retrieve a value from a per-host cache file
 */

int fetch_value_from_cache(char *hostfile, char *metric, char *result, char *units)
{
	int retc = 0;
	FILE *f;

	f = fopen(hostfile, "r");

	if (f == NULL)
		return -1;

	char buf[256];
	while (fgets(buf, 256, f) != NULL) {
		// stip newline
		buf[strlen(buf) - 1] = '\0';

		char *pch;
		pch = strtok (buf, ",");
		if (strcmp(metric, pch) == 0) {
			strcpy(units, strtok(NULL, ","));
			strcpy(result, strtok(NULL, ","));

			retc = 1;

			break;
		}
	}

	fclose(f);

	return retc;
}

/*
 * Write XML out to a file
 */

int write_xml(char *xml, int xlen, char *xmlfile)
{
	int f;
	f = open(xmlfile, O_WRONLY);

	if (f < 0)
		return -1;

	int ret = write(f, xml, xlen);
	if (ret < 0 || ret < xlen) {
		printf("Error: written %d bytes\n", ret);
	}

	close(f);

	return 0;
}

/*
 * Display a debug message is debugging is enabled
 */

static void debug(const char *fmt, ...)
{
	if (config.debug) {
		va_list parg;
		va_start(parg, fmt);
		vprintf(fmt, parg);
		va_end(parg);
	}
}

/*
 * Perform a threshold check of a particular value
 */

static int threshold_check(char *threshold, char *value)
{
	if (strcmp(threshold, "") == 0 || strcmp(value, "") == 0) {
		return 0;
	}

	int length = strlen(threshold);

	float val = strtof(value, NULL);
	float val2, val3;

	char *colon = strchr(threshold, ':');
	if (colon != NULL) {
		*colon = '\0';
	}

	if (colon == NULL) {
		val2 = strtof(threshold, NULL);
		debug("if %f < 0 or %f > %f\n", val, val, val2);
		return (val < 0 || val > val2);
	} else if (threshold[length-1] == '\0') {
		val2 = strtof(threshold, NULL);
		debug("if %f < %f\n", val, val2);
		return (val < val2);
	} else if (threshold[0] == '~') {
		val2 = strtof(colon + 1, NULL);
		debug("if %f > %f\n", val, val2);
		return (val > val2);
	} else if (threshold[0] == '@') {
		val2 = strtof(threshold + 1, NULL);
		val3 = strtof(colon + 1, NULL);
		debug("if %f > %f and %f < %f\n", val, val2, val, val3);
		return (val >= val2 && val <= val3);
	} else {
		val2 = strtof(threshold, NULL);
                val3 = strtof(colon + 1, NULL);
		debug("if %f < %f or %f > %f\n", val, val2, val, val3);
		return (val < val2 || val > val3);
	}

	return 0;
}

/*
 * Read command-line options and build a runtime configuration
 */

int get_config(int argc, char *argv[])
{
	int c;

        // set defaults for optional params
        config.max_age = 120;
        strcpy(config.gmetad_host, "localhost");
        config.gmetad_port = 8651;
        strcpy(config.cachepath, "/tmp");
        strcpy(config.cachename, ".gm-cache");
        config.debug = 0;
        config.dump = 0;
	config.warning[0] = '\0';
	config.critical[0] = '\0';
	config.host[0] = '\0';
	config.metric[0] = '\0';
	config.heartbeat = -1;
	config.short_name = 0;

	// get command line options
	static struct option long_options[] = {
        	{"cache_path",    required_argument, 0, 'f'},
        	{"gmetad_host",   required_argument, 0, 'd' },
        	{"warning",       required_argument, 0, 'w' },
        	{"critical",      required_argument, 0, 'c' },
        	{"metric_host",   required_argument, 0, 'a' },
        	{"metric_name",   required_argument, 0, 'm' },
		{"heartbeat",     required_argument, 0, 'h' },
		{"verbose",       no_argument,       0, 'v' },
		{"short_name",    no_argument,       0, 's' },
        	{0,               0,                 0,  0  }
        };

	while (1) {
		int option_index = 0;
		c = getopt_long(argc, argv, "svf:w:c:m:a:d:h:", long_options, &option_index);
	        if (c == -1)
			break;

	        switch (c) {
			case 'v':
				config.debug = 1;
				debug("Debugging enabled\n");
				break;

		        case 'w':
		        	strcpy(config.warning, optarg);
		        	break;

		        case 'c':
		        	strcpy(config.critical, optarg);
		        	break;

		        case 'f':
		        	strcpy(config.cachepath, optarg);
		        	break;

			case 'd':
				strcpy(config.gmetad_host, optarg);
				break;

			case 'a':
				strcpy(config.host, optarg);
				break;

			case 'm':
				strcpy(config.metric, optarg);
				break;

			case 'h':
				config.heartbeat = strtol(optarg, NULL, 10);
				break;

			case 's':
				config.short_name = 1;
				break;

		        case '?':
		            break;
	        }
	}

	if (strcmp(config.host, "") == 0) {
		printf("Must supply host to check!\n");
		return -1;
	}

	if (strcmp(config.metric, "") == 0 && config.heartbeat < 0) {
		printf("Must choose positive heartbeat or supply metric to check!\n");
		return -1;
	}

	return 0;
}

/*
 * Backoff timer for global cache file lock collisions
 */

void backoff(float base)
{
        float f = 1.0 / RAND_MAX;

        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);

        srandom(t.tv_nsec);

        float r = base + 3 * (f * random());

        struct timespec b;
        b.tv_sec = (int) r;
        b.tv_nsec = 1000000000 * (r - b.tv_sec);

	debug("Sleeping for %f seconds\n", r);

	nanosleep(&b, NULL);
}

int locate_hostfile (char *hostfile)
{
	struct stat f;

	if (stat(hostfile, &f) == 0) {
		return 0;
	}

	if (config.short_name) {
		char *host = get_shortname(config.host);

		int hostfile_len = strlen(config.cachepath) + strlen(host) + 2;
		hostfile = realloc(hostfile, hostfile_len);
		snprintf(hostfile, hostfile_len, "%s/%s", config.cachepath, host);

		free(host);

		if (stat(hostfile, &f) == 0) {
			return 0;
		}
	}

	return -1; // ultimately not found
}

int main(int argc, char *argv[])
{
	int retc = 0;
	int ret;

	char *hostfile = NULL;
        char *cachefile = NULL;
	char *xml = NULL;
        char *xmlfile = NULL;

	int cachefd = -1;

	ret = get_config(argc, argv);
	if (ret < 0) {
		retc = 2;
		goto cleanup;
	}

	if (config.heartbeat > 0) {
		debug("Checking heartbeat for %s with threshold %d\n", config.host, config.heartbeat);
	} else {
		debug("Checking %s for %s metric\n", config.host, config.metric);
	}

	int hostfile_len = strlen(config.cachepath) + strlen(config.host) + 2;
	int cachefile_len = strlen(config.cachepath) + strlen(config.cachename) + 2;

	hostfile = malloc(hostfile_len);
	cachefile = malloc(cachefile_len);

	snprintf(hostfile, hostfile_len, "%s/%s", config.cachepath, config.host);
	snprintf(cachefile, cachefile_len, "%s/%s", config.cachepath, config.cachename);

	char value[64];
	char units[64];

	int retry_count = 0, ret2;

retry:
	debug("Checking cache at %s\n", cachefile);
	ret = check_cache_age(cachefile);
	if (ret < 0) {
		printf("ERROR: Unable to check cache age.\n");
		retc = 2;
		goto cleanup;
	}
	debug("Cache age is %d\n", ret);

	if (ret >= config.max_age) {
		debug("Connecting to %s on port %d\n", config.gmetad_host, config.gmetad_port);
		ret = fetch_xml(config.gmetad_host, config.gmetad_port, &xml);
		if (ret < 0) {
			printf("ERROR: Unable to get XML data: %d.\n", ret);
			retc = 2;
			goto cleanup;
		}

		debug("Read %d bytes from %s\n", ret, config.gmetad_host);

		if (config.dump) {
			xmlfile = calloc((strlen(config.cachepath) + 9), sizeof(char));
			sprintf(xmlfile, "%s/dump.xml", config.cachepath);

			debug("Dumping XML to %s\n", xmlfile);
			if (write_xml(xml, ret, xmlfile) < 0) {
				printf("ERROR: Unable to dump XML.\n");
				retc = 2;
				goto cleanup;
			}
		}

		ret2 = get_cache_lock(cachefile, &cachefd);
		if (ret2 < 0) {
			if (retry_count == MAX_RETRY) {
				printf("ERROR: Unable to get cache lock after retrying %d times. Stale lock?", retry_count);
		                retc = 2;
                		goto cleanup;
			} else {
				backoff(retry_count / 2.0);
			}

			retry_count++;
			goto retry;
		}

		debug("Parsing XML into %s\n", config.cachepath);
		ret = parse_xml_to_cache(xml, ret, config.cachepath, cachefile);
		if (ret < 0) {
			printf("ERROR: Unable to parse XML.\n");
			retc = 2;
			goto cleanup;
		}

		release_cache_lock(cachefile, &cachefd);
	}

	if (config.heartbeat > 0) {
		strcpy(config.metric, "#REPORTED");
	}

	ret = locate_hostfile(hostfile);

	if (ret < 0) {
		printf("CRITICAL - Unable to locate cache file for %s\n", config.host);
		retc = 2;
		goto cleanup;
	}

	debug("Fetching %s metric from cache at %s\n", config.metric, hostfile);

	ret = fetch_value_from_cache(hostfile, config.metric, (char *) &value, (char *) units);

	if (ret < 0) {
		printf("CRITICAL - Unable to read cache at %s\n", hostfile);
		retc = 2;
		goto cleanup;
	} else if (ret == 0) {
		printf("CRITICAL - Metric %s not found\n", config.metric);
		retc = 2;
		goto cleanup;
	}

	debug("Checking...\n");

	if (config.heartbeat > 0) {
		int diff = time(NULL) - strtol(value, NULL, 10);
		if (diff > config.heartbeat) {
			printf("CRITICAL - %d over threshold %d\n", diff, config.heartbeat);
			retc = 2;
			goto cleanup;
		} else {
			printf("OK - %d\n", diff);
			goto cleanup;
		}
	}

	if (threshold_check(config.critical, value)) {
		printf("CRITICAL - %s %s\n", value, units);
                retc = 2;
	} else {
		if (threshold_check(config.warning, value)) {
			printf("WARNING - %s %s\n", value, units);
                        retc = 1;
                } else {
                        printf("OK - %s %s\n", value, units);
                }
	}

cleanup:
	if (cachefd >= 0) {
		release_cache_lock(cachefile, &cachefd);
	}

	if (xml != NULL)
		free(xml);

	if (xmlfile != NULL)
		free(xmlfile);

	if (hostfile != NULL)
		free(hostfile);

	if (cachefile != NULL)
		free(cachefile);

	exit(retc);
}

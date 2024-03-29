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
#include <limits.h>

#define MAX_RETRY 4
#define CHUNK 1048576
#define MINI_CHUNK 65536

static struct {
	int max_age;
	char metric[256];
	char host[256];
	char gmetad_host[256];
	int gmetad_port;
	char cachepath[4096];
	char cachename[256];

	char short_name; /* bool */

	char warning[64];
	char critical[64];

	int heartbeat;

	int debug;
	int dump;
} config;

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
 * Get the shortname for a host
 */

static char *get_shortname(const char *longname) {
	char *shortname = malloc(strlen(longname) + 1);

	strcpy(shortname, longname);
	strtok(shortname, ".");

	return shortname;
}

/*
 * Create global cache file
 */

static int create_cachefile(const char *cachefile)
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

static int check_cache_age(const char *cachefile)
{
	struct stat f;

	if (stat(cachefile, &f) < 0) {
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

static int gmetad_connect(const char *host, int port)
{
	int sockfd;
	struct hostent *gmetad;
	struct sockaddr_in addr;

	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0)
		return -1;

	gmetad = gethostbyname(host);
	if (gmetad == NULL)
		return -2;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	memcpy(&addr.sin_addr.s_addr, gmetad->h_addr, gmetad->h_length);
	addr.sin_port = htons(port);

	if (connect(sockfd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		printf("Connection error: %s (%d)\n", strerror(errno), errno);
		return -3;
	}

	debug("Connected\n");
	
	return sockfd;
}

/*
 * Fetch XML from gmetad
 */

static int fetch_xml(const char *host, int port, char **dest)
{
	int sockfd;
	int ret, offset = 0;
	int buffer_size = CHUNK;
	char *buffer;
	char *buffer2 = NULL;

	sockfd = gmetad_connect(host, port);
	if (sockfd < 0) {
		return sockfd;
	}

	buffer = malloc(buffer_size);

	debug("%d kB chunk size\n", buffer_size / 1024);

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

	debug("Receive used approx %d kB of %d kB allocated.\n",
	      offset / 1024, buffer_size / 1024);

	*dest = buffer;

	close(sockfd);

	return offset;
}

/*
 * Ensure a path exists, create if it does not
 */

static int create_abs_path(const char *path)
{
	char *copy = strdup(path);
	char *p;
	p = copy+1;
	while((p = strchr(p, '/')) != NULL) {
		p = strchr(p, '/');
		*p = '\0';
		printf("%s\n", copy);
		if (mkdir(copy, S_IRWXU | S_IXOTH | S_IXGRP | S_IROTH | S_IRGRP) < 0 && errno != EEXIST)
			return -1;
		*p = '/';
		p += 1;
	}

	free(copy);
	return 0;
}

/*
 * Lock the global cache file
 */

static int get_cache_lock(const char *cachefile, int *cachefd)
{
	struct flock l;

	if (*cachefd < 0) {
		*cachefd = open(cachefile, O_RDWR);
		if (*cachefd < 0) {
			printf("Unable to get cache FD\n");
			return -1;
		}
	}

	l.l_type = F_WRLCK;
	l.l_whence = SEEK_SET;
	l.l_start = 0;
	l.l_len = 0;

	if (fcntl(*cachefd, F_SETLK, &l) < 0)
		return -1;

	return 0;
}

/*
 * Touch cache lock to indicate it has been updated
 */

static int touch_cache_lock (const char *cachefile)
{
	utimes(cachefile, NULL);

	return 0;
}

/*
 * Release the lock on the global cache file
 */

static int release_cache_lock (const char *cachefile, int *cachefd)
{
	struct flock l;

	l.l_type = F_UNLCK;
	l.l_whence = SEEK_SET;
	l.l_start = 0;
	l.l_len = 0;

	if (fcntl(*cachefd, F_SETLK, &l) < 0) {
		printf("Failed to remove lock\n");
		/* TODO: will this be fatal ? */
	}

	close(*cachefd);
	*cachefd = -1;

	return 0;
}

/*
 * Get named property from XML node
 */

static char *get_prop(xmlNode *node, const char *property)
{
	return (char *) xmlGetProp(node, (const xmlChar *) property);
}

/*
 * Determine if XML node name matches given name
 */

static int is_element(xmlNode *node, const char *name)
{
	return (node->type == XML_ELEMENT_NODE &&
		strcmp((const char *) node->name, name) == 0);
}

static int parse_xml_tree_to_cache(xmlNode *root, const char *cachepath, const char *cachefile)
{
	xmlNode *node, *node2, *node3, *node4 = NULL;
	char *name, *units, *value, *grid, *cluster, *host;
	FILE *f;
	char filenamebuf[PATH_MAX];
	int count;

	for (node = root->children; node; node = node->next) {
		if (!is_element(node, "GRID"))
			continue; /* skip non-element and non-cluster nodes */

		grid = get_prop(node, "NAME");
		debug("Found new grid: %s\n", grid);

		for (node2 = node->children; node2; node2 = node2->next) {
			if (!is_element(node2, "CLUSTER"))
				continue; /* skip non-element and non-cluster nodes */

			cluster = get_prop(node2, "NAME");
			debug("\tFound new cluster: %s\n", cluster);

			for (node3 = node2->children; node3; node3 = node3->next) {
				if (!is_element(node3, "HOST"))
					continue; /* skip non-element and non-host nodes */

				host = get_prop(node3, "NAME");
				if (config.short_name) {
					host = get_shortname(host);
				}

				debug("\t\tFound new host: %s\n", host);

				snprintf(filenamebuf, PATH_MAX, "%s/%s/%s/%s", cachepath, grid, cluster, host);
				if (config.short_name) {
					free(host);
				}

				if (create_abs_path(filenamebuf) < 0)
					return -1;

				count = 0;

				f = fopen(filenamebuf, "w");
				if (f == NULL)
					return -1;

				value = get_prop(node3, "REPORTED");
				fprintf(f, "#REPORTED, ,%s\n", value);

				for (node4 = node3->children; node4; node4 = node4->next) {
					if (!is_element(node4, "METRIC"))
						continue; /* skip non-element and non-metric nodes */

					name = get_prop(node4, "NAME");
					units = get_prop(node4, "UNITS");
					value = get_prop(node4, "VAL");

					debug("\t\t\tFound new metric: %s\n", name);

					fprintf(f, "%s,%s,%s\n", name, units, value);
					count++;
				}

				fclose(f);

				debug("\t\t\tWrote %d metrics to %s\n", count, filenamebuf);
			}
		}
	}

	return 0;
}

/*
 * Parse the XML out to per-host cache files
 */

static int parse_xml_to_cache(const char *xml, int xlen,
			      const char *cachepath, const char *cachefile)
{
	int retc = -1;

	xmlDoc *doc = NULL;
	xmlNode *root = NULL;

	/* suggested ABI check */
	LIBXML_TEST_VERSION

	doc = xmlReadMemory(xml, xlen, "xml", NULL, 0);

	if (doc == NULL) {
		xmlError *pErr = xmlGetLastError();
		if (pErr == NULL) {
			printf("panic!\n");
			exit(100);
		}

		printf("Error parsing #%d (%d,%d)\n",
		       pErr->code, pErr->line,pErr->int2);

		goto cleanup;
	}

	root = xmlDocGetRootElement(doc);

	if (strcmp((char *) root->name, "GANGLIA_XML") != 0) {
		goto cleanup;
	}

	if (parse_xml_tree_to_cache(root, cachepath, cachefile) != 0)
		retc = 0;

cleanup:
	xmlFreeDoc(doc);

	xmlCleanupParser();

	return retc;
}

/*
 * Retrieve a value from a per-host cache file
 */

static int fetch_value_from_cache(const char *hostfile, const char *metric,
				  char *result, char *units)
{
	int retc = 0;
	FILE *f;
	char *pch;
	char buf[256];

	f = fopen(hostfile, "r");
	if (f == NULL)
		return -1;

	while (fgets(buf, 256, f) != NULL) {
		/* stip newline */
		buf[strlen(buf) - 1] = '\0';

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

static int write_xml(const char *xml, int xlen, const char *xmlfile)
{
	int f;
	int ret;

	f = open(xmlfile, O_WRONLY);
	if (f < 0)
		return -1;

	ret = write(f, xml, xlen);
	if (ret < 0 || ret < xlen) {
		printf("Error: written %d bytes\n", ret);
	}

	close(f);

	return 0;
}

/*
 * Perform a threshold check of a particular value
 */

static int threshold_check(char *threshold, char *value)
{
	int length;
	float val, val2, val3;
	char *colon;

	if (strcmp(threshold, "") == 0 || strcmp(value, "") == 0) {
		return 0;
	}

	length = strlen(threshold);
	val = strtof(value, NULL);

	colon = strchr(threshold, ':');
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

static int get_config(int argc, char *argv[])
{
	int c;

	/* command line options */
	static struct option long_options[] = {
		{"cache_path",    required_argument, NULL, 'f'},
		{"gmetad_host",   required_argument, NULL, 'd' },
		{"warning",       required_argument, NULL, 'w' },
		{"critical",      required_argument, NULL, 'c' },
		{"metric_host",   required_argument, NULL, 'a' },
		{"metric_name",   required_argument, NULL, 'm' },
		{"heartbeat",     required_argument, NULL, 'h' },
		{"verbose",       no_argument,       NULL, 'v' },
		{"short_name",    no_argument,       NULL, 's' },
		{"max_age",       required_argument, NULL, 'x' },
		{NULL,	       0,		 NULL,  0  }
	};

	/* set defaults for optional params */
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

	while (1) {
		int option_index = 0;
		c = getopt_long(argc, argv, "svf:w:c:m:a:d:h:x:",
				long_options, &option_index);
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

			case 'x':
				config.max_age = strtol(optarg, NULL, 10);
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

static void backoff(float base)
{
	float f = 1.0 / RAND_MAX;
	float r;
	struct timespec t;
	struct timespec b;

	clock_gettime(CLOCK_MONOTONIC, &t);
	srandom(t.tv_nsec);

	r = base + 3 * (f * random());

	b.tv_sec = (int) r;
	b.tv_nsec = 1000000000 * (r - b.tv_sec);

	debug("Sleeping for %f seconds\n", r);

	nanosleep(&b, NULL);
}

static int locate_hostfile (char *hostfile)
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

	return -1; /* ultimately not found */
}

static char *get_full_cache_path(const char *cachepath, const char *file)
{
	int len = strlen(cachepath) + strlen(file) + 2;
	char *fullpath = malloc(len);
	snprintf(fullpath, len, "%s/%s", cachepath, file);
	return fullpath;
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

	char value[64];
	char units[64];

	int retry_count = 0, ret2;

	if (get_config(argc, argv) < 0) {
		retc = 2;
		goto cleanup;
	}

	if (config.heartbeat > 0) {
		debug("Checking heartbeat for %s with threshold %d\n",
		      config.host, config.heartbeat);
	} else {
		debug("Checking %s for %s metric\n",
		      config.host, config.metric);
	}

	hostfile = get_full_cache_path(config.cachepath, config.host);
	cachefile = get_full_cache_path(config.cachepath, config.cachename);

retry:
	debug("Checking cache at %s\n", cachefile);
	ret = check_cache_age(cachefile);
	if (ret < 0) {
		printf("ERROR: Unable to check cache age.\n");
		retc = 2;
		goto cleanup;
	}

	if (ret < config.max_age) {
		debug("Cache age is %d\n", ret);
	} else {
		debug("Cache age greater than configured max (%d >= %d)\n", ret, config.max_age);
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

		touch_cache_lock(cachefile);
		release_cache_lock(cachefile, &cachefd);
	}

	if (config.heartbeat > 0) {
		strcpy(config.metric, "#REPORTED");
	}

	if (locate_hostfile(hostfile) < 0) {
		printf("CRITICAL - Unable to locate cache file for %s\n", config.host);
		retc = 2;
		goto cleanup;
	}

	debug("Fetching %s metric from cache at %s\n", config.metric, hostfile);

	ret = fetch_value_from_cache(hostfile, config.metric, value, units);
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

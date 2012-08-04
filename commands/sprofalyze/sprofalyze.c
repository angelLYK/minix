#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* user-configurable settings */
#define BINARY_HASHTAB_SIZE 1024

#define DEBUG 0

#define NM "/usr/pkg/bin/nm"

static const char *default_binaries[] = {
	"kernel/kernel",
	"servers/",
	"drivers/",
};

static const char *src_path = "/usr/src";

/* types */

#define BINARY_NAME_SIZE 8

#define LINE_WIDTH 80

#define SYMBOL_NAME_SIZE 52

#define SYMBOL_NAME_WIDTH 22

#define SYMBOL_SIZE_MAX 0x100000

#define PC_MAP_L1_SIZE 0x10000
#define PC_MAP_L2_SIZE 0x10000

struct binary_info;

struct symbol_count {
	struct symbol_count *next;
	struct binary_info *binary;
	uint32_t addr;
	int samples;
	char name[SYMBOL_NAME_SIZE];
};

struct pc_map_l2 {
	struct symbol_count *l2[PC_MAP_L2_SIZE];
};

struct pc_map_l1 {
	struct pc_map_l2 *l1[PC_MAP_L1_SIZE];
};

struct binary_info {
	char name[BINARY_NAME_SIZE];
	const char *path;
	int samples;
	struct symbol_count *symbols;
	struct pc_map_l1 *pc_map;
	struct binary_info *next;
	struct binary_info *hashtab_next;
	char no_more_warnings;
};

struct trace_sample {
	char name[BINARY_NAME_SIZE];
	uint32_t pc;
};

/* global variables */
static struct binary_info *binaries;
static struct binary_info *binary_hashtab[BINARY_HASHTAB_SIZE];
static double minimum_perc = 1.0;
static int samples_idle, samples_system, samples_total, samples_user;

/* prototypes */
static struct binary_info *binary_add(const char *path);
static struct binary_info *binary_find(const char *name);
static struct binary_info *binary_hashtab_get(const char *name);
static struct binary_info **binary_hashtab_get_ptr(const char *name);
static void binary_load_pc_map(struct binary_info *binary_info);
static const char *binary_name(const char *path);
static int compare_binaries(const void *p1, const void *p2);
static int compare_symbols(const void *p1, const void *p2);
static int count_symbols(const struct binary_info *binary, int threshold);
static void load_trace(const char *path);
static void *malloc_checked(size_t size);
static unsigned name_hash(const char *name);
static float percent(int value, int percent_of);
static void print_report(void);
static void print_report_overall(void);
static void print_report_per_binary(const struct binary_info *binary);
static void print_reports_per_binary(void);
static void print_report_symbols(struct symbol_count **symbols,
	unsigned symbol_count, int total, int show_binary);
static void print_separator(void);
static int read_hex(FILE *file, unsigned long *value);
static int read_newline(FILE *file);
static void read_nm_line(FILE *file, int line, char *name, char *type,
	unsigned long *addr, unsigned long *size);
static void read_to_whitespace(FILE *file, char *buffer, size_t size);
static void sample_store(const struct trace_sample *sample);
static char *strdup_checked(const char *s);
static void usage(const char *argv0);

#define MALLOC_CHECKED(type, count) \
	((type *) malloc_checked(sizeof(type) * (count)))

#define LENGTHOF(array) (sizeof((array)) / sizeof((array)[0]))

#if DEBUG
#define dprintf(...) do { 						\
	fprintf(stderr, "debug(%s:%d): ", __FUNCTION__, __LINE__); 	\
	fprintf(stderr, __VA_ARGS__); 					\
} while(0)
#else
#define dprintf(...)
#endif

int main(int argc, char **argv) {
	int opt;

#ifdef DEBUG
	/* disable buffering so the output mixes correctly */
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
#endif

	/* parse arguments */
	while ((opt = getopt(argc, argv, "b:p:s:")) != -1) {
		switch (opt) {
		case 'b':
			/* additional binary specified */
			binary_add(optarg);
			break;
		case 'p':
			/* minimum percentage specified */
			minimum_perc = atof(optarg);
			if (minimum_perc < 0 || minimum_perc > 100) {
				fprintf(stderr, "error: cut-off percentage "
					"makes no sense: %g\n", minimum_perc);
				exit(1);
			}
			break;
		case 's':
			/* source tree directory specified */
			src_path = optarg;
			break;
		default: usage(argv[0]);
		}
	}

	/* load samples */
	if (optind >= argc) usage(argv[0]);
	for (; optind < argc; optind++) {
		load_trace(argv[optind]);
	}

	/* print report */
	print_report();
	return 0;
}

static struct binary_info *binary_add(const char *path) {
	struct binary_info *binary, **ptr;
	const char *name;

	/* assumption: path won't be overwritten or deallocated in the future */

	/* not too much effort escaping for popen, prevent problems here */
	assert(path);
	if (strchr(path, '"')) {
		fprintf(stderr, "error: path \"%s\" contains a quote\n", path);
		exit(1);
	}

	/* get filename */
	name = binary_name(path);
	dprintf("adding binary \"%s\" with name \"%.*s\"\n",
		path, BINARY_NAME_SIZE, name);
	if (strlen(name) == 0) {
		fprintf(stderr, "error: path \"%s\" does not "
			"contain a filename\n", path);
		exit(1);
	}

	/* check in hashtable whether this entry is indeed new */
	ptr = binary_hashtab_get_ptr(name);
	if (*ptr) {
		fprintf(stderr, "warning: ignoring \"%s\" because \"%s\" was "
			"previously specified\n", path, (*ptr)->path);
		return *ptr;
	}
	dprintf("using %.*s from \"%s\"\n", BINARY_NAME_SIZE, name, path);

	/* allocate new binary_info */
	binary = MALLOC_CHECKED(struct binary_info, 1);
	memset(binary, 0, sizeof(struct binary_info));
	binary->path = path;
	strncpy(binary->name, name, sizeof(binary->name));

	/* insert into linked list */
	binary->next = binaries;
	binaries = binary;

	/* insert into hashtable */
	*ptr = binary;
	return binary;
}

static struct binary_info *binary_find(const char *name) {
	struct binary_info *binary;
	const char *current_name;
	unsigned i;
	char path[PATH_MAX + 1], *path_end;

	assert(name);

	/* name is required */
	if (!*name) {
		fprintf(stderr, "warning: binary unspecified in sample\n");
		return NULL;
	}

	/* do we already know this binary? */
	binary = binary_hashtab_get(name);
	if (binary) return binary;

	/* search for it */
	dprintf("searching for binary \"%.*s\" in \"%s\"\n",
		BINARY_NAME_SIZE, name, src_path);
	for (i = 0; i < LENGTHOF(default_binaries); i++) {
		snprintf(path, sizeof(path), "%s/%s", src_path,
			default_binaries[i]);
		current_name = binary_name(path);
		assert(current_name);
		if (*current_name) {
			/* paths not ending in slash: use if name matches */
			if (strncmp(name, current_name,
				BINARY_NAME_SIZE) != 0) {
				continue;
			}
		} else {
			/* paths ending in slash: look in subdir named after
			 * binary
			 */
			path_end = path + strlen(path);
			snprintf(path_end, sizeof(path) - (path_end - path),
				"%.*s/%.*s", BINARY_NAME_SIZE, name,
				BINARY_NAME_SIZE, name);
		}

		/* use access to find out whether the binary exists and is
		 * readable
		 */
		dprintf("checking whether \"%s\" exists\n", path);
		if (access(path, R_OK) < 0) continue;

		/* ok, this seems to be the one */
		return binary_add(strdup_checked(path));
	}

	/* not found */
	return NULL;
}

static struct binary_info *binary_hashtab_get(const char *name) {
	return *binary_hashtab_get_ptr(name);
}

static struct binary_info **binary_hashtab_get_ptr(const char *name) {
	struct binary_info *binary, **ptr;

	/* get pointer to location of the binary in hash table */
	ptr = &binary_hashtab[name_hash(name) % BINARY_HASHTAB_SIZE];
	while ((binary = *ptr) && strncmp(binary->name, name,
		BINARY_NAME_SIZE) != 0) {
		ptr = &binary->hashtab_next;
	}
	dprintf("looked up \"%.*s\" in hash table, %sfound\n",
		BINARY_NAME_SIZE, name, *ptr ? "" : "not ");
	return ptr;
}

static void binary_load_pc_map(struct binary_info *binary_info) {
	unsigned long addr, size;
	char *command;
	size_t command_len;
#if DEBUG
	unsigned count = 0;
#endif
	FILE *file;
	int index_l1, index_l2, line;
	char name[SYMBOL_NAME_SIZE];
	struct pc_map_l2 *pc_map_l2, **pc_map_l2_ptr;
	struct symbol_count *symbol, **symbol_ptr;
	char type;

	assert(binary_info);
	assert(!strchr(NM, '"'));
	assert(!strchr(binary_info->path, '"'));

	/* does the file exist? */
	if (access(binary_info->path, R_OK) < 0) {
		if (!binary_info->no_more_warnings) {
			fprintf(stderr, "warning: \"%s\" does not exist or "
				"not readable.\n", binary_info->path);
			fprintf(stderr, "         Did you do a make?\n");
			binary_info->no_more_warnings = 1;
		}
		return;
	}

	/* execute nm to get symbols */
	command_len = strlen(NM) + strlen(binary_info->path) + 32;
	command = MALLOC_CHECKED(char, command_len);
	snprintf(command, command_len, "\"%s\" -nP \"%s\"",
		NM, binary_info->path);
	dprintf("running command for extracting addresses: %s\n", command);
	file = popen(command, "r");
	if (!file) {
		perror("failed to start " NM);
		exit(-1);
	}

	/* read symbols from nm output */
	assert(!binary_info->symbols);
	symbol_ptr = &binary_info->symbols;
	line = 1;
	while (!feof(file)) {
		/* read nm output line; can't use fscanf as it doesn't know
		 * where to stop
		 */
		read_nm_line(file, line++, name, &type, &addr, &size);

		/* store only text symbols */
		if (type != 't' && type != 'T') continue;

		*symbol_ptr = symbol = MALLOC_CHECKED(struct symbol_count, 1);
		memset(symbol, 0, sizeof(*symbol));
		symbol->binary = binary_info;
		symbol->addr = addr;
		strncpy(symbol->name, name, SYMBOL_NAME_SIZE);
		symbol_ptr = &symbol->next;
#if DEBUG
		count++;
#endif
	}
	fclose(file);
	dprintf("extracted %u symbols\n", count);

	/* create program counter map from symbols */
	assert(!binary_info->pc_map);
	binary_info->pc_map = MALLOC_CHECKED(struct pc_map_l1, 1);
	memset(binary_info->pc_map, 0, sizeof(struct pc_map_l1));
	for (symbol = binary_info->symbols; symbol; symbol = symbol->next) {
		/* compute size if not specified */
		size = symbol->next ? (symbol->next->addr - symbol->addr) : 1;
		if (size > SYMBOL_SIZE_MAX) size = SYMBOL_SIZE_MAX;

		/* mark each address */
		for (addr = symbol->addr; addr - symbol->addr < size; addr++) {
			index_l1 = addr / PC_MAP_L2_SIZE;
			assert(index_l1 < PC_MAP_L1_SIZE);
			pc_map_l2_ptr = &binary_info->pc_map->l1[index_l1];
			if (!(pc_map_l2 = *pc_map_l2_ptr)) {
				*pc_map_l2_ptr = pc_map_l2 =
					MALLOC_CHECKED(struct pc_map_l2, 1);
				memset(pc_map_l2, 0, sizeof(struct pc_map_l2));
			}
			index_l2 = addr % PC_MAP_L2_SIZE;
			pc_map_l2->l2[index_l2] = symbol;
		}
	}
}

static const char *binary_name(const char *path) {
	const char *name, *p;

	/* much like basename, but guarantees to not modify the path */
	name = path;
	for (p = path; *p; p++) {
		if (*p == '/') name = p + 1;
	}
	return name;
}

static int compare_binaries(const void *p1, const void *p2) {
	const struct binary_info *const *b1 = p1, *const *b2 = p2;

	/* binaries with more samples come first */
	assert(b1);
	assert(b2);
	assert(*b1);
	assert(*b2);
	if ((*b1)->samples > (*b2)->samples) return -1;
	if ((*b1)->samples < (*b2)->samples) return 1;
	return 0;
}

static int compare_symbols(const void *p1, const void *p2) {
	const struct symbol_count *const *s1 = p1, *const *s2 = p2;

	/* symbols with more samples come first */
	assert(s1);
	assert(s2);
	assert(*s1);
	assert(*s2);
	if ((*s1)->samples > (*s2)->samples) return -1;
	if ((*s1)->samples < (*s2)->samples) return 1;
	return 0;
}

static int count_symbols(const struct binary_info *binary, int threshold) {
	struct symbol_count *symbol;
	int result = 0;

	for (symbol = binary->symbols; symbol; symbol = symbol->next) {
		if (symbol->samples >= threshold) result++;
	}
	return result;
}

static void load_trace(const char *path) {
	FILE *file;
	int s_idle, s_system, s_total, s_user;
	int samples_read;
	struct trace_sample sample;

	/* open trace file */
	file = fopen(path, "rb");
	if (!file) {
		fprintf(stderr, "error: cannot open trace file \"%s\": %s\n",
			path, strerror(errno));
		exit(1);
	}

	/* check file format and update totals */
	if (fscanf(file, "stat\n%d %d %d %d\n",
		&s_total, &s_idle, &s_system, &s_user) != 4) {
		fprintf(stderr, "error: file \"%s\" does not contain an "
			"sprofile trace\n", path);
		exit(1);
	}
	samples_total += s_total;
	samples_idle += s_idle;
	samples_system += s_system;
	samples_user += s_user;

	/* read and store samples */
	samples_read = 0;
	while (fread(&sample, sizeof(sample), 1, file) == 1) {
		sample_store(&sample);
		samples_read++;
	}
	if (samples_read != s_system) {
		fprintf(stderr, "warning: number of system samples (%d) in "
			"\"%s\" does not match number of records (%d)\n",
			s_system, path, samples_read);
	}
	if (!feof(file)) {
		fprintf(stderr, "warning: partial sample at end of \"%s\", "
			"was the trace file truncated?\n", path);
	}
	fclose(file);
}

static void *malloc_checked(size_t size) {
	void *p;
	if (!size) return NULL;
	p = malloc(size);
	if (!p) {
		fprintf(stderr, "error: malloc cannot allocate %lu bytes: %s\n",
			(unsigned long) size, strerror(errno));
		exit(-1);
	}
	return p;
}

static unsigned name_hash(const char *name) {
	int i;
	unsigned r = 0;

	/* remember: strncpy initializes all bytes */
	for (i = 0; i < BINARY_NAME_SIZE && name[i]; i++) {
		r = r * 31 + name[i];
	}
	dprintf("name_hash(\"%.*s) = 0x%.8x\n", BINARY_NAME_SIZE, name, r);
	return r;
}

static float percent(int value, int percent_of) {
	assert(value >= 0);
	assert(value <= percent_of);

	return (percent_of > 0) ? (value * 100.0 / percent_of) : 0;
}

static void print_report(void) {
	printf("Showing processes and functions using at least %3.0f%% "
		"time.\n\n", minimum_perc);
	printf("  System process ticks: %10d (%3.0f%%)\n",
		samples_system, percent(samples_system, samples_total));
	printf("    User process ticks: %10d (%3.0f%%)          "
		"Details of system process\n",
		samples_user, percent(samples_user, samples_total));
	printf("       Idle time ticks: %10d (%3.0f%%)          "
		"samples, aggregated and\n",
		samples_idle, percent(samples_idle, samples_total));
	printf("                        ----------  ----           "
		"per process, are below.\n");
	printf("           Total ticks: %10d (100%%)\n\n", samples_total);

	print_report_overall();
	print_reports_per_binary();
}

static void print_report_overall(void) {
	struct binary_info *binary;
	struct symbol_count *symbol, **symbols_sorted;
	unsigned index, symbol_count;
	int sample_threshold;

	/* count number of symbols to display */
	sample_threshold = samples_system * minimum_perc / 100;
	symbol_count = 0;
	for (binary = binaries; binary; binary = binary->next) {
		symbol_count += count_symbols(binary, sample_threshold);
	}

	/* sort symbols by decreasing number of samples */
	symbols_sorted = MALLOC_CHECKED(struct symbol_count *, symbol_count);
	index = 0;
	for (binary = binaries; binary; binary = binary->next) {
		for (symbol = binary->symbols; symbol; symbol = symbol->next) {
			if (symbol->samples >= sample_threshold) {
				symbols_sorted[index++] = symbol;
			}
		}
	}
	assert(index == symbol_count);
	qsort(symbols_sorted, symbol_count, sizeof(symbols_sorted[0]),
		compare_symbols);

	/* report most common symbols overall */
	print_separator();
	printf("Total system process time %*d samples\n",
		LINE_WIDTH - 34, samples_system);
	print_separator();
	print_report_symbols(symbols_sorted, symbol_count, samples_system, 1);
	free(symbols_sorted);
}

static void print_report_per_binary(const struct binary_info *binary) {
	struct symbol_count *symbol, **symbols_sorted;
	unsigned index, symbol_count;
	int sample_threshold;

	/* count number of symbols to display */
	sample_threshold = binary->samples * minimum_perc / 100;
	symbol_count = count_symbols(binary, sample_threshold);

	/* sort symbols by decreasing number of samples */
	symbols_sorted = MALLOC_CHECKED(struct symbol_count *, symbol_count);
	index = 0;
	for (symbol = binary->symbols; symbol; symbol = symbol->next) {
		if (symbol->samples >= sample_threshold) {
			symbols_sorted[index++] = symbol;
		}
	}
	assert(index == symbol_count);
	qsort(symbols_sorted, symbol_count, sizeof(symbols_sorted[0]),
		compare_symbols);

	/* report most common symbols for this binary */
	print_separator();
	printf("%-*.*s %4.1f%% of system process samples\n",
		LINE_WIDTH - 32, BINARY_NAME_SIZE, binary->name,
		percent(binary->samples, samples_system));
	print_separator();
	print_report_symbols(symbols_sorted, symbol_count, binary->samples, 0);
	free(symbols_sorted);
}

static void print_reports_per_binary(void) {
	struct binary_info *binary, **binaries_sorted;
	unsigned binary_count, i, index;
	int sample_threshold, samples_shown;
	struct symbol_count *symbol;

	/* count total per-binary samples */
	binary_count = 0;
	for (binary = binaries; binary; binary = binary->next) {
		assert(!binary->samples);
		for (symbol = binary->symbols; symbol; symbol = symbol->next) {
			binary->samples += symbol->samples;
		}
		binary_count++;
	}

	/* sort binaries by decreasing number of samples */
	binaries_sorted = MALLOC_CHECKED(struct binary_info *, binary_count);
	index = 0;
	for (binary = binaries; binary; binary = binary->next) {
		binaries_sorted[index++] = binary;
	}
	assert(index == binary_count);
	qsort(binaries_sorted, binary_count, sizeof(binaries_sorted[0]),
		compare_binaries);

	/* display reports for binaries with enough samples */
	sample_threshold = samples_system * minimum_perc / 100;
	samples_shown = 0;
	for (i = 0; i < binary_count; i++) {
		if (binaries_sorted[i]->samples < sample_threshold) break;
		print_report_per_binary(binaries_sorted[i]);
		samples_shown += binaries_sorted[i]->samples;
	}
	print_separator();
	printf("processes <%3.0f%% (not showing functions) %*.1f%% of system "
		"process samples\n", minimum_perc, LINE_WIDTH - 60,
		percent(samples_system - samples_shown, samples_system));
	print_separator();

	free(binaries_sorted);
}

static void print_report_symbols(struct symbol_count **symbols,
	unsigned symbol_count, int total, int show_process) {
	unsigned bar_dots, bar_width, i, j, process_width;
	int samples, samples_shown;
	struct symbol_count *symbol;

	/* find out how much space we have available */
	process_width = show_process ? (BINARY_NAME_SIZE + 1) : 0;
	bar_width = LINE_WIDTH - process_width - SYMBOL_NAME_WIDTH - 17;

	/* print the symbol lines */
	samples_shown = 0;
	for (i = 0; i <= symbol_count; i++) {
		if (i < symbol_count) {
			/* first list the symbols themselves */
			symbol = symbols[i];
			printf("%*.*s %*.*s ",
				process_width,
				show_process ? BINARY_NAME_SIZE : 0,
				symbol->binary->name,
				SYMBOL_NAME_WIDTH,
				SYMBOL_NAME_WIDTH,
				symbol->name);
			samples = symbol->samples;
		} else {
			/* at the end, list the remainder */
			printf("%*s<%3.0f%% ",
				process_width + SYMBOL_NAME_WIDTH - 4,
				"",
				minimum_perc);
			samples = total - samples_shown;
		}
		assert(samples >= 0);
		assert(samples <= total);
		bar_dots = (total > 0) ? (samples * bar_width / total) : 0;
		for (j = 0; j < bar_dots; j++) printf("*");
		for (; j < bar_width; j++) printf(" ");
		printf("%8d %5.1f%%\n", samples, percent(samples, total));
		samples_shown += samples;
	}

	/* print remainder and summary */
	print_separator();
	printf("%-*.*s%*d 100.0%%\n\n", BINARY_NAME_SIZE, BINARY_NAME_SIZE,
		(show_process || symbol_count == 0) ?
		"total" : symbols[0]->binary->name,
		LINE_WIDTH - BINARY_NAME_SIZE - 7, total);
}

static void print_separator(void) {
	int i;
	for (i = 0; i < LINE_WIDTH; i++) printf("-");
	printf("\n");
}

static int read_hex(FILE *file, unsigned long *value) {
	int c, cvalue;
	unsigned index;

	assert(file);
	assert(value);

	index = 0;
	c = fgetc(file);
	*value = 0;
	while (index < 8) {
		if (c >= '0' && c <= '9') {
			cvalue = c - '0';
		} else if (c >= 'A' && c <= 'F') {
			cvalue = c - 'A' + 10;
		} else if (c >= 'a' && c <= 'f') {
			cvalue = c - 'a' + 10;
		} else {
			break;
		}

		*value = *value * 16 + cvalue;
		index++;
		c = fgetc(file);
	}
	if (c != EOF) ungetc(c, file);

	return index;
}

static int read_newline(FILE *file) {
	int c;

	do {
		c = fgetc(file);
	} while (c != EOF && c != '\n' && isspace(c));
	if (c == EOF || c == '\n') return 1;
	ungetc(c, file);
	return 0;
}

static void read_nm_line(FILE *file, int line, char *name, char *type,
	unsigned long *addr, unsigned long *size) {

	assert(file);
	assert(name);
	assert(type);
	assert(addr);
	assert(size);
	*type = 0;
	*addr = 0;
	*size = 0;
	if (read_newline(file)) {
		memset(name, 0, SYMBOL_NAME_SIZE);
		return;
	}

	/* name and type are compulsory */
	read_to_whitespace(file, name, SYMBOL_NAME_SIZE);
	if (read_newline(file)) {
		fprintf(stderr, "error: bad output format from nm: "
			"symbol type missing on line %d\n", line);
		exit(-1);
	}
	*type = fgetc(file);

	/* address is optional */
	if (read_newline(file)) return;
	if (!read_hex(file, addr)) {
		fprintf(stderr, "error: bad output format from nm: junk where "
			"address should be on line %d\n", line);
		exit(-1);
	}

	/* size is optional */
	if (read_newline(file)) return;
	if (!read_hex(file, size)) {
		fprintf(stderr, "error: bad output format from nm: junk where "
			"size should be on line %d\n", line);
		exit(-1);
	}

	/* nothing else expected */
	if (read_newline(file)) return;
	fprintf(stderr, "error: bad output format from nm: junk after size "
		"on line %d\n", line);
	exit(-1);
}

static void read_to_whitespace(FILE *file, char *buffer, size_t size) {
	int c;

	/* read up to and incl first whitespace, store at most size chars */
	while ((c = fgetc(file)) != EOF && !isspace(c)) {
		if (size > 0) {
			*(buffer++) = c;
			size--;
		}
	}
	if (size > 0) *buffer = 0;
}

static void sample_store(const struct trace_sample *sample) {
	struct binary_info *binary;
	int index_l1;
	char path[PATH_MAX + 1];
	struct pc_map_l2 *pc_map_l2;
	struct symbol_count *symbol;

	/* locate binary */
	binary = binary_find(sample->name);
	if (!binary) {
		fprintf(stderr, "warning: ignoring unknown binary \"%.*s\"\n",
			BINARY_NAME_SIZE, sample->name);
		fprintf(stderr, "         did you include this executable in "
			"the configuration?\n");
		fprintf(stderr, "         (use -b to add additional "
			"binaries)\n");

		/* prevent additional warnings by adding bogus binary */
		snprintf(path, sizeof(path), "/DOESNOTEXIST/%.*s",
			BINARY_NAME_SIZE, sample->name);
		binary = binary_add(strdup_checked(path));
		binary->no_more_warnings = 1;
		return;
	}

	/* load symbols if this hasn't been done yet */
	if (!binary->pc_map) {
		binary_load_pc_map(binary);
		if (!binary->pc_map) return;
	}

	/* find the applicable symbol (two-level lookup) */
	index_l1 = sample->pc / PC_MAP_L2_SIZE;
	assert(index_l1 < PC_MAP_L1_SIZE);
	pc_map_l2 = binary->pc_map->l1[index_l1];
	if (pc_map_l2) {
		symbol = pc_map_l2->l2[sample->pc % PC_MAP_L2_SIZE];
	} else {
		symbol = NULL;
	}

	if (symbol) {
		symbol->samples++;
	} else if (!binary->no_more_warnings) {
		fprintf(stderr, "warning: address 0x%lx not associated with a "
			"symbol\n", (unsigned long) sample->pc);
		fprintf(stderr, "         binary may not match the profiled "
			"version\n");
		fprintf(stderr, "         path: \"%s\"\n", binary->path);
		binary->no_more_warnings = 1;
	}
}

static char *strdup_checked(const char *s) {
	char *p;
	if (!s) return NULL;
	p = strdup(s);
	if (!p) {
		fprintf(stderr, "error: strdup failed: %s\n",
			strerror(errno));
		exit(-1);
	}
	return p;
}

static void usage(const char *argv0) {
	printf("usage:\n");
	printf("  %s [-p percentage] [-s src-tree-path] "
		"[-b binary]... file...\n", argv0);
	exit(1);
}

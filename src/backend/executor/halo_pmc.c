/*
 * halo_pmc.c
 *   Named, per-backend counter observations for the private Hash build.
 */
#include "postgres.h"

#include <unistd.h>
#ifdef __linux__
#include <linux/perf_event.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#elif defined(__APPLE__)
#include <dlfcn.h>
#include <libproc.h>
#include <sys/resource.h>
#endif

#include "access/table.h"
#include "access/xact.h"
#include "catalog/pg_type_d.h"
#include "executor/halo_pmc.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "port/atomics.h"
#include "storage/fd.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/varlena.h"

#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
#define PMC_DIRECT_READ
#endif

#if defined(__APPLE__) && defined(RUSAGE_INFO_V4)
/* XNU's optional THSC_CPI ABI (bsd/sys/resource_private.h). */
typedef int (*PmcThreadRead)(uint32 kind, void *data, size_t size);
typedef struct PmcThreadCounts
{
	uint64		instructions;
	uint64		cycles;
} PmcThreadCounts;
#endif

#define PMC_MAX_EVENTS 10

char	   *halo_pmc_list;
int			halo_pmc_sample_period;
#ifdef __APPLE__
bool		halo_pmc_use_process_reader;
#endif

typedef struct PmcSpec
{
	char		name[NAMEDATALEN];	/* dynahash string key */
	uint32		type;
	uint64		config;
	uint64		config1;
	bool		user_only;			/* called config2 in the course table */
} PmcSpec;

typedef struct PmcValue
{
	uint64		count;
	uint64		enabled;
	uint64		running;
} PmcValue;

typedef struct PmcEvent
{
	PmcSpec		spec;
	PmcValue	before;
	uint64		total;
	uint64		enabled;
	uint64		running;
	uint64		direct_reads;
	bool		valid;
#ifdef __linux__
	int			fd;
	struct perf_event_mmap_page *page;
#endif
} PmcEvent;

struct PmcScope
{
	MemoryContext context;
	MemoryContextCallback cleanup;
	int			nevents;
	int			period;
	uint64		samples;
	PmcEvent	events[PMC_MAX_EVENTS];
#ifdef __linux__
	Size		page_size;
#elif defined(__APPLE__) && defined(RUSAGE_INFO_V4)
	PmcThreadRead read_thread;
	uint64		boundary_reads;
#endif
};

static HTAB *dictionary;
static MemoryContext dictionary_context;
static Oid dictionary_role;
static Oid dictionary_rel;
static bool dictionary_stale;
static bool callbacks_registered;

/* A dictionary first seen inside a rolled-back transaction must not survive it. */
static void
pmc_xact(XactEvent event, void *unused)
{
	if (event == XACT_EVENT_ABORT || event == XACT_EVENT_PARALLEL_ABORT ||
		event == XACT_EVENT_PREPARE)
		dictionary_stale = true;
}

static void
pmc_subxact(SubXactEvent event, SubTransactionId subid,
			SubTransactionId parent, void *unused)
{
	if (event == SUBXACT_EVENT_ABORT_SUB)
		dictionary_stale = true;
}

static void
fold_name(char *name)
{
	for (; *name; name++)
		*name = pg_ascii_tolower((unsigned char) *name);
}

/* Reject malformed settings at SET time, before any query is instrumented. */
bool
PmcCheckList(char **newval, void **extra, GucSource source)
{
	char	   *copy = pstrdup(*newval);
	List	   *names = NIL;
	ListCell   *lc;
	bool		valid = SplitGUCList(copy, ',', &names) && list_length(names) <= PMC_MAX_EVENTS;

	foreach(lc, names)
	{
		const char *name = lfirst(lc);

		if (name[0] == '\0' || strlen(name) >= NAMEDATALEN)
			valid = false;
	}
	list_free(names);
	pfree(copy);
	if (!valid)
		GUC_check_errdetail("Use at most %d comma-separated names of 1 to %d bytes.",
							PMC_MAX_EVENTS, NAMEDATALEN - 1);
	return valid;
}

static int64
read_integer(HeapTuple tuple, TupleDesc desc, int attno)
{
	bool		isnull;
	Datum		value = SPI_getbinval(tuple, desc, attno, &isnull);
	Oid			type = TupleDescAttr(desc, attno - 1)->atttypid;

	if (!isnull && type == INT4OID)
		return DatumGetInt32(value);
	if (!isnull && type == INT8OID)
		return DatumGetInt64(value);
	elog(ERROR, "PMC dictionary column %d must contain a non-null integer", attno);
	return 0;
}

/* Called with an active transaction and snapshot. Publish only a complete cache. */
bool
PmcInit(void)
{
	MemoryContext context;
	HASHCTL		ctl;
	HTAB	   *table;
	Relation	rel;
	Oid			relid;
	Oid			role = GetUserId();
	uint64		i;
	const char *sql = "SELECT pcm_name, pmutype, pe_config, config1, config2"
					  " FROM ONLY public.pmc_name";

	rel = table_openrv_extended(makeRangeVar("public", "pmc_name", -1),
								AccessShareLock, true);
	if (rel == NULL)
	{
		elog(WARNING, "PMC dictionary public.pmc_name is missing");
		return false;
	}
	relid = RelationGetRelid(rel);
	/* A configuration dictionary contains plain data, never executable rules. */
	if (rel->rd_rel->relkind != RELKIND_RELATION || rel->rd_rel->relhasrules ||
		rel->rd_rel->relrowsecurity ||
		pg_class_aclcheck(relid, role, ACL_SELECT) != ACLCHECK_OK)
	{
		table_close(rel, AccessShareLock);
		elog(WARNING, "PMC dictionary must be a readable ordinary table without rules or row security");
		return false;
	}
	if (dictionary && !dictionary_stale && dictionary_role == role &&
		dictionary_rel == relid)
	{
		table_close(rel, AccessShareLock);
		return true;
	}

	if (!callbacks_registered)
	{
		RegisterXactCallback(pmc_xact, NULL);
		RegisterSubXactCallback(pmc_subxact, NULL);
		callbacks_registered = true;
	}
	context = AllocSetContextCreate(CurrentMemoryContext, "PMC dictionary",
									ALLOCSET_SMALL_SIZES);
	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = NAMEDATALEN;
	ctl.entrysize = sizeof(PmcSpec);
	ctl.hcxt = context;
	table = hash_create("PMC events", 32, &ctl, HASH_CONTEXT | HASH_ELEM | HASH_STRINGS);
	if (SPI_connect() != SPI_OK_CONNECT || SPI_execute(sql, true, 0) != SPI_OK_SELECT)
		elog(ERROR, "could not read the PMC dictionary");

	for (i = 0; i < SPI_processed; i++)
	{
		TupleDesc	desc = SPI_tuptable->tupdesc;
		HeapTuple	tuple = SPI_tuptable->vals[i];
		Oid			name_type = TupleDescAttr(desc, 0)->atttypid;
		char	   *name;
		bool		found;
		PmcSpec    *spec;
		int64		type = read_integer(tuple, desc, 2);
		int64		config = read_integer(tuple, desc, 3);
		int64		config1 = read_integer(tuple, desc, 4);
		int64		user_only = read_integer(tuple, desc, 5);

		if (name_type != TEXTOID && name_type != VARCHAROID)
			elog(ERROR, "PMC dictionary names must be text or varchar");
		name = SPI_getvalue(tuple, desc, 1);
		if (name == NULL || !name[0] || strlen(name) >= NAMEDATALEN ||
			type < 0 || type > PG_UINT32_MAX || config < 0 || config1 < 0 ||
			(user_only != 0 && user_only != 1))
			elog(ERROR, "invalid PMC dictionary entry in public.pmc_name");
		fold_name(name);
		spec = hash_search(table, name, HASH_ENTER, &found);
		if (found)
			elog(ERROR, "duplicate PMC dictionary name \"%s\"", name);
		spec->type = type;
		spec->config = config;
		spec->config1 = config1;
		spec->user_only = user_only != 0;
	}
	SPI_finish();
	table_close(rel, AccessShareLock);
	MemoryContextSetParent(context, TopMemoryContext);
	if (dictionary_context)
		MemoryContextDelete(dictionary_context);
	dictionary_context = context;
	dictionary = table;
	dictionary_role = role;
	dictionary_rel = relid;
	dictionary_stale = false;
	return true;
}

static void
pmc_cleanup(void *arg)
{
#ifdef __linux__
	PmcScope   *scope = arg;
	int			i;

	for (i = 0; i < scope->nevents; i++)
	{
		PmcEvent   *event = &scope->events[i];

		if (event->page)
			munmap(event->page, scope->page_size);
		if (event->fd >= 0)
		{
			close(event->fd);
			ReleaseExternalFD();
		}
	}
#endif
}

static bool
open_event(PmcScope *scope, PmcEvent *event)
{
#ifdef __linux__
	struct perf_event_attr attr = {0};

	attr.size = sizeof(attr);
	attr.type = event->spec.type;
	attr.config = event->spec.config;
	attr.config1 = event->spec.config1;
	attr.exclude_kernel = attr.exclude_hv = event->spec.user_only;
	/* A short interval cannot be scaled from unrelated multiplexing time. */
	attr.pinned = scope->period > 0;
	attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
	if (!AcquireExternalFD())
	{
		elog(WARNING, "PMC %s unavailable: file descriptor limit", event->spec.name);
		return false;
	}
#if defined(__aarch64__)
	/* Arm PMUv3 opt-in bit for EL0 reads of generic hardware events. */
	if (scope->period && attr.type == PERF_TYPE_HARDWARE && !attr.config1)
		attr.config1 = 2;
#endif
	event->fd = syscall(__NR_perf_event_open, &attr, 0, -1, -1, PERF_FLAG_FD_CLOEXEC);
#if defined(__aarch64__)
	if (event->fd < 0 && attr.config1 != event->spec.config1)
	{
		attr.config1 = event->spec.config1;
		event->fd = syscall(__NR_perf_event_open, &attr, 0, -1, -1, PERF_FLAG_FD_CLOEXEC);
	}
#endif
	if (event->fd < 0)
	{
		int			save_errno = errno;

		ReleaseExternalFD();
		elog(WARNING, "PMC %s unavailable: %s", event->spec.name, strerror(save_errno));
		return false;
	}
#ifdef PMC_DIRECT_READ
	if (scope->period && scope->page_size)
	{
		void	   *page = mmap(NULL, scope->page_size, PROT_READ, MAP_SHARED, event->fd, 0);

		if (page != MAP_FAILED)
			event->page = page;
	}
#endif
	return true;
#elif defined(__APPLE__) && defined(RUSAGE_INFO_V4)
	if (event->spec.type == 0 && event->spec.config <= 1 && event->spec.config1 == 0)
		return true;
	elog(WARNING, "PMC %s unavailable: macOS exposes only fixed cycles and instructions here",
		 event->spec.name);
#else
	elog(WARNING, "PMC %s unavailable on this platform", event->spec.name);
#endif
	return false;
}

PmcScope *
PmcOpen(MemoryContext parent)
{
	MemoryContext context;
	MemoryContext previous;
	PmcScope   *scope;
	List	   *names;
	ListCell   *lc;
	int			active = 0;

	if (!PmcInit())
		return NULL;
	context = AllocSetContextCreate(parent, "Hash PMC scope", ALLOCSET_SMALL_SIZES);
	previous = MemoryContextSwitchTo(context);
	scope = palloc0(sizeof(*scope));
	scope->context = context;
	scope->period = halo_pmc_sample_period;
#ifdef __linux__
	{
		long		page_size = sysconf(_SC_PAGESIZE);

		scope->page_size = page_size > 0 ? (Size) page_size : 0;
	}
#elif defined(__APPLE__) && defined(RUSAGE_INFO_V4)
	{
		PmcThreadCounts trial;

		/* Resolve once per build, outside every measured interval. */
		if (!halo_pmc_use_process_reader)
			scope->read_thread = (PmcThreadRead) dlsym(RTLD_DEFAULT, "thread_selfcounts");
		if (scope->read_thread &&
			(scope->read_thread(1, &trial, sizeof(trial)) != 0 || !trial.cycles || !trial.instructions))
			scope->read_thread = NULL;
	}
#endif
	scope->cleanup.func = pmc_cleanup;
	scope->cleanup.arg = scope;
	MemoryContextRegisterResetCallback(context, &scope->cleanup);

	if (!SplitGUCList(pstrdup(halo_pmc_list), ',', &names) ||
		list_length(names) > PMC_MAX_EVENTS)
		elog(ERROR, "halo_pmc_list requires a comma-separated list of at most %d names",
			 PMC_MAX_EVENTS);
	foreach(lc, names)
	{
		char	   *name = lfirst(lc);
		PmcSpec    *spec;
		PmcEvent   *event;
		int			i;

		fold_name(name);
		spec = strlen(name) < NAMEDATALEN ? hash_search(dictionary, name, HASH_FIND, NULL) : NULL;
		if (!spec)
		{
			elog(WARNING, "unknown PMC event \"%s\"", name);
			continue;
		}
		for (i = 0; i < scope->nevents; i++)
			if (strcmp(scope->events[i].spec.name, name) == 0)
				break;
		if (i < scope->nevents)
			continue;
		event = &scope->events[scope->nevents++];
		event->spec = *spec;
#ifdef __linux__
		event->fd = -1;
#endif
		event->valid = open_event(scope, event);
		if (event->valid)
			active++;
	}
	MemoryContextSwitchTo(previous);
	if (active == 0)
	{
		MemoryContextDelete(context);
		return NULL;
	}
	return scope;
}

#ifdef __linux__
static bool
read_event(PmcEvent *event, PmcValue *value)
{
	ssize_t		n;
#ifdef PMC_DIRECT_READ
	volatile struct perf_event_mmap_page *page = event->page;

	/* perf's seqlock protects the (slot, width, offset) tuple across migration. */
	if (page)
	{
		for (;;)
		{
			uint32		seq = page->lock;
			uint32		slot;
			uint16		width;
			uint64		bits;
			uint64		offset;

			pg_read_barrier();
			if (seq & 1)
				continue;
			slot = page->index;
			width = page->pmc_width;
			offset = page->offset;
			if (!page->cap_user_rdpmc || !slot || !width || width > 64)
				break;
#if defined(__x86_64__)
			{
				uint32		lo, hi;

				__asm__ volatile("lfence; rdpmc; lfence"
								 : "=a"(lo), "=d"(hi) : "c"(slot - 1) : "memory");
				bits = ((uint64) hi << 32) | lo;
			}
#else
			if (slot == 32)
				__asm__ volatile("isb; mrs %0, pmccntr_el0; isb" : "=r"(bits) :: "memory");
			else if (slot <= 31)
				__asm__ volatile("msr pmselr_el0, %1; isb; mrs %0, pmxevcntr_el0; isb"
								 : "=r"(bits) : "r"((uint64) slot - 1) : "memory");
			else
				break;			/* Unknown slot encoding: use read(2). */
#endif
			/* Sign-extend the hardware value to perf's software width. */
			value->count = offset + ((int64) (bits << (64 - width)) >> (64 - width));
			pg_read_barrier();
			if (page->lock == seq)
			{
				event->direct_reads++;
				return true;
			}
		}
	}
#endif
	do
		n = read(event->fd, value, sizeof(*value));
	while (n < 0 && errno == EINTR);
	return n == sizeof(*value) && value->running > 0;
}
#endif

static void
snapshot(PmcScope *scope, bool end)
{
	int			i;
#if defined(__APPLE__) && defined(RUSAGE_INFO_V4)
	PmcThreadCounts counts = {0};
	bool		available;

	/* Count calls at the read site; the availability probe is outside the interval. */
	scope->boundary_reads++;
	if (scope->read_thread)
		available = scope->read_thread(1, &counts, sizeof(counts)) == 0;
	else
	{
		struct rusage_info_v4 usage;

		available = proc_pid_rusage(MyProcPid, RUSAGE_INFO_V4,
									(rusage_info_t *) &usage) == 0;
		if (available)
		{
			counts.cycles = usage.ri_cycles;
			counts.instructions = usage.ri_instructions;
		}
	}
#endif

	for (i = 0; i < scope->nevents; i++)
	{
		PmcEvent   *event = &scope->events[i];
		PmcValue	value = {0};

		if (!event->valid)
			continue;
#ifdef __linux__
		event->valid = read_event(event, &value);
#elif defined(__APPLE__) && defined(RUSAGE_INFO_V4)
		event->valid = available && counts.cycles && counts.instructions;
		if (event->valid)
			value.count = event->spec.config == 0 ? counts.cycles : counts.instructions;
#endif
		if (!event->valid)
		{
			elog(WARNING, "PMC %s unavailable: counter could not be read or scheduled",
				 event->spec.name);
			continue;
		}
		if (!end)
			event->before = value;
		else
		{
			event->total += value.count - event->before.count;
			if (scope->period == 0)
			{
				event->enabled = value.enabled - event->before.enabled;
				event->running = value.running - event->before.running;
			}
		}
	}
}

void
PmcBegin(PmcScope *scope)
{
	snapshot(scope, false);
}

void
PmcEnd(PmcScope *scope)
{
	snapshot(scope, true);
	scope->samples++;
}

void
PmcReportAndClose(PmcScope *scope)
{
	int			i;

	elog(INFO, "HaloPMC target=%s period=%d samples=" UINT64_FORMAT,
		 scope->period ? "ExecHashGetHashValue" : "MultiExecPrivateHash.loop",
		 scope->period, scope->samples);
	for (i = 0; i < scope->nevents; i++)
	{
		PmcEvent   *event = &scope->events[i];

		if (!event->valid)
			continue;
#ifdef __linux__
		if (scope->period == 0)
		{
			if (event->running == 0)
			{
				elog(WARNING, "PMC %s unavailable: no time running in interval", event->spec.name);
				continue;
			}
			elog(INFO, "PMC %s raw=" UINT64_FORMAT " scaled=%.0f enabled_ns=" UINT64_FORMAT
				 " running_ns=" UINT64_FORMAT " source=perf",
				 event->spec.name, event->total,
				 (double) event->total * event->enabled / event->running,
				 event->enabled, event->running);
		}
		else
			elog(INFO, "PMC %s raw=" UINT64_FORMAT " direct_reads=" UINT64_FORMAT
				 " source=perf-pinned", event->spec.name, event->total, event->direct_reads);
#elif defined(__APPLE__) && defined(RUSAGE_INFO_V4)
		elog(INFO, "PMC %s raw=" UINT64_FORMAT " boundary_reads=" UINT64_FORMAT " source=%s(user+system)",
			 event->spec.name, event->total, scope->boundary_reads,
			 scope->read_thread ? "thread_selfcounts" : "proc_pid_rusage");
#endif
	}
	MemoryContextDelete(scope->context);
}

/* Hardware counter observations for the backend-private Hash build. */
#ifndef HALO_PMC_H
#define HALO_PMC_H

#include "utils/guc.h"

typedef struct PmcScope PmcScope;

extern char *halo_pmc_list;
extern int halo_pmc_sample_period;
#ifdef __APPLE__
extern bool halo_pmc_use_process_reader;
#endif

extern bool PmcCheckList(char **newval, void **extra, GucSource source);
extern bool PmcInit(void);
extern PmcScope *PmcOpen(MemoryContext parent);
extern void PmcBegin(PmcScope *scope);
extern void PmcEnd(PmcScope *scope);
extern void PmcReportAndClose(PmcScope *scope);

#endif

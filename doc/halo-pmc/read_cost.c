/* Build on macOS with cc -O2 -Wall read_cost.c -o read_cost. */
#include <dlfcn.h>
#include <libproc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

typedef int (*read_thread_fn)(uint32_t, void *, size_t);
struct counts { uint64_t instructions, cycles; };
enum { CALLS = 100000, ROUNDS = 24 };

static double measure_thread(read_thread_fn read_thread)
{
    struct counts first, last;
    if (read_thread(1, &first, sizeof(first)) != 0) exit(1);
    uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    for (int i = 0; i < CALLS; i++)
        if (read_thread(1, &last, sizeof(last)) != 0) exit(1);
    uint64_t elapsed = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start;
    if (last.instructions <= first.instructions || last.cycles <= first.cycles) exit(1);
    return (double)elapsed / CALLS;
}

static double measure_process(pid_t pid)
{
    struct rusage_info_v4 first, last;
    if (proc_pid_rusage(pid, RUSAGE_INFO_V4, (rusage_info_t *)&first) != 0) exit(1);
    uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    for (int i = 0; i < CALLS; i++)
        if (proc_pid_rusage(pid, RUSAGE_INFO_V4, (rusage_info_t *)&last) != 0) exit(1);
    uint64_t elapsed = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start;
    if (last.ri_instructions <= first.ri_instructions || last.ri_cycles <= first.ri_cycles) exit(1);
    return (double)elapsed / CALLS;
}

static int by_cost(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(void)
{
    read_thread_fn read_thread = (read_thread_fn)dlsym(RTLD_DEFAULT, "thread_selfcounts");
    pid_t pid = getpid();
    double thread[ROUNDS], process[ROUNDS];
    if (!read_thread) return 1;
    (void)measure_thread(read_thread);
    (void)measure_process(pid);
    printf("round calls_per_reader thread_ns_per_read process_ns_per_read\n");
    for (int r = 0; r < ROUNDS; r++) {
        if (r % 2 == 0) {
            thread[r] = measure_thread(read_thread);
            process[r] = measure_process(pid);
        } else {
            process[r] = measure_process(pid);
            thread[r] = measure_thread(read_thread);
        }
        printf("%d %d %.3f %.3f\n", r + 1, CALLS, thread[r], process[r]);
    }
    qsort(thread, ROUNDS, sizeof(double), by_cost);
    qsort(process, ROUNDS, sizeof(double), by_cost);
    printf("median_ns %.3f %.3f\n", (thread[ROUNDS/2 - 1] + thread[ROUNDS/2]) / 2,
           (process[ROUNDS/2 - 1] + process[ROUNDS/2]) / 2);
    return 0;
}

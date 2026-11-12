/* FreeBSD-native system-stat readers. Included in tclock.c's platform reader
 * section by freebsd-build.awk when building on FreeBSD. */
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/time.h>

#include <limits.h>

/* The kern.cp_time and kern.cp_times sysctls expose these five CPU states. */
enum { FBSD_CP_USER, FBSD_CP_NICE, FBSD_CP_SYS, FBSD_CP_INTR, FBSD_CP_IDLE,
       FBSD_CPUSTATES };

/* FreeBSD hides this BSD interface under strict _XOPEN_SOURCE builds. */
extern int getloadavg(double [], int);

static double cpu_busy_percent(unsigned long long total,
                               unsigned long long idle,
                               unsigned long long old_total,
                               unsigned long long old_idle)
{
    if (total <= old_total)
        return 0.0;
    unsigned long long dt = total - old_total;
    unsigned long long di = idle >= old_idle ? idle - old_idle : 0;
    unsigned long long busy = dt > di ? dt - di : 0;
    double pct = 100.0 * (double)busy / (double)dt;
    return pct > 100.0 ? 100.0 : pct;
}

static bool read_cpu(double *overall, double *per, int *nper)
{
    long aggregate[FBSD_CPUSTATES];
    size_t aggregate_len = sizeof(aggregate);
    if (sysctlbyname("kern.cp_time", aggregate, &aggregate_len, NULL, 0) != 0 ||
        aggregate_len < sizeof(aggregate))
        return false;

    size_t bytes = 0;
    if (sysctlbyname("kern.cp_times", NULL, &bytes, NULL, 0) != 0 ||
        bytes < sizeof(long) * FBSD_CPUSTATES)
        return false;
    long *counters = malloc(bytes);
    if (!counters)
        return false;
    if (sysctlbyname("kern.cp_times", counters, &bytes, NULL, 0) != 0) {
        free(counters);
        return false;
    }

    int n = (int)(bytes / (sizeof(long) * FBSD_CPUSTATES));
    if (n > MAX_CORES)
        n = MAX_CORES;
    if (n <= 0) {
        free(counters);
        return false;
    }

    unsigned long long total = 0, idle = 0;
    for (int s = 0; s < FBSD_CPUSTATES; s++) {
        unsigned long long value = (unsigned long long)aggregate[s];
        total += value;
        if (s == FBSD_CP_IDLE)
            idle = value;
    }
    *overall = g_have_prev
        ? cpu_busy_percent(total, idle, g_prev[0].tot, g_prev[0].idle) : 0.0;
    g_prev[0].tot = total;
    g_prev[0].idle = idle;

    for (int cpu = 0; cpu < n; cpu++) {
        unsigned long long cpu_total = 0, cpu_idle = 0;
        for (int s = 0; s < FBSD_CPUSTATES; s++) {
            unsigned long long value = (unsigned long long)
                counters[cpu * FBSD_CPUSTATES + s];
            cpu_total += value;
            if (s == FBSD_CP_IDLE)
                cpu_idle = value;
        }
        per[cpu] = g_have_prev
            ? cpu_busy_percent(cpu_total, cpu_idle,
                               g_prev[cpu + 1].tot, g_prev[cpu + 1].idle)
            : 0.0;
        g_prev[cpu + 1].tot = cpu_total;
        g_prev[cpu + 1].idle = cpu_idle;
    }
    free(counters);
    g_have_prev = true;
    *nper = n;
    return true;
}

static bool read_mem(mem_t *m)
{
    unsigned long long physical_bytes = 0;
    unsigned int page_size = 0;
    unsigned long free_pages = 0, inactive_pages = 0, cache_pages = 0;
    size_t len;

    len = sizeof(physical_bytes);
    if (sysctlbyname("hw.physmem", &physical_bytes, &len, NULL, 0) != 0 ||
        physical_bytes == 0)
        return false;
    len = sizeof(page_size);
    if (sysctlbyname("hw.pagesize", &page_size, &len, NULL, 0) != 0 ||
        page_size == 0)
        return false;

    memset(m, 0, sizeof(*m));
    m->total = (long)(physical_bytes / 1024);

    len = sizeof(free_pages);
    if (sysctlbyname("vm.stats.vm.v_free_count", &free_pages, &len,
                     NULL, 0) != 0)
        return false;
    len = sizeof(inactive_pages);
    (void)sysctlbyname("vm.stats.vm.v_inactive_count", &inactive_pages,
                       &len, NULL, 0);
    len = sizeof(cache_pages);
    (void)sysctlbyname("vm.stats.vm.v_cache_count", &cache_pages, &len,
                       NULL, 0);

    unsigned long long available_pages = (unsigned long long)free_pages +
        (unsigned long long)inactive_pages + (unsigned long long)cache_pages;
    unsigned long long available_kb =
        available_pages * (unsigned long long)page_size / 1024;
    if (available_kb > (unsigned long long)m->total)
        available_kb = (unsigned long long)m->total;
    m->avail = (long)available_kb;
    m->mfree = (long)((unsigned long long)free_pages * page_size / 1024);
    return m->total > 0;
}

static bool read_load(double ld[3], long *runnable, long *total)
{
    if (getloadavg(ld, 3) != 3)
        return false;
    /* FreeBSD's load-average API does not expose Linux-style task counts. */
    *runnable = 0;
    *total = 0;
    return true;
}

static bool read_uptime(long *uptime, int *procs)
{
    struct timeval boot;
    size_t len = sizeof(boot);
    if (sysctlbyname("kern.boottime", &boot, &len, NULL, 0) != 0)
        return false;

    time_t now = time(NULL);
    *uptime = now > boot.tv_sec ? (long)(now - boot.tv_sec) : 0;
    /* FreeBSD process enumeration requires private struct kinfo_proc details;
     * leave the optional process count unavailable rather than include headers
     * that conflict with strict POSIX feature-test settings. */
    *procs = 0;
    return true;
}

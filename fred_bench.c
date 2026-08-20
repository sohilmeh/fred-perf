#define _GNU_SOURCE 1
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <inttypes.h>
#include <stdbool.h>
#include <unistd.h>
#include <math.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <asm/unistd.h>
#include <sys/time.h>
#include <sys/mman.h>

// Read TSC counter
static inline __attribute__((always_inline)) uint64_t rdtsc_lfence(void)
{
    unsigned long eax, edx;
    __asm__ __volatile__("lfence ; rdtsc ; lfence"
			 : "=a" (eax), "=d" (edx));
    return ((uint64_t)edx << 32) + eax;
}

#define WITH_RDPMC 1

// Read a PMC counter
static inline __attribute__((always_inline)) uint64_t rdpmc(uint32_t ctr)
{
#if WITH_RDPMC
    unsigned long eax, edx;
    __asm__ __volatile__("rdpmc" : "=a" (eax), "=d" (edx) : "c" (ctr));
    return ((uint64_t)edx << 32) + eax;
#else
    return 0;
#endif
}

// Read value from (virtual) file
static int read_file_u64(const char *path, uint64_t *value)
{
    FILE *fp = fopen(path, "r");

    *value = -1;
    if (!fp)
	return -1;

    int ret = fscanf(fp, "%"SCNi64, value);
    int err = ferror(fp);
    fclose(fp);

    return (ret == 1 && !err) ? 0 : -1;
}

/* Write a value to a (virtual) file */
static int write_file_u64(const char *path, uint64_t value)
{
    FILE *fp = fopen(path, "w");
    if (!fp)
	return -1;

    int ret = fprintf(fp, "%"PRIu64, value);
    int err = ferror(fp);
    fclose(fp);

    return (ret > 0 && !err) ? 0 : -1;
}

static bool is_fred;

#define USE_SYSCALL __NR__sysctl	/* Invalid syscall -> sys_ni_syscall */

static bool check_fred(void)
{
    /* XXX: FRED check for i386? */
    const uint64_t magic = 0xfeedfacedeadbeef;
    long nr_ret;
    uint64_t rcx;

    nr_ret = USE_SYSCALL;
    rcx    = magic;
    asm volatile("syscall" : "+a" (nr_ret), "+c" (rcx) : : "r11");

    return rcx == magic;
}

struct cpuid {
    uint32_t eax, ecx, edx, ebx;
};

static inline __attribute__((const)) struct cpuid cpuid(uint32_t leaf, uint32_t subleaf)
{
    struct cpuid c;
    asm("cpuid"
	: "=a" (c.eax), "=c" (c.ecx), "=d" (c.edx), "=b" (c.ebx)
	: "a" (leaf), "c" (subleaf));
    return c;
}

static void print_system_info(double ratio, unsigned int ncalls)
{
    char buf[256];
    FILE *fp;

    unsigned int cpu_nr;
    getcpu(&cpu_nr, NULL);
    printf("\"CPU:\",%u\n", cpu_nr);

    uint32_t max_leaf = cpuid(0,0).eax;

    uint32_t fms = cpuid(1,0).eax;
    uint32_t f = (fms >> 8) & 15;
    if (f == 15)
	f += (fms >> 20) & 0xff;
    uint32_t m = ((fms >> 4) & 15) + ((fms >> 12) & 0xf0);
    uint32_t s = fms & 15;
    printf("\"FMS:\",\"%08x\",\"%02x\",\"%02x\",\"%02x\"\n", fms, f, m, s);

    uint32_t core_type = (max_leaf >= 0x1a) ? cpuid(0x1a,0).eax : 0;
    printf("\"Core:\",\"%02x\",\"%06x\"\n", core_type >> 24, core_type & 0xffffff);

    snprintf(buf, sizeof buf, "/sys/devices/system/cpu/cpu%u/microcode/version", cpu_nr);
    uint64_t ucode = 0;
    read_file_u64(buf, &ucode);
    printf("\"Ucode:\",\"%08"PRIx64"\"\n", ucode);

    double MHz = 0.0;
    if (max_leaf >= 0x15) {
	struct cpuid tsc = cpuid(0x15, 0);
	if (tsc.eax && tsc.ebx)
	  MHz = (double)tsc.ecx * tsc.ebx / (tsc.eax * 1.0e+6);
    }
    printf("\"TSC MHz:\",%.6f\n", MHz);
    printf("\"Core MHz:\",%.6f\n", MHz*ratio);
    printf("\"Calls/loop:\",%u\n", ncalls);

    printf("\"FRED:\",%s\n", (int)is_fred ? "TRUE" : "FALSE");

    buf[0] = '\0';
    if ((fp = fopen("/proc/version", "r"))) {
	char *p;
	fgets(buf, sizeof buf, fp);
	p = strchr(buf, '\n');
	if (p)
	    *p = '\0';
	fclose(fp);
    }
    printf("\"Kernel:\",\"%s\"\n", buf);
    putchar('\n');
}

#define WARM    10
#define CALLS   1000
#define DATAPTS 100

static inline unsigned long tvtous(struct timeval tv)
{
    return (tv.tv_sec * 1000000UL) + tv.tv_usec;
}

/* Burn CPU time to try to avoid P-state funnies */
static void burn_cpu(void)
{
    struct timeval tv;
    unsigned long t0, t1;
    gettimeofday(&tv, NULL);
    t0 = tvtous(tv);
    do {
	gettimeofday(&tv, NULL);
	t1 = tvtous(tv);
    } while (t1-t0 < 1000000UL);
}

static void before_readout(void)
{
    /* Delay here?? */
    asm volatile("serialize" : : : "memory");
}

static inline void test_syscall(void)
{
    long num_ret = USE_SYSCALL;
#ifdef __i386__
    /* Use the vdso, as applications are expected to. */
    extern long __attribute__((regparm(1))) __kernel_vsyscall(int nr);
    asm volatile("call *%1" : "+r" (num_ret) : "rm" (__kernel_vsyscall));
#else
    asm volatile("syscall" : "+a" (num_ret) : : "rcx", "r11");
#endif
}

/* Check to see if RDPMC is enabled, otherwise try to enable it */
static bool enable_rdpmc(void)
{
    static const char * const pmc_files[] = {
	"/sys/devices/cpu/rdpmc",
	"/sys/devices/cpu_core/rdpmc",
	"/sys/devices/cpu_atom/rdpmc",
	NULL
    };
    const char * const *pf;

    for (pf = pmc_files; *pf; pf++) {
	uint64_t pv;
	errno = 0;
	if (!read_file_u64(*pf, &pv) || errno != ENOENT) {
	    if (pv != 2) {
		write_file_u64(*pf, 2);
		read_file_u64(*pf, &pv);
		if (pv != 2)
		    return false; /* Failed to enable RDPMC */
	    }
	}
    }

    /* RDPMC is (hopefully) enabled if we get here */
    return true;
}

#define npoints 2
static int64_t d[DATAPTS][npoints];
static int64_t sum[npoints];
static int64_t sumsq[npoints];

int main(int argc, char *argv[])
{
    const int maxj = npoints - 1;
    bool have_rdpmc;

    is_fred = check_fred();

    if (argc > 1) {
	/* Lock to the speficied logical CPU */

	unsigned long cpu = strtoul(argv[1], NULL, 0);
	size_t ssz = cpu+1;
	cpu_set_t *cset = CPU_ALLOC(ssz);
	CPU_ZERO_S(ssz, cset);
	CPU_SET_S(cpu, ssz, cset);
	sched_setaffinity(0, ssz, cset);
	CPU_FREE(cset);
    }

    /* Try to get maximum realtime priority */
    {
	struct sched_param param;
	memset(&param, 0, sizeof param);
	param.sched_priority = sched_get_priority_max(SCHED_FIFO);
	sched_setscheduler(0, SCHED_FIFO, &param);
    }

    /* Try locking all of memory */
    mlockall(MCL_CURRENT | MCL_FUTURE);

    /* Try to enable RDPMC */
    have_rdpmc = enable_rdpmc();

#if 0
    FILE *f = fopen("/sys/kernel/syscall_trace/syscall", "w");
    if (f) {
	fprintf(f, "%u\n", USE_SYSCALL);
	fclose(f);
    }
#endif

    unsigned int pmc_bits = (cpuid(0x0a, 0).edx >> 5) & 0xff;

    burn_cpu();

    int64_t tscclocks_init = rdtsc_lfence();
    int64_t clocks_init    = have_rdpmc ? rdpmc(0x40000001) : 0;

    for (int i = 0; i < DATAPTS; i++) {
	uint64_t t[npoints+1];
	uint64_t last_t = rdtsc_lfence();

	for (int j = 0; j < WARM; j++) {
	    t[0] = last_t;
	    t[1] = rdtsc_lfence();
	    for (int k = 0; k < CALLS; k++) {
		test_syscall();
	    }
	    t[npoints] = last_t = rdtsc_lfence();
	}

	before_readout();

#if 0
	read_sysfs_u64("/sys/kernel/syscall_trace/tsc_t1", &t[2]);
//	read_sysfs_u64("/sys/kernel/syscall_trace/tsc_t2", &t[2]);
//	read_sysfs_u64("/sys/kernel/syscall_trace/tsc_t3", &t[3]);
	read_sysfs_u64("/sys/kernel/syscall_trace/tsc_t4", &t[3]);
#endif

	for (int j = 0; j <= maxj; j++) {
	    int64_t delta = t[j+1] - t[j];
	    d[i][j] = delta;
	    sum[j] += delta;
	    sumsq[j] += delta * delta;
	}
    }

    int64_t tscclocks_end = rdtsc_lfence();
    int64_t clocks_end    = have_rdpmc ? rdpmc(0x40000001) : 0;
    uint64_t pmc_wrap     = (uint64_t)2 << (pmc_bits-1);

    int64_t cpuclocks = clocks_end - clocks_init;
    if (cpuclocks < 0)
	cpuclocks += pmc_wrap;
    int64_t tscclocks = tscclocks_end - tscclocks_init;
    if (tscclocks < 0)
	tscclocks += pmc_wrap;

    double M[npoints], D[npoints];
    static const int64_t iscale[npoints] = { 1, CALLS };
    for (int j = 0; j <= maxj; j++) {
	double fscale = 1.0/(DATAPTS*iscale[j]);
	M[j] = sum[j] * fscale;
	D[j] = sqrt((DATAPTS*sumsq[j] - sum[j]*sum[j])) * fscale;
    }

    /* Output system information */
    print_system_info((double)cpuclocks/tscclocks, CALLS);

    /* Output the results in CSV format */
    for (int j = 0 ; j <= maxj; j++) {
	printf("\"T%d-T%d\"%c", j+1, j, (j == maxj) ? '\n' : ',');
    }

    /* Mean value */
    for (int j = 0; j <= maxj; j++) {
	printf("%.3f%c", M[j], j == maxj ? '\n' : ',');
    }

    /* Standard deviation */
    for (int j = 0; j <= maxj; j++) {
	printf("%.3f%c", D[j], j == maxj ? '\n' : ',');
    }

    /* Raw data points */
    for (int i = 0; i < DATAPTS; i++) {
	for (int j = 0; j <= maxj; j++) {
	    printf("%"PRId64"%c", d[i][j], j == maxj ? '\n' : ',');
	}
    }

    return 0;
}

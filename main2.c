#include <atomic.h>
#include <libc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iofunc.h>
#include <sys/neutrino.h>
#include <sys/resmgr.h>
#include <sys/syspage.h>
#include <unistd.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/debug.h>
#include <sys/procfs.h>
#include <sys/syspage.h>
#include <sys/neutrino.h>
#include <sys/time.h>
#include <time.h>
#include <fcntl.h>
#include <devctl.h>
#include <errno.h>
#include <dirent.h>

#define TRUE 0
#define FALSE 1

#define MAX_CPUS 32

#define LIGHT_LOAD 0
#define NORMAL_LOAD 1
#define HEAVY_LOAD 2
#define LOAD_PROPORTION_THRESHOLD 0.2

struct load_state_t {
	unsigned int task_ready_count;
	unsigned int task_sleep_count;
	debug_thread_t current_task;
	debug_thread_t min_sleep_task;
	unsigned long int totaltime;
} load_state_t;

struct cpu_loads_t {
	int total_time;
	int num_tasks;
} cpu_loads_t;

static struct load_state_t LoadStates[MAX_CPUS];
static struct cpu_loads_t CpuLoads[MAX_CPUS];

static int NumCpus = 0;
static int min_cpu, max_cpu, min_load, max_load;
static float ave_load;

void populate_loads_struct(int cpu, debug_thread_t thread, struct load_state_t loads[], int min_sleep_task_found) {
	if (thread.state == STATE_RUNNING) {
		loads[cpu].current_task = thread;
		printf("found running task\n");
	} else if (thread.state == STATE_READY) {
		loads[cpu].task_ready_count += 1;
		loads[cpu].totaltime += thread.sutime;
		printf("total time: %ld\n", loads[cpu].totaltime);
	} else if (thread.state != STATE_STOPPED && thread.state != STATE_DEAD) {
		loads[cpu].task_sleep_count += 1;
		if (min_sleep_task_found == FALSE) {
			LoadStates[cpu].min_sleep_task = thread;
			min_sleep_task_found = TRUE;
			printf("first min sleep task set\n");
		} else if (thread.sutime < loads[cpu].min_sleep_task.sutime) {
			loads[cpu].min_sleep_task = thread;
			printf("new min sleep task: %d\n", loads[cpu].min_sleep_task.sutime);
		}
	}
}

void sampleCpus(struct cpu_loads_t cpus[], int cpu, debug_thread_t thread) {
	if (thread.state == STATE_READY) {
		cpus[cpu].total_time += thread.sutime;
		cpus[cpu].num_tasks += 1;
	}
}

void find_min_max_cpus(struct cpu_loads_t cpuLoads[]) {
	float sum;
	min_load = 0;
	max_load = 0;
	min_cpu = 0;
	max_cpu = 0;

	for (int i=0; i<NumCpus; i++) {
		sum += cpuLoads[i].total_time;
	}

	ave_load = sum / NumCpus;

	for (int i=0; i<NumCpus; i++) {
		if (cpuLoads[i].total_time < min_load) {
			min_load = cpuLoads[i].total_time;
			min_cpu = i;
		}
		if (cpuLoads[i].total_time > max_load) {
			max_load = cpuLoads[i].total_time;
			max_cpu = i;
		}
	}
}

int get_load_state(int processor_load, int avg_processor_load) {
	if (processor_load > avg_processor_load * (1-LOAD_PROPORTION_THRESHOLD)) {
		return HEAVY_LOAD;
	} else if (avg_processor_load * (1+LOAD_PROPORTION_THRESHOLD) >= processor_load && processor_load >= avg_processor_load * (1-LOAD_PROPORTION_THRESHOLD)) {
		return NORMAL_LOAD;
	} else if (processor_load < avg_processor_load * (1-LOAD_PROPORTION_THRESHOLD)) {
		return LIGHT_LOAD;
	} else {
		return -1; // something went wrong
	}
}

int partA() {
	DIR			*dir;
	char		fname[PATH_MAX];

	dir = opendir("/proc");

	if (dir == NULL) {
		fprintf(stderr, "Unable to open %s\n", fname);
		return 0;
	} else {
		struct dirent * dirent;
		/* Reset values */
		debug_process_t procinfo;
		for (int i=0; i<NumCpus; i++) {
			LoadStates[i].task_ready_count = 0;
			LoadStates[i].task_sleep_count = 0;
			LoadStates[i].totaltime = 0;

			CpuLoads[i].total_time = 0;
			CpuLoads[i].num_tasks = 0;
		}
		int min_sleep_task_found = FALSE;

		while ((dirent = readdir(dir)) != NULL) {
			memset(&procinfo, 0, sizeof(procinfo));
			if (isdigit(dirent->d_name[0])) {
				int pid = atoi(dirent->d_name);
				int fd;
				char buff[512];
				int procstatus;


				snprintf(buff, sizeof(buff), "/proc/%d/as", pid);
				printf("pid:%d\n", pid);

				if ((fd = open(buff, O_RDONLY)) == -1) {
					return 0;
				}

				if ((procstatus = devctl(fd, DCMD_PROC_INFO, &procinfo, sizeof procinfo, 0)) != -1) {
					int lasttid, tid, cpu;
					printf("proc_status=%d\n", procstatus);
					printf("num_threads=%d\n", procinfo.num_threads);
					if (procinfo.flags & _NTO_PF_ZOMBIE) {
						close(fd);
						continue;
					} else {
						for (lasttid = tid = 1; ; lasttid = ++tid) {
							int status;
							debug_thread_t threadinfo;
							memset(&threadinfo, 0, sizeof(threadinfo));
							threadinfo.tid = tid;
							if ((status = devctl(fd, DCMD_PROC_TIDSTATUS, &threadinfo, sizeof(threadinfo), 0)) != EOK) {
								printf("status=%d\n", status);
								break;
							}
							tid = threadinfo.tid;
							if (tid < lasttid) {
								break;
							}

							// Populate load_state_t array
							cpu = threadinfo.last_cpu;
							printf("CPU: %d\n", cpu);

							// Part A
							populate_loads_struct(cpu, threadinfo, LoadStates, min_sleep_task_found);

							// Part B
							sampleCpus(CpuLoads, cpu, threadinfo);
							find_min_max_cpus(CpuLoads);
							int load_state_min_core = get_load_state(min_load, ave_load);
							int load_state_max_core = get_load_state(max_load, ave_load);

							if (load_state_max_core != HEAVY_LOAD || load_state_max_core != LIGHT_LOAD) {
								// no migration needed
							}

							// Part B part 5 to check if migration needed
						}
					}
				}
				close(fd);
			}
		}
		closedir(dir);
	}
	return 1;
}

int init_cpu( void ) {
	int i;
	int ProcFd = -1;
	debug_thread_t debug_data;

	memset( &debug_data, 0, sizeof( debug_data ) );

	/*
	* Open a connection to proc to talk over.
	*/
	ProcFd = open( "/proc/1/as", O_RDONLY );
	if( ProcFd == -1 ) {
		fprintf( stderr, "pload: Unable to access procnto: %s\n",
		strerror( errno ) );
		fflush( stderr );
		return -1;
	}

	i = fcntl(ProcFd,F_GETFD);

	if(i != -1) {
		i |= FD_CLOEXEC;

		if(fcntl(ProcFd,F_SETFD,i) != -1) {
			/* Grab this value */
			NumCpus = _syspage_ptr->num_cpu;

			partA();

			return(EOK);
		}

	}

	close(ProcFd);
	return(-1);
}


int main(int argc, char* argv[]) {

	init_cpu();
	printf("System has: %d CPUs\n", NumCpus);

	while(1) {
		sleep(1);
		partA();
	}
}

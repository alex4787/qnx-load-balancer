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
#include <sys/trace.h>
#include <sys/kercalls.h>
#include <sys/procmgr.h>

#define TRUE 0
#define FALSE 1

#define MAX_CPUS 32

#define LIGHT_LOAD 0
#define NORMAL_LOAD 1
#define HEAVY_LOAD 2
#define LOAD_PROPORTION_THRESHOLD 0.2

struct load_state_t {
	debug_thread_t *min_task;
	uint64_t totaltime;
} load_state_t;

static struct load_state_t LoadStates[MAX_CPUS];

static int NumCpus = 0;
static int MinCpu, MaxCpu, MinLoad, MaxLoad;
static float AveLoad;

void populate_load_state(int cpu, debug_thread_t thread) {
	struct load_state_t *cur_load_state = LoadStates + cpu;

	if (cur_load_state->min_task == NULL) {
		cur_load_state->min_task = malloc(sizeof(load_state_t));
		memcpy(cur_load_state->min_task, &thread, sizeof(load_state_t));
		printf("new min_task for cpu=%d: pid=%d tid=%d\n", cpu, thread.pid,
				thread.tid);
	} else if (thread.sutime < cur_load_state->min_task->sutime) {
		memcpy(cur_load_state->min_task, &thread, sizeof(load_state_t));
		printf("new min_task for cpu=%d: pid=%d tid=%d\n", cpu, thread.pid,
				thread.tid);
	}

	if (thread.state == STATE_READY) {
		cur_load_state->totaltime += thread.sutime;

		printf("total time: %ld\n", cur_load_state->totaltime);
	}
}

void calculate_cpu_loads() {
	float sum;

	MinLoad = -1;
	MaxLoad = -1;
	MinCpu = -1;
	MaxCpu = -1;
	AveLoad = -1;

	for (int i = 0; i < NumCpus; i++) {
		uint64_t totaltime = LoadStates[i].totaltime;

		sum += totaltime;

		if (MinLoad == -1 || totaltime < MinLoad) {
			MinLoad = totaltime;
			MinCpu = i;
		}
		if (MaxLoad == -1 || totaltime > MaxLoad) {
			MaxLoad = totaltime;
			MaxCpu = i;
		}
	}

	AveLoad = sum / NumCpus;
}

int get_load_state(int processor_load, int avg_processor_load) {
	if (processor_load > avg_processor_load * (1 - LOAD_PROPORTION_THRESHOLD)) {
		return HEAVY_LOAD;
	} else if (avg_processor_load * (1 + LOAD_PROPORTION_THRESHOLD)
			>= processor_load
			&& processor_load
					>= avg_processor_load * (1 - LOAD_PROPORTION_THRESHOLD)) {
		return NORMAL_LOAD;
	} else if (processor_load
			< avg_processor_load * (1 - LOAD_PROPORTION_THRESHOLD)) {
		return LIGHT_LOAD;
	} else {
		return -1; // something went wrong
	}
}

int run_load_balancer() {
	DIR *dir;
	char fname[PATH_MAX];

	dir = opendir("/proc");

	if (dir == NULL) {
		fprintf(stderr, "Unable to open %s\n", fname);
		return -1;
	}

	struct dirent *dirent;
	debug_process_t procinfo;

	// Reset values
	for (int i = 0; i < NumCpus; i++) {
		LoadStates[i].min_task = NULL;
		LoadStates[i].totaltime = 0;
	}

	// Loop through all processes (all subdirectories of /proc)
	while ((dirent = readdir(dir)) != NULL) {
		memset(&procinfo, 0, sizeof(procinfo));
		if (isdigit(dirent->d_name[0])) {
			int pid = atoi(dirent->d_name);

			// Skip procnto
			if (pid == 1) {
				continue;
			}

			int fd;
			char buff[512];
			int procstatus;

			snprintf(buff, sizeof(buff), "/proc/%d/as", pid);

			if ((fd = open(buff, O_RDONLY)) == -1) {
				return -1;
			}

			// Get process info
			if ((procstatus = devctl(fd, DCMD_PROC_INFO, &procinfo,
					sizeof procinfo, 0)) != -1) {
				int lasttid, tid, cpu;

				printf("\npid=%d [proc_status=%d num_threads=%d]\n", pid,
						procstatus, procinfo.num_threads);

				if (procinfo.flags & _NTO_PF_ZOMBIE) {
					close(fd);
					continue;
				} else {
					// Loop through all threads in process
					for (lasttid = tid = 1;; lasttid = ++tid) {
						int status;
						procfs_status threadinfo;
						threadinfo.tid = tid;
						// Get thread info
						if ((status = devctl(fd, DCMD_PROC_TIDSTATUS,
								&threadinfo, sizeof(threadinfo), NULL)) != EOK) {
							printf("error status=%d\n", status);
							break;
						}
						tid = threadinfo.tid;
						if (tid < lasttid) {
							break;
						}

						cpu = threadinfo.last_cpu;
						printf("\ttid=%d cpu=%d\n", tid, cpu);

						// Part A
						// Populate load_state_t array
						populate_load_state(cpu, threadinfo);
					}
				}
			}
			close(fd);
		}
	}
	// Finished getting all thread information
	closedir(dir);

	// Part B
	// could refactor to use total_time from LoadStates instead of sampleCpus

	calculate_cpu_loads();
	int load_state_min_core = get_load_state(MinLoad, AveLoad);
	int load_state_max_core = get_load_state(MaxLoad, AveLoad);

	// Part B4
	if (load_state_max_core != HEAVY_LOAD || load_state_min_core != LIGHT_LOAD) {
		printf("no migration needed 1\n");
		return 0;
	}

	// Part B5
	debug_thread_t min_task = *LoadStates[MaxCpu].min_task;
	if (MinLoad + min_task.sutime >= MaxLoad - min_task.sutime) {
		printf("no migration needed 2\n");
		return 0;
	}

	// Migrate here
	printf("migration needed\n");
	int slay_pid;
	char slay_buf[128];
	char min_task_pid[128];
	sprintf(min_task_pid, "%d", min_task.pid);
	char min_cpu[128];
	sprintf(min_cpu, "%d", MinCpu);
	char min_task_tid[128];
	sprintf(min_task_tid, "%d", min_task.tid);

	slay_pid = spawnlp(P_WAIT, "slay", "slay", "-C", min_cpu, "-T", min_task_tid, min_task_pid, NULL);

	printf("after slay\n");

	if (slay_pid == -1) {
		printf("Unable to execute slay (%s)", strerror(errno));
		return -1;
	}


	return 1;
}

int init_cpu(void) {
	int i;
	int ProcFd = -1;
	debug_thread_t debug_data;

	memset(&debug_data, 0, sizeof(debug_data));

	/*
	 * Open a connection to proc to talk over.
	 */
	ProcFd = open("/proc/1/as", O_RDONLY);
	if (ProcFd == -1) {
		fprintf( stderr, "pload: Unable to access procnto: %s\n",
				strerror( errno));
		fflush( stderr);
		return -1;
	}

	i = fcntl(ProcFd, F_GETFD);

	if (i != -1) {
		i |= FD_CLOEXEC;

		if (fcntl(ProcFd, F_SETFD, i) != -1) {
			/* Grab this value */
			NumCpus = _syspage_ptr->num_cpu;
			printf("System has: %d CPUs\n", NumCpus);

			return (EOK);
		}
	}

	close(ProcFd);
	return (-1);
}

/* int do_something(event_data_t* e_d) { */
/*	 printf("Hello\n"); */
/*	 return 0; */
/* } */

int main(int argc, char *argv[]) {

	if (init_cpu() == -1) {
		perror("init_cpu() failed\n");
		return -1;
	}

	// Meant to add event handler for when thread created
	/* procmgr_ability(0, */
	/*	 PROCMGR_ADN_ROOT|PROCMGR_AOP_ALLOW|PROCMGR_AID_TRACE, */
	/*	 PROCMGR_ADN_ROOT|PROCMGR_AOP_ALLOW|PROCMGR_AID_IO, */
	/*	 PROCMGR_ADN_NONROOT|PROCMGR_AOP_ALLOW|PROCMGR_AID_TRACE, */
	/*	 PROCMGR_ADN_NONROOT|PROCMGR_AOP_ALLOW|PROCMGR_AID_IO, */
	/*	 PROCMGR_AID_EOL); */

	/* TraceEvent(_NTO_TRACE_DELALLCLASSES); */
	/* TraceEvent(_NTO_TRACE_CLRCLASSPID, _NTO_TRACE_KERCALL); */
	/* TraceEvent(_NTO_TRACE_CLRCLASSTID, _NTO_TRACE_KERCALL); */
	/* TraceEvent(_NTO_TRACE_CLRCLASSPID, _NTO_TRACE_THREAD); */
	/* TraceEvent(_NTO_TRACE_CLRCLASSTID, _NTO_TRACE_THREAD); */

	/* TraceEvent(_NTO_TRACE_SETALLCLASSESFAST); */

	/* if (ThreadCtl(_NTO_TCTL_IO, 0)!=EOK) { */
	/*	 (void) fprintf(stderr, "argv[0]: Failed to obtain I/O privileges\n"); */

	/*	 return (-1); */
	/* } */

	/* TraceEvent(_NTO_TRACE_ADDEVENT, _NTO_TRACE_THREAD, _NTO_TRACE_THCREATE); */
	/* TraceEvent(_NTO_TRACE_ADDEVENTHANDLER, _NTO_TRACE_THREAD, */
	/*	 _NTO_TRACE_THCREATE, do_something, NULL); */
	/* TraceEvent(_NTO_TRACE_START); */

	/* sleep(10); */

	/* TraceEvent(_NTO_TRACE_STOP); */
	/* TraceEvent(_NTO_TRACE_FLUSHBUFFER); */
	/* TraceEvent(_NTO_TRACE_DELEVENTHANDLER, _NTO_TRACE_THREAD, _NTO_TRACE_THCREATE); */

	while (1) {
		run_load_balancer();
		printf("==================================================\n\n");
		sleep(5);
	}
}

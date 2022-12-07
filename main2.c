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
#include <sys/resource.h>

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
// store the cpu with the lightest and heaviest loads, as well as the value of these loads
static int MinCpu, MaxCpu, MinLoad, MaxLoad;
// average load across the entire system
static float AveLoad;
// task with the smallest load, from the cpu with the heaviest load
static debug_thread_t min_task;

static DIR *dir;

static unsigned    *rmaskp, *imaskp;
static void		   *my_runmask;

struct arg_struct {
    int value;
    unsigned int cpu;
};


// CORE FUNCTIONALITY
void populate_load_state(int, debug_thread_t);
int get_load_state(int, int);
void calculate_cpu_loads();
int perform_migration(pid_t, int, int);
int transfer_task(pid_t, int);
int run_load_balancer();
int init_cpu(void);
void init_runmask(void **);
void access_runmask(void *);
void set_runmask(void *);
void set_runmask_ext(pid_t, int, void*);

// TEST SUITE
void *run_task(void *);

void load_balance_test();
void performance_test();
void utilization_test();


int main() {
	// Initialize number of CPUs
	NumCpus = _syspage_ptr->num_cpu;

	// Open /proc directory
	dir = opendir("/proc");

	if (dir == NULL) {
		fprintf(stderr, "Unable to open /proc\n");
		return -1;
	}

	// Initialize runmask and necessary permissions
	procmgr_ability(0,
		PROCMGR_ADN_NONROOT|PROCMGR_AOP_ALLOW|PROCMGR_AID_XPROCESS_DEBUG,
		PROCMGR_AID_EOL);

	struct rlimit rlp;

	getrlimit(RLIMIT_NOFILE, &rlp);
	rlp.rlim_cur = 9000;
	rlp.rlim_max = 9000;
	setrlimit(RLIMIT_NOFILE, &rlp);

	init_runmask(&my_runmask);
	RMSK_SET(0, rmaskp);
	RMSK_CLR(0, imaskp);
	set_runmask(my_runmask);

	// Tests
//	load_balance_test();
//	performance_test();
	utilization_test();

	// Finished getting all thread information
	closedir(dir);

	return 0;
}

// first part of the load monitoring phase of the algorithm
void populate_load_state(int cpu, debug_thread_t thread) {
	struct load_state_t *cur_load_state = LoadStates + cpu;

	if (cur_load_state->min_task == NULL) {
		cur_load_state->min_task = malloc(sizeof(load_state_t));
		memcpy(cur_load_state->min_task, &thread, sizeof(load_state_t));
//		printf("new min_task for cpu=%d: pid=%d tid=%d\n", cpu, thread.pid,
//				thread.tid);
	} else if (thread.sutime < cur_load_state->min_task->sutime) {
		memcpy(cur_load_state->min_task, &thread, sizeof(load_state_t));
//		printf("new min_task for cpu=%d: pid=%d tid=%d\n", cpu, thread.pid,
//				thread.tid);
	}

	LoadStates[cpu].totaltime += thread.sutime;

//	printf("total time: %ld\n", cur_load_state->totaltime);
}

// second part of the load monitoring phase of the algorithm
// this function corresponds to equation 1 in the project report
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
		return -1;
	}
}

// finds the cpus with the lightest and heaviest loads, and what these load values are
// calculates the average load across the entire system
void calculate_cpu_loads() {
	float sum;

	MinLoad = -1;
	MaxLoad = -1;
	MinCpu = -1;
	MaxCpu = -1;
	AveLoad = -1;

	for (int i = 1; i < NumCpus; i++) {
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

	AveLoad = sum / (NumCpus - 1);
}

// load migration part of the algorithm
int perform_migration(pid_t min_task_pid, int min_task_tid, int MinCpu) {
	void *runmask;
	init_runmask(&runmask);

	RMSK_SET(MinCpu, rmaskp);

	if (min_task_pid == getpid()) {
		min_task_pid = 0;
	}

	printf("Moving [pid=%d tid=%d] to CPU %d\n", min_task_pid, min_task_tid, MinCpu);

	set_runmask_ext(min_task_pid, min_task_tid, runmask);

	return 0;
}

int run_load_balancer() {
	return transfer_task(-1, -1);
}

int transfer_task(pid_t transfer_task_pid, int transfer_task_tid) {
	rewinddir(dir);

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

			// Skip all other
			if (pid == 1) {
				continue;
			}


			int fd;
			char buff[512];
			int procstatus;

			snprintf(buff, sizeof(buff), "/proc/%d/as", pid);

			if ((fd = open(buff, O_RDONLY)) == -1) {
				perror("open()");
				return -1;
			}

			// Get process info
			if ((procstatus = devctl(fd, DCMD_PROC_INFO, &procinfo,
					sizeof procinfo, 0)) != -1) {
				int lasttid, tid, cpu;
//				void *runmask;

				if (pid == getpid()) {
//					printf("\npid=%d [proc_status=%d num_threads=%d]\n", pid,
//							procstatus, procinfo.num_threads);
				} else {
					continue;
//					init_runmask(&runmask);
//					RMSK_SET(0, rmaskp);
//					RMSK_SET(0, imaskp);
				}

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
//							printf("error status=%d\n", status);
							break;
						}
						tid = threadinfo.tid;
						if (tid < lasttid) {
							break;
						}

						cpu = threadinfo.last_cpu;

						if (pid != getpid()) {
//							if (cpu != 0 && threadinfo.state)
//								set_runmask_ext(pid, tid, runmask);
						} else if (tid != gettid()) {
//							printf("\ttid=%d cpu=%d state=%d\n", tid, cpu, threadinfo.state);

							// Part A
							// Populate load_state_t array
							populate_load_state(cpu, threadinfo);

//							printf("\t\ttotaltime=%ld\n", LoadStates[cpu].totaltime);
						}
					}
				}
			}
			if (close(fd) == -1) {
				perror("close()");
			}
		}
	}

	// Part B
	// could refactor to use total_time from LoadStates instead of sampleCpus

	calculate_cpu_loads();
	int load_state_min_core = get_load_state(MinLoad, AveLoad);
	int load_state_max_core = get_load_state(MaxLoad, AveLoad);

	if (transfer_task_pid == -1 && transfer_task_tid == -1) {
		// Part B4
		if (load_state_max_core != HEAVY_LOAD || load_state_min_core != LIGHT_LOAD) {
			printf("no migration needed 1\n");
			return 0;
		}

		// Part B5
		min_task = *LoadStates[MaxCpu].min_task;
		if (MinLoad + min_task.sutime >= MaxLoad - min_task.sutime) {
			printf("no migration needed 2\n");
			return 0;
		}

		transfer_task_pid = min_task.pid;
		transfer_task_tid = min_task.tid;
	}

	// a task migration is performed to move the min_task (from the heaviest loaded core) to the core with the lightest load
	perform_migration(transfer_task_pid, transfer_task_tid, MinCpu);

	return 1;
}

void init_runmask(void **runmask) {
	int         *rsizep, size;
	unsigned    num_elements = 0;
	int 		masksize_bytes;

	/* Determine the number of array elements required to hold
	 * the runmasks, based on the number of CPUs in the system. */
	num_elements = RMSK_SIZE(_syspage_ptr->num_cpu);

	/* Determine the size of the runmask, in bytes. */
	masksize_bytes = num_elements * sizeof(unsigned);

	/* Allocate memory for the data structure that we'll pass
	 * to ThreadCtl(). We need space for an integer (the number
	 * of elements in each mask array) and the two masks
	 * (runmask and inherit mask). */

	size = sizeof(int) + 2 * masksize_bytes;
	if ((*runmask = malloc(size)) == NULL) {
		exit(-1);
	} else {
		memset(*runmask, 0x00, size);

		/* Set up pointers to the "members" of the structure. */
		rsizep = (int *)*runmask;
		rmaskp = rsizep + 1;
		imaskp = rmaskp + num_elements;

		/* Set the size. */
		*rsizep = num_elements;
	}
}

void access_runmask(void *runmask) {
	int         *rsizep, size;
	unsigned    num_elements = 0;
	int 		masksize_bytes;

	/* Determine the number of array elements required to hold
	 * the runmasks, based on the number of CPUs in the system. */
	num_elements = RMSK_SIZE(_syspage_ptr->num_cpu);

	/* Determine the size of the runmask, in bytes. */
	masksize_bytes = num_elements * sizeof(unsigned);

	/* Allocate memory for the data structure that we'll pass
	 * to ThreadCtl(). We need space for an integer (the number
	 * of elements in each mask array) and the two masks
	 * (runmask and inherit mask). */

	size = sizeof(int) + 2 * masksize_bytes;

	/* Set up pointers to the "members" of the structure. */
	rsizep = (int *)runmask;
	rmaskp = rsizep + 1;
	imaskp = rmaskp + num_elements;
}

void set_runmask(void *runmask) {
	if (ThreadCtl(_NTO_TCTL_RUNMASK_GET_AND_SET_INHERIT, runmask) == -1) {
		perror ("ThreadCtl()");
		exit(-1);
	}
}

void set_runmask_ext(pid_t pid, int tid, void *runmask) {
	if (ThreadCtlExt(pid, tid, _NTO_TCTL_RUNMASK_GET_AND_SET_INHERIT, runmask) == -1) {
		perror ("ThreadCtlExt()");
		exit(-1);
	}
}

// ============================== TEST SUITE ==============================

void *run_task(void *_args) {
	struct arg_struct *args = (struct arg_struct *)_args;
	int cpu = args->cpu;

	void *runmask;
	init_runmask(&runmask);
	RMSK_SET(cpu, rmaskp);
	set_runmask(runmask);

	int msec = args->value;
	struct timespec when;

	printf("[%d] Sleeping for %d milliseconds\n", gettid(), msec);

	when.tv_sec = 0;
	when.tv_nsec = msec * 1000000;
	nanospin(&when);

	printf("[%d] Done after %d milliseconds\n", gettid(), msec);
}

void load_balance_test() {
	pthread_t threads[6];
	pthread_attr_t thread_attrs[6];
	pid_t pid = getpid();
	struct arg_struct arg_structs[6];
	int tid;

	printf("pid=%d\n", pid);

	for (int i = 0; i < 6; i++) {
		pthread_attr_init(thread_attrs + i);
		thread_attrs[i].__param.__sched_priority = 255;
	}

	arg_structs[0].value = 1000;
	arg_structs[0].cpu = 1;
	pthread_create(threads + 0, thread_attrs + 0, run_task, (void *) (arg_structs));

	arg_structs[1].value = 500;
	arg_structs[1].cpu = 2;
	pthread_create(threads + 1, thread_attrs + 1, run_task, (void *) (arg_structs + 1));

	arg_structs[2].value = 500;
	arg_structs[2].cpu = 3;
	pthread_create(threads + 2, thread_attrs + 2, run_task, (void *) (arg_structs + 2));
	transfer_task(pid, threads[2]);

	arg_structs[3].value = 500;
	arg_structs[3].cpu = 1;
	pthread_create(threads + 3, thread_attrs + 3, run_task, (void *) (arg_structs + 3));
	transfer_task(pid, threads[3]);

	arg_structs[4].value = 500;
	arg_structs[4].cpu = 1;
	pthread_create(threads + 4, thread_attrs + 4, run_task, (void *) (arg_structs + 4));
	transfer_task(pid, threads[4]);

	arg_structs[5].value = 500;
	arg_structs[5].cpu = 1;
	pthread_create(threads + 5, thread_attrs + 5, run_task, (void *) (arg_structs + 5));

	pthread_cancel(threads[2]);
	run_load_balancer();
}

void performance_test() {
	long max_time = 0;
	long total_time = 0;
	long avg_time = 0;
	long time;
	struct timespec start, stop;

	int trials = 50;

	for (int i = 0; i < trials; i++) {
		clock_gettime( CLOCK_PROCESS_CPUTIME_ID, &start);
		run_load_balancer();
		clock_gettime( CLOCK_PROCESS_CPUTIME_ID, &stop);

		time = ((stop.tv_sec - start.tv_sec) * 1000000) + (stop.tv_nsec - start.tv_nsec);

		if (time > max_time) {
			max_time = time;
		}

		total_time += time;

		printf("TOOK %ld nanoseconds\n\n", time);

		if ((i + 1) % 10 == 0) {
			avg_time = (long) ((double) total_time / (double) (i+1));
			printf("====== %d tests: total=%lu average=%lu max=%lu\n\n", (i+1), total_time, avg_time, max_time);
		}
	}

	total_time = 0;
	max_time = 0;

	pid_t pid = getpid();
	int tid = gettid();

	for (int i = 0; i < trials; i++) {
		clock_gettime( CLOCK_PROCESS_CPUTIME_ID, &start);
		transfer_task(pid, tid);
		clock_gettime( CLOCK_PROCESS_CPUTIME_ID, &stop);

		time = ((stop.tv_sec - start.tv_sec) * 1000000) + (stop.tv_nsec - start.tv_nsec);

		if (time > max_time) {
			max_time = time;
		}

		total_time += time;

		printf("TOOK %ld nanoseconds\n\n", time);

		if ((i + 1) % 10 == 0) {
			avg_time = (long) ((double) total_time / (double) (i+1));
			printf("====== %d tests: total=%lu average=%lu max=%lu\n\n", (i+1), total_time, avg_time, max_time);
		}
	}
}

// function to multiply two matrices
void *multiplyMatrices(void *_args) {
	struct arg_struct *args = (struct arg_struct *)_args;
	int cpu = args->cpu;

	void *runmask;
	init_runmask(&runmask);
	RMSK_SET(cpu, rmaskp);
	set_runmask(runmask);

	// dividing size of matrix into 4 sub-blocks
	int x = args->value / 4;

	int first[x][x];
	int second[x][x];
	int result[x][x];

	for (int i = 0; i < x; ++i) {
		for (int j = 0; j < x; ++j) {
			first[i][j] = i+j;
			second[i][j] = i+j;
		}
	}

   // Initializing elements of matrix mult to 0.
   for (int i = 0; i < x; ++i) {
      for (int j = 0; j < x; ++j) {
         result[i][j] = 0;
      }
   }

   // Multiplying first and second matrices and storing it in result
   for (int i = 0; i < x; ++i) {
      for (int j = 0; j < x; ++j) {
         for (int k = 0; k < x; ++k) {
            result[i][j] += first[i][k] * second[k][j];
         }
      }
   }
}

void utilization_test() {
	long time;
	struct timespec start, stop;

	pthread_t threads[24];
	pthread_attr_t thread_attrs[24];

	struct arg_struct args;

	for (int i = 0; i < 24; i++) {
		pthread_attr_init(thread_attrs + i);
		thread_attrs[i].__param.__sched_priority = 15;
	}

	for (int x = 32; x <= 256; x *= 2) {
		printf("%d\n", x);

		clock_gettime( CLOCK_PROCESS_CPUTIME_ID, &start);

		// add 10 tasks running matrix multiplication on core 1, 7 on core 2, and 7 on core 3.
		args.value = x;

		for (int i = 0; i < 24; i++) {
			if (i < 10)
				args.cpu = 1;
			else if (i < 17)
				args.cpu = 2;
			else
				args.cpu = 3;

			pthread_create(threads + i, thread_attrs + i, (void *) multiplyMatrices, (void *) &args);
			transfer_task(getpid(), threads[i]);
		}

		for (int i = 0; i < 24; i++)
		   pthread_join(threads[i], NULL);

		clock_gettime( CLOCK_PROCESS_CPUTIME_ID, &stop);

		time = ((stop.tv_sec - start.tv_sec) * 1000000) + (stop.tv_nsec - start.tv_nsec);
		printf("================= TOOK %ld nanoseconds\n\n", time);
	}
}

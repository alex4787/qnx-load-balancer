/*
 * main.c
 *
 * QNX load balancer implementation for BMP (bound multiprocessing) including related tests.
 * Created for the final project in COMP 4900-A, Real-time Operating Systems.
 *
 * Authors: Morgan Vallati, Alex Chan
 */

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


#define TRUE 1
#define FALSE 0

#define MAX_CPUS 32

#define LIGHT_LOAD 0
#define NORMAL_LOAD 1
#define HEAVY_LOAD 2
#define LOAD_PROPORTION_THRESHOLD 0.2


// Total utilization time and thread with smallest load for a CPU
struct load_state_t {
	debug_thread_t *min_task;
	uint64_t totaltime;
} load_state_t;

// Used to pass arguments to start routine of a thread
struct arg_struct {
	int value;
	unsigned int cpu;
};


// STATIC VARIABLES
static struct load_state_t LoadStates[MAX_CPUS];

static int            NumCpus = 0;
static int            MinCpu, MaxCpu, MinLoad, MaxLoad; // store the cpu with the lightest and heaviest loads, as well as the value of these loads
static float          AveLoad;                          // average load across the entire system
static debug_thread_t min_task;                         // task with the smallest load, from the cpu with the heaviest load

static DIR *dir;

static unsigned    *rmaskp, *imaskp;	// Pointers to runmask and inherit mask that can be set after initialization using init_runmask()
static void		   *my_runmask;			// Runmask struct for the parent process (the load balancer itself)


// CORE FUNCTIONALITY
void populate_load_state(int, debug_thread_t);
int get_load_state(int, int);
void calculate_cpu_loads();
int perform_migration(pid_t, int, int);
int run_load_balancer();
int transfer_task(pid_t, int, int);
void init_runmask(void **);
void access_runmask(void *);
void set_runmask(void *);
void set_runmask_ext(pid_t, int, void*);

// TEST SUITE
void *run_task(void *);
void *multiply_matrices(void *);

void load_balance_test();
void performance_test();
void utilization_test();


int main() {
	// Initialize number of CPUs
	NumCpus = _syspage_ptr->num_cpu;

	// Open /proc directory to access information on all processes and threads
	dir = opendir("/proc");

	if (dir == NULL) {
		fprintf(stderr, "Unable to open /proc\n");
		return -1;
	}

	// Add ProcMgr permission to allow this process to change the runmask of other processes
	procmgr_ability(0,
			PROCMGR_ADN_NONROOT|PROCMGR_AOP_ALLOW|PROCMGR_AID_XPROCESS_DEBUG,
			PROCMGR_AID_EOL);

	// Increase the file descriptor limit to 9000 (due to the number of files opened in /proc)
	struct rlimit rlp;

	getrlimit(RLIMIT_NOFILE, &rlp);
	rlp.rlim_cur = 9000;
	rlp.rlim_max = 9000;
	setrlimit(RLIMIT_NOFILE, &rlp);

	// Set the runmask of this process to only run on CPU 0,
	// and the inherit mask to prevent any other threads run on this process from using CPU 0 (for tests)
	init_runmask(&my_runmask);
	RMSK_SET(0, rmaskp);
	RMSK_CLR(0, imaskp);
	set_runmask(my_runmask);

	// Tests (can comment out to run one at a time)
	load_balance_test();
//	performance_test();
//	utilization_test();

	// Finished getting all thread information
	closedir(dir);

	return 0;
}

// First part of the load monitoring phase of the algorithm
void populate_load_state(int cpu, debug_thread_t thread) {
	struct load_state_t *cur_load_state = LoadStates + cpu;

	// Update the min_task on this CPU if none already or the running time is less than the min_task
	if (cur_load_state->min_task == NULL) {
		cur_load_state->min_task = malloc(sizeof(debug_thread_t));
		memcpy(cur_load_state->min_task, &thread, sizeof(debug_thread_t));
	} else if (thread.sutime < cur_load_state->min_task->sutime) {
		memcpy(cur_load_state->min_task, &thread, sizeof(debug_thread_t));
	}

	// Increase the totaltime of the CPU by the thread's running time
	LoadStates[cpu].totaltime += thread.sutime;
}

// Second part of the load monitoring phase of the algorithm
// This function corresponds to equation 1 in the project report
int get_load_state(int processor_load, int avg_processor_load) {
	if (processor_load > avg_processor_load * (1 + LOAD_PROPORTION_THRESHOLD)) {
		return HEAVY_LOAD;
	} else if (processor_load < avg_processor_load * (1 - LOAD_PROPORTION_THRESHOLD)) {
		return LIGHT_LOAD;
	} else {
		return NORMAL_LOAD;
	}
}

// Finds the cpus with the lightest and heaviest loads, and what these load values are.
// Calculates the average load across the entire system.
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

// Task migration part of the algorithm (migrates min_task to the CPU with the minimum load)
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

// Akin to the get_transfer_task() in the paper
// This should be run periodically to determine the minimum CPU and which task to transfer, and migrate it if necessary.
int run_load_balancer() {
	return transfer_task(-1, -1, 0);
}

// Akin to the get_free_core() in the paper
// This is run when a new task is created and is moved to the minimum CPU (no need to find min_task)
// Largely inspired by: https://github.com/vocho/openqnx/blob/cc95df3543a1c481d4f8ad387e29f0d1ff0c87fe/trunk/utils/p/pidin/pidin.c#L526
int transfer_task(pid_t transfer_task_pid, int transfer_task_tid, int time_slice) {
	rewinddir(dir);

	struct dirent *dirent;
	debug_process_t procinfo;
	unsigned char found_transfer_task = FALSE;

	// Reset values in LoadStates
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
				perror("open()");
				return -1;
			}

			// Get process info
			if ((procstatus = devctl(fd, DCMD_PROC_INFO, &procinfo,
					sizeof procinfo, 0)) != -1) {
				int lasttid, tid, cpu;

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

						// Only consider the tasks run by the tests (i.e. same pid, but not the load balancer thread)
						if (pid == getpid() && tid != gettid()) {
							printf("\ttid=%d cpu=%d state=%d\n", tid, cpu, threadinfo.state);

							// If running this on creation of a new task, make it the min_task
							if (pid == transfer_task_pid && tid == transfer_task_tid) {
								found_transfer_task = TRUE;
								memcpy(&min_task, &threadinfo, sizeof(debug_thread_t));
								continue;
							}

							// Part A
							// Populate load_state_t array
							populate_load_state(cpu, threadinfo);
						}
					}
				}
			}
			if (close(fd) == -1) {
				perror("close()");
			}
		}
	}

	for (int i = 0; i < NumCpus; i++) {
		printf("CPU %d totaltime=%ld\n", i, LoadStates[i].totaltime);
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
		min_task = *LoadStates[MaxCpu].min_task;

		transfer_task_pid = min_task.pid;
		transfer_task_tid = min_task.tid;

		// Part B5
		if (MinLoad + min_task.sutime >= MaxLoad - min_task.sutime) {
			printf("no migration needed 2\n");
			return 0;
		}
	} else if (!found_transfer_task) {
		fprintf(stderr, "Could not find transfer task specified.\n");
		return -1;
	}

	// A task migration is performed to move the min_task (from the heaviest loaded core) to the core with the lightest load
	perform_migration(transfer_task_pid, transfer_task_tid, MinCpu);

	return 1;
}

// Initializes a runmask to be used for transferring tasks to another CPU.
// From the QNX reference docs: https://www.qnx.com/developers/docs/7.1/#com.qnx.doc.neutrino.lib_ref/topic/t/threadctl.html#threadctl___NTO_TCTL_RUNMASK_GET_AND_SET
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

// Sets the static variables representing the components of the runmask to be accessible for changing.
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

// Sets the runmask of the current load balancer thread.
void set_runmask(void *runmask) {
	if (ThreadCtl(_NTO_TCTL_RUNMASK_GET_AND_SET_INHERIT, runmask) == -1) {
		perror ("ThreadCtl()");
		exit(-1);
	}
}

// Sets the runmask of another process + thread.
void set_runmask_ext(pid_t pid, int tid, void *runmask) {
	if (ThreadCtlExt(pid, tid, _NTO_TCTL_RUNMASK_GET_AND_SET_INHERIT, runmask) == -1) {
		perror ("ThreadCtlExt()");
		exit(-1);
	}
}

// Start routine of the load_balancer_test threads.
// Sets its own CPU as specified by args.cpu.
// Simply occupies the CPU for args.value milliseconds.
void *run_task(void *_args) {
	struct arg_struct *args = (struct arg_struct *)_args;
	int cpu = args->cpu;

	void *runmask;
	init_runmask(&runmask);
	RMSK_SET(cpu, rmaskp);
	set_runmask(runmask);

	int msec = args->value;
	int msec_to_wait;
	struct timespec when;

	printf("[%d] Sleeping for %d milliseconds\n", gettid(), msec);

	// Necessary because nanospin takes no values above 500 milliseconds.
	while (msec > 0) {
		msec_to_wait = msec >= 500 ? 500 : msec;
		msec -= msec_to_wait;

		when.tv_sec = 0;
		when.tv_nsec = msec_to_wait * 1000000;

		nanospin(&when);
	}

	printf("[%d] Done after %d milliseconds\n", gettid(), args->value);
}

// Function to multiply two matrices, used in the utilization_test threads.
// Sets its own CPU as specified by args.cpu.
void *multiply_matrices(void *_args) {
	struct arg_struct *args = (struct arg_struct *)_args;
	int cpu = args->cpu;

	void *runmask;
	init_runmask(&runmask);
	RMSK_SET(cpu, rmaskp);
	set_runmask(runmask);

	// Dividing size of matrix into 4 sub-blocks
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

// First test case in the paper that ensures the load balancer algorithm works as expected.
void load_balance_test() {
	pthread_t threads[6];
	pthread_attr_t thread_attrs[6];
	pid_t pid = getpid();
	struct arg_struct args;
	int tid;
	int time_slice;

	printf("pid=%d\n", pid);

	for (int i = 0; i < 6; i++) {
		pthread_attr_init(thread_attrs + i);
		pthread_attr_setinheritsched (thread_attrs + i, PTHREAD_EXPLICIT_SCHED);

		struct sched_param param;

		param.sched_priority = 255;

		pthread_attr_setschedparam(thread_attrs + i, &param);
		pthread_attr_setschedpolicy (thread_attrs + i, SCHED_RR);
	}

	args.value = 100;
	args.cpu = 1;
	time_slice = 100;
	pthread_create(threads + 0, thread_attrs + 0, run_task, (void *) &args);
	transfer_task(pid, threads[0], time_slice);

	args.value = 50;
	time_slice = 50;

	pthread_create(threads + 1, thread_attrs + 1, run_task, (void *) &args);
	transfer_task(pid, threads[1], time_slice);

	pthread_create(threads + 2, thread_attrs + 2, run_task, (void *) &args);
	transfer_task(pid, threads[2], time_slice);

	args.cpu = 1;
	pthread_create(threads + 3, thread_attrs + 3, run_task, (void *) &args);
	transfer_task(pid, threads[3], time_slice);

	pthread_create(threads + 4, thread_attrs + 4, run_task, (void *) &args);
	transfer_task(pid, threads[4], time_slice);

	pthread_create(threads + 5, thread_attrs + 5, run_task, (void *) &args);
	transfer_task(pid, threads[4], time_slice);

	pthread_cancel(threads[2]);

	pthread_join(threads[2], NULL);

	run_load_balancer();

	sleep(1);

	run_load_balancer();

	for (int i = 0; i < 6; i++)
		pthread_join(threads[i], NULL);
}

// Second test case in the paper that measures the overhead of the load balancer.
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

//		printf("TOOK %ld nanoseconds\n\n", time);

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
		transfer_task(pid, tid, 50);
		clock_gettime( CLOCK_PROCESS_CPUTIME_ID, &stop);

		time = ((stop.tv_sec - start.tv_sec) * 1000000) + (stop.tv_nsec - start.tv_nsec);

		if (time > max_time) {
			max_time = time;
		}

		total_time += time;

//		printf("TOOK %ld nanoseconds\n\n", time);

		if ((i + 1) % 10 == 0) {
			avg_time = (long) ((double) total_time / (double) (i+1));
			printf("====== %d tests: total=%lu average=%lu max=%lu\n\n", (i+1), total_time, avg_time, max_time);
		}
	}
}

// Third test case in the paper that sees the change in CPU utilization / thread allocation with/without the load balancer.
void utilization_test() {
	long time;
	struct timespec start, stop;

	pthread_t threads[24];
	pthread_attr_t thread_attrs[24];

	struct arg_struct args;

	for (int i = 0; i < 24; i++) {
		pthread_attr_init(thread_attrs + i);
		pthread_attr_setinheritsched (thread_attrs + i, PTHREAD_EXPLICIT_SCHED);

		struct sched_param param;

		param.sched_priority = 15;

		pthread_attr_setschedparam(thread_attrs + i, &param);
		pthread_attr_setschedpolicy (thread_attrs + i, SCHED_RR);
	}

	// Runs the utilization test for matrices with size 32, 64, 128, and 256.
	for (int x = 32; x <= 256; x *= 2) {
		printf("%d\n", x);

		clock_gettime( CLOCK_PROCESS_CPUTIME_ID, &start);

		args.value = x;

		// add 10 tasks running matrix multiplication on core 1, 7 on core 2, and 7 on core 3.
		for (int i = 0; i < 24; i++) {
			if (i < 10)
				args.cpu = 1;
			else if (i < 17)
				args.cpu = 2;
			else
				args.cpu = 3;

			pthread_create(threads + i, thread_attrs + i, (void *) multiply_matrices, (void *) &args);
		}

		for (int i = 0; i < 20; i++) {
			run_load_balancer();
			sleep(1);
		}

		for (int i = 0; i < 24; i++)
			pthread_join(threads[i], NULL);

		clock_gettime( CLOCK_PROCESS_CPUTIME_ID, &stop);

		time = ((stop.tv_sec - start.tv_sec) * 1000000) + (stop.tv_nsec - start.tv_nsec);
		printf("================= TOOK %ld nanoseconds\n\n", time);
	}
}

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
// store the cpu with the lightest and heaviest loads, as well as the value of these loads
static int MinCpu, MaxCpu, MinLoad, MaxLoad;
// average load across the entire system
static float AveLoad;
// task with the smallest load, from the cpu with the heaviest load
static debug_thread_t min_task;


// FOR CPU UTILIZATION
static unsigned long LastSutime[MAX_CPUS];
static unsigned long LastNsec[MAX_CPUS];
static unsigned long Loads[MAX_CPUS];
static int ProcFd = -1;


static _uint64 nanoseconds( void ) {
	_uint64 sec, usec;
	struct timeval tval;

	gettimeofday( &tval, NULL );
	sec = tval.tv_sec;
	usec = tval.tv_usec;
	return( ( ( sec * 1000000 ) + usec ) * 1000 );
}



// first part of the load monitoring phase of the algorithm
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
		LoadStates[cpu].totaltime += thread.sutime;

		printf("total time: %ld\n", cur_load_state->totaltime);
	}
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


// load migration part of the algorithm
int perform_migration(debug_thread_t min_task, int MinCpu) {
	int slay_pid;

	char min_task_pid[64];
	sprintf(min_task_pid, "%d", min_task.pid);

	char min_cpu[64];
	sprintf(min_cpu, "%d", MinCpu);

	char min_task_tid[64];
	sprintf(min_task_tid, "%d", min_task.tid);

	slay_pid = spawnlp(P_WAIT, "slay", "slay", "-C", min_cpu, "-T", min_task_tid, min_task_pid, NULL);

	if (slay_pid == -1) {
		printf("Unable to execute slay (%s)", strerror(errno));
		return -1;
	}

	return 0;
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
						printf("\ttid=%d cpu=%d state=%d\n", tid, cpu, threadinfo.state);

						// Part A
						// Populate load_state_t array
						populate_load_state(cpu, threadinfo);

						printf("\ttotaltime=%ld\n", LoadStates[cpu].totaltime);
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
	min_task = *LoadStates[MaxCpu].min_task;
	if (MinLoad + min_task.sutime >= MaxLoad - min_task.sutime) {
		printf("no migration needed 2\n");
		return 0;
	}

	printf("migration needed\n");

	// a task migration is performed to move the min_task (from the heaviest loaded core) to the core with the lightest load
	perform_migration(min_task, MinCpu);

	return 1;
}

int init_cpu(void) {
	int i;
	ProcFd = -1;
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

			for( i=0; i<NumCpus; i++ ) {
				/*
				* the sutime of idle thread is how much
				* time that thread has been using, we can compare this
				* against how much time has passed to get an idea of the
				* load on the system.
				*/
				debug_data.tid = i + 1;
				devctl( ProcFd, DCMD_PROC_TIDSTATUS, &debug_data, sizeof( debug_data ), NULL );
				LastSutime[i] = debug_data.sutime;
				LastNsec[i] = nanoseconds();
			}

			printf("System has: %d CPUs\n", NumCpus);

			return (EOK);
		}
	}

	close(ProcFd);
	return (-1);
}

int sample_cpus( void ) {
	int i;
	debug_thread_t debug_data;
	_uint64 current_nsec, sutime_delta, time_delta;

	memset( &debug_data, 0, sizeof( debug_data ) );

	for( i=0; i<NumCpus; i++ ) {
		/* Get the sutime of the idle thread #i+1 */
		debug_data.tid = i + 1;
		devctl( ProcFd, DCMD_PROC_TIDSTATUS,
		&debug_data, sizeof( debug_data ), NULL );

		/* Get the current time */
		current_nsec = nanoseconds();

		/* Get the deltas between now and the last samples */
		sutime_delta = debug_data.sutime - LastSutime[i];
		time_delta = current_nsec - LastNsec[i];

		/* Figure out the load */
		Loads[i] = 100.0 - ( (float)( sutime_delta * 100 ) / (float)time_delta );

		/*
		* Flat out strange rounding issues.
		*/
		if( Loads[i] < 0 ) {
			Loads[i] = 0;
		}

		printf("CPU %d utilization: %.5f\n", i, Loads[i]);

		/* Keep these for reference in the next cycle */
		LastNsec[i] = current_nsec;
		LastSutime[i] = debug_data.sutime;
	}

	return EOK;
}

/* int do_something(event_data_t* e_d) { */
/*	 printf("Hello\n"); */
/*	 return 0; */
/* } */

void load_balance_test() {
	pthread_t threads[6];
	pthread_attr_t thread_attrs[6];

	printf("pid=%d\n", getpid());

	for (int i = 0; i < 6; i++) {
		pthread_attr_init(thread_attrs + i);
		thread_attrs[i].__param.__sched_priority = 15;
	}

	pthread_create(threads + 0, thread_attrs + 0, (void *) usleep, (void *) 100000);
	pthread_create(threads + 1, thread_attrs + 1, (void *) usleep, (void *) 50000);
	pthread_create(threads + 2, thread_attrs + 2, (void *) usleep, (void *) 50000);
	run_load_balancer();

	pthread_create(threads + 3, thread_attrs + 3, (void *) usleep, (void *) 50000);
	run_load_balancer();

	pthread_create(threads + 4, thread_attrs + 4, (void *) usleep, (void *) 50000);
	run_load_balancer();

	pthread_create(threads + 5, thread_attrs + 5, (void *) usleep, (void *) 50000);

	pthread_cancel(threads[2]);
	run_load_balancer();
	run_load_balancer();

	sleep(10);
	for(int i = 0; i < NumCpus; i++) {
		printf("CPU %d totaltime: %.5f\n", i, LoadStates[i].totaltime);
	}
}

void performance_test() {
	double max_time = 0;
	double total_time = 0;

	for (int i = 0; i < 50; i++) {
		struct timespec start, stop;
		double time;
		clock_gettime( CLOCK_REALTIME, &start);
		run_load_balancer();
		clock_gettime( CLOCK_REALTIME, &stop);
		time = (stop.tv_sec - start.tv_sec) + (double)(stop.tv_nsec - start.tv_nsec) / (double)1000000000L;

		if (time > max_time) {
			max_time = time;
		}

		total_time += time;

		printf("TOOK %.10f seconds\n\n", total_time);
		sleep(5);
	}
}


// function to multiply two matrices
void multiplyMatrices(int y) {
	// dividing size of matrix into 4 sub-blocks
	int x = y/4;

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

void utilization_test(long x) {
	pthread_t threads[24];
	pthread_attr_t thread_attrs[24];

	// core 1
	// add 10 tasks running matrix multiplication on core 1
	for (int i = 0; i < 10; i++) {
		pthread_attr_init(thread_attrs + i);
		thread_attrs[i].__param.__sched_priority = 15;
	}

	for (int i = 0; i < 10; i++) {
		pthread_create(threads + i, thread_attrs + i, (void *) multiplyMatrices, (void *) x);

		char tid[64];
		sprintf(tid, "%d", threads[i]);
		char pid[64];
		sprintf(pid, "%d", getpid());

		//spawnlp(P_WAIT, "slay", "slay", "-C", "1", "-T", tid, pid, NULL);

		run_load_balancer();
	}

	// core 2
	// add 7 tasks running matrix multiplication on core 2
	for (int i = 10; i < 17 ; i++) {
		pthread_attr_init(thread_attrs + i);
		thread_attrs[i].__param.__sched_priority = 15;
	}

	for (int i = 10; i < 17; i++) {
		pthread_create(threads + i, thread_attrs + i, (void *) multiplyMatrices, (void *) x);

		char tid[64];
		sprintf(tid, "%d", threads[i]);
		char pid[64];
		sprintf(pid, "%d", getpid());

		//spawnlp(P_WAIT, "slay", "slay", "-C", "2", "-T", tid, pid, NULL);

		run_load_balancer();
	}

	// core 3
	// add 7 tasks running matrix multiplication on core 3
	for (int i = 17; i < 24; i++) {
		pthread_attr_init(thread_attrs + i);
		thread_attrs[i].__param.__sched_priority = 15;
	}

	for (int i = 17; i < 24; i++) {
		pthread_create(threads + i, thread_attrs + i, (void *) multiplyMatrices, (void *) x);

		char tid[64];
		sprintf(tid, "%d", threads[i]);
		char pid[64];
		sprintf(pid, "%d", getpid());

		//spawnlp(P_WAIT, "slay", "slay", "-C", "3", "-T", tid, pid, NULL);

		run_load_balancer();
	}
	sleep(5);

	for(int i = 0; i < NumCpus; i++) {
		printf("CPU %d totaltime: %ld\n", i, LoadStates[i].totaltime);
	}

	sample_cpus();

//	for(int i = 0; i < NumCpus; i++) {
//		float utilization = ((float) LoadStates[i].totaltime / (float) MaxLoad) * 100.0f;
//		printf("CPU %d utilization: %.5f\n", i, utilization);
//	}
}

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

//	load_balance_test();
//	performance_test();
	utilization_test(512);

//	while (1) {
//		run_load_balancer();
//		printf("==================================================\n\n");
//		sleep(5);
//	}
}

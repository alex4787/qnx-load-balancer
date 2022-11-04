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

#define MAX_CPUS 32

static float Loads[MAX_CPUS];
static _uint64 LastSutime[MAX_CPUS];
static _uint64 LastNsec[MAX_CPUS];
static int ProcFd = -1;
static int NumCpus = 0;


int find_ncpus(void) {
	return NumCpus;
}

int get_cpu(int cpu) {
	int ret;
	ret = (int)Loads[ cpu % MAX_CPUS ];
	ret = max(0,ret);
	ret = min(100,ret);
	return( ret );
}

static _uint64 nanoseconds( void ) {
	_uint64 sec, usec;
	struct timeval tval;

	gettimeofday( &tval, NULL );
	sec = tval.tv_sec;
	usec = tval.tv_usec;
	return( ( ( sec * 1000000 ) + usec ) * 1000 );
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

		/* Keep these for reference in the next cycle */
		LastNsec[i] = current_nsec;
		LastSutime[i] = debug_data.sutime;
	}

	return EOK;
}


int init_cpu( void ) {
	int i;
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

			/* Get a starting point for the comparisons */
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

			return(EOK);
		}

	}

	close(ProcFd);
	return(-1);
}


void close_cpu(void) {
	if(ProcFd != -1) {
		close(ProcFd);
		ProcFd = -1;
	}
}

int partB() {
	int j;

	float t = 0.2;
	float ave;
	float min_value, max_value;
	float min_cpu, max_cpu;

	init_cpu();
	printf("System has: %d CPUs\n", NumCpus);
	while(1) {
		sample_cpus();
		ave = 0;
		for(j=0; j<NumCpus;j++) {
			printf("CPU #%d: %f\n", j, Loads[j]);
			ave += Loads[j];
		}
		ave /= NumCpus;

		min_value = max_value = Loads[0];
		min_cpu = max_cpu = 0;

		for (j=1; j<NumCpus; j++) {
			if (Loads[j] < min_value) {
				min_value = Loads[j];
				min_cpu = j;
			}
			if (Loads[j] > max_value) {
				max_value = Loads[j];
				max_cpu = j;
			}
		}

		// Check if max is heavy and min is light
		if (max_value > ave * (1+t) && min_value < ave * (1-t)) {
			// Step 5
		}

		sleep(1);
	}

	close_cpu();
}


// for part A

//struct load_state_t {
//	unsigned int task_ready_count;
//	unsigned int task_sleep_count;
//	// currently running tasks
//	tcb_t* pcurrent;
//  thread_t *pcurrent[50];
//	// queue where sleep tasks are located
//  thread_t *ppend[50];
//	tcb_t* ppend;
//	// sum of time slices of all ready tasks
//	unsigned int totaltime;
//	// size of each ready task and its time slice on the core
//	task_info_t task_info[CONFIG_MAX_TASKS];
//} load_state_t;

// defining load states
#define LIGHT 0
#define NORMAL 1
#define HEAVY 2

#define LOAD_PROPORTION_THRESHOLD 0.2

int get_load_state(int processor_load, int avg_processor_load) {
	if (processor_load > avg_processor_load * (1-LOAD_PROPORTION_THRESHOLD)) {
		return HEAVY;
	} else if (avg_processor_load * (1+LOAD_PROPORTION_THRESHOLD) >= processor_load && processor_load >= avg_processor_load * (1-LOAD_PROPORTION_THRESHOLD)) {
		return NORMAL;
	} else if (processor_load < avg_processor_load * (1-LOAD_PROPORTION_THRESHOLD)) {
		return LIGHT;
	} else {
		return -1; // something went wrong
	}
}

void partA() {
// load monitoring
	// load monitoring involves populating the load_state_t struct
}


int main(int argc, char* argv[]) {

	system("pidin");
	system("pidin | grep 'RUNNING'");
	system("pidin | grep 'READY'");
//	 first num = process id, second num = thread id
//	 need to figure out how to store these


	return 0;
}




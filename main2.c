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

struct load_state_t {
	unsigned int task_ready_count;
	unsigned int task_sleep_count;
	debug_thread_t current_task;
	debug_thread_t min_sleep_task;
	unsigned long int totaltime;
};

static struct load_state_t LoadStates[MAX_CPUS];

static int NumCpus = 0;

int populate_load_states() {
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
							if (threadinfo.state == STATE_RUNNING) {
								LoadStates[cpu].current_task = threadinfo;
								printf("found running task\n");
							} else if (threadinfo.state == STATE_READY) {
								LoadStates[cpu].task_ready_count += 1;
								LoadStates[cpu].totaltime += threadinfo.sutime;
								printf("total time: %ld\n", LoadStates[cpu].totaltime);
							} else if (threadinfo.state != STATE_STOPPED && threadinfo.state != STATE_DEAD) {
								LoadStates[cpu].task_sleep_count += 1;
								if (min_sleep_task_found == FALSE) {
									LoadStates[cpu].min_sleep_task = threadinfo;
									min_sleep_task_found = TRUE;
									printf("first min sleep task set\n");
								} else if (threadinfo.sutime < LoadStates[cpu].min_sleep_task.sutime) {
									LoadStates[cpu].min_sleep_task = threadinfo;
									printf("new min sleep task: %d\n", LoadStates[cpu].min_sleep_task.sutime);
								}
							}

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

			populate_load_states();

			return(EOK);
		}

	}

	close(ProcFd);
	return(-1);
}

void partA() {
	int j;

	float t = 0.2;

	init_cpu();
	printf("System has: %d CPUs\n", NumCpus);
	while(1) {
		sleep(1);
		populate_load_states();
//		sum = 0;
//		for(j=0; j<NumCpus;j++) {
//			sum += Loads[j];
//		}
//		ave = sum / NumCpus;
//
//		load_sum = sum;
//		load_average = ave;
	}
}


int main(int argc, char* argv[]) {

	partA();

	return 0;
}

//#include <atomic.h>
//#include <libc.h>
//#include <pthread.h>
//#include <stdio.h>
//#include <stdlib.h>
//#include <string.h>
//#include <sys/iofunc.h>
//#include <sys/neutrino.h>
//#include <sys/resmgr.h>
//#include <sys/syspage.h>
//#include <unistd.h>
//#include <inttypes.h>
//#include <stdio.h>
//#include <stdlib.h>
//#include <unistd.h>
//#include <sys/types.h>
//#include <sys/debug.h>
//#include <sys/procfs.h>
//#include <sys/syspage.h>
//#include <sys/neutrino.h>
//#include <sys/time.h>
//#include <time.h>
//#include <fcntl.h>
//#include <devctl.h>
//#include <errno.h>
//
//#define MAX_CPUS 32
//
//static float Loads[MAX_CPUS];
//static float LoadValues[MAX_CPUS];
//static _uint64 LastSutime[MAX_CPUS];
//static _uint64 LastNsec[MAX_CPUS];
//static int ProcFd = -1;
//static int NumCpus = 0;
//static float load_sum = 0;
//static float load_average = 0;
//
//struct {
//	float min_task_size;
//} max_cpu_info_t;
//
//
//int find_ncpus(void) {
//	return NumCpus;
//}
//
//int get_cpu(int cpu) {
//	int ret;
//	ret = (int)Loads[ cpu % MAX_CPUS ];
//	ret = max(0,ret);
//	ret = min(100,ret);
//	return( ret );
//}
//
//static _uint64 nanoseconds( void ) {
//	_uint64 sec, usec;
//	struct timeval tval;
//
//	gettimeofday( &tval, NULL );
//	sec = tval.tv_sec;
//	usec = tval.tv_usec;
//	return( ( ( sec * 1000000 ) + usec ) * 1000 );
//}
//
//int sample_cpus( void ) {
//	int i;
//	debug_thread_t debug_data;
//	_uint64 current_nsec, sutime_delta, time_delta;
//
//	memset( &debug_data, 0, sizeof( debug_data ) );
//
//	for( i=0; i<NumCpus; i++ ) {
//		/* Get the sutime of the idle thread #i+1 */
//		debug_data.tid = i + 1;
//		devctl( ProcFd, DCMD_PROC_TIDSTATUS,
//		&debug_data, sizeof( debug_data ), NULL );
//
//		/* Get the current time */
//		current_nsec = nanoseconds();
//
//		/* Get the deltas between now and the last samples */
//		sutime_delta = debug_data.sutime - LastSutime[i];
//		time_delta = current_nsec - LastNsec[i];
//
//		/* Figure out the load */
//		Loads[i] = 100.0 - ( (float)( sutime_delta * 100 ) / (float)time_delta );
//
//		/*
//		* Flat out strange rounding issues.
//		*/
//		if( Loads[i] < 0 ) {
//			Loads[i] = 0;
//		}
//
//		/* Keep these for reference in the next cycle */
//		LastNsec[i] = current_nsec;
//		LastSutime[i] = debug_data.sutime;
//	}
//
//	return EOK;
//}
//
//
//int init_cpu( void ) {
//	int i;
//	debug_thread_t debug_data;
//
//	memset( &debug_data, 0, sizeof( debug_data ) );
//
//	/*
//	* Open a connection to proc to talk over.
//	*/
//	ProcFd = open( "/proc/1/as", O_RDONLY );
//	if( ProcFd == -1 ) {
//		fprintf( stderr, "pload: Unable to access procnto: %s\n",
//		strerror( errno ) );
//		fflush( stderr );
//		return -1;
//	}
//
//	i = fcntl(ProcFd,F_GETFD);
//
//	if(i != -1) {
//		i |= FD_CLOEXEC;
//
//		if(fcntl(ProcFd,F_SETFD,i) != -1) {
//			/* Grab this value */
//			NumCpus = _syspage_ptr->num_cpu;
//
//			/* Get a starting point for the comparisons */
//			for( i=0; i<NumCpus; i++ ) {
//				/*
//				* the sutime of idle thread is how much
//				* time that thread has been using, we can compare this
//				* against how much time has passed to get an idea of the
//				* load on the system.
//				*/
//				debug_data.tid = i + 1;
//				devctl( ProcFd, DCMD_PROC_TIDSTATUS, &debug_data, sizeof( debug_data ), NULL );
//				LastSutime[i] = debug_data.sutime;
//				LastNsec[i] = nanoseconds();
//			}
//
//			return(EOK);
//		}
//
//	}
//
//	close(ProcFd);
//	return(-1);
//}
//
//
//void close_cpu(void) {
//	if(ProcFd != -1) {
//		close(ProcFd);
//		ProcFd = -1;
//	}
//}
//
//int partB() {
//	int j;
//
//	float t = 0.2;
//	float sum, ave;
//	float min_value, max_value;
//	int min_cpu, max_cpu;
//
//	init_cpu();
//	printf("System has: %d CPUs\n", NumCpus);
//	while(1) {
//		sample_cpus();
//		sum = 0;
//		for(j=0; j<NumCpus;j++) {
////			printf("CPU #%d: %f\n", j, Loads[j]);
//			sum += Loads[j];
//		}
//		ave = sum / NumCpus;
//
//		min_value = max_value = Loads[0];
//		min_cpu = max_cpu = 0;
//
//		for (j=1; j<NumCpus; j++) {
//			if (Loads[j] < min_value) {
//				min_value = Loads[j];
//				min_cpu = j;
//			}
//			if (Loads[j] > max_value) {
//				max_value = Loads[j];
//				max_cpu = j;
//			}
//		}
//
//		// Check if max is heavy and min is light
//		// could call get_load_state() function defined below
//		if (max_value > ave * (1+t) && min_value < ave * (1-t)) {
//			if (migration_needed(max_value, min_value, min_task_size(max_cpu))) {
//
//			}
//		}
//
//		sleep(1);
//	}
//
//	close_cpu();
//}
//
//#define FALSE 0
//#define TRUE 1
//
//int migration_needed(float max_thread, float min_thread, float task_size) {
//	if (min_thread + task_size >= max_thread) {
//		return FALSE;
//	} else {
//		return TRUE;
//	}
//}
//
//
//// for part A
//
//// defining load states
//#define LIGHT 0
//#define NORMAL 1
//#define HEAVY 2
//
//#define LOAD_PROPORTION_THRESHOLD 0.2
//
//int get_load_state(int processor_load, int avg_processor_load) {
//	if (processor_load > avg_processor_load * (1-LOAD_PROPORTION_THRESHOLD)) {
//		return HEAVY;
//	} else if (avg_processor_load * (1+LOAD_PROPORTION_THRESHOLD) >= processor_load && processor_load >= avg_processor_load * (1-LOAD_PROPORTION_THRESHOLD)) {
//		return NORMAL;
//	} else if (processor_load < avg_processor_load * (1-LOAD_PROPORTION_THRESHOLD)) {
//		return LIGHT;
//	} else {
//		return -1; // something went wrong
//	}
//}
//
//void partA() {
//
//	int j;
//
//	float t = 0.2;
//	float sum, ave;
//
//	init_cpu();
//	printf("System has: %d CPUs\n", NumCpus);
//	while(1) {
//		sleep(1);
//		sample_cpus();
//		sum = 0;
//		for(j=0; j<NumCpus;j++) {
//			sum += Loads[j];
//		}
//		ave = sum / NumCpus;
//
//		load_sum = sum;
//		load_average = ave;
//	}
//}
//
//
//int get_node_procs (max_cpu_info_t *tree_p)
//{
//	tree_p->min_task_size = -1;
//    DIR			*dir;
//	char		fname[PATH_MAX];
//
//	if (!tree_p){
//		return NULL;
//	}
//
//	dir = opendir("/proc");
//
//	if (dir == NULL) {
//		fprintf(stderr, "Unable to open %s\n", fname);
//		return 0;
//	} else {
//		struct dirent * dirent;
//		/* The only value that matters gets reset */
//		debug_process_t procinfo;
//		while ((dirent = readdir(dir)) != NULL) {
//			if (isdigit(dirent->d_name[0])) {
//				int pid = atoi(dirent->d_name);
//				int fd;
//				char buff[512];
//
//				snprintf(buff, sizeof(buff), "/proc/%d", pid);
//
//				if ((fd = open(buff, O_RDONLY)) == -1) {
//					return 0;
//				}
//
//				if (devctl(fd, DCMD_PROC_INFO, &procinfo, sizeof procinfo, 0) != -1) {
//					int lasttid, tid;
//					if (procinfo.flags & _NTO_PF_ZOMBIE) {
//						close(fd);
//						return 0;
//					} else {
//
//						for (lasttid = tid = 1; ; lasttid = ++tid) {
//							debug_thread_t threadinfo;
//							memset(&threadinfo, 0, sizeof(threadinfo));
//							threadinfo.tid = tid;
//							if (devctl(fd, DCMD_PROC_TIDSTATUS, &threadinfo, sizeof(threadinfo), 0) != EOK) {
//								break;
//							}
//							tid = threadinfo.tid;
//							//
//							if (tid < lasttid) {
//								break;
//							}
//						}
//					}
//				}
//				close(fd);
//            }
//        }
//        closedir(dir);
//    }
//    return 1;
//}
//
//
////int do_process(pid_t pid, process_entry * pe_p)
////{
////	int fd;
////	char buff[512];
////
////	if (pe_p == NULL) {
////		return NULL;
////	}
////
////	pe_p->n_threads = 0;
////
////	snprintf(buff, sizeof(buff), "/proc/%d", pid);
////
////	if ((fd = open(buff, O_RDONLY)) == -1) {
////		return 0;
////	}
////
////	if (devctl(fd, DCMD_PROC_INFO, &pe_p->info, sizeof pe_p->info, 0) != -1) {
////		int lasttid, tid;
////		if (pe_p->info.flags & _NTO_PF_ZOMBIE) {
////			close(fd);
////			return 0;
////		} else {
////
////			for (lasttid = tid = 1; ; lasttid = ++tid) {
////				thread_entry te_p;
////				memset(&te_p, 0, sizeof(thread_entry));
////				te_p.status.tid = tid;
////				if (devctl(fd, DCMD_PROC_TIDSTATUS, &te_p.status, sizeof(te_p.status), 0) != EOK) {
////					break;
////				}
////				tid = te_p.status.tid;
////				//
////				if (tid < lasttid) {
////					break;
////				}
////				pe_p->n_threads++;
////			}
////		}
////	}
////	close(fd);
////	return 1;
////}
//
//
//float min_task_size(int cpu_num) {
//	max_cpu_info_t info;
//
//	get_node_procs(&info);
//
//	return info.min_task_size;
//}
//
//int main(int argc, char* argv[]) {
//
//	partA();
//
//
//	return 0;
//}
//
//
//

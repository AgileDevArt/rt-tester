/*
 * Real Time Tester
 *
 * Based on:
 * https://wiki.linuxfoundation.org/realtime/documentation/howto/applications/application_base
 * https://wiki.linuxfoundation.org/realtime/documentation/howto/applications/cyclic
 *
 * This was built using VS on Windows and WSL as the Target System
 * 
 * Use vcpkg to install pthreads
 * https://github.com/microsoft/vcpkg
 *
 * Add CMAKE_TOOLCHAIN_FILE variable to CMakePresets.json
 * "CMAKE_TOOLCHAIN_FILE": "[path to vcpkg]/scripts/buildsystems/vcpkg.cmake"
 *
 */

#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "rt-tester.h"
#include <string>

#if defined(_WIN32)
#define OS "Windows"
#elif defined(__linux__)
#define OS "Linux"
#elif defined(__APPLE__)
#define OS "MacOS"
#else
#define OS "Unknown OS"
#endif

void print_logo(long tid, int policy, int priority, long double period_ms, long print_per_sec)
{
    printf(R"(                              
  ____ _____   _            _            
 |  _ \_   _| | |_ ___  ___| |_ ___ _ __ 
 | |_) || |   | __/ _ \/ __| __/ _ \ '__|
 |  _ < | |   | ||  __/\__ \ ||  __/ |   
 |_| \_\|_|    \__\___||___/\__\___|_|    ver: %s                                       

 scheduler policy: %s
 priority: %d

 period: %.4Lf ms   
 console refresh rate: %ld Hz

)", VERSION, 
    //tid, 
    policy == SCHED_OTHER ? "SCHED_OTHER" :
   (policy == SCHED_RR ? "SCHED_RR" :
   (policy == SCHED_FIFO ? "SCHED_FIFO" : "Unknown...")), 
    priority, 
    period_ms, 
    print_per_sec);
}

long double diff_nanosec(const struct timespec* time1, const struct timespec* time0)
{
    return 1000000000 * (time1->tv_sec - time0->tv_sec) + (time1->tv_nsec - time0->tv_nsec);
}

static void inc_period(struct period_info* pinfo)
{
    pinfo->next_period.tv_nsec += pinfo->period_ns;
    //timespec nsec overflow check
    while (pinfo->next_period.tv_nsec >= 1000000000) 
    {
        pinfo->next_period.tv_sec++;
        pinfo->next_period.tv_nsec -= 1000000000;
    }
}

static void periodic_task_init(struct period_info* pinfo, long period_ns, long print_per_sec)
{
    pinfo->period_ns = period_ns;
    pinfo->print_rate = print_per_sec == 0 ? 0 : 1000000000 / (period_ns * print_per_sec);
    clock_gettime(CLOCK_MONOTONIC, &(pinfo->next_period));
}

static void wait_rest_of_period(struct period_info* pinfo)
{
    //for simplicity, ignoring possibilities of signal wakes
    inc_period(pinfo);
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &pinfo->next_period, NULL);
}

static void do_rt_task(struct period_info* pinfo)
{
    long double delay_ns, task_ns;
    struct timespec start, end;

    clock_gettime(CLOCK_MONOTONIC, &start);
    delay_ns = diff_nanosec(&start, &(pinfo->next_period));

    time_t rawtime;
    time(&rawtime);
    struct tm* timeinfo = localtime(&rawtime);

    bool period_exceeded = delay_ns > pinfo->period_ns;
    bool print_info = pinfo->print_rate != 0 && ((pinfo->next_period.tv_nsec / pinfo->period_ns) % pinfo->print_rate) == 0;

    clock_gettime(CLOCK_MONOTONIC, &end);
    task_ns = diff_nanosec(&end, &start);

    if (period_exceeded)
    {
        printf("\33[2K\r[%02d:%02d:%02d] " RED "delay: %.4Lfms task: %.4Lfms" RESET "\n", timeinfo->tm_hour, timeinfo->tm_min,  timeinfo->tm_sec, delay_ns / 1000000.0, task_ns / 1000000.0);
    }
    else if (print_info)
    {
        printf("\33[2K\r[%02d:%02d:%02d] delay: %.4Lfms task: %.4Lfms", timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec, delay_ns / 1000000.0, task_ns / 1000000.0);
        fflush(stdout);
    }
}

void* simple_cyclic_task(void* data)
{
    struct sched_param sp;
    long tid = syscall(SYS_gettid);
    int policy = sched_getscheduler(tid);
    int priority = sched_getparam(0, &sp) ? 0 : sp.sched_priority;
    long period_ns = ((task_data*)data)->period_ns;
    long print_per_sec = ((task_data*)data)->print_per_sec;
    print_logo(tid, policy, priority, (long double)period_ns / 1000000, print_per_sec);

    struct period_info pinfo;
    periodic_task_init(&pinfo, period_ns, print_per_sec);

    while (1) {
        do_rt_task(&pinfo);
        wait_rest_of_period(&pinfo);
    }

    return NULL;
}

int main(int argc, char* argv[])
{
    struct sched_param param;
    pthread_attr_t attr;
    pthread_t thread;
    int ret;

    struct task_data data;
    data.period_ns = 1000000; //hardcoding a 1ms period
    data.print_per_sec = 5; //hardcoding 5 prints per sec

    int opt;
    while ((opt = getopt(argc, argv, "p:r:")) != -1)
    {
        switch (opt) 
        {
            case 'p': data.period_ns = atof(optarg) * 1000000; break;
            case 'r': data.print_per_sec = atol(optarg); break;
            default:
                fprintf(stderr, "Usage: %s [-p ns] [-r Hz]\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    /* Lock memory */
    if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1) {
        printf("mlockall failed: %s\n", strerror(errno));
        exit(-2);
    }

    /* Initialize pthread attributes (default values) */
    ret = pthread_attr_init(&attr);
    if (ret) {
        printf("init pthread attributes failed: %s\n", strerror(ret));
        goto out;
    }

    /* Set a specific stack size  */
    ret = pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN);
    if (ret) {
        printf("pthread setstacksize failed: %s\n", strerror(ret));
        goto out;
    }

    /* Set scheduler policy and priority of pthread */
    ret = pthread_attr_setschedpolicy(&attr, SCHED_FIFO); /*o windows only SCHED_OTHER*/
    if (ret) {
        printf("pthread setschedpolicy failed: %s\n", strerror(ret));
        goto out;
    }

    param.sched_priority = 80;
    ret = pthread_attr_setschedparam(&attr, &param);
    if (ret) {
        printf("pthread setschedparam failed: %s\n", strerror(ret));
        goto out;
    }

    /* Use scheduling parameters of attr */
    ret = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    if (ret) {
        printf("pthread setinheritsched failed: %s\n", strerror(ret));
        goto out;
    }

    /* Create a pthread with specified attributes */
    ret = pthread_create(&thread, &attr, simple_cyclic_task, &data);
    if (ret) {
        printf("create pthread failed: %s\n", strerror(ret));
        goto out;
    }

    /* Join the thread and wait until it is done */
    ret = pthread_join(thread, NULL);
    if (ret)
        printf("join pthread failed: %s\n", strerror(errno));

out:
    return ret;
}

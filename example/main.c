#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <inttypes.h>
#define gettid() syscall(SYS_gettid)
#include "rdtsc.h"
#include "lock.h"

#ifndef CYCLE_PER_US
#error Must define CYCLE_PER_US for the current machine in the Makefile or elsewhere
#endif

typedef unsigned long long ull;

typedef struct {
    volatile int *stop;
    pthread_t thread;
    int priority;
    int weight;
    int id;
    double cs;
    int ncpu;
    int parent;
    // outputs
    ull loop_in_cs;
    ull lock_acquires;
    ull lock_hold;
} task_t __attribute__ ((aligned (64)));

void compute_path(int path_arr[], int child, int* path_arr_idx){

    if(H_node_adj[child].parent == -1)return ;

    compute_path(path_arr, H_node_adj[child].parent, path_arr_idx);

    path_arr[*path_arr_idx] = child;
    (*path_arr_idx) += 1;
    
}

void *worker(void *arg) {
    int ret;
    task_t *task = (task_t *) arg;

    if (task->ncpu != 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        for (int i = 0; i < task->ncpu; i++) {
            if (i < 8 || i >= 24)
                CPU_SET(i, &cpuset);
            else if (i < 16)
                CPU_SET(i+8, &cpuset);
            else
                CPU_SET(i-8, &cpuset);
        }
        ret = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
        if (ret != 0) {
            perror("pthread_set_affinity_np");
            exit(-1);
        }
    }

    pid_t tid = gettid();
    ret = setpriority(PRIO_PROCESS, tid, task->priority);
    if (ret != 0) {
        perror("setpriority");
        exit(-1);
    }


    int path_arr[MAX_LEVEL];
    int path_arr_idx = 0;
    compute_path(path_arr, task->parent, &path_arr_idx);

    printf("\n");
    printf("thread %d path -> ", task->id);
    for(int i = 0; i<MAX_LEVEL; i++){
        printf("%d ", path_arr[i]);
    }
    printf("\n");
    
#ifdef HRLOCK                                                   /*it was there in u-SCL*/

    hrlock_thread_init(0, task->weight, path_arr);
#endif

    // loop
    ull now, start, then;
    ull lock_acquires = 0;
    ull lock_hold = 0;
    ull loop_in_cs = 0;
    const ull delta = CYCLE_PER_US * task->cs;

    while (!*task->stop) {

        // lock_acquire(&lock);
        ull acquire = rdtscp();
        lock_acquire(0, path_arr);
        now = rdtscp();

        lock_acquires++;
        start = now;
        then = now + delta;

        do {
            loop_in_cs++;
        } while ((now = rdtscp()) < then);
        

        lock_hold += now - start;
        // printf("ok\n");
        lock_release(task->parent, path_arr);
    }

    task->lock_acquires = lock_acquires;
    task->loop_in_cs = loop_in_cs;
    task->lock_hold = lock_hold;

    pid_t pid = getpid();
    char path[256];
    char buffer[1024] = { 0 };
    snprintf(path, 256, "/proc/%d/task/%d/schedstat", pid, tid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("open");
        exit(-1);
    }
    if (read(fd, buffer, 1024) <= 0) {
        perror("read");
        exit(-1);
    }

    printf("id %02d "
            "loop %10llu "
            "lock_acquires %8llu "
            "lock_hold(ms) %10.3f "
            "schedstat %s",
            task->id,
            task->loop_in_cs,
            task->lock_acquires,
            task->lock_hold / (float) (CYCLE_PER_US * 1000),
            buffer);
}

int main(int argc, char *argv[]) {

    FILE*fptr;
    fptr = fopen(argv[1], "r");
    printf("%s\n", argv[1]);
    // fptr = fopen("/home/kali/Desktop/manoj-proportional-share/Hierarchical - SCLs/example/input.txt", "r");

    int num_non_leaf_nodes, nthreads, max_level, duration;
    fscanf(fptr, "%d %d %d %d", &num_non_leaf_nodes, &nthreads, &max_level, &duration);


    task_t *tasks = malloc(sizeof(task_t) * nthreads);
    int stop __attribute__((aligned (64))) = 0;

    int threads_itr = 0;
    // need to do root intialisation separately
    H_node_adj[0].parent =  -1;


    for(int i = 0; i<num_non_leaf_nodes;i++){

        int level, num_children, is_penultimate; 
        fscanf(fptr, "%d %d %d", &level, &is_penultimate, &num_children);

        H_node_adj[i].slice = pow_arr[max_level + 1 - level];
        H_node_adj[i].level = level;
        H_node_adj[i].is_penultimate = is_penultimate;
        H_node_adj[i].num_children = num_children;

        tot_children_at_level[level + 1] += num_children;

        if(!is_penultimate){
            
            /*initialise node specific information*/
            node_specific_arr[i].banned_until = rdtsc();
            node_specific_arr[i].weight = 0;
            node_specific_arr[i].slice = 0;
            node_specific_arr[i].start_ticks = 0;
            node_specific_arr[i].banned = 0;  

            for(int j = 0; j<num_children;j++){

                int child;
                fscanf(fptr, "%d", &child);
                H_node_adj[i].children_idx[j] = child;
                H_node_adj[i].banned_until[j] = 0;
                // parents are added here
                H_node_adj[child].parent  = i;
                // printf("child, parent  = %d %d\n", child, i);

            }
        }
        else{

            int tot_weight = 0;                                 /*needs to handled for locks*/
            int ncpu = 8;                                       /*has to be changed*/  
            int temp = num_children + threads_itr; 

            for (; threads_itr < temp; threads_itr++) {

                tasks[threads_itr].stop = &stop;
                double cs; int priority;
                fscanf(fptr, "%lf %d", &cs, &priority);

                tasks[threads_itr].cs = cs;
                tasks[threads_itr].priority = priority;
                int weight = prio_to_weight[priority+20];
                tasks[threads_itr].weight = weight;
                tot_weight += weight;
                
                tasks[threads_itr].ncpu = ncpu;
                tasks[threads_itr].id = threads_itr;

                tasks[threads_itr].loop_in_cs = 0;
                tasks[threads_itr].lock_acquires = 0;
                tasks[threads_itr].lock_hold = 0;

                tasks[threads_itr].parent = i;
                // printf("thread, parent  = %d %d\n", threads_itr, i);
            }
        }
    }
    // for(int i = 0; i< MAX_LEVEL + 1; i++){
    //     printf(" children at level %d - %d\n",i, tot_children_at_level[i]);
    // }
    // printf("/n");

    for(int i = 0; i < num_non_leaf_nodes; i++){
        printf("node: %d ", i);
        printf("level: %d static slice:%lld num_children: %d and they are ", H_node_adj[i].level,
        H_node_adj[i].slice, H_node_adj[i].num_children);

        for(int j = 0; j<H_node_adj[i].num_children;j++){
            printf("%d ", H_node_adj[i].children_idx[j]);
        }

        printf("\n");
    }

    for(int i = 0; i<nthreads; i++){
        printf("thread_id: %d, <%lf %d>\n",tasks[i].id, tasks[i].cs, tasks[i].priority);
    }

    // lock_init(&lock);
    lock_init(0);

    for (int i = 0; i < nthreads; i++) {
        pthread_create(&tasks[i].thread, NULL, worker, &tasks[i]);
    }
    
    sleep(duration);
    stop = 1;
    for (int i = 0; i < nthreads; i++) {
        pthread_join(tasks[i].thread, NULL);
    }
    return 0;
}
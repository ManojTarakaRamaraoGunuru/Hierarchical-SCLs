#define _GNU_SOURCE
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdio.h>

#include "fairlock.h"


void hrlock_init(int idx){
    
    if(H_node_adj[idx].is_penultimate == 1){
        fairlock_init(&H_node_adj[idx].lock);
        return;
    }
    fairlock_init(&H_node_adj[idx].lock);
    for(int i = 0; i<H_node_adj[idx].num_children; i++){
        hrlock_init(H_node_adj[idx].children_idx[i]);
    }

}

int get_next_node_idx(int idx, int* path_arr){

    int level = H_node_adj[idx].level;             
    int idx_dash = path_arr[level]; 
    return idx_dash;
}

void hrlock_thread_init(int idx, int weight, int *path_arr){
    if(H_node_adj[idx].is_penultimate == 1){
        fairlock_thread_init(&H_node_adj[idx].lock, weight);
        return;
    }
    // fairlock_thread_init(&H_node_adj[idx].lock, weight, path_arr);

    if (weight == 0) {
        int prio = getpriority(PRIO_PROCESS, 0);
        weight = prio_to_weight[prio+20];
    }
    __sync_add_and_fetch(&(&H_node_adj[idx].lock)->total_weight, weight);
    int idx_dash = get_next_node_idx(idx, path_arr);
    node_specific_arr[idx_dash].weight += weight;
    hrlock_thread_init(idx_dash, weight, path_arr);
}


void hrlock_acquire(int idx, int* path_arr){
    
    if(H_node_adj[idx].is_penultimate == 1){
        fairlock_acquire(&H_node_adj[idx].lock, idx);
    }
    
    else{
        int idx_dash = get_next_node_idx(idx, path_arr);
        nl_fairlock_acquire(&H_node_adj[idx].lock, idx_dash);                            
        hrlock_acquire(idx_dash, path_arr);
    }
}

void hrlock_release(int idx, int*path_arr){

    if(idx == 0){
        if(H_node_adj[idx].is_penultimate == 1){
            fairlock_release(&H_node_adj[idx].lock, idx);
        }
        else{
            int idx_dash = get_next_node_idx(idx, path_arr);
            nl_fairlock_release(&H_node_adj[idx].lock, idx_dash);
        }
    }
    else if(H_node_adj[idx].is_penultimate == 1){
        fairlock_release(&H_node_adj[idx].lock, idx);
        hrlock_release(H_node_adj[idx].parent, path_arr);
    }
    else{
        int idx_dash = get_next_node_idx(idx, path_arr);
        ull now = rdtsc();
        if(now <= (&H_node_adj[idx].lock)->slice){
            return;
        }   
        nl_fairlock_release(&H_node_adj[idx].lock, idx_dash);
        hrlock_release(H_node_adj[idx].parent, path_arr);
    }
    
}
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

#include "rdtsc.h"
#include "common.h"

typedef unsigned long long ull;


#ifdef DEBUG
typedef struct stats {
    ull reenter;
    ull banned_time;
    ull start;
    ull next_runnable_wait;
    ull prev_slice_wait;
    ull own_slice_wait;
    ull runnable_wait;
    ull succ_wait;
    ull release_succ_wait;
} stats_t;
#endif

typedef struct flthread_info {
    ull banned_until;
    ull weight;
    ull slice;
    ull start_ticks;
    int banned;
    int *path_arr;                                
#ifdef DEBUG
    stats_t stat;
#endif
} flthread_info_t;

enum qnode_state {
    INIT = 0, // not waiting or after next runnable node
    NEXT,
    RUNNABLE,
    RUNNING
};

typedef struct qnode {
    int state __attribute__ ((aligned (CACHELINE)));
    struct qnode *next __attribute__ ((aligned (CACHELINE)));
} qnode_t __attribute__ ((aligned (CACHELINE)));

typedef struct fairlock {
    qnode_t *qtail __attribute__ ((aligned (CACHELINE)));
    qnode_t *qnext __attribute__ ((aligned (CACHELINE)));
    ull slice __attribute__ ((aligned (CACHELINE)));
    int slice_valid __attribute__ ((aligned (CACHELINE)));
    pthread_key_t flthread_info_key;
    ull total_weight;
} fairlock_t __attribute__ ((aligned (CACHELINE)));

static inline qnode_t *flqnode(fairlock_t *lock) {
    return (qnode_t *) ((char *) &lock->qnext - offsetof(qnode_t, next));
}

static inline int futex(int *uaddr, int futex_op, int val, const struct timespec *timeout) {
    return syscall(SYS_futex, uaddr, futex_op, val, timeout, NULL, 0);
}
/*portion added for non leaf nodes and hierarchical nodes*/

typedef struct hierarchical_node{
    fairlock_t lock;        // need to write a small logic for RW_SCL
    ull slice;              // 2^level in the reverse order
    // int slice_valid;        // already present in U-SCL
    // ull total_wt;           // "  total weight of children
    int level; 
    int num_children;      
    int children_idx[MAX_CHILDREN];
    int banned_until[MAX_CHILDREN];
    int is_penultimate;
    int parent;
} H_node;

H_node H_node_adj[MAX_NODES];

int pow_arr[6] = {1, 2, 4, 8, 16, 32};   /*Upadte this when you increase number of levels*/

int tot_children_at_level[MAX_LEVEL + 2];

typedef struct non_leaf_node_info {
    ull banned_until;
    ull weight;
    ull slice;
    ull start_ticks;
    int banned;                              
#ifdef DEBUG
    stats_t stat;
#endif
} nl_node_info;

nl_node_info node_specific_arr[MAX_NODES];

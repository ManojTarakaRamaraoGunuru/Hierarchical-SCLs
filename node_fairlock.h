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

#include "struct.h"


void nl_fairlock_acquire(fairlock_t *lock, int idx) {

    ull now;

    if (readvol(lock->slice_valid)) {
        ull curr_slice = lock->slice;
        // If owner of current slice, try to reenter at the beginning of the queue
        if (curr_slice == node_specific_arr[idx].slice && (now = rdtsc()) < curr_slice) {
            qnode_t *succ = readvol(lock->qnext);
            if (NULL == succ) {
                if (__sync_bool_compare_and_swap(&lock->qtail, NULL, flqnode(lock)))
                    goto reenter;
                spin_then_yield(SPIN_LIMIT, (now = rdtsc()) < curr_slice && NULL == (succ = readvol(lock->qnext)));
#ifdef DEBUG
                node_specific_arr[idx].stat.own_slice_wait += rdtsc() - now;
#endif
                // let the succ invalidate the slice, and don't need to wake it up because slice expires naturally
                if (now >= curr_slice)
                    goto begin;
            }
            // if state < RUNNABLE, it won't become RUNNABLE unless someone releases lock,
            // but as no one is holding the lock, there is no race
            if (succ->state < RUNNABLE || __sync_bool_compare_and_swap(&succ->state, RUNNABLE, NEXT)) {
reenter:
#ifdef DEBUG
                node_specific_arr[idx].stat.reenter++;
#endif
                node_specific_arr[idx].start_ticks = now;
                return;
            }
        }
    }
begin:

    if (node_specific_arr[idx].banned) {
        if ((now = rdtsc()) < node_specific_arr[idx].banned_until) {
            ull banned_time = node_specific_arr[idx].banned_until - now;
#ifdef DEBUG
            node_specific_arr[idx].stat.banned_time += banned_time;
#endif
            // sleep with granularity of SLEEP_GRANULARITY us
            while (banned_time > CYCLE_PER_US * SLEEP_GRANULARITY) {
                struct timespec req = {
                    .tv_sec = banned_time / CYCLE_PER_S,
                    .tv_nsec = (banned_time % CYCLE_PER_S / CYCLE_PER_US / SLEEP_GRANULARITY) * SLEEP_GRANULARITY * 1000,
                };
                nanosleep(&req, NULL);
                if ((now = rdtsc()) >= node_specific_arr[idx].banned_until)
                    break;
                banned_time = node_specific_arr[idx].banned_until - now;
            }
            // spin for the remaining (<SLEEP_GRANULARITY us)
            spin_then_yield(SPIN_LIMIT, (now = rdtsc()) < node_specific_arr[idx].banned_until);
        }
    }
    qnode_t n = { 0 };
    while (1) {
        qnode_t *prev = readvol(lock->qtail);
        if (__sync_bool_compare_and_swap(&lock->qtail, prev, &n)) {
            // enter the lock queue
            if (NULL == prev) {
                n.state = RUNNABLE;
                lock->qnext = &n;
            } else {
                if (prev == flqnode(lock)) {
                    n.state = NEXT;
                    prev->next = &n;
                } else {
                    prev->next = &n;
                    // wait until we become the next runnable
#ifdef DEBUG
                    now = rdtsc();
#endif
                    do {
                        futex(&n.state, FUTEX_WAIT_PRIVATE, INIT, NULL);
                    } while (INIT == readvol(n.state));
#ifdef DEBUG
                    node_specific_arr[idx].stat.next_runnable_wait += rdtsc() - now;
#endif
                }
            }
            // invariant: n.state >= NEXT
            // printf("// invariant: n.state >= NEXT %d\n", n.state);

            // wait until the current slice expires
            int slice_valid;
            ull curr_slice;
            while ((slice_valid = readvol(lock->slice_valid)) && (now = rdtsc()) + SLEEP_GRANULARITY < (curr_slice = readvol(lock->slice))) {
                ull slice_left = curr_slice - now;
                struct timespec timeout = {
                    .tv_sec = 0, // slice will be less then 1 sec
                    .tv_nsec = (slice_left / (CYCLE_PER_US * SLEEP_GRANULARITY)) * SLEEP_GRANULARITY * 1000,
                };
                futex(&lock->slice_valid, FUTEX_WAIT_PRIVATE, 0, &timeout);
#ifdef DEBUG
                node_specific_arr[idx].stat.prev_slice_wait += rdtsc() - now;
#endif
            }
            if (slice_valid) {
                spin_then_yield(SPIN_LIMIT, (slice_valid = readvol(lock->slice_valid)) && rdtsc() < readvol(lock->slice));
                if (slice_valid)
                    lock->slice_valid = 0;
            }
            // invariant: rdtsc() >= curr_slice && lock->slice_valid == 0
            // printf("// invariant: rdtsc() >= curr_slice && lock->slice_valid == 0 %llu %llu %d\n" , rdtsc(), curr_slice, lock->slice_valid);

#ifdef DEBUG
            now = rdtsc();
#endif
            // spin until RUNNABLE and try to grab the lock
            spin_then_yield(SPIN_LIMIT, RUNNABLE != readvol(n.state) || 0 == __sync_bool_compare_and_swap(&n.state, RUNNABLE, RUNNING));
            // invariant: n.state == RUNNING
            // printf("// invariant: n.state == RUNNING %d\n", n.state);
#ifdef DEBUG
            node_specific_arr[idx].stat.runnable_wait += rdtsc() - now;
#endif

            // record the successor in the lock so we can notify it when we release
            qnode_t *succ = readvol(n.next);
            if (NULL == succ) {
                lock->qnext = NULL;
                if (0 == __sync_bool_compare_and_swap(&lock->qtail, &n, flqnode(lock))) {
                    spin_then_yield(SPIN_LIMIT, NULL == (succ = readvol(n.next)));
#ifdef DEBUG
                    node_specific_arr[idx].stat.succ_wait += rdtsc() - now;
#endif
                    lock->qnext = succ;
                }
            } else {
                lock->qnext = succ;
            }
            // invariant: NULL == succ <=> lock->qtail == flqnode(lock)
            // printf("// invariant: NULL == succ <=> lock->qtail == flqnode(lock) succ = %d, nothing else\n", succ);

            now = rdtsc();
            node_specific_arr[idx].start_ticks = now;
            node_specific_arr[idx].slice = now + H_node_adj[H_node_adj[idx].parent].slice*CYCLE_PER_MS;
            lock->slice = node_specific_arr[idx].slice;
            lock->slice_valid = 1;
            // wake up successor if necessary
            if (succ) {
                succ->state = NEXT;
                futex(&succ->state, FUTEX_WAKE_PRIVATE, 1, NULL);
            }
            return;
        }
    }
}


void nl_fairlock_release(fairlock_t *lock, int idx) {
    ull now, cs;
#ifdef DEBUG
    ull succ_start = 0, succ_end = 0;
#endif

    qnode_t *succ = lock->qnext;
    if (NULL == succ) {
        if (__sync_bool_compare_and_swap(&lock->qtail, flqnode(lock), NULL))
            goto accounting;
#ifdef DEBUG
        succ_start = rdtsc();
#endif
        spin_then_yield(SPIN_LIMIT, NULL == (succ = readvol(lock->qnext)));
#ifdef DEBUG
        succ_end = rdtsc();
#endif
    }
    succ->state = RUNNABLE;

accounting:
    // invariant: NULL == succ || succ->state = RUNNABLE
    now = rdtsc();
    cs = now - node_specific_arr[idx].start_ticks;
    
    H_node temp = H_node_adj[H_node_adj[idx].parent];

    // node_specific_arr[idx].banned_until += (temp.num_children-1)*temp.slice * CYCLE_PER_MS;
    
    node_specific_arr[idx].banned_until += (tot_children_at_level[H_node_adj[idx].level]-1)*temp.slice * CYCLE_PER_MS;

    node_specific_arr[idx].banned_until += cs * (__atomic_load_n(&lock->total_weight, __ATOMIC_RELAXED) / node_specific_arr[idx].weight);         //(Penalty calucaltion and imposing)
    node_specific_arr[idx].banned = now < node_specific_arr[idx].banned_until;                                                                    // check for wether thread has crossed its slice
    // printf("node_specific_arr[idx].banned until %llu \n", node_specific_arr[idx].banned_until);
    if (node_specific_arr[idx].banned) {
        if (__sync_bool_compare_and_swap(&lock->slice_valid, 1, 0)) {
            futex(&lock->slice_valid, FUTEX_WAKE_PRIVATE, 1, NULL);
        }
    }
#ifdef DEBUG
    node_specific_arr[idx].stat.release_succ_wait += succ_end - succ_start;
#endif
}

#ifndef __LOCK_H__
#define __LOCK_H__

#if HRLOCK
#include "hrscl.h"
// typedef fairlock_t lock_t;                              /*has to be changed*/
#define lock_init(plock) hrlock_init(plock)
#define lock_acquire(plock, path_arr) hrlock_acquire(plock, path_arr)
#define lock_release(plock, path_arr) hrlock_release(plock, path_arr)

#endif

#endif // __LOCK_H__


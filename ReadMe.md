<font size = 3>

## Input Advice
To properly feed a input, <br>
Draw your hierarchy on a paper. Note that circles represent a non leaf nodes and squares represent leaf nodes. We followed 0-based indexing where the index of the root is 0. While giving numbering to the nodes make sure you are incrementing your index by doing a BFS traversal. 
<br>
Note that both leaf node indices and thread indices start with 0.


## Input Format
<ul>

<li>First line contains 4 integers nl, nt, max_l, t that corrseponds to number of non leaf nodes, number of threads, maximum level of the hierarchy and duration of the benchmark. </li>

<li>Then the following nl lines contains information about all non leaf node nl. Each line describes about a non leaf.
        <ul>
            <li>First three integers describes level, is_penultimate, number of children </li>
            <li> If is_penultimate == 1</li>
            <li> &emsp; there will be (number of children ) positive integers representing the id of the nodes</li>
            <li>else</li>
            <li> &emsp; there will be (number of children) pairs of integers {cs ,pr} where cs = critical section and pr = priority </li>
        </ul>
 </li>
</ul>

## Create microbenchmark and run the lock
Create a file with name in ***example/input*** folder "input5.txt" <br>
write your input in the file by following the input format instructions. <br>

Set your machine's speed in Makefile

```
make clean
make hrlock
./main input/input5.txt
```

## Description of the files

***rdtsc.h***     : This should not be changed. It is related to ticks.
<br> 

***common.h***    : This file contains assumptions and constants. In case you want to increase the bounds of hierarchy, make changes here. Note that your hardware may not support your bounds. So, change your bounds that are accountable to your hardware.
<br>

***struct.h***    : This file stores all the structures from hrscl.h, fairlock.h and node_fairlock.h. 
<br>

***node_fairlock.h*** : This file ensures fairness between non leaf nodes. Threads belonging to a particular group make a change on the common node structure. Basically, This file ensures threads belonging to the same group will have a common pass to traverse down in the hierarchy.
<br>

***fairlock.h***  :  This file ensures fairness between threads. Functions in this file are called at the penultimate level.
<br>

***hrscl.h***     :  This file contains recursive calls (Threads move up and down here). From initialization to hierarchy traversals happens over here. Main.c file directly refer functions from hrscl.h
<br>

***lock.h***      : This is a helper file. It contains macros that are directly called in main.c . To use different types of locks directly from main.c, create a macro in lock.h.

## How to read code?

1. start with main.c
2. main.c refers functions in hrscl.h through lock.h
3. hrscl.h uses functinons from two independent files, node_fairlock.h and fairlock.h
4. (Optional) wanted to add any sturcture for debugging purpose, add it in struct.h

## Issues need to be handled

1. When slice of a particular divison is active, when one thread make any change in the banned_until attribute, other threads of the same group should not make change on it. In other words, (anyone but only one)one thread of a group has to make change on the banned_until attribute per every valid slice.
2. Sometimes the lock is going into a state where complete output is not getting printed. But only partial output is getting printed. [In that case run the program again]
3. Poor performance (wastage of 9ms in 10 ms total duration when I run the lock with input/input.txt)


## Important Notes 

1. To get a specific thread information, we have pthread_getspecific and pthread_setspecific. To get a specific information for a group of threads, I created a node_specific_array.
2. In this code, threads traverse from top to bottom. So, they need to know path from root to leaf. Currently, I computed path in main.c. and gave the path in lock_acquire of main.c itself. 
3. node_fairlock.h and fairlock.h are almost similar except some tweaks.
4. We are following 0-based indexing, 0 is the root.
5. Our traditonal example(discussed in slides) is in input/input.txt

</font>

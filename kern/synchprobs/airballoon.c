/*
 * Driver code for airballoon problem
 */
#include <types.h>
#include <lib.h>
#include <thread.h>
#include <test.h>

#include <synch.h>


#define N_LORD_FLOWERKILLER 8
#define NROPES 16
static int ropes_left = NROPES;

/* Data structures for rope mappings */
struct rope {
    	bool severed;          
    	int rope_index;         
};
struct stake {
	
    	struct rope *rope;     
    	struct lock *lock;      
};
struct hook {
    	struct rope *rope;      
    	struct lock *lock;      
};
static struct stake stakes[NROPES];
static struct hook hooks[NROPES];
static struct rope ropes[NROPES];


/* Synchronization primitives */
static struct lock *ropes_left_lock;       
static struct cv *balloon_cv;              
static struct lock *balloon_lock;          
static struct semaphore *threads_done_sem; 
static struct lock *print_lock;   



/*
 * Describe your design and any invariants or locking protocols
 * that must be maintained. Explain the exit conditions. How
 * do all threads know when they are done?
 */

/* dandelion and marigold has similar logic. Only difference is that dandelion handles hooks, while marigold handles stakes. 
 * Logics are this:
 * 	- check remainder of ropes. If there's no more, then exit.
 * 	- pick a random hook or stake. 
 * 	- acquire lock for the hook or stake.
 *	- check the rope is exist and not severed from the hook and stake. If it is, then changed its status as severed and decrement counter. 
 *	- print message.
 *
 * Print function needs locks since it requires atoicity, since without lock, the output can be interleaved.
 */
static
void
dandelion(void *p, unsigned long arg)
{
	(void)p;
	(void)arg;

	kprintf("Dandelion thread starting\n");

	
	int remaining;
    	do {
        	lock_acquire(ropes_left_lock);
        	remaining = ropes_left;
        	lock_release(ropes_left_lock);
        
        	if (remaining > 0) {
            		int hook_index = random() % NROPES;
            
            		lock_acquire(hooks[hook_index].lock);
            
            		if (hooks[hook_index].rope != NULL && !hooks[hook_index].rope->severed) {
                		hooks[hook_index].rope->severed = true;
                		int rope_num = hooks[hook_index].rope->rope_index;
                
                		lock_acquire(ropes_left_lock);
                		ropes_left--;
                		if (ropes_left == 0) {
                    			lock_acquire(balloon_lock);
                    			cv_signal(balloon_cv, balloon_lock);
                    			lock_release(balloon_lock);
                		}
                		lock_release(ropes_left_lock);
                
                		lock_acquire(print_lock);
                		kprintf("Dandelion severed rope %d\n", rope_num);
                		lock_release(print_lock);
                
                		lock_release(hooks[hook_index].lock);
                		thread_yield();
            		} else {
                		lock_release(hooks[hook_index].lock);
            		}
        	}
    	} while (remaining > 0);
    	
    	kprintf("Dandelion thread done\n");
    	V(threads_done_sem);	
         	
}

static
void
marigold(void *p, unsigned long arg)
{
	(void)p;
	(void)arg;

	kprintf("Marigold thread starting\n");



	int remaining;
    	do {
        	lock_acquire(ropes_left_lock);
        	remaining = ropes_left;
        	lock_release(ropes_left_lock);

        	if (remaining > 0) {
            		int stake_index = random() % NROPES;

            		lock_acquire(stakes[stake_index].lock);

            		if (stakes[stake_index].rope != NULL && !stakes[stake_index].rope->severed) {
                		stakes[stake_index].rope->severed = true;
                		int rope_num = stakes[stake_index].rope->rope_index;

                		lock_acquire(ropes_left_lock);
                		ropes_left--;
                		if (ropes_left == 0) {
                    			lock_acquire(balloon_lock);
                    			cv_signal(balloon_cv, balloon_lock);
                    			lock_release(balloon_lock);
                		}
                		lock_release(ropes_left_lock);

                		lock_acquire(print_lock);
                		kprintf("Marigold severed rope %d from stake %d\n", rope_num, stake_index);
                		lock_release(print_lock);

                		lock_release(stakes[stake_index].lock);
                		thread_yield();
            		} else {
                		lock_release(stakes[stake_index].lock);
            		}
        	}
    	} while (remaining > 0);

    	kprintf("Marigold thread done\n");
    	V(threads_done_sem);
	
}


/* Logic for flowerKiller: 
 * 	- check if rope for working remains. If not, exit the code. 
 * 	- select two random stakes
 * 	- acquire locks in order for the two stakes.
 * 	- check both ropes are valid and not severed.
 * 	- swap rope pointers.
 * 	- print message.
 *
 */
static
void
flowerkiller(void *p, unsigned long arg)
{
	(void)p;
	(void)arg;

	kprintf("Lord FlowerKiller thread starting\n");
	
	
	int remaining;
    	do {
        	lock_acquire(ropes_left_lock);
        	remaining = ropes_left;
        	lock_release(ropes_left_lock);
        
        	if (remaining > 0) {
            		int stake1 = random() % NROPES;
            		int stake2 = random() % NROPES;
            
            		if (stake1 != stake2) { 
                		int first = stake1 < stake2 ? stake1 : stake2;
                		int second = stake1 < stake2 ? stake2 : stake1;
                
                		lock_acquire(stakes[first].lock);
                		lock_acquire(stakes[second].lock);
                
                		if (stakes[first].rope != NULL && !stakes[first].rope->severed && stakes[second].rope != NULL && !stakes[second].rope->severed) {
                    
                    			struct rope *temp = stakes[first].rope;
                    			stakes[first].rope = stakes[second].rope;
                    			stakes[second].rope = temp;
                    
                    			lock_acquire(print_lock);
                    			kprintf("Lord FlowerKiller switched rope %d from stake %d to stake %d\n", stakes[second].rope->rope_index, first, second);
                    			kprintf("Lord FlowerKiller switched rope %d from stake %d to stake %d\n", stakes[first].rope->rope_index, second, first);
                    			lock_release(print_lock);
                    
                    			lock_release(stakes[second].lock);
                    			lock_release(stakes[first].lock);
                    			thread_yield();
                		} else {
                    			lock_release(stakes[second].lock);
                    			lock_release(stakes[first].lock);
                		}
            		}
        	}
    	} while (remaining > 0);
    
    	kprintf("Lord FlowerKiller thread done\n");
    	V(threads_done_sem);
	
}


/* Logic for balloon:
 * 	- wait on conditional variable while rope works remain.
 * 	- if words are done and number of rope is 0, print message and exit. 
 */
static
void
balloon(void *p, unsigned long arg)
{
	(void)p;
	(void)arg;

	kprintf("Balloon thread starting\n");

	/* Implement this function */
	
	
	/* balloon_lock is for cv_wait	*/
	lock_acquire(balloon_lock);
    	
	/* rope_left_lock is required in here to get exact ropes_left */
	lock_acquire(ropes_left_lock);
    	while (ropes_left > 0) {
        	lock_release(ropes_left_lock);
        	cv_wait(balloon_cv, balloon_lock);
        	lock_acquire(ropes_left_lock);
    	}
    	lock_release(ropes_left_lock);
    	lock_release(balloon_lock);

    
    	lock_acquire(print_lock);
    	kprintf("Balloon freed and Prince Dandelion escapes!\n");
    	lock_release(print_lock);

    	kprintf("Balloon thread done\n");
    	V(threads_done_sem);

}


/* Change this function as necessary */
int
airballoon(int nargs, char **args)
{

	int err = 0, i;
    	(void)nargs;
    	(void)args;


	/* initialize ropes, stakes, and hooks */
    	for (i = 0; i < NROPES; i++) {
        	ropes[i].severed = false;
        	ropes[i].rope_index = i;

        	stakes[i].rope = &ropes[i];
        	stakes[i].lock = lock_create("stake");
        	if (stakes[i].lock == NULL) {
            		panic("airballoon: failed to create stake lock\n");
        	}

        	hooks[i].rope = &ropes[i];
        	hooks[i].lock = lock_create("hook");
        	if (hooks[i].lock == NULL) {
            		panic("airballoon: failed to create hook lock\n");
        	}
    	}

    
	/* initialize Synchronization primitives */
    	ropes_left = NROPES;
    	ropes_left_lock = lock_create("ropes_left");
    	balloon_cv = cv_create("balloon");
    	balloon_lock = lock_create("balloon");
    	print_lock = lock_create("print");
    	threads_done_sem = sem_create("threads_done", 0);


    	if (ropes_left_lock == NULL || balloon_cv == NULL || balloon_lock == NULL || print_lock == NULL || threads_done_sem == NULL) {
        	panic("airballoon: failed to create synchronization primitives\n");
    	}

    	/* create threads */
    	err = thread_fork("Marigold Thread", NULL, marigold, NULL, 0);
    	if(err)
        	goto panic;

    	err = thread_fork("Dandelion Thread", NULL, dandelion, NULL, 0);
    	if(err)
        	goto panic;
	

    	for (i = 0; i < N_LORD_FLOWERKILLER; i++) {
        	err = thread_fork("Lord FlowerKiller Thread", NULL, flowerkiller, NULL, 0);
        	if(err)
            		goto panic;
    	}

    	err = thread_fork("Air Balloon", NULL, balloon, NULL, 0);
    	if(err)
        	goto panic;

    	/* main thread will be continued after processing 11 threads.  */
    	for (i = 0; i < 11; i++) {  /* Number of total threads are 11. Dandelion(1) + merigold(1) + flowerkiller(8) + balloon(1) */
        	P(threads_done_sem);
    	}

    	/* cleanup */
    	for (i = 0; i < NROPES; i++) {
        	lock_destroy(stakes[i].lock);
        	lock_destroy(hooks[i].lock);
    	}
    	lock_destroy(ropes_left_lock);
    	lock_destroy(balloon_lock);
    	lock_destroy(print_lock);
    	cv_destroy(balloon_cv);
    	sem_destroy(threads_done_sem);

	/* main thread exit */
    	kprintf("Main thread done\n");
    	goto done;

panic:
    	panic("airballoon: thread_fork failed: %s)\n", strerror(err));

done:
    	return 0;
    
 
}

//============================================================
//
//  sdlwork.c - SDL OSD core work item functions
//
//  Copyright (c) 1996-2007, Nicola Salmoria and the MAME Team.
//  Visit http://mamedev.org for licensing and usage restrictions.
//
//  SDLMAME by Olivier Galibert and R. Belmont
//
//============================================================

// MinGW does not have pthreads, defer to Aaron's implementation on that platform
#if defined(SDLMAME_WIN32)
#include "../windows/winwork.c"
#elif defined(SDLMAME_OS2)	// use separate OS/2 implementation
#include "os2work.c"
#else

// standard headers
#include <time.h>
#if defined(SDLMAME_UNIX) && !defined(SDLMAME_DARWIN)
#include <sys/time.h>
#endif
#include <pthread.h>
#include <unistd.h>
#include "osdcore.h"
#include "osinline.h"

#ifdef SDLMAME_DARWIN
#include <mach/mach.h>
#include "osxutils.h"
#endif
#include "sdlsync.h"


//============================================================
//  DEBUGGING
//============================================================

#define KEEP_STATISTICS			(0)



//============================================================
//  PARAMETERS
//============================================================

#define INFINITE				(osd_ticks_per_second()*10000)
#define MAX_THREADS				(16)
#define SPIN_LOOP_TIME			(osd_ticks_per_second() / 1000)



//============================================================
//  MACROS
//============================================================

#if KEEP_STATISTICS
#define add_to_stat(v,x)		do { osd_interlocked_add((v), (x)); } while (0)
#define begin_timing(v)			do { (v) -= osd_profiling_ticks(); } while (0)
#define end_timing(v)			do { (v) += osd_profiling_ticks(); } while (0)
#else
#define add_to_stat(v,x)		do { } while (0)
#define begin_timing(v)			do { } while (0)
#define end_timing(v)			do { } while (0)
#endif



//============================================================
//  TYPE DEFINITIONS
//============================================================

typedef struct _scalable_lock scalable_lock;
struct _scalable_lock
{
   struct
   {
      volatile INT32 	haslock;		// do we have the lock?
      UINT8 			filler[60];		// assumes a 64-byte cache line
   } slot[MAX_THREADS];					// one slot per thread
   volatile INT32 		nextindex;		// index of next slot to use
};


typedef struct _mame_thread_info mame_thread_info;
struct _mame_thread_info
{
	osd_work_queue *	queue;			// pointer back to the queue
	osd_thread *		handle;			// handle to the thread
	osd_event *			wakeevent;		// wake event for the thread
	volatile UINT8		active;			// are we actively processing work?

#if KEEP_STATISTICS
	osd_ticks_t			runtime;
	osd_ticks_t			spintime;
	osd_ticks_t			waittime;
#endif
};


struct _osd_work_queue
{
	scalable_lock	 	lock;			// lock for protecting the queue
	osd_work_item * volatile list;		// list of items in the queue
	osd_work_item ** volatile tailptr;	// pointer to the tail pointer of work items in the queue
	osd_work_item * volatile free;		// free list of work items
	volatile INT32		items;			// items in the queue
	volatile INT32		livethreads;	// number of live threads
	volatile UINT8		waiting;		// is someone waiting on the queue to complete?
	volatile UINT8		exiting;		// should the threads exit on their next opportunity?
	UINT32				threads;		// number of threads in this queue
	UINT32				flags;			// creation flags
	mame_thread_info *	thread;			// array of thread information
	osd_event	*		doneevent;		// event signalled when work is complete

#if KEEP_STATISTICS
	volatile INT32		itemsqueued;	// total items queued
	volatile INT32		setevents;		// number of times we called SetEvent
	volatile INT32		extraitems;		// how many extra items we got after the first in the queue loop
	volatile INT32		spinloops;		// how many times spinning bought us more items
#endif
};


struct _osd_work_item
{
	osd_work_item *		next;			// pointer to next item
	osd_work_queue *	queue;			// pointer back to the owning queue
	osd_work_callback 	callback;		// callback function
	void *				param;			// callback parameter
	void *				result;			// callback result
	osd_event *			event;			// event signalled when complete
	UINT32				flags;			// creation flags
	volatile UINT8		done;			// is the item done?
};



//============================================================
//  FUNCTION PROTOTYPES
//============================================================

static int effective_num_processors(void);
static void * worker_thread_entry(void *param);
static void worker_thread_process(osd_work_queue *queue, mame_thread_info *thread);

//============================================================
//  INLINE FUNCTIONS
//============================================================

#ifndef osd_exchange32
INLINE INT32 osd_exchange32(INT32 volatile *ptr, INT32 exchange)
{
	INT32 origvalue;
	do {
		origvalue = *ptr;
	} while (osd_compare_exchange32(ptr, origvalue, exchange) != origvalue);
	return origvalue;
}
#endif

#ifndef osd_interlocked_increment
INLINE INT32 osd_interlocked_increment(INT32 volatile *ptr)
{
	return osd_sync_add(ptr, 1);
}
#endif

#ifndef osd_interlocked_decrement
INLINE INT32 osd_interlocked_decrement(INT32 volatile *ptr)
{
	return osd_sync_add(ptr, -1);
}
#endif

#ifndef osd_interlocked_add
INLINE INT32 osd_interlocked_add(INT32 volatile *ptr, INT32 add)
{
	return osd_sync_add(ptr, add);
}
#endif

INLINE void scalable_lock_init(scalable_lock *lock)
{
	memset(lock, 0, sizeof(*lock));
	lock->slot[0].haslock = TRUE;
}


INT32 scalable_lock_acquire(scalable_lock *lock)
{
	INT32 myslot = (osd_interlocked_increment(&lock->nextindex) - 1) & (MAX_THREADS - 1);
	INT32 backoff = 1;

	while (!osd_compare_exchange32(&lock->slot[myslot].haslock, TRUE, FALSE))
	{
		INT32 backcount;
		for (backcount = 0; backcount < backoff; backcount++)
			osd_yield_processor();
		backoff <<= 1;
	}
	return myslot;
}

void scalable_lock_release(scalable_lock *lock, INT32 myslot)
{
	osd_exchange32(&lock->slot[(myslot + 1) & (MAX_THREADS - 1)].haslock, TRUE);
}


//============================================================
//  osd_work_queue_alloc
//============================================================

osd_work_queue *osd_work_queue_alloc(int flags)
{
	int numprocs = effective_num_processors();
	osd_work_queue *queue;
	int threadnum;

	// allocate a new queue
	queue = malloc(sizeof(*queue));
	if (queue == NULL)
		goto error;
	memset(queue, 0, sizeof(*queue));

	// initialize basic queue members
	queue->tailptr = (osd_work_item **)&queue->list;
	queue->flags = flags;

	// allocate events for the queue
	queue->doneevent = osd_event_alloc(TRUE, TRUE);		// manual reset, signalled
	if (queue->doneevent == NULL)
		goto error;

	// initialize the critical section
	scalable_lock_init(&queue->lock);

	// determine how many threads to create...
	// on a single-CPU system, create 1 thread for I/O queues, and 0 threads for everything else
	if (numprocs == 1)
		queue->threads = (flags & WORK_QUEUE_FLAG_IO) ? 1 : 0;

	// on an n-CPU system, create (n-1) threads for multi queues, and 1 thread for everything else
	else
		queue->threads = (flags & WORK_QUEUE_FLAG_MULTI) ? (numprocs - 1) : 1;

	// clamp to the maximum
	queue->threads = MIN(queue->threads, MAX_THREADS);

	// allocate memory for thread array (+1 to count the calling thread)
	queue->thread = malloc((queue->threads + 1) * sizeof(queue->thread[0]));
	if (queue->thread == NULL)
		goto error;
	memset(queue->thread, 0, (queue->threads + 1) * sizeof(queue->thread[0]));

	// iterate over threads
	for (threadnum = 0; threadnum < queue->threads; threadnum++)
	{
		mame_thread_info *thread = &queue->thread[threadnum];

		// set a pointer back to the queue
		thread->queue = queue;

		// create the per-thread wake event
		thread->wakeevent = osd_event_alloc(FALSE, FALSE);	// auto-reset, not signalled
		if (thread->wakeevent == NULL)
			goto error;

		// create the thread
		thread->handle = osd_thread_create(worker_thread_entry, thread);
		if (thread->handle == NULL)
			goto error;

		// set its priority: I/O threads get high priority because they are assumed to be
		// blocked most of the time; other threads just match the creator's priority
		if (flags & WORK_QUEUE_FLAG_IO)
			osd_thread_adjust_priority(thread->handle, 0);	// TODO: specify appropriate priority
		else
			osd_thread_adjust_priority(thread->handle, 0);	// TODO: specify appropriate priority
	}

	// start a timer going for "waittime" on the main thread
	begin_timing(queue->thread[queue->threads].waittime);
	return queue;

error:
	osd_work_queue_free(queue);
	return NULL;
}


//============================================================
//  osd_work_queue_items
//============================================================

int osd_work_queue_items(osd_work_queue *queue)
{
	// return the number of items currently in the queue
	return queue->items;
}


//============================================================
//  osd_work_queue_wait
//============================================================

int osd_work_queue_wait(osd_work_queue *queue, osd_ticks_t timeout)
{
	// if no threads, no waiting
	if (queue->threads == 0)
		return TRUE;

	// if no items, we're done
	if (queue->items == 0)
		return TRUE;

	// if this is a multi queue, help out rather than doing nothing
	if (queue->flags & WORK_QUEUE_FLAG_MULTI)
	{
		mame_thread_info *thread = &queue->thread[queue->threads];

		end_timing(thread->waittime);

		// process what we can as a worker thread
		worker_thread_process(queue, thread);

		begin_timing(thread->waittime);
		return TRUE;
	}

	// reset our done event and double-check the items before waiting
	osd_event_reset(queue->doneevent);
	queue->waiting = TRUE;
	if (queue->items != 0)
	{
		osd_event_wait(queue->doneevent, timeout);
	}
	queue->waiting = FALSE;

	// return TRUE if we actually hit 0
	return (queue->items == 0);
}


//============================================================
//  osd_work_queue_free
//============================================================

void osd_work_queue_free(osd_work_queue *queue)
{
	// if we have threads, clean them up
	if (queue->threads > 0 && queue->thread != NULL)
	{
		int threadnum;

		// stop the timer for "waittime" on the main thread
		end_timing(queue->thread[queue->threads].waittime);

		// signal all the threads to exit
		//osd_work_queue_wait(queue, osd_ticks_per_second()*10);
		queue->exiting = TRUE;
		for (threadnum = 0; threadnum < queue->threads; threadnum++)
		{
			mame_thread_info *thread = &queue->thread[threadnum];
			if (thread->wakeevent != NULL)
				osd_event_set(thread->wakeevent);
		}

		// wait for all the threads to go away
		for (threadnum = 0; threadnum < queue->threads; threadnum++)
		{
			mame_thread_info *thread = &queue->thread[threadnum];

			// block on the thread going away, then close the handle
			if (thread->handle != NULL)
			{
				osd_thread_wait_free(thread->handle);
			}

			// clean up the wake event
			if (thread->wakeevent != NULL)
				osd_event_free(thread->wakeevent);
		}

#if KEEP_STATISTICS
		// output per-thread statistics
		for (threadnum = 0; threadnum <= queue->threads; threadnum++)
		{
			mame_thread_info *thread = &queue->thread[threadnum];
			osd_ticks_t total = thread->runtime + thread->waittime + thread->spintime;
			printf("Thread %d:  run=%5.2f%%  spin=%5.2f%%  wait/other=%5.2f%%\n",
					threadnum,
					(double)thread->runtime * 100.0 / (double)total,
					(double)thread->spintime * 100.0 / (double)total,
					(double)thread->waittime * 100.0 / (double)total);
		}
#endif

		// free the list
		free(queue->thread);
	}

	// free all the events
	if (queue->doneevent != NULL)
		osd_event_free(queue->doneevent);

	// free all items in the free list
	while (queue->free != NULL)
	{
		osd_work_item *item = (osd_work_item *)queue->free;
		queue->free = item->next;
		if (item->event != NULL)
			osd_event_free(item->event);
		free(item);
	}

	// free all items in the active list
	while (queue->list != NULL)
	{
		osd_work_item *item = (osd_work_item *)queue->list;
		queue->list = item->next;
		if (item->event != NULL)
			osd_event_free(item->event);
		free(item);
	}

#if KEEP_STATISTICS
	printf("Items queued   = %9d\n", queue->itemsqueued);
	printf("SetEvent calls = %9d\n", queue->setevents);
	printf("Extra items    = %9d\n", queue->extraitems);
	printf("Spin loops     = %9d\n", queue->spinloops);
#endif

	// free the queue itself
	free(queue);
}


//============================================================
//  osd_work_item_queue_multiple
//============================================================

osd_work_item *osd_work_item_queue_multiple(osd_work_queue *queue, osd_work_callback callback, INT32 numitems, void *parambase, INT32 paramstep, UINT32 flags)
{
	osd_work_item *itemlist = NULL;
	osd_work_item **item_tailptr = &itemlist;
	INT32 lockslot;
	int itemnum;

	// loop over items, building up a local list of work
	for (itemnum = 0; itemnum < numitems; itemnum++)
	{
		osd_work_item *item;

		// first allocate a new work item; try the free list first
		do
		{
			item = (osd_work_item *)queue->free;
		} while (item != NULL && osd_compare_exchange_ptr((void * volatile *)&queue->free, item, item->next) != item);

		// if nothing, allocate something new
		if (item == NULL)
		{
			// allocate the item
			item = malloc(sizeof(*item));
			if (item == NULL)
				return NULL;
			item->event = NULL;
			item->queue = queue;
		}

		// fill in the basics
		item->next = NULL;
		item->callback = callback;
		item->param = parambase;
		item->result = NULL;
		item->flags = flags;
		item->done = FALSE;

		// advance to the next
		*item_tailptr = item;
		item_tailptr = &item->next;
		parambase = (UINT8 *)parambase + paramstep;
	}

	// enqueue the whole thing within the critical section
	lockslot = scalable_lock_acquire(&queue->lock);
	*queue->tailptr = itemlist;
	queue->tailptr = item_tailptr;
	scalable_lock_release(&queue->lock, lockslot);

	// increment the number of items in the queue
	osd_interlocked_add(&queue->items, numitems);
	add_to_stat(&queue->itemsqueued, numitems);

	// look for free threads to do the work
	if (queue->livethreads < queue->threads)
	{
		int threadnum;

		// iterate over all the threads
		for (threadnum = 0; threadnum < queue->threads; threadnum++)
		{
			mame_thread_info *thread = &queue->thread[threadnum];

			// if this thread is not active, wake him up
			if (!thread->active)
			{
				osd_event_set(thread->wakeevent);
				add_to_stat(&queue->setevents, 1);

				// for non-shared, the first one we find is good enough
				if (--numitems == 0)
					break;
			}
		}
	}

	// if no threads, run the queue now on this thread
	if (queue->threads == 0)
		worker_thread_process(queue, &queue->thread[0]);

	// only return the item if it won't get released automatically
	//return (flags & WORK_ITEM_FLAG_AUTO_RELEASE) ? NULL : *item_tailptr;
	return (flags & WORK_ITEM_FLAG_AUTO_RELEASE) ? NULL : itemlist;
}


//============================================================
//  osd_work_item_wait
//============================================================

int osd_work_item_wait(osd_work_item *item, osd_ticks_t timeout)
{
	// if we're done already, just return
	if (item->done)
		return TRUE;

	// if we don't have an event, create one
	if (item->event == NULL)
		item->event = osd_event_alloc(TRUE, FALSE);		// manual reset, not signalled
	else
		 osd_event_reset(item->event);

	// if we don't have an event, we need to spin (shouldn't ever really happen)
	if (item->event == NULL)
	{
		osd_ticks_t stopspin = osd_ticks() + timeout;
		while (!item->done && osd_ticks() < stopspin)
			osd_yield_processor();
	}

	// otherwise, block on the event until done
	else if (!item->done)
		osd_event_wait(item->event, timeout);

	// return TRUE if the refcount actually hit 0
	return item->done;
}


//============================================================
//  osd_work_item_result
//============================================================

void *osd_work_item_result(osd_work_item *item)
{
	return item->result;
}


//============================================================
//  osd_work_item_release
//============================================================

void osd_work_item_release(osd_work_item *item)
{
	osd_work_item *next;

	// make sure we're done first
	osd_work_item_wait(item, 100 * osd_ticks_per_second());

	// add us to the free list on our queue
	do
	{
		next = (osd_work_item *)item->queue->free;
		item->next = next;
	} while (osd_compare_exchange_ptr((void * volatile *)&item->queue->free, next, item) != next);
}


//============================================================
//  effective_num_processors
//============================================================

static int effective_num_processors(void)
{
	char *procsoverride;
	int numprocs = 0;

	// if the OSDPROCESSORS environment variable is set, use that value if valid
	procsoverride = getenv("OSDPROCESSORS");
	if (procsoverride != NULL && sscanf(procsoverride, "%d", &numprocs) == 1 && numprocs > 0)
		return numprocs;

	// otherwise, fetch the info from the system
	return osd_num_processors();
}


//============================================================
//  worker_thread_entry
//============================================================

static void *worker_thread_entry(void *param)
{
	mame_thread_info *thread = param;
	osd_work_queue *queue = thread->queue;

	// loop until we exit
	for ( ;; )
	{
		// block waiting for work or exit
		// bail on exit, and only wait if there are no pending items in queue
		if (!queue->exiting && queue->items == 0)
		{
			begin_timing(thread->waittime);
			osd_event_wait(thread->wakeevent, INFINITE);
			end_timing(thread->waittime);
		}
		if (queue->exiting)
			break;

		// indicate that we are live
		thread->active = TRUE;
		osd_interlocked_increment(&queue->livethreads);

		// process work items
		for ( ;; )
		{
			osd_ticks_t stopspin;

			// process as much as we can
			worker_thread_process(queue, thread);

			// spin for a while looking for more work
			begin_timing(thread->spintime);
			stopspin = osd_ticks() + SPIN_LOOP_TIME;
			while (queue->items == 0 && osd_ticks() < stopspin)
				osd_yield_processor();
			end_timing(thread->spintime);

			// if nothing more, release the processor
			if (queue->items == 0)
				break;
			add_to_stat(&queue->spinloops, 1);
		}

		// decrement the live thread count
		thread->active = FALSE;
		osd_interlocked_decrement(&queue->livethreads);
	}
	return NULL;
}


//============================================================
//  worker_thread_process
//============================================================

static void worker_thread_process(osd_work_queue *queue, mame_thread_info *thread)
{
	begin_timing(thread->runtime);

	// loop until everything is processed
	while (queue->items != 0)
	{
		osd_work_item *item;
		INT32 lockslot;

		// use a critical section to synchronize the removal of items
		lockslot = scalable_lock_acquire(&queue->lock);
		{
			// pull the item from the queue
			item = (osd_work_item *)queue->list;

			if (item != NULL)
			{
				queue->list = item->next;
				if (queue->list == NULL)
					queue->tailptr = (osd_work_item **)&queue->list;
			}
		}
		scalable_lock_release(&queue->lock, lockslot);

		// process non-NULL items
		if (item != NULL)
		{
			// call the callback and stash the result
			item->result = (*item->callback)(item->param);
			osd_interlocked_decrement(&queue->items);
			item->done = TRUE;

			// if it's an auto-release item, release it
			if (item->flags & WORK_ITEM_FLAG_AUTO_RELEASE)
				osd_work_item_release(item);

			// set the result and signal the event
			else if (item->event != NULL)
			{
				osd_event_set(item->event);
				add_to_stat(&item->queue->setevents, 1);
			}

			// if we removed an item and there's still work to do, bump the stats
			if (queue->items != 0)
				add_to_stat(&queue->extraitems, 1);
		}
	}

	// we don't need to set the doneevent for multi queues because they spin
	if (queue->waiting)
	{
		osd_event_set(queue->doneevent);
		add_to_stat(&queue->setevents, 1);
	}

	end_timing(thread->runtime);
}

#endif	// SDLMAME_WIN32

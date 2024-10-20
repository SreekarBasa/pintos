#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "devices/pit.h"
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "threads/thread.h"

#include <stdlib.h>
#include "list.h"
//should keep track of all threads that are currently sleeping
// and their wake-up time
static struct list sleeping_threads_list;
struct sleeping_thread{ //structure for a sleeping thread, pointer to a thread
  struct thread *t; // thread which is sleeping
  int64_t wake_up_time; //wake up time of thread
  struct list_elem elem; //used to link sleeping_thread instances into a list
  // doubly-linked list
  // for ordered lists this is useful
};

struct lock sleeper_lock; //lock for sleeping_threads_list

void init_sleeper_lock(void);
bool compare_wake_up_time(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED);
void timer_wakeup(void);
static bool wakeup_needed;

void
init_sleeper_lock(void){ //function for initialization
  lock_init(&sleeper_lock); //lock initialization
}

/* See [8254] for hardware details of the 8254 timer chip. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* Number of timer ticks since OS booted. */
static int64_t ticks;

/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);
static void real_time_delay (int64_t num, int32_t denom);

/* Sets up the timer to interrupt TIMER_FREQ times per second,
   and registers the corresponding interrupt. */
void
timer_init (void) 
{
  list_init(&sleeping_threads_list); // initialize sleeping thread list
  pit_configure_channel (0, 2, TIMER_FREQ);
  intr_register_ext (0x20, timer_interrupt, "8254 Timer");
}

/* Calibrates loops_per_tick, used to implement brief delays. */
void
timer_calibrate (void) 
{
  unsigned high_bit, test_bit;

  ASSERT (intr_get_level () == INTR_ON);
  printf ("Calibrating timer...  ");

  /* Approximate loops_per_tick as the largest power-of-two
     still less than one timer tick. */
  loops_per_tick = 1u << 10;
  while (!too_many_loops (loops_per_tick << 1)) 
    {
      loops_per_tick <<= 1;
      ASSERT (loops_per_tick != 0);
    }

  /* Refine the next 8 bits of loops_per_tick. */
  high_bit = loops_per_tick;
  for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
    if (!too_many_loops (loops_per_tick | test_bit))
      loops_per_tick |= test_bit;

  printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted. */
int64_t
timer_ticks (void) 
{
  enum intr_level old_level = intr_disable ();
  int64_t t = ticks;
  intr_set_level (old_level);
  return t;
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */
int64_t
timer_elapsed (int64_t then) 
{
  return timer_ticks () - then;
}


bool
compare_wake_up_time(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) //aux?
{
  struct sleeping_thread *st_a = list_entry(a, struct sleeping_thread, elem);
  struct sleeping_thread *st_b = list_entry(b, struct sleeping_thread, elem);

  return st_a->wake_up_time < st_b->wake_up_time; // less -> true
}

/* Sleeps for approximately TICKS timer ticks.  Interrupts must
   be turned on. */
void
timer_sleep (int64_t ticks) //argument - ticks.. no.of ticks needed to wait 
{                           // now we will block instead of busy wait
  if(ticks<=0){
    return; // can avoid this / optional
  }

  int64_t start = timer_ticks (); //current time since sys started
  int64_t wake_up_time = start + ticks; //added

  lock_acquire(&sleeper_lock); //acquire the lock --task 3

  //sleeping thread
  struct sleeping_thread *st = malloc(sizeof(struct sleeping_thread));
  if(st==NULL){
    PANIC("allocation problem"); //for debugging
  }
  st->t = thread_current(); // to get current thread
  st->wake_up_time = wake_up_time; //set wake up time

  // ASSERT(list_is_initialized(&sleeping_threads_list)); // to verify initialization

  // enum intr_level old_level = intr_disable(); --previous method
  // to avoid race contd just like in timer ticks function
  //now we need to insert the thread to sleeping_threads_list, need to be in sorted order of wake-up-times
  // passing the element
  //?

  if(list_empty(&sleeping_threads_list)){ //empty case
    list_push_back(&sleeping_threads_list, &st->elem);
  }else{ //sort if >1 elements only
    list_insert_ordered(&sleeping_threads_list, &st->elem, compare_wake_up_time, NULL); // aux? 
  }
  lock_release(&sleeper_lock); // release the lock -- task3
  thread_block();
  // intr_set_level(old_level); --task 2 //restore interrupts /states
}

void
timer_wakeup(void){ //new custom function to manage synchronization using locks
  // acquire the lock for synchronization
  while(true){
    if(wakeup_needed){
      wakeup_needed = false;
      lock_acquire(&sleeper_lock);

      int64_t curr = timer_ticks(); // current ticks
      while(!list_empty(&sleeping_threads_list)){
        struct list_elem *e = list_front(&sleeping_threads_list);
        struct sleeping_thread *st = list_entry(e, struct sleeping_thread, elem);
        if(st->wake_up_time > curr){
          break; // not ready yet
        }
        list_pop_front(&sleeping_threads_list); // remove ready unblocking thread instance
        thread_unblock(st->t); //unblock the thread in sleeping thread instance
        free(st); //freeing the memory
      }
      lock_release(&sleeper_lock); //release lock
    }
    enum intr_level old_level = intr_enable();  // Enable interrupts
    timer_msleep(1);  // Small sleep 
    intr_set_level(old_level);  // Restore interrupt level
  }
}

/* Sleeps for approximately MS milliseconds.  Interrupts must be
   turned on. */
void
timer_msleep (int64_t ms) 
{
  real_time_sleep (ms, 1000);
}

/* Sleeps for approximately US microseconds.  Interrupts must be
   turned on. */
void
timer_usleep (int64_t us) 
{
  real_time_sleep (us, 1000 * 1000);
}

/* Sleeps for approximately NS nanoseconds.  Interrupts must be
   turned on. */
void
timer_nsleep (int64_t ns) 
{
  real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* Busy-waits for approximately MS milliseconds.  Interrupts need
   not be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_msleep()
   instead if interrupts are enabled. */
void
timer_mdelay (int64_t ms) 
{
  real_time_delay (ms, 1000);
}

/* Sleeps for approximately US microseconds.  Interrupts need not
   be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_usleep()
   instead if interrupts are enabled. */
void
timer_udelay (int64_t us) 
{
  real_time_delay (us, 1000 * 1000);
}

/* Sleeps execution for approximately NS nanoseconds.  Interrupts
   need not be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_nsleep()
   instead if interrupts are enabled.*/
void
timer_ndelay (int64_t ns) 
{
  real_time_delay (ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void
timer_print_stats (void) 
{
  printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

/* Timer interrupt handler. */
static void
timer_interrupt (struct intr_frame *args UNUSED)
{
  ticks++; //global tick count ++
  thread_tick (); //for thread scheduling
  timer_wakeup();
  wakeup_needed = true;
  // while(!list_empty(&sleeping_threads_list)){ // till all wake up
  //   //elements of linked list
  //   struct list_elem *e = list_front(&sleeping_threads_list); //like getting front element in list
  //   //now e is a struct sleeping_thread instance pointer,
  
  //   struct sleeping_thread *st = list_entry(e, struct sleeping_thread, elem); 
  //   //get element from that e, which is a pointer
  //   // elem helped to fetch the thread from the list

  //   if(st->wake_up_time>ticks){ //not ready to wake up
  //     break;
  //   }
  //   list_pop_front(&sleeping_threads_list); //remove the woken thread
  //   thread_unblock(st->t); // t is the actual thread, passing it to unblock
  // }
}

/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */
static bool
too_many_loops (unsigned loops) 
{
  /* Wait for a timer tick. */
  int64_t start = ticks;
  while (ticks == start)
    barrier ();

  /* Run LOOPS loops. */
  start = ticks;
  busy_wait (loops);

  /* If the tick count changed, we iterated too long. */
  barrier ();
  return start != ticks;
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. */
static void NO_INLINE
busy_wait (int64_t loops) 
{
  while (loops-- > 0)
    barrier ();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep (int64_t num, int32_t denom) 
{
  /* Convert NUM/DENOM seconds into timer ticks, rounding down.
          
        (NUM / DENOM) s          
     ---------------------- = NUM * TIMER_FREQ / DENOM ticks. 
     1 s / TIMER_FREQ ticks
  */
  int64_t ticks = num * TIMER_FREQ / denom;

  ASSERT (intr_get_level () == INTR_ON);
  if (ticks > 0)
    {
      /* We're waiting for at least one full timer tick.  Use
         timer_sleep() because it will yield the CPU to other
         processes. */                
      timer_sleep (ticks); 
    }
  else 
    {
      /* Otherwise, use a busy-wait loop for more accurate
         sub-tick timing. */
      real_time_delay (num, denom); 
    }
}

/* Busy-wait for approximately NUM/DENOM seconds. */
static void
real_time_delay (int64_t num, int32_t denom)
{
  /* Scale the numerator and denominator down by 1000 to avoid
     the possibility of overflow. */
  ASSERT (denom % 1000 == 0);
  busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000)); 
}

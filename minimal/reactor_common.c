/* Runtime infrastructure for the C target of Lingua Franca. */

/*************
Copyright (c) 2019, The University of California at Berkeley.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
***************/

/** Runtime infrastructure for the C target of Lingua Franca.
 *  This file contains resources that are shared by the threaded and
 *  non-threaded versions of the C runtime.
 *  
 *  @author{Edward A. Lee <eal@berkeley.edu>}
 *  @author{Marten Lohstroh <marten@berkeley.edu>}
 *  @author{Mehrdad Niknami <mniknami@berkeley.edu>}
 */
#include "reactor.h"

/** Global constants. */
bool False = false;
bool True = true;

// Indicator of whether to wait for physical time to match logical time.
// By default, execution will wait. The command-line argument -fast will
// eliminate the wait and allow logical time to exceed physical time.
bool fast = false;

// By default, execution is not threaded and this variable will have value 0.
int number_of_threads = 0;

unsigned int number_of_reactions = 100; // FIXME: set this accordingly

// Current time in nanoseconds since January 1, 1970.
// This is not in scope for reactors.
instant_t current_time = 0LL;

// Logical time at the start of execution.
interval_t start_time = 0LL;

// Physical time at the start of the execution.
struct timespec physicalStartTime;

// Indicator that the execution should stop after the completion of the
// current logical time. This can be set to true by calling the stop()
// function in a reaction.
bool stop_requested = false;

// Duration, or -1 if no timeout time has been given.
instant_t duration = -1LL;

// Stop time, or 0 if no timeout time has been given.
instant_t stop_time = 0LL;

// Indicator of whether the keepalive command-line option was given.
bool keepalive_specified = false;

/////////////////////////////
// The following functions are in scope for all reactors:

/** Return the elpased logical time in nanoseconds since the start of execution. */
interval_t get_elapsed_logical_time() {
    return current_time - start_time;
}

/** Return the current logical time in nanoseconds since January 1, 1970. */
instant_t get_logical_time() {
    return current_time;
}

/** Return the current physical time in nanoseconds since January 1, 1970. */
instant_t get_physical_time() {
    struct timespec physicalTime;
    clock_gettime(CLOCK_REALTIME, &physicalTime);
    return physicalTime.tv_sec * BILLION + physicalTime.tv_nsec;
}

/** Return the elapsed physical time in nanoseconds. */
instant_t get_elapsed_physical_time() {
    struct timespec physicalTime;
    clock_gettime(CLOCK_REALTIME, &physicalTime);
    return physicalTime.tv_sec * BILLION + physicalTime.tv_nsec -
    (physicalStartTime.tv_sec * BILLION + physicalStartTime.tv_nsec);
}

/**
 * Specialized version of malloc used by Lingua Franca for action values
 * and messages contained in dynamically allocated memory.
 * @param size The size of the memory block to allocate.
 * @return A pointer to the allocated memory block.
 */
void* lf_malloc(size_t size) {
	// NOTE: For now, this just delegates to malloc.
	// But in the future, we expect to use it to attach a reference count to the
	// allocated object.
	return malloc(size);
}

/////////////////////////////
// The following is not in scope for reactors:

// Priority queues.
pqueue_t* event_q;     // For sorting by time.
pqueue_t* blocked_q;   // To store reactions that are blocked by other reactions.
pqueue_t* transfer_q;  // To store reactions that are still blocked by other reactions.
pqueue_t* reaction_q;  // For sorting by deadline.
pqueue_t* recycle_q;   // For recycling malloc'd events.
pqueue_t* free_q;      // For free malloc'd values carried by events.

handle_t __handle = 1;


// ********** Priority Queue Support Start
// FIXME: rename these

// Compare two priorities.
static int cmp_pri(pqueue_pri_t next, pqueue_pri_t curr) {
    return (next > curr);
}
// Compare two events. They are "equal" if they refer to the same trigger.
static int eql_evt(void* next, void* curr) {
    return (((event_t*)next)->trigger == ((event_t*)curr)->trigger);
}
// Compare two reactions.
static int eql_rct(void* next, void* curr) {
    return (next == curr);
}
// Get priorities based on time.
// Used for sorting event_t structs.
static pqueue_pri_t get_event_time(void *a) {
    return (pqueue_pri_t)(((event_t*) a)->time);
}

// Get priorities based on indices, which reflect topological sort.
// Used for sorting reaction_t structs.
static pqueue_pri_t get_reaction_index(void *a) {
    return ((reaction_t*) a)->index;
}

static pqueue_pri_t get_reaction_deadline(void *a) {
    return ((reaction_t*) a)->local_deadline;
}

// Get position in the queue of the specified event.
static size_t get_event_position(void *a) {
    return ((event_t*) a)->pos;
}

// Get position in the queue of the specified event.
static size_t get_reaction_position(void *a) {
    return ((reaction_t*) a)->pos;
}

// Set position of the specified event.
static void set_event_position(void *a, size_t pos) {
    ((event_t*) a)->pos = pos;
}

// Set position of the specified event.
static void set_reaction_position(void *a, size_t pos) {
    ((reaction_t*) a)->pos = pos;
}

static void print_reaction(FILE *out, void *a) {
	reaction_t *n = a;
    fprintf(out, "index: %lld, reaction: %p\n",
			n->index, n);
}

static void print_event(FILE *out, void *a) {
	event_t *n = a;
    fprintf(out, "time: %lld, trigger: %p, value: %p\n",
			n->time, n->trigger, n->value);
}

// ********** Priority Queue Support End

// Counter used to issue a warning if memory is allocated and never freed.
static int __count_allocations;

// Library function to decrement the reference count and free
// the memory, if appropriate, for messages carried by a token_t struct.
void __done_using(token_t* token) {
    token->ref_count--;
    // printf("****** After reacting, ref_count = %d.\n", token->ref_count);
    if (token->ref_count == 0) {
        // Count frees to issue a warning if this is never freed.
        __count_allocations--;
        // printf("****** Freeing allocated memory.\n");
        free(token->value);
    }
}

bool is_blocked_by(pqueue_t* q, reaction_t* reaction) {
    // Check if there is another reaction ready to execute 
    // that has precedence over the given reaction.
    for (int i = 1; i < q->size; i++) {
        reaction_t* ready = q->d[i];
        if ((reaction->chain_id & ready->chain_id != 0) 
                && reaction->index > ready->index) {
            printf("Reaction blocked!\n");
            return true;
        }
    }
    printf("Reaction not blocked.\n");
    return false;
}

// Schedule the specified trigger at current_time plus the
// offset of the specified trigger plus the delay.
// The value is required to be a pointer returned by malloc
// because it will be freed after having been delivered to
// all relevant destinations unless it is NULL, in which case
// it will be ignored. If the trigger offset plus the extra
// delay is greater than zero and stop has been requested,
// then ignore this and return 0.
// Otherwise, return a handle to the scheduled trigger,
// which is an integer greater than 0.
handle_t __schedule(trigger_t* trigger, interval_t extra_delay, void* value) {
    // Compute the tag.  How we do that depends on whether
	// this is a logical or physical action.
    interval_t tag = current_time;
    event_t* existing = NULL;

    // Recycle event_t structs, if possible.    
    event_t* e = pqueue_pop(recycle_q);
    if (e == NULL) {
        e = malloc(sizeof(struct event_t));
    }
    
    e->trigger = trigger;
    e->value = value;
    // For logical actions, the logical time of the new event is just
    // the current logical time plus the minimum offset (action parameter)
    // plus the extra delay specified in the call to schedule.
    e->time = tag + trigger->offset + extra_delay;

    if (trigger->is_physical) {
    	// If the trigger is physical, then we need to use
    	// physical time and the time of the last invocation to adjust the tag.
    	// Specifically, the timestamp assigned to the action event will be
    	// the maximum of the current logical time, the
    	// current physical time, and the time of last
    	// invocation plus the minTime (action parameter) plus the
    	// extra_delay (argument to this function).
    	// If the action has never been scheduled before, then the
    	// timestamp will be the maximum of the current logical time,
    	// the current physical time,
    	// and the start time + minTime + extra_delay.

        // Get the current physical time.
        struct timespec current_physical_time;
        clock_gettime(CLOCK_REALTIME, &current_physical_time);
        // Convert to an instant.
        instant_t physical_time =
                current_physical_time.tv_sec * BILLION
                + current_physical_time.tv_nsec;
        if (physical_time > current_time) {
        	tag = physical_time;
        }

        interval_t min_inter_arrival = trigger->offset + extra_delay;
        // Compute the earliest time that this event can be scheduled.
        instant_t earliest_time;
        if (trigger->scheduled == NEVER) {
            earliest_time = start_time + min_inter_arrival;
        } else {
            earliest_time = trigger->scheduled + min_inter_arrival;
        }
        
        if (earliest_time > tag) {
            // The event is early. See which policy applies.
            if (trigger->policy == UPDATE) {
                // Update existing event if it exists.   
                e->time = tag;
                // See if there is an existing event up to but not including
                // the earliest time this event can be scheduled.
                existing = pqueue_find_equal(event_q, e, earliest_time-1);
                if (existing != NULL) {
                    // Update the value of the existing event.
                    existing->value = value;
                }
            }
            if (trigger->policy == DROP || existing != NULL) {
                // Recycle the new event.
                e->value = NULL;    // FIXME: Memory leak.
                pqueue_insert(recycle_q, e);
                return(0);
            }
            if (trigger->policy == DEFER 
                || (trigger->policy == UPDATE && existing == NULL)) {
                // Adjust the tag.
                tag = earliest_time;
            }
        }

        // Record the tag.
        trigger->scheduled = tag;        
        e->time = tag;                        
    }
    
    // Do not schedule events if a stop has been requested.
    if (tag != current_time && stop_requested) {
    	return 0;
    }

    // Handle duplicate events for logical actions.
    if (!trigger->is_physical) {
        existing = pqueue_find_equal_same_priority(event_q, e);
        if (existing != NULL) {
            existing->value = value;
            // Recycle the new event.
            e->value = NULL;    // FIXME: Memory leak.
            pqueue_insert(recycle_q, e);
            return(0);
        }
    }
    // NOTE: There is no need for an explicit microstep because
    // when this is called, all events at the current tag
    // (time and microstep) have been pulled from the queue,
    // and any new events added at this tag will go into the reaction_q
    // rather than the event_q, so anything put in the event_q with this
    // same time will automatically be executed at the next microstep.
    pqueue_insert(event_q, e);
    // FIXME: make a record of handle and implement unschedule.
    return __handle++;
}

// For the specified reaction, if it has produced outputs, put the
// resulting triggered reactions into the reaction queue.
void schedule_output_reactions(reaction_t* reaction) {
    // If the reaction produced outputs, put the resulting triggered
    // reactions into the queue.
    
    for(int i=0; i < reaction->num_outputs; i++) {
        if (*(reaction->output_produced[i])) {
            trigger_t** triggerArray = (reaction->triggers)[i];
            for (int j=0; j < reaction->triggered_sizes[i]; j++) {
            	trigger_t* trigger = triggerArray[j];
                if (trigger != NULL) {
                    for (int k=0; k < trigger->number_of_reactions; k++) {
                        reaction_t* reaction = trigger->reactions[k];
                        if (reaction != NULL) {
                            // Do not enqueue this reaction twice.
                            if (has_not_been_triggered(reaction)) {
                                if (is_blocked(reaction)) {
                                    printf("Before insert into blocked_q:\n");
                                    pqueue_dump(blocked_q, stdout, blocked_q->prt);
                                    // If there is something blocking this reaction, 
                                    // add it to the blocking queue.
                                    pqueue_insert(blocked_q, reaction);
                                    printf("After insert into blocked_q:\n");
                                    pqueue_dump(blocked_q, stdout, blocked_q->prt);
                                    printf("State of reaction_q:\n");
                                    pqueue_dump(reaction_q, stdout, reaction_q->prt);
                                } else {
                                    printf("Before insert into reaction_q:\n");
                                    pqueue_dump(reaction_q, stdout, reaction_q->prt);
                                    // Otherwise, add it to the ready queue.
                                    pqueue_insert(reaction_q, reaction);
                                    printf("After insert into reaction_q:\n");
                                    pqueue_dump(reaction_q, stdout, reaction_q->prt);
                                }
                            }
                        }
                    }
                }
            }
        }
	}
}

// Library function for allocating memory for an array output.
// This turns over "ownership" of the allocated memory to the output.
void* __set_new_array_impl(token_t* token, int length) {
    // FIXME: Error checking needed.
    token->value = malloc(token->element_size * length);
    token->ref_count = token->initial_ref_count;
    // Count allocations to issue a warning if this is never freed.
    __count_allocations++;
    // printf("****** Allocated object with starting ref_count = %d.\n", token->ref_count);
    token->length = length;
    return token->value;
}

// Library function for returning a writable copy of a token.
// If the reference count is 1, it returns the original rather than a copy.
void* __writable_copy_impl(token_t* token) {
    // printf("****** Requesting writable copy with reference count %d.\n", token->ref_count);
    if (token->ref_count == 1) {
        // printf("****** Avoided copy because reference count is exactly one.\n");
        // Decrement the reference count to avoid the automatic free().
        token->ref_count--;
        return token->value;
    } else {
        // printf("****** Copying array because reference count is not one.\n");
        int size = token->element_size * token->length;
        void* copy = malloc(size);
        memcpy(copy, token->value, size);
        // Count allocations to issue a warning if this is never freed.
        __count_allocations++;
        return copy;
    }
}

// Print a usage message.
void usage(char* command) {
    printf("\nCommand-line arguments: \n\n");
    printf("  -fast\n");
    printf("   Do not wait for physical time to match logical time.\n\n");
    printf("  -timeout <duration> <units>\n");
    printf("   Stop after the specified amount of logical time, where units are one of\n");
    printf("   nsec, usec, msec, sec, minute, hour, day, week, or the plurals of those.\n\n");
    printf("  -keepalive\n");
    printf("   Do not stop execution even if there are no events to process. Just wait.\n\n");
    printf("  -threads <n>\n");
    printf("   Executed in <n> threads if possible (optional feature).\n\n");
}

// If a run option is given in the target directive, then the code
// generator will overwrite these with default command-line options.
int default_argc = 0;
char** default_argv = NULL;

// Process the command-line arguments.
// If the command line arguments are not understood, then
// print a usage message and return 0.
// Otherwise, return 1.
int process_args(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-fast") == 0) {
            fast = true;
        } else if (strcmp(argv[i], "-timeout") == 0) {
            if (argc < i + 3) {
                usage(argv[0]);
                return 0;
            }
            i++;
            char* time_spec = argv[i++];
            char* units = argv[i];
            duration = atoll(time_spec);
            // A parse error returns 0LL, so check to see whether that is what is meant.
            if (duration == 0LL && strncmp(time_spec, "0", 1) != 0) {
                // Parse error.
                printf("Error: invalid time value: %s", time_spec);
                usage(argv[0]);
                return 0;
            }
            if (strncmp(units, "sec", 3) == 0) {
                duration = SEC(duration);
            } else if (strncmp(units, "msec", 4) == 0) {
                duration = MSEC(duration);
            } else if (strncmp(units, "usec", 4) == 0) {
                duration = USEC(duration);
            } else if (strncmp(units, "nsec", 4) == 0) {
                duration = NSEC(duration);
            } else if (strncmp(units, "minute", 6) == 0) {
                duration = MINUTE(duration);
            } else if (strncmp(units, "hour", 4) == 0) {
                duration = HOUR(duration);
            } else if (strncmp(units, "day", 3) == 0) {
                duration = DAY(duration);
            } else if (strncmp(units, "week", 4) == 0) {
                duration = WEEK(duration);
            } else {
                // Invalid units.
                printf("Error: invalid time units: %s", units);
                usage(argv[0]);
                return 0;
            }
        } else if (strcmp(argv[i], "-keepalive") == 0) {
            keepalive_specified = true;
        } else if (strcmp(argv[i], "-threads") == 0) {
            if (argc < i + 2) {
                usage(argv[0]);
                return 0;
            }
            i++;
            char* threads_spec = argv[i++];
            number_of_threads = atoi(threads_spec);           
        } else {
            usage(argv[0]);
            return 0;
        }
    }
    return 1;
}

// Initialize the priority queues and set logical time to match
// physical time. This also prints a message reporting the start time.
void initialize() {
    __count_allocations = 0;
#if _WIN32 || WIN32
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (ntdll) {
        NtDelayExecution = (NtDelayExecution_t *)GetProcAddress(ntdll, "NtDelayExecution");
        NtQueryPerformanceCounter = (NtQueryPerformanceCounter_t *)GetProcAddress(ntdll, "NtQueryPerformanceCounter");
        NtQuerySystemTime = (NtQuerySystemTime_t *)GetProcAddress(ntdll, "NtQuerySystemTime");
    }
#endif

    // Initialize our priority queues.

    // Reaction queue ordered by deadline.
    reaction_q = pqueue_init(INITIAL_REACT_QUEUE_SIZE, cmp_pri, get_reaction_deadline,
            get_reaction_position, set_reaction_position, eql_rct, print_reaction);    

    // Blocked reactions ordered by index.
    blocked_q = pqueue_init(INITIAL_REACT_QUEUE_SIZE, cmp_pri, get_reaction_index,
            get_reaction_position, set_reaction_position, eql_rct, print_reaction);

    transfer_q = pqueue_init(INITIAL_REACT_QUEUE_SIZE, cmp_pri, get_reaction_index,
            get_reaction_position, set_reaction_position, eql_rct, print_reaction);


    event_q = pqueue_init(INITIAL_EVENT_QUEUE_SIZE, cmp_pri, get_event_time,
            get_event_position, set_event_position, eql_evt, print_event);
	// NOTE: The recycle queue does not need to be sorted. But here it is.
    recycle_q = pqueue_init(INITIAL_EVENT_QUEUE_SIZE, cmp_pri, get_event_time,
            get_event_position, set_event_position, eql_evt, print_event);
	// NOTE: The free queue does not need to be sorted. But here it is.
    free_q = pqueue_init(INITIAL_EVENT_QUEUE_SIZE, cmp_pri, get_event_time,
            get_event_position, set_event_position, eql_evt, print_event);

    // Initialize the trigger table.
    __initialize_trigger_objects();

    // Initialize logical time to match physical time.
    clock_gettime(CLOCK_REALTIME, &physicalStartTime);
    printf("---- Start execution at time %s---- plus %ld nanoseconds.\n",
    		ctime(&physicalStartTime.tv_sec), physicalStartTime.tv_nsec);
    current_time = physicalStartTime.tv_sec * BILLION + physicalStartTime.tv_nsec;
    start_time = current_time;
    
    if (duration >= 0LL) {
        // A duration has been specified. Calculate the stop time.
        stop_time = current_time + duration;
    }
}



// Check that memory allocated by set_new, set_new_array, or writable_copy
// has been freed and print a warning message if not.
void termination() {
    if (__count_allocations != 0) {
        printf("**** WARNING: Memory allocated by set_new, set_new_array, or writable_copy has not been freed!\n");
        printf("**** Number of unfreed tokens: %d.\n", __count_allocations);
    }
    // Print elapsed times.
    interval_t elapsed_logical_time
        = current_time - (physicalStartTime.tv_sec * BILLION + physicalStartTime.tv_nsec);
    printf("---- Elapsed logical time (in nsec): %lld\n", elapsed_logical_time);
    
    struct timespec physicalEndTime;
    clock_gettime(CLOCK_REALTIME, &physicalEndTime);
    interval_t elapsed_physical_time
        = (physicalEndTime.tv_sec * BILLION + physicalEndTime.tv_nsec)
        - (physicalStartTime.tv_sec * BILLION + physicalStartTime.tv_nsec);
    printf("---- Elapsed physical time (in nsec): %lld\n", elapsed_physical_time);
}

// ********** Start Windows Support
// Windows is not POSIX, so we include here compatibility definitions.
#if _WIN32 || WIN32
NtDelayExecution_t *NtDelayExecution = NULL;
NtQueryPerformanceCounter_t *NtQueryPerformanceCounter = NULL;
NtQuerySystemTime_t *NtQuerySystemTime = NULL;
int clock_gettime(clockid_t clk_id, struct timespec *tp) {
    int result = -1;
    int days_from_1601_to_1970 = 134774 /* there were no leap seconds during this time, so life is easy */;
    long long timestamp, counts, counts_per_sec;
    switch (clk_id) {
    case CLOCK_REALTIME:
        NtQuerySystemTime((PLARGE_INTEGER)&timestamp);
        timestamp -= days_from_1601_to_1970 * 24LL * 60 * 60 * 1000 * 1000 * 10;
        tp->tv_sec = (time_t)(timestamp / (BILLION / 100));
        tp->tv_nsec = (long)((timestamp % (BILLION / 100)) * 100);
        result = 0;
        break;
    case CLOCK_MONOTONIC:
        if ((*NtQueryPerformanceCounter)((PLARGE_INTEGER)&counts, (PLARGE_INTEGER)&counts_per_sec) == 0) {
            tp->tv_sec = counts / counts_per_sec;
            tp->tv_nsec = (long)((counts % counts_per_sec) * BILLION / counts_per_sec);
            result = 0;
        } else {
            errno = EINVAL;
            result = -1;
        }
        break;
    default:
        errno = EINVAL;
        result = -1;
        break;
    }
    return result;
}
int nanosleep(const struct timespec *req, struct timespec *rem) {
    unsigned char alertable = rem ? 1 : 0;
    long long duration = -(req->tv_sec * (BILLION / 100) + req->tv_nsec / 100);
    NTSTATUS status = (*NtDelayExecution)(alertable, (PLARGE_INTEGER)&duration);
    int result = status == 0 ? 0 : -1;
    if (alertable) {
        if (status < 0) {
            errno = EINVAL;
        } else if (status > 0 && clock_gettime(CLOCK_MONOTONIC, rem) == 0) {
            errno = EINTR;
        }
    }
    return result;
}
#endif
// ********** End Windows Support

int clock_gettime(clockid_t clk_id, struct timespec *tp) {
    uint32_t high;
    uint32_t low;
    
    asm(
    "again:\n"
        "rdtimeh t0\n"
        "rdtime %1\n"
        "rdtimeh %0\n"
        "bne t0, %0, again"
    : "=r"(low), "=r"(high)// outputs
    : // inputs
    : "t0" // clobbers
    );


    tp->tv_sec = 0;
    tp->tv_nsec = 0;
}

//int nanosleep(const struct timespec *req, struct timespec *rem) {
//    return 0;
//}

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include <pthread.h>
// #include <stdint.h>

struct id_time {
    u_int64_t id;
    u_int32_t sec;
    u_int32_t nsec;
};

/* Lock buffer file pointer */
FILE * lock_buffer_log_fp;
char * lock_buffer_filename;

/* Lock buffer code */
struct lock_buffer {
    pthread_mutex_t  * locks; // array of locks
    struct id_time * buffer;
    unsigned int pos_lock_push;
    unsigned int pos_lock_pull;
    unsigned int lock_step;
    unsigned int lock_num;
    unsigned int pos_push;
    unsigned int pos_pull;
    unsigned int item_num;
    int finish_signal;
};

struct lock_buffer_section {
    struct id_time * start;
    struct id_time * end;
};

# define NUMBER_OF_LOCKS 2
void lock_buffer_init (struct lock_buffer * lb, size_t elem_size, int item_num) {
    int i;
    
    // printf("  Init lock buffer\n");
    
    // init the locks in the array
    lb->locks = malloc( sizeof(pthread_mutex_t) * NUMBER_OF_LOCKS );// malloc the lock array
    for (i=0; i<NUMBER_OF_LOCKS; i++) {
        pthread_mutex_init( &lb->locks[i], NULL );
        pthread_mutex_lock( &lb->locks[i] );
    }
    
    // malloc the data array
    lb->buffer = malloc( elem_size * item_num );
    
    // set the initial values of all the members
    lb->pos_lock_push = 0;
    lb->pos_lock_pull = 0;
    lb->lock_step = item_num / NUMBER_OF_LOCKS;
    lb->lock_num = NUMBER_OF_LOCKS;
    lb->pos_push = 0;
    lb->pos_pull = 0;
    lb->item_num = item_num;
    lb->finish_signal = 0;
    // printf("    lock_step: %d\n", lb->lock_step);
    // printf("    lock_num:  %d\n", lb->lock_num);
    // printf("    item_num:  %d\n", lb->item_num);
}

/* this really cannot be a function call, the assignment of the item needs to be done __in place__.. */
static inline void lock_buffer_push (struct lock_buffer * lb, struct id_time* item) {
    
    // printf("  Pushing item to buffer\n");
    
    // lb->buffer[ lb->pos_push ].id = item->id;
    // lb->buffer[ lb->pos_push ].ts = item->ts;
    
    lb->pos_push++;
    
    // check if lock needs to be unlocked
    if ( (lb->pos_push) >= ( (1+lb->pos_lock_push) * lb->lock_step)) {
        // printf("  pos: %d   pos_lock: %d step: %d\n", lb->pos_push, lb->pos_lock_push, lb->lock_step);
        // printf("  ** (push) unlock lock: 0x%X\n", &lb->locks[ lb->pos_lock_push ]);
        pthread_mutex_unlock( &lb->locks[ lb->pos_lock_push ] );  // hopefully shouldn't stall things.. should never be unlocked anyways
        
        lb->pos_lock_push++;
    }
    // Ensure positions are within bounds, wrap if not
    if (lb->pos_lock_push >= lb->lock_num) 
        lb->pos_lock_push = 0;
    if (lb->pos_push >= lb->item_num) 
        lb->pos_push = 0;
}

void lock_buffer_pull (struct lock_buffer * lb, struct lock_buffer_section* lbs) {
    // wait for next unlock
    // printf("  Attempt to pull items from buffer - lock: 0x%X\n", &lb->locks[ lb->pos_lock_pull ] );
    pthread_mutex_lock( &lb->locks[ lb->pos_lock_pull++ ] );
    // printf("  ** (pull) UNLOCKED! - lock: 0x%X\n", &lb->locks[ lb->pos_lock_pull-1 ] );
    if (lb->finish_signal) {
        // printf("  **     BUT FINISHED - lock: 0x%X\n", &lb->locks[ lb->pos_lock_pull-1 ] );
        return;
    }
    
    // Get the start and end points to return
    lbs->start = lb->buffer + lb->pos_pull; // The position of the unlocked lock
    lb->pos_pull += lb->lock_step;
    lbs->end   = lb->buffer + lb->pos_pull; // The position before the next lock, or the last position in the buffer
    
    // Ensure position is within bounds, wrap if not
    if (lb->pos_lock_pull >= lb->lock_num) 
        lb->pos_lock_pull = 0;
    if (lb->pos_pull >= lb->item_num) 
        lb->pos_pull = 0;
}

void lock_buffer_pull_final (struct lock_buffer * lb, struct lock_buffer_section* lbs) {
    // wait for next unlock
    // printf("Pulling final items from buffer\n");
    
    // Get the last written pull and final push positions
    lbs->start = &lb->buffer[lb->pos_pull]; 
    lbs->end   = &lb->buffer[lb->pos_push != 0 ? lb->pos_push : lb->item_num];
}

void lock_buffer_finish (struct lock_buffer *lb) {
    lb->finish_signal = 1;
    
    // unlock the last step
    pthread_mutex_unlock( &lb->locks[ lb->pos_lock_push ] );
}

/* Main code */

/* Creates the data structure for pushing and pulling packet data to/from
    - pkt_rate: mean rate of packets being sent (pkt / second)
    - pkt_size: mean size of packets being sent (bytes / packet)
    - seconds:  the amount of time to buffer between writes
 */
struct lock_buffer * lock_buffer_create (int pkt_rate, int data_size, int seconds) {
    int items_to_buffer = pkt_rate * seconds;
    // printf("## Creating buffer of size %d\n", items_to_buffer);
    
    struct lock_buffer * lb = malloc( sizeof(struct lock_buffer) );
    lock_buffer_init(lb, data_size, items_to_buffer);
    
    // setbuffer() ?
    
    return lb;
}

void * lock_buffer_write_loop( void * x ) { // struct lock_buffer * lb) { 
    struct lock_buffer * lb = (struct lock_buffer *) x;
    struct lock_buffer_section * lbs = malloc( sizeof(struct lock_buffer_section) );
    
    printf("write loop starting, lock step: %d\n", lb->lock_step);
    
    // printf("location of lb - write loop: 0x%X\n", lb);
    // printf("finish signal - write loop: %d\n", lb->finish_signal);
    while (!lb->finish_signal) {
        // printf("## write loop: %d \n", i++);
        lock_buffer_pull(lb, lbs);
        if (lb->finish_signal) break;
        fwrite(lbs->start, sizeof(struct id_time), lbs->end - lbs->start, lock_buffer_log_fp);
        // printf("  pos_push: %d pos_pull: %d\n", lb->pos_push, lb->pos_pull);
        // printf("  Wrote 0x%X (%d)\n      - 0x%X (%d)\n", lbs->start, lbs->start - lb->buffer, lbs->end,  lbs->end - lb->buffer);
    }
    // puts("finish signal heard in write loop, writing remaining data");
    puts("finish signal heard in write loop");
    
    // write the remaining items
    // lock_buffer_pull_final(lb, lbs);
    // printf("  Final write: 0x%X (%d)",lbs->start, (lbs->end - lbs->start));
    // fwrite(lbs->start, sizeof(struct id_time), lbs->end - lbs->start, lock_buffer_log_fp);
    
    return NULL;
}

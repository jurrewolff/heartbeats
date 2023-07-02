/**
 * Shared memory implementation of heartbeat.h
 *
 * @see heartbeat-util-shared.c
 * @author Hank Hoffmann
 * @author Connor Imes
 */
#include "heartbeat.h"
#include "heartbeat-util-shared.h"
#include "sim_api.h"
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>

heartbeat_t* heartbeat_init(int64_t window_size,
                            int64_t buffer_depth,
                            const char* log_name,
                            double min_target,
                            double max_target) {
  int pid = getpid();
  char* enabled_dir;

  heartbeat_t* hb = (heartbeat_t*) malloc(sizeof(heartbeat_t));
  if (hb == NULL) {
    perror("Failed to malloc heartbeat");
    return NULL;
  }
  // set to NULL so free doesn't fail in finish function if we have to abort
  hb->window = NULL;
  hb->text_file = NULL;

  hb->state = HB_alloc_state(pid);
  if (hb->state == NULL) {
    heartbeat_finish(hb);
    return NULL;
  }
  hb->state->pid = pid;

  if(log_name != NULL) {
    hb->text_file = fopen(log_name, "w");
    if (hb->text_file == NULL) {
      perror("Failed to open heartbeat log file");
      heartbeat_finish(hb);
      return NULL;
    } else {
      fprintf(hb->text_file, "Beat\tTag\tTimestamp\tGlobal Rate\tWindow Rate\tInstant Rate\tMin Rate\tMax Rate\n");
    }
  } else {
    hb->text_file = NULL;
  }

  enabled_dir = getenv("HEARTBEAT_ENABLED_DIR");
  if(!enabled_dir) {
    heartbeat_finish(hb);
    return NULL;
  }
  snprintf(hb->filename, sizeof(hb->filename), "%s/%d", enabled_dir, hb->state->pid);
  printf("%s\n", hb->filename);

  hb->log = HB_alloc_log(hb->state->pid, buffer_depth);
  if(hb->log == NULL) {
    heartbeat_finish(hb);
    return NULL;
  }

  hb->first_timestamp = hb->last_timestamp = -1;
  hb->state->window_size = window_size;
  hb->window = (int64_t*) malloc((size_t)window_size * sizeof(int64_t));
  if (hb->window == NULL) {
    perror("Failed to malloc window size");
    heartbeat_finish(hb);
    return NULL;
  }
  hb->current_index = 0;
  hb->state->min_heartrate = min_target;
  hb->state->max_heartrate = max_target;
  hb->state->counter = 0;
  hb->state->buffer_index = 0;
  hb->state->read_index = 0;
  hb->state->buffer_depth = buffer_depth;
  pthread_mutex_init(&hb->mutex, NULL);
  hb->steady_state = 0;
  hb->state->valid = 0;

  hb->binary_file = fopen(hb->filename, "w");
  if ( hb->binary_file == NULL ) {
    perror("Failed to open heartbeat log");
    heartbeat_finish(hb);
    return NULL;
  }
  fclose(hb->binary_file);

  return hb;
}

/**
 *
 * @param hb pointer to heartbeat_t
 */
static void hb_flush_buffer(heartbeat_t volatile * hb) {
  int64_t i;
  int64_t nrecords = hb->state->buffer_index; // buffer_depth

  //printf("Flushing buffer - %lld records\n",
  //	 (long long int) nrecords);

  if(hb->text_file != NULL) {
    for(i = 0; i < nrecords; i++) {
      fprintf(hb->text_file,
	      "%lld\t%d\t%lld\t%f\t%f\t%f\t%f\t%f\n",
	      (long long int) hb->log[i].beat,
	      hb->log[i].tag,
	      (long long int) hb->log[i].timestamp,
	      hb->log[i].global_rate,
	      hb->log[i].window_rate,
	      hb->log[i].instant_rate,
        hb->state->min_heartrate,
        hb->state->max_heartrate);
    }

    fflush(hb->text_file);
  }
}

void heartbeat_finish(heartbeat_t* hb) {
  if (hb != NULL) {
    pthread_mutex_destroy(&hb->mutex);
    free(hb->window);
    if(hb->text_file != NULL) {
      hb_flush_buffer(hb);
      fclose(hb->text_file);
    }
    remove(hb->filename);
    /*TODO : need to deallocate log */
    free(hb);
  }
}

/**
 * Helper function to compute windowed heart rate
 * @param hb pointer to heartbeat_t
 * @param time int64_t
 */
static inline float hb_window_average(heartbeat_t volatile * hb,
				      int64_t time) {
  int i;
  double average_time = 0;
  double fps;


  if(!hb->steady_state) {
    hb->window[hb->current_index] = time;

    for(i = 0; i < hb->current_index+1; i++) {
      average_time += (double) hb->window[i];
    }
    average_time = average_time / ((double) hb->current_index+1);
    hb->last_average_time = average_time;
    hb->current_index++;
    if( hb->current_index == hb->state->window_size) {
      hb->current_index = 0;
      hb->steady_state = 1;
    }
  }
  else {
    average_time =
      hb->last_average_time -
      ((double) hb->window[hb->current_index]/ (double) hb->state->window_size);
    average_time += (double) time /  (double) hb->state->window_size;

    hb->last_average_time = average_time;

    hb->window[hb->current_index] = time;
    hb->current_index++;

    if( hb->current_index == hb->state->window_size)
      hb->current_index = 0;
  }
  fps = (1.0 / (float) average_time)*1000000000;

  return (float)fps;
}

int64_t heartbeat( heartbeat_t* hb, int tag )
{
    struct timespec time_info;
    int64_t time;
    int64_t old_last_time;

    pthread_mutex_lock(&hb->mutex);
    //printf("Registering Heartbeat\n");
    old_last_time = hb->last_timestamp;
    time = SimUser(0x123, 1) / 1000000; // get fs time and convert to ns

    // TODO - parse with gmtime(), to see if value is valid timestamp?
    //      - Maybe simpler parse method to minimize perf impact of beats.
    //      - Make sure types don't clash

    hb->last_timestamp = time;

    if(hb->first_timestamp == -1) {
      //printf("In heartbeat - first time stamp\n");
      hb->first_timestamp = time;
      hb->last_timestamp  = time;
      hb->window[0] = 0;

      //printf("             - accessing state and log\n");
      hb->log[0].beat = hb->state->counter;
      hb->log[0].tag = tag;
      hb->log[0].timestamp = time;
      hb->log[0].window_rate = 0;
      hb->log[0].instant_rate = 0;
      hb->log[0].global_rate = 0;
      hb->state->counter++;
      hb->state->buffer_index++;
      hb->state->valid = 1;
    }
    else {
      //printf("In heartbeat - NOT first time stamp - read index = %d\n",hb->state->read_index );
      int64_t index =  hb->state->buffer_index;
      hb->last_timestamp = time;
      double window_heartrate = hb_window_average(hb, time-old_last_time);
      double global_heartrate =
	(((double) hb->state->counter+1) /
	 ((double) (time - hb->first_timestamp)))*1000000000.0;
      double instant_heartrate = 1.0 /(((double) (time - old_last_time))) *
	1000000000.0;

      hb->log[index].beat = hb->state->counter;
      hb->log[index].tag = tag;
      hb->log[index].timestamp = time;
      hb->log[index].window_rate = window_heartrate;
      hb->log[index].instant_rate = instant_heartrate;
      hb->log[index].global_rate = global_heartrate;
      hb->state->buffer_index++;
      hb->state->counter++;
      hb->state->read_index++;

      if(hb->state->buffer_index%hb->state->buffer_depth == 0) {
	if(hb->text_file != NULL)
	  hb_flush_buffer(hb);
	hb->state->buffer_index = 0;
      }
      if(hb->state->read_index%hb->state->buffer_depth == 0) {
	hb->state->read_index = 0;
      }
    }
    pthread_mutex_unlock(&hb->mutex);
    return time;

}

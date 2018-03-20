#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "traffic.h"

extern struct intersection isection;

/**
 * Populate the car lists by parsing a file where each line has
 * the following structure:
 *
 * <id> <in_direction> <out_direction>
 *
 * Each car is added to the list that corresponds with 
 * its in_direction
 * 
 * Note: this also updates 'inc' on each of the lanes
 */
void parse_schedule(char *file_name) {
    int id;
    struct car *cur_car;
    struct lane *cur_lane;
    enum direction in_dir, out_dir;
    FILE *f = fopen(file_name, "r");

    /* parse file */
    while (fscanf(f, "%d %d %d", &id, (int*)&in_dir, (int*)&out_dir) == 3) {

        /* construct car */
        cur_car = malloc(sizeof(struct car));
        cur_car->id = id;
        cur_car->in_dir = in_dir;
        cur_car->out_dir = out_dir;

        /* append new car to head of corresponding list */
        cur_lane = &isection.lanes[in_dir];
        cur_car->next = cur_lane->in_cars;
        cur_lane->in_cars = cur_car;
        cur_lane->inc++;
    }

    fclose(f);
}

/**
 * Do all of the work required to prepare the intersection
 * before any cars start coming
 */
void init_intersection() {
	int i, j;
	for (i = 0; i < 4; i++) {
		pthread_mutex_init (&isection.quad[i], NULL);
		pthread_mutex_init (&isection.lanes[i].lock, NULL);
		pthread_cond_init (&isection.lanes[i].producer_cv, NULL);
		pthread_cond_init (&isection.lanes[i].consumer_cv, NULL);
		isection.lanes[i].in_cars = NULL;
		isection.lanes[i].out_cars = NULL;
		isection.lanes[i].inc = 0;
		isection.lanes[i].passed = 0;
		isection.lanes[i].capacity = LANE_LENGTH;
		
		// allocate memory for lane buffer
		isection.lanes[i].buffer = (struct car **) malloc(sizeof(struct car *) * isection.lanes[i].capacity);
		// initialize buffer values
		for (j = 0; j < isection.lanes[i].capacity; j++) {
			isection.lanes[i].buffer[j] = NULL;
		}
		
		isection.lanes[i].head = 0;
		isection.lanes[i].tail = 0;
		isection.lanes[i].in_buf = 0;
	}
}

/**
 * Populate the corresponding lane with cars as room becomes
 * available. Ensure to notify the cross thread as new cars are
 * added to the lane.
 */
void *car_arrive(void *arg) {
    struct lane *l = arg;
	
	int i;
	for (i = 0; i < l->inc; i++) {
		pthread_mutex_lock(&l->lock);
		// wait until buffer has space to add next car
		while (l->in_buf == l->capacity) {
			pthread_cond_wait(&l->producer_cv, &l->lock);
		}
		struct car *cur_car;
		// take first car from list
		cur_car = l->in_cars;
		// update list head
        l->in_cars = cur_car->next;
		// add car to buffer
		l->buffer[l->tail] = cur_car;
		// update buffer tail for next buffer addition
		l->tail = (l->tail + 1) % l->capacity;
		l->in_buf++;
		// notify cross thread that car(s) available in lane
		pthread_cond_signal(&l->consumer_cv);
		pthread_mutex_unlock(&l->lock);
	}
	
    return NULL;
}

/**
 * Move cars from a single lane across the intersection. Cars
 * crossing the intersection must abide the rules of the road
 * and cross along the correct path. Ensure to notify the
 * arrival thread as room becomes available in the lane.
 */
void *car_cross(void *arg) {
    struct lane *l = arg;
	
	int i;
	for (i = 0; i < l->inc; i++) {
		pthread_mutex_lock(&l->lock);
		// wait until buffer has next car to read from it
		while (l->in_buf == 0) {
			pthread_cond_wait(&l->consumer_cv, &l->lock);
		}
		struct car *cur_car;
		// take first car from list
		cur_car = l->buffer[l->head];
		// update buffer head for next buffer read
		l->head = (l->head + 1) % l->capacity;
		l->in_buf--;
		
		// get path needed for car to cross intersection
		int *path = compute_path(cur_car->in_dir, cur_car->out_dir);
		
		int q;
		for (q = 0; q < 4; q++) {
			if (path[q] < 4) {
				// acquire locks necessary to go through intersection
				pthread_mutex_lock(&isection.quad[path[q]]);
			}
		}
		
		/* once necessary quadrants acquired, cur_car can pass through
		   intersection and its info is printed */
		printf("%d %d %d\n", cur_car->in_dir, cur_car->out_dir, cur_car->id);
		
		for (q = 0; q < 4; q++) {
			if (path[q] < 4) {
				// release locks after intersection passed
				pthread_mutex_unlock(&isection.quad[path[q]]);
			}
		}
		
		free(path);
		
		/* notify arrive thread that space available in lane that the car
		   arrived from */
		pthread_cond_signal(&l->producer_cv);
		pthread_mutex_unlock(&l->lock);
		
		pthread_mutex_lock(&(isection.lanes[cur_car->out_dir].lock));
		
		// add current car to out_cars of the lane it goes out
		cur_car->next = isection.lanes[cur_car->out_dir].out_cars;
		isection.lanes[cur_car->out_dir].out_cars = cur_car;
		isection.lanes[cur_car->out_dir].passed++;
		
		pthread_mutex_unlock(&(isection.lanes[cur_car->out_dir].lock));
	}
	free(l->buffer);
	
    return NULL;
}

/**
 * Given a car's in_dir and out_dir return a sorted 
 * list of the quadrants the car will pass through.
 */
int *compute_path(enum direction in_dir, enum direction out_dir) {
	
	int *path = (int *) malloc(sizeof(int) * 4); //max 4 quadrant path length
	
	//default invalid quadrant indeces
	path[0] = 4;
	path[1] = 4;
	path[2] = 4;
	path[3] = 4;
	
	if (in_dir == NORTH) {
		path[0] = 1;
		if (out_dir == NORTH) {
			path[0] = 0;
			path[1] = 1;
			path[2] = 2;
			path[3] = 3;
		} else if (out_dir == EAST) {
			path[1] = 2;
			path[2] = 3;
		} else if (out_dir == SOUTH) {
			path[1] = 2;
		}
	}
	
	else if (in_dir == EAST) {
		path[0] = 0;
		if (out_dir == EAST) {
			path[0] = 0;
			path[1] = 1;
			path[2] = 2;
			path[3] = 3;
		} else if (out_dir == SOUTH) {
			path[1] = 1;
			path[2] = 2;
		} else if (out_dir == WEST) {
			path[1] = 1;
		}
	}
	
	else if (in_dir == SOUTH) {
		path[0] = 3;
		if (out_dir == SOUTH) {
			path[0] = 0;
			path[1] = 1;
			path[2] = 2;
			path[3] = 3;
		} else if (out_dir == WEST) {
			path[0] = 0;
			path[1] = 1;
			path[2] = 3;
		} else if (out_dir == NORTH) {
			path[0] = 0;
			path[1] = 3;
		}
	}
	
	else if (in_dir == WEST) {
		path[0] = 2;
		if (out_dir == WEST) {
			path[0] = 0;
			path[1] = 1;
			path[2] = 2;
			path[3] = 3;
		} else if (out_dir == NORTH) {
			path[0] = 0;
			path[1] = 2;
			path[2] = 3;
		} else if (out_dir == EAST) {
			path[1] = 3;
		}
	}
	
	return path;
	return NULL;
}
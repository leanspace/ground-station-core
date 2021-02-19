#pragma once

#include <predict/predict.h>

#include "entry.h"

#define MAX_TLE_LEN 			256
#define MAX_TLE_SAT_NAME_LEN 	25 /** 24 chars + null */
#define MAX_GS_NAME_LEN 		16

typedef enum modulation_t {
	MODULATION_FM,
	MODULATION_AFSK
} modulation_t;

typedef struct observation_t observation_t;

typedef struct satellite_t {
	char name[MAX_TLE_SAT_NAME_LEN]; 
	char tle1[MAX_TLE_LEN];
	char tle2[MAX_TLE_LEN];
  	modulation_t modulation;
	double min_elevation;
  	int frequency;
  	int bandwidth;
  	int priority;
	time_t next_aos;
	time_t next_los;
	double aos_az;
	double los_az;
	bool zero_transition;
	bool parked;
	observation_t *obs;
	predict_orbital_elements_t *orbital_elements;
  	LIST_ENTRY(satellite_t) entries;
} satellite_t;

typedef struct netcli_t {
	int port_az;
	int port_el;
	int az_conn_fd;
	int el_conn_fd;
	char addr[32];
} netcli_t;

typedef struct observation_t {
	float latitude;
	float longitude;
	satellite_t *active;
	char gs_name[MAX_GS_NAME_LEN];
	time_t sim_time; /** used for simulation */
	netcli_t cli;
	bool sch_terminate;
	pthread_t sch_thread;
	predict_observer_t *observer;
	LIST_HEAD(satellites_list_head, satellite_t) satellites_list;
} observation_t;

typedef enum sat_process_plane_t {
	SAT_PROCESS_AZIMUTH,
	SAT_PROCESS_ELEVATION
} sat_process_plane_t;

// typedef struct thread_param_t {
//     observation_t *obs;
	
// } thread_param_t;

int sat_setup(satellite_t *sat);
int sat_setup_observation(void);
observation_t *sat_get_observation(void);


void sat_simul_time_step(time_t timestep);
void sat_simul_time_set(time_t val);
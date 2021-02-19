#include <math.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "log.h"
#include "sat.h"
#include "rotctl.h"
#include "helpers.h"

static observation_t *_observation;

void sat_simul_time_step(time_t timestep)
{
	if (_observation) {
		_observation->sim_time += timestep;
		LOG_V("Time increment: %ld", _observation->sim_time);
	}
}

void sat_simul_time_set(time_t val)
{
	if (_observation)
		_observation->sim_time = val;
}


static void newline_guillotine(char *str)
{
	char *pos;

	if ((pos = strchr(str, '\r')) != NULL)
		*pos = '\0';

	if ((pos = strchr(str, '\n')) != NULL)
		*pos = '\0';
}

static bool sat_is_crossing_zero(double aos_az, double los_az)
{
	if (los_az > 180.0) {
		if (los_az - (aos_az + 180.0) > 0)
			return true;
		else
			return false;
	} else {
		if (los_az - (aos_az - 180.0) < 0)
			return true;
		else
			return false;
	}
}

static double sat_reverse_azimuth(double az)
{
	return (az  > 180.0) ? az - 180.0 : az + 180.0;
}

static void *sat_scheduler(void *opt)
{
	time_t current_time;
	int time_delay = 1000000;

	satellite_t *sat;
	observation_t *obs = (observation_t *) opt;
	predict_observer_t *observer;
	predict_orbital_elements_t *orbital_elements;

	if (obs == NULL)
		return NULL;

	LOG_I("Scheduler started");
	fflush(stdout); /** just to avoid mixing up the output: happens sometimes */
	while (obs->sch_terminate == false) {

		time(&current_time);

		if (obs->sim_time)
			current_time += obs->sim_time;

		if (obs->active == NULL) {

			time_delay = 1000000;

			LIST_FOREACH(sat, &obs->satellites_list, entries) {

				if (sat->next_aos > 0) {
					if ((current_time > (sat->next_aos - 120)) && sat->parked == false) {
						LOG_I("Parking antenna for the receiving of %s", sat->name);
						if (!sat->zero_transition)
							rotctl_send_and_wait(obs, sat->aos_az, 0);
						else
							rotctl_send_and_wait(obs, sat->aos_az, 180);
						LOG_I("Parking done");
						sat->parked = true;
					}
					if (current_time > sat->next_aos) {

						obs->active = sat;
						orbital_elements = predict_parse_tle(sat->tle1, sat->tle2);
						observer = predict_create_observer("ISU GS", sat_get_observation()->latitude * M_PI / 180.0, sat_get_observation()->longitude * M_PI / 180.0, 10);

						if (orbital_elements == NULL || observer == NULL) {
							LOG_E("Error creating observer");
						}

						LOG_I("Tracking started: %s", sat->name);
						break;
					}
				}
			}
		} else {
			struct predict_position orbit;
			struct predict_observation observation;

			time_delay = 100000;
			predict_orbit(orbital_elements, &orbit, predict_to_julian(current_time));
			predict_observe_orbit(observer, &orbit, &observation);
			double az = rad_to_deg(observation.azimuth);
			double el = rad_to_deg(observation.elevation);
			double shift = predict_doppler_shift(&observation, obs->active->frequency);

			if (sat->zero_transition) {
				az = sat_reverse_azimuth(az);
				el = 180 - el;
			}
			LOG_I("Az: %f; El: %f, Doppler shift: %f", az, el, shift);

			rotctl_send_and_wait(obs, az, el);

			if (current_time > obs->active->next_los) {
				predict_destroy_observer(observer);
				predict_destroy_orbital_elements(orbital_elements);

				if (sat_setup(obs->active) == -1) {
					LOG_E("Error while rescheduling %s", obs->active->name);
				}
				obs->active->parked = false;
				LOG_I("Rescheduled %s", obs->active->name);
				obs->active = NULL;
			}
		}

		usleep(time_delay);
	}

	LOG_I("Scheduler terminated");
	return NULL;
}

static int sat_fetch_tle(const char *name, char *tle1, char *tle2)
{
	int ret;
	int size;
	FILE *fd;
	char *buf;
	bool found;

	ret = 0;
	buf = NULL;
	found = false;

	if (tle1 == NULL || tle2 == NULL) {
		ret = -1;
		goto out;
	}

	fd = fopen("active.txt", "r");
	if (fd == NULL) {
		LOG_E("TLE file not found");
		ret = -1;
		goto out;
	}

	fseek(fd, 0, SEEK_END);
	size = ftell(fd);
	fseek(fd, 0, SEEK_SET);

	if ((buf = malloc(size + 1)) == NULL) {
		LOG_E("error of malloc()");
		ret = -1;
		goto out;
	}

	buf[0] = 0;

	while(!feof(fd)) {
		fgets(buf, 32, fd);
		if (!strncmp(buf, name, strlen(name))) {
			fgets(tle1, MAX_TLE_LEN, fd);
			fgets(tle2, MAX_TLE_LEN, fd);
			newline_guillotine(tle1);
			newline_guillotine(tle2);
			found = true;
			break;
		}
	}

	if (!found) {
		LOG_I("Satellite %s not found", name);
		ret = -1;
		goto out;
	}

out:
	if (buf)
		free(buf);

	if (fd)
		fclose(fd);

	return ret;
}

int sat_predict(satellite_t *sat)
{
	int ret;
	float max_elev;
	time_t current_time;
	struct predict_observation aos;
	struct predict_observation los;
	/* struct predict_position orbit; */
	/* struct predict_observation observation; */
	struct predict_observation elev;

	ret = 0;

	if (!sat)
		return -1;

	time(&current_time);

	if (sat_get_observation()->sim_time)
		current_time += sat_get_observation()->sim_time;

	predict_orbital_elements_t *orbital_elements = predict_parse_tle(sat->tle1, sat->tle2);
	predict_observer_t *observer = predict_create_observer("ISU GS", sat_get_observation()->latitude * M_PI / 180.0, sat_get_observation()->longitude * M_PI / 180.0, 10);

	predict_julian_date_t start_time = predict_to_julian(current_time);

reschedule:

	do {
		elev = predict_at_max_elevation(observer, orbital_elements, start_time);
		aos = predict_next_aos(observer, orbital_elements, start_time);
		los = predict_next_los(observer, orbital_elements, start_time);
		start_time = los.time;
		max_elev = rad_to_deg(elev.elevation);
	} while (max_elev < sat->min_elevation);

	if (max_elev > 0) {
		struct tm tm_aos;
		struct tm tm_los;
		char aos_buf[32];
		char los_buf[32];
		struct predict_position orbit;
		struct predict_observation observation;

		predict_orbit(orbital_elements, &orbit, aos.time);
		predict_observe_orbit(observer, &orbit, &observation);

		time_t aos_time_t = predict_from_julian(aos.time);
		time_t los_time_t = predict_from_julian(los.time);

		tm_aos = *localtime(&aos_time_t);
		tm_los = *localtime(&los_time_t);

		strftime(aos_buf, sizeof(aos_buf), "%d %B %Y - %I:%M%p %Z", &tm_aos);
		strftime(los_buf, sizeof(los_buf), "%d %B %Y - %I:%M%p %Z", &tm_los);

		sat->next_aos = aos_time_t;
		sat->next_los = los_time_t;
		sat->aos_az = rad_to_deg(observation.azimuth);

		predict_orbit(orbital_elements, &orbit, los.time);
		predict_observe_orbit(observer, &orbit, &observation);

		sat->los_az = rad_to_deg(observation.azimuth);
		LOG_I("Max elevation %f deg., AOS on %s (az. %f), LOS on %s (az. %f)", max_elev, aos_buf, sat->aos_az, los_buf, sat->los_az);

		sat->zero_transition = sat_is_crossing_zero(sat->aos_az, sat->los_az);
		if (sat->zero_transition) {
			sat->aos_az = sat_reverse_azimuth(sat->aos_az);
			sat->los_az = sat_reverse_azimuth(sat->los_az);
			LOG_I("Zero transition, reversing azimuths");
			LOG_I("New AOS azimuth: %f", sat->aos_az);
			LOG_I("New LOS azimuth: %f", sat->los_az);
		}
		else
			LOG_I("No zero transition.");

	} else {
		ret = -1;
		LOG_I("Couldn't find the needed elevation");
	}

	satellite_t *iter;
	observation_t *obs = (observation_t *) sat->obs;

	/** reschedule if overlapped */
	LIST_FOREACH(iter, &obs->satellites_list, entries) {

		if (iter == sat)
			continue;

		/* printf("sat2: next los = %ld, next aos = %ld\n", sat->next_los, sat->next_aos); */
		/* printf("sat1: next los = %ld, next aos = %ld\n", iter->next_los, iter->next_aos); */

		if ((sat->next_los > iter->next_aos && sat->next_los < iter->next_los) ||
				(sat->next_aos < iter->next_los && sat->next_aos > iter->next_aos)) {

			if (sat->priority > iter->priority) {
				LOG_I("Overlap found, %s rescheduled", iter->name);
				sat_predict(iter);
			} else {
				LOG_I("Overlap found, %s rescheduled", sat->name);
				goto reschedule;
			}
		}
	}

	predict_destroy_observer(observer);
	predict_destroy_orbital_elements(orbital_elements);

	return ret;
}

int sat_setup(satellite_t *sat)
{
	int ret;
	char tle1[MAX_TLE_LEN] = { 0 };
	char tle2[MAX_TLE_LEN] = { 0 };

	ret = 0;

	if ((ret = sat_fetch_tle(sat->name, tle1, tle2)) == -1) {
		goto out;
	}

	strcpy(sat->tle1, tle1);
	strcpy(sat->tle2, tle2);

	LOG_I("Satellite found: [%s]", sat->name);
	LOG_I("TLE1: [%s]", tle1);
	LOG_I("TLE2: [%s]", tle2);

	sat_predict(sat);

out:
	return ret;
}

static observation_t *sat_alloc_observation_data(void)
{
	observation_t *obs = calloc(1, sizeof(observation_t));
	if (obs == NULL) {
		LOG_E("Error on malloc");
		return NULL;
	}

	obs->latitude = 48.5833f; /** FIXME */
	obs->longitude = 7.75f; /** FIXME */
	strncpy(obs->gs_name, "ISU GS", sizeof(obs->gs_name));

	setenv("TZ", "GMT", 1);
	tzset();

	LIST_INIT(&(obs)->satellites_list);

	obs->sch_terminate = false;

	/** FIXME */
	strncpy(obs->cli.addr, "127.0.0.1", sizeof(obs->cli.addr));
	obs->cli.port_az = 8080;
	obs->cli.port_el = 8081;

	if (rotctl_open(obs, ROT_TYPE_AZ) == -1) {
		printf("error\n");
	}

	if (rotctl_open(obs, ROT_TYPE_EL) == -1) {
		printf("error\n");
	}

	pthread_create(&obs->sch_thread, NULL, sat_scheduler, obs);
	return obs;
}

static int sat_clear_all(observation_t *obs)
{
	satellite_t *sat;

	while (!LIST_EMPTY(&obs->satellites_list)) {
		sat = LIST_FIRST(&obs->satellites_list);
		LIST_REMOVE(sat, entries);
		free(sat);
	}

	rotctl_close(obs, ROT_TYPE_AZ);
	rotctl_close(obs, ROT_TYPE_EL);

	obs->sch_terminate = true;
	pthread_join(obs->sch_thread, NULL);

	free(obs);

	return 0;
}

int sat_setup_observation()
{
	int ret;

	ret = 0;

	if (_observation) {
		LOG_I("Remove old observation entries");
		sat_clear_all(_observation);
	}

	if ((_observation = sat_alloc_observation_data()) == NULL) {
		LOG_E("Couldn't create a new observation entry");
	}

	return ret;
}

observation_t *sat_get_observation()
{
	return _observation;
}


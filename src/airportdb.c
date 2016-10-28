/*
 * CDDL HEADER START
 *
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 *
 * CDDL HEADER END
 *
 * Copyright 2016 Saso Kiselkov. All rights reserved.
 */

#include <errno.h>
#include <stddef.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "assert.h"
#include "avl.h"
#include "geom.h"
#include "helpers.h"
#include "list.h"
#include "log.h"
#include "math.h"
#include "types.h"

#include "airportdb.h"

#define	RWY_PROXIMITY_LAT_FRACT		3
#define	RWY_PROXIMITY_LON_DISPL		609.57		/* meters, 2000 ft */
#define	INIT_ERR_MSG_TIMEOUT		25		/* seconds */

#define	RWY_APCH_PROXIMITY_LAT_ANGLE	3.3	/* degrees */
#define	RWY_APCH_PROXIMITY_LON_DISPL	5500	/* meters */
/* precomputed, since it doesn't change */
#define	RWY_APCH_PROXIMITY_LAT_DISPL \
    RWY_APCH_PROXIMITY_LON_DISPL * \
    __builtin_tan(DEG2RAD(RWY_APCH_PROXIMITY_LAT_ANGLE))
#define	XRAAS_apt_dat_cache_version	3
#define	ARPT_LOAD_LIMIT			(8 * 1852)	/* meters, 8nm */

#define	TILE_NAME_FMT			"%+03.0f%+04.0f"

void log_init_msg(bool_t display, int timeout, int man_sect_number,
    const char *man_sect_name, const char *fmt, ...) PRINTF_ATTR(5);

static airport_t *apt_dat_lookup(avl_tree_t *apt_dat, const char *icao);
static void apt_dat_insert(avl_tree_t *apt_dat, airport_t *arpt);
static void free_airport(airport_t *arpt);

static int
apt_dat_compar(const void *a, const void *b)
{
	const airport_t *aa = a, *ab = b;
	int res = strcmp(aa->icao, ab->icao);
	if (res < 0)
		return (-1);
	else if (res == 0)
		return (0);
	else
		return (1);
}

static int
geo_table_compar(const void *a, const void *b)
{
	const tile_t *ta = a, *tb = b;

	if (ta->pos.lat < tb->pos.lat) {
		return (-1);
	} else if (ta->pos.lat == tb->pos.lat) {
		if (ta->pos.lon < tb->pos.lon)
			return (-1);
		else if (ta->pos.lon == tb->pos.lon)
			return (0);
		else
			return (1);
	} else {
		return (1);
	}
}

static int
runway_compar(const void *a, const void *b)
{
	const runway_t *ra = a, *rb = b;
	int res = strcmp(ra->joint_id, rb->joint_id);
	if (res != 0) {
		/* make sure the runways are not simply reversals */
		ASSERT(strcmp(ra->ends[0].id, rb->ends[1].id) != 0);
		ASSERT(strcmp(ra->ends[1].id, rb->ends[0].id) != 0);
	}
	if (res < 0)
		return (-1);
	else if (res == 0)
		return (0);
	else
		return (1);
}

/*
 * Retrieves the geo table tile which contains position `pos'. If create is
 * B_TRUE, if the tile doesn't exit, it will be created.
 * Returns the table tile (if it exists) and a boolean (in created_p if
 * non-NULL) informing whether the table tile was created in this call
 * (if create == B_TRUE).
 */
static tile_t *
geo_table_get_tile(avl_tree_t *geo_table, geo_pos2_t pos, bool_t create,
    bool_t *created_p)
{
	pos.lat = floor(pos.lat);
	pos.lon = floor(pos.lon);

	bool_t created = B_FALSE;
	tile_t srch = { .pos = pos };
	tile_t *tile;
	avl_index_t where;

	tile = avl_find(geo_table, &srch, &where);
	if (tile == NULL && create) {
		tile = malloc(sizeof (*tile));
		tile->pos = pos;
		list_create(&tile->arpts, sizeof (airport_t),
		    offsetof(airport_t, tile_node));
		avl_insert(geo_table, tile, where);
		created = B_TRUE;
	}
	if (created_p != NULL)
		*created_p = created;

	return (tile);
}

/*
 * Given a runway threshold vector, direction vector, width, length and
 * threshold longitudinal displacement, prepares a bounding box which
 * encompasses that runway.
 */
static vect2_t *
make_rwy_bbox(vect2_t thresh_v, vect2_t dir_v, double width, double len,
    double long_displ)
{
	vect2_t *bbox;
	vect2_t len_displ_v;

	bbox = malloc(sizeof (*bbox) * 5);

	/*
	 * Displace the 'a' point from the runway threshold laterally
	 * by 1/2 width to the right.
	 */
	bbox[0] = vect2_add(thresh_v, vect2_set_abs(vect2_norm(dir_v, B_TRUE),
	    width / 2));
	/* pull it back by `long_displ' */
	bbox[0] = vect2_add(bbox[0], vect2_set_abs(vect2_neg(dir_v),
	    long_displ));

	/* do the same for the `d' point, but displace to the left */
	bbox[3] = vect2_add(thresh_v, vect2_set_abs(vect2_norm(dir_v, B_FALSE),
	    width / 2));
	/* pull it back by `long_displ' */
	bbox[3] = vect2_add(bbox[3], vect2_set_abs(vect2_neg(dir_v),
	    long_displ));

	/*
	 * points `b' and `c' are along the runway simply as runway len +
	 * long_displ
	 */
	len_displ_v = vect2_set_abs(dir_v, len + long_displ);
	bbox[1] = vect2_add(bbox[0], len_displ_v);
	bbox[2] = vect2_add(bbox[3], len_displ_v);

	bbox[4] = NULL_VECT2;

	return (bbox);
}

/*
 * Checks if the numerical runway type `t' is a hard-surface runway. From
 * the X-Plane v850 apt.dat spec:
 *	t=1: asphalt
 *	t=2: concrete
 *	t=15: unspecified hard surface (transparent)
 */
static bool_t
rwy_is_hard(int t)
{
	return (t == 1 || t == 2 || t == 15);
}

static airport_t *
apt_dat_lookup(avl_tree_t *apt_dat, const char *icao)
{
	airport_t srch;
	my_strlcpy(srch.icao, icao, sizeof (srch.icao));
	return (avl_find(apt_dat, &srch, NULL));
}

static void
apt_dat_insert(avl_tree_t *apt_dat, airport_t *arpt)
{
	avl_index_t where;
	VERIFY(avl_find(apt_dat, arpt, &where) == NULL);
	avl_insert(apt_dat, arpt, where);
}

static void
geo_link_airport(avl_tree_t *geo_table, airport_t *arpt)
{
	tile_t *tile = geo_table_get_tile(geo_table, GEO3_TO_GEO2(arpt->refpt),
	    B_TRUE, NULL);
	ASSERT(!list_link_active(&arpt->tile_node));
	list_insert_tail(&tile->arpts, arpt);
	dbg_log("tile", 2, "geo_xref\t%s\t%f\t%f", arpt->icao, arpt->refpt.lat,
	    arpt->refpt.lon);
}

/*
 * Parses an apt.dat (or X-RAAS_apt_dat.cache) file, parses its contents
 * and reconstructs our state.apt_dat tree. This is called at the start of
 * X-RAAS to populate the airport and runway database.
 */
static size_t
map_apt_dat(airportdb_t *db, const char *apt_dat_fname, bool_t geo_link)
{
	FILE *apt_dat_f;
	size_t arpt_cnt = 0;
	airport_t *arpt = NULL;
	char *line = NULL;
	size_t linecap = 0;
	int line_num = 0;
	char **comps;
	size_t ncomps;

	dbg_log("tile", 2, "map_apt_dat(\"%s\")", apt_dat_fname);

	apt_dat_f = fopen(apt_dat_fname, "r");
	if (apt_dat_f == NULL)
		return (0);

	while (!feof(apt_dat_f)) {
		line_num++;
		if (getline(&line, &linecap, apt_dat_f) <= 0) {
			continue;
		}
		strip_space(line);
		if (strlen(line) == 0) {
			/*
			 * An empty line denotes the end of an airport entry,
			 * so stop adding runways to the previous airport.
			 */
			arpt = NULL;
			continue;
		}
		if (strstr(line, "1 ") == line) {
			const char *new_icao;
			double TA = 0, TL = 0;
			geo_pos3_t pos = NULL_GEO_POS3;

			comps = strsplit(line, " ", B_TRUE, &ncomps);
			if (ncomps < 5) {
				dbg_log("tile", 0, "%s:%d: malformed airport "
				    "entry, skipping. Offending line:\n%s",
				    apt_dat_fname, line_num, line);
				free_strlist(comps, ncomps);
				continue;
			}

			new_icao = comps[4];
			pos.elev = atof(comps[1]);
			arpt = apt_dat_lookup(&db->apt_dat, new_icao);
			if (ncomps >= 9 &&
			    strstr(comps[5], "TA:") == comps[5] &&
			    strstr(comps[6], "TL:") == comps[6] &&
			    strstr(comps[7], "LAT:") == comps[7] &&
			    strstr(comps[8], "LON:") == comps[8]) {
				TA = atof(&comps[5][3]);
				TL = atof(&comps[6][3]);
				pos.lat = atof(&comps[7][4]);
				pos.lon = atof(&comps[8][4]);
			} else if (arpt != NULL) {
				pos = arpt->refpt;
			}

			if (arpt == NULL) {
				arpt_cnt++;
				arpt = calloc(1, sizeof (*arpt));
				avl_create(&arpt->rwys, runway_compar,
				    sizeof(runway_t),
				    offsetof(runway_t, node));
				my_strlcpy(arpt->icao, new_icao,
				    sizeof (arpt->icao));
				arpt->refpt = pos;
				arpt->TL = TL;
				arpt->TA = TA;
				apt_dat_insert(&db->apt_dat, arpt);
				if (!IS_NULL_GEO_POS(pos)) {
					ASSERT(geo_link);
					geo_link_airport(&db->geo_table, arpt);
				} else {
					ASSERT(!geo_link);
				}
			} else {
				/*
				 * This airport was already known from
				 * a previously loaded apt.dat. Avoid
				 * overwriting its data.
				 */
				arpt = NULL;
			}
			free_strlist(comps, ncomps);
		} else if (strstr(line, "100 ") == line && arpt != NULL) {
			runway_t *rwy;

			comps = strsplit(line, " ", B_TRUE, &ncomps);
			if (ncomps < 8 + 9 + 5) {
				dbg_log("tile", 0, "%s:%d: malformed runway "
				    "entry, skipping. Offending line:\n%s",
				    apt_dat_fname, line_num, line);
				free_strlist(comps, ncomps);
				continue;
			}

			if (rwy_is_hard(atoi(comps[2]))) {
				avl_index_t where;

				rwy = calloc(1, sizeof (*rwy));

				rwy->arpt = arpt;
				rwy->width = atof(comps[1]);

				my_strlcpy(rwy->ends[0].id, comps[8 + 0],
				    sizeof (rwy->ends[0].id));
				rwy->ends[0].thr = GEO_POS3(atof(comps[8 + 1]),
				    atof(comps[8 + 2]), NAN);
				rwy->ends[0].displ = atof(comps[8 + 3]);
				rwy->ends[0].blast = atof(comps[8 + 4]);

				my_strlcpy(rwy->ends[1].id, comps[8 + 9 + 0],
				    sizeof (rwy->ends[1].id));
				rwy->ends[1].thr = GEO_POS3(
				    atof(comps[8 + 9 + 1]),
				    atof(comps[8 + 9 + 2]), NAN);
				rwy->ends[1].displ = atof(comps[8 + 9 + 3]);
				rwy->ends[1].blast = atof(comps[8 + 9 + 4]);

				snprintf(rwy->joint_id, sizeof (rwy->joint_id),
				    "%s%s", rwy->ends[0].id, rwy->ends[1].id);

				if (ncomps >= 28 &&
				    strstr(comps[22], "GPA1:") == comps[22] &&
				    strstr(comps[23], "GPA2:") == comps[23] &&
				    strstr(comps[24], "TCH1:") == comps[24] &&
				    strstr(comps[25], "TCH2:") == comps[25] &&
				    strstr(comps[26], "TELEV1:") == comps[26] &&
				    strstr(comps[27], "TELEV2:") == comps[27]) {
					rwy->ends[0].gpa = atof(&comps[22][5]);
					rwy->ends[1].gpa = atof(&comps[23][5]);
					rwy->ends[0].tch = atof(&comps[24][5]);
					rwy->ends[1].tch = atof(&comps[25][5]);
					rwy->ends[0].thr.elev =
					    atof(&comps[26][7]);
					rwy->ends[1].thr.elev =
					    atof(&comps[27][7]);
				}

				if (avl_find(&arpt->rwys, rwy, &where) !=
				    NULL) {
					logMsg("%s seems corrupted, it "
					    "contains a duplicate runway "
					    "entry %s/%s at airport %s",
					    apt_dat_fname, rwy->ends[0].id,
					    rwy->ends[1].id, arpt->icao);
					free(rwy);
					free_strlist(comps, ncomps);
					break;
				} else {
					avl_insert(&arpt->rwys, rwy, where);
				}
			}
			free_strlist(comps, ncomps);
		} else if (strstr(line, "1302 ") == line && arpt != NULL) {
			comps = strsplit(line, " ", B_TRUE, &ncomps);
			/* This line can contain varying numbers of comps */
			if (ncomps != 3) {
				free_strlist(comps, ncomps);
				continue;
			}
			if (strcmp(comps[1], "transition_alt") == 0)
				arpt->TA = atoi(comps[2]);
			else if (strcmp(comps[1], "transition_level") == 0)
				arpt->TL = atoi(comps[2]);
			free_strlist(comps, ncomps);
		}
	}

	free(line);
	fclose(apt_dat_f);

	return (arpt_cnt);
}

/*
 * Locates all apt.dat files used by X-Plane to display scenery. It consults
 * scenery_packs.ini to determine which scenery packs are currently enabled
 * and together with the default apt.dat returns them in a list sorted
 * numerically in preference order (lowest index for highest priority).
 * If the as_keys argument is true, the returned list is instead indexed
 * by the apt.dat file name and the values are the preference order of that
 * apt.dat (starting from 1 for highest priority and increasing with lowering
 * priority).
 * The apt.dat filenames are full filesystem paths.
 */
static char **
find_all_apt_dats(const airportdb_t *db, size_t *num)
{
	size_t n = 0;
	char *fname;
	FILE *scenery_packs_ini;
	char *line = NULL;
	size_t linecap = 0;
	char **apt_dats = NULL;

	fname = mkpathname(db->xpdir, "Custom Scenery", "scenery_packs.ini",
	    NULL);
	scenery_packs_ini = fopen(fname, "r");
	free(fname);

	if (scenery_packs_ini != NULL) {
		while (!feof(scenery_packs_ini)) {
			if (getline(&line, &linecap, scenery_packs_ini) <= 0)
				continue;
			strip_space(line);
			if (strstr(line, "SCENERY_PACK ") == line) {
				char *scn_name, *filename;
				FILE *fp;

				scn_name = strdup(&line[13]);
				strip_space(scn_name);
				filename = mkpathname(db->xpdir, scn_name,
				    "Earth nav data", "apt.dat", NULL);
				fp = fopen(filename, "r");
				if (fp != NULL) {
					fclose(fp);
					n++;
					apt_dats = realloc(apt_dats,
					    n * sizeof (char *));
					apt_dats[n - 1] = filename;
				} else {
					free(filename);
				}
				free(scn_name);
			}
		}
		fclose(scenery_packs_ini);
	}

	/* append the default apt.dat */
	n += 1;
	apt_dats = realloc(apt_dats, n * sizeof (char *));
	apt_dats[n - 1] = mkpathname(db->xpdir, "Resources", "default scenery",
	    "default apt dat", "Earth nav data", "apt.dat", NULL);

	free(line);

	if (num != NULL)
		*num = n;

	return (apt_dats);
}

/*
 * Reloads ~/GNS430/navdata/Airports.txt and populates our apt_dat airports
 * with the latest info in it, notably:
 * *) transition altitudes & transition levels for the airports
 * *) runway threshold elevation, glide path angle & threshold crossing height
 */
static bool_t
load_airports_txt(airportdb_t *db)
{
	char *fname;
	FILE *fp;
	char *line = NULL;
	size_t linecap = 0;
	int line_num = 0;
	char **comps;
	size_t ncomps;
	airport_t *arpt = NULL;

	/* We first try the Custom Data version, as that's more up to date */
	fname = mkpathname(db->xpdir, "Custom Data", "GNS430", "navdata",
	    "Airports.txt", NULL);
	fp = fopen(fname, "r");

	if (fp == NULL) {
		/* Try the Airports.txt shipped with X-Plane. */
		free(fname);
		fname = mkpathname(db->xpdir, "Resources", "GNS430", "navdata",
		    "Airports.txt", NULL);
		fp = fopen(fname, "r");

		if (fp == NULL) {
			free(fname);
			log_init_msg(B_TRUE, INIT_ERR_MSG_TIMEOUT, 2,
			    "Installation", "X-RAAS navdata error: your "
			    "Airports.txt is missing or unreadable. "
			    "Please correct this and recreate the cache.");
			return (B_FALSE);
		}
	}

	while (!feof(fp)) {
		line_num++;
		if (getline(&line, &linecap, fp) <= 0)
			continue;
		strip_space(line);
		if (strstr(line, "A,") == line) {
			char *icao;

			comps = strsplit(line, ",", B_FALSE, &ncomps);
			if (ncomps < 8) {
				dbg_log("tile", 0, "%s:%d: malformed airport "
				    "entry, skipping. Offending line:\n%s",
				    fname, line_num, line);
				free_strlist(comps, ncomps);
				continue;
			}
			icao = comps[1];
			arpt = apt_dat_lookup(&db->apt_dat, icao);

			if (arpt != NULL) {
				arpt->refpt = GEO_POS3(atof(comps[3]),
				    atof(comps[4]), arpt->refpt.elev);
				geo_link_airport(&db->geo_table, arpt);
				arpt->TA = atof(comps[6]);
				arpt->TL = atof(comps[7]);
			}
			free_strlist(comps, ncomps);
		} else if (strstr(line, "R,") == line) {
			char *rwy_id;
			double telev, gpa, tch;

			if (arpt == NULL) {
				/* airport not in scenery database, skip it */
				continue;
			}

			comps = strsplit(line, ",", B_FALSE, &ncomps);
			if (ncomps < 13) {
				dbg_log("tile", 0, "%s:%d: malformed runway "
				    "entry, skipping. Offending line:\n%s",
				    fname, line_num, line);
				free_strlist(comps, ncomps);
				continue;
			}

			rwy_id = comps[1];
			telev = atof(comps[10]);
			gpa = atof(comps[11]);
			tch = atof(comps[12]);

			for (runway_t *rwy = avl_first(&arpt->rwys);
			    rwy != NULL; rwy = AVL_NEXT(&arpt->rwys, rwy)) {
				if (strcmp(rwy->ends[0].id, rwy_id) == 0) {
					rwy->ends[0].thr.elev = telev;
					rwy->ends[0].gpa = gpa;
					rwy->ends[0].tch = tch;
					break;
				} else if (strcmp(rwy->ends[1].id, rwy_id) ==
				    0) {
					rwy->ends[1].thr.elev = telev;
					rwy->ends[1].gpa = gpa;
					rwy->ends[1].tch = tch;
					break;
				}
			}
			free_strlist(comps, ncomps);
		}
	}

	fclose(fp);
	free(fname);

	return (B_TRUE);
}

static char *
apt_dat_cache_dir(const airportdb_t *db, geo_pos2_t pos, const char *suffix)
{
	char lat_lon[16];

	VERIFY(!IS_NULL_GEO_POS(pos));
	snprintf(lat_lon, sizeof (lat_lon), TILE_NAME_FMT,
	    floor(pos.lat / 10) * 10, floor(pos.lon / 10) * 10);

	if (suffix != NULL)
		return (mkpathname(db->xpprefsdir, "X-RAAS_apt_dat_cache",
		    lat_lon, suffix, NULL));
	else
		return (mkpathname(db->xpprefsdir, "X-RAAS_apt_dat_cache",
		    lat_lon, NULL));
}

static bool_t
write_apt_dat(const airportdb_t *db, const airport_t *arpt)
{
	char lat_lon[16];
	char *fname;
	FILE *fp;

	snprintf(lat_lon, sizeof (lat_lon), TILE_NAME_FMT,
	    arpt->refpt.lat, arpt->refpt.lon);
	fname = apt_dat_cache_dir(db, GEO3_TO_GEO2(arpt->refpt), lat_lon);

	fp = fopen(fname, "a");
	if (fp == NULL) {
		logMsg("Error writing file %s: %s", fname, strerror(errno));
		return (B_FALSE);
	}

	fprintf(fp, "1 %f 0 0 %s TA:%.0f TL:%.0f LAT:%f LON:%f\n",
	    arpt->refpt.elev, arpt->icao, arpt->TL, arpt->TA,
	    arpt->refpt.lat, arpt->refpt.lon);
	for (const runway_t *rwy = avl_first(&arpt->rwys); rwy != NULL;
	    rwy = AVL_NEXT(&arpt->rwys, rwy)) {
		fprintf(fp, "100 %.2f 1 0 0 0 0 0 "
		    "%s %f %f %.1f %.1f 0 0 0 0 "
		    "%s %f %f %.1f %.1f "
		    "GPA1:%f GPA2:%f TCH1:%f TCH2:%f TELEV1:%f TELEV2:%f\n",
		    rwy->width,
		    rwy->ends[0].id, rwy->ends[0].thr.lat,
		    rwy->ends[0].thr.lon, rwy->ends[0].displ,
		    rwy->ends[0].blast,
		    rwy->ends[1].id, rwy->ends[1].thr.lat,
		    rwy->ends[1].thr.lon, rwy->ends[1].displ,
		    rwy->ends[1].blast,
		    rwy->ends[0].gpa, rwy->ends[1].gpa,
		    rwy->ends[0].tch, rwy->ends[1].tch,
		    rwy->ends[0].thr.elev, rwy->ends[1].thr.elev);
	}
	fclose(fp);
	free(fname);

	return (B_TRUE);
}

/*
 * Some airports appear in apt.dat files, but not in the Airports.txt, but
 * apt.dat doesn't tell us their airport reference point. Thus we do the
 * next best thing and auto-compute the lat/lon as the arithmetic average
 * of the lat/lon of the first runway's thresholds.
 */
static void
airport_auto_refpt(airportdb_t *db, airport_t *arpt)
{
	runway_t *rwy;
	geo_pos3_t p1, p2;

	rwy = avl_first(&arpt->rwys);
	ASSERT(IS_NULL_GEO_POS(arpt->refpt));
	ASSERT(rwy != NULL);

	p1 = rwy->ends[0].thr;
	p2 = rwy->ends[1].thr;
	/* Just to make sure there are no airports on the date line. */
	ASSERT(fabs(p1.lon - p2.lon) < 90);
	arpt->refpt.lat = (p1.lat + p2.lat) / 2;
	arpt->refpt.lon = (p1.lon + p2.lon) / 2;
	ASSERT(!isnan(arpt->refpt.elev));

	/*
	 * Because the airport lacked a refpt, it's not in the
	 * airport_geo_tree, so add it back in.
	 */
	geo_link_airport(&db->geo_table, arpt);
}

/*
 * Takes the current state of the apt_dat table and writes all the
 * airports in it to the X-RAAS_apt_dat.cache so that a subsequent run
 * of X-RAAS can pick this info up.
 */
bool_t
recreate_apt_dat_cache(airportdb_t *db)
{
	char *version_filename;
	int version = -1;
	FILE *version_file;

	version_filename = mkpathname(db->xpprefsdir, "X-RAAS_apt_dat_cache",
	    "version", NULL);
	version_file = fopen(version_filename, "r");
	if (version_file != NULL) {
		if (fscanf(version_file, "%d", &version) != 1)
			version = -1;
		fclose(version_file);
	}

	if (version == XRAAS_apt_dat_cache_version) {
		/* cache version current, no need to rebuild it */
		free(version_filename);
		dbg_log("tile", 1, "X-RAAS_apt_dat_cache up to date");
		return (B_TRUE);
	}
	dbg_log("tile", 1, "X-RAAS_apt_dat_cache out of date");

	size_t n_apt_dat_files;
	char **apt_dat_files = find_all_apt_dats(db, &n_apt_dat_files);
	bool_t success = B_TRUE;
	char *filename;

	/* First scan all the provided apt.dat files */
	for (size_t i = 0; i < n_apt_dat_files; i++)
		map_apt_dat(db, apt_dat_files[i], B_FALSE);

	/*
	 * Remove airports without runways. We can do this step now, because
	 * no airports have yet been linked in to airport_geo_tree.
	 */
	airport_t *arpt, *next_arpt;
	for (arpt = avl_first(&db->apt_dat); arpt != NULL; arpt = next_arpt) {
		next_arpt = AVL_NEXT(&db->apt_dat, arpt);
		if (avl_first(&arpt->rwys) == NULL) {
			avl_remove(&db->apt_dat, arpt);
			free_airport(arpt);
		}
	}

	/* After this step, removal of airports must be done through tiles. */
	if (!load_airports_txt(db)) {
		success = B_FALSE;
		goto out;
	}

	filename = mkpathname(db->xpprefsdir, "X-RAAS_apt_dat_cache", NULL);
	remove_directory(filename);
	if (!create_directory(filename)) {
		free(filename);
		success = B_FALSE;
		goto out;
	}
	free(filename);
	version_file = fopen(version_filename, "w");
	if (version_file == NULL) {
		success = B_FALSE;
		goto out;
	}
	fprintf(version_file, "%d\n", XRAAS_apt_dat_cache_version);
	fclose(version_file);

	for (arpt = avl_first(&db->apt_dat); arpt != NULL;
	    arpt = AVL_NEXT(&db->apt_dat, arpt)) {
		char *dirname;

		if (IS_NULL_GEO_POS(arpt->refpt))
			airport_auto_refpt(db, arpt);
		dirname = apt_dat_cache_dir(db, GEO3_TO_GEO2(arpt->refpt),
		    NULL);
		if (!create_directory(dirname) || !write_apt_dat(db, arpt)) {
			free(dirname);
			success = B_FALSE;
			goto out;
		}
		free(dirname);
	}
out:
	unload_distant_airport_tiles(db, NULL_GEO_POS2);

	free_strlist(apt_dat_files, n_apt_dat_files);
	free(version_filename);

	return (success);
}

/*
 * The approach proximity bounding box is constructed as follows:
 *
 *   5500 meters
 *   |<=======>|
 *   |         |
 * d +-_  (c1) |
 *   |   -._3 degrees
 *   |      -_ c
 *   |         +-------------------------------+
 *   |         | ====  ----         ----  ==== |
 * x +   thr_v-+ ==== - ------> dir_v - - ==== |
 *   |         | ====  ----         ----  ==== |
 *   |         +-------------------------------+
 *   |      _- b
 *   |   _-.
 * a +--    (b1)
 *
 * If there is another parallel runway, we make sure our bounding boxes
 * don't overlap. We do this by introducing two additional points, b1 and
 * c1, in between a and b or c and d respectively. We essentially shear
 * the overlapping excess from the bounding polygon.
 */
static vect2_t *
make_apch_prox_bbox(const runway_t *rwy, int end_i)
{
	const runway_end_t *end, *oend;
	const fpp_t *fpp = &rwy->arpt->fpp;
	double limit_left = 1000000, limit_right = 1000000;
	vect2_t x, a, b, b1, c, c1, d, thr_v, othr_v, dir_v;
	vect2_t *bbox = calloc(7, sizeof (vect2_t));
	size_t n_pts = 0;

	ASSERT(fpp != NULL);
	ASSERT(end_i == 0 || end_i == 1);

	end = &rwy->ends[end_i];
	oend = &rwy->ends[!end_i];

	/*
	 * By pre-initing the whole array to null vectors, we can make the
	 * bbox either contain 4, 5 or 6 points, depending on whether
	 * shearing due to a close parallel runway needs to be applied.
	 */
	for (int i = 0; i < 7; i++)
		bbox[i] = NULL_VECT2;

	thr_v = geo2fpp(GEO3_TO_GEO2(end->thr), fpp);
	othr_v = geo2fpp(GEO3_TO_GEO2(oend->thr), fpp);
	dir_v = vect2_sub(othr_v, thr_v);

	x = vect2_add(thr_v, vect2_set_abs(vect2_neg(dir_v),
	    RWY_APCH_PROXIMITY_LON_DISPL));
	a = vect2_add(x, vect2_set_abs(vect2_norm(dir_v, B_TRUE),
	    rwy->width / 2 + RWY_APCH_PROXIMITY_LAT_DISPL));
	b = vect2_add(thr_v, vect2_set_abs(vect2_norm(dir_v, B_TRUE),
	    rwy->width / 2));
	c = vect2_add(thr_v, vect2_set_abs(vect2_norm(dir_v, B_FALSE),
	    rwy->width / 2));
	d = vect2_add(x, vect2_set_abs(vect2_norm(dir_v, B_FALSE),
	    rwy->width / 2 + RWY_APCH_PROXIMITY_LAT_DISPL));

	b1 = NULL_VECT2;
	c1 = NULL_VECT2;

	/*
	 * If our rwy_id designator contains a L/C/R, then we need to
	 * look for another parallel runway.
	 */
	if (strlen(end->id) >= 3) {
		int my_num_id = atoi(end->id);

		for (const runway_t *orwy = avl_first(&rwy->arpt->rwys);
		    orwy != NULL; orwy = AVL_NEXT(&rwy->arpt->rwys, orwy)) {
			const runway_end_t *orwy_end;
			vect2_t othr_v, v;
			double a, dist;

			if (orwy == rwy)
				continue;
			if (atoi(orwy->ends[0].id) == my_num_id)
				orwy_end = &orwy->ends[0];
			else if (atoi(orwy->ends[1].id) == my_num_id)
				orwy_end = &orwy->ends[1];
			else
				continue;

			/*
			 * This is a parallel runway, measure the
			 * distance to it from us.
			 */
			othr_v = geo2fpp(GEO3_TO_GEO2(orwy_end->thr), fpp);
			v = vect2_sub(othr_v, thr_v);
			a = rel_hdg(dir2hdg(dir_v), dir2hdg(v));
			dist = fabs(sin(DEG2RAD(a)) * vect2_abs(v));

			if (a < 0)
				limit_left = MIN(dist / 2, limit_left);
			else
				limit_right = MIN(dist / 2, limit_right);
		}
	}

	if (limit_left < RWY_APCH_PROXIMITY_LAT_DISPL) {
		c1 = vect2vect_isect(vect2_sub(d, c), c, vect2_neg(dir_v),
		    vect2_add(thr_v, vect2_set_abs(vect2_norm(dir_v, B_FALSE),
		    limit_left)), B_FALSE);
		d = vect2_add(x, vect2_set_abs(vect2_norm(dir_v, B_FALSE),
		    limit_left));
	}
	if (limit_right < RWY_APCH_PROXIMITY_LAT_DISPL) {
		b1 = vect2vect_isect(vect2_sub(b, a), a, vect2_neg(dir_v),
		    vect2_add(thr_v, vect2_set_abs(vect2_norm(dir_v, B_TRUE),
		    limit_right)), B_FALSE);
		a = vect2_add(x, vect2_set_abs(vect2_norm(dir_v, B_TRUE),
		    limit_right));
	}

	bbox[n_pts++] = a;
	if (!IS_NULL_VECT(b1))
		bbox[n_pts++] = b1;
	bbox[n_pts++] = b;
	bbox[n_pts++] = c;
	if (!IS_NULL_VECT(c1))
		bbox[n_pts++] = c1;
	bbox[n_pts++] = d;

	return (bbox);
}

/*
 * Prepares a runway's bounding box vector coordinates using the airport
 * coord fpp transform.
 */
static void
load_rwy_info(runway_t *rwy)
{
	/*
	 * RAAS runway proximity entry bounding box is defined as:
	 *
	 *              1000ft                                   1000ft
	 *            |<======>|                               |<======>|
	 *            |        |                               |        |
	 *     ---- d +-------------------------------------------------+ c
	 * 1.5x  ^    |        |                               |        |
	 *  rwy  |    |        |                               |        |
	 * width |    |        +-------------------------------+        |
	 *       v    |        | ====  ----         ----  ==== |        |
	 *     -------|-thresh-x ==== - - - - - - - - - - ==== |        |
	 *       ^    |        | ====  ----         ----  ==== |        |
	 * 1.5x  |    |        +-------------------------------+        |
	 *  rwy  |    |                                                 |
	 * width v    |                                                 |
	 *     ---- a +-------------------------------------------------+ b
	 */
	vect2_t dt1v = geo2fpp(GEO3_TO_GEO2(rwy->ends[0].thr), &rwy->arpt->fpp);
	vect2_t dt2v = geo2fpp(GEO3_TO_GEO2(rwy->ends[1].thr), &rwy->arpt->fpp);
	double displ1 = rwy->ends[0].displ;
	double displ2 = rwy->ends[1].displ;
	double blast1 = rwy->ends[0].blast;
	double blast2 = rwy->ends[1].blast;

	vect2_t dir_v = vect2_sub(dt2v, dt1v);
	double dlen = vect2_abs(dir_v);
	double hdg1 = dir2hdg(dir_v);
	double hdg2 = dir2hdg(vect2_neg(dir_v));

	vect2_t t1v = vect2_add(dt1v, vect2_set_abs(dir_v, displ1));
	vect2_t t2v = vect2_add(dt2v, vect2_set_abs(vect2_neg(dir_v), displ2));
	double len = vect2_abs(vect2_sub(t2v, t1v));

	double prox_lon_bonus1 = MAX(displ1, RWY_PROXIMITY_LON_DISPL - displ1);
	double prox_lon_bonus2 = MAX(displ2, RWY_PROXIMITY_LON_DISPL - displ2);

	rwy->ends[0].thr_v = t1v;
	rwy->ends[1].thr_v = t2v;
	rwy->ends[0].dthr_v = dt1v;
	rwy->ends[1].dthr_v = dt2v;
	rwy->ends[0].hdg = hdg1;
	rwy->ends[1].hdg = hdg2;
	rwy->ends[0].land_len = vect2_abs(vect2_sub(dt2v, t1v));
	rwy->ends[1].land_len = vect2_abs(vect2_sub(dt1v, t2v));
	rwy->length = len;

	ASSERT(rwy->rwy_bbox == NULL);

	rwy->rwy_bbox = make_rwy_bbox(t1v, dir_v, rwy->width, len, 0);
	rwy->tora_bbox = make_rwy_bbox(dt1v, dir_v, rwy->width, dlen, 0);
	rwy->asda_bbox = make_rwy_bbox(dt1v, dir_v, rwy->width,
	    dlen + blast2, blast1);
	rwy->prox_bbox = make_rwy_bbox(t1v, dir_v, RWY_PROXIMITY_LAT_FRACT *
	    rwy->width, len + prox_lon_bonus2, prox_lon_bonus1);

	rwy->ends[0].apch_bbox = make_apch_prox_bbox(rwy, 0);
	rwy->ends[1].apch_bbox = make_apch_prox_bbox(rwy, 1);
}

static void
unload_rwy_info(runway_t *rwy)
{
	ASSERT(rwy->rwy_bbox != NULL);

	free(rwy->rwy_bbox);
	rwy->rwy_bbox = NULL;
	free(rwy->tora_bbox);
	rwy->tora_bbox = NULL;
	free(rwy->asda_bbox);
	rwy->asda_bbox = NULL;
	free(rwy->prox_bbox);
	rwy->prox_bbox = NULL;

	free(rwy->ends[0].apch_bbox);
	rwy->ends[0].apch_bbox = NULL;
	free(rwy->ends[1].apch_bbox);
	rwy->ends[1].apch_bbox = NULL;
}

/*
 * Given an airport, loads the information of the airport into a more readily
 * workable (but more verbose) format. This function prepares a flat plane
 * transform centered on the airport's reference point and pre-computes all
 * relevant points for the airport in that space.
 */
static void
load_airport(airport_t *arpt)
{
	if (arpt->load_complete)
		return;

	arpt->fpp = ortho_fpp_init(GEO3_TO_GEO2(arpt->refpt), 0, &wgs84,
	    B_FALSE);
	arpt->ecef = sph2ecef(arpt->refpt);

	for (runway_t *rwy = avl_first(&arpt->rwys); rwy != NULL;
	    rwy = AVL_NEXT(&arpt->rwys, rwy))
		load_rwy_info(rwy);

	arpt->load_complete = B_TRUE;
}

static void
unload_airport(airport_t *arpt)
{
	if (!arpt->load_complete)
		return;
	for (runway_t *rwy = avl_first(&arpt->rwys); rwy != NULL;
	    rwy = AVL_NEXT(&arpt->rwys, rwy))
		unload_rwy_info(rwy);
	arpt->load_complete = B_FALSE;
}

static void
free_airport(airport_t *arpt)
{
	void *cookie = NULL;
	runway_t *rwy;

	ASSERT(!arpt->load_complete);

	while ((rwy = avl_destroy_nodes(&arpt->rwys, &cookie)) != NULL)
		free(rwy);
	avl_destroy(&arpt->rwys);
	ASSERT(!list_link_active(&arpt->cur_arpts_node));
	ASSERT(!list_link_active(&arpt->tile_node));
	free(arpt);
}

/*
 * The actual worker function for find_nearest_airports. Performs the
 * search in a specified geo_table tile. Position is a 3-space ECEF vector.
 */
static void
find_nearest_airports_tile(airportdb_t *db, vect3_t ecef,
    geo_pos2_t tile_coord, list_t *l)
{
	tile_t *tile = geo_table_get_tile(&db->geo_table, tile_coord, B_FALSE,
	    NULL);

	if (tile == NULL)
		return;
	for (airport_t *arpt = list_head(&tile->arpts); arpt != NULL;
	    arpt = list_next(&tile->arpts, arpt)) {
		vect3_t arpt_ecef = sph2ecef(arpt->refpt);
		if (vect3_abs(vect3_sub(ecef, arpt_ecef)) < ARPT_LOAD_LIMIT) {
			list_insert_tail(l, arpt);
			load_airport(arpt);
		}
	}
}

/*
 * Locates all airports within an ARPT_LOAD_LIMIT distance limit (in meters)
 * of a geographic reference position. The airports are searched for in the
 * apt_dat database and this function returns its result into the list argument.
 */
list_t *
find_nearest_airports(airportdb_t *db, geo_pos2_t my_pos)
{
	vect3_t ecef = sph2ecef(GEO_POS3(my_pos.lat, my_pos.lon, 0));
	list_t *l;

	l = malloc(sizeof (*l));
	list_create(l, sizeof (airport_t), offsetof(airport_t, cur_arpts_node));
	for (int i = -1; i <= 1; i++) {
		for (int j = -1; j <= 1; j++)
			find_nearest_airports_tile(db, ecef,
			    GEO_POS2(my_pos.lat + i, my_pos.lon + j), l);
	}

	return (l);
}

void
free_nearest_airport_list(list_t *l)
{
	for (airport_t *a = list_head(l); a != NULL; a = list_head(l))
		list_remove(l, a);
	free(l);
}

static void
load_airports_in_tile(airportdb_t *db, geo_pos2_t tile_pos)
{
	bool_t created;
	char *cache_dir, *fname;
	char lat_lon[16];

	(void) geo_table_get_tile(&db->geo_table, tile_pos, B_TRUE, &created);
	if (!created)
		return;

	cache_dir = apt_dat_cache_dir(db, tile_pos, NULL);
	snprintf(lat_lon, sizeof (lat_lon), TILE_NAME_FMT,
	    tile_pos.lat, tile_pos.lon);
	fname = mkpathname(cache_dir, lat_lon, NULL);
	map_apt_dat(db, fname, B_TRUE);
	free(cache_dir);
	free(fname);
}

static void
free_tile(airportdb_t *db, tile_t *tile)
{
	for (airport_t *arpt = list_head(&tile->arpts); arpt != NULL;
	    arpt = list_head(&tile->arpts)) {
		list_remove(&tile->arpts, arpt);
		avl_remove(&db->apt_dat, arpt);
		unload_airport(arpt);
		free_airport(arpt);
	}
	list_destroy(&tile->arpts);
	free(tile);
}

static void
unload_tile(airportdb_t *db, tile_t *tile)
{
	avl_remove(&db->geo_table, tile);
	free_tile(db, tile);
}

void
load_nearest_airport_tiles(airportdb_t *db, geo_pos2_t my_pos)
{
	for (int i = -1; i <= 1; i++) {
		for (int j = -1; j <= 1; j++)
			load_airports_in_tile(db, GEO_POS2(my_pos.lat + i,
			    my_pos.lon + j));
	}
}

static double
lon_delta(double x, double y)
{
	double u = MAX(x, y), d = MIN(x, y);

	if (u - d <= 180)
		return (fabs(u - d));
	else
		return (fabs((180 - u) - (-180 - d)));
}

void
unload_distant_airport_tiles_i(airportdb_t *db, tile_t *tile, geo_pos2_t my_pos)
{
	if (fabs(tile->pos.lat - floor(my_pos.lat)) > 1 ||
	    lon_delta(tile->pos.lon, floor(my_pos.lon)) > 1) {
		dbg_log("tile", 1, "unloading tile %.0f x %.0f",
		    tile->pos.lat, tile->pos.lon);
		unload_tile(db, tile);
	}
}

void
unload_distant_airport_tiles(airportdb_t *db, geo_pos2_t my_pos)
{
	tile_t *tile, *next_tile;

	for (tile = avl_first(&db->geo_table); tile != NULL; tile = next_tile) {
		next_tile = AVL_NEXT(&db->geo_table, tile);
		unload_distant_airport_tiles_i(db, tile, my_pos);
	}

	if (IS_NULL_GEO_POS(my_pos)) {
		ASSERT(avl_numnodes(&db->geo_table) == 0);
		ASSERT(avl_numnodes(&db->apt_dat) == 0);
	}
}

void
airportdb_create(airportdb_t *db, const char *xpdir, const char *xpprefsdir)
{
	db->xpdir = strdup(xpdir);
	db->xpprefsdir = strdup(xpprefsdir);

	avl_create(&db->apt_dat, apt_dat_compar, sizeof (airport_t),
	    offsetof(airport_t, apt_dat_node));
	avl_create(&db->geo_table, geo_table_compar, sizeof (tile_t),
	    offsetof(tile_t, node));
}

void
airportdb_destroy(airportdb_t *db)
{
	tile_t *tile;
	void *cookie = NULL;

	/* airports are freed in the free_tile function */
	while ((tile = avl_destroy_nodes(&db->geo_table, &cookie)) !=
	    NULL)
		free_tile(db, tile);
	avl_destroy(&db->geo_table);
	avl_destroy(&db->apt_dat);
}

airport_t *
airport_lookup(airportdb_t *db, const char *icao, geo_pos2_t pos)
{
	load_airports_in_tile(db, pos);
	return (apt_dat_lookup(&db->apt_dat, icao));
}

airport_t *
any_airport_at_coords(airportdb_t *db, geo_pos2_t pos)
{
	tile_t *tile;

	/* Grab the first airport in that tile */
	load_airports_in_tile(db, pos);
	tile = geo_table_get_tile(&db->geo_table, pos, B_FALSE, NULL);
	if (tile != NULL)
		return (list_head(&tile->arpts));
	return (NULL);
}
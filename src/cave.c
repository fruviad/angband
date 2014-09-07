/*
 * File: cave.c
 * Purpose: Lighting and update functions
 *
 * Copyright (c) 1997 Ben Harrison, James E. Wilson, Robert A. Koeneke
 *
 * This work is free software; you can redistribute it and/or modify it
 * under the terms of either:
 *
 * a) the GNU General Public License as published by the Free Software
 *    Foundation, version 2, or
 *
 * b) the "Angband licence":
 *    This software may be copied and distributed for educational, research,
 *    and not for profit purposes provided that this copyright and statement
 *    are included in all such copies.  Other copyrights may also apply.
 */

#include "angband.h"
#include "cave.h"
#include "cmds.h"
#include "dungeon.h"
#include "cmd-core.h"
#include "game-event.h"
#include "init.h"
#include "monster.h"
#include "obj-ignore.h"
#include "obj-tval.h"
#include "obj-util.h"
#include "object.h"
#include "player-timed.h"
#include "tables.h"
#include "trap.h"

feature_type *f_info;

/*
 * Approximate distance between two points.
 *
 * When either the X or Y component dwarfs the other component,
 * this function is almost perfect, and otherwise, it tends to
 * over-estimate about one grid per fifteen grids of distance.
 *
 * Algorithm: hypot(dy,dx) = max(dy,dx) + min(dy,dx) / 2
 */
int distance(int y1, int x1, int y2, int x2)
{
	/* Find the absolute y/x distance components */
	int ay = abs(y2 - y1);
	int ax = abs(x2 - x1);

	/* Approximate the distance */
	return ay > ax ? ay + (ax>>1) : ax + (ay>>1);
}


/*
 * A simple, fast, integer-based line-of-sight algorithm.  By Joseph Hall,
 * 4116 Brewster Drive, Raleigh NC 27606.  Email to jnh@ecemwl.ncsu.edu.
 *
 * This function returns TRUE if a "line of sight" can be traced from the
 * center of the grid (x1,y1) to the center of the grid (x2,y2), with all
 * of the grids along this path (except for the endpoints) being non-wall
 * grids.  Actually, the "chess knight move" situation is handled by some
 * special case code which allows the grid diagonally next to the player
 * to be obstructed, because this yields better gameplay semantics.  This
 * algorithm is totally reflexive, except for "knight move" situations.
 *
 * Because this function uses (short) ints for all calculations, overflow
 * may occur if dx and dy exceed 90.
 *
 * Once all the degenerate cases are eliminated, we determine the "slope"
 * ("m"), and we use special "fixed point" mathematics in which we use a
 * special "fractional component" for one of the two location components
 * ("qy" or "qx"), which, along with the slope itself, are "scaled" by a
 * scale factor equal to "abs(dy*dx*2)" to keep the math simple.  Then we
 * simply travel from start to finish along the longer axis, starting at
 * the border between the first and second tiles (where the y offset is
 * thus half the slope), using slope and the fractional component to see
 * when motion along the shorter axis is necessary.  Since we assume that
 * vision is not blocked by "brushing" the corner of any grid, we must do
 * some special checks to avoid testing grids which are "brushed" but not
 * actually "entered".
 *
 * Angband three different "line of sight" type concepts, including this
 * function (which is used almost nowhere), the "project()" method (which
 * is used for determining the paths of projectables and spells and such),
 * and the "update_view()" concept (which is used to determine which grids
 * are "viewable" by the player, which is used for many things, such as
 * determining which grids are illuminated by the player's torch, and which
 * grids and monsters can be "seen" by the player, etc).
 */
bool los(struct chunk *c, int y1, int x1, int y2, int x2)
{
	/* Delta */
	int dx, dy;

	/* Absolute */
	int ax, ay;

	/* Signs */
	int sx, sy;

	/* Fractions */
	int qx, qy;

	/* Scanners */
	int tx, ty;

	/* Scale factors */
	int f1, f2;

	/* Slope, or 1/Slope, of LOS */
	int m;


	/* Extract the offset */
	dy = y2 - y1;
	dx = x2 - x1;

	/* Extract the absolute offset */
	ay = ABS(dy);
	ax = ABS(dx);


	/* Handle adjacent (or identical) grids */
	if ((ax < 2) && (ay < 2)) return (TRUE);


	/* Directly South/North */
	if (!dx)
	{
		/* South -- check for walls */
		if (dy > 0)
		{
			for (ty = y1 + 1; ty < y2; ty++)
			{
				if (!square_isprojectable(c, ty, x1)) return (FALSE);
			}
		}

		/* North -- check for walls */
		else
		{
			for (ty = y1 - 1; ty > y2; ty--)
			{
				if (!square_isprojectable(c, ty, x1)) return (FALSE);
			}
		}

		/* Assume los */
		return (TRUE);
	}

	/* Directly East/West */
	if (!dy)
	{
		/* East -- check for walls */
		if (dx > 0)
		{
			for (tx = x1 + 1; tx < x2; tx++)
			{
				if (!square_isprojectable(c, y1, tx)) return (FALSE);
			}
		}

		/* West -- check for walls */
		else
		{
			for (tx = x1 - 1; tx > x2; tx--)
			{
				if (!square_isprojectable(c, y1, tx)) return (FALSE);
			}
		}

		/* Assume los */
		return (TRUE);
	}


	/* Extract some signs */
	sx = (dx < 0) ? -1 : 1;
	sy = (dy < 0) ? -1 : 1;

	/* Vertical "knights" */
	if (ax == 1)
	{
		if (ay == 2)
		{
			if (square_isprojectable(c, y1 + sy, x1)) return (TRUE);
		}
	}

	/* Horizontal "knights" */
	else if (ay == 1)
	{
		if (ax == 2)
		{
			if (square_isprojectable(c, y1, x1 + sx)) return (TRUE);
		}
	}

	/* Calculate scale factor div 2 */
	f2 = (ax * ay);

	/* Calculate scale factor */
	f1 = f2 << 1;


	/* Travel horizontally */
	if (ax >= ay)
	{
		/* Let m = dy / dx * 2 * (dy * dx) = 2 * dy * dy */
		qy = ay * ay;
		m = qy << 1;

		tx = x1 + sx;

		/* Consider the special case where slope == 1. */
		if (qy == f2)
		{
			ty = y1 + sy;
			qy -= f1;
		}
		else
		{
			ty = y1;
		}

		/* Note (below) the case (qy == f2), where */
		/* the LOS exactly meets the corner of a tile. */
		while (x2 - tx)
		{
			if (!square_isprojectable(c, ty, tx)) return (FALSE);

			qy += m;

			if (qy < f2)
			{
				tx += sx;
			}
			else if (qy > f2)
			{
				ty += sy;
				if (!square_isprojectable(c, ty, tx)) return (FALSE);
				qy -= f1;
				tx += sx;
			}
			else
			{
				ty += sy;
				qy -= f1;
				tx += sx;
			}
		}
	}

	/* Travel vertically */
	else
	{
		/* Let m = dx / dy * 2 * (dx * dy) = 2 * dx * dx */
		qx = ax * ax;
		m = qx << 1;

		ty = y1 + sy;

		if (qx == f2)
		{
			tx = x1 + sx;
			qx -= f1;
		}
		else
		{
			tx = x1;
		}

		/* Note (below) the case (qx == f2), where */
		/* the LOS exactly meets the corner of a tile. */
		while (y2 - ty)
		{
			if (!square_isprojectable(c, ty, tx)) return (FALSE);

			qx += m;

			if (qx < f2)
			{
				ty += sy;
			}
			else if (qx > f2)
			{
				tx += sx;
				if (!square_isprojectable(c, ty, tx)) return (FALSE);
				qx -= f1;
				ty += sy;
			}
			else
			{
				tx += sx;
				qx -= f1;
				ty += sy;
			}
		}
	}

	/* Assume los */
	return (TRUE);
}

/*
 * Returns true if the player's grid is dark
 */
bool no_light(void)
{
	return (!player_can_see_bold(player->py, player->px));
}

/*
 * Determine if a given location may be "destroyed"
 *
 * Used by destruction spells, and for placing stairs, etc.
 */
bool square_valid_bold(int y, int x)
{
	object_type *o_ptr;

	/* Forbid perma-grids */
	if (square_isperm(cave, y, x) || square_isshop(cave, y, x) || 
		square_isstairs(cave, y, x)) return (FALSE);

	/* Check objects */
	for (o_ptr = get_first_object(y, x); o_ptr; o_ptr = get_next_object(o_ptr))
	{
		/* Forbid artifact grids */
		if (o_ptr->artifact) return (FALSE);
	}

	/* Accept */
	return (TRUE);
}


/* 
 * Checks if a square is at the (inner) edge of a trap detect area 
 */ 
bool dtrap_edge(int y, int x) 
{ 
	/* Check if the square is a dtrap in the first place */ 
	if (!square_isdtrap(cave, y, x)) return FALSE;

 	/* Check for non-dtrap adjacent grids */ 
	if (square_in_bounds_fully(cave, y + 1, x) &&
		(!square_isdtrap(cave, y + 1, x)))
		return TRUE;
	if (square_in_bounds_fully(cave, y, x + 1) &&
		(!square_isdtrap(cave, y, x + 1)))
		return TRUE;
	if (square_in_bounds_fully(cave, y - 1, x) &&
		(!square_isdtrap(cave, y - 1, x)))
		return TRUE;
	if (square_in_bounds_fully(cave, y, x - 1) &&
		(!square_isdtrap(cave, y, x - 1)))
		return TRUE;

	return FALSE; 
}



/*
 * This function takes a grid location (x, y) and extracts information the
 * player is allowed to know about it, filling in the grid_data structure
 * passed in 'g'.
 *
 * The information filled in is as follows:
 *  - g->f_idx is filled in with the terrain's feature type, or FEAT_NONE
 *    if the player doesn't know anything about the grid.  The function
 *    makes use of the "mimic" field in terrain in order to allow one
 *    feature to look like another (hiding secret doors, invisible traps,
 *    etc).  This will return the terrain type the player "Knows" about,
 *    not necessarily the real terrain.
 *  - g->m_idx is set to the monster index, or 0 if there is none (or the
 *    player doesn't know it).
 *  - g->first_kind is set to the object_kind of the first object in a grid
 *    that the player knows about, or NULL for no objects.
 *  - g->muliple_objects is TRUE if there is more than one object in the
 *    grid that the player knows and cares about (to facilitate any special
 *    floor stack symbol that might be used).
 *  - g->in_view is TRUE if the player can currently see the grid - this can
 *    be used to indicate field-of-view, such as through the OPT(view_bright_light)
 *    option.
 *  - g->lighting is set to indicate the lighting level for the grid:
 *    LIGHTING_DARK for unlit grids, LIGHTING_LIT for inherently light
 *    grids (lit rooms, etc), LIGHTING_TORCH for grids lit by the player's
 *    light source, and LIGHTING_LOS for grids in the player's line of sight.
 *    Note that lighting is always LIGHTING_LIT for known "interesting" grids
 *    like walls.
 *  - g->is_player is TRUE if the player is on the given grid.
 *  - g->hallucinate is TRUE if the player is hallucinating something "strange"
 *    for this grid - this should pick a random monster to show if the m_idx
 *    is non-zero, and a random object if first_kind is non-zero.
 * 
 * NOTES:
 * This is called pretty frequently, whenever a grid on the map display
 * needs updating, so don't overcomplicate it.
 *
 * Terrain is remembered separately from objects and monsters, so can be
 * shown even when the player can't "see" it.  This leads to things like
 * doors out of the player's view still change from closed to open and so on.
 *
 * TODO:
 * Hallucination is currently disabled (it was a display-level hack before,
 * and we need it to be a knowledge-level hack).  The idea is that objects
 * may turn into different objects, monsters into different monsters, and
 * terrain may be objects, monsters, or stay the same.
 */
void map_info(unsigned y, unsigned x, grid_data *g)
{
	object_type *o_ptr;

	assert(x < (unsigned) cave->width);
	assert(y < (unsigned) cave->height);

	/* Default "clear" values, others will be set later where appropriate. */
	g->first_kind = NULL;
    g->trap = cave_trap_max(cave);
	g->multiple_objects = FALSE;
	g->lighting = LIGHTING_DARK;
	g->unseen_object = FALSE;
	g->unseen_money = FALSE;

	g->f_idx = cave->feat[y][x];
	if (f_info[g->f_idx].mimic)
		g->f_idx = f_info[g->f_idx].mimic;

	g->in_view = (square_isseen(cave, y, x)) ? TRUE : FALSE;
	g->is_player = (cave->m_idx[y][x] < 0) ? TRUE : FALSE;
	g->m_idx = (g->is_player) ? 0 : cave->m_idx[y][x];
	g->hallucinate = player->timed[TMD_IMAGE] ? TRUE : FALSE;
	g->trapborder = (square_isdedge(cave, y, x)) ? TRUE : FALSE;

	if (g->in_view)
	{
		g->lighting = LIGHTING_LOS;

		if (!square_isglow(cave, y, x) && OPT(view_yellow_light))
			g->lighting = LIGHTING_TORCH;
	}
	else if (!square_ismark(cave, y, x))
	{
		g->f_idx = FEAT_NONE;
	}
	else if (square_isglow(cave, y, x))
	{
		g->lighting = LIGHTING_LIT;
	}


    /* There is a trap in this square */
    if (square_istrap(cave, y, x) && square_ismark(cave, y, x))
    {
		int i;

		/* Scan the current trap list */
		for (i = 0; i < cave_trap_max(cave); i++)
		{
			/* Point to this trap */
			struct trap *trap = cave_trap(cave, i);

			/* Find a trap in this position */
			if ((trap->fy == y) && (trap->fx == x))
			{
				/* Get the trap */
				g->trap = i;
				break;
			}
		}
    }

	/* Objects */
	for (o_ptr = get_first_object(y, x); o_ptr; o_ptr = get_next_object(o_ptr))
	{
		if (o_ptr->marked == MARK_AWARE) {

			/* Distinguish between unseen money and objects */
			if (tval_is_money(o_ptr)) {
				g->unseen_money = TRUE;
			} else {
				g->unseen_object = TRUE;
			}

		} else if (o_ptr->marked == MARK_SEEN && !ignore_item_ok(o_ptr)) {
			if (!g->first_kind) {
				g->first_kind = o_ptr->kind;
			} else {
				g->multiple_objects = TRUE;
				break;
			}
		}
	}

	/* Monsters */
	if (g->m_idx > 0)
	{
		/* If the monster isn't "visible", make sure we don't list it.*/
		monster_type *m_ptr = cave_monster(cave, g->m_idx);
		if (!m_ptr->ml) g->m_idx = 0;
	}

	/* Rare random hallucination on non-outer walls */
	if (g->hallucinate && g->m_idx == 0 && g->first_kind == 0)
	{
		if (one_in_(128) && g->f_idx != FEAT_PERM)
			g->m_idx = 1;
		else if (one_in_(128) && g->f_idx != FEAT_PERM)
			/* if hallucinating, we just need first_kind to not be NULL */
			g->first_kind = k_info;
		else
			g->hallucinate = FALSE;
	}

	assert(g->f_idx <= FEAT_PERM);
	if (!g->hallucinate)
		assert((int)g->m_idx < cave->mon_max);
	/* All other g fields are 'flags', mostly booleans. */
}


/*
 * Memorize interesting viewable object/features in the given grid
 *
 * This function should only be called on "legal" grids.
 *
 * This function will memorize the object and/or feature in the given grid,
 * if they are (1) see-able and (2) interesting.  Note that all objects are
 * interesting, all terrain features except floors (and invisible traps) are
 * interesting, and floors (and invisible traps) are interesting sometimes
 * (depending on various options involving the illumination of floor grids).
 *
 * The automatic memorization of all objects and non-floor terrain features
 * as soon as they are displayed allows incredible amounts of optimization
 * in various places, especially "map_info()" and this function itself.
 *
 * Note that the memorization of objects is completely separate from the
 * memorization of terrain features, preventing annoying floor memorization
 * when a detected object is picked up from a dark floor, and object
 * memorization when an object is dropped into a floor grid which is
 * memorized but out-of-sight.
 *
 * This function should be called every time the "memorization" of a grid
 * (or the object in a grid) is called into question, such as when an object
 * is created in a grid, when a terrain feature "changes" from "floor" to
 * "non-floor", and when any grid becomes "see-able" for any reason.
 *
 * This function is called primarily from the "update_view()" function, for
 * each grid which becomes newly "see-able".
 */
void square_note_spot(struct chunk *c, int y, int x)
{
	object_type *o_ptr;

	/* Require "seen" flag */
	if (!square_isseen(c, y, x))
		return;

	for (o_ptr = get_first_object(y, x); o_ptr; o_ptr = get_next_object(o_ptr))
		o_ptr->marked = MARK_SEEN;

	if (square_ismark(c, y, x))
		return;

	/* Memorize this grid */
	sqinfo_on(c->info[y][x], SQUARE_MARK);
}



/*
 * Redraw (on the screen) a given map location
 *
 * This function should only be called on "legal" grids.
 */
void square_light_spot(struct chunk *c, int y, int x)
{
	event_signal_point(EVENT_MAP, x, y);
}


/*
 * This routine will Perma-Light all grids in the set passed in.
 *
 * This routine is used (only) by "light_room(..., LIGHT)"
 *
 * Dark grids are illuminated.
 *
 * Also, process all affected monsters.
 *
 * SMART monsters always wake up when illuminated
 * NORMAL monsters wake up 1/4 the time when illuminated
 * STUPID monsters wake up 1/10 the time when illuminated
 */
static void cave_light(struct point_set *ps)
{
	int i;

	/* Apply flag changes */
	for (i = 0; i < ps->n; i++)
	{
		int y = ps->pts[i].y;
		int x = ps->pts[i].x;

		/* Perma-Light */
		sqinfo_on(cave->info[y][x], SQUARE_GLOW);
	}

	/* Fully update the visuals */
	player->upkeep->update |= (PU_FORGET_VIEW | PU_UPDATE_VIEW | PU_MONSTERS);

	/* Update stuff */
	update_stuff(player->upkeep);

	/* Process the grids */
	for (i = 0; i < ps->n; i++)
	{
		int y = ps->pts[i].y;
		int x = ps->pts[i].x;

		/* Redraw the grid */
		square_light_spot(cave, y, x);

		/* Process affected monsters */
		if (cave->m_idx[y][x] > 0)
		{
			int chance = 25;

			monster_type *m_ptr = square_monster(cave, y, x);

			/* Stupid monsters rarely wake up */
			if (rf_has(m_ptr->race->flags, RF_STUPID)) chance = 10;

			/* Smart monsters always wake up */
			if (rf_has(m_ptr->race->flags, RF_SMART)) chance = 100;

			/* Sometimes monsters wake up */
			if (m_ptr->m_timed[MON_TMD_SLEEP] && (randint0(100) < chance))
			{
				/* Wake up! */
				mon_clear_timed(m_ptr, MON_TMD_SLEEP,
					MON_TMD_FLG_NOTIFY, FALSE);

			}
		}
	}
}



/*
 * This routine will "darken" all grids in the set passed in.
 *
 * In addition, some of these grids will be "unmarked".
 *
 * This routine is used (only) by "light_room(..., UNLIGHT)"
 */
static void cave_unlight(struct point_set *ps)
{
	int i;

	/* Apply flag changes */
	for (i = 0; i < ps->n; i++)
	{
		int y = ps->pts[i].y;
		int x = ps->pts[i].x;

		/* Darken the grid */
		sqinfo_off(cave->info[y][x], SQUARE_GLOW);

		/* Hack -- Forget "boring" grids */
		if (!square_isinteresting(cave, y, x))
			sqinfo_off(cave->info[y][x], SQUARE_MARK);
	}

	/* Fully update the visuals */
	player->upkeep->update |= (PU_FORGET_VIEW | PU_UPDATE_VIEW | PU_MONSTERS);

	/* Update stuff */
	update_stuff(player->upkeep);

	/* Process the grids */
	for (i = 0; i < ps->n; i++)
	{
		int y = ps->pts[i].y;
		int x = ps->pts[i].x;

		/* Redraw the grid */
		square_light_spot(cave, y, x);
	}
}

/*
 * Aux function -- see below
 */
static void cave_room_aux(struct point_set *seen, int y, int x)
{
	if (point_set_contains(seen, y, x))
		return;

	if (!square_isroom(cave, y, x))
		return;

	/* Add it to the "seen" set */
	add_to_point_set(seen, y, x);
}

/*
 * Illuminate or darken any room containing the given location.
 */
void light_room(int y1, int x1, bool light)
{
	int i, x, y;
	struct point_set *ps;

	ps = point_set_new(200);
	/* Add the initial grid */
	cave_room_aux(ps, y1, x1);

	/* While grids are in the queue, add their neighbors */
	for (i = 0; i < ps->n; i++)
	{
		x = ps->pts[i].x, y = ps->pts[i].y;

		/* Walls get lit, but stop light */
		if (!square_isprojectable(cave, y, x)) continue;

		/* Spread adjacent */
		cave_room_aux(ps, y + 1, x);
		cave_room_aux(ps, y - 1, x);
		cave_room_aux(ps, y, x + 1);
		cave_room_aux(ps, y, x - 1);

		/* Spread diagonal */
		cave_room_aux(ps, y + 1, x + 1);
		cave_room_aux(ps, y - 1, x - 1);
		cave_room_aux(ps, y - 1, x + 1);
		cave_room_aux(ps, y + 1, x - 1);
	}

	/* Now, lighten or darken them all at once */
	if (light) {
		cave_light(ps);
	} else {
		cave_unlight(ps);
	}
	point_set_dispose(ps);
}


/*
 * Some comments on the dungeon related data structures and functions...
 *
 * Angband is primarily a dungeon exploration game, and it should come as
 * no surprise that the internal representation of the dungeon has evolved
 * over time in much the same way as the game itself, to provide semantic
 * changes to the game itself, to make the code simpler to understand, and
 * to make the executable itself faster or more efficient in various ways.
 *
 * There are a variety of dungeon related data structures, and associated
 * functions, which store information about the dungeon, and provide methods
 * by which this information can be accessed or modified.
 *
 * Some of this information applies to the dungeon as a whole, such as the
 * list of unique monsters which are still alive.  Some of this information
 * only applies to the current dungeon level, such as the current depth, or
 * the list of monsters currently inhabiting the level.  And some of the
 * information only applies to a single grid of the current dungeon level,
 * such as whether the grid is illuminated, or whether the grid contains a
 * monster, or whether the grid can be seen by the player.  If Angband was
 * to be turned into a multi-player game, some of the information currently
 * associated with the dungeon should really be associated with the player,
 * such as whether a given grid is viewable by a given player.
 *
 * Currently, a lot of the information about the dungeon is stored in ways
 * that make it very efficient to access or modify the information, while
 * still attempting to be relatively conservative about memory usage, even
 * if this means that some information is stored in multiple places, or in
 * ways which require the use of special code idioms.  For example, each
 * monster record in the monster array contains the location of the monster,
 * and each cave grid has an index into the monster array, or a zero if no
 * monster is in the grid.  This allows the monster code to efficiently see
 * where the monster is located, while allowing the dungeon code to quickly
 * determine not only if a monster is present in a given grid, but also to
 * find out which monster.  The extra space used to store the information
 * twice is inconsequential compared to the speed increase.
 *
 * Several pieces of information about each cave grid are stored in the
 * "cave->info" array, which is a special two dimensional array of bitflags.
 *
 * The "SQUARE_ROOM" flag is used to determine which grids are part of "rooms", 
 * and thus which grids are affected by "illumination" spells.
 *
 * The "SQUARE_VAULT" flag is used to determine which grids are part of 
 * "vaults", and thus which grids cannot serve as the destinations of player 
 * teleportation.
 *
 * The "SQUARE_MARK" flag is used to determine which grids have been memorized 
 * by the player.  This flag is used by the "map_info()" function to determine
 * if a grid should be displayed. This flag is used in a few other places to 
 * determine if the player can * "know" about a given grid.
 *
 * The "SQUARE_GLOW" flag is used to determine which grids are "permanently 
 * illuminated".  This flag is used by the update_view() function to help 
 * determine which viewable flags may be "seen" by the player.  This flag 
 * is used by the "map_info" function to determine if a grid is only lit by 
 * the player's torch.  This flag has special semantics for wall grids 
 * (see "update_view()").
 *
 * The "SQUARE_VIEW" flag is used to determine which grids are currently in
 * line of sight of the player.  This flag is set by (and used by) the
 * "update_view()" function.  This flag is used by any code which needs to
 * know if the player can "view" a given grid.  This flag is used by the
 * "map_info()" function for some optional special lighting effects.  The
 * "player_has_los_bold()" macro wraps an abstraction around this flag, but
 * certain code idioms are much more efficient.  This flag is used to check
 * if a modification to a terrain feature might affect the player's field of
 * view.  This flag is used to see if certain monsters are "visible" to the
 * player.  This flag is used to allow any monster in the player's field of
 * view to "sense" the presence of the player.
 *
 * The "SQUARE_SEEN" flag is used to determine which grids are currently in
 * line of sight of the player and also illuminated in some way.  This flag
 * is set by the "update_view()" function, using computations based on the
 * "SQUARE_VIEW" and "SQUARE_GLOW" flags and terrain of various grids.  
 * This flag is used by any code which needs to know if the player can "see" a
 * given grid.  This flag is used by the "map_info()" function both to see
 * if a given "boring" grid can be seen by the player, and for some optional
 * special lighting effects.  The "player_can_see_bold()" macro wraps an
 * abstraction around this flag, but certain code idioms are much more
 * efficient.  This flag is used to see if certain monsters are "visible" to
 * the player.  This flag is never set for a grid unless "SQUARE_VIEW" is also
 * set for the grid.  Whenever the terrain or "SQUARE_GLOW" flag changes
 * for a grid which has the "SQUARE_VIEW" flag set, the "SQUARE_SEEN" flag must
 * be recalculated.  The simplest way to do this is to call "forget_view()"
 * and "update_view()" whenever the terrain or "SQUARE_GLOW" flag changes
 * for a grid which has "SQUARE_VIEW" set.
 *
 * The "SQUARE_WASSEEN" flag is used for a variety of temporary purposes.  This
 * flag is used to determine if the "SQUARE_SEEN" flag for a grid has changed
 * during the "update_view()" function.  This flag is used to "spread" light
 * or darkness through a room.  This flag is used by the "monster flow code".
 * This flag must always be cleared by any code which sets it.
 *
 * Note that the "SQUARE_MARK" flag is used for many reasons, some of which
 * are strictly for optimization purposes.  The "SQUARE_MARK" flag means that
 * even if the player cannot "see" the grid, he "knows" about the terrain in
 * that grid.  This is used to "memorize" grids when they are first "seen" by
 * the player, and to allow certain grids to be "detected" by certain magic.
 *
 * Objects are "memorized" in a different way, using a special "marked" flag
 * on the object itself, which is set when an object is observed or detected.
 * This allows objects to be "memorized" independant of the terrain features.
 *
 * The "update_view()" function is an extremely important function.  It is
 * called only when the player moves, significant terrain changes, or the
 * player's blindness or torch radius changes.  Note that when the player
 * is resting, or performing any repeated actions (like digging, disarming,
 * farming, etc), there is no need to call the "update_view()" function, so
 * even if it was not very efficient, this would really only matter when the
 * player was "running" through the dungeon.  It sets the "SQUARE_VIEW" flag
 * on every cave grid in the player's field of view.  It also checks the torch
 * radius of the player, and sets the "SQUARE_SEEN" flag for every grid which
 * is in the "field of view" of the player and which is also "illuminated",
 * either by the players torch (if any) or by any permanent light source.
 * It could use and help maintain information about multiple light sources,
 * which would be helpful in a multi-player version of Angband.
 *
 * Note that the "update_view()" function allows, among other things, a room
 * to be "partially" seen as the player approaches it, with a growing cone
 * of floor appearing as the player gets closer to the door.  Also, by not
 * turning on the "memorize perma-lit grids" option, the player will only
 * "see" those floor grids which are actually in line of sight.  And best
 * of all, you can now activate the special lighting effects to indicate
 * which grids are actually in the player's field of view by using dimmer
 * colors for grids which are not in the player's field of view, and/or to
 * indicate which grids are illuminated only by the player's torch by using
 * the color yellow for those grids.
 *
 * It seems as though slight modifications to the "update_view()" functions
 * would allow us to determine "reverse" line-of-sight as well as "normal"
 * line-of-sight", which would allow monsters to have a more "correct" way
 * to determine if they can "see" the player, since right now, they "cheat"
 * somewhat and assume that if the player has "line of sight" to them, then
 * they can "pretend" that they have "line of sight" to the player.  But if
 * such a change was attempted, the monsters would actually start to exhibit
 * some undesirable behavior, such as "freezing" near the entrances to long
 * hallways containing the player, and code would have to be added to make
 * the monsters move around even if the player was not detectable, and to
 * "remember" where the player was last seen, to avoid looking stupid.
 *
 * Note that the "SQUARE_GLOW" flag means that a grid is permanently lit in
 * some way.  However, for the player to "see" the grid, as determined by
 * the "SQUARE_SEEN" flag, the player must not be blind, the grid must have
 * the "SQUARE_VIEW" flag set, and if the grid is a "wall" grid, and it is
 * not lit by the player's torch, then it must touch a projectable grid 
 * which has both the "SQUARE_GLOW"
 * and "SQUARE_VIEW" flags set.  This last part about wall grids is induced
 * by the semantics of "SQUARE_GLOW" as applied to wall grids, and checking
 * the technical requirements can be very expensive, especially since the
 * grid may be touching some "illegal" grids.  Luckily, it is more or less
 * correct to restrict the "touching" grids from the eight "possible" grids
 * to the (at most) three grids which are touching the grid, and which are
 * closer to the player than the grid itself, which eliminates more than
 * half of the work, including all of the potentially "illegal" grids, if
 * at most one of the three grids is a "diagonal" grid.  In addition, in
 * almost every situation, it is possible to ignore the "SQUARE_VIEW" flag
 * on these three "touching" grids, for a variety of technical reasons.
 * Finally, note that in most situations, it is only necessary to check
 * a single "touching" grid, in fact, the grid which is strictly closest
 * to the player of all the touching grids, and in fact, it is normally
 * only necessary to check the "SQUARE_GLOW" flag of that grid, again, for
 * various technical reasons.  However, one of the situations which does
 * not work with this last reduction is the very common one in which the
 * player approaches an illuminated room from a dark hallway, in which the
 * two wall grids which form the "entrance" to the room would not be marked
 * as "SQUARE_SEEN", since of the three "touching" grids nearer to the player
 * than each wall grid, only the farthest of these grids is itself marked
 * "SQUARE_GLOW".
 *
 *
 * Here are some pictures of the legal "light source" radius values, in
 * which the numbers indicate the "order" in which the grids could have
 * been calculated, if desired.  Note that the code will work with larger
 * radiuses, though currently yields such a radius, and the game would
 * become slower in some situations if it did.
 *
 *       Rad=0     Rad=1      Rad=2        Rad=3
 *      No-Light Torch,etc   Lantern     Artifacts
 *
 *                                          333
 *                             333         43334
 *                  212       32123       3321233
 *         @        1@1       31@13       331@133
 *                  212       32123       3321233
 *                             333         43334
 *                                          333
 *
 */

/*
 * Forget the "SQUARE_VIEW" grids, redrawing as needed
 */
void forget_view(struct chunk *c)
{
	int x, y;

	for (y = 0; y < c->height; y++) {
		for (x = 0; x < c->width; x++) {
			if (!square_isview(c, y, x))
				continue;
			sqinfo_off(c->info[y][x], SQUARE_VIEW);
			sqinfo_off(c->info[y][x], SQUARE_SEEN);
			square_light_spot(c, y, x);
		}
	}
}



/*
 * Calculate the complete field of view using a new algorithm
 */
static void mark_wasseen(struct chunk *c) 
{
	int x, y;
	/* Save the old "view" grids for later */
	for (y = 0; y < c->height; y++) {
		for (x = 0; x < c->width; x++) {
			if (square_isseen(c, y, x))
				sqinfo_on(c->info[y][x], SQUARE_WASSEEN);
			sqinfo_off(c->info[y][x], SQUARE_VIEW);
			sqinfo_off(c->info[y][x], SQUARE_SEEN);
		}
	}
}

static void add_monster_lights(struct chunk *c, struct loc from)
{
	int i, j, k;

	/* Scan monster list and add monster lights */
	for (k = 1; k < z_info->m_max; k++) {
		/* Check the k'th monster */
		struct monster *m = cave_monster(c, k);

		bool in_los = los(c, from.y, from.x, m->fy, m->fx);

		/* Skip dead monsters */
		if (!m->race)
			continue;

		/* Skip monsters not carrying light */
		if (!rf_has(m->race->flags, RF_HAS_LIGHT))
			continue;

		/* Light a 3x3 box centered on the monster */
		for (i = -1; i <= 1; i++)
		{
			for (j = -1; j <= 1; j++)
			{
				int sy = m->fy + i;
				int sx = m->fx + j;
				
				/* If the monster isn't visible we can only light open tiles */
				if (!in_los && !square_isprojectable(c, sy, sx))
					continue;

				/* If the tile is too far away we won't light it */
				if (distance(from.y, from.x, sy, sx) > MAX_SIGHT)
					continue;
				
				/* If the tile itself isn't in LOS, don't light it */
				if (!los(c, from.y, from.x, sy, sx))
					continue;

				/* Mark the square lit and seen */
				sqinfo_on(c->info[sy][sx], SQUARE_VIEW);
				sqinfo_on(c->info[sy][sx], SQUARE_SEEN);
			}
		}
	}
}

static void update_one(struct chunk *c, int y, int x, int blind)
{
	if (blind)
		sqinfo_off(c->info[y][x], SQUARE_SEEN);

	/* Square went from unseen -> seen */
	if (square_isseen(c, y, x) && !square_wasseen(c, y, x)) {
		if (square_isfeel(c, y, x)) {
			c->feeling_squares++;
			sqinfo_off(c->info[y][x], SQUARE_FEEL);
			if (c->feeling_squares == FEELING1)
				display_feeling(TRUE);
		}

		square_note_spot(c, y, x);
		square_light_spot(c, y, x);
	}

	/* Square went from seen -> unseen */
	if (!square_isseen(c, y, x) && square_wasseen(c, y, x))
		square_light_spot(c, y, x);

	sqinfo_off(c->info[y][x], SQUARE_WASSEEN);
}

static void become_viewable(struct chunk *c, int y, int x, int lit, int py, int px)
{
	int xc = x;
	int yc = y;
	if (square_isview(c, y, x))
		return;

	sqinfo_on(c->info[y][x], SQUARE_VIEW);

	if (lit)
		sqinfo_on(c->info[y][x], SQUARE_SEEN);

	if (square_isglow(c, y, x)) {
		if (square_iswall(c, y, x)) {
			/* For walls, move a bit towards the player.
			 * TODO(elly): huh? why?
			 */
			xc = (x < px) ? (x + 1) : (x > px) ? (x - 1) : x;
			yc = (y < py) ? (y + 1) : (y > py) ? (y - 1) : y;
		}
		if (square_isglow(c, yc, xc))
			sqinfo_on(c->info[y][x], SQUARE_SEEN);
	}
}

static void update_view_one(struct chunk *c, int y, int x, int radius, int py, int px)
{
	int xc = x;
	int yc = y;

	int d = distance(y, x, py, px);
	int lit = d < radius;

	if (d > MAX_SIGHT)
		return;

	/* Special case for wall lighting. If we are a wall and the square in
	 * the direction of the player is in LOS, we are in LOS. This avoids
	 * situations like:
	 * #1#############
	 * #............@#
	 * ###############
	 * where the wall cell marked '1' would not be lit because the LOS
	 * algorithm runs into the adjacent wall cell.
	 */
	if (square_iswall(c, y, x)) {
		int dx = x - px;
		int dy = y - py;
		int ax = ABS(dx);
		int ay = ABS(dy);
		int sx = dx > 0 ? 1 : -1;
		int sy = dy > 0 ? 1 : -1;

		xc = (x < px) ? (x + 1) : (x > px) ? (x - 1) : x;
		yc = (y < py) ? (y + 1) : (y > py) ? (y - 1) : y;

		/* Check that the cell we're trying to steal LOS from isn't a
		 * wall. If we don't do this, double-thickness walls will have
		 * both sides visible.
		 */
		if (square_iswall(c, yc, xc)) {
			xc = x;
			yc = y;
		}

		/* Check that we got here via the 'knight's move' rule. If so,
		 * don't steal LOS. */
		if (ax == 2 && ay == 1) {
			if (  !square_iswall(c, y, x - sx)
				  && square_iswall(c, y - sy, x - sx)) {
				xc = x;
				yc = y;
			}
		} else if (ax == 1 && ay == 2) {
			if (  !square_iswall(c, y - sy, x)
				  && square_iswall(c, y - sy, x - sx)) {
				xc = x;
				yc = y;
			}
		}
	}


	if (los(c, py, px, yc, xc))
		become_viewable(c, y, x, lit, py, px);
}

void update_view(struct chunk *c, struct player *p)
{
	int x, y;

	int radius;

	mark_wasseen(c);

	/* Extract "radius" value */
	radius = p->state.cur_light;

	/* Handle real light */
	if (radius > 0) ++radius;

	add_monster_lights(c, loc(p->px, p->py));

	/* Assume we can view the player grid */
	sqinfo_on(c->info[p->py][p->px], SQUARE_VIEW);
	if (radius > 0 || square_isglow(c, p->py, p->px))
		sqinfo_on(c->info[p->py][p->px], SQUARE_SEEN);

	/* View squares we have LOS to */
	for (y = 0; y < c->height; y++)
		for (x = 0; x < c->width; x++)
			update_view_one(c, y, x, radius, p->py, p->px);

	/*** Step 3 -- Complete the algorithm ***/

	for (y = 0; y < c->height; y++)
		for (x = 0; x < c->width; x++)
			update_one(c, y, x, p->timed[TMD_BLIND]);
}


/*
 * Determine if a "legal" grid is within "los" of the player
 */
bool player_has_los_bold(int y, int x)
{
	if (sqinfo_has(cave->info[y][x], SQUARE_VIEW))
		return TRUE;

	return FALSE;
}

/*
 * Determine if a "legal" grid can be "seen" by the player
 */
bool player_can_see_bold(int y, int x)
{
	if (sqinfo_has(cave->info[y][x], SQUARE_SEEN))
		return TRUE;

	return FALSE;
}

/*
 * Size of the circular queue used by "update_flow()"
 */
#define FLOW_MAX 2048

/*
 * Hack -- provide some "speed" for the "flow" code
 * This entry is the "current index" for the "when" field
 * Note that a "when" value of "zero" means "not used".
 *
 * Note that the "cost" indexes from 1 to 127 are for
 * "old" data, and from 128 to 255 are for "new" data.
 *
 * This means that as long as the player does not "teleport",
 * then any monster up to 128 + MONSTER_FLOW_DEPTH will be
 * able to track down the player, and in general, will be
 * able to track down either the player or a position recently
 * occupied by the player.
 */
static int flow_save = 0;



/*
 * Hack -- forget the "flow" information
 */
void cave_forget_flow(struct chunk *c)
{
	int x, y;

	/* Nothing to forget */
	if (!flow_save) return;

	/* Check the entire dungeon */
	for (y = 0; y < c->height; y++)
	{
		for (x = 0; x < c->width; x++)
		{
			/* Forget the old data */
			c->cost[y][x] = 0;
			c->when[y][x] = 0;
		}
	}

	/* Start over */
	flow_save = 0;
}


/*
 * Hack -- fill in the "cost" field of every grid that the player can
 * "reach" with the number of steps needed to reach that grid.  This
 * also yields the "distance" of the player from every grid.
 *
 * In addition, mark the "when" of the grids that can reach the player
 * with the incremented value of "flow_save".
 *
 * Hack -- use the local "flow_y" and "flow_x" arrays as a "circular
 * queue" of cave grids.
 *
 * We do not need a priority queue because the cost from grid to grid
 * is always "one" (even along diagonals) and we process them in order.
 */
void cave_update_flow(struct chunk *c)
{
	int py = player->py;
	int px = player->px;

	int ty, tx;

	int y, x;

	int n, d;

	int flow_n;

	int flow_tail = 0;
	int flow_head = 0;

	byte flow_y[FLOW_MAX];
	byte flow_x[FLOW_MAX];


	/*** Cycle the flow ***/

	/* Cycle the flow */
	if (flow_save++ == 255)
	{
		/* Cycle the flow */
		for (y = 0; y < c->height; y++)
		{
			for (x = 0; x < c->width; x++)
			{
				int w = c->when[y][x];
				c->when[y][x] = (w >= 128) ? (w - 128) : 0;
			}
		}

		/* Restart */
		flow_save = 128;
	}

	/* Local variable */
	flow_n = flow_save;


	/*** Player Grid ***/

	/* Save the time-stamp */
	c->when[py][px] = flow_n;

	/* Save the flow cost */
	c->cost[py][px] = 0;

	/* Enqueue that entry */
	flow_y[flow_head] = py;
	flow_x[flow_head] = px;

	/* Advance the queue */
	++flow_tail;


	/*** Process Queue ***/

	/* Now process the queue */
	while (flow_head != flow_tail)
	{
		/* Extract the next entry */
		ty = flow_y[flow_head];
		tx = flow_x[flow_head];

		/* Forget that entry (with wrap) */
		if (++flow_head == FLOW_MAX) flow_head = 0;

		/* Child cost */
		n = c->cost[ty][tx] + 1;

		/* Hack -- Limit flow depth */
		if (n == MONSTER_FLOW_DEPTH) continue;

		/* Add the "children" */
		for (d = 0; d < 8; d++)
		{
			int old_head = flow_tail;

			/* Child location */
			y = ty + ddy_ddd[d];
			x = tx + ddx_ddd[d];
			if (!square_in_bounds(c, y, x)) continue;

			/* Ignore "pre-stamped" entries */
			if (c->when[y][x] == flow_n) continue;

			/* Ignore "walls" and "rubble" */
			if (tf_has(f_info[c->feat[y][x]].flags, TF_NO_FLOW)) continue;

			/* Save the time-stamp */
			c->when[y][x] = flow_n;

			/* Save the flow cost */
			c->cost[y][x] = n;

			/* Enqueue that entry */
			flow_y[flow_tail] = y;
			flow_x[flow_tail] = x;

			/* Advance the queue */
			if (++flow_tail == FLOW_MAX) flow_tail = 0;

			/* Hack -- Overflow by forgetting new entry */
			if (flow_tail == flow_head) flow_tail = old_head;
		}
	}
}




/*
 * Light up the dungeon using "claravoyance"
 *
 * This function "illuminates" every grid in the dungeon, memorizes all
 * "objects", and memorizes all grids as with magic mapping.
 */
void wiz_light(struct chunk *c, bool full)
{
	int i, y, x;

	/* Memorize objects */
	for (i = 1; i < cave_object_max(cave); i++)
	{
		object_type *o_ptr = cave_object(c, i);

		/* Skip dead objects */
		if (!o_ptr->kind) continue;

		/* Skip held objects */
		if (o_ptr->held_m_idx) continue;

		/* Memorize it */
		if (o_ptr->marked < MARK_SEEN)
			o_ptr->marked = full ? MARK_SEEN : MARK_AWARE;
	}

	/* Scan all normal grids */
	for (y = 1; y < c->height - 1; y++)
	{
		/* Scan all normal grids */
		for (x = 1; x < c->width - 1; x++)
		{
			/* Process all non-walls */
			if (!square_seemslikewall(c, y, x))
			{
				/* Scan all neighbors */
				for (i = 0; i < 9; i++)
				{
					int yy = y + ddy_ddd[i];
					int xx = x + ddx_ddd[i];

					/* Perma-light the grid */
					sqinfo_on(c->info[yy][xx], SQUARE_GLOW);

					/* Memorize normal features */
					if (!square_isfloor(c, yy, xx) || 
						square_visible_trap(c, yy, xx))
						sqinfo_on(c->info[yy][xx], SQUARE_MARK);
				}
			}
		}
	}

	/* Fully update the visuals */
	player->upkeep->update |= (PU_FORGET_VIEW | PU_UPDATE_VIEW | PU_MONSTERS);

	/* Redraw whole map, monster list */
	player->upkeep->redraw |= (PR_MAP | PR_MONLIST | PR_ITEMLIST);
}


/*
 * Forget the dungeon map (ala "Thinking of Maud...").
 */
void wiz_dark(void)
{
	int i, y, x;


	/* Forget every grid */
	for (y = 0; y < cave->height; y++)
	{
		for (x = 0; x < cave->width; x++)
		{
			/* Process the grid */
			sqinfo_off(cave->info[y][x], SQUARE_MARK);
			sqinfo_off(cave->info[y][x], SQUARE_DTRAP);
			sqinfo_off(cave->info[y][x], SQUARE_DEDGE);
		}
	}

	/* Forget all objects */
	for (i = 1; i < cave_object_max(cave); i++)
	{
		object_type *o_ptr = cave_object(cave, i);

		/* Skip dead objects */
		if (!o_ptr->kind) continue;

		/* Skip held objects */
		if (o_ptr->held_m_idx) continue;

		/* Forget the object */
		o_ptr->marked = MARK_UNAWARE;
	}

	/* Fully update the visuals */
	player->upkeep->update |= (PU_FORGET_VIEW | PU_UPDATE_VIEW | PU_MONSTERS);

	/* Redraw map, monster list */
	player->upkeep->redraw |= (PR_MAP | PR_MONLIST | PR_ITEMLIST);
}



/*
 * Light or Darken the town
 */
void cave_illuminate(struct chunk *c, bool daytime)
{
	int y, x, i;

	/* Apply light or darkness */
	for (y = 0; y < c->height; y++)
		for (x = 0; x < c->width; x++) {
			feature_type *f_ptr = &f_info[c->feat[y][x]];
			
			/* Only interesting grids at night */
			if (daytime || !tf_has(f_ptr->flags, TF_FLOOR)) {
				sqinfo_on(c->info[y][x], SQUARE_GLOW);
				sqinfo_on(c->info[y][x], SQUARE_MARK);
			}
			else {
				sqinfo_off(c->info[y][x], SQUARE_GLOW);
				sqinfo_off(c->info[y][x], SQUARE_MARK);
			}
		}
			
			
	/* Light shop doorways */
	for (y = 0; y < c->height; y++) {
		for (x = 0; x < c->width; x++) {
			if (!square_isshop(c, y, x))
				continue;
			for (i = 0; i < 8; i++) {
				int yy = y + ddy_ddd[i];
				int xx = x + ddx_ddd[i];
				sqinfo_on(c->info[yy][xx], SQUARE_GLOW);
				sqinfo_on(c->info[yy][xx], SQUARE_MARK);
			}
		}
	}


	/* Fully update the visuals */
	player->upkeep->update |= (PU_FORGET_VIEW | PU_UPDATE_VIEW | PU_MONSTERS);

	/* Redraw map, monster list */
	player->upkeep->redraw |= (PR_MAP | PR_MONLIST | PR_ITEMLIST);
}

struct feature *square_feat(struct chunk *c, int y, int x)
{
	assert(c);
	assert(y >= 0 && y < c->height);
	assert(x >= 0 && x < c->width);

	return &f_info[c->feat[y][x]];
}

void square_set_feat(struct chunk *c, int y, int x, int feat)
{
	int current_feat = c->feat[y][x];

	assert(c);
	assert(y >= 0 && y < c->height);
	assert(x >= 0 && x < c->width);

	/* Track changes */
	if (current_feat) c->feat_count[current_feat]--;
	if (feat) c->feat_count[feat]++;

	/* Make the change */
	c->feat[y][x] = feat;

	/* Make the new terrain feel at home */
	if (character_dungeon) {
		square_note_spot(c, y, x);
		square_light_spot(c, y, x);
	} else {
		/* Make sure no incorrect wall flags set for dungeon generation */
		   sqinfo_off(c->info[y][x], SQUARE_WALL_INNER);
		   sqinfo_off(c->info[y][x], SQUARE_WALL_OUTER);
		   sqinfo_off(c->info[y][x], SQUARE_WALL_SOLID);
	}
}

bool square_in_bounds(struct chunk *c, int y, int x)
{
	return x >= 0 && x < c->width && y >= 0 && y < c->height;
}

bool square_in_bounds_fully(struct chunk *c, int y, int x)
{
	return x > 0 && x < c->width - 1 && y > 0 && y < c->height - 1;
}


/*
 * Standard "find me a location" function
 *
 * Obtains a legal location within the given distance of the initial
 * location, and with "los()" from the source to destination location.
 *
 * This function is often called from inside a loop which searches for
 * locations while increasing the "d" distance.
 *
 * need_los determines whether line of sight is needed
 */
void scatter(struct chunk *c, int *yp, int *xp, int y, int x, int d, bool need_los)
{
	int nx, ny;


	/* Pick a location */
	while (TRUE)
	{
		/* Pick a new location */
		ny = rand_spread(y, d);
		nx = rand_spread(x, d);

		/* Ignore annoying locations */
		if (!square_in_bounds_fully(c, ny, nx)) continue;

		/* Ignore "excessively distant" locations */
		if ((d > 1) && (distance(y, x, ny, nx) > d)) continue;
		
		/* Don't need los */
		if (!need_los) break;

		/* Require "line of sight" if set */
		if (need_los && (los(c, y, x, ny, nx))) break;
	}

	/* Save the location */
	(*yp) = ny;
	(*xp) = nx;
}


struct chunk *cave = NULL;

struct chunk *cave_new(int height, int width) {
	int y, x;

	struct chunk *c = mem_zalloc(sizeof *c);
	c->height = height;
	c->width = width;
	c->feat_count = mem_zalloc((z_info->f_max + 1) * sizeof(int));
	c->info = mem_zalloc(c->height * sizeof(bitflag**));
	c->feat = mem_zalloc(c->height * sizeof(byte*));
	c->cost = mem_zalloc(c->height * sizeof(byte*));
	c->when = mem_zalloc(c->height * sizeof(byte*));
	c->m_idx = mem_zalloc(c->height * sizeof(s16b*));
	c->o_idx = mem_zalloc(c->height * sizeof(s16b*));
	for (y = 0; y < c->height; y++){
		c->info[y] = mem_zalloc(c->width * sizeof(bitflag*));
		for (x = 0; x < c->width; x++)
			c->info[y][x] = mem_zalloc(SQUARE_SIZE * sizeof(bitflag));
		c->feat[y] = mem_zalloc(c->width * sizeof(byte));
		c->cost[y] = mem_zalloc(c->width * sizeof(byte));
		c->when[y] = mem_zalloc(c->width * sizeof(byte));
		c->m_idx[y] = mem_zalloc(c->width * sizeof(s16b));
		c->o_idx[y] = mem_zalloc(c->width * sizeof(s16b));
	}

	c->monsters = mem_zalloc(z_info->m_max * sizeof(struct monster));
	c->mon_max = 1;
	c->mon_current = -1;

	c->objects = mem_zalloc(z_info->o_max * sizeof(struct object));
	c->obj_max = 1;

	c->traps = mem_zalloc(z_info->l_max * sizeof(struct trap));
	c->trap_max = 1;

	c->created_at = turn;
	return c;
}

void cave_free(struct chunk *c) {
	int y, x;
	for (y = 0; y < c->height; y++){
		for (x = 0; x < c->width; x++)
			mem_free(c->info[y][x]);
		mem_free(c->info[y]);
		mem_free(c->feat[y]);
		mem_free(c->cost[y]);
		mem_free(c->when[y]);
		mem_free(c->m_idx[y]);
		mem_free(c->o_idx[y]);
	}
	mem_free(c->feat_count);
	mem_free(c->info);
	mem_free(c->feat);
	mem_free(c->cost);
	mem_free(c->when);
	mem_free(c->m_idx);
	mem_free(c->o_idx);
	mem_free(c->monsters);
	mem_free(c->objects);
	mem_free(c->traps);
	mem_free(c);
}

/**
 * FEATURE PREDICATES
 *
 * These functions are used to figure out what kind of square something is,
 * via c->feat[y][x]. All direct testing of c->feat[y][x] should be rewritten
 * in terms of these functions.
 *
 * It's often better to use feature behavior predicates (written in terms of
 * these functions) instead of these functions directly. For instance,
 * square_isrock() will return false for a secret door, even though it will
 * behave like a rock wall until the player determines it's a door.
 *
 * Use functions like square_isdiggable, square_iswall, etc. in these cases.
 */

/**
 * True if the square is normal open floor.
 */
bool square_isfloor(struct chunk *c, int y, int x) {
	return tf_has(f_info[c->feat[y][x]].flags, TF_FLOOR);
}

/**
 * True if the square is a normal granite rock wall.
 */
bool square_isrock(struct chunk *c, int y, int x) {
	return (tf_has(f_info[c->feat[y][x]].flags, TF_GRANITE) &&
			!tf_has(f_info[c->feat[y][x]].flags, TF_DOOR_ANY));
}

/**
 * True if the square is a permanent wall.
 */
bool square_isperm(struct chunk *c, int y, int x) {
	return (tf_has(f_info[c->feat[y][x]].flags, TF_PERMANENT) &&
			tf_has(f_info[c->feat[y][x]].flags, TF_ROCK));
}

/**
 * True if the square is a magma wall.
 */
bool feat_is_magma(int feat)
{
	return tf_has(f_info[feat].flags, TF_MAGMA);
}

/**
 * True if the square is a magma wall.
 */
bool square_ismagma(struct chunk *c, int y, int x) {
	return feat_is_magma(c->feat[y][x]);
}

/**
 * True if the square is a quartz wall.
 */
bool feat_is_quartz(int feat)
{
	return tf_has(f_info[feat].flags, TF_QUARTZ);
}

/**
 * True if the square is a quartz wall.
 */
bool square_isquartz(struct chunk *c, int y, int x) {
	return feat_is_quartz(c->feat[y][x]);
}

/**
 * True if the square is a mineral wall (magma/quartz).
 */
bool square_ismineral(struct chunk *c, int y, int x) {
	return square_isrock(c, y, x) || square_ismagma(c, y, x) || square_isquartz(c, y, x);
}

/**
 * True if the square is a mineral wall with treasure (magma/quartz).
 */
bool feat_is_treasure(int feat) {
	return (tf_has(f_info[feat].flags, TF_GOLD) &&
			tf_has(f_info[feat].flags, TF_INTERESTING));
}

/**
 * True if the square is rubble.
 */
bool square_isrubble(struct chunk *c, int y, int x) {
    return (!tf_has(f_info[c->feat[y][x]].flags, TF_WALL) &&
			tf_has(f_info[c->feat[y][x]].flags, TF_ROCK));
}

/**
 * True if the square is a hidden secret door.
 *
 * These squares appear as if they were granite--when detected a secret door
 * is replaced by a closed door.
 */
bool square_issecretdoor(struct chunk *c, int y, int x) {
    return (tf_has(f_info[c->feat[y][x]].flags, TF_DOOR_ANY) &&
			tf_has(f_info[c->feat[y][x]].flags, TF_ROCK));
}

/**
 * True if the square is an open door.
 */
bool square_isopendoor(struct chunk *c, int y, int x) {
    return (tf_has(f_info[c->feat[y][x]].flags, TF_CLOSABLE));
}

/**
 * True if the square is a closed door (possibly locked or jammed).
 */
bool square_iscloseddoor(struct chunk *c, int y, int x) {
	int feat = c->feat[y][x];
	return tf_has(f_info[feat].flags, TF_DOOR_CLOSED);
}

/**
 * True if the square is a closed, locked door.
 */
bool square_islockeddoor(struct chunk *c, int y, int x) {
	int feat = c->feat[y][x];
	return (tf_has(f_info[feat].flags, TF_DOOR_LOCKED) ||
			tf_has(f_info[feat].flags, TF_DOOR_JAMMED));
}

/**
 * True if the square is a door.
 *
 * This includes open, closed, and hidden doors.
 */
bool square_isdoor(struct chunk *c, int y, int x) {
	int feat = c->feat[y][x];
	return tf_has(f_info[feat].flags, TF_DOOR_ANY);
}

/**
 * True if the square is an unknown trap (it will appear as a floor tile).
 */
bool square_issecrettrap(struct chunk *c, int y, int x) {
    return square_invisible_trap(c, y, x);
}

/**
 * True is the feature is a solid wall (not rubble).
 */
bool feat_is_wall(int feat) {
	return tf_has(f_info[feat].flags, TF_WALL);
}

/**
 * True if the square is a known trap.
 */
bool square_isknowntrap(struct chunk *c, int y, int x) {
	return square_visible_trap(c, y, x);
}

/**
 * True if the feature is a shop entrance.
 */
bool feature_isshop(int feat) {
	return tf_has(f_info[feat].flags, TF_SHOP);
}

/**
 * True if square is any stair
 */
bool square_isstairs(struct chunk*c, int y, int x) {
	int feat = c->feat[y][x];
	return tf_has(f_info[feat].flags, TF_STAIR);
}

/**
 * True if square is an up stair.
 */
bool square_isupstairs(struct chunk*c, int y, int x) {
	int feat = c->feat[y][x];
	return tf_has(f_info[feat].flags, TF_UPSTAIR);
}

/**
 * True if square is a down stair.
 */
bool square_isdownstairs(struct chunk *c, int y, int x) {
	int feat = c->feat[y][x];
	return tf_has(f_info[feat].flags, TF_DOWNSTAIR);
}

/**
 * True if the square is a shop entrance.
 */
bool square_isshop(struct chunk *c, int y, int x) {
	return feature_isshop(c->feat[y][x]);
}

int square_shopnum(struct chunk *c, int y, int x) {
	if (square_isshop(c, y, x))
		return c->feat[y][x] - FEAT_SHOP_HEAD;
	return -1;
}

/**
 * True if the square contains the player
 */
bool square_isplayer(struct chunk *c, int y, int x) {
	return c->m_idx[y][x] < 0 ? TRUE : FALSE;
}

/**
 * SQUARE BEHAVIOR PREDICATES
 *
 * These functions define how a given square behaves, e.g. whether it is
 * passable by the player, whether it is diggable, contains items, etc.
 *
 * These functions use the FEATURE PREDICATES (as well as c->info) to make
 * the determination.
 */

/**
 * True if the square is open (a floor square not occupied by a monster).
 */
bool square_isopen(struct chunk *c, int y, int x) {
	return square_isfloor(c, y, x) && !c->m_idx[y][x];
}

/**
 * True if the square is empty (an open square without any items).
 */
bool square_isempty(struct chunk *c, int y, int x) {
	return square_isopen(c, y, x) && !c->o_idx[y][x];
}

/**
 * True if the square is a floor square without items.
 */
bool square_canputitem(struct chunk *c, int y, int x) {
	return square_isfloor(c, y, x) && !c->o_idx[y][x];
}

/**
 * True if the square can be dug: this includes rubble and non-permanent walls.
 */
bool square_isdiggable(struct chunk *c, int y, int x) {
	return (square_ismineral(c, y, x) ||
			square_issecretdoor(c, y, x) || 
			square_isrubble(c, y, x));
}

/**
 * True if a monster can walk through the feature.
 */
bool feat_is_monster_walkable(feature_type *feature)
{
	return tf_has(feature->flags, TF_PASSABLE);
}

/**
 * True if a monster can walk through the tile.
 *
 * This is needed for polymorphing. A monster may be on a feature that isn't
 * an empty space, causing problems when it is replaced with a new monster.
 */
bool square_is_monster_walkable(struct chunk *c, int y, int x)
{
	assert(square_in_bounds(c, y, x));
	return feat_is_monster_walkable(&f_info[c->feat[y][x]]);
}

/**
 * True if the feature is passable by the player.
 */
bool feat_ispassable(feature_type *f_ptr) {
	return tf_has(f_ptr->flags, TF_PASSABLE);
}

/**
 * True if the square is passable by the player.
 */
bool square_ispassable(struct chunk *c, int y, int x) {
	assert(square_in_bounds(c, y, x));
	return feat_ispassable(&f_info[c->feat[y][x]]);
}

/**
 * True if any projectable can pass through the feature.
 */
bool feat_isprojectable(feature_type *f_ptr) {
	return tf_has(f_ptr->flags, TF_PROJECT);
}

/**
 * True if any projectable can pass through the square.
 *
 * This function is the logical negation of square_iswall().
 */
bool square_isprojectable(struct chunk *c, int y, int x) {
	assert(square_in_bounds(c, y, x));
	return feat_isprojectable(&f_info[c->feat[y][x]]);
}

/**
 * True if the square is a wall square (impedes the player).
 *
 * This function is the logical negation of square_isprojectable().
 */
bool square_iswall(struct chunk *c, int y, int x) {
	assert(square_in_bounds(c, y, x));
	return !square_isprojectable(c, y, x);
}

/**
 * True if the square is a permanent wall or one of the "stronger" walls.
 *
 * The stronger walls are granite, magma and quartz. This excludes things like
 * secret doors and rubble.
 */
bool square_isstrongwall(struct chunk *c, int y, int x) {
	assert(square_in_bounds(c, y, x));
	return square_ismineral(c, y, x) || square_isperm(c, y, x);
}

/**
 * True if a square's terrain is memorized by the player
 */
bool square_ismark(struct chunk *c, int y, int x) {
	assert(square_in_bounds(c, y, x));
	return sqinfo_has(c->info[y][x], SQUARE_MARK);
}

/**
 * True if the square is lit
 */
bool square_isglow(struct chunk *c, int y, int x) {
	assert(square_in_bounds(c, y, x));
	return sqinfo_has(c->info[y][x], SQUARE_GLOW);
}

/**
 * True if the square is part of a vault.
 *
 * This doesn't say what kind of square it is, just that it is part of a vault.
 */
bool square_isvault(struct chunk *c, int y, int x) {
	assert(square_in_bounds(c, y, x));
	return sqinfo_has(c->info[y][x], SQUARE_VAULT);
}

/**
 * True if the square is part of a room.
 */
bool square_isroom(struct chunk *c, int y, int x) {
	assert(square_in_bounds(c, y, x));
	return sqinfo_has(c->info[y][x], SQUARE_ROOM);
}

/**
 * True if the square has been seen by the player
 */
bool square_isseen(struct chunk *c, int y, int x) {
	assert(square_in_bounds(c, y, x));
	return sqinfo_has(c->info[y][x], SQUARE_SEEN);
}

/**
 * True if the cave square is currently viewable by the player
 */
bool square_isview(struct chunk *c, int y, int x) {
	assert(square_in_bounds(c, y, x));
	return sqinfo_has(c->info[y][x], SQUARE_VIEW);
}

/**
 * True if the cave square was seen before the current update
 */
bool square_wasseen(struct chunk *c, int y, int x) {
	assert(square_in_bounds(c, y, x));
	return sqinfo_has(c->info[y][x], SQUARE_WASSEEN);
}

/**
 * True if the square has been detected for traps
 */
bool square_isdtrap(struct chunk *c, int y, int x) {
	assert(square_in_bounds(c, y, x));
	return sqinfo_has(c->info[y][x], SQUARE_DTRAP);
}

/**
 * True if cave square is a feeling trigger square 
 */
bool square_isfeel(struct chunk *c, int y, int x) {
	assert(square_in_bounds(c, y, x));
	return sqinfo_has(c->info[y][x], SQUARE_FEEL);
}

/**
 * True if the square is on the trap detection edge
 */
bool square_isdedge(struct chunk *c, int y, int x) {
	assert(square_in_bounds(c, y, x));
	return sqinfo_has(c->info[y][x], SQUARE_DEDGE);
}

/**
 * True if the square has a known trap
 */
bool square_istrap(struct chunk *c, int y, int x) {
	assert(square_in_bounds(c, y, x));
	return sqinfo_has(c->info[y][x], SQUARE_TRAP);
}

/**
 * True if the square has an unknown trap
 */
bool square_isinvis(struct chunk *c, int y, int x) {
	assert(square_in_bounds(c, y, x));
	return sqinfo_has(c->info[y][x], SQUARE_INVIS);
}

/**
 * True if cave square is an inner wall (generation)
 */
bool square_iswall_inner(struct chunk *c, int y, int x) {
	assert(square_in_bounds(c, y, x));
	return sqinfo_has(c->info[y][x], SQUARE_WALL_INNER);
}

/**
 * True if cave square is an outer wall (generation)
 */
bool square_iswall_outer(struct chunk *c, int y, int x) {
	assert(square_in_bounds(c, y, x));
	return sqinfo_has(c->info[y][x], SQUARE_WALL_OUTER);
}

/**
 * True if cave square is a solid wall (generation)
 */
bool square_iswall_solid(struct chunk *c, int y, int x) {
	assert(square_in_bounds(c, y, x));
	return sqinfo_has(c->info[y][x], SQUARE_WALL_SOLID);
}

/**
 * True if cave square has monster restrictions (generation)
 */
bool square_ismon_restrict(struct chunk *c, int y, int x) {
	assert(square_in_bounds(c, y, x));
	return sqinfo_has(c->info[y][x], SQUARE_MON_RESTRICT);
}

/**
 * True if cave square can't be teleported from by the player
 */
bool square_isno_teleport(struct chunk *c, int y, int x) {
	assert(square_in_bounds(c, y, x));
	return sqinfo_has(c->info[y][x], SQUARE_NO_TELEPORT);
}

/**
 * True if cave square can't be magically mapped by the player
 */
bool square_isno_map(struct chunk *c, int y, int x) {
	assert(square_in_bounds(c, y, x));
	return sqinfo_has(c->info[y][x], SQUARE_NO_MAP);
}

/**
 * True if cave square can't be detected by player ESP
 */
bool square_isno_esp(struct chunk *c, int y, int x) {
	assert(square_in_bounds(c, y, x));
	return sqinfo_has(c->info[y][x], SQUARE_NO_ESP);
}

/**
 * True if the feature is "boring".
 */
bool feat_isboring(feature_type *f_ptr) {
	return !tf_has(f_ptr->flags, TF_INTERESTING);
}

/**
 * True if the cave square is "boring".
 */
bool square_isboring(struct chunk *c, int y, int x) {
	assert(square_in_bounds(c, y, x));
	return feat_isboring(&f_info[c->feat[y][x]]);
}

/**
 * Get a monster on the current level by its index.
 */
struct monster *cave_monster(struct chunk *c, int idx) {
	if (idx <= 0) return NULL;
	return &c->monsters[idx];
}

/**
 * Get a monster on the current level by its position.
 */
struct monster *square_monster(struct chunk *c, int y, int x) {
	if (c->m_idx[y][x] > 0) {
		struct monster *mon = cave_monster(c, c->m_idx[y][x]);
		return mon->race ? mon : NULL;
	}

	return NULL;
}

/**
 * The maximum number of monsters allowed in the level.
 */
int cave_monster_max(struct chunk *c) {
	return c->mon_max;
}

/**
 * The current number of monsters present on the level.
 */
int cave_monster_count(struct chunk *c) {
	return c->mon_cnt;
}

/**
 * Get an object on the current level by its index.
 */
struct object *cave_object(struct chunk *c, int idx) {
	assert(idx > 0);
	assert(idx <= z_info->o_max);
	return &c->objects[idx];
}

/**
 * Get the top object of a pile on the current level by its position.
 */
struct object *square_object(struct chunk *c, int y, int x) {
	if (c->o_idx[y][x] > 0) {
	struct object *obj = cave_object(c, c->o_idx[y][x]);
	return obj->kind ? obj : NULL;
	}

	return NULL;
}

/**
 * The maximum number of objects allowed in the level.
 */
int cave_object_max(struct chunk *c) {
	return c->obj_max;
}

/**
 * The current number of objects present on the level.
 */
int cave_object_count(struct chunk *c) {
	return c->obj_cnt;
}

/**
 * Get a trap on the current level by its index.
 */
struct trap *cave_trap(struct chunk *c, int idx) {
	return &c->traps[idx];
}

/**
 * The maximum number of traps allowed in the level.
 */
int cave_trap_max(struct chunk *c) {
	return c->trap_max;
}

/**
 * Add visible treasure to a mineral square.
 */
void upgrade_mineral(struct chunk *c, int y, int x) {
	switch (c->feat[y][x]) {
	case FEAT_MAGMA: square_set_feat(c, y, x, FEAT_MAGMA_K); break;
	case FEAT_QUARTZ: square_set_feat(c, y, x, FEAT_QUARTZ_K); break;
	}
}

int square_door_power(struct chunk *c, int y, int x) {
	return (c->feat[y][x] - FEAT_DOOR_HEAD) & 0x07;
}

void square_open_door(struct chunk *c, int y, int x) {
	square_set_feat(c, y, x, FEAT_OPEN);
}

void square_smash_door(struct chunk *c, int y, int x) {
	square_set_feat(c, y, x, FEAT_BROKEN);
}

void square_destroy_trap(struct chunk *c, int y, int x) {
	square_remove_trap(c, y, x, FALSE, -1);
}

void square_lock_door(struct chunk *c, int y, int x, int power) {
	square_set_feat(c, y, x, FEAT_DOOR_HEAD + power);
}

bool square_hasgoldvein(struct chunk *c, int y, int x) {
	return tf_has(f_info[c->feat[y][x]].flags, TF_GOLD);
}

void square_tunnel_wall(struct chunk *c, int y, int x) {
	square_set_feat(c, y, x, FEAT_FLOOR);
}

void square_destroy_wall(struct chunk *c, int y, int x) {
	square_set_feat(c, y, x, FEAT_FLOOR);
}

void square_close_door(struct chunk *c, int y, int x) {
	square_set_feat(c, y, x, FEAT_DOOR_HEAD);
}

bool square_isbrokendoor(struct chunk *c, int y, int x) {
	int feat = c->feat[y][x];
    return (tf_has(f_info[feat].flags, TF_DOOR_ANY) &&
			tf_has(f_info[feat].flags, TF_PASSABLE) &&
			!tf_has(f_info[feat].flags, TF_CLOSABLE));
}

void square_add_trap(struct chunk *c, int y, int x) {
	place_trap(c, y, x, -1, c->depth);
}

bool square_iswarded(struct chunk *c, int y, int x) {
	struct trap_kind *rune = lookup_trap("glyph of warding");
	return square_trap_specific(c, y, x, rune->tidx);
}

void square_add_ward(struct chunk *c, int y, int x) {
	struct trap_kind *rune = lookup_trap("glyph of warding");
	place_trap(c, y, x, rune->tidx, 0);
}

void square_remove_ward(struct chunk *c, int y, int x) {
	struct trap_kind *rune = lookup_trap("glyph of warding");
	assert(square_iswarded(c, y, x));
	square_remove_trap_kind(c, y, x, TRUE, rune->tidx);
}

bool square_canward(struct chunk *c, int y, int x) {
	return square_isfloor(c, y, x);
}

bool square_seemslikewall(struct chunk *c, int y, int x) {
	return tf_has(f_info[c->feat[y][x]].flags, TF_ROCK);
}

bool square_isinteresting(struct chunk *c, int y, int x) {
	int f = c->feat[y][x];
	return tf_has(f_info[f].flags, TF_INTERESTING);
}

void square_show_vein(struct chunk *c, int y, int x) {
	if (c->feat[y][x] == FEAT_MAGMA_H)
		square_set_feat(c, y, x, FEAT_MAGMA_K);
	else if (c->feat[y][x] == FEAT_QUARTZ_H)
		square_set_feat(c, y, x, FEAT_QUARTZ_K);
}

void square_add_stairs(struct chunk *c, int y, int x, int depth) {
	int down = randint0(100) < 50;
	if (depth == 0)
		down = 1;
	else if (is_quest(depth) || depth >= MAX_DEPTH - 1)
		down = 0;
	square_set_feat(c, y, x, down ? FEAT_MORE : FEAT_LESS);
}

void square_destroy(struct chunk *c, int y, int x) {
	int feat = FEAT_FLOOR;
	int r = randint0(200);

	if (r < 20)
		feat = FEAT_GRANITE;
	else if (r < 70)
		feat = FEAT_QUARTZ;
	else if (r < 100)
		feat = FEAT_MAGMA;

	square_set_feat(c, y, x, feat);
}

void square_earthquake(struct chunk *c, int y, int x) {
	int t = randint0(100);
	int f;

	if (!square_ispassable(c, y, x)) {
		square_set_feat(c, y, x, FEAT_FLOOR);
		return;
	}

	if (t < 20)
		f = FEAT_GRANITE;
	else if (t < 70)
		f = FEAT_QUARTZ;
	else
		f = FEAT_MAGMA;
	square_set_feat(c, y, x, f);
}

bool square_hassecretvein(struct chunk *c, int y, int x) {
	return (tf_has(f_info[c->feat[y][x]].flags, TF_GOLD) &&
			!tf_has(f_info[c->feat[y][x]].flags, TF_INTERESTING));
}

bool square_noticeable(struct chunk *c, int y, int x) {
	return tf_has(f_info[c->feat[y][x]].flags, TF_INTERESTING);
}

const char *square_apparent_name(struct chunk *c, struct player *p, int y, int x) {
	int f = f_info[c->feat[y][x]].mimic;

	if (!square_ismark(c, y, x) && !player_can_see_bold(y, x))
		f = FEAT_NONE;

	if (f == FEAT_NONE)
		return "unknown_grid";

	return f_info[f].name;
}

void square_unlock_door(struct chunk *c, int y, int x) {
	assert(square_islockeddoor(c, y, x));
	square_set_feat(c, y, x, FEAT_DOOR_HEAD);
}

void square_destroy_door(struct chunk *c, int y, int x) {
	assert(square_isdoor(c, y, x));
	square_set_feat(c, y, x, FEAT_FLOOR);
}

void square_destroy_rubble(struct chunk *c, int y, int x) {
	assert(square_isrubble(c, y, x));
	square_set_feat(c, y, x, FEAT_FLOOR);
}

void square_add_door(struct chunk *c, int y, int x, bool closed) {
	square_set_feat(c, y, x, closed ? FEAT_DOOR_HEAD : FEAT_OPEN);
}

void square_force_floor(struct chunk *c, int y, int x) {
	square_set_feat(c, y, x, FEAT_FLOOR);
}

/*
 * Return the number of doors/traps around (or under) the character.
 */
int count_feats(int *y, int *x, bool (*test)(struct chunk *c, int y, int x), bool under)
{
	int d;
	int xx, yy;
	int count = 0; /* Count how many matches */

	/* Check around (and under) the character */
	for (d = 0; d < 9; d++)
	{
		/* if not searching under player continue */
		if ((d == 8) && !under) continue;

		/* Extract adjacent (legal) location */
		yy = player->py + ddy_ddd[d];
		xx = player->px + ddx_ddd[d];

		/* Paranoia */
		if (!square_in_bounds_fully(cave, yy, xx)) continue;

		/* Must have knowledge */
		if (!square_ismark(cave, yy, xx)) continue;

		/* Not looking for this feature */
		if (!((*test)(cave, yy, xx))) continue;

		/* Count it */
		++count;

		/* Remember the location of the last door found */
		*y = yy;
		*x = xx;
	}

	/* All done */
	return count;
}

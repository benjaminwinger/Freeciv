/********************************************************************** 
 Freeciv - Copyright (C) 1996 - A Kjeldberg, L Gregersen, P Unold
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
***********************************************************************/
#ifndef __UNITFUNC_H
#define __UNITFUNC_H

#include "packets.h"
#include "unit.h"

void diplomat_bribe(struct player *pplayer, struct unit *pdiplomat, 
		    struct unit *pvictim);
void diplomat_get_tech(struct player *pplayer, struct unit *pdiplomat, 
		       struct city  *city);
void diplomat_incite(struct player *pplayer, struct unit *pdiplomat, 
		     struct city *pcity);
void diplomat_sabotage(struct player *pplayer, struct unit *pdiplomat, 
		       struct city *pcity);

void player_restore_units(struct player *pplayer);
void unit_restore_hitpoints(struct player *pplayer, struct unit *punit);
void unit_restore_movepoints(struct player *pplayer, struct unit *punit);
void update_unit_activities(struct player *pplayer);
void update_unit_activity(struct player *pplayer, struct unit *punit);

void create_unit(struct player *pplayer, int x, int y, enum unit_type_id type,
		 int make_veteran, int homecity_id);
void send_remove_unit(struct player *pplayer, int unit_id);
void wipe_unit(struct player *dest, struct unit *punit);
void kill_unit(struct player *dest, struct unit *punit);
void send_unit_info(struct player *dest, struct unit *punit, int dosend);

void maybe_make_veteran(struct unit *punit);
void unit_versus_unit(struct unit *attacker, struct unit *defender);
int get_total_attack_power(struct unit *attacker, struct unit *defender);
int get_total_defense_power(struct unit *attacker, struct unit *defender);
void set_unit_activity(struct unit *punit, enum unit_activity new_activity);
void do_nuke_tile(int x, int y);
void do_nuclear_explosion(int x, int y);
int try_move_unit(struct unit *punit, int dest_x, int dest_y); 
int do_airline(struct unit *punit, int x, int y);
void raze_city(struct city *pcity);
void get_a_tech(struct player *pplayer, struct player *target);
void place_partisans(struct city *pcity,int count);
void make_partisans(struct city *pcity);

int auto_settler_do_goto(struct player *pplayer, struct unit *punit, 
			 int x, int y);
void auto_settler_findwork(struct player *pplayer, struct unit *punit); 
void auto_settlers_player(struct player *pplayer); 
void auto_settlers();

#endif




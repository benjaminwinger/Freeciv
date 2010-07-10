/********************************************************************** 
 Freeciv - Copyright (C) 1996-2004 - The Freeciv Project
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
***********************************************************************/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

/* utility */
#include "fcintl.h"
#include "game.h"
#include "ioz.h"
#include "log.h"
#include "registry.h"

/* common */
#include "map.h"

/* server */
#include "ggzserver.h"
#include "plrhand.h"
#include "report.h"
#include "settings.h"
#include "srv_main.h"
#include "stdinhand.h"

/* The following classes determine what can be changed when.
 * Actually, some of them have the same "changeability", but
 * different types are separated here in case they have
 * other uses.
 * Also, SSET_GAME_INIT/SSET_RULES separate the two sections
 * of server settings sent to the client.
 * See the settings[] array and setting_is_changeable() for what
 * these correspond to and explanations.
 */
enum sset_class {
  SSET_MAP_SIZE,
  SSET_MAP_GEN,
  SSET_MAP_ADD,
  SSET_PLAYERS,
  SSET_GAME_INIT,
  SSET_RULES,
  SSET_RULES_FLEXIBLE,
  SSET_META,
  SSET_LAST
};

typedef bool (*bool_validate_func_t)(bool value, struct connection *pconn,
                                     char *reject_msg,
                                     size_t reject_msg_len);
typedef bool (*int_validate_func_t)(int value, struct connection *pconn,
                                    char *reject_msg,
                                    size_t reject_msg_len);
typedef bool (*string_validate_func_t)(const char * value,
                                       struct connection *pconn,
                                       char *reject_msg,
                                       size_t reject_msg_len);
typedef bool (*enum_validate_func_t) (int value, struct connection *pconn,
                                      char *reject_msg,
                                      size_t reject_msg_len);

typedef void (*action_callback_func_t)(const struct setting *pset);
typedef const char * (*bool_name_func_t) (bool value);
typedef const char * (*enum_name_func_t) (int value);

struct setting {
  const char *name;
  enum sset_class sclass;
  bool to_client;

  /*
   * Sould be less than 42 chars (?), or shorter if the values may
   * have more than about 4 digits. Don't put "." on the end.
   */
  const char *short_help;

  /*
   * May be empty string, if short_help is sufficient. Need not
   * include embedded newlines (but may, for formatting); lines will
   * be wrapped (and indented) automatically. Should have punctuation
   * etc, and should end with a "."
   */
  const char *extra_help;
  enum sset_type stype;
  enum sset_category scategory;
  enum sset_level slevel;

  /*
   * About the *_validate functions: If the function is non-NULL, it
   * is called with the new value, and returns whether the change is
   * legal. The char * is an error message in the case of reject.
   */

  union {
    /*** bool part ***/
    struct {
      bool *const pvalue;
      const bool default_value;
      const bool_validate_func_t validate;
      const bool_name_func_t name;
      bool game_value;
    } boolean;
    /*** int part ***/
    struct {
      int *const pvalue;
      const int default_value;
      const int min_value;
      const int max_value;
      const int_validate_func_t validate;
      int game_value;
    } integer;
    /*** string part ***/
    struct {
      char *const value;
      const char *const default_value;
      const size_t value_size;
      const string_validate_func_t validate;
      char *game_value;
    } string;
    /*** enumerator part ***/
    struct {
      int *const pvalue;
      const int default_value;
      const enum_validate_func_t validate;
      const enum_name_func_t name;
      int game_value;
    } enumerator;
  };

  /* action function */
  const action_callback_func_t action;

  /* ruleset lock for game settings */
  bool locked;
};

/* Category names must match the values in enum sset_category. */
const char *sset_category_names[] = {N_("Geological"),
				     N_("Sociological"),
				     N_("Economic"),
				     N_("Military"),
				     N_("Scientific"),
				     N_("Internal"),
				     N_("Networking")};

/* Level names must match the values in enum sset_level. */
const char *sset_level_names[] = {N_("None"),
				  N_("All"),
				  N_("Vital"),
				  N_("Situational"),
				  N_("Rare"),
				  N_("Changed"),
				  N_("Locked")};
const int OLEVELS_NUM = ARRAY_SIZE(sset_level_names);

static void setting_set_to_default(struct setting *pset);
static bool setting_ruleset_one(struct section_file *file,
                                const char *name, const char *path);
static void setting_game_set(struct setting *pset, bool init);
static void setting_game_free(struct setting *pset);
static void setting_game_restore(struct setting *pset);

#define settings_snprintf(_buf, _buf_len, format, ...)                      \
  if (_buf != NULL) {                                                       \
    fc_snprintf(_buf, _buf_len, format, ## __VA_ARGS__);                    \
  }

/****************************************************************************
  Enumerator name accessors.
****************************************************************************/

/****************************************************************************
  Topology setting names accessor.
****************************************************************************/
static const char *topology_name(int topology)
{
  static const char *names[] = {
    N_("Flat Earth (unwrapped)"),
    N_("Earth (wraps E-W)"),
    N_("Uranus (wraps N-S)"),
    N_("Donut World (wraps N-S, E-W)"),
    N_("Flat Earth (isometric)"),
    N_("Earth (isometric)"),
    N_("Uranus (isometric)"),
    N_("Donut World (isometric)"),
    N_("Flat Earth (hexagonal)"),
    N_("Earth (hexagonal)"),
    N_("Uranus (hexagonal)"),
    N_("Donut World (hexagonal)"),
    N_("Flat Earth (iso-hex)"),
    N_("Earth (iso-hex)"),
    N_("Uranus (iso-hex)"),
    N_("Donut World (iso-hex)")
  };

  return (0 <= topology && topology < ARRAY_SIZE(names)
          ? names[topology] : NULL);
}

/****************************************************************************
  Generator setting names accessor.
****************************************************************************/
static const char *generator_name(int generator)
{
  static const char *names[] = {
    N_("Scenario map"),
    N_("Fully random height"),
    N_("Pseudo-fractal height"),
    N_("Island-based")
  };

  return (0 <= generator && generator < ARRAY_SIZE(names)
          ? names[generator] : NULL);
}

/****************************************************************************
  Start position setting names accessor.
****************************************************************************/
static const char *startpos_name(int startpos)
{
  static const char *names[] = {
    N_("Generator's choice"),
    N_("One player per continent"),
    N_("Two players per continent"),
    N_("All players on a single continent"),
    N_("Depending on size of continents")
  };

  return (0 <= startpos && startpos < ARRAY_SIZE(names)
          ? names[startpos] : NULL);
}

/****************************************************************************
  Borders setting names accessor.
****************************************************************************/
static const char *borders_name(int borders)
{
 static const char *names[] = {
    N_("Disabled"),
    N_("Enabled"),
    N_("See everything inside borders"),
    N_("Borders expand to unknown, revealing tiles")
  };

  return (0 <= borders && borders < ARRAY_SIZE(names)
          ? names[borders] : NULL);
}

/****************************************************************************
  Names accessor for all bool (disable/enable or [0/1]) settings.
****************************************************************************/
static const char *bool_name(bool enable)
{
 static const char *names[] = {
    N_("Disabled"),
    N_("Enabled"),
  };

  return (enable ? names[1] : names[0]);
}

/****************************************************************************
  Diplomacy setting names accessor.
****************************************************************************/
static const char *diplomacy_name(int diplomacy)
{
 static const char *names[] = {
    N_("Enabled for everyone"),
    N_("Only allowed between human players"),
    N_("Only allowed between AI players"),
    N_("Restricted to teams"),
    N_("Disabled for everyone")
  };

  return (0 <= diplomacy && diplomacy < ARRAY_SIZE(names)
          ? names[diplomacy] : NULL);
}

/****************************************************************************
  City name setting names accessor.
****************************************************************************/
static const char *cityname_name(int cityname)
{
 static const char *names[] = {
    N_("No restrictions"),
    N_("Unique to a player"),
    N_("Globally unique"),
    N_("No city name stealing")
  };

  return (0 <= cityname && cityname < ARRAY_SIZE(names)
          ? names[cityname] : NULL);
}

/****************************************************************************
  Barbarian setting names accessor.
****************************************************************************/
static const char *barbarians_name(int barbarians)
{
 static const char *names[] = {
    N_("No barbarians"),
    N_("Only in huts"),
    N_("Normal rate of appearance"),
    N_("Frequent barbarian uprising"),
    N_("Raging hordes")
  };

  return (0 <= barbarians && barbarians < ARRAY_SIZE(names)
          ? names[barbarians] : NULL);
}

/****************************************************************************
  Phase mode names accessor.
****************************************************************************/
static const char *phasemode_name(int phasemode)
{
 static const char *names[] = {
    N_("All players move concurrently"),
    N_("All players alternate movement"),
    N_("Team alternate movement")
  };

  return (0 <= phasemode && phasemode < ARRAY_SIZE(names)
          ? names[phasemode] : NULL);
}


/*************************************************************************
  Action callback functions. 'caller' and 'message' are not used and
  should be set to NULL by the calling function.
*************************************************************************/

/*************************************************************************
  Create the selected number of AI's.
*************************************************************************/
static void aifill_action(const struct setting *pset)
{
  aifill(*pset->integer.pvalue);
}

/*************************************************************************
  Toggle player AI status.
*************************************************************************/
static void autotoggle_action(const struct setting *pset)
{
  if (*pset->boolean.pvalue) {
    players_iterate(pplayer) {
      if (!pplayer->ai_data.control && !pplayer->is_connected) {
        toggle_ai_player_direct(NULL, pplayer);
        send_player_info_c(pplayer, game.est_connections);
      }
    } players_iterate_end;
  }
}

/*************************************************************************
  Validation callback functions.
*************************************************************************/

/****************************************************************************
  Verify the value of the generator option (notably the 0 case).
****************************************************************************/
static bool generator_validate(int value, struct connection *caller,
                               char *reject_msg, size_t reject_msg_len)
{
  if (map_is_empty()) {
    if (0 == value) {
      settings_snprintf(reject_msg, reject_msg_len,
                        _("You cannot disable the map generator."));
      return FALSE;
    }
    return TRUE;
  } else {
    if (0 != value) {
      settings_snprintf(reject_msg, reject_msg_len,
                        _("You cannot require a map generator "
                          "when a map is loaded."));
      return FALSE;
    }
  }
  return TRUE;
}

/*************************************************************************
  Verify that a given demography string is valid. See
  game.demography.
*************************************************************************/
static bool demography_callback(const char *value,
                                struct connection *caller,
                                char *reject_msg,
                                size_t reject_msg_len)
{
  int error;

  if (is_valid_demography(value, &error)) {
    return TRUE;
  } else {
    settings_snprintf(reject_msg, reject_msg_len,
                      _("Demography string validation failed at character: "
                        "'%c'. Try \"help demography\"."), value[error]);
    return FALSE;
  }
}

/*************************************************************************
  Verify that a given allowtake string is valid.  See
  game.allow_take.
*************************************************************************/
static bool allowtake_callback(const char *value,
                               struct connection *caller,
                               char *reject_msg,
                               size_t reject_msg_len)
{
  int len = strlen(value), i;
  bool havecharacter_state = FALSE;

  /* We check each character individually to see if it's valid.  This
   * does not check for duplicate entries.
   *
   * We also track the state of the machine.  havecharacter_state is
   * true if the preceeding character was a primary label, e.g.
   * NHhAadb.  It is false if the preceeding character was a modifier
   * or if this is the first character. */

  for (i = 0; i < len; i++) {
    /* Check to see if the character is a primary label. */
    if (strchr("HhAadbOo", value[i])) {
      havecharacter_state = TRUE;
      continue;
    }

    /* If we've already passed a primary label, check to see if the
     * character is a modifier. */
    if (havecharacter_state && strchr("1234", value[i])) {
      havecharacter_state = FALSE;
      continue;
    }

    /* Looks like the character was invalid. */
    settings_snprintf(reject_msg, reject_msg_len,
                      _("Allowed take string validation failed at "
                        "character: '%c'. Try \"help allowtake\"."),
                      value[i]);
    return FALSE;
  }

  /* All characters were valid. */
  return TRUE;
}

/*************************************************************************
  Verify that a given startunits string is valid.  See
  game.server.start_units.
*************************************************************************/
static bool startunits_callback(const char *value,
                                struct connection *caller,
                                char *reject_msg,
                                size_t reject_msg_len)
{
  int len = strlen(value), i;
  bool have_founder = FALSE;

  /* We check each character individually to see if it's valid, and
   * also make sure there is at least one city founder. */

  for (i = 0; i < len; i++) {
    /* Check for a city founder */
    if (value[i] == 'c') {
      have_founder = TRUE;
      continue;
    }
    /* TODO: add 'f' back in here when we can support ferry units */
    if (strchr("cwxksdDaA", value[i])) {
      continue;
    }

    /* Looks like the character was invalid. */
    settings_snprintf(reject_msg, reject_msg_len,
                      _("Starting units string validation failed at "
                        "characters '%c'. Try \"help startunits\"."),
                      value[i]);
    return FALSE;
  }

  if (!have_founder) {
    settings_snprintf(reject_msg, reject_msg_len,
                      _("No city founder ('c') within the starting units "
                        "string: '%s'. Try \"help startunits\"."), value);
    return FALSE;
  }

  /* All characters were valid. */
  return TRUE;
}

/*************************************************************************
  Verify that a given endturn is valid.
*************************************************************************/
static bool endturn_callback(int value, struct connection *caller,
                             char *reject_msg, size_t reject_msg_len)
{
  if (value < game.info.turn) {
    /* Tried to set endturn earlier than current turn */
    settings_snprintf(reject_msg, reject_msg_len,
                      _("Cannot set endturn earlier than current turn."));
    return FALSE;
  }
  return TRUE;
}

/*************************************************************************
  Verify that a given maxplayers string is valid.
*************************************************************************/
static bool maxplayers_callback(int value, struct connection *caller,
                                char *reject_msg, size_t reject_msg_len)
{
#ifdef GGZ_SERVER
  if (with_ggz) {
    /* In GGZ mode the maxplayers is the number of actual players - set
     * when the game is lauched and not changed thereafter.  This may be
     * changed in future. */
    settings_snprintf(reject_msg, reject_msg_len,
                      _("Cannot change maxplayers in GGZ mode."));
    return FALSE;
  }
#endif
  if (value < player_count()) {
    settings_snprintf(reject_msg, reject_msg_len,
                      _("Number of players (%d) is higher than requested "
                        "value (%d). Keeping old value."), player_count(),
                      value);
    return FALSE;
  }

  return TRUE;
}

/*************************************************************************
  Disallow low timeout values for non-hack connections.
*************************************************************************/
static bool timeout_callback(int value, struct connection *caller,
                             char *reject_msg, size_t reject_msg_len)
{
  if (caller && caller->access_level < ALLOW_HACK && value < 30) {
    settings_snprintf(reject_msg, reject_msg_len,
                      _("You are not allowed to set timeout values less "
                        "than 30 seconds."));
    return FALSE;
  }

  if (value == -1 && game.server.unitwaittime != 0) {
    /* autogame only with 'unitwaitime' = 0 */
    settings_snprintf(reject_msg, reject_msg_len,
                      _("For autogames ('timeout' = -1) 'unitwaittime' "
                        "should be deactivated (= 0)."));
    return FALSE;
  }

  if (value != -1 && value < game.server.unitwaittime * 3 / 2) {
    /* for normal games 'timeout' should be at least 3/2 times the value
     * of 'unitwaittime' */
    settings_snprintf(reject_msg, reject_msg_len,
                      _("'timeout' can not be lower than 3/2 of the "
                        "'unitwaittime' setting (= %d). Please change "
                        "'unitwaittime' first."), game.server.unitwaittime);
    return FALSE;
  }

  return TRUE;
}

/*************************************************************************
  Check 'timeout' setting if 'unitwaittime' is changed.
*************************************************************************/
static bool unitwaittime_callback(int value, struct connection *caller,
                                  char *reject_msg, size_t reject_msg_len)
{
  if (game.info.timeout == -1 && value != 0) {
    settings_snprintf(reject_msg, reject_msg_len,
                      _("For autogames ('timeout' = -1) 'unitwaittime' "
                        "should be deactivated (= 0)."));
    return FALSE;
  }

  if (value > game.info.timeout * 2 / 3) {
    settings_snprintf(reject_msg, reject_msg_len,
                      _("'unitwaittime' has to be lower than 2/3 of the "
                        "'timeout' setting (= %d). Please change 'timeout' "
                        "first."), game.info.timeout);
    return FALSE;
  }

  return TRUE;
}

/*************************************************************************
  Check that everyone is on a team for team-alternating simultaneous
  phases. NB: Assumes that it is not possible to first set team
  alternating phase mode then make teamless players.
*************************************************************************/
static bool phasemode_callback(int value, struct connection *caller,
                               char *reject_msg, size_t reject_msg_len)
{
  if (value == PMT_TEAMS_ALTERNATE) {
    players_iterate(pplayer) {
      if (!pplayer->team) {
        settings_snprintf(reject_msg, reject_msg_len,
                          _("All players must have a team if this option "
                            "value is used."));
        return FALSE;
      }
    } players_iterate_end;
  }

  return TRUE;
}

#define GEN_BOOL(name, value, sclass, scateg, slevel, to_client,            \
                 short_help, extra_help, func_validate, func_action,        \
                 _default)                                                  \
  {name, sclass, to_client, short_help, extra_help, SSET_BOOL,              \
      scateg, slevel,                                                       \
      {.boolean = {&value, _default, func_validate, bool_name,              \
                   FALSE}}, func_action, FALSE},

#define GEN_INT(name, value, sclass, scateg, slevel, to_client,         \
                short_help, extra_help, func_validate, func_action,     \
                _min, _max, _default)                                   \
  {name, sclass, to_client, short_help, extra_help, SSET_INT,           \
      scateg, slevel,                                                   \
      {.integer = {(int *) &value, _default, _min, _max, func_validate, \
                   0}},                                                 \
      func_action, FALSE},

#define GEN_STRING(name, value, sclass, scateg, slevel, to_client,      \
                   short_help, extra_help, func_validate, func_action,  \
                   _default)                                            \
  {name, sclass, to_client, short_help, extra_help, SSET_STRING,        \
      scateg, slevel,                                                   \
      {.string = {value, _default, sizeof(value), func_validate, ""}},  \
      func_action, FALSE},

#define GEN_ENUM(name, value, sclass, scateg, slevel, to_client,            \
                 short_help, extra_help, func_validate, func_action,        \
                 func_name, _default)                                       \
  { name, sclass, to_client, short_help, extra_help, SSET_ENUM,             \
    scateg, slevel,                                                         \
     { .enumerator = { (int *) &value, _default, func_validate,             \
       (enum_name_func_t) func_name, 0 }}, func_action, FALSE},

#define GEN_END                                                         \
  {NULL, SSET_LAST, SSET_SERVER_ONLY, NULL, NULL, SSET_INT,             \
      SSET_NUM_CATEGORIES, SSET_NONE,                                   \
      {.boolean = {FALSE, FALSE, NULL, FALSE}}, NULL, FALSE},

/* game settings */
static struct setting settings[] = {

  /* These should be grouped by sclass */
  
  /* Map size parameters: adjustable if we don't yet have a map */  
  GEN_INT("size", map.server.size, SSET_MAP_SIZE,
	  SSET_GEOLOGY, SSET_VITAL, SSET_TO_CLIENT,
          N_("Map size (in thousands of tiles)"),
          N_("This value is used to determine the map dimensions.\n"
             "  size = 4 is a normal map of 4,000 tiles (default)\n"
             "  size = 20 is a huge map of 20,000 tiles"), NULL, NULL,
          MAP_MIN_SIZE, MAP_MAX_SIZE, MAP_DEFAULT_SIZE)
  GEN_ENUM("topology", map.topology_id, SSET_MAP_SIZE,
           SSET_GEOLOGY, SSET_VITAL, SSET_TO_CLIENT,
           N_("Map topology index"),
           /* TRANS: do not edit the ugly ASCII art */
           N_("Freeciv maps are always two-dimensional. They may wrap at "
              "the north-south and east-west directions to form a flat map, "
              "a cylinder, or a torus (donut). Individual tiles may be "
              "rectangular or hexagonal, with either a classic or isometric "
              "alignment - this should be set based on the tileset being "
              "used.\n"
              "Classic rectangular:       Isometric rectangular:\n"
              "      _________               /\\/\\/\\/\\/\\\n"
              "     |_|_|_|_|_|             /\\/\\/\\/\\/\\/\n"
              "     |_|_|_|_|_|             \\/\\/\\/\\/\\/\\\n"
              "     |_|_|_|_|_|             /\\/\\/\\/\\/\\/\n"
              "                             \\/\\/\\/\\/\\/\n"
              "Hex tiles:                 Iso-hex:\n"
              "  /\\/\\/\\/\\/\\/\\               _   _   _   _   _\n"
              "  | | | | | | |             / \\_/ \\_/ \\_/ \\_/ \\\n"
              "  \\/\\/\\/\\/\\/\\/\\             \\_/ \\_/ \\_/ \\_/ \\_/\n"
              "   | | | | | | |            / \\_/ \\_/ \\_/ \\_/ \\\n"
              "   \\/\\/\\/\\/\\/\\/             \\_/ \\_/ \\_/ \\_/ \\_/\n"
          ), NULL, NULL, topology_name, MAP_DEFAULT_TOPO)

  /* Map generation parameters: once we have a map these are of historical
   * interest only, and cannot be changed.
   */
  GEN_ENUM("generator", map.server.generator,
           SSET_MAP_GEN, SSET_GEOLOGY, SSET_VITAL,  SSET_TO_CLIENT,
           N_("Method used to generate map"),
           /* TRANS: Don't translate "startpos". */
           N_("If the default value of startpos is used then a startpos "
              "setting will be chosen based on the generator:\n"
              "- \"Fully random height\": depending on continent size\n"
              "- \"Pseudo-fractal height\": all on a single continent\n"
              "- \"Island-based\": one player per continent\n"
              "See the \"startpos\" setting."),
           generator_validate, NULL, generator_name, MAP_DEFAULT_GENERATOR)

  GEN_ENUM("startpos", map.server.startpos,
           SSET_MAP_GEN, SSET_GEOLOGY, SSET_VITAL, SSET_TO_CLIENT,
           N_("Method used to choose start positions"),
           N_("Selecting \"Generator's choice\" means the default value "
              "will be picked based on the generator chosen. See the "
              "\"generator\" setting.\n"
              "Note: generators try to create the right number of "
              "continents for the choice of start pos and to the number "
              "of players"), NULL, NULL, startpos_name, MAP_DEFAULT_STARTPOS)

  GEN_BOOL("tinyisles", map.server.tinyisles,
	   SSET_MAP_GEN, SSET_GEOLOGY, SSET_RARE, SSET_TO_CLIENT,
	   N_("Presence of 1x1 islands"),
           N_("0 = no 1x1 islands; 1 = some 1x1 islands"), NULL, NULL,
	   MAP_DEFAULT_TINYISLES)

  GEN_BOOL("separatepoles", map.server.separatepoles,
	   SSET_MAP_GEN, SSET_GEOLOGY, SSET_SITUATIONAL, SSET_TO_CLIENT,
	   N_("Whether the poles are separate continents"),
	   N_("0 = continents may attach to poles; 1 = poles will "
              "usually be separate"), NULL, NULL,
	   MAP_DEFAULT_SEPARATE_POLES)

  GEN_BOOL("alltemperate", map.server.alltemperate, 
           SSET_MAP_GEN, SSET_GEOLOGY, SSET_RARE, SSET_TO_CLIENT,
	   N_("All the map is temperate"),
	   N_("0 = normal Earth-like planet; 1 = all-temperate planet "),
           NULL, NULL, MAP_DEFAULT_ALLTEMPERATE)

  GEN_INT("temperature", map.server.temperature,
          SSET_MAP_GEN, SSET_GEOLOGY, SSET_SITUATIONAL, SSET_TO_CLIENT,
          N_("Average temperature of the planet"),
          N_("Small values will give a cold map, while larger values will "
             "give a hotter map.\n"
             "\n"
             "100 means a very dry and hot planet with no polar arctic "
             "zones, only tropical and dry zones.\n"
             " 70 means a hot planet with little polar ice.\n"
             " 50 means a temperate planet with normal polar, cold, "
             "temperate, and tropical zones; a desert zone overlaps "
             "tropical and temperate zones.\n"
             " 30 means a cold planet with small tropical zones.\n"
             "  0 means a very cold planet with large polar zones and no "
             "tropics"),
          NULL, NULL,
          MAP_MIN_TEMPERATURE, MAP_MAX_TEMPERATURE, MAP_DEFAULT_TEMPERATURE)
 
  GEN_INT("landmass", map.server.landpercent,
	  SSET_MAP_GEN, SSET_GEOLOGY, SSET_SITUATIONAL, SSET_TO_CLIENT,
	  N_("Percentage of the map that is land"),
	  N_("This setting gives the approximate percentage of the map "
             "that will be made into land."), NULL, NULL,
	  MAP_MIN_LANDMASS, MAP_MAX_LANDMASS, MAP_DEFAULT_LANDMASS)

  GEN_INT("steepness", map.server.steepness,
	  SSET_MAP_GEN, SSET_GEOLOGY, SSET_SITUATIONAL, SSET_TO_CLIENT,
	  N_("Amount of hills/mountains"),
	  N_("Small values give flat maps, while higher values give a "
             "steeper map with more hills and mountains."), NULL, NULL,
	  MAP_MIN_STEEPNESS, MAP_MAX_STEEPNESS, MAP_DEFAULT_STEEPNESS)

  GEN_INT("wetness", map.server.wetness,
 	  SSET_MAP_GEN, SSET_GEOLOGY, SSET_SITUATIONAL, SSET_TO_CLIENT,
 	  N_("Amount of water on lands"), 
	  N_("Small values mean lots of dry, desert-like land; "
	     "higher values give a wetter map with more swamps, "
             "jungles, and rivers."), NULL, NULL,
          MAP_MIN_WETNESS, MAP_MAX_WETNESS, MAP_DEFAULT_WETNESS)

  GEN_BOOL("globalwarming", game.info.global_warming,
           SSET_RULES, SSET_GEOLOGY, SSET_VITAL, SSET_TO_CLIENT,
           N_("Global warming"),
           N_("If turned off, global warming will not occur "
              "as a result of pollution. This settings does not "
              "affect pollution."), NULL, NULL,
           GAME_DEFAULT_GLOBAL_WARMING)

  GEN_BOOL("nuclearwinter", game.info.nuclear_winter,
           SSET_RULES, SSET_GEOLOGY, SSET_VITAL, SSET_TO_CLIENT,
           N_("Nuclear winter"),
           N_("If turned off, nuclear winter will not occur "
              "as a result of nuclear war."), NULL, NULL,
           GAME_DEFAULT_NUCLEAR_WINTER)

  GEN_INT("mapseed", map.server.seed,
	  SSET_MAP_GEN, SSET_INTERNAL, SSET_RARE, SSET_SERVER_ONLY,
	  N_("Map generation random seed"),
	  N_("The same seed will always produce the same map; "
	     "for zero (the default) a seed will be chosen based on "
	     "the time to give a random map. This setting is usually "
             "only of interest while debugging the game."), NULL, NULL,
	  MAP_MIN_SEED, MAP_MAX_SEED, MAP_DEFAULT_SEED)

  /* Map additional stuff: huts and specials.  gameseed also goes here
   * because huts and specials are the first time the gameseed gets used (?)
   * These are done when the game starts, so these are historical and
   * fixed after the game has started.
   */
  GEN_INT("gameseed", game.server.seed,
	  SSET_MAP_ADD, SSET_INTERNAL, SSET_RARE, SSET_SERVER_ONLY,
	  N_("Game random seed"),
	  N_("For zero (the default) a seed will be chosen based "
	     "on the time. This setting is usually "
             "only of interest while debugging the game"), NULL, NULL,
	  GAME_MIN_SEED, GAME_MAX_SEED, GAME_DEFAULT_SEED)

  GEN_INT("specials", map.server.riches,
	  SSET_MAP_ADD, SSET_GEOLOGY, SSET_VITAL, SSET_TO_CLIENT,
	  N_("Amount of \"special\" resource tiles"),
	  N_("Special resources improve the basic terrain type they "
	     "are on. The server variable's scale is parts per "
             "thousand."), NULL, NULL,
	  MAP_MIN_RICHES, MAP_MAX_RICHES, MAP_DEFAULT_RICHES)

  GEN_INT("huts", map.server.huts,
	  SSET_MAP_ADD, SSET_GEOLOGY, SSET_VITAL, SSET_TO_CLIENT,
	  N_("Amount of huts (minor tribe villages)"),
	  N_("This setting gives the exact number of huts that will be "
	     "placed on the entire map. Huts are small tribal villages "
             "that may be investigated by units."), NULL, NULL,
          MAP_MIN_HUTS, MAP_MAX_HUTS, MAP_DEFAULT_HUTS)

  /* Options affecting numbers of players and AI players.  These only
   * affect the start of the game and can not be adjusted after that.
   * (Actually, minplayers does also affect reloads: you can't start a
   * reload game until enough players have connected (or are AI).)
   */
  GEN_INT("minplayers", game.server.min_players,
	  SSET_PLAYERS, SSET_INTERNAL, SSET_VITAL,
          SSET_TO_CLIENT,
	  N_("Minimum number of players"),
	  N_("There must be at least this many players (connected "
	     "human players) before the game can start."),
          NULL, NULL,
	  GAME_MIN_MIN_PLAYERS, GAME_MAX_MIN_PLAYERS, GAME_DEFAULT_MIN_PLAYERS)

  GEN_INT("maxplayers", game.server.max_players,
	  SSET_PLAYERS, SSET_INTERNAL, SSET_VITAL, SSET_TO_CLIENT,
	  N_("Maximum number of players"),
          N_("The maximal number of human and AI players who can be in "
             "the game. When this number of players are connected in "
             "the pregame state, any new players who try to connect "
             "will be rejected."), maxplayers_callback, NULL,
	  GAME_MIN_MAX_PLAYERS, GAME_MAX_MAX_PLAYERS, GAME_DEFAULT_MAX_PLAYERS)

  GEN_INT("aifill", game.info.aifill,
	  SSET_PLAYERS, SSET_INTERNAL, SSET_VITAL, SSET_TO_CLIENT,
	  N_("Limited number of AI players"),
	  N_("If set to a positive value, then AI players will be "
	     "automatically created or removed to keep the total "
	     "number of players at this amount.  As more players join, "
	     "these AI players will be replaced.  When set to zero, "
             "all AI players will be removed."), NULL,
          aifill_action, GAME_MIN_AIFILL, GAME_MAX_AIFILL,
          GAME_DEFAULT_AIFILL)

  GEN_INT("ec_turns", game.server.event_cache.turns,
          SSET_RULES_FLEXIBLE, SSET_INTERNAL, SSET_SITUATIONAL, SSET_TO_CLIENT,
          N_("Event cache for this number of turns"),
          N_("Event messages are saved for this number of turns. A value of "
             "0 deactivates the event cache."),
          NULL, NULL, GAME_MIN_EVENT_CACHE_TURNS, GAME_MAX_EVENT_CACHE_TURNS,
          GAME_DEFAULT_EVENT_CACHE_TURNS)

  GEN_INT("ec_max_size", game.server.event_cache.max_size,
          SSET_RULES_FLEXIBLE, SSET_INTERNAL, SSET_SITUATIONAL, SSET_TO_CLIENT,
          N_("Size of the event cache"),
          N_("This defines the maximal number of events in the event cache."),
          NULL, NULL, GAME_MIN_EVENT_CACHE_MAX_SIZE, GAME_MAX_EVENT_CACHE_MAX_SIZE,
          GAME_DEFAULT_EVENT_CACHE_MAX_SIZE)

  GEN_BOOL("ec_chat", game.server.event_cache.chat,
           SSET_RULES_FLEXIBLE, SSET_INTERNAL, SSET_SITUATIONAL, SSET_TO_CLIENT,
           N_("Save chat messages in the event cache"),
           N_("If set to 1 chat messages will be saved in the event cache."),
           NULL, NULL, GAME_DEFAULT_EVENT_CACHE_CHAT)

  GEN_BOOL("ec_info", game.server.event_cache.info,
           SSET_RULES_FLEXIBLE, SSET_INTERNAL, SSET_SITUATIONAL, SSET_TO_CLIENT,
           N_("Print turn and time for each cached event"),
           N_("If set to 1 all cached events will be marked by the turn and time "
              "of the event like '(T2 - 15:29:52)'."),
           NULL, NULL, GAME_DEFAULT_EVENT_CACHE_INFO)

  /* Game initialization parameters (only affect the first start of the game,
   * and not reloads).  Can not be changed after first start of game.
   */
  /* TODO: Add this line back when we can support Ferry units */
  /* "    f   = Ferryboat (eg., Trireme)\n" */
  GEN_STRING("startunits", game.server.start_units,
	     SSET_GAME_INIT, SSET_SOCIOLOGY, SSET_VITAL, SSET_TO_CLIENT,
             N_("List of players' initial units"),
             N_("This should be a string of characters, each of which "
		"specifies a unit role. There must be at least one city "
		"founder in the string. The characters and their "
		"meanings are:\n"
		"    c   = City founder (eg., Settlers)\n"
		"    w   = Terrain worker (eg., Engineers)\n"
		"    x   = Explorer (eg., Explorer)\n"
		"    k   = Gameloss (eg., King)\n"
		"    s   = Diplomat (eg., Diplomat)\n"
		"    d   = Ok defense unit (eg., Warriors)\n"
		"    D   = Good defense unit (eg., Phalanx)\n"
		"    a   = Fast attack unit (eg., Horsemen)\n"
		"    A   = Strong attack unit (eg., Catapult)\n"),
             startunits_callback, NULL, GAME_DEFAULT_START_UNITS)

  GEN_INT("dispersion", game.server.dispersion,
	  SSET_GAME_INIT, SSET_SOCIOLOGY, SSET_SITUATIONAL, SSET_TO_CLIENT,
	  N_("Area where initial units are located"),
	  N_("This is the radius within "
             "which the initial units are dispersed."), NULL, NULL,
	  GAME_MIN_DISPERSION, GAME_MAX_DISPERSION, GAME_DEFAULT_DISPERSION)

  GEN_INT("gold", game.info.gold,
	  SSET_GAME_INIT, SSET_ECONOMICS, SSET_VITAL, SSET_TO_CLIENT,
	  N_("Starting gold per player"), 
	  N_("At the beginning of the game, each player is given this "
             "much gold."), NULL, NULL,
	  GAME_MIN_GOLD, GAME_MAX_GOLD, GAME_DEFAULT_GOLD)

  GEN_INT("techlevel", game.info.tech,
	  SSET_GAME_INIT, SSET_SCIENCE, SSET_VITAL, SSET_TO_CLIENT,
	  N_("Number of initial techs per player"), 
	  N_("At the beginning of the game, each player is given this "
	     "many technologies. The technologies chosen are random for "
	     "each player. Depending on the value of tech_cost_style in "
             "the ruleset, a big value for techlevel can make the next "
             "techs really expensive."), NULL, NULL,
	  GAME_MIN_TECHLEVEL, GAME_MAX_TECHLEVEL, GAME_DEFAULT_TECHLEVEL)

  GEN_INT("sciencebox", game.info.sciencebox,
	  SSET_RULES, SSET_SCIENCE, SSET_SITUATIONAL, SSET_TO_CLIENT,
	  N_("Technology cost multiplier percentage"),
	  N_("This affects how quickly players can research new "
	     "technology. All tech costs are multiplied by this amount "
	     "(as a percentage). The base tech costs are determined by "
	     "the ruleset or other game settings."),
          NULL, NULL, GAME_MIN_SCIENCEBOX, GAME_MAX_SCIENCEBOX,
	  GAME_DEFAULT_SCIENCEBOX)

  GEN_INT("techpenalty", game.server.techpenalty,
	  SSET_RULES, SSET_SCIENCE, SSET_RARE, SSET_TO_CLIENT,
	  N_("Percentage penalty when changing tech"),
	  N_("If you change your current research technology, and you have "
	     "positive research points, you lose this percentage of those "
	     "research points. This does not apply when you have just gained "
             "a technology this turn."), NULL, NULL,
	  GAME_MIN_TECHPENALTY, GAME_MAX_TECHPENALTY,
	  GAME_DEFAULT_TECHPENALTY)

  GEN_INT("techlost_recv", game.server.techlost_recv,
          SSET_RULES, SSET_SCIENCE, SSET_RARE, SSET_TO_CLIENT,
          N_("Chance to lose an invention while receiving it"),
          N_("If you receive an invention via an treaty this setting "
             "defines the chance that the invention is lost during the "
             "transfer."),
          NULL, NULL, GAME_MIN_TECHLOST_RECV, GAME_MAX_TECHLOST_RECV,
          GAME_DEFAULT_TECHLOST_RECV)

  GEN_INT("techlost_donor", game.server.techlost_donor,
          SSET_RULES, SSET_SCIENCE, SSET_RARE, SSET_TO_CLIENT,
          N_("Chance to lose an invention while giving it"),
          N_("If you give an invention via an treaty this setting "
             "defines the chance that the invention is lost for your "
             "civilisation during the transfer."),
          NULL, NULL, GAME_MIN_TECHLOST_DONOR, GAME_MAX_TECHLOST_DONOR,
          GAME_DEFAULT_TECHLOST_DONOR)

  GEN_INT("diplcost", game.server.diplcost,
	  SSET_RULES, SSET_SCIENCE, SSET_RARE, SSET_TO_CLIENT,
	  N_("Penalty when getting tech or gold from treaty"),
	  N_("For each technology you gain from a diplomatic treaty, you "
	     "lose research points equal to this percentage of the cost to "
	     "research a new technology. If this is non-zero, you can end up "
	     "with negative research points. Also applies to gold "
             "transfers in diplomatic treaties."),
          NULL, NULL,
	  GAME_MIN_DIPLCOST, GAME_MAX_DIPLCOST, GAME_DEFAULT_DIPLCOST)

  GEN_INT("conquercost", game.server.conquercost,
	  SSET_RULES, SSET_SCIENCE, SSET_RARE, SSET_TO_CLIENT,
	  N_("Penalty when getting tech from conquering"),
	  N_("For each technology you gain by conquering an enemy city, you "
	     "lose research points equal to this percentage of the cost to "
	     "research a new technology. If this is non-zero, you can end up "
	     "with negative research points."),
          NULL, NULL,
	  GAME_MIN_CONQUERCOST, GAME_MAX_CONQUERCOST,
	  GAME_DEFAULT_CONQUERCOST)

  GEN_INT("freecost", game.server.freecost,
	  SSET_RULES, SSET_SCIENCE, SSET_RARE, SSET_TO_CLIENT,
	  N_("Penalty when getting a free tech"),
	  N_("For each technology you gain \"for free\" (other than "
	     "covered by diplcost or conquercost: specifically, from huts "
	     "or from Great Library effects), you lose research points "
	     "equal to this percentage of the cost to research a new "
	     "technology. If this is non-zero, you can end up "
             "with negative research points."),
          NULL, NULL,
	  GAME_MIN_FREECOST, GAME_MAX_FREECOST, GAME_DEFAULT_FREECOST)

  GEN_INT("foodbox", game.info.foodbox,
	  SSET_RULES, SSET_ECONOMICS, SSET_SITUATIONAL, SSET_TO_CLIENT,
	  N_("Food required for a city to grow"),
	  N_("This is the base amount of food required to grow a city. "
	     "This value is multiplied by another factor that comes from "
	     "the ruleset and is dependent on the size of the city."),
          NULL, NULL,
	  GAME_MIN_FOODBOX, GAME_MAX_FOODBOX, GAME_DEFAULT_FOODBOX)

  GEN_INT("aqueductloss", game.server.aqueductloss,
	  SSET_RULES, SSET_ECONOMICS, SSET_RARE, SSET_TO_CLIENT,
	  N_("Percentage food lost when building needed"),
	  N_("If a city would expand, but it can't because it needs "
	     "an Aqueduct (or Sewer System), it loses this percentage "
	     "of its foodbox (or half that amount when it has a "
             "Granary)."), NULL, NULL,
	  GAME_MIN_AQUEDUCTLOSS, GAME_MAX_AQUEDUCTLOSS, 
	  GAME_DEFAULT_AQUEDUCTLOSS)

  GEN_INT("shieldbox", game.info.shieldbox,
	  SSET_RULES, SSET_ECONOMICS, SSET_SITUATIONAL, SSET_TO_CLIENT,
	  N_("Multiplier percentage for production costs"),
	  N_("This affects how quickly units and buildings can be "
	     "produced.  The base costs are multiplied by this value (as "
	     "a percentage)."),
          NULL, NULL, GAME_MIN_SHIELDBOX, GAME_MAX_SHIELDBOX,
	  GAME_DEFAULT_SHIELDBOX)

  /* Notradesize and fulltradesize used to have callbacks to prevent them
   * from being set illegally (notradesize > fulltradesize).  However this
   * provided a problem when setting them both through the client's settings
   * dialog, since they cannot both be set atomically.  So the callbacks were
   * removed and instead the game now knows how to deal with invalid
   * settings. */
  GEN_INT("fulltradesize", game.info.fulltradesize,
	  SSET_RULES, SSET_ECONOMICS, SSET_RARE, SSET_TO_CLIENT,
	  N_("Minimum city size to get full trade"),
	  N_("There is a trade penalty in all cities smaller than this. "
	     "The penalty is 100% (no trade at all) for sizes up to "
	     "notradesize, and decreases gradually to 0% (no penalty "
	     "except the normal corruption) for size=fulltradesize. "
             "See also notradesize."), NULL, NULL,
	  GAME_MIN_FULLTRADESIZE, GAME_MAX_FULLTRADESIZE, 
	  GAME_DEFAULT_FULLTRADESIZE)

  GEN_INT("notradesize", game.info.notradesize,
	  SSET_RULES, SSET_ECONOMICS, SSET_RARE, SSET_TO_CLIENT,
	  N_("Maximum size of a city without trade"),
	  N_("Cities do not produce any trade at all unless their size "
	     "is larger than this amount. The produced trade increases "
	     "gradually for cities larger than notradesize and smaller "
             "than fulltradesize. See also fulltradesize."), NULL, NULL,
	  GAME_MIN_NOTRADESIZE, GAME_MAX_NOTRADESIZE,
	  GAME_DEFAULT_NOTRADESIZE)

  GEN_INT("citymindist", game.info.citymindist,
	  SSET_RULES, SSET_SOCIOLOGY, SSET_SITUATIONAL, SSET_TO_CLIENT,
	  N_("Minimum distance between cities"),
	  N_("When a player attempts to found a new city, there may be "
	     "no other city in this distance. For example, when "
	     "this value is 3, there have to be at least two empty "
	     "fields between two cities in every direction. If set "
	     "to 0 (default), the ruleset value will be used."),
          NULL, NULL,
	  GAME_MIN_CITYMINDIST, GAME_MAX_CITYMINDIST,
	  GAME_DEFAULT_CITYMINDIST)

  GEN_BOOL("trading_tech", game.info.trading_tech,
           SSET_RULES, SSET_SOCIOLOGY, SSET_RULES, SSET_TO_CLIENT,
           N_("Technology trading"),
           N_("If turned off, trading technologies in diplomacy dialog "
              "is not allowed."), NULL, NULL,
           GAME_DEFAULT_TRADING_TECH)

  GEN_BOOL("trading_gold", game.info.trading_gold,
           SSET_RULES, SSET_SOCIOLOGY, SSET_RULES, SSET_TO_CLIENT,
           N_("Gold trading"),
           N_("If turned off, trading gold in diplomacy dialog "
              "is not allowed."), NULL, NULL,
           GAME_DEFAULT_TRADING_GOLD)

  GEN_BOOL("trading_city", game.info.trading_city,
           SSET_RULES, SSET_SOCIOLOGY, SSET_RULES, SSET_TO_CLIENT,
           N_("City trading"),
           N_("If turned off, trading cities in diplomacy dialog "
              "is not allowed."), NULL, NULL,
           GAME_DEFAULT_TRADING_CITY)

  GEN_INT("trademindist", game.info.trademindist,
          SSET_RULES, SSET_ECONOMICS, SSET_RARE, SSET_TO_CLIENT,
          N_("Minimum distance for trade routes"),
          N_("In order to establish a trade route, cities must be at "
             "least this far apart on the map. The distance is calculated "
             "as \"manhattan distance\", that is, the sum of the "
             "displacements along the x and y directions."), NULL, NULL,
          GAME_MIN_TRADEMINDIST, GAME_MAX_TRADEMINDIST,
          GAME_DEFAULT_TRADEMINDIST)

  GEN_INT("rapturedelay", game.info.rapturedelay,
	  SSET_RULES, SSET_SOCIOLOGY, SSET_SITUATIONAL, SSET_TO_CLIENT,
          N_("Number of turns between rapture effect"),
          N_("Sets the number of turns between rapture growth of a city. "
             "If set to n a city will grow after celebrating for n+1 "
             "turns."),
          NULL, NULL,
          GAME_MIN_RAPTUREDELAY, GAME_MAX_RAPTUREDELAY,
          GAME_DEFAULT_RAPTUREDELAY)

  GEN_INT("razechance", game.server.razechance,
	  SSET_RULES, SSET_MILITARY, SSET_RARE, SSET_TO_CLIENT,
	  N_("Chance for conquered building destruction"),
	  N_("When a player conquers a city, each city improvement has this "
             "percentage chance to be destroyed."), NULL, NULL,
	  GAME_MIN_RAZECHANCE, GAME_MAX_RAZECHANCE, GAME_DEFAULT_RAZECHANCE)

  GEN_INT("occupychance", game.server.occupychance,
	  SSET_RULES, SSET_MILITARY, SSET_RARE, SSET_TO_CLIENT,
	  N_("Chance of moving into tile after attack"),
	  N_("If set to 0, combat is Civ1/2-style (when you attack, "
	     "you remain in place). If set to 100, attacking units "
	     "will always move into the tile they attacked when they win "
	     "the combat (and no enemy units remain in the tile). If "
	     "set to a value between 0 and 100, this will be used as "
             "the percent chance of \"occupying\" territory."), NULL, NULL,
	  GAME_MIN_OCCUPYCHANCE, GAME_MAX_OCCUPYCHANCE, 
	  GAME_DEFAULT_OCCUPYCHANCE)

  GEN_BOOL("autoattack", game.server.autoattack, SSET_RULES_FLEXIBLE, SSET_MILITARY,
         SSET_SITUATIONAL, SSET_TO_CLIENT,
         N_("Turn on/off server-side autoattack"),
         N_("If set to on, units with move left will automatically "
            "consider attacking enemy units that move adjacent to them."),
         NULL, NULL, GAME_DEFAULT_AUTOATTACK)

  GEN_INT("killcitizen", game.info.killcitizen,
          SSET_RULES, SSET_MILITARY, SSET_RARE, SSET_TO_CLIENT,
          N_("Reduce city population after attack"),
          N_("This flag indicates whether city population is reduced "
             "after successful attack of enemy unit, depending on "
             "its movement type (OR-ed):\n"
             "  1 = land moving units\n"
             "  2 = sea moving units\n"
             "  4 = units able to move both on land and sea"), NULL, NULL,
          GAME_MIN_KILLCITIZEN, GAME_MAX_KILLCITIZEN,
          GAME_DEFAULT_KILLCITIZEN)

  GEN_INT("killunhomed", game.server.killunhomed,
          SSET_RULES, SSET_MILITARY, SSET_RARE, SSET_TO_CLIENT,
          N_("Slowly kill unhomecitied units (eg. startunits)"),
          N_("If greater than 0, then every unit without a homecity will "
             "lose hitpoints each turn. The number of hitpoints lost is "
             "given by 'killunhomed' percent of the hitpoints of the unit "
             "type. At least one hitpoint is lost every turn until the "
             "death of the unit."),
          NULL, NULL, GAME_MIN_KILLUNHOMED, GAME_MAX_KILLUNHOMED,
          GAME_DEFAULT_KILLUNHOMED)

  GEN_ENUM("borders", game.info.borders,
           SSET_RULES, SSET_MILITARY, SSET_SITUATIONAL, SSET_TO_CLIENT,
           N_("National borders"),
           N_("If this is not disabled, then any land tiles around a "
              "fortress or city will be owned by that nation."),
           NULL, NULL, borders_name, GAME_DEFAULT_BORDERS)

  GEN_BOOL("happyborders", game.info.happyborders,
	   SSET_RULES, SSET_MILITARY, SSET_SITUATIONAL,
	   SSET_TO_CLIENT,
	   N_("Units inside borders cause no unhappiness"),
	   N_("If this is set, units will not cause unhappiness when "
              "inside your own borders."), NULL, NULL,
	   GAME_DEFAULT_HAPPYBORDERS)

  GEN_ENUM("diplomacy", game.info.diplomacy,
           SSET_RULES, SSET_MILITARY, SSET_SITUATIONAL, SSET_TO_CLIENT,
           N_("Ability to do diplomacy with other players"),
           N_("This setting controls the ability to do diplomacy with "
              "other players."),
           NULL, NULL, diplomacy_name, GAME_DEFAULT_DIPLOMACY)

  GEN_ENUM("citynames", game.server.allowed_city_names,
           SSET_RULES, SSET_SOCIOLOGY, SSET_RARE, SSET_TO_CLIENT,
           N_("Allowed city names"),
           N_("- \"No restrictions\": players can have multiple cities with "
              "the same names.\n"
              "- \"Unique to a player\": one player can't have multiple "
              "cities with the same name.\n"
              "- \"Globally unique\": all cities in a game have to have "
              "different names.\n"
              "- \"No city name stealing\": like \"Globally unique\", but a "
              "player isn't allowed to use a default city name of another "
              "nations unless it is a default for their nation also."),
           NULL, NULL, cityname_name, GAME_DEFAULT_ALLOWED_CITY_NAMES)

  /* Flexible rules: these can be changed after the game has started.
   *
   * The distinction between "rules" and "flexible rules" is not always
   * clearcut, and some existing cases may be largely historical or
   * accidental.  However some generalizations can be made:
   *
   *   -- Low-level game mechanics should not be flexible (eg, rulesets).
   *   -- Options which would affect the game "state" (city production etc)
   *      should not be flexible (eg, foodbox).
   *   -- Options which are explicitly sent to the client (eg, in
   *      packet_game_info) should probably not be flexible, or at
   *      least need extra care to be flexible.
   */
  GEN_ENUM("barbarians", game.server.barbarianrate,
           SSET_RULES_FLEXIBLE, SSET_MILITARY, SSET_VITAL, SSET_TO_CLIENT,
           N_("Barbarian appearance frequency"),
           N_("This setting controls how frequently the barbarians appears "
              "in the game. See also the \"onsetbarbs\" setting."),
           NULL, NULL, barbarians_name, GAME_DEFAULT_BARBARIANRATE)

  GEN_INT("onsetbarbs", game.server.onsetbarbarian,
	  SSET_RULES_FLEXIBLE, SSET_MILITARY, SSET_VITAL, SSET_TO_CLIENT,
	  N_("Barbarian onset turn"),
          N_("Barbarians will not appear before this turn."), NULL, NULL,
	  GAME_MIN_ONSETBARBARIAN, GAME_MAX_ONSETBARBARIAN, 
	  GAME_DEFAULT_ONSETBARBARIAN)

  GEN_INT("revolen", game.server.revolution_length,
	  SSET_RULES_FLEXIBLE, SSET_SOCIOLOGY, SSET_RARE, SSET_TO_CLIENT,
	  N_("Length in turns of revolution"),
	  N_("When changing governments, a period of anarchy lasting this "
	     "many turns will occur. "
             "Setting this value to 0 will give a random "
             "length of 1-6 turns."), NULL, NULL,
	  GAME_MIN_REVOLUTION_LENGTH, GAME_MAX_REVOLUTION_LENGTH, 
	  GAME_DEFAULT_REVOLUTION_LENGTH)

  GEN_BOOL("fogofwar", game.info.fogofwar,
	   SSET_RULES, SSET_MILITARY, SSET_RARE, SSET_TO_CLIENT,
	   N_("Whether to enable fog of war"),
	   N_("If this is set to 1, only those units and cities within "
	      "the vision range of your own units and cities will be "
	      "revealed to you. You will not see new cities or terrain "
              "changes in tiles not observed."), NULL, NULL,
	   GAME_DEFAULT_FOGOFWAR)

  GEN_BOOL("foggedborders", game.server.foggedborders,
           SSET_RULES, SSET_MILITARY, SSET_RARE, SSET_TO_CLIENT,
           N_("Whether border changes are seen through fog of war"),
           N_("If this setting is enabled, players will not be able "
              "to see changes in tile ownership if they do not have "
              "direct sight of the affected tiles. Otherwise, players "
              "can see any or all changes to borders as long as they "
              "have previously seen the tiles."), NULL, NULL,
           GAME_DEFAULT_FOGGEDBORDERS)

  GEN_INT("airliftingstyle", game.info.airlifting_style,
          SSET_RULES_FLEXIBLE, SSET_MILITARY, SSET_SITUATIONAL,
          SSET_TO_CLIENT, N_("Airlifting style"),
          N_("This settings affects airlifting units between cities. "
             "The value of this setting is a OR-ed value from:\n"
             "  0 = Like classical Freeciv, only as many units per turn "
             "to source or to destination the ruleset allows between your "
             "own cities.\n"
             "  1 = Allows units to be airlifted from allied cities.\n"
             "  2 = Allows units to be airlifted to allied cities.\n"
             "  4 = Unlimited units from source. Airlifting from a city "
             "doesn't reduce the airlifted counter, but still needs at "
             "least 1.\n"
             "  8 = Unlimited units to destination. Airlifting to a city "
             "doesn't reduce the airlifted counter, and doesn't need any."),
          NULL, NULL, GAME_MIN_AIRLIFTINGSTYLE, GAME_MAX_AIRLIFTINGSTYLE,
          GAME_DEFAULT_AIRLIFTINGSTYLE)

  GEN_INT("diplchance", game.server.diplchance,
	  SSET_RULES_FLEXIBLE, SSET_MILITARY, SSET_SITUATIONAL, SSET_TO_CLIENT,
	  N_("Base chance for diplomats and spies to succeed."),
	  /* xgettext:no-c-format */
	  N_("The chance of a spy returning from a successful mission and "
	     "the base chance of success for diplomats and spies."),
          NULL, NULL,
	  GAME_MIN_DIPLCHANCE, GAME_MAX_DIPLCHANCE, GAME_DEFAULT_DIPLCHANCE)

  GEN_BOOL("spacerace", game.info.spacerace,
	   SSET_RULES_FLEXIBLE, SSET_SCIENCE, SSET_VITAL, SSET_TO_CLIENT,
	   N_("Whether to allow space race"),
	   N_("If this option is set to 1, players can build spaceships."),
           NULL, NULL,
	   GAME_DEFAULT_SPACERACE)

  GEN_BOOL("endspaceship", game.server.endspaceship, SSET_RULES_FLEXIBLE,
           SSET_SCIENCE, SSET_VITAL, SSET_TO_CLIENT,
           N_("Should the game end if the spaceship arrives?"),
           N_("If this option is set to 1, the game will end with the "
              "arrival of a spaceship at Alpha Centauri."), NULL, NULL,
           GAME_DEFAULT_END_SPACESHIP)

  GEN_INT("civilwarsize", game.server.civilwarsize,
	  SSET_RULES_FLEXIBLE, SSET_SOCIOLOGY, SSET_RARE, SSET_TO_CLIENT,
	  N_("Minimum number of cities for civil war"),
	  N_("A civil war is triggered when a player has at least this "
	     "many cities and the player's capital is captured. If "
	     "this option is set to the maximum value, civil wars are "
             "turned off altogether."), NULL, NULL,
	  GAME_MIN_CIVILWARSIZE, GAME_MAX_CIVILWARSIZE, 
	  GAME_DEFAULT_CIVILWARSIZE)

  GEN_BOOL("restrictinfra", game.info.restrictinfra,
           SSET_RULES_FLEXIBLE, SSET_MILITARY, SSET_RARE, SSET_TO_CLIENT,
           N_("Restrict the use of the infrastructure for enemy units"),
           N_("If this option is set to 1, the use of roads and rails "
              "will be restricted for enemy units."), NULL, NULL,
           GAME_DEFAULT_RESTRICTINFRA)

  GEN_INT("contactturns", game.server.contactturns,
	  SSET_RULES_FLEXIBLE, SSET_MILITARY, SSET_RARE, SSET_TO_CLIENT,
	  N_("Turns until player contact is lost"),
	  N_("Players may meet for diplomacy this number of turns "
	     "after their units have last met, even when they do not have "
	     "an embassy. If set to zero, then players cannot meet unless "
	     "they have an embassy."),
          NULL, NULL,
	  GAME_MIN_CONTACTTURNS, GAME_MAX_CONTACTTURNS, 
	  GAME_DEFAULT_CONTACTTURNS)

  GEN_BOOL("savepalace", game.server.savepalace,
	   SSET_RULES_FLEXIBLE, SSET_MILITARY, SSET_RARE, SSET_TO_CLIENT,
	   N_("Rebuild palace whenever capital is conquered"),
	   N_("If this is set to 1, when the capital is conquered the "
	      "palace is automatically rebuilt for free in another randomly "
	      "choosen city. This is significant because the technology "
	      "requirement for building a palace will be ignored."),
           NULL, NULL,
	   GAME_DEFAULT_SAVEPALACE)

  GEN_BOOL("naturalcitynames", game.server.natural_city_names,
           SSET_RULES_FLEXIBLE, SSET_SOCIOLOGY, SSET_RARE, SSET_TO_CLIENT,
           N_("Whether to use natural city names"),
           N_("If enabled, the default city names will be determined based "
              "on the surrounding terrain."),
           NULL, NULL, GAME_DEFAULT_NATURALCITYNAMES)

  GEN_BOOL("migration", game.server.migration,
           SSET_RULES_FLEXIBLE, SSET_SOCIOLOGY, SSET_RARE, SSET_TO_CLIENT,
           N_("Whether to enable citizen migration"),
           N_("This is the master setting that controls whether citizen "
              "migration is active in the game. If enabled, citizens may "
              "automatically move from less desirable cities to more "
              "desirable ones. The \"desirability\" of a given city is "
              "calculated from a number of factors. In general larger "
              "cities with more income and improvements will be preferred. "
              "Citizens will never migrate out of the capital, or cause "
              "a wonder to be lost by disbanding a city. A number of other "
              "settings control how migration behaves:\n"
              "  mgr_turninterval - How often citizens try to migrate.\n"
              "  mgr_foodneeded   - Whether destination food is checked.\n"
              "  mgr_distance     - How far citizens will migrate.\n"
              "  mgr_worldchance  - Chance for inter-nation migration.\n"
              "  mgr_nationchance - Chance for intra-nation migration."),
           NULL, NULL, GAME_DEFAULT_MIGRATION)

  GEN_INT("mgr_turninterval", game.server.mgr_turninterval,
          SSET_RULES_FLEXIBLE, SSET_SOCIOLOGY, SSET_RARE, SSET_TO_CLIENT,
          N_("Number of turns between migrations from a city"),
          N_("This setting controls the number of turns between migration "
             "checks for a given city. The interval is calculated from "
             "the founding turn of the city. So for example if this "
             "setting is 5, citizens will look for a suitable migration "
             "destination every five turns from the founding of their "
             "current city. Migration will never occur the same turn "
             "that a city is built. This setting has no effect unless "
             "migration is enabled by the 'migration' setting."), NULL,
          NULL, GAME_MIN_MGR_TURNINTERVAL, GAME_MAX_MGR_TURNINTERVAL,
          GAME_DEFAULT_MGR_TURNINTERVAL)

  GEN_BOOL("mgr_foodneeded", game.server.mgr_foodneeded,
          SSET_RULES_FLEXIBLE, SSET_SOCIOLOGY, SSET_RARE, SSET_TO_CLIENT,
           N_("Whether migration is limited by food"),
           N_("If this setting is enabled, citizens will not migrate to "
              "cities which would not have enough food to support them. "
              "This setting has no effect unless migration is enabled by "
              "the 'migration' setting."), NULL, NULL,
           GAME_DEFAULT_MGR_FOODNEEDED)

  GEN_INT("mgr_distance", game.server.mgr_distance,
          SSET_RULES_FLEXIBLE, SSET_SOCIOLOGY, SSET_RARE, SSET_TO_CLIENT,
          N_("Maximum distance citizens may migrate"),
          N_("This setting controls how far citizens may look for a "
             "suitable migration destination when deciding which city "
             "to migrate to. The value is added to the current city radius "
             "and compared to the distance between the two cities. If "
             "the distance is lower or equal, migration is possible. This "
             "setting has no effect unless migration is activated by the "
             "'migration' setting."),
          NULL, NULL, GAME_MIN_MGR_DISTANCE, GAME_MAX_MGR_DISTANCE,
          GAME_DEFAULT_MGR_DISTANCE)

  GEN_INT("mgr_nationchance", game.server.mgr_nationchance,
          SSET_RULES_FLEXIBLE, SSET_SOCIOLOGY, SSET_RARE, SSET_TO_CLIENT,
          N_("Percent probability for migration within the same nation"),
          N_("This setting controls how likely it is for citizens to "
             "migrate between cities owned by the same player. Zero "
             "indicates migration will never occur, 100 means that "
             "migration will always occur if the citizens find a suitable "
             "destination. This setting has no effect unless migration "
             "is activated by the 'migration' setting."), NULL, NULL,
          GAME_MIN_MGR_NATIONCHANCE, GAME_MAX_MGR_NATIONCHANCE,
          GAME_DEFAULT_MGR_NATIONCHANCE)

  GEN_INT("mgr_worldchance", game.server.mgr_worldchance,
          SSET_RULES_FLEXIBLE, SSET_SOCIOLOGY, SSET_RARE, SSET_TO_CLIENT,
          N_("Percent probability for migration between foreign cities"),
          N_("This setting controls how likely it is for migration "
             "to occur between cities owned by different players. "
             "Zero indicates migration will never occur, 100 means "
             "that citizens will always migrate if they find a suitable "
             "destination. This setting has no effect if migration is "
             "not enabled by the 'migration' setting."), NULL, NULL,
          GAME_MIN_MGR_WORLDCHANCE, GAME_MAX_MGR_WORLDCHANCE,
          GAME_DEFAULT_MGR_WORLDCHANCE)

  /* Meta options: these don't affect the internal rules of the game, but
   * do affect players.  Also options which only produce extra server
   * "output" and don't affect the actual game.
   * ("endturn" is here, and not RULES_FLEXIBLE, because it doesn't
   * affect what happens in the game, it just determines when the
   * players stop playing and look at the score.)
   */
  GEN_STRING("allowtake", game.server.allow_take,
             SSET_META, SSET_NETWORK, SSET_RARE, SSET_TO_CLIENT,
             N_("Players that users are allowed to take"),
             N_("This should be a string of characters, each of which "
                "specifies a type or status of a civilization (player).\n"
                "Clients will only be permitted to take or observe those "
                "players which match one of the specified letters. This "
                "only affects future uses of the take or observe command; "
                "it is not retroactive. The characters and their meanings "
                "are:\n"
                "    o,O = Global observer\n"
                "    b   = Barbarian players\n"
                "    d   = Dead players\n"
                "    a,A = AI players\n"
                "    h,H = Human players\n"
                "The first description on this list which matches a "
                "player is the one which applies. Thus 'd' does not "
                "include dead barbarians, 'a' does not include dead AI "
                "players, and so on. Upper case letters apply before "
                "the game has started, lower case letters afterwards.\n"
                "Each character above may be followed by one of the "
                "following numbers to allow or restrict the manner "
                "of connection:\n"
                "(none) = Controller allowed, observers allowed, "
                "can displace connections. (Displacing a connection means "
                "that you may take over a player, even when another user "
                "already controls that player.)\n"
                "     1 = Controller allowed, observers allowed, "
                "can't displace connections;\n"
                "     2 = Controller allowed, no observers allowed, "
                "can displace connections;\n"
                "     3 = Controller allowed, no observers allowed, "
                "can't displace connections;\n"
                "     4 = No controller allowed, observers allowed"),
             allowtake_callback, NULL, GAME_DEFAULT_ALLOW_TAKE)

  GEN_BOOL("autotoggle", game.server.auto_ai_toggle,
	   SSET_META, SSET_NETWORK, SSET_SITUATIONAL, SSET_TO_CLIENT,
	   N_("Whether AI-status toggles with connection"),
	   N_("If this is set to 1, AI status is turned off when a player "
	      "connects, and on when a player disconnects."),
           NULL, autotoggle_action, GAME_DEFAULT_AUTO_AI_TOGGLE)

  GEN_INT("endturn", game.server.end_turn,
	  SSET_META, SSET_SOCIOLOGY, SSET_VITAL, SSET_TO_CLIENT,
	  N_("Turn the game ends"),
          N_("The game will end at the end of the given turn."),
          endturn_callback, NULL,
          GAME_MIN_END_TURN, GAME_MAX_END_TURN, GAME_DEFAULT_END_TURN)

  GEN_INT("timeout", game.info.timeout,
	  SSET_META, SSET_INTERNAL, SSET_VITAL, SSET_TO_CLIENT,
	  N_("Maximum seconds per turn"),
	  N_("If all players have not hit \"Turn Done\" before this "
	     "time is up, then the turn ends automatically. Zero "
	     "means there is no timeout. In servers compiled with "
             "debugging, a timeout of -1 sets the autogame test mode. "
             "Only connections with hack level access may set the "
             "timeout to lower than 30 seconds. Use this with the "
             "command \"timeoutincrease\" to have a dynamic timer."),
          timeout_callback, NULL,
          GAME_MIN_TIMEOUT, GAME_MAX_TIMEOUT, GAME_DEFAULT_TIMEOUT)

  GEN_INT("timeaddenemymove", game.server.timeoutaddenemymove,
	  SSET_META, SSET_INTERNAL, SSET_VITAL, SSET_TO_CLIENT,
	  N_("Timeout at least n seconds when enemy moved"),
	  N_("Any time a unit moves while in sight of an enemy player, "
	     "the remaining timeout is increased to this value."),
          NULL, NULL, 0, GAME_MAX_TIMEOUT, GAME_DEFAULT_TIMEOUTADDEMOVE)

  GEN_INT("unitwaittime", game.server.unitwaittime,
          SSET_RULES_FLEXIBLE, SSET_INTERNAL, SSET_VITAL, SSET_TO_CLIENT,
          N_("Time between unit moves over turn change"),
          N_("This setting gives the minimum amount of time in seconds "
             "between unit moves after a turn change occurs. For "
             "example, if this setting is set to 20 and a unit moves "
             "5 seconds before the turn change, it will not be able "
             "to move in the next turn for at least 15 seconds. Building "
             "cities is also affected by this setting, as well as units "
             "moving inside a transporter. This value is limited to "
             "a maximum value of 2/3 'timeout'."),
          unitwaittime_callback, NULL, GAME_MIN_UNITWAITTIME,
          GAME_MAX_UNITWAITTIME, GAME_DEFAULT_UNITWAITTIME)

  /* This setting points to the "stored" value; changing it won't have
   * an effect until the next synchronization point (i.e., the start of
   * the next turn). */
  GEN_ENUM("phasemode", game.server.phase_mode_stored,
           SSET_META, SSET_INTERNAL, SSET_SITUATIONAL, SSET_TO_CLIENT,
           N_("Control of simultaneous player/team phases."),
           N_("This setting controls whether players may make "
              "moves at the same time during a turn."),
           phasemode_callback, NULL, phasemode_name, GAME_DEFAULT_PHASE_MODE)

  GEN_INT("nettimeout", game.server.tcptimeout,
	  SSET_META, SSET_NETWORK, SSET_RARE, SSET_TO_CLIENT,
	  N_("Seconds to let a client's network connection block"),
	  N_("If a network connection is blocking for a time greater than "
	     "this value, then the connection is closed. Zero "
	     "means there is no timeout (although connections will be "
	     "automatically disconnected eventually)."),
          NULL, NULL,
	  GAME_MIN_TCPTIMEOUT, GAME_MAX_TCPTIMEOUT, GAME_DEFAULT_TCPTIMEOUT)

  GEN_INT("netwait", game.server.netwait,
	  SSET_META, SSET_NETWORK, SSET_RARE, SSET_TO_CLIENT,
	  N_("Max seconds for network buffers to drain"),
	  N_("The server will wait for up to the value of this "
	     "parameter in seconds, for all client connection network "
	     "buffers to unblock. Zero means the server will not "
             "wait at all."), NULL, NULL,
	  GAME_MIN_NETWAIT, GAME_MAX_NETWAIT, GAME_DEFAULT_NETWAIT)

  GEN_INT("pingtime", game.server.pingtime,
	  SSET_META, SSET_NETWORK, SSET_RARE, SSET_TO_CLIENT,
	  N_("Seconds between PINGs"),
	  N_("The civserver will poll the clients with a PING request "
             "each time this period elapses."), NULL, NULL,
	  GAME_MIN_PINGTIME, GAME_MAX_PINGTIME, GAME_DEFAULT_PINGTIME)

  GEN_INT("pingtimeout", game.server.pingtimeout,
	  SSET_META, SSET_NETWORK, SSET_RARE,
          SSET_TO_CLIENT,
	  N_("Time to cut a client"),
	  N_("If a client doesn't reply to a PING in this time the "
             "client is disconnected."), NULL, NULL,
	  GAME_MIN_PINGTIMEOUT, GAME_MAX_PINGTIMEOUT, GAME_DEFAULT_PINGTIMEOUT)

  GEN_BOOL("turnblock", game.server.turnblock,
	   SSET_META, SSET_INTERNAL, SSET_SITUATIONAL, SSET_TO_CLIENT,
	   N_("Turn-blocking game play mode"),
	   N_("If this is set to 1 the game turn is not advanced "
	      "until all players have finished their turn, including "
              "disconnected players."), NULL, NULL,
	   GAME_DEFAULT_TURNBLOCK)

  GEN_BOOL("fixedlength", game.server.fixedlength,
	   SSET_META, SSET_INTERNAL, SSET_SITUATIONAL, SSET_TO_CLIENT,
	   N_("Fixed-length turns play mode"),
	   N_("If this is set to 1 the game turn will not advance "
	      "until the timeout has expired, even after all players "
              "have clicked on \"Turn Done\"."), NULL, NULL,
	   FALSE)

  GEN_STRING("demography", game.server.demography,
	     SSET_META, SSET_INTERNAL, SSET_SITUATIONAL, SSET_TO_CLIENT,
	     N_("What is in the Demographics report"),
	     N_("This should be a string of characters, each of which "
		"specifies the inclusion of a line of information "
		"in the Demographics report.\n"
		"The characters and their meanings are:\n"
		"    N = include Population\n"
		"    P = include Production\n"
		"    A = include Land Area\n"
		"    L = include Literacy\n"
		"    R = include Research Speed\n"
		"    S = include Settled Area\n"
		"    E = include Economics\n"
		"    M = include Military Service\n"
		"    O = include Pollution\n"
		"Additionally, the following characters control whether "
		"or not certain columns are displayed in the report:\n"
		"    q = display \"quantity\" column\n"
		"    r = display \"rank\" column\n"
		"    b = display \"best nation\" column\n"
		"The order of characters is not significant, but "
		"their capitalization is."),
             demography_callback, NULL, GAME_DEFAULT_DEMOGRAPHY)

  GEN_INT("saveturns", game.server.save_nturns,
	  SSET_META, SSET_INTERNAL, SSET_VITAL, SSET_SERVER_ONLY,
	  N_("Turns per auto-save"),
	  N_("The game will be automatically saved per this number of "
             "turns. Zero means never auto-save."), NULL, NULL,
          GAME_MIN_SAVETURNS, GAME_MAX_SAVETURNS, GAME_DEFAULT_SAVETURNS)

  GEN_INT("compress", game.server.save_compress_level,
	  SSET_META, SSET_INTERNAL, SSET_RARE, SSET_SERVER_ONLY,
	  N_("Savegame compression level"),
	  N_("If non-zero, saved games will be compressed using zlib "
	     "(gzip format) or bzip2. Larger values will give better "
             "compression but take longer."), NULL, NULL,
	  GAME_MIN_COMPRESS_LEVEL, GAME_MAX_COMPRESS_LEVEL,
	  GAME_DEFAULT_COMPRESS_LEVEL)

  GEN_INT("compresstype", game.server.save_compress_type,
          SSET_META, SSET_INTERNAL, SSET_RARE, SSET_SERVER_ONLY,
          N_("Savegame compression algorithm"),
          N_("Compression library to use for savegames.\n"
             " 0 - none\n"
             " 1 - zlib (gzip format)\n"
             " 2 - bzip2\n"
             "Not all servers support all compression methods."), NULL,
          NULL, GAME_MIN_COMPRESS_TYPE, GAME_MAX_COMPRESS_TYPE,
	  GAME_DEFAULT_COMPRESS_TYPE)

  GEN_STRING("savename", game.server.save_name,
             SSET_META, SSET_INTERNAL, SSET_VITAL, SSET_SERVER_ONLY,
             N_("Definition of the save file name"),
             N_("Within the string the following custom formats are "
                "allowed:\n"
                "  %R = <reason>\n"
                "  %S = <suffix>\n"
                "  %T = <game.info.turn>\n"
                "  %Y = <game.info.year>\n"
                "\n"
                "Example: 'civgame-T%04T-Y%+05Y-%R' => "
                "'civgame-T0100-Y00001-manual'\n"
                "\n"
                "Be careful to use at least one of %T and %Y, else newer "
                "savegames will overwrite old ones. If none of the formats "
                "is used '-T%04T-Y%05Y-%R' is appended to the value of "
                "'savename'."),
             NULL, NULL, GAME_DEFAULT_SAVE_NAME)

  GEN_BOOL("scorelog", game.server.scorelog,
	   SSET_META, SSET_INTERNAL, SSET_SITUATIONAL, SSET_SERVER_ONLY,
	   N_("Whether to log player statistics"),
	   N_("If this is set to 1, player statistics are appended to "
	      "the file \"civscore.log\" every turn. These statistics "
	      "can be used to create power graphs after the game."), NULL,
           NULL, GAME_DEFAULT_SCORELOG)

  GEN_END
};

#undef GEN_BOOL
#undef GEN_INT
#undef GEN_STRING
#undef GEN_ENUM
#undef GEN_END

/* The number of settings, not including the END. */
const int SETTINGS_NUM = ARRAY_SIZE(settings) - 1;

/****************************************************************************
  Returns the setting to the given id.
****************************************************************************/
struct setting *setting_by_number(int id)
{
  fc_assert_ret_val(0 <= id && id < SETTINGS_NUM, NULL);
  return settings + id;
}

/****************************************************************************
  Returns the id to the given setting.
****************************************************************************/
int setting_number(const struct setting *pset)
{
  fc_assert_ret_val(pset != NULL, -1);
  return pset - settings;
}

/****************************************************************************
  Access function for the setting name.
****************************************************************************/
const char *setting_name(const struct setting *pset)
{
  return pset->name;
}

/****************************************************************************
  Access function for the short help (not translated yet) of the setting.
****************************************************************************/
const char *setting_short_help(const struct setting *pset)
{
  return pset->short_help;
}

/****************************************************************************
  Access function for the long (extra) help (not translated yet) of
  the setting.
****************************************************************************/
const char *setting_extra_help(const struct setting *pset)
{
  return pset->extra_help;
}

/****************************************************************************
  Access function for the setting type.
****************************************************************************/
enum sset_type setting_type(const struct setting *pset)
{
  return pset->stype;
}

/****************************************************************************
  Access function for the setting level (used by the /show command).
****************************************************************************/
enum sset_level setting_level(const struct setting *pset)
{
  return pset->slevel;
}

/****************************************************************************
  Access function for the setting category name.
****************************************************************************/
const char *setting_category_name(const struct setting *pset)
{
  return sset_category_names[pset->scategory];
}

/****************************************************************************
  Access function for the setting level name.
****************************************************************************/
const char *setting_level_name(const struct setting *pset)
{
  return sset_level_names[pset->slevel];
}

/****************************************************************************
  Returns whether the specified server setting (option) can currently
  be changed by the caller. If it returns FALSE, the reason of the failure
  is available by the function setting_error().
****************************************************************************/
bool setting_is_changeable(const struct setting *pset,
                           struct connection *caller, char *reject_msg,
                           size_t reject_msg_len)
{
  if (caller
      && (caller->access_level < ALLOW_BASIC
          || (caller->access_level < ALLOW_HACK && !pset->to_client))) {
    settings_snprintf(reject_msg, reject_msg_len,
                      _("You are not allowed to change the setting '%s'."),
                      setting_name(pset));
    return FALSE;
  }

  if (setting_locked(pset)) {
    /* setting is locked by the ruleset */
    settings_snprintf(reject_msg, reject_msg_len,
                      _("The setting '%s' is locked by the ruleset."),
                      setting_name(pset));
    return FALSE;
  }

  switch (pset->sclass) {
  case SSET_MAP_SIZE:
  case SSET_MAP_GEN:
    /* Only change map options if we don't yet have a map: */
    if (map_is_empty()) {
      return TRUE;
    }

    settings_snprintf(reject_msg, reject_msg_len,
                      _("The setting '%s' can't be modified after the map "
                        "is fixed."), setting_name(pset));
    return FALSE;

  case SSET_MAP_ADD:
  case SSET_PLAYERS:
  case SSET_GAME_INIT:
  case SSET_RULES:
    /* Only change start params and most rules if we don't yet have a map,
     * or if we do have a map but its a scenario one (ie, the game has
     * never actually been started).
     */
    if (map_is_empty() || game.info.is_new_game) {
      return TRUE;
    }

    settings_snprintf(reject_msg, reject_msg_len,
                      _("The setting '%s' can't be modified after the game "
                        "has started."), setting_name(pset));
    return FALSE;

  case SSET_RULES_FLEXIBLE:
  case SSET_META:
    /* These can always be changed: */
    return TRUE;

  case SSET_LAST:
    break;
  }

  log_error("Wrong class variant for setting %s (%d): %d.",
            setting_name(pset), setting_number(pset), pset->sclass);
  settings_snprintf(reject_msg, reject_msg_len, _("Internal error."));

  return FALSE;
}

/****************************************************************************
  Returns whether the specified server setting (option) can be seen by the
  caller.
****************************************************************************/
bool setting_is_visible(const struct setting *pset,
                        struct connection *caller)
{
  return (!caller
          || pset->to_client
          || caller->access_level >= ALLOW_HACK);
}

/****************************************************************************
  Convert a boolean string value to a bool. Returns TRUE if successful.
****************************************************************************/
bool setting_bool_str_to_bool(const struct setting *pset, const char *val,
                              bool *bval)
{
  if (0 == fc_strcasecmp(val, pset->enumerator.name(FALSE))) {
    *bval = FALSE;
    return TRUE;
  } else if (0 == fc_strcasecmp(val, pset->enumerator.name(TRUE))) {
    *bval = TRUE;
    return TRUE;
  }

  return FALSE;
}

/****************************************************************************
  Returns the current boolean value.
****************************************************************************/
bool setting_bool_get(const struct setting *pset)
{
  fc_assert_ret_val(pset->stype == SSET_BOOL, FALSE);
  return *pset->boolean.pvalue;
}

/****************************************************************************
  Returns the current boolean value as string.
****************************************************************************/
const char *setting_bool_get_str(const struct setting *pset)
{
  fc_assert_ret_val(pset->stype == SSET_BOOL, FALSE);
  return bool_name(*pset->boolean.pvalue);
}

/****************************************************************************
  Returns the default boolean value for this setting.
****************************************************************************/
bool setting_bool_def(const struct setting *pset)
{
  fc_assert_ret_val(pset->stype == SSET_BOOL, FALSE);
  return pset->boolean.default_value;
}

/****************************************************************************
  Returns the default boolean value for this setting as string.
****************************************************************************/
const char *setting_bool_def_str(const struct setting *pset)
{
  fc_assert_ret_val(pset->stype == SSET_BOOL, FALSE);
  return bool_name(pset->boolean.default_value);
}

/****************************************************************************
  Set the setting to 'val'. Returns TRUE on success. If it fails, the
  reason of the failure is available by the function setting_error().
****************************************************************************/
bool setting_bool_set(struct setting *pset, bool val,
                      struct connection *caller, char *reject_msg,
                      size_t reject_msg_len)
{
  if (!setting_is_changeable(pset, caller, reject_msg, reject_msg_len)
      || !setting_bool_validate(pset, val, caller, reject_msg,
                                reject_msg_len)) {
    return FALSE;
  }

  *pset->boolean.pvalue = val;
  return TRUE;
}

/****************************************************************************
  Set the setting to 'val'. Returns TRUE on success. If it fails, the
  reason of the failure is available by the function setting_error().
****************************************************************************/
bool setting_bool_set_str(struct setting *pset, const char *val,
                          struct connection *caller, char *reject_msg,
                          size_t reject_msg_len)
{
  bool bval;

  if (!setting_is_changeable(pset, caller, reject_msg, reject_msg_len)
      || !setting_bool_validate_str(pset, val, caller, reject_msg,
                                    reject_msg_len)) {
    return FALSE;
  }

  setting_bool_str_to_bool(pset, val, &bval);
  *pset->boolean.pvalue = bval;
  return TRUE;
}

/****************************************************************************
  Returns TRUE if 'val' is a valid value for this setting. If it's not,
  the reason of the failure is available by the function setting_error().

  FIXME: also check the access level of pconn.
****************************************************************************/
bool setting_bool_validate(const struct setting *pset, bool val,
                           struct connection *caller, char *reject_msg,
                           size_t reject_msg_len)
{
  if (SSET_BOOL != pset->stype) {
    settings_snprintf(reject_msg, reject_msg_len,
                      _("This setting is not a boolean."));
    return FALSE;
  }

  return (!pset->boolean.validate
          || pset->boolean.validate(val, caller, reject_msg,
                                    reject_msg_len));
}

/****************************************************************************
  Returns TRUE if 'val' is a valid value for this setting. If it's not,
  the reason of the failure is available by the function setting_error().

  FIXME: also check the access level of pconn.
****************************************************************************/
bool setting_bool_validate_str(const struct setting *pset, const char *val,
                               struct connection *caller, char *reject_msg,
                               size_t reject_msg_len)
{
  bool bval;

  if (SSET_BOOL != pset->stype) {
    settings_snprintf(reject_msg, reject_msg_len,
                      _("This setting is not a boolean."));
    return FALSE;
  }

  if (!setting_bool_str_to_bool(pset, val, &bval)) {
    settings_snprintf(reject_msg, reject_msg_len,
                      _("\"%s\" is not an allowed value for this setting."),
                      val);
    return FALSE;
  }

  return (!pset->boolean.validate
          || pset->boolean.validate(bval, caller, reject_msg,
                                    reject_msg_len));
}

/****************************************************************************
  Returns the current integer value.
****************************************************************************/
int setting_int_get(const struct setting *pset)
{
  fc_assert_ret_val(pset->stype == SSET_INT, 0);
  return *pset->integer.pvalue;
}

/****************************************************************************
  Returns the default integer value for this setting.
****************************************************************************/
int setting_int_def(const struct setting *pset)
{
  fc_assert_ret_val(pset->stype == SSET_INT, 0);
  return pset->integer.default_value;
}

/****************************************************************************
  Returns the minimal integer value for this setting.
****************************************************************************/
int setting_int_min(const struct setting *pset)
{
  fc_assert_ret_val(pset->stype == SSET_INT, 0);
  return pset->integer.min_value;
}

/****************************************************************************
  Returns the maximal integer value for this setting.
****************************************************************************/
int setting_int_max(const struct setting *pset)
{
  fc_assert_ret_val(pset->stype == SSET_INT, 0);
  return pset->integer.max_value;
}

/****************************************************************************
  Set the setting to 'val'. Returns TRUE on success. If it fails, the
  reason of the failure is available by the function setting_error().
****************************************************************************/
bool setting_int_set(struct setting *pset, int val,
                     struct connection *caller, char *reject_msg,
                     size_t reject_msg_len)
{
  if (!setting_is_changeable(pset, caller, reject_msg, reject_msg_len)
      || !setting_int_validate(pset, val, caller, reject_msg,
                               reject_msg_len)) {
    return FALSE;
  }

  *pset->integer.pvalue = val;
  return TRUE;
}

/****************************************************************************
  Returns TRUE if 'val' is a valid value for this setting. If it's not,
  the reason of the failure is available by the function setting_error().

  FIXME: also check the access level of pconn.
****************************************************************************/
bool setting_int_validate(const struct setting *pset, int val,
                          struct connection *caller, char *reject_msg,
                          size_t reject_msg_len)
{
  if (SSET_INT != pset->stype) {
    settings_snprintf(reject_msg, reject_msg_len,
                      _("This setting is not an integer."));
    return FALSE;
  }

  if (val < pset->integer.min_value || val > pset->integer.max_value) {
    settings_snprintf(reject_msg, reject_msg_len,
                      _("Value out of range: %d (min: %d; max: %d)."),
                      val, pset->integer.min_value, pset->integer.max_value);
    return FALSE;
  }

  return (!pset->integer.validate
          || pset->integer.validate(val, caller, reject_msg,
                                    reject_msg_len));
}

/****************************************************************************
  Returns the current string.
****************************************************************************/
const char *setting_str_get(const struct setting *pset)
{
  fc_assert_ret_val(pset->stype == SSET_STRING, NULL);
  return pset->string.value;
}

/****************************************************************************
  Returns the default string for this setting.
****************************************************************************/
const char *setting_str_def(const struct setting *pset)
{
  fc_assert_ret_val(pset->stype == SSET_STRING, NULL);
  return pset->string.default_value;
}

/****************************************************************************
  Set the setting to 'val'. Returns TRUE on success. If it fails, the
  reason of the failure is available by the function setting_error().
****************************************************************************/
bool setting_str_set(struct setting *pset, const char *val,
                     struct connection *caller, char *reject_msg,
                     size_t reject_msg_len)
{
  if (!setting_is_changeable(pset, caller, reject_msg, reject_msg_len)
      || !setting_str_validate(pset, val, caller, reject_msg,
                               reject_msg_len)) {
    return FALSE;
  }

  fc_strlcpy(pset->string.value, val, pset->string.value_size);
  return TRUE;
}

/****************************************************************************
  Returns TRUE if 'val' is a valid value for this setting. If it's not,
  the reason of the failure is available by the function setting_error().

  FIXME: also check the access level of pconn.
****************************************************************************/
bool setting_str_validate(const struct setting *pset, const char *val,
                          struct connection *caller, char *reject_msg,
                          size_t reject_msg_len)
{
  if (SSET_STRING != pset->stype) {
    settings_snprintf(reject_msg, reject_msg_len,
                      _("This setting is not a string."));
    return FALSE;
  }

  if (strlen(val) > pset->string.value_size) {
    settings_snprintf(reject_msg, reject_msg_len,
                      _("String value too long (max length: %lu)."),
                      (unsigned long)pset->string.value_size);
    return FALSE;
  }

  return (!pset->string.validate
          || pset->string.validate(val, caller, reject_msg,
                                   reject_msg_len));
}

/****************************************************************************
  Convert the string to the integer representation of an enumerator.
  Return -1 if 'str' doesn't match any enumerator name.
****************************************************************************/
int setting_enum_str_to_int(const struct setting *pset, const char *str)
{
  const char *name;
  int val;

  fc_assert_ret_val(SSET_ENUM == pset->stype, -1);

  for (val = 0; (name = pset->enumerator.name(val)); val++) {
    if (0 == fc_strcasecmp(str, name)) {
      return val;
    }
  }
  return -1;
}

/****************************************************************************
  Convert the integer to the string representation of an enumerator.
  Return NULL if 'val' is not a valid enumerator.
****************************************************************************/
const char *setting_enum_int_to_str(const struct setting *pset, int val)
{
  fc_assert_ret_val(SSET_ENUM == pset->stype, NULL);
  return pset->enumerator.name(val);
}

/****************************************************************************
  Returns the current enumerator value (as an integer).
****************************************************************************/
int setting_enum_get_int(const struct setting *pset)
{
  fc_assert_ret_val(SSET_ENUM == pset->stype, -1);
  return *pset->enumerator.pvalue;
}

/****************************************************************************
  Returns the current enumerator value (as a string).
****************************************************************************/
const char *setting_enum_get_str(const struct setting *pset)
{
  fc_assert_ret_val(SSET_ENUM == pset->stype, NULL);
  return setting_enum_int_to_str(pset, *pset->enumerator.pvalue);
}

/****************************************************************************
  Returns the default enumerator value (as an integer).
****************************************************************************/
int setting_enum_def_int(const struct setting *pset)
{
  fc_assert_ret_val(SSET_ENUM == pset->stype, -1);
  return pset->enumerator.default_value;
}

/****************************************************************************
  Returns the default enumerator value (as a string).
****************************************************************************/
const char *setting_enum_def_str(const struct setting *pset)
{
  fc_assert_ret_val(SSET_ENUM == pset->stype, NULL);
  return setting_enum_int_to_str(pset, pset->enumerator.default_value);
}

/****************************************************************************
  Set the setting to 'val'. Returns TRUE on success. If it fails, the
  reason of the failure is available in the optionnal parameter
  'reject_msg'.
****************************************************************************/
bool setting_enum_set_int(struct setting *pset, int val,
                          struct connection *caller, char *reject_msg,
                          size_t reject_msg_len)
{
  if (!setting_is_changeable(pset, caller, reject_msg, reject_msg_len)
      || !setting_enum_validate_int(pset, val, caller, reject_msg,
                                    reject_msg_len)) {
    return FALSE;
  }

  *pset->enumerator.pvalue = val;
  return TRUE;
}

/****************************************************************************
  Set the setting to 'val'. Returns TRUE on success. If it fails, the
  reason of the failure is available in the optionnal parameter
  'reject_msg'.
****************************************************************************/
bool setting_enum_set_str(struct setting *pset, const char *val,
                          struct connection *caller, char *reject_msg,
                          size_t reject_msg_len)
{
  if (!setting_is_changeable(pset, caller, reject_msg, reject_msg_len)
      || !setting_enum_validate_str(pset, val, caller, reject_msg,
                                    reject_msg_len)) {
    return FALSE;
  }

  *pset->enumerator.pvalue = setting_enum_str_to_int(pset, val);
  return TRUE;
}

/****************************************************************************
  Returns TRUE if 'val' is a valid value for this setting. If it's not,
  the reason of the failure is available in the optionnal parameter
  'reject_msg'.

  FIXME: also check the access level of pconn.
****************************************************************************/
bool setting_enum_validate_int(const struct setting *pset, int val,
                               struct connection *caller, char *reject_msg,
                               size_t reject_msg_len)
{
  if (SSET_ENUM != pset->stype) {
    settings_snprintf(reject_msg, reject_msg_len,
                      _("This setting is not a enumerator."));
    return FALSE;
  }

  if (NULL == setting_enum_int_to_str(pset, val)) {
    settings_snprintf(reject_msg, reject_msg_len,
                      _("%d is not a right value for this setting."), val);
    return FALSE;
  }

  return (NULL == pset->enumerator.validate
          || pset->enumerator.validate(val, caller, reject_msg,
                                       reject_msg_len));
}

/****************************************************************************
  Returns TRUE if 'val' is a valid value for this setting. If it's not,
  the reason of the failure is available in the optionnal parameter
  'reject_msg'.

  FIXME: also check the access level of pconn.
****************************************************************************/
bool setting_enum_validate_str(const struct setting *pset, const char *val,
                               struct connection *caller, char *reject_msg,
                               size_t reject_msg_len)
{
  int int_value;

  if (SSET_ENUM != pset->stype) {
    settings_snprintf(reject_msg, reject_msg_len,
                      _("This setting is not a enumerator."));
    return FALSE;
  }

  int_value = setting_enum_str_to_int(pset, val);
  if (-1 == int_value) {
    settings_snprintf(reject_msg, reject_msg_len,
                      _("\"%s\" is not an allowed value for this setting."),
                      val);
    return FALSE;
  }

  return (NULL == pset->enumerator.validate
          || pset->enumerator.validate(int_value, caller, reject_msg,
                                       reject_msg_len));
}

/********************************************************************
  Update the setting to the default value
*********************************************************************/
static void setting_set_to_default(struct setting *pset)
{
  switch (pset->stype) {
  case SSET_BOOL:
    (*pset->boolean.pvalue) = pset->boolean.default_value;
    break;
  case SSET_INT:
    (*pset->integer.pvalue) = pset->integer.default_value;
    break;
  case SSET_STRING:
    fc_strlcpy(pset->string.value, pset->string.default_value,
               pset->string.value_size);
    break;
  case SSET_ENUM:
    (*pset->enumerator.pvalue) = pset->enumerator.default_value;
    break;
  }

  setting_action(pset);
}

/********************************************************************
  Execute the action callback if needed.
*********************************************************************/
void setting_action(const struct setting *pset) {
  if (pset->action != NULL) {
    pset->action(pset);
  }
}

/**************************************************************************
  Load game settings from ruleset file 'game.ruleset'.
**************************************************************************/
bool settings_ruleset(struct section_file *file, const char *section)
{
  const char *name;
  int j;

  /* set all settings to their default values and unlock them */
  settings_init();

  /* settings */
  if (NULL == secfile_section_by_name(file, section)) {
    /* no settings in ruleset file */
    log_verbose("no [%s] section for game settings in %s", section,
                secfile_name(file));
    return FALSE;
  }

  for (j = 0; (name = secfile_lookup_str_default(file, NULL, "%s.set%d.name",
                                                 section, j)); j++) {
    char path[256];
    fc_snprintf(path, sizeof(path), "%s.set%d", section, j);

    if (!setting_ruleset_one(file, name, path)) {
      log_error("unknown setting in '%s': %s", secfile_name(file), name);
    }
  }

  /* send game settings */
  send_server_settings(NULL);

  return TRUE;
}

/**************************************************************************
  Set one setting from the game.ruleset file.
**************************************************************************/
static bool setting_ruleset_one(struct section_file *file,
                                const char *name, const char *path)
{
  bool bval, lock;
  int ival;
  const char *sval = NULL;
  char reject_msg[258] = "";
  struct setting *pset = NULL;

  settings_iterate(pset_check) {
    if (0 == strcmp(setting_name(pset_check), name)) {
      pset = pset_check;
      break;
    }
  } settings_iterate_end;

  if (pset == NULL) {
    /* no setting found */
    return FALSE;
  }

  /* FIXME: mostly a duplicate of parts of stdinhand:set_command() */
  switch (pset->stype) {
  case SSET_BOOL:
    if (!secfile_lookup_bool(file, &bval, "%s.value", path)) {
        log_error("Can't read value for setting '%s': %s", name,
                  secfile_error());
    } else if (bval != setting_bool_get(pset)) {
      if (setting_bool_set(pset, bval, NULL, reject_msg,
                           sizeof(reject_msg))) {
        log_normal(_("Option: %s has been set to %d."),
                   setting_name(pset), setting_bool_get(pset) ? 1 : 0);
      } else {
        log_error("%s", reject_msg);
      }
    }
    break;

  case SSET_INT:
    if (!secfile_lookup_int(file, &ival, "%s.value", path)) {
        log_error("Can't read value for setting '%s': %s", name,
                  secfile_error());
    } else if (ival != setting_int_get(pset)) {
      if (setting_int_set(pset, ival, NULL, reject_msg,
                          sizeof(reject_msg))) {
        log_normal(_("Option: %s has been set to %d."),
                   setting_name(pset), setting_int_get(pset));
      } else {
        log_error("%s", reject_msg);
      }
    }
    break;

  case SSET_STRING:
    if (!(sval = secfile_lookup_str(file, "%s.value", path))) {
        log_error("Can't read value for setting '%s': %s", name,
                  secfile_error());
    } else if (strcmp(sval, setting_str_get(pset)) != 0) {
      if (setting_str_set(pset, sval, NULL, reject_msg,
                          sizeof(reject_msg))) {
        log_normal(_("Option: %s has been set to \"%s\"."),
                   setting_name(pset), setting_str_get(pset));
      } else {
        log_error("%s", reject_msg);
      }
    }
    break;

  case SSET_ENUM:
    if (!(sval = secfile_lookup_str(file, "%s.value", path))) {
      log_error("Can't read value for setting '%s': %s",
                name, secfile_error());
    } else if (0 != fc_strcasecmp(sval, setting_enum_get_str(pset))) {
      if (setting_enum_set_str(pset, sval, NULL, reject_msg,
                               sizeof(reject_msg))) {
        log_normal(_("Option: %s has been set to \"%s\" (%d)."),
                   setting_name(pset), _(setting_enum_get_str(pset)),
                   setting_enum_get_int(pset));
      } else {
        log_error("%s", reject_msg);
      }
    }
    break;
  }

  if (!secfile_lookup_bool(file, &lock, "%s.lock", path)) {
    log_error("Can't read lock status for setting '%s': %s", name,
              secfile_error());
  } else if (lock) {
    /* set lock */
    setting_lock_set(pset, lock);
    log_normal(_("Option: %s has been locked by the ruleset."),
               setting_name(pset));
  }

  return TRUE;
}

/**************************************************************************
  Returns if the setting is locked by the ruleset.
**************************************************************************/
bool setting_locked(const struct setting *pset)
{
  return pset->locked;
}

/**************************************************************************
  Set the value for the lock of a setting.
**************************************************************************/
void setting_lock_set(struct setting *pset, bool lock)
{
  pset->locked = lock;
}

/**************************************************************************
  Save the setting value of the current game.
**************************************************************************/
static void setting_game_set(struct setting *pset, bool init)
{
  switch (setting_type(pset)) {
  case SSET_BOOL:
    pset->boolean.game_value = setting_bool_get(pset);
    break;

  case SSET_INT:
    pset->integer.game_value = setting_int_get(pset);
    break;

  case SSET_STRING:
    if (init) {
      pset->string.game_value
        = fc_calloc(1, pset->string.value_size
                       * sizeof(pset->string.game_value));
    }
    fc_strlcpy(pset->string.game_value, setting_str_get(pset),
              pset->string.value_size);
    break;

  case SSET_ENUM:
    pset->enumerator.game_value = setting_enum_get_int(pset);
    break;
  }
}

/**************************************************************************
  Free the memory used for the settings at game start.
**************************************************************************/
static void setting_game_free(struct setting *pset)
{
  if (setting_type(pset) == SSET_STRING) {
    FC_FREE(pset->string.game_value);
  }
}

/**************************************************************************
  Restore the setting to the value used at the start of the current game.
**************************************************************************/
static void setting_game_restore(struct setting *pset)
{
  bool res = FALSE;
  char reject_msg[258] = "";

  if (!setting_is_changeable(pset, NULL, reject_msg, sizeof(reject_msg))) {
    log_debug("Can't restore '%s': %s", setting_name(pset),
              reject_msg);
    return;
  }

  switch (setting_type(pset)) {
  case SSET_BOOL:
    res = setting_bool_set(pset, pset->boolean.game_value, NULL, reject_msg,
                           sizeof(reject_msg));
    break;

  case SSET_INT:
    res = setting_int_set(pset, pset->integer.game_value, NULL, reject_msg,
                          sizeof(reject_msg));
    break;

  case SSET_STRING:
    res = setting_str_set(pset, pset->string.game_value, NULL, reject_msg,
                          sizeof(reject_msg));
    break;

  case SSET_ENUM:
    res = setting_enum_set_int(pset, pset->enumerator.game_value,
                               NULL, reject_msg, sizeof(reject_msg));
    break;
  }

  if (!res) {
    log_error("Error restoring setting '%s' to the value from game start: "
              "%s", setting_name(pset), reject_msg);
  }
}

/**************************************************************************
  Save setting values at the start of  the game.
**************************************************************************/
void settings_game_start(void)
{
  settings_iterate(pset) {
    setting_game_set(pset, FALSE);
  } settings_iterate_end;

  /* Settings from the start of the game are saved. */
  game.server.settings_gamestart_valid = TRUE;
}

/********************************************************************
  Save game settings.
*********************************************************************/
void settings_game_save(struct section_file *file, const char *section)
{
  int set_count = 0;

  settings_iterate(pset) {
    switch (setting_type(pset)) {
    case SSET_BOOL:
      secfile_insert_str(file, setting_name(pset),
                         "%s.set%d.name", section, set_count);
      secfile_insert_bool(file, setting_bool_get(pset),
                          "%s.set%d.value", section, set_count);
      secfile_insert_bool(file, pset->boolean.game_value,
                          "%s.set%d.gamestart", section, set_count);
      break;
    case SSET_INT:
      secfile_insert_str(file, setting_name(pset),
                         "%s.set%d.name", section, set_count);
      secfile_insert_int(file, setting_int_get(pset),
                          "%s.set%d.value", section, set_count);
      secfile_insert_int(file, pset->integer.game_value,
                          "%s.set%d.gamestart", section, set_count);
      break;
    case SSET_STRING:
      secfile_insert_str(file, setting_name(pset),
                         "%s.set%d.name", section, set_count);
      secfile_insert_str(file, setting_str_get(pset),
                          "%s.set%d.value", section, set_count);
      secfile_insert_str(file, pset->string.game_value,
                          "%s.set%d.gamestart", section, set_count);
      break;
    case SSET_ENUM:
      secfile_insert_str(file, setting_name(pset),
                         "%s.set%d.name", section, set_count);
      secfile_insert_str(file, setting_enum_get_str(pset),
                         "%s.set%d.value", section, set_count);
      secfile_insert_str(file, setting_enum_int_to_str(pset,
                         pset->enumerator.game_value),
                         "%s.set%d.gamestart", section, set_count);
      break;
    }
    set_count++;
  } settings_iterate_end;

  secfile_insert_int(file, set_count, "%s.set_count", section);
  secfile_insert_bool(file, game.server.settings_gamestart_valid,
                      "%s.gamestart_valid", section);
}

/********************************************************************
  Restore all settings from a savegame.
*********************************************************************/
void settings_game_load(struct section_file *file, const char *section)
{
  bool bval;
  int ival;
  const char *sval = NULL, *name = NULL;
  char buf[258] = "";
  int i, set_count;

  if (!secfile_lookup_int(file, &set_count, "%s.set_count", section)) {
    /* Old savegames and scenarios doesn't contain this, not an error. */
    log_verbose("Can't read the number of settings in the save file.");
    return;
  }

  /* Check if the saved settings are valid settings from game start. */
  game.server.settings_gamestart_valid
    = secfile_lookup_bool_default(file, FALSE, "%s.gamestart_valid",
                                  section);

  for (i = 0; i < set_count; i++) {
    name = secfile_lookup_str(file, "%s.set%d.name", section, i);

    settings_iterate(pset) {
      if (fc_strcasecmp(setting_name(pset), name) != 0) {
        continue;
      }

      /* Load the current value of the setting. */
      switch (pset->stype) {
      case SSET_BOOL:
        bval = secfile_lookup_bool_default(file, pset->boolean.default_value,
                                           "%s.set%d.value", section, i);
        if (bval != setting_bool_get(pset)
            && !setting_bool_set(pset, bval, NULL, buf, sizeof(buf))) {
          log_error("Error restoring '%s': %s", setting_name(pset), buf);
        }
        break;

      case SSET_INT:
        ival = secfile_lookup_int_default(file, pset->integer.default_value,
                                          "%s.set%d.value", section, i);
        if (ival != setting_int_get(pset)
            && !setting_int_set(pset, ival, NULL, buf, sizeof(buf))) {
          log_error("Error restoring '%s': %s", setting_name(pset), buf);
        }
        break;

      case SSET_STRING:
        sval = secfile_lookup_str_default(file, pset->string.default_value,
                                          "%s.set%d.value", section, i);
        if (fc_strcasecmp(sval, setting_str_get(pset)) != 0
            && !setting_str_set(pset, sval, NULL, buf, sizeof(buf))) {
          log_error("Error restoring '%s': %s", setting_name(pset), buf);
        }
        break;

      case SSET_ENUM:
        sval = secfile_lookup_str_default(file, setting_enum_def_str(pset),
                                          "%s.set%d.value", section, i);
        if (!setting_enum_set_str(pset, sval, NULL, buf, sizeof(buf))) {
          log_error("Error restoring '%s': %s", setting_name(pset), buf);
        }
        break;
      }

      if (game.server.settings_gamestart_valid) {
        /* Load the value of the setting at the start of the game. */
        switch (pset->stype) {
        case SSET_BOOL:
          bval = secfile_lookup_bool_default(file, setting_bool_get(pset),
                                             "%s.set%d.gamestart", section,
                                             i);
          pset->boolean.game_value = bval;
          break;

        case SSET_INT:
          ival = secfile_lookup_int_default(file, setting_int_get(pset),
                                            "%s.set%d.gamestart", section,
                                            i);
          pset->integer.game_value = ival;
          break;

        case SSET_STRING:
          sval = secfile_lookup_str_default(file, setting_str_get(pset),
                                            "%s.set%d.gamestart", section,
                                            i);
          fc_strlcpy(pset->string.game_value, sval, pset->string.value_size);
          break;

        case SSET_ENUM:
          sval = secfile_lookup_str_default(file, setting_enum_get_str(pset),
                                            "%s.set%d.gamestart", section,
                                            i);
          ival = setting_enum_str_to_int(pset, sval);
          if (-1 != ival) {
            pset->enumerator.game_value = ival;
          }
          break;
        }
      }
    } settings_iterate_end;
  }

  settings_iterate(pset) {
    /* Have to do this at the end due to dependencies ('aifill' and
     * 'maxplayer'). */
    setting_action(pset);
  } settings_iterate_end;
}

/**************************************************************************
  Reset all settings to the values at game start.
**************************************************************************/
bool settings_game_reset(void)
{
  if (!game.server.settings_gamestart_valid) {
    log_debug("No saved settings from the game start available.");
    return FALSE;
  }

  settings_iterate(pset) {
    setting_game_restore(pset);
  } settings_iterate_end;

  return TRUE;
}

/**************************************************************************
  Initialize stuff related to this code module.
**************************************************************************/
void settings_init(void)
{
  settings_iterate(pset) {
    setting_lock_set(pset, FALSE);
    setting_set_to_default(pset);
    setting_game_set(pset, TRUE);
  } settings_iterate_end;
}

/********************************************************************
  Reset all settings iff they are changeable.
*********************************************************************/
void settings_reset(void)
{
  settings_iterate(pset) {
    if (setting_is_changeable(pset, NULL, NULL, 0)) {
      setting_set_to_default(pset);
    }
  } settings_iterate_end;
}

/**************************************************************************
  Update stuff every turn that is related to this code module. Run this
  on turn end.
**************************************************************************/
void settings_turn(void)
{
  /* Nothing at the moment. */
}

/**************************************************************************
  Deinitialize stuff related to this code module.
**************************************************************************/
void settings_free(void)
{
  settings_iterate(pset) {
    setting_game_free(pset);
  } settings_iterate_end;
}

/****************************************************************************
  Tell the client about just one server setting.  Call this after a setting
  is saved.
****************************************************************************/
void send_server_setting(struct conn_list *dest, const struct setting *pset)
{
  if (!dest) {
    dest = game.est_connections;
  }

#define PACKET_COMMON_INIT(packet, pset, pconn)                             \
  memset(&packet, 0, sizeof(packet));                                       \
  packet.id = setting_number(pset);                                         \
  packet.is_visible = setting_is_visible(pset, pconn);                      \
  packet.is_changeable = setting_is_changeable(pset, pconn, NULL, 0);       \
  packet.initial_setting = game.info.is_new_game;

  switch (setting_type(pset)) {
  case SSET_BOOL:
    {
      struct packet_server_setting_bool packet;

      conn_list_iterate(dest, pconn) {
        PACKET_COMMON_INIT(packet, pset, pconn);
        if (packet.is_visible) {
          packet.val = setting_bool_get(pset);
          packet.default_val = setting_bool_def(pset);
        }
        send_packet_server_setting_bool(pconn, &packet);
      } conn_list_iterate_end;
    }
    break;
  case SSET_INT:
    {
      struct packet_server_setting_int packet;

      conn_list_iterate(dest, pconn) {
        PACKET_COMMON_INIT(packet, pset, pconn);
        if (packet.is_visible) {
          packet.val = setting_int_get(pset);
          packet.default_val = setting_int_def(pset);
          packet.min_val = setting_int_min(pset);
          packet.max_val = setting_int_max(pset);
        }
        send_packet_server_setting_int(pconn, &packet);
      } conn_list_iterate_end;
    }
    break;
  case SSET_STRING:
    {
      struct packet_server_setting_str packet;

      conn_list_iterate(dest, pconn) {
        PACKET_COMMON_INIT(packet, pset, pconn);
        if (packet.is_visible) {
          sz_strlcpy(packet.val, setting_str_get(pset));
          sz_strlcpy(packet.default_val, setting_str_def(pset));
        }
        send_packet_server_setting_str(pconn, &packet);
      } conn_list_iterate_end;
    }
    break;
  case SSET_ENUM:
    {
      struct packet_server_setting_enum packet;
      const char *value;
      int i;

      conn_list_iterate(dest, pconn) {
        PACKET_COMMON_INIT(packet, pset, pconn);
        if (packet.is_visible) {
          packet.val = setting_enum_get_int(pset);
          packet.default_val = setting_enum_def_int(pset);
          for (i = 0; (value = setting_enum_int_to_str(pset, i)); i++) {
            sz_strlcpy(packet.values[i], value);
          }
          packet.values_num = i;
          fc_assert(i <= ARRAY_SIZE(packet.values));
        }
        send_packet_server_setting_enum(pconn, &packet);
      } conn_list_iterate_end;
    }
    break;
  }

#undef PACKET_INIT
}

/****************************************************************************
  Tell the client about all server settings.
****************************************************************************/
void send_server_settings(struct conn_list *dest)
{
  settings_iterate(pset) {
    send_server_setting(dest, pset);
  } settings_iterate_end;
}

/****************************************************************************
  Send the ALLOW_HACK server settings.  Usually called when the access level
  of the user changes.
****************************************************************************/
void send_server_hack_level_settings(struct conn_list *dest)
{
  settings_iterate(pset) {
    if (!pset->to_client) {
      send_server_setting(dest, pset);
    }
  } settings_iterate_end;
}

/****************************************************************************
  Tell the client about all server settings.
****************************************************************************/
void send_server_setting_control(struct connection *pconn)
{
  struct packet_server_setting_control control;
  struct packet_server_setting_const setting;
  int i;

  control.settings_num = SETTINGS_NUM;

  /* Fill in the category strings. */
  fc_assert(SSET_NUM_CATEGORIES <= ARRAY_SIZE(control.category_names));
  control.categories_num = SSET_NUM_CATEGORIES;
  for (i = 0; i < SSET_NUM_CATEGORIES; i++) {
    sz_strlcpy(control.category_names[i], sset_category_names[i]);
  }

  /* Send off the control packet. */
  send_packet_server_setting_control(pconn, &control);

  /* Send the constant and common part of the settings. */
  settings_iterate(pset) {
    setting.id = setting_number(pset);
    sz_strlcpy(setting.name, setting_name(pset));
    sz_strlcpy(setting.short_help, setting_short_help(pset));
    sz_strlcpy(setting.extra_help, setting_extra_help(pset));
    setting.category = pset->scategory;

    send_packet_server_setting_const(pconn, &setting);
  } settings_iterate_end;
}

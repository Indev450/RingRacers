// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 1998-2000 by DooM Legacy Team.
// Copyright (C) 1999-2016 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  dehacked.c
/// \brief Load dehacked file and change tables and text

#include "doomdef.h"
#include "g_game.h"
#include "sounds.h"
#include "info.h"
#include "d_think.h"
#include "m_argv.h"
#include "z_zone.h"
#include "w_wad.h"
#include "m_menu.h"
#include "m_misc.h"
#include "f_finale.h"
#include "dehacked.h"
#include "st_stuff.h"
#include "i_system.h"
#include "p_local.h" // for var1 and var2, and some constants
#include "p_setup.h"
#include "r_data.h"
#include "r_sky.h"
#include "fastcmp.h"
#include "lua_script.h"
#include "lua_hook.h"

#include "m_cond.h"

#ifdef HAVE_BLUA
#include "v_video.h" // video flags (for lua)
#endif

#ifdef HWRENDER
#include "hardware/hw_light.h"
#endif

#ifdef PC_DOS
#include <stdio.h> // for snprintf
//int	snprintf(char *str, size_t n, const char *fmt, ...);
int	vsnprintf(char *str, size_t n, const char *fmt, va_list ap);
#endif

// Free slot names
// The crazy word-reading stuff uses these.
static char *FREE_STATES[NUMSTATEFREESLOTS];
static char *FREE_MOBJS[NUMMOBJFREESLOTS];
static UINT8 used_spr[(NUMSPRITEFREESLOTS / 8) + 1]; // Bitwise flag for sprite freeslot in use! I would use ceil() here if I could, but it only saves 1 byte of memory anyway.
#define initfreeslots() {\
memset(FREE_STATES,0,sizeof(char *) * NUMSTATEFREESLOTS);\
memset(FREE_MOBJS,0,sizeof(char *) * NUMMOBJFREESLOTS);\
memset(used_spr,0,sizeof(UINT8) * ((NUMSPRITEFREESLOTS / 8) + 1));\
}

// Crazy word-reading stuff
/// \todo Put these in a seperate file or something.
static mobjtype_t get_mobjtype(const char *word);
static statenum_t get_state(const char *word);
static spritenum_t get_sprite(const char *word);
static playersprite_t get_sprite2(const char *word);
static sfxenum_t get_sfx(const char *word);
#ifdef MUSICSLOT_COMPATIBILITY
static UINT16 get_mus(const char *word, UINT8 dehacked_mode);
#endif
static hudnum_t get_huditem(const char *word);
#ifndef HAVE_BLUA
static powertype_t get_power(const char *word);
#endif

boolean deh_loaded = false;
static int dbg_line;

static boolean gamedataadded = false;
static boolean titlechanged = false;
static boolean introchanged = false;

ATTRINLINE static FUNCINLINE char myfget_color(MYFILE *f)
{
	char c = *f->curpos++;
	if (c == '^') // oh, nevermind then.
		return '^';

	if (c >= '0' && c <= '9')
		return 0x80+(c-'0');
	return 0x80; // Unhandled -- default to no color
}

ATTRINLINE static FUNCINLINE char myfget_hex(MYFILE *f)
{
	char c = *f->curpos++, endchr = 0;
	if (c == '\\') // oh, nevermind then.
		return '\\';

	if (c >= '0' && c <= '9')
		endchr += (c-'0') << 4;
	else if (c >= 'A' && c <= 'F')
		endchr += ((c-'A') + 10) << 4;
	else if (c >= 'a' && c <= 'f')
		endchr += ((c-'a') + 10) << 4;
	else // invalid. stop and return a question mark.
		return '?';

	c = *f->curpos++;
	if (c >= '0' && c <= '9')
		endchr += (c-'0');
	else if (c >= 'A' && c <= 'F')
		endchr += ((c-'A') + 10);
	else if (c >= 'a' && c <= 'f')
		endchr += ((c-'a') + 10);
	else // invalid. stop and return a question mark.
		return '?';

	return endchr;
}

char *myfgets(char *buf, size_t bufsize, MYFILE *f)
{
	size_t i = 0;
	if (myfeof(f))
		return NULL;
	// we need one byte for a null terminated string
	bufsize--;
	while (i < bufsize && !myfeof(f))
	{
		char c = *f->curpos++;
		if (c == '^')
			buf[i++] = myfget_color(f);
		else if (c == '\\')
			buf[i++] = myfget_hex(f);
		else if (c != '\r')
			buf[i++] = c;
		if (c == '\n')
			break;
	}
	buf[i] = '\0';

	dbg_line++;
	return buf;
}

static char *myhashfgets(char *buf, size_t bufsize, MYFILE *f)
{
	size_t i = 0;
	if (myfeof(f))
		return NULL;
	// we need one byte for a null terminated string
	bufsize--;
	while (i < bufsize && !myfeof(f))
	{
		char c = *f->curpos++;
		if (c == '^')
			buf[i++] = myfget_color(f);
		else if (c == '\\')
			buf[i++] = myfget_hex(f);
		else if (c != '\r')
			buf[i++] = c;
		if (c == '\n') // Ensure debug line is right...
			dbg_line++;
		if (c == '#')
			break;
	}
	i++;
	buf[i] = '\0';

	return buf;
}

static INT32 deh_num_warning = 0;

FUNCPRINTF static void deh_warning(const char *first, ...)
{
	va_list argptr;
	char *buf = Z_Malloc(1000, PU_STATIC, NULL);

	va_start(argptr, first);
	vsnprintf(buf, 1000, first, argptr); // sizeof only returned 4 here. it didn't like that pointer.
	va_end(argptr);

	if(dbg_line == -1) // Not in a SOC, line number unknown.
		CONS_Alert(CONS_WARNING, "%s\n", buf);
	else
		CONS_Alert(CONS_WARNING, "Line %u: %s\n", dbg_line, buf);

	deh_num_warning++;

	Z_Free(buf);
}

static void deh_strlcpy(char *dst, const char *src, size_t size, const char *warntext)
{
	size_t len = strlen(src)+1; // Used to determine if truncation has been done
	if (len > size)
		deh_warning("%s exceeds max length of %s", warntext, sizeu1(size-1));
	strlcpy(dst, src, size);
}

/* ======================================================================== */
// Load a dehacked file format
/* ======================================================================== */
/* a sample to see
                   Thing 1 (Player)       {           // MT_PLAYER
INT32 doomednum;     ID # = 3232              -1,             // doomednum
INT32 spawnstate;    Initial frame = 32       "PLAY",         // spawnstate
INT32 spawnhealth;   Hit points = 3232        100,            // spawnhealth
INT32 seestate;      First moving frame = 32  "PLAY_RUN1",    // seestate
INT32 seesound;      Alert sound = 32         sfx_None,       // seesound
INT32 reactiontime;  Reaction time = 3232     0,              // reactiontime
INT32 attacksound;   Attack sound = 32        sfx_None,       // attacksound
INT32 painstate;     Injury frame = 32        "PLAY_PAIN",    // painstate
INT32 painchance;    Pain chance = 3232       255,            // painchance
INT32 painsound;     Pain sound = 32          sfx_plpain,     // painsound
INT32 meleestate;    Close attack frame = 32  "NULL",         // meleestate
INT32 missilestate;  Far attack frame = 32    "PLAY_ATK1",    // missilestate
INT32 deathstate;    Death frame = 32         "PLAY_DIE1",    // deathstate
INT32 xdeathstate;   Exploding frame = 32     "PLAY_XDIE1",   // xdeathstate
INT32 deathsound;    Death sound = 32         sfx_pldeth,     // deathsound
INT32 speed;         Speed = 3232             0,              // speed
INT32 radius;        Width = 211812352        16*FRACUNIT,    // radius
INT32 height;        Height = 211812352       56*FRACUNIT,    // height
INT32 dispoffset;    DispOffset = 0           0,              // dispoffset
INT32 mass;          Mass = 3232              100,            // mass
INT32 damage;        Missile damage = 3232    0,              // damage
INT32 activesound;   Action sound = 32        sfx_None,       // activesound
INT32 flags;         Bits = 3232              MF_SOLID|MF_SHOOTABLE|MF_DROPOFF|MF_PICKUP|MF_NOTDMATCH,
INT32 raisestate;    Respawn frame = 32       S_NULL          // raisestate
                                         }, */

static INT32 searchvalue(const char *s)
{
	while (s[0] != '=' && s[0])
		s++;
	if (s[0] == '=')
		return atoi(&s[1]);
	else
	{
		deh_warning("No value found");
		return 0;
	}
}

#ifdef HWRENDER
static float searchfvalue(const char *s)
{
	while (s[0] != '=' && s[0])
		s++;
	if (s[0] == '=')
		return (float)atof(&s[1]);
	else
	{
		deh_warning("No value found");
		return 0;
	}
}
#endif

// These are for clearing all of various things
static void clear_conditionsets(void)
{
	UINT8 i;
	for (i = 0; i < MAXCONDITIONSETS; ++i)
		M_ClearConditionSet(i+1);
}

static void clear_levels(void)
{
	INT16 i;

	// This is potentially dangerous but if we're resetting these headers,
	// we may as well try to save some memory, right?
	for (i = 0; i < NUMMAPS; ++i)
	{
		if (!mapheaderinfo[i])
			continue;

		// Custom map header info
		// (no need to set num to 0, we're freeing the entire header shortly)
		Z_Free(mapheaderinfo[i]->customopts);

		P_DeleteFlickies(i);
		P_DeleteGrades(i);

		Z_Free(mapheaderinfo[i]);
		mapheaderinfo[i] = NULL;
	}

	// Realloc the one for the current gamemap as a safeguard
	P_AllocMapHeader(gamemap-1);
}

static boolean findFreeSlot(INT32 *num)
{
	// Send the character select entry to a free slot.
	while (*num < 32 && (description[*num].used))
		*num = *num+1;

	// No more free slots. :(
	if (*num >= 32)
		return false;

	description[*num].picname[0] = '\0'; // Redesign your logo. (See M_DrawSetupChoosePlayerMenu in m_menu.c...)

	// Found one! ^_^
	return (description[*num].used = true);
}

// Reads a player.
// For modifying the character select screen
static void readPlayer(MYFILE *f, INT32 num)
{
	char *s = Z_Malloc(MAXLINELEN, PU_STATIC, NULL);
	char *word;
	char *word2;
	INT32 i;
	boolean slotfound = false;

	do
	{
		if (myfgets(s, MAXLINELEN, f))
		{
			if (s[0] == '\n')
				break;

			word = strtok(s, " ");
			if (word)
				strupr(word);
			else
				break;

			if (fastcmp(word, "PLAYERTEXT"))
			{
				char *playertext = NULL;

				if (!slotfound && (slotfound = findFreeSlot(&num)) == false)
					goto done;

				for (i = 0; i < MAXLINELEN-3; i++)
				{
					if (s[i] == '=')
					{
						playertext = &s[i+2];
						break;
					}
				}
				if (playertext)
				{
					strcpy(description[num].notes, playertext);
					strcat(description[num].notes, myhashfgets(playertext, sizeof (description[num].notes), f));
				}
				else
					strcpy(description[num].notes, "");

				// For some reason, cutting the string did not work above. Most likely due to strcpy or strcat...
				// It works down here, though.
				{
					INT32 numline = 0;
					for (i = 0; i < MAXLINELEN-1; i++)
					{
						if (numline < 20 && description[num].notes[i] == '\n')
							numline++;

						if (numline >= 20 || description[num].notes[i] == '\0' || description[num].notes[i] == '#')
							break;
					}
				}
				description[num].notes[strlen(description[num].notes)-1] = '\0';
				description[num].notes[i] = '\0';
				continue;
			}

			word2 = strtok(NULL, " = ");
			if (word2)
				strupr(word2);
			else
				break;

			if (word2[strlen(word2)-1] == '\n')
				word2[strlen(word2)-1] = '\0';
			i = atoi(word2);

			if (fastcmp(word, "PICNAME"))
			{
				if (!slotfound && (slotfound = findFreeSlot(&num)) == false)
					goto done;

				strncpy(description[num].picname, word2, 8);
			}
			else if (fastcmp(word, "STATUS"))
			{
				/*
					You MAY disable previous entries if you so desire...
					But try to enable something that's already enabled and you will be sent to a free slot.

					Because of this, you are allowed to edit any previous entries you like, but only if you
					signal that you are purposely doing so by disabling and then reenabling the slot.
				*/
				if (i && !slotfound && (slotfound = findFreeSlot(&num)) == false)
					goto done;

				description[num].used = (!!i);
			}
			else if (fastcmp(word, "SKINNAME"))
			{
				// Send to free slot.
				if (!slotfound && (slotfound = findFreeSlot(&num)) == false)
					goto done;

				strlcpy(description[num].skinname, word2, sizeof description[num].skinname);
				strlwr(description[num].skinname);
			}
			else
				deh_warning("readPlayer %d: unknown word '%s'", num, word);
		}
	} while (!myfeof(f)); // finish when the line is empty

done:
	Z_Free(s);
}

// TODO: Figure out how to do undolines for this....
// TODO: Warnings for running out of freeslots
static void readfreeslots(MYFILE *f)
{
	char *s = Z_Malloc(MAXLINELEN, PU_STATIC, NULL);
	char *word,*type;
	char *tmp;
	int i;

	do
	{
		if (myfgets(s, MAXLINELEN, f))
		{
			if (s[0] == '\n')
				break;

			tmp = strchr(s, '#');
			if (tmp)
				*tmp = '\0';
			if (s == tmp)
				continue; // Skip comment lines, but don't break.

			type = strtok(s, "_");
			if (type)
				strupr(type);
			else
				break;

			word = strtok(NULL, "\n");
			if (word)
				strupr(word);
			else
				break;

			// TODO: Check for existing freeslot mobjs/states/etc. and make errors.
			// TODO: Out-of-slots warnings/errors.
			// TODO: Name too long (truncated) warnings.
			if (fastcmp(type, "SFX"))
				S_AddSoundFx(word, false, 0, false);
			else if (fastcmp(type, "SPR"))
			{
				for (i = SPR_FIRSTFREESLOT; i <= SPR_LASTFREESLOT; i++)
				{
					if (used_spr[(i-SPR_FIRSTFREESLOT)/8] & (1<<(i%8)))
					{
						if (!sprnames[i][4] && memcmp(sprnames[i],word,4)==0)
							sprnames[i][4] = (char)f->wad;
						continue; // Already allocated, next.
					}
					// Found a free slot!
					strncpy(sprnames[i],word,4);
					//sprnames[i][4] = 0;
					used_spr[(i-SPR_FIRSTFREESLOT)/8] |= 1<<(i%8); // Okay, this sprite slot has been named now.
					break;
				}
			}
			else if (fastcmp(type, "S"))
			{
				for (i = 0; i < NUMSTATEFREESLOTS; i++)
					if (!FREE_STATES[i]) {
						FREE_STATES[i] = Z_Malloc(strlen(word)+1, PU_STATIC, NULL);
						strcpy(FREE_STATES[i],word);
						break;
					}
			}
			else if (fastcmp(type, "MT"))
			{
				for (i = 0; i < NUMMOBJFREESLOTS; i++)
					if (!FREE_MOBJS[i]) {
						FREE_MOBJS[i] = Z_Malloc(strlen(word)+1, PU_STATIC, NULL);
						strcpy(FREE_MOBJS[i],word);
						break;
					}
			}
			else if (fastcmp(type, "SPR2"))
			{
				// Search if we already have an SPR2 by that name...
				for (i = SPR2_FIRSTFREESLOT; i < (int)free_spr2; i++)
					if (memcmp(spr2names[i],word,4) == 0)
						break;
				// We found it? (Two mods using the same SPR2 name?) Then don't allocate another one.
				if (i < (int)free_spr2)
					continue;
				// Copy in the spr2 name and increment free_spr2.
				if (free_spr2 < NUMPLAYERSPRITES) {
					strncpy(spr2names[free_spr2],word,4);
					spr2names[free_spr2++][4] = 0;
				} else
					CONS_Alert(CONS_WARNING, "Ran out of free SPR2 slots!\n");
			}
			else
				deh_warning("Freeslots: unknown enum class '%s' for '%s_%s'", type, type, word);
		}
	} while (!myfeof(f)); // finish when the line is empty

	Z_Free(s);
}

static void readthing(MYFILE *f, INT32 num)
{
	char *s = Z_Malloc(MAXLINELEN, PU_STATIC, NULL);
	char *word, *word2;
	char *tmp;

	do
	{
		if (myfgets(s, MAXLINELEN, f))
		{
			if (s[0] == '\n')
				break;

			tmp = strchr(s, '#');
			if (tmp)
				*tmp = '\0';
			if (s == tmp)
				continue; // Skip comment lines, but don't break.

			word = strtok(s, " ");
			if (word)
				strupr(word);
			else
				break;

			word2 = strtok(NULL, " = ");
			if (word2)
				strupr(word2);
			else
				break;
			if (word2[strlen(word2)-1] == '\n')
				word2[strlen(word2)-1] = '\0';

			if (fastcmp(word, "MAPTHINGNUM") || fastcmp(word, "DOOMEDNUM"))
			{
				mobjinfo[num].doomednum = (INT32)atoi(word2);
			}
			else if (fastcmp(word, "SPAWNSTATE"))
			{
				mobjinfo[num].spawnstate = get_number(word2);
			}
			else if (fastcmp(word, "SPAWNHEALTH"))
			{
				mobjinfo[num].spawnhealth = (INT32)get_number(word2);
			}
			else if (fastcmp(word, "SEESTATE"))
			{
				mobjinfo[num].seestate = get_number(word2);
			}
			else if (fastcmp(word, "SEESOUND"))
			{
				mobjinfo[num].seesound = get_number(word2);
			}
			else if (fastcmp(word, "REACTIONTIME"))
			{
				mobjinfo[num].reactiontime = (INT32)get_number(word2);
			}
			else if (fastcmp(word, "ATTACKSOUND"))
			{
				mobjinfo[num].attacksound = get_number(word2);
			}
			else if (fastcmp(word, "PAINSTATE"))
			{
				mobjinfo[num].painstate = get_number(word2);
			}
			else if (fastcmp(word, "PAINCHANCE"))
			{
				mobjinfo[num].painchance = (INT32)get_number(word2);
			}
			else if (fastcmp(word, "PAINSOUND"))
			{
				mobjinfo[num].painsound = get_number(word2);
			}
			else if (fastcmp(word, "MELEESTATE"))
			{
				mobjinfo[num].meleestate = get_number(word2);
			}
			else if (fastcmp(word, "MISSILESTATE"))
			{
				mobjinfo[num].missilestate = get_number(word2);
			}
			else if (fastcmp(word, "DEATHSTATE"))
			{
				mobjinfo[num].deathstate = get_number(word2);
			}
			else if (fastcmp(word, "DEATHSOUND"))
			{
				mobjinfo[num].deathsound = get_number(word2);
			}
			else if (fastcmp(word, "XDEATHSTATE"))
			{
				mobjinfo[num].xdeathstate = get_number(word2);
			}
			else if (fastcmp(word, "SPEED"))
			{
				mobjinfo[num].speed = get_number(word2);
			}
			else if (fastcmp(word, "RADIUS"))
			{
				mobjinfo[num].radius = get_number(word2);
			}
			else if (fastcmp(word, "HEIGHT"))
			{
				mobjinfo[num].height = get_number(word2);
			}
			else if (fastcmp(word, "DISPOFFSET"))
			{
				mobjinfo[num].dispoffset = get_number(word2);
			}
			else if (fastcmp(word, "MASS"))
			{
				mobjinfo[num].mass = (INT32)get_number(word2);
			}
			else if (fastcmp(word, "DAMAGE"))
			{
				mobjinfo[num].damage = (INT32)get_number(word2);
			}
			else if (fastcmp(word, "ACTIVESOUND"))
			{
				mobjinfo[num].activesound = get_number(word2);
			}
			else if (fastcmp(word, "FLAGS"))
			{
				mobjinfo[num].flags = (INT32)get_number(word2);
			}
			else if (fastcmp(word, "RAISESTATE"))
			{
				mobjinfo[num].raisestate = get_number(word2);
			}
			else
				deh_warning("Thing %d: unknown word '%s'", num, word);
		}
	} while (!myfeof(f)); // finish when the line is empty

	Z_Free(s);
}

#ifdef HWRENDER
static void readlight(MYFILE *f, INT32 num)
{
	char *s = Z_Malloc(MAXLINELEN, PU_STATIC, NULL);
	char *word;
	char *tmp;
	INT32 value;
	float fvalue;

	do
	{
		if (myfgets(s, MAXLINELEN, f))
		{
			if (s[0] == '\n')
				break;

			tmp = strchr(s, '#');
			if (tmp)
				*tmp = '\0';
			if (s == tmp)
				continue; // Skip comment lines, but don't break.

			fvalue = searchfvalue(s);
			value = searchvalue(s);

			word = strtok(s, " ");
			if (word)
				strupr(word);
			else
				break;

			if (fastcmp(word, "TYPE"))
			{
				lspr[num].type = (UINT16)value;
			}
			else if (fastcmp(word, "OFFSETX"))
			{
				lspr[num].light_xoffset = fvalue;
			}
			else if (fastcmp(word, "OFFSETY"))
			{
				lspr[num].light_yoffset = fvalue;
			}
			else if (fastcmp(word, "CORONACOLOR"))
			{
				lspr[num].corona_color = value;
			}
			else if (fastcmp(word, "CORONARADIUS"))
			{
				lspr[num].corona_radius = fvalue;
			}
			else if (fastcmp(word, "DYNAMICCOLOR"))
			{
				lspr[num].dynamic_color = value;
			}
			else if (fastcmp(word, "DYNAMICRADIUS"))
			{
				lspr[num].dynamic_radius = fvalue;

				/// \note Update the sqrradius! unnecessary?
				lspr[num].dynamic_sqrradius = fvalue * fvalue;
			}
			else
				deh_warning("Light %d: unknown word '%s'", num, word);
		}
	} while (!myfeof(f)); // finish when the line is empty

	Z_Free(s);
}

static void readspritelight(MYFILE *f, INT32 num)
{
	char *s = Z_Malloc(MAXLINELEN, PU_STATIC, NULL);
	char *word;
	char *tmp;
	INT32 value;

	do
	{
		if (myfgets(s, MAXLINELEN, f))
		{
			if (s[0] == '\n')
				break;

			tmp = strchr(s, '#');
			if (tmp)
				*tmp = '\0';
			if (s == tmp)
				continue; // Skip comment lines, but don't break.

			value = searchvalue(s);

			word = strtok(s, " ");
			if (word)
				strupr(word);
			else
				break;

			if (fastcmp(word, "LIGHTTYPE"))
			{
				INT32 oldvar;
				for (oldvar = 0; t_lspr[num] != &lspr[oldvar]; oldvar++)
					;
				t_lspr[num] = &lspr[value];
			}
			else
				deh_warning("Sprite %d: unknown word '%s'", num, word);
		}
	} while (!myfeof(f)); // finish when the line is empty

	Z_Free(s);
}
#endif // HWRENDER

static void readsprite2(MYFILE *f, INT32 num)
{
	char *s = Z_Malloc(MAXLINELEN, PU_STATIC, NULL);
	char *word, *word2;
	char *tmp;

	do
	{
		if (myfgets(s, MAXLINELEN, f))
		{
			if (s[0] == '\n')
				break;

			tmp = strchr(s, '#');
			if (tmp)
				*tmp = '\0';
			if (s == tmp)
				continue; // Skip comment lines, but don't break.

			word = strtok(s, " ");
			if (word)
				strupr(word);
			else
				break;

			word2 = strtok(NULL, " = ");
			if (word2)
				strupr(word2);
			else
				break;
			if (word2[strlen(word2)-1] == '\n')
				word2[strlen(word2)-1] = '\0';

			if (fastcmp(word, "DEFAULT"))
				spr2defaults[num] = get_number(word2);
			else
				deh_warning("Sprite2 %s: unknown word '%s'", spr2names[num], word);
		}
	} while (!myfeof(f)); // finish when the line is empty

	Z_Free(s);
}

static const struct {
	const char *name;
	const UINT16 flag;
} TYPEOFLEVEL[] = {
	{"SOLO",TOL_SP},
	{"SP",TOL_SP},
	{"SINGLEPLAYER",TOL_SP},
	{"SINGLE",TOL_SP},

	{"COOP",TOL_COOP},
	{"CO-OP",TOL_COOP},

	{"COMPETITION",TOL_COMPETITION},
	{"RACE",TOL_RACE},

	{"MATCH",TOL_MATCH},
	{"TAG",TOL_TAG},
	{"CTF",TOL_CTF},

	{"CUSTOM",TOL_CUSTOM},

	{"2D",TOL_2D},
	{"MARIO",TOL_MARIO},
	{"NIGHTS",TOL_NIGHTS},
	{"OLDBRAK",TOL_ERZ3},

	{"XMAS",TOL_XMAS},
	{"CHRISTMAS",TOL_XMAS},
	{"WINTER",TOL_XMAS},

	{NULL, 0}
};

static const struct {
	const char *name;
	const mobjtype_t type;
} FLICKYTYPES[] = {
	{"BLUEBIRD", MT_FLICKY_01},
	{"RABBIT",   MT_FLICKY_02},
	{"CHICKEN",  MT_FLICKY_03},
	{"SEAL",     MT_FLICKY_04},
	{"PIG",      MT_FLICKY_05},
	{"CHIPMUNK", MT_FLICKY_06},
	{"PENGUIN",  MT_FLICKY_07},
	{"FISH",     MT_FLICKY_08},
	{"RAM",      MT_FLICKY_09},
	{"PUFFIN",   MT_FLICKY_10},
	{"COW",      MT_FLICKY_11},
	{"RAT",      MT_FLICKY_12},
	{"BEAR",     MT_FLICKY_13},
	{"DOVE",     MT_FLICKY_14},
	{"CAT",      MT_FLICKY_15},
	{"CANARY",   MT_FLICKY_16},
	{"a", 0}, // End of normal flickies - a lower case character so will never fastcmp valid with uppercase tmp
	//{"FLICKER",  MT_FLICKER},
	{"SEED",          MT_SEED},
	{NULL, 0}
};

#define MAXFLICKIES 64

static void readlevelheader(MYFILE *f, INT32 num)
{
	char *s = Z_Malloc(MAXLINELEN, PU_STATIC, NULL);
	char *word;
	char *word2;
	//char *word3; // Non-uppercase version of word2
	char *tmp;
	INT32 i;

	// Reset all previous map header information
	P_AllocMapHeader((INT16)(num-1));

	do
	{
		if (myfgets(s, MAXLINELEN, f))
		{
			if (s[0] == '\n')
				break;

			// First remove trailing newline, if there is one
			tmp = strchr(s, '\n');
			if (tmp)
				*tmp = '\0';

			tmp = strchr(s, '#');
			if (tmp)
				*tmp = '\0';
			if (s == tmp)
				continue; // Skip comment lines, but don't break.

			// Set / reset word, because some things (Lua.) move it
			word = s;

			// Get the part before the " = "
			tmp = strchr(s, '=');
			*(tmp-1) = '\0';
			strupr(word);

			// Now get the part after
			word2 = tmp += 2;
			i = atoi(word2); // used for numerical settings

			// CHEAP HACK: move this over here for lowercase subtitles
			if (fastcmp(word, "SUBTITLE"))
			{
				deh_strlcpy(mapheaderinfo[num-1]->subttl, word2,
					sizeof(mapheaderinfo[num-1]->subttl), va("Level header %d: subtitle", num));
				continue;
			}

			// Lua custom options also go above, contents may be case sensitive.
			if (fastncmp(word, "LUA.", 4))
			{
#ifdef HAVE_BLUA
				UINT8 j;
				customoption_t *modoption;

				// Note: we actualy strlwr word here, so things are made a little easier for Lua
				strlwr(word);
				word += 4; // move past "lua."

				// ... and do a simple name sanity check; the name must start with a letter
				if (*word < 'a' || *word > 'z')
				{
					deh_warning("Level header %d: invalid custom option name \"%s\"", num, word);
					continue;
				}

				// Sanity limit of 128 params
				if (mapheaderinfo[num-1]->numCustomOptions == 128)
				{
					deh_warning("Level header %d: too many custom parameters", num);
					continue;
				}
				j = mapheaderinfo[num-1]->numCustomOptions++;

				mapheaderinfo[num-1]->customopts =
					Z_Realloc(mapheaderinfo[num-1]->customopts,
						sizeof(customoption_t) * mapheaderinfo[num-1]->numCustomOptions, PU_STATIC, NULL);

				// Newly allocated
				modoption = &mapheaderinfo[num-1]->customopts[j];

				strncpy(modoption->option, word,  31);
				modoption->option[31] = '\0';
				strncpy(modoption->value,  word2, 255);
				modoption->value[255] = '\0';
#else
				// Silently ignore.
#endif
				continue;
			}

			// Now go to uppercase
			strupr(word2);

			// List of flickies that are be freed in this map
			if (fastcmp(word, "FLICKYLIST") || fastcmp(word, "ANIMALLIST"))
			{
				if (fastcmp(word2, "NONE"))
					P_DeleteFlickies(num-1);
				else if (fastcmp(word2, "DEMO"))
					P_SetDemoFlickies(num-1);
				else if (fastcmp(word2, "ALL"))
				{
					mobjtype_t tmpflickies[MAXFLICKIES];

					for (mapheaderinfo[num-1]->numFlickies = 0;
					((mapheaderinfo[num-1]->numFlickies < MAXFLICKIES) && FLICKYTYPES[mapheaderinfo[num-1]->numFlickies].type);
					mapheaderinfo[num-1]->numFlickies++)
						tmpflickies[mapheaderinfo[num-1]->numFlickies] = FLICKYTYPES[mapheaderinfo[num-1]->numFlickies].type;

					if (mapheaderinfo[num-1]->numFlickies) // just in case...
					{
						size_t newsize = sizeof(mobjtype_t) * mapheaderinfo[num-1]->numFlickies;
						mapheaderinfo[num-1]->flickies = Z_Realloc(mapheaderinfo[num-1]->flickies, newsize, PU_STATIC, NULL);
						M_Memcpy(mapheaderinfo[num-1]->flickies, tmpflickies, newsize);
					}
				}
				else
				{
					mobjtype_t tmpflickies[MAXFLICKIES];
					mapheaderinfo[num-1]->numFlickies = 0;
					tmp = strtok(word2,",");
					// get up to the first MAXFLICKIES flickies
					do {
						if (mapheaderinfo[num-1]->numFlickies == MAXFLICKIES) // never going to get above that number
						{
							deh_warning("Level header %d: too many flickies\n", num);
							break;
						}

						if (fastncmp(tmp, "MT_", 3)) // support for specified mobjtypes...
						{
							i = get_mobjtype(tmp);
							if (!i)
							{
								//deh_warning("Level header %d: unknown flicky mobj type %s\n", num, tmp); -- no need for this line as get_mobjtype complains too
								continue;
							}
							tmpflickies[mapheaderinfo[num-1]->numFlickies] = i;
						}
						else // ...or a quick, limited selection of default flickies!
						{
							for (i = 0; FLICKYTYPES[i].name; i++)
								if (fastcmp(tmp, FLICKYTYPES[i].name))
									break;

							if (!FLICKYTYPES[i].name)
							{
								deh_warning("Level header %d: unknown flicky selection %s\n", num, tmp);
								continue;
							}
							tmpflickies[mapheaderinfo[num-1]->numFlickies] = FLICKYTYPES[i].type;
						}
						mapheaderinfo[num-1]->numFlickies++;
					} while ((tmp = strtok(NULL,",")) != NULL);

					if (mapheaderinfo[num-1]->numFlickies)
					{
						size_t newsize = sizeof(mobjtype_t) * mapheaderinfo[num-1]->numFlickies;
						mapheaderinfo[num-1]->flickies = Z_Realloc(mapheaderinfo[num-1]->flickies, newsize, PU_STATIC, NULL);
						// now we add them to the list!
						M_Memcpy(mapheaderinfo[num-1]->flickies, tmpflickies, newsize);
					}
					else
						deh_warning("Level header %d: no valid flicky types found\n", num);
				}
			}

			// NiGHTS grades
			else if (fastncmp(word, "GRADES", 6))
			{
				UINT8 mare = (UINT8)atoi(word + 6);

				if (mare <= 0 || mare > 8)
				{
					deh_warning("Level header %d: unknown word '%s'", num, word);
					continue;
				}

				P_AddGradesForMare((INT16)(num-1), mare-1, word2);
			}

			// Strings that can be truncated
			else if (fastcmp(word, "LEVELNAME"))
			{
				deh_strlcpy(mapheaderinfo[num-1]->lvlttl, word2,
					sizeof(mapheaderinfo[num-1]->lvlttl), va("Level header %d: levelname", num));
				strlcpy(mapheaderinfo[num-1]->selectheading, word2, sizeof(mapheaderinfo[num-1]->selectheading)); // not deh_ so only complains once
			}
			else if (fastcmp(word, "SELECTHEADING"))
			{
				deh_strlcpy(mapheaderinfo[num-1]->selectheading, word2,
					sizeof(mapheaderinfo[num-1]->selectheading), va("Level header %d: selectheading", num));
			}
			else if (fastcmp(word, "SCRIPTNAME"))
			{
				deh_strlcpy(mapheaderinfo[num-1]->scriptname, word2,
					sizeof(mapheaderinfo[num-1]->scriptname), va("Level header %d: scriptname", num));
			}
			else if (fastcmp(word, "RUNSOC"))
			{
				deh_strlcpy(mapheaderinfo[num-1]->runsoc, word2,
					sizeof(mapheaderinfo[num-1]->runsoc), va("Level header %d: runsoc", num));
			}
			else if (fastcmp(word, "ACT"))
			{
				if (i >= 0 && i < 20) // 0 for no act number, TTL1 through TTL19
					mapheaderinfo[num-1]->actnum = (UINT8)i;
				else
					deh_warning("Level header %d: invalid act number %d", num, i);
			}
			else if (fastcmp(word, "NEXTLEVEL"))
			{
				if      (fastcmp(word2, "TITLE"))      i = 1100;
				else if (fastcmp(word2, "EVALUATION")) i = 1101;
				else if (fastcmp(word2, "CREDITS"))    i = 1102;
				else
				// Support using the actual map name,
				// i.e., Nextlevel = AB, Nextlevel = FZ, etc.

				// Convert to map number
				if (word2[0] >= 'A' && word2[0] <= 'Z' && word2[2] == '\0')
					i = M_MapNumber(word2[0], word2[1]);

				mapheaderinfo[num-1]->nextlevel = (INT16)i;
			}
			else if (fastcmp(word, "TYPEOFLEVEL"))
			{
				if (i) // it's just a number
					mapheaderinfo[num-1]->typeoflevel = (UINT16)i;
				else
				{
					UINT16 tol = 0;
					tmp = strtok(word2,",");
					do {
						for (i = 0; TYPEOFLEVEL[i].name; i++)
							if (fastcmp(tmp, TYPEOFLEVEL[i].name))
								break;
						if (!TYPEOFLEVEL[i].name)
							deh_warning("Level header %d: unknown typeoflevel flag %s\n", num, tmp);
						tol |= TYPEOFLEVEL[i].flag;
					} while((tmp = strtok(NULL,",")) != NULL);
					mapheaderinfo[num-1]->typeoflevel = tol;
				}
			}
			else if (fastcmp(word, "MUSIC"))
			{
				if (fastcmp(word2, "NONE"))
					mapheaderinfo[num-1]->musname[0] = 0; // becomes empty string
				else
				{
					deh_strlcpy(mapheaderinfo[num-1]->musname, word2,
						sizeof(mapheaderinfo[num-1]->musname), va("Level header %d: music", num));
				}
			}
#ifdef MUSICSLOT_COMPATIBILITY
			else if (fastcmp(word, "MUSICSLOT"))
			{
				i = get_mus(word2, true);
				if (i && i <= 1035)
					snprintf(mapheaderinfo[num-1]->musname, 7, "%sM", G_BuildMapName(i));
				else if (i && i <= 1050)
					strncpy(mapheaderinfo[num-1]->musname, compat_special_music_slots[i - 1036], 7);
				else
					mapheaderinfo[num-1]->musname[0] = 0; // becomes empty string
				mapheaderinfo[num-1]->musname[6] = 0;
			}
#endif
			else if (fastcmp(word, "MUSICTRACK"))
				mapheaderinfo[num-1]->mustrack = ((UINT16)i - 1);
			else if (fastcmp(word, "FORCECHARACTER"))
			{
				strlcpy(mapheaderinfo[num-1]->forcecharacter, word2, SKINNAMESIZE+1);
				strlwr(mapheaderinfo[num-1]->forcecharacter); // skin names are lowercase
			}
			else if (fastcmp(word, "WEATHER"))
				mapheaderinfo[num-1]->weather = (UINT8)get_number(word2);
			else if (fastcmp(word, "SKYNUM"))
				mapheaderinfo[num-1]->skynum = (INT16)i;
			else if (fastcmp(word, "INTERSCREEN"))
				strncpy(mapheaderinfo[num-1]->interscreen, word2, 8);
			else if (fastcmp(word, "PRECUTSCENENUM"))
				mapheaderinfo[num-1]->precutscenenum = (UINT8)i;
			else if (fastcmp(word, "CUTSCENENUM"))
				mapheaderinfo[num-1]->cutscenenum = (UINT8)i;
			else if (fastcmp(word, "COUNTDOWN"))
				mapheaderinfo[num-1]->countdown = (INT16)i;
			else if (fastcmp(word, "PALETTE"))
				mapheaderinfo[num-1]->palette = (UINT16)i;
			else if (fastcmp(word, "NUMLAPS"))
				mapheaderinfo[num-1]->numlaps = (UINT8)i;
			else if (fastcmp(word, "UNLOCKABLE"))
			{
				if (i >= 0 && i <= MAXUNLOCKABLES) // 0 for no unlock required, anything else requires something
					mapheaderinfo[num-1]->unlockrequired = (SINT8)i - 1;
				else
					deh_warning("Level header %d: invalid unlockable number %d", num, i);
			}
			else if (fastcmp(word, "LEVELSELECT"))
				mapheaderinfo[num-1]->levelselect = (UINT8)i;
			else if (fastcmp(word, "SKYBOXSCALE"))
				mapheaderinfo[num-1]->skybox_scalex = mapheaderinfo[num-1]->skybox_scaley = mapheaderinfo[num-1]->skybox_scalez = (INT16)i;
			else if (fastcmp(word, "SKYBOXSCALEX"))
				mapheaderinfo[num-1]->skybox_scalex = (INT16)i;
			else if (fastcmp(word, "SKYBOXSCALEY"))
				mapheaderinfo[num-1]->skybox_scaley = (INT16)i;
			else if (fastcmp(word, "SKYBOXSCALEZ"))
				mapheaderinfo[num-1]->skybox_scalez = (INT16)i;

			else if (fastcmp(word, "BONUSTYPE"))
			{
				if      (fastcmp(word2, "NONE"))   i = -1;
				else if (fastcmp(word2, "NORMAL")) i =  0;
				else if (fastcmp(word2, "BOSS"))   i =  1;
				else if (fastcmp(word2, "ERZ3"))   i =  2;

				if (i >= -1 && i <= 2) // -1 for no bonus. Max is 2.
					mapheaderinfo[num-1]->bonustype = (SINT8)i;
				else
					deh_warning("Level header %d: invalid bonus type number %d", num, i);
			}

			else if (fastcmp(word, "LEVELFLAGS"))
				mapheaderinfo[num-1]->levelflags = (UINT8)i;
			else if (fastcmp(word, "MENUFLAGS"))
				mapheaderinfo[num-1]->menuflags = (UINT8)i;

			// Individual triggers for level flags, for ease of use (and 2.0 compatibility)
			else if (fastcmp(word, "SCRIPTISFILE"))
			{
				if (i || word2[0] == 'T' || word2[0] == 'Y')
					mapheaderinfo[num-1]->levelflags |= LF_SCRIPTISFILE;
				else
					mapheaderinfo[num-1]->levelflags &= ~LF_SCRIPTISFILE;
			}
			else if (fastcmp(word, "SPEEDMUSIC"))
			{
				if (i || word2[0] == 'T' || word2[0] == 'Y')
					mapheaderinfo[num-1]->levelflags |= LF_SPEEDMUSIC;
				else
					mapheaderinfo[num-1]->levelflags &= ~LF_SPEEDMUSIC;
			}
			else if (fastcmp(word, "NOSSMUSIC"))
			{
				if (i || word2[0] == 'T' || word2[0] == 'Y')
					mapheaderinfo[num-1]->levelflags |= LF_NOSSMUSIC;
				else
					mapheaderinfo[num-1]->levelflags &= ~LF_NOSSMUSIC;
			}
			else if (fastcmp(word, "NORELOAD"))
			{
				if (i || word2[0] == 'T' || word2[0] == 'Y')
					mapheaderinfo[num-1]->levelflags |= LF_NORELOAD;
				else
					mapheaderinfo[num-1]->levelflags &= ~LF_NORELOAD;
			}
			else if (fastcmp(word, "NOZONE"))
			{
				if (i || word2[0] == 'T' || word2[0] == 'Y')
					mapheaderinfo[num-1]->levelflags |= LF_NOZONE;
				else
					mapheaderinfo[num-1]->levelflags &= ~LF_NOZONE;
			}
			else if (fastcmp(word, "SAVEGAME"))
			{
				if (i || word2[0] == 'T' || word2[0] == 'Y')
					mapheaderinfo[num-1]->levelflags |= LF_SAVEGAME;
				else
					mapheaderinfo[num-1]->levelflags &= ~LF_SAVEGAME;
			}

			// Individual triggers for menu flags
			else if (fastcmp(word, "HIDDEN"))
			{
				if (i || word2[0] == 'T' || word2[0] == 'Y')
					mapheaderinfo[num-1]->menuflags |= LF2_HIDEINMENU;
				else
					mapheaderinfo[num-1]->menuflags &= ~LF2_HIDEINMENU;
			}
			else if (fastcmp(word, "HIDEINSTATS"))
			{
				if (i || word2[0] == 'T' || word2[0] == 'Y')
					mapheaderinfo[num-1]->menuflags |= LF2_HIDEINSTATS;
				else
					mapheaderinfo[num-1]->menuflags &= ~LF2_HIDEINSTATS;
			}
			else if (fastcmp(word, "RECORDATTACK") || fastcmp(word, "TIMEATTACK"))
			{ // TIMEATTACK is an accepted alias
				if (i || word2[0] == 'T' || word2[0] == 'Y')
					mapheaderinfo[num-1]->menuflags |= LF2_RECORDATTACK;
				else
					mapheaderinfo[num-1]->menuflags &= ~LF2_RECORDATTACK;
			}
			else if (fastcmp(word, "NIGHTSATTACK"))
			{
				if (i || word2[0] == 'T' || word2[0] == 'Y')
					mapheaderinfo[num-1]->menuflags |= LF2_NIGHTSATTACK;
				else
					mapheaderinfo[num-1]->menuflags &= LF2_NIGHTSATTACK;
			}
			else if (fastcmp(word, "NOVISITNEEDED"))
			{
				if (i || word2[0] == 'T' || word2[0] == 'Y')
					mapheaderinfo[num-1]->menuflags |= LF2_NOVISITNEEDED;
				else
					mapheaderinfo[num-1]->menuflags &= ~LF2_NOVISITNEEDED;
			}
			else if (fastcmp(word, "WIDEICON"))
			{
				if (i || word2[0] == 'T' || word2[0] == 'Y')
					mapheaderinfo[num-1]->menuflags |= LF2_WIDEICON;
				else
					mapheaderinfo[num-1]->menuflags &= ~LF2_WIDEICON;
			}
			else
				deh_warning("Level header %d: unknown word '%s'", num, word);
		}
	} while (!myfeof(f)); // finish when the line is empty

	Z_Free(s);
}

#undef MAXFLICKIES

static void readcutscenescene(MYFILE *f, INT32 num, INT32 scenenum)
{
	char *s = Z_Calloc(MAXLINELEN, PU_STATIC, NULL);
	char *word;
	char *word2;
	INT32 i;
	UINT16 usi;
	UINT8 picid;

	do
	{
		if (myfgets(s, MAXLINELEN, f))
		{
			if (s[0] == '\n')
				break;

			word = strtok(s, " ");
			if (word)
				strupr(word);
			else
				break;

			if (fastcmp(word, "SCENETEXT"))
			{
				char *scenetext = NULL;
				char *buffer;
				const int bufferlen = 4096;

				for (i = 0; i < MAXLINELEN; i++)
				{
					if (s[i] == '=')
					{
						scenetext = &s[i+2];
						break;
					}
				}

				if (!scenetext)
				{
					Z_Free(cutscenes[num]->scene[scenenum].text);
					cutscenes[num]->scene[scenenum].text = NULL;
					continue;
				}

				for (i = 0; i < MAXLINELEN; i++)
				{
					if (s[i] == '\0')
					{
						s[i] = '\n';
						s[i+1] = '\0';
						break;
					}
				}

				buffer = Z_Malloc(4096, PU_STATIC, NULL);
				strcpy(buffer, scenetext);

				strcat(buffer,
					myhashfgets(scenetext, bufferlen
					- strlen(buffer) - 1, f));

				// A cutscene overwriting another one...
				Z_Free(cutscenes[num]->scene[scenenum].text);

				cutscenes[num]->scene[scenenum].text = Z_StrDup(buffer);

				Z_Free(buffer);

				continue;
			}

			word2 = strtok(NULL, " = ");
			if (word2)
				strupr(word2);
			else
				break;

			if (word2[strlen(word2)-1] == '\n')
				word2[strlen(word2)-1] = '\0';
			i = atoi(word2);
			usi = (UINT16)i;


			if (fastcmp(word, "NUMBEROFPICS"))
			{
				cutscenes[num]->scene[scenenum].numpics = (UINT8)i;
			}
			else if (fastncmp(word, "PIC", 3))
			{
				picid = (UINT8)atoi(word + 3);
				if (picid > 8 || picid == 0)
				{
					deh_warning("CutSceneScene %d: unknown word '%s'", num, word);
					continue;
				}
				--picid;

				if (fastcmp(word+4, "NAME"))
				{
					strncpy(cutscenes[num]->scene[scenenum].picname[picid], word2, 8);
				}
				else if (fastcmp(word+4, "HIRES"))
				{
					cutscenes[num]->scene[scenenum].pichires[picid] = (UINT8)(i || word2[0] == 'T' || word2[0] == 'Y');
				}
				else if (fastcmp(word+4, "DURATION"))
				{
					cutscenes[num]->scene[scenenum].picduration[picid] = usi;
				}
				else if (fastcmp(word+4, "XCOORD"))
				{
					cutscenes[num]->scene[scenenum].xcoord[picid] = usi;
				}
				else if (fastcmp(word+4, "YCOORD"))
				{
					cutscenes[num]->scene[scenenum].ycoord[picid] = usi;
				}
				else
					deh_warning("CutSceneScene %d: unknown word '%s'", num, word);
			}
			else if (fastcmp(word, "MUSIC"))
			{
				strncpy(cutscenes[num]->scene[scenenum].musswitch, word2, 7);
				cutscenes[num]->scene[scenenum].musswitch[6] = 0;
			}
#ifdef MUSICSLOT_COMPATIBILITY
			else if (fastcmp(word, "MUSICSLOT"))
			{
				i = get_mus(word2, true);
				if (i && i <= 1035)
					snprintf(cutscenes[num]->scene[scenenum].musswitch, 7, "%sM", G_BuildMapName(i));
				else if (i && i <= 1050)
					strncpy(cutscenes[num]->scene[scenenum].musswitch, compat_special_music_slots[i - 1036], 7);
				else
					cutscenes[num]->scene[scenenum].musswitch[0] = 0; // becomes empty string
				cutscenes[num]->scene[scenenum].musswitch[6] = 0;
			}
#endif
			else if (fastcmp(word, "MUSICTRACK"))
			{
				cutscenes[num]->scene[scenenum].musswitchflags = ((UINT16)i) & MUSIC_TRACKMASK;
			}
			else if (fastcmp(word, "MUSICLOOP"))
			{
				cutscenes[num]->scene[scenenum].musicloop = (UINT8)(i || word2[0] == 'T' || word2[0] == 'Y');
			}
			else if (fastcmp(word, "TEXTXPOS"))
			{
				cutscenes[num]->scene[scenenum].textxpos = usi;
			}
			else if (fastcmp(word, "TEXTYPOS"))
			{
				cutscenes[num]->scene[scenenum].textypos = usi;
			}
			else if (fastcmp(word, "FADEINID"))
			{
				cutscenes[num]->scene[scenenum].fadeinid = (UINT8)i;
			}
			else if (fastcmp(word, "FADEOUTID"))
			{
				cutscenes[num]->scene[scenenum].fadeoutid = (UINT8)i;
			}
			else if (fastcmp(word, "FADECOLOR"))
			{
				cutscenes[num]->scene[scenenum].fadecolor = (UINT8)i;
			}
			else
				deh_warning("CutSceneScene %d: unknown word '%s'", num, word);
		}
	} while (!myfeof(f)); // finish when the line is empty

	Z_Free(s);
}

static void readcutscene(MYFILE *f, INT32 num)
{
	char *s = Z_Malloc(MAXLINELEN, PU_STATIC, NULL);
	char *word;
	char *word2;
	char *tmp;
	INT32 value;

	// Allocate memory for this cutscene if we don't yet have any
	if (!cutscenes[num])
		cutscenes[num] = Z_Calloc(sizeof (cutscene_t), PU_STATIC, NULL);

	do
	{
		if (myfgets(s, MAXLINELEN, f))
		{
			if (s[0] == '\n')
				break;

			tmp = strchr(s, '#');
			if (tmp)
				*tmp = '\0';
			if (s == tmp)
				continue; // Skip comment lines, but don't break.

			word = strtok(s, " ");
			if (word)
				strupr(word);
			else
				break;

			word2 = strtok(NULL, " ");
			if (word2)
				value = atoi(word2);
			else
			{
				deh_warning("No value for token %s", word);
				continue;
			}

			if (fastcmp(word, "NUMSCENES"))
			{
				cutscenes[num]->numscenes = value;
			}
			else if (fastcmp(word, "SCENE"))
			{
				if (1 <= value && value <= 128)
				{
					readcutscenescene(f, num, value - 1);
				}
				else
					deh_warning("Scene number %d out of range (1 - 128)", value);

			}
			else
				deh_warning("Cutscene %d: unknown word '%s', Scene <num> expected.", num, word);
		}
	} while (!myfeof(f)); // finish when the line is empty

	Z_Free(s);
}

static void readhuditem(MYFILE *f, INT32 num)
{
	char *s = Z_Malloc(MAXLINELEN, PU_STATIC, NULL);
	char *word = s;
	char *word2;
	char *tmp;
	INT32 i;

	do
	{
		if (myfgets(s, MAXLINELEN, f))
		{
			if (s[0] == '\n')
				break;

			// First remove trailing newline, if there is one
			tmp = strchr(s, '\n');
			if (tmp)
				*tmp = '\0';

			tmp = strchr(s, '#');
			if (tmp)
				*tmp = '\0';
			if (s == tmp)
				continue; // Skip comment lines, but don't break.

			// Get the part before the " = "
			tmp = strchr(s, '=');
			*(tmp-1) = '\0';
			strupr(word);

			// Now get the part after
			word2 = tmp += 2;
			strupr(word2);

			i = atoi(word2); // used for numerical settings

			if (fastcmp(word, "X"))
			{
				hudinfo[num].x = i;
			}
			else if (fastcmp(word, "Y"))
			{
				hudinfo[num].y = i;
			}
			else
				deh_warning("Level header %d: unknown word '%s'", num, word);
		}
	} while (!myfeof(f)); // finish when the line is empty

	Z_Free(s);
}

/*
Sprite number = 10
Sprite subnumber = 32968
Duration = 200
Next frame = 200
*/

/** Action pointer for reading actions from Dehacked lumps.
  */
typedef struct
{
	actionf_t action; ///< Function pointer corresponding to the actual action.
	const char *name; ///< Name of the action in ALL CAPS.
} actionpointer_t;

/** Array mapping action names to action functions.
  * Names must be in ALL CAPS for case insensitive comparisons.
  */
static actionpointer_t actionpointers[] =
{
	{{A_Explode},              "A_EXPLODE"},
	{{A_Pain},                 "A_PAIN"},
	{{A_Fall},                 "A_FALL"},
	{{A_MonitorPop},           "A_MONITORPOP"},
	{{A_GoldMonitorPop},       "A_GOLDMONITORPOP"},
	{{A_GoldMonitorRestore},   "A_GOLDMONITORRESTORE"},
	{{A_GoldMonitorSparkle},   "A_GOLDMONITORSPARKLE"},
	{{A_Look},                 "A_LOOK"},
	{{A_Chase},                "A_CHASE"},
	{{A_FaceStabChase},        "A_FACESTABCHASE"},
	{{A_FaceTarget},           "A_FACETARGET"},
	{{A_FaceTracer},           "A_FACETRACER"},
	{{A_Scream},               "A_SCREAM"},
	{{A_BossDeath},            "A_BOSSDEATH"},
	{{A_CustomPower},          "A_CUSTOMPOWER"},
	{{A_GiveWeapon},           "A_GIVEWEAPON"},
	{{A_RingBox},              "A_RINGBOX"},
	{{A_Invincibility},        "A_INVINCIBILITY"},
	{{A_SuperSneakers},        "A_SUPERSNEAKERS"},
	{{A_BunnyHop},             "A_BUNNYHOP"},
	{{A_BubbleSpawn},          "A_BUBBLESPAWN"},
	{{A_FanBubbleSpawn},       "A_FANBUBBLESPAWN"},
	{{A_BubbleRise},           "A_BUBBLERISE"},
	{{A_BubbleCheck},          "A_BUBBLECHECK"},
	{{A_AwardScore},           "A_AWARDSCORE"},
	{{A_ExtraLife},            "A_EXTRALIFE"},
	{{A_GiveShield},           "A_GIVESHIELD"},
	{{A_GravityBox},           "A_GRAVITYBOX"},
	{{A_ScoreRise},            "A_SCORERISE"},
	{{A_ParticleSpawn},        "A_PARTICLESPAWN"},
	{{A_AttractChase},         "A_ATTRACTCHASE"},
	{{A_DropMine},             "A_DROPMINE"},
	{{A_FishJump},             "A_FISHJUMP"},
	{{A_ThrownRing},           "A_THROWNRING"},
	{{A_SetSolidSteam},        "A_SETSOLIDSTEAM"},
	{{A_UnsetSolidSteam},      "A_UNSETSOLIDSTEAM"},
	{{A_SignPlayer},           "A_SIGNPLAYER"},
	{{A_OverlayThink},         "A_OVERLAYTHINK"},
	{{A_JetChase},             "A_JETCHASE"},
	{{A_JetbThink},            "A_JETBTHINK"},
	{{A_JetgThink},            "A_JETGTHINK"},
	{{A_JetgShoot},            "A_JETGSHOOT"},
	{{A_ShootBullet},          "A_SHOOTBULLET"},
	{{A_MinusDigging},         "A_MINUSDIGGING"},
	{{A_MinusPopup},           "A_MINUSPOPUP"},
	{{A_MinusCheck},           "A_MINUSCHECK"},
	{{A_ChickenCheck},         "A_CHICKENCHECK"},
	{{A_MouseThink},           "A_MOUSETHINK"},
	{{A_DetonChase},           "A_DETONCHASE"},
	{{A_CapeChase},            "A_CAPECHASE"},
	{{A_RotateSpikeBall},      "A_ROTATESPIKEBALL"},
	{{A_SlingAppear},          "A_SLINGAPPEAR"},
	{{A_UnidusBall},           "A_UNIDUSBALL"},
	{{A_RockSpawn},            "A_ROCKSPAWN"},
	{{A_SetFuse},              "A_SETFUSE"},
	{{A_CrawlaCommanderThink}, "A_CRAWLACOMMANDERTHINK"},
	{{A_SmokeTrailer},         "A_SMOKETRAILER"},
	{{A_RingExplode},          "A_RINGEXPLODE"},
	{{A_OldRingExplode},       "A_OLDRINGEXPLODE"},
	{{A_MixUp},                "A_MIXUP"},
	{{A_RecyclePowers},        "A_RECYCLEPOWERS"},
	{{A_Boss1Chase},           "A_BOSS1CHASE"},
	{{A_FocusTarget},          "A_FOCUSTARGET"},
	{{A_Boss2Chase},           "A_BOSS2CHASE"},
	{{A_Boss2Pogo},            "A_BOSS2POGO"},
	{{A_BossZoom},             "A_BOSSZOOM"},
	{{A_BossScream},           "A_BOSSSCREAM"},
	{{A_Boss2TakeDamage},      "A_BOSS2TAKEDAMAGE"},
	{{A_Boss7Chase},           "A_BOSS7CHASE"},
	{{A_GoopSplat},            "A_GOOPSPLAT"},
	{{A_Boss2PogoSFX},         "A_BOSS2POGOSFX"},
	{{A_Boss2PogoTarget},      "A_BOSS2POGOTARGET"},
	{{A_BossJetFume},          "A_BOSSJETFUME"},
	{{A_EggmanBox},            "A_EGGMANBOX"},
	{{A_TurretFire},           "A_TURRETFIRE"},
	{{A_SuperTurretFire},      "A_SUPERTURRETFIRE"},
	{{A_TurretStop},           "A_TURRETSTOP"},
	{{A_JetJawRoam},           "A_JETJAWROAM"},
	{{A_JetJawChomp},          "A_JETJAWCHOMP"},
	{{A_PointyThink},          "A_POINTYTHINK"},
	{{A_CheckBuddy},           "A_CHECKBUDDY"},
	{{A_HoodThink},            "A_HOODTHINK"},
	{{A_ArrowCheck},           "A_ARROWCHECK"},
	{{A_SnailerThink},         "A_SNAILERTHINK"},
	{{A_SharpChase},           "A_SHARPCHASE"},
	{{A_SharpSpin},            "A_SHARPSPIN"},
	{{A_VultureVtol},          "A_VULTUREVTOL"},
	{{A_VultureCheck},         "A_VULTURECHECK"},
	{{A_SkimChase},            "A_SKIMCHASE"},
	{{A_1upThinker},           "A_1UPTHINKER"},
	{{A_SkullAttack},          "A_SKULLATTACK"},
	{{A_LobShot},              "A_LOBSHOT"},
	{{A_FireShot},             "A_FIRESHOT"},
	{{A_SuperFireShot},        "A_SUPERFIRESHOT"},
	{{A_BossFireShot},         "A_BOSSFIRESHOT"},
	{{A_Boss7FireMissiles},    "A_BOSS7FIREMISSILES"},
	{{A_Boss1Laser},           "A_BOSS1LASER"},
	{{A_Boss4Reverse},         "A_BOSS4REVERSE"},
	{{A_Boss4SpeedUp},         "A_BOSS4SPEEDUP"},
	{{A_Boss4Raise},           "A_BOSS4RAISE"},
	{{A_SparkFollow},          "A_SPARKFOLLOW"},
	{{A_BuzzFly},              "A_BUZZFLY"},
	{{A_GuardChase},           "A_GUARDCHASE"},
	{{A_EggShield},            "A_EGGSHIELD"},
	{{A_SetReactionTime},      "A_SETREACTIONTIME"},
	{{A_Boss1Spikeballs},      "A_BOSS1SPIKEBALLS"},
	{{A_Boss3TakeDamage},      "A_BOSS3TAKEDAMAGE"},
	{{A_Boss3Path},            "A_BOSS3PATH"},
	{{A_LinedefExecute},       "A_LINEDEFEXECUTE"},
	{{A_PlaySeeSound},         "A_PLAYSEESOUND"},
	{{A_PlayAttackSound},      "A_PLAYATTACKSOUND"},
	{{A_PlayActiveSound},      "A_PLAYACTIVESOUND"},
	{{A_SpawnObjectAbsolute},  "A_SPAWNOBJECTABSOLUTE"},
	{{A_SpawnObjectRelative},  "A_SPAWNOBJECTRELATIVE"},
	{{A_ChangeAngleRelative},  "A_CHANGEANGLERELATIVE"},
	{{A_ChangeAngleAbsolute},  "A_CHANGEANGLEABSOLUTE"},
	{{A_PlaySound},            "A_PLAYSOUND"},
	{{A_FindTarget},           "A_FINDTARGET"},
	{{A_FindTracer},           "A_FINDTRACER"},
	{{A_SetTics},              "A_SETTICS"},
	{{A_SetRandomTics},        "A_SETRANDOMTICS"},
	{{A_ChangeColorRelative},  "A_CHANGECOLORRELATIVE"},
	{{A_ChangeColorAbsolute},  "A_CHANGECOLORABSOLUTE"},
	{{A_MoveRelative},         "A_MOVERELATIVE"},
	{{A_MoveAbsolute},         "A_MOVEABSOLUTE"},
	{{A_Thrust},               "A_THRUST"},
	{{A_ZThrust},              "A_ZTHRUST"},
	{{A_SetTargetsTarget},     "A_SETTARGETSTARGET"},
	{{A_SetObjectFlags},       "A_SETOBJECTFLAGS"},
	{{A_SetObjectFlags2},      "A_SETOBJECTFLAGS2"},
	{{A_RandomState},          "A_RANDOMSTATE"},
	{{A_RandomStateRange},     "A_RANDOMSTATERANGE"},
	{{A_DualAction},           "A_DUALACTION"},
	{{A_RemoteAction},         "A_REMOTEACTION"},
	{{A_ToggleFlameJet},       "A_TOGGLEFLAMEJET"},
	{{A_OrbitNights},          "A_ORBITNIGHTS"},
	{{A_GhostMe},              "A_GHOSTME"},
	{{A_SetObjectState},       "A_SETOBJECTSTATE"},
	{{A_SetObjectTypeState},   "A_SETOBJECTTYPESTATE"},
	{{A_KnockBack},            "A_KNOCKBACK"},
	{{A_PushAway},             "A_PUSHAWAY"},
	{{A_RingDrain},            "A_RINGDRAIN"},
	{{A_SplitShot},            "A_SPLITSHOT"},
	{{A_MissileSplit},         "A_MISSILESPLIT"},
	{{A_MultiShot},            "A_MULTISHOT"},
	{{A_InstaLoop},            "A_INSTALOOP"},
	{{A_Custom3DRotate},       "A_CUSTOM3DROTATE"},
	{{A_SearchForPlayers},     "A_SEARCHFORPLAYERS"},
	{{A_CheckRandom},          "A_CHECKRANDOM"},
	{{A_CheckTargetRings},     "A_CHECKTARGETRINGS"},
	{{A_CheckRings},           "A_CHECKRINGS"},
	{{A_CheckTotalRings},      "A_CHECKTOTALRINGS"},
	{{A_CheckHealth},          "A_CHECKHEALTH"},
	{{A_CheckRange},           "A_CHECKRANGE"},
	{{A_CheckHeight},          "A_CHECKHEIGHT"},
	{{A_CheckTrueRange},       "A_CHECKTRUERANGE"},
	{{A_CheckThingCount},      "A_CHECKTHINGCOUNT"},
	{{A_CheckAmbush},          "A_CHECKAMBUSH"},
	{{A_CheckCustomValue},     "A_CHECKCUSTOMVALUE"},
	{{A_CheckCusValMemo},      "A_CHECKCUSVALMEMO"},
	{{A_SetCustomValue},       "A_SETCUSTOMVALUE"},
	{{A_UseCusValMemo},        "A_USECUSVALMEMO"},
	{{A_RelayCustomValue},     "A_RELAYCUSTOMVALUE"},
	{{A_CusValAction},         "A_CUSVALACTION"},
	{{A_ForceStop},            "A_FORCESTOP"},
	{{A_ForceWin},             "A_FORCEWIN"},
	{{A_SpikeRetract},         "A_SPIKERETRACT"},
	{{A_InfoState},            "A_INFOSTATE"},
	{{A_Repeat},               "A_REPEAT"},
	{{A_SetScale},             "A_SETSCALE"},
	{{A_RemoteDamage},         "A_REMOTEDAMAGE"},
	{{A_HomingChase},          "A_HOMINGCHASE"},
	{{A_TrapShot},             "A_TRAPSHOT"},
	{{A_VileTarget},           "A_VILETARGET"},
	{{A_VileAttack},           "A_VILEATTACK"},
	{{A_VileFire},             "A_VILEFIRE"},
	{{A_BrakChase},            "A_BRAKCHASE"},
	{{A_BrakFireShot},         "A_BRAKFIRESHOT"},
	{{A_BrakLobShot},          "A_BRAKLOBSHOT"},
	{{A_NapalmScatter},        "A_NAPALMSCATTER"},
	{{A_SpawnFreshCopy},       "A_SPAWNFRESHCOPY"},
	{{A_FlickySpawn},          "A_FLICKYSPAWN"},
	{{A_FlickyAim},            "A_FLICKYAIM"},
	{{A_FlickyFly},            "A_FLICKYFLY"},
	{{A_FlickySoar},           "A_FLICKYSOAR"},
	{{A_FlickyCoast},          "A_FLICKYCOAST"},
	{{A_FlickyHop},            "A_FLICKYHOP"},
	{{A_FlickyFlounder},       "A_FLICKYFLOUNDER"},
	{{A_FlickyCheck},          "A_FLICKYCHECK"},
	{{A_FlickyHeightCheck},    "A_FLICKYHEIGHTCHECK"},
	{{A_FlickyFlutter},        "A_FLICKYFLUTTER"},
	{{A_FlameParticle},        "A_FLAMEPARTICLE"},

	{{NULL},                   "NONE"},

	// This NULL entry must be the last in the list
	{{NULL},                   NULL},
};

static void readframe(MYFILE *f, INT32 num)
{
	char *s = Z_Malloc(MAXLINELEN, PU_STATIC, NULL);
	char *word1;
	char *word2 = NULL;
	char *tmp;
	INT32 j;

	do
	{
		if (myfgets(s, MAXLINELEN, f))
		{
			if (s[0] == '\n')
				break;

			tmp = strchr(s, '#');
			if (tmp)
				*tmp = '\0';
			if (s == tmp)
				continue; // Skip comment lines, but don't break.

			for (j = 0; s[j] != '\n'; j++)
			{
				if (s[j] == '=')
				{
					j += 2;
					j = atoi(&s[j]);
					break;
				}
			}

			word1 = strtok(s, " ");
			if (word1)
				strupr(word1);
			else
				break;

			word2 = strtok(NULL, " = ");
			if (word2)
				strupr(word2);
			else
				break;
			if (word2[strlen(word2)-1] == '\n')
				word2[strlen(word2)-1] = '\0';

			if (fastcmp(word1, "SPRITENUMBER") || fastcmp(word1, "SPRITENAME"))
			{
				states[num].sprite = get_sprite(word2);
			}
			else if (fastcmp(word1, "SPRITESUBNUMBER") || fastcmp(word1, "SPRITEFRAME"))
			{
				states[num].frame = (INT32)get_number(word2); // So the FF_ flags get calculated
			}
			else if (fastcmp(word1, "DURATION"))
			{
				states[num].tics = (INT32)get_number(word2); // So TICRATE can be used
			}
			else if (fastcmp(word1, "NEXT"))
			{
				states[num].nextstate = get_state(word2);
			}
			else if (fastcmp(word1, "VAR1"))
			{
				states[num].var1 = (INT32)get_number(word2);
			}
			else if (fastcmp(word1, "VAR2"))
			{
				states[num].var2 = (INT32)get_number(word2);
			}
			else if (fastcmp(word1, "ACTION"))
			{
				size_t z;
				boolean found = false;
				XBOXSTATIC char actiontocompare[32];

				memset(actiontocompare, 0x00, sizeof(actiontocompare));
				strlcpy(actiontocompare, word2, sizeof (actiontocompare));
				strupr(actiontocompare);

				for (z = 0; z < 32; z++)
				{
					if (actiontocompare[z] == '\n' || actiontocompare[z] == '\r')
					{
						actiontocompare[z] = '\0';
						break;
					}
				}

				for (z = 0; actionpointers[z].name; z++)
				{
					if (actionpointers[z].action.acv == states[num].action.acv)
						break;
				}

				z = 0;
#ifdef HAVE_BLUA
				found = LUA_SetLuaAction(&states[num], actiontocompare);
				if (!found)
#endif
				while (actionpointers[z].name)
				{
					if (fastcmp(actiontocompare, actionpointers[z].name))
					{
						states[num].action = actionpointers[z].action;
						states[num].action.acv = actionpointers[z].action.acv; // assign
						states[num].action.acp1 = actionpointers[z].action.acp1;
						found = true;
						break;
					}
					z++;
				}

				if (!found)
					deh_warning("Unknown action %s", actiontocompare);
			}
			else
				deh_warning("Frame %d: unknown word '%s'", num, word1);
		}
	} while (!myfeof(f));

	Z_Free(s);
}

static void readsound(MYFILE *f, INT32 num)
{
	char *s = Z_Malloc(MAXLINELEN, PU_STATIC, NULL);
	char *word;
	char *word2;
	char *tmp;
	INT32 value;

	do
	{
		if (myfgets(s, MAXLINELEN, f))
		{
			if (s[0] == '\n')
				break;

			tmp = strchr(s, '#');
			if (tmp)
				*tmp = '\0';
			if (s == tmp)
				continue; // Skip comment lines, but don't break.

			word = strtok(s, " ");
			if (word)
				strupr(word);
			else
				break;

			word2 = strtok(NULL, " ");
			if (word2)
				value = atoi(word2);
			else
			{
				deh_warning("No value for token %s", word);
				continue;
			}

			if (fastcmp(word, "SINGULAR"))
			{
				S_sfx[num].singularity = value;
			}
			else if (fastcmp(word, "PRIORITY"))
			{
				S_sfx[num].priority = value;
			}
			else if (fastcmp(word, "FLAGS"))
			{
				S_sfx[num].pitch = value;
			}
			else if (fastcmp(word, "CAPTION") || fastcmp(word, "DESCRIPTION"))
			{
				deh_strlcpy(S_sfx[num].caption, word2,
					sizeof(S_sfx[num].caption), va("Sound effect %d: caption", num));
			}
			else
				deh_warning("Sound %d : unknown word '%s'",num,word);
		}
	} while (!myfeof(f));

	Z_Free(s);
}

/** Checks if a game data file name for a mod is good.
 * "Good" means that it contains only alphanumerics, _, and -;
 * ends in ".dat"; has at least one character before the ".dat";
 * and is not "gamedata.dat" (tested case-insensitively).
 *
 * Assumption: that gamedata.dat is the only .dat file that will
 * ever be treated specially by the game.
 *
 * Note: Check for the tail ".dat" case-insensitively since at
 * present, we get passed the filename in all uppercase.
 *
 * \param s Filename string to check.
 * \return True if the filename is good.
 * \sa readmaincfg()
 * \author Graue <graue@oceanbase.org>
 */
static boolean GoodDataFileName(const char *s)
{
	const char *p;
	const char *tail = ".dat";

	for (p = s; *p != '\0'; p++)
		if (!isalnum(*p) && *p != '_' && *p != '-' && *p != '.')
			return false;

	p = s + strlen(s) - strlen(tail);
	if (p <= s) return false; // too short
	if (!fasticmp(p, tail)) return false; // doesn't end in .dat
	if (fasticmp(s, "gamedata.dat")) return false;

	return true;
}

static void reademblemdata(MYFILE *f, INT32 num)
{
	char *s = Z_Malloc(MAXLINELEN, PU_STATIC, NULL);
	char *word = s;
	char *word2;
	char *tmp;
	INT32 value;

	memset(&emblemlocations[num-1], 0, sizeof(emblem_t));

	do
	{
		if (myfgets(s, MAXLINELEN, f))
		{
			if (s[0] == '\n')
				break;

			// First remove trailing newline, if there is one
			tmp = strchr(s, '\n');
			if (tmp)
				*tmp = '\0';

			tmp = strchr(s, '#');
			if (tmp)
				*tmp = '\0';
			if (s == tmp)
				continue; // Skip comment lines, but don't break.

			// Get the part before the " = "
			tmp = strchr(s, '=');
			*(tmp-1) = '\0';
			strupr(word);

			// Now get the part after
			word2 = tmp += 2;
			value = atoi(word2); // used for numerical settings

			// Up here to allow lowercase in hints
			if (fastcmp(word, "HINT"))
			{
				while ((tmp = strchr(word2, '\\')))
					*tmp = '\n';
				deh_strlcpy(emblemlocations[num-1].hint, word2, sizeof (emblemlocations[num-1].hint), va("Emblem %d: hint", num));
				continue;
			}
			strupr(word2);

			if (fastcmp(word, "TYPE"))
			{
				if (fastcmp(word2, "GLOBAL"))
					emblemlocations[num-1].type = ET_GLOBAL;
				else if (fastcmp(word2, "SKIN"))
					emblemlocations[num-1].type = ET_SKIN;
				else if (fastcmp(word2, "SCORE"))
					emblemlocations[num-1].type = ET_SCORE;
				else if (fastcmp(word2, "TIME"))
					emblemlocations[num-1].type = ET_TIME;
				else if (fastcmp(word2, "RINGS"))
					emblemlocations[num-1].type = ET_RINGS;
				else if (fastcmp(word2, "MAP"))
					emblemlocations[num-1].type = ET_MAP;
				else if (fastcmp(word2, "NGRADE"))
					emblemlocations[num-1].type = ET_NGRADE;
				else if (fastcmp(word2, "NTIME"))
					emblemlocations[num-1].type = ET_NTIME;
				else
					emblemlocations[num-1].type = (UINT8)value;
			}
			else if (fastcmp(word, "X"))
				emblemlocations[num-1].x = (INT16)value;
			else if (fastcmp(word, "Y"))
				emblemlocations[num-1].y = (INT16)value;
			else if (fastcmp(word, "Z"))
				emblemlocations[num-1].z = (INT16)value;
			else if (fastcmp(word, "MAPNUM"))
			{
				// Support using the actual map name,
				// i.e., Level AB, Level FZ, etc.

				// Convert to map number
				if (word2[0] >= 'A' && word2[0] <= 'Z')
					value = M_MapNumber(word2[0], word2[1]);

				emblemlocations[num-1].level = (INT16)value;
			}
			else if (fastcmp(word, "SPRITE"))
			{
				if (word2[0] >= 'A' && word2[0] <= 'Z')
					value = word2[0];
				else
					value += 'A'-1;

				if (value < 'A' || value > 'Z')
					deh_warning("Emblem %d: sprite must be from A - Z (1 - 26)", num);
				else
					emblemlocations[num-1].sprite = (UINT8)value;
			}
			else if (fastcmp(word, "COLOR"))
				emblemlocations[num-1].color = get_number(word2);
			else if (fastcmp(word, "VAR"))
				emblemlocations[num-1].var = get_number(word2);
			else
				deh_warning("Emblem %d: unknown word '%s'", num, word);
		}
	} while (!myfeof(f));

	// Default sprite and color definitions for lazy people like me
	if (!emblemlocations[num-1].sprite) switch (emblemlocations[num-1].type)
	{
		case ET_RINGS:
			emblemlocations[num-1].sprite = 'R'; break;
		case ET_SCORE: case ET_NGRADE:
			emblemlocations[num-1].sprite = 'S'; break;
		case ET_TIME: case ET_NTIME:
			emblemlocations[num-1].sprite = 'T'; break;
		default:
			emblemlocations[num-1].sprite = 'A'; break;
	}
	if (!emblemlocations[num-1].color) switch (emblemlocations[num-1].type)
	{
		case ET_RINGS:
			emblemlocations[num-1].color = SKINCOLOR_GOLD; break;
		case ET_SCORE:
			emblemlocations[num-1].color = SKINCOLOR_BROWN; break;
		case ET_NGRADE:
			emblemlocations[num-1].color = SKINCOLOR_TEAL; break;
		case ET_TIME: case ET_NTIME:
			emblemlocations[num-1].color = SKINCOLOR_GREY; break;
		default:
			emblemlocations[num-1].color = SKINCOLOR_BLUE; break;
	}

	Z_Free(s);
}

static void readextraemblemdata(MYFILE *f, INT32 num)
{
	char *s = Z_Malloc(MAXLINELEN, PU_STATIC, NULL);
	char *word = s;
	char *word2;
	char *tmp;
	INT32 value;

	memset(&extraemblems[num-1], 0, sizeof(extraemblem_t));

	do
	{
		if (myfgets(s, MAXLINELEN, f))
		{
			if (s[0] == '\n')
				break;

			// First remove trailing newline, if there is one
			tmp = strchr(s, '\n');
			if (tmp)
				*tmp = '\0';

			tmp = strchr(s, '#');
			if (tmp)
				*tmp = '\0';
			if (s == tmp)
				continue; // Skip comment lines, but don't break.

			// Get the part before the " = "
			tmp = strchr(s, '=');
			*(tmp-1) = '\0';
			strupr(word);

			// Now get the part after
			word2 = tmp += 2;
			strupr(word2);

			value = atoi(word2); // used for numerical settings

			if (fastcmp(word, "NAME"))
				deh_strlcpy(extraemblems[num-1].name, word2,
					sizeof (extraemblems[num-1].name), va("Extra emblem %d: name", num));
			else if (fastcmp(word, "OBJECTIVE"))
				deh_strlcpy(extraemblems[num-1].description, word2,
					sizeof (extraemblems[num-1].description), va("Extra emblem %d: objective", num));
			else if (fastcmp(word, "CONDITIONSET"))
				extraemblems[num-1].conditionset = (UINT8)value;
			else if (fastcmp(word, "SPRITE"))
			{
				if (word2[0] >= 'A' && word2[0] <= 'Z')
					value = word2[0];
				else
					value += 'A'-1;

				if (value < 'A' || value > 'Z')
					deh_warning("Emblem %d: sprite must be from A - Z (1 - 26)", num);
				else
					extraemblems[num-1].sprite = (UINT8)value;
			}
			else if (fastcmp(word, "COLOR"))
				extraemblems[num-1].color = get_number(word2);
			else
				deh_warning("Extra emblem %d: unknown word '%s'", num, word);
		}
	} while (!myfeof(f));

	if (!extraemblems[num-1].sprite)
		extraemblems[num-1].sprite = 'X';
	if (!extraemblems[num-1].color)
		extraemblems[num-1].color = SKINCOLOR_BLUE;

	Z_Free(s);
}

static void readunlockable(MYFILE *f, INT32 num)
{
	char *s = Z_Malloc(MAXLINELEN, PU_STATIC, NULL);
	char *word = s;
	char *word2;
	char *tmp;
	INT32 i;

	memset(&unlockables[num], 0, sizeof(unlockable_t));
	unlockables[num].objective[0] = '/';

	do
	{
		if (myfgets(s, MAXLINELEN, f))
		{
			if (s[0] == '\n')
				break;

			// First remove trailing newline, if there is one
			tmp = strchr(s, '\n');
			if (tmp)
				*tmp = '\0';

			tmp = strchr(s, '#');
			if (tmp)
				*tmp = '\0';
			if (s == tmp)
				continue; // Skip comment lines, but don't break.

			// Get the part before the " = "
			tmp = strchr(s, '=');
			*(tmp-1) = '\0';
			strupr(word);

			// Now get the part after
			word2 = tmp += 2;
			strupr(word2);

			i = atoi(word2); // used for numerical settings

			if (fastcmp(word, "NAME"))
				deh_strlcpy(unlockables[num].name, word2,
					sizeof (unlockables[num].name), va("Unlockable %d: name", num));
			else if (fastcmp(word, "OBJECTIVE"))
				deh_strlcpy(unlockables[num].objective, word2,
					sizeof (unlockables[num].objective), va("Unlockable %d: objective", num));
			else if (fastcmp(word, "HEIGHT"))
				unlockables[num].height = (UINT16)i;
			else if (fastcmp(word, "CONDITIONSET"))
				unlockables[num].conditionset = (UINT8)i;
			else if (fastcmp(word, "NOCECHO"))
				unlockables[num].nocecho = (UINT8)(i || word2[0] == 'T' || word2[0] == 'Y');
			else if (fastcmp(word, "NOCHECKLIST"))
				unlockables[num].nochecklist = (UINT8)(i || word2[0] == 'T' || word2[0] == 'Y');
			else if (fastcmp(word, "TYPE"))
			{
				if (fastcmp(word2, "NONE"))
					unlockables[num].type = SECRET_NONE;
				else if (fastcmp(word2, "ITEMFINDER"))
					unlockables[num].type = SECRET_ITEMFINDER;
				else if (fastcmp(word2, "EMBLEMHINTS"))
					unlockables[num].type = SECRET_EMBLEMHINTS;
				else if (fastcmp(word2, "PANDORA"))
					unlockables[num].type = SECRET_PANDORA;
				else if (fastcmp(word2, "CREDITS"))
					unlockables[num].type = SECRET_CREDITS;
				else if (fastcmp(word2, "RECORDATTACK"))
					unlockables[num].type = SECRET_RECORDATTACK;
				else if (fastcmp(word2, "NIGHTSMODE"))
					unlockables[num].type = SECRET_NIGHTSMODE;
				else if (fastcmp(word2, "HEADER"))
					unlockables[num].type = SECRET_HEADER;
				else if (fastcmp(word2, "LEVELSELECT"))
					unlockables[num].type = SECRET_LEVELSELECT;
				else if (fastcmp(word2, "WARP"))
					unlockables[num].type = SECRET_WARP;
				else if (fastcmp(word2, "SOUNDTEST"))
					unlockables[num].type = SECRET_SOUNDTEST;
				else
					unlockables[num].type = (INT16)i;
			}
			else if (fastcmp(word, "VAR"))
			{
				// Support using the actual map name,
				// i.e., Level AB, Level FZ, etc.

				// Convert to map number
				if (word2[0] >= 'A' && word2[0] <= 'Z')
					i = M_MapNumber(word2[0], word2[1]);

				unlockables[num].variable = (INT16)i;
			}
			else
				deh_warning("Unlockable %d: unknown word '%s'", num+1, word);
		}
	} while (!myfeof(f)); // finish when the line is empty

	Z_Free(s);
}

#define PARAMCHECK(n) do { if (!params[n]) { deh_warning("Too few parameters, need %d", n); return; }} while (0)
static void readcondition(UINT8 set, UINT32 id, char *word2)
{
	INT32 i;
	char *params[4]; // condition, requirement, extra info, extra info
	char *spos;

	conditiontype_t ty;
	INT32 re;
	INT16 x1 = 0, x2 = 0;

	INT32 offset = 0;

	spos = strtok(word2, " ");

	for (i = 0; i < 4; ++i)
	{
		if (spos != NULL)
		{
			params[i] = spos;
			spos = strtok(NULL, " ");
		}
		else
			params[i] = NULL;
	}

	if (!params[0])
	{
		deh_warning("condition line is empty");
		return;
	}

	if (fastcmp(params[0], "PLAYTIME"))
	{
		PARAMCHECK(1);
		ty = UC_PLAYTIME;
		re = atoi(params[1]);
	}
	else if        (fastcmp(params[0], "GAMECLEAR")
	|| (++offset && fastcmp(params[0], "ALLEMERALDS"))
	|| (++offset && fastcmp(params[0], "ULTIMATECLEAR")))
	{
		ty = UC_GAMECLEAR + offset;
		re = (params[1]) ? atoi(params[1]) : 1;
	}
	else if ((offset=0) || fastcmp(params[0], "OVERALLSCORE")
	||        (++offset && fastcmp(params[0], "OVERALLTIME"))
	||        (++offset && fastcmp(params[0], "OVERALLRINGS")))
	{
		PARAMCHECK(1);
		ty = UC_OVERALLSCORE + offset;
		re = atoi(params[1]);
	}
	else if ((offset=0) || fastcmp(params[0], "MAPVISITED")
	||        (++offset && fastcmp(params[0], "MAPBEATEN"))
	||        (++offset && fastcmp(params[0], "MAPALLEMERALDS"))
	||        (++offset && fastcmp(params[0], "MAPULTIMATE"))
	||        (++offset && fastcmp(params[0], "MAPPERFECT")))
	{
		PARAMCHECK(1);
		ty = UC_MAPVISITED + offset;

		// Convert to map number if it appears to be one
		if (params[1][0] >= 'A' && params[1][0] <= 'Z')
			re = M_MapNumber(params[1][0], params[1][1]);
		else
			re = atoi(params[1]);

		if (re < 0 || re >= NUMMAPS)
		{
			deh_warning("Level number %d out of range (1 - %d)", re, NUMMAPS);
			return;
		}
	}
	else if ((offset=0) || fastcmp(params[0], "MAPSCORE")
	||        (++offset && fastcmp(params[0], "MAPTIME"))
	||        (++offset && fastcmp(params[0], "MAPRINGS")))
	{
		PARAMCHECK(2);
		ty = UC_MAPSCORE + offset;
		re = atoi(params[2]);

		// Convert to map number if it appears to be one
		if (params[1][0] >= 'A' && params[1][0] <= 'Z')
			x1 = (INT16)M_MapNumber(params[1][0], params[1][1]);
		else
			x1 = (INT16)atoi(params[1]);

		if (x1 < 0 || x1 >= NUMMAPS)
		{
			deh_warning("Level number %d out of range (1 - %d)", re, NUMMAPS);
			return;
		}
	}
	else if ((offset=0) || fastcmp(params[0], "NIGHTSSCORE")
	||        (++offset && fastcmp(params[0], "NIGHTSTIME"))
	||        (++offset && fastcmp(params[0], "NIGHTSGRADE")))
	{
		PARAMCHECK(2); // one optional one

		ty = UC_NIGHTSSCORE + offset;
		re = atoi(params[2 + !!(params[3])]);

		// Convert to map number if it appears to be one
		if (params[1][0] >= 'A' && params[1][0] <= 'Z')
			x1 = (INT16)M_MapNumber(params[1][0], params[1][1]);
		else
			x1 = (INT16)atoi(params[1]);

		if (x1 < 0 || x1 >= NUMMAPS)
		{
			deh_warning("Level number %d out of range (1 - %d)", re, NUMMAPS);
			return;
		}

		// Mare number (0 for overall)
		if (params[3]) // Only if we actually got 3 params (so the second one == mare and not requirement)
			x2 = (INT16)atoi(params[2]);
		else
			x2 = 0;

	}
	else if (fastcmp(params[0], "TRIGGER"))
	{
		PARAMCHECK(1);
		ty = UC_TRIGGER;
		re = atoi(params[1]);

		// constrained by 32 bits
		if (re < 0 || re > 31)
		{
			deh_warning("Trigger ID %d out of range (0 - 31)", re);
			return;
		}
	}
	else if (fastcmp(params[0], "TOTALEMBLEMS"))
	{
		PARAMCHECK(1);
		ty = UC_TOTALEMBLEMS;
		re = atoi(params[1]);
	}
	else if (fastcmp(params[0], "EMBLEM"))
	{
		PARAMCHECK(1);
		ty = UC_EMBLEM;
		re = atoi(params[1]);

		if (re <= 0 || re > MAXEMBLEMS)
		{
			deh_warning("Emblem %d out of range (1 - %d)", re, MAXEMBLEMS);
			return;
		}
	}
	else if (fastcmp(params[0], "EXTRAEMBLEM"))
	{
		PARAMCHECK(1);
		ty = UC_EXTRAEMBLEM;
		re = atoi(params[1]);

		if (re <= 0 || re > MAXEXTRAEMBLEMS)
		{
			deh_warning("Extra emblem %d out of range (1 - %d)", re, MAXEXTRAEMBLEMS);
			return;
		}
	}
	else if (fastcmp(params[0], "CONDITIONSET"))
	{
		PARAMCHECK(1);
		ty = UC_CONDITIONSET;
		re = atoi(params[1]);

		if (re <= 0 || re > MAXCONDITIONSETS)
		{
			deh_warning("Condition set %d out of range (1 - %d)", re, MAXCONDITIONSETS);
			return;
		}
	}
	else
	{
		deh_warning("Invalid condition name %s", params[0]);
		return;
	}

	M_AddRawCondition(set, (UINT8)id, ty, re, x1, x2);
}
#undef PARAMCHECK

static void readconditionset(MYFILE *f, UINT8 setnum)
{
	char *s = Z_Malloc(MAXLINELEN, PU_STATIC, NULL);
	char *word = s;
	char *word2;
	char *tmp;
	UINT8 id;
	UINT8 previd = 0;

	M_ClearConditionSet(setnum);

	do
	{
		if (myfgets(s, MAXLINELEN, f))
		{
			if (s[0] == '\n')
				break;

			// First remove trailing newline, if there is one
			tmp = strchr(s, '\n');
			if (tmp)
				*tmp = '\0';

			tmp = strchr(s, '#');
			if (tmp)
				*tmp = '\0';
			if (s == tmp)
				continue; // Skip comment lines, but don't break.

			// Get the part before the " = "
			tmp = strchr(s, '=');
			*(tmp-1) = '\0';
			strupr(word);

			// Now get the part after
			word2 = tmp += 2;
			strupr(word2);

			if (fastncmp(word, "CONDITION", 9))
			{
				id = (UINT8)atoi(word + 9);
				if (id == 0)
				{
					deh_warning("Condition set %d: unknown word '%s'", setnum, word);
					continue;
				}
				else if (previd > id)
				{
					// out of order conditions can cause problems, so enforce proper order
					deh_warning("Condition set %d: conditions are out of order, ignoring this line", setnum);
					continue;
				}
				previd = id;

				readcondition(setnum, id, word2);
			}
			else
				deh_warning("Condition set %d: unknown word '%s'", setnum, word);
		}
	} while (!myfeof(f)); // finish when the line is empty

	Z_Free(s);
}

static void readmaincfg(MYFILE *f)
{
	char *s = Z_Malloc(MAXLINELEN, PU_STATIC, NULL);
	char *word = s;
	char *word2;
	char *tmp;
	INT32 value;

	do
	{
		if (myfgets(s, MAXLINELEN, f))
		{
			if (s[0] == '\n')
				break;

			// First remove trailing newline, if there is one
			tmp = strchr(s, '\n');
			if (tmp)
				*tmp = '\0';

			tmp = strchr(s, '#');
			if (tmp)
				*tmp = '\0';
			if (s == tmp)
				continue; // Skip comment lines, but don't break.

			// Get the part before the " = "
			tmp = strchr(s, '=');
			*(tmp-1) = '\0';
			strupr(word);

			// Now get the part after
			word2 = tmp += 2;
			strupr(word2);

			value = atoi(word2); // used for numerical settings

			if (fastcmp(word, "EXECCFG"))
				COM_BufAddText(va("exec %s\n", word2));

			else if (fastcmp(word, "SPSTAGE_START"))
			{
				// Support using the actual map name,
				// i.e., Level AB, Level FZ, etc.

				// Convert to map number
				if (word2[0] >= 'A' && word2[0] <= 'Z')
					value = M_MapNumber(word2[0], word2[1]);
				else
					value = get_number(word2);

				spstage_start = (INT16)value;
			}
			else if (fastcmp(word, "SSTAGE_START"))
			{
				// Support using the actual map name,
				// i.e., Level AB, Level FZ, etc.

				// Convert to map number
				if (word2[0] >= 'A' && word2[0] <= 'Z')
					value = M_MapNumber(word2[0], word2[1]);
				else
					value = get_number(word2);

				sstage_start = (INT16)value;
				sstage_end = (INT16)(sstage_start+6); // 7 special stages total
			}
			else if (fastcmp(word, "USENIGHTSSS"))
			{
				useNightsSS = (UINT8)(value || word2[0] == 'T' || word2[0] == 'Y');
			}
			else if (fastcmp(word, "REDTEAM"))
			{
				skincolor_redteam = (UINT8)get_number(word2);
			}
			else if (fastcmp(word, "BLUETEAM"))
			{
				skincolor_blueteam = (UINT8)get_number(word2);
			}
			else if (fastcmp(word, "REDRING"))
			{
				skincolor_redring = (UINT8)get_number(word2);
			}
			else if (fastcmp(word, "BLUERING"))
			{
				skincolor_bluering = (UINT8)get_number(word2);
			}
			else if (fastcmp(word, "INVULNTICS"))
			{
				invulntics = (UINT16)get_number(word2);
			}
			else if (fastcmp(word, "SNEAKERTICS"))
			{
				sneakertics = (UINT16)get_number(word2);
			}
			else if (fastcmp(word, "FLASHINGTICS"))
			{
				flashingtics = (UINT16)get_number(word2);
			}
			else if (fastcmp(word, "TAILSFLYTICS"))
			{
				tailsflytics = (UINT16)get_number(word2);
			}
			else if (fastcmp(word, "UNDERWATERTICS"))
			{
				underwatertics = (UINT16)get_number(word2);
			}
			else if (fastcmp(word, "SPACETIMETICS"))
			{
				spacetimetics = (UINT16)get_number(word2);
			}
			else if (fastcmp(word, "EXTRALIFETICS"))
			{
				extralifetics = (UINT16)get_number(word2);
			}
			else if (fastcmp(word, "GAMEOVERTICS"))
			{
				gameovertics = get_number(word2);
			}

			else if (fastcmp(word, "INTROTOPLAY"))
			{
				introtoplay = (UINT8)get_number(word2);
				// range check, you morons.
				if (introtoplay > 128)
					introtoplay = 128;
				introchanged = true;
			}
			else if (fastcmp(word, "LOOPTITLE"))
			{
				looptitle = (boolean)(value || word2[0] == 'T' || word2[0] == 'Y');
				titlechanged = true;
			}
			else if (fastcmp(word, "TITLEMAP"))
			{
				// Support using the actual map name,
				// i.e., Level AB, Level FZ, etc.

				// Convert to map number
				if (word2[0] >= 'A' && word2[0] <= 'Z')
					value = M_MapNumber(word2[0], word2[1]);
				else
					value = get_number(word2);

				DEH_WriteUndoline(word, va("%d", titlemap), UNDO_NONE);
				titlemap = (INT16)value;
				titlechanged = true;
			}
			else if (fastcmp(word, "HIDETITLEPICS"))
			{
				DEH_WriteUndoline(word, va("%d", hidetitlepics), UNDO_NONE);
				hidetitlepics = (boolean)(value || word2[0] == 'T' || word2[0] == 'Y');
				titlechanged = true;
			}
			else if (fastcmp(word, "TITLESCROLLSPEED"))
			{
				titlescrollspeed = get_number(word2);
				titlechanged = true;
			}
			else if (fastcmp(word, "CREDITSCUTSCENE"))
			{
				creditscutscene = (UINT8)get_number(word2);
				// range check, you morons.
				if (creditscutscene > 128)
					creditscutscene = 128;
			}
			else if (fastcmp(word, "DISABLESPEEDADJUST"))
			{
				disableSpeedAdjust = (UINT8)get_number(word2);
			}
			else if (fastcmp(word, "NUMDEMOS"))
			{
				numDemos = (UINT8)get_number(word2);
				titlechanged = true;
			}
			else if (fastcmp(word, "DEMODELAYTIME"))
			{
				demoDelayTime = get_number(word2);
				titlechanged = true;
			}
			else if (fastcmp(word, "DEMOIDLETIME"))
			{
				demoIdleTime = get_number(word2);
				titlechanged = true;
			}
			else if (fastcmp(word, "USE1UPSOUND"))
			{
				use1upSound = (UINT8)(value || word2[0] == 'T' || word2[0] == 'Y');
			}
			else if (fastcmp(word, "MAXXTRALIFE"))
			{
				maxXtraLife = (UINT8)get_number(word2);
			}

			else if (fastcmp(word, "GAMEDATA"))
			{
				size_t filenamelen;

				// Check the data filename so that mods
				// can't write arbitrary files.
				if (!GoodDataFileName(word2))
					I_Error("Maincfg: bad data file name '%s'\n", word2);

				G_SaveGameData();
				strlcpy(gamedatafilename, word2, sizeof (gamedatafilename));
				strlwr(gamedatafilename);
				savemoddata = true;

				// Also save a time attack folder
				filenamelen = strlen(gamedatafilename)-4;  // Strip off the extension
				strncpy(timeattackfolder, gamedatafilename, min(filenamelen, sizeof (timeattackfolder)));
				timeattackfolder[min(filenamelen, sizeof (timeattackfolder) - 1)] = '\0';

				strncpy(savegamename, timeattackfolder, sizeof (timeattackfolder));
				strlcat(savegamename, "%u.ssg", sizeof(savegamename));

				gamedataadded = true;
				titlechanged = true;
			}
			else if (fastcmp(word, "RESETDATA"))
			{
				P_ResetData(value);
				titlechanged = true;
			}
			else if (fastcmp(word, "CUSTOMVERSION"))
			{
				strlcpy(customversionstring, word2, sizeof (customversionstring));
				//titlechanged = true;
			}
			else if (fastcmp(word, "BOOTMAP"))
			{
				// Support using the actual map name,
				// i.e., Level AB, Level FZ, etc.

				// Convert to map number
				if (word2[0] >= 'A' && word2[0] <= 'Z')
					value = M_MapNumber(word2[0], word2[1]);
				else
					value = get_number(word2);

				DEH_WriteUndoline(word, va("%d", bootmap), UNDO_NONE);
				bootmap = (INT16)value;
				//titlechanged = true;
			}
			else
				deh_warning("Maincfg: unknown word '%s'", word);
		}
	} while (!myfeof(f));

	Z_Free(s);
}

static void readwipes(MYFILE *f)
{
	char *s = Z_Malloc(MAXLINELEN, PU_STATIC, NULL);
	char *word = s;
	char *pword = word;
	char *word2;
	char *tmp;
	INT32 value;
	INT32 wipeoffset;

	do
	{
		if (myfgets(s, MAXLINELEN, f))
		{
			if (s[0] == '\n')
				break;

			// First remove trailing newline, if there is one
			tmp = strchr(s, '\n');
			if (tmp)
				*tmp = '\0';

			tmp = strchr(s, '#');
			if (tmp)
				*tmp = '\0';
			if (s == tmp)
				continue; // Skip comment lines, but don't break.

			// Get the part before the " = "
			tmp = strchr(s, '=');
			*(tmp-1) = '\0';
			strupr(word);

			// Now get the part after
			word2 = tmp += 2;
			value = atoi(word2); // used for numerical settings

			if (value < -1 || value > 99)
			{
				deh_warning("Wipes: bad value '%s'", word2);
				continue;
			}
			else if (value == -1)
				value = UINT8_MAX;

			// error catching
			wipeoffset = -1;

			if (fastncmp(word, "LEVEL_", 6))
			{
				pword = word + 6;
				if (fastcmp(pword, "TOBLACK"))
					wipeoffset = wipe_level_toblack;
				else if (fastcmp(pword, "FINAL"))
					wipeoffset = wipe_level_final;
			}
			else if (fastncmp(word, "INTERMISSION_", 13))
			{
				pword = word + 13;
				if (fastcmp(pword, "TOBLACK"))
					wipeoffset = wipe_intermission_toblack;
				else if (fastcmp(pword, "FINAL"))
					wipeoffset = wipe_intermission_final;
			}
			else if (fastncmp(word, "SPECINTER_", 10))
			{
				pword = word + 10;
				if (fastcmp(pword, "TOBLACK"))
					wipeoffset = wipe_specinter_toblack;
				else if (fastcmp(pword, "FINAL"))
					wipeoffset = wipe_specinter_final;
			}
			else if (fastncmp(word, "MULTINTER_", 10))
			{
				pword = word + 10;
				if (fastcmp(pword, "TOBLACK"))
					wipeoffset = wipe_multinter_toblack;
				else if (fastcmp(pword, "FINAL"))
					wipeoffset = wipe_multinter_final;
			}
			else if (fastncmp(word, "CONTINUING_", 11))
			{
				pword = word + 11;
				if (fastcmp(pword, "TOBLACK"))
					wipeoffset = wipe_continuing_toblack;
				else if (fastcmp(pword, "FINAL"))
					wipeoffset = wipe_continuing_final;
			}
			else if (fastncmp(word, "TITLESCREEN_", 12))
			{
				pword = word + 12;
				if (fastcmp(pword, "TOBLACK"))
					wipeoffset = wipe_titlescreen_toblack;
				else if (fastcmp(pword, "FINAL"))
					wipeoffset = wipe_titlescreen_final;
			}
			else if (fastncmp(word, "TIMEATTACK_", 11))
			{
				pword = word + 11;
				if (fastcmp(pword, "TOBLACK"))
					wipeoffset = wipe_timeattack_toblack;
				else if (fastcmp(pword, "FINAL"))
					wipeoffset = wipe_timeattack_final;
			}
			else if (fastncmp(word, "CREDITS_", 8))
			{
				pword = word + 8;
				if (fastcmp(pword, "TOBLACK"))
					wipeoffset = wipe_credits_toblack;
				else if (fastcmp(pword, "FINAL"))
					wipeoffset = wipe_credits_final;
				else if (fastcmp(pword, "INTERMEDIATE"))
					wipeoffset = wipe_credits_intermediate;
			}
			else if (fastncmp(word, "EVALUATION_", 11))
			{
				pword = word + 11;
				if (fastcmp(pword, "TOBLACK"))
					wipeoffset = wipe_evaluation_toblack;
				else if (fastcmp(pword, "FINAL"))
					wipeoffset = wipe_evaluation_final;
			}
			else if (fastncmp(word, "GAMEEND_", 8))
			{
				pword = word + 8;
				if (fastcmp(pword, "TOBLACK"))
					wipeoffset = wipe_gameend_toblack;
				else if (fastcmp(pword, "FINAL"))
					wipeoffset = wipe_gameend_final;
			}
			else if (fastncmp(word, "SPECLEVEL_", 10))
			{
				pword = word + 10;
				if (fastcmp(pword, "TOWHITE"))
					wipeoffset = wipe_speclevel_towhite;
			}

			if (wipeoffset < 0)
			{
				deh_warning("Wipes: unknown word '%s'", word);
				continue;
			}

			if (value == UINT8_MAX
			 && (wipeoffset <= wipe_level_toblack || wipeoffset >= wipe_speclevel_towhite))
			{
				 // Cannot disable non-toblack wipes
				 // (or the level toblack wipe, or the special towhite wipe)
				deh_warning("Wipes: can't disable wipe of type '%s'", word);
				continue;
			}

			wipedefs[wipeoffset] = (UINT8)value;
		}
	} while (!myfeof(f));

	Z_Free(s);
}

// Used when you do something invalid like read a bad item number
// to prevent extra unnecessary errors
static void ignorelines(MYFILE *f)
{
	char *s = Z_Malloc(MAXLINELEN, PU_STATIC, NULL);
	do
	{
		if (myfgets(s, MAXLINELEN, f))
		{
			if (s[0] == '\n')
				break;
		}
	} while (!myfeof(f));
	Z_Free(s);
}

static void DEH_LoadDehackedFile(MYFILE *f)
{
	char *s = Z_Malloc(MAXLINELEN, PU_STATIC, NULL);
	char *word;
	char *word2;
	INT32 i;

	if (!deh_loaded)
		initfreeslots();

	deh_num_warning = 0;

	gamedataadded = titlechanged = introchanged = false;

	// it doesn't test the version of SRB2 and version of dehacked file
	dbg_line = -1; // start at -1 so the first line is 0.
	while (!myfeof(f))
	{
		XBOXSTATIC char origpos[128];
		INT32 size = 0;
		char *traverse;

		myfgets(s, MAXLINELEN, f);
		if (s[0] == '\n' || s[0] == '#')
			continue;

		traverse = s;

		while (traverse[0] != '\n')
		{
			traverse++;
			size++;
		}

		strncpy(origpos, s, size);
		origpos[size] = '\0';

		if (NULL != (word = strtok(s, " "))) {
			strupr(word);
			if (word[strlen(word)-1] == '\n')
				word[strlen(word)-1] = '\0';
		}
		if (word)
		{
			if (fastcmp(word, "FREESLOT"))
			{
				readfreeslots(f);
				continue;
			}
			else if (fastcmp(word, "MAINCFG"))
			{
				readmaincfg(f);
				continue;
			}
			else if (fastcmp(word, "WIPES"))
			{
				readwipes(f);
				continue;
			}
			word2 = strtok(NULL, " ");
			if (fastcmp(word, "CHARACTER"))
			{
				if (word2) {
					strupr(word2);
					if (word2[strlen(word2)-1] == '\n')
						word2[strlen(word2)-1] = '\0';
					i = atoi(word2);
				} else
					i = 0;
				if (i >= 0 && i < 32)
					readPlayer(f, i);
				else
				{
					deh_warning("Character %d out of range (0 - 31)", i);
					ignorelines(f);
				}
				continue;
			}
			if (word2)
			{
				strupr(word2);
				if (word2[strlen(word2)-1] == '\n')
					word2[strlen(word2)-1] = '\0';
				i = atoi(word2);

				if (fastcmp(word, "THING") || fastcmp(word, "MOBJ") || fastcmp(word, "OBJECT"))
				{
					if (i == 0 && word2[0] != '0') // If word2 isn't a number
						i = get_mobjtype(word2); // find a thing by name
					if (i < NUMMOBJTYPES && i > 0)
						readthing(f, i);
					else
					{
						deh_warning("Thing %d out of range (1 - %d)", i, NUMMOBJTYPES-1);
						ignorelines(f);
					}
				}
				else if (fastcmp(word, "SPRITE2"))
				{
					if (i == 0 && word2[0] != '0') // If word2 isn't a number
						i = get_sprite2(word2); // find a sprite by name
					if (i < (INT32)free_spr2 && i >= (INT32)SPR2_FIRSTFREESLOT)
						readsprite2(f, i);
					else
					{
						deh_warning("Sprite2 number %d out of range (%d - %d)", i, SPR2_FIRSTFREESLOT, free_spr2-1);
						ignorelines(f);
					}
				}
#ifdef HWRENDER
				else if (fastcmp(word, "LIGHT"))
				{
					// TODO: Read lights by name
					if (i > 0 && i < NUMLIGHTS)
						readlight(f, i);
					else
					{
						deh_warning("Light number %d out of range (1 - %d)", i, NUMLIGHTS-1);
						ignorelines(f);
					}
				}
				else if (fastcmp(word, "SPRITE"))
				{
					if (i == 0 && word2[0] != '0') // If word2 isn't a number
						i = get_sprite(word2); // find a sprite by name
					if (i < NUMSPRITES && i > 0)
						readspritelight(f, i);
					else
					{
						deh_warning("Sprite number %d out of range (0 - %d)", i, NUMSPRITES-1);
						ignorelines(f);
					}
				}
#endif
				else if (fastcmp(word, "LEVEL"))
				{
					// Support using the actual map name,
					// i.e., Level AB, Level FZ, etc.

					// Convert to map number
					if (word2[0] >= 'A' && word2[0] <= 'Z')
						i = M_MapNumber(word2[0], word2[1]);

					if (i > 0 && i <= NUMMAPS)
						readlevelheader(f, i);
					else
					{
						deh_warning("Level number %d out of range (1 - %d)", i, NUMMAPS);
						ignorelines(f);
					}
				}
				else if (fastcmp(word, "CUTSCENE"))
				{
					if (i > 0 && i < 129)
						readcutscene(f, i - 1);
					else
					{
						deh_warning("Cutscene number %d out of range (1 - 128)", i);
						ignorelines(f);
					}
				}
				else if (fastcmp(word, "FRAME") || fastcmp(word, "STATE"))
				{
					if (i == 0 && word2[0] != '0') // If word2 isn't a number
						i = get_state(word2); // find a state by name
					if (i < NUMSTATES && i >= 0)
						readframe(f, i);
					else
					{
						deh_warning("Frame %d out of range (0 - %d)", i, NUMSTATES-1);
						ignorelines(f);
					}
				}
				else if (fastcmp(word, "SOUND"))
				{
					if (i == 0 && word2[0] != '0') // If word2 isn't a number
						i = get_sfx(word2); // find a sound by name
					if (i < NUMSFX && i > 0)
						readsound(f, i);
					else
					{
						deh_warning("Sound %d out of range (1 - %d)", i, NUMSFX-1);
						ignorelines(f);
					}
				}
				else if (fastcmp(word, "HUDITEM"))
				{
					if (i == 0 && word2[0] != '0') // If word2 isn't a number
						i = get_huditem(word2); // find a huditem by name
					if (i >= 0 && i < NUMHUDITEMS)
						readhuditem(f, i);
					else
					{
						deh_warning("HUD item number %d out of range (0 - %d)", i, NUMHUDITEMS-1);
						ignorelines(f);
					}
				}
				else if (fastcmp(word, "EMBLEM"))
				{
					if (!gamedataadded)
					{
						deh_warning("You must define a custom gamedata to use \"%s\"", word);
						ignorelines(f);
					}
					else if (i > 0 && i <= MAXEMBLEMS)
					{
						if (numemblems < i)
							numemblems = i;
						reademblemdata(f, i);
					}
					else
					{
						deh_warning("Emblem number %d out of range (1 - %d)", i, MAXEMBLEMS);
						ignorelines(f);
					}
				}
				else if (fastcmp(word, "EXTRAEMBLEM"))
				{
					if (!gamedataadded)
					{
						deh_warning("You must define a custom gamedata to use \"%s\"", word);
						ignorelines(f);
					}
					else if (i > 0 && i <= MAXEXTRAEMBLEMS)
					{
						if (numextraemblems < i)
							numextraemblems = i;
						readextraemblemdata(f, i);
					}
					else
					{
						deh_warning("Extra emblem number %d out of range (1 - %d)", i, MAXEXTRAEMBLEMS);
						ignorelines(f);
					}
				}
				else if (fastcmp(word, "UNLOCKABLE"))
				{
					if (!gamedataadded)
					{
						deh_warning("You must define a custom gamedata to use \"%s\"", word);
						ignorelines(f);
					}
					else if (i > 0 && i <= MAXUNLOCKABLES)
						readunlockable(f, i - 1);
					else
					{
						deh_warning("Unlockable number %d out of range (1 - %d)", i, MAXUNLOCKABLES);
						ignorelines(f);
					}
				}
				else if (fastcmp(word, "CONDITIONSET"))
				{
					if (!gamedataadded)
					{
						deh_warning("You must define a custom gamedata to use \"%s\"", word);
						ignorelines(f);
					}
					else if (i > 0 && i <= MAXCONDITIONSETS)
						readconditionset(f, (UINT8)i);
					else
					{
						deh_warning("Condition set number %d out of range (1 - %d)", i, MAXCONDITIONSETS);
						ignorelines(f);
					}
				}
				// Last I heard this crashes the game if you try to use it
				// so this is disabled for now
				// -- Monster Iestyn
/*				else if (fastcmp(word, "SRB2"))
				{
					INT32 ver = searchvalue(strtok(NULL, "\n"));
					if (ver != PATCHVERSION)
						deh_warning("Patch is for SRB2 version %d,\nonly version %d is supported", ver, PATCHVERSION);
				}
				// Clear all data in certain locations (mostly for unlocks)
				// Unless you REALLY want to piss people off,
				// define a custom gamedata /before/ doing this!!
				// (then again, modifiedgame will prevent game data saving anyway)
*/
				else if (fastcmp(word, "CLEAR"))
				{
					boolean clearall = (fastcmp(word2, "ALL"));

					if (!gamedataadded)
					{
						deh_warning("You must define a custom gamedata to use \"%s\"", word);
						continue;
					}

					if (clearall || fastcmp(word2, "UNLOCKABLES"))
						memset(&unlockables, 0, sizeof(unlockables));

					if (clearall || fastcmp(word2, "EMBLEMS"))
					{
						memset(&emblemlocations, 0, sizeof(emblemlocations));
						numemblems = 0;
					}

					if (clearall || fastcmp(word2, "EXTRAEMBLEMS"))
					{
						memset(&extraemblems, 0, sizeof(extraemblems));
						numextraemblems = 0;
					}

					if (clearall || fastcmp(word2, "CONDITIONSETS"))
						clear_conditionsets();

					if (clearall || fastcmp(word2, "LEVELS"))
						clear_levels();
				}
				else
					deh_warning("Unknown word: %s", word);
			}
			else
				deh_warning("missing argument for '%s'", word);
		}
		else
			deh_warning("No word in this line: %s", s);
	} // end while

	if (gamedataadded)
		G_LoadGameData();

	if (gamestate == GS_TITLESCREEN)
	{
		if (introchanged)
			COM_BufAddText("playintro");
		else if (titlechanged)
			COM_BufAddText("exitgame"); // Command_ExitGame_f() but delayed
	}

	dbg_line = -1;
	if (deh_num_warning)
	{
		CONS_Printf(M_GetText("%d warning%s in the SOC lump\n"), deh_num_warning, deh_num_warning == 1 ? "" : "s");
		if (devparm) {
			I_Error("%s%s",va(M_GetText("%d warning%s in the SOC lump\n"), deh_num_warning, deh_num_warning == 1 ? "" : "s"), M_GetText("See log.txt for details.\n"));
			//while (!I_GetKey())
				//I_OsPolling();
		}
	}

	deh_loaded = true;
	Z_Free(s);
}

// read dehacked lump in a wad (there is special trick for for deh
// file that are converted to wad in w_wad.c)
void DEH_LoadDehackedLumpPwad(UINT16 wad, UINT16 lump)
{
	MYFILE f;
	f.wad = wad;
	f.size = W_LumpLengthPwad(wad, lump);
	f.data = Z_Malloc(f.size + 1, PU_STATIC, NULL);
	W_ReadLumpPwad(wad, lump, f.data);
	f.curpos = f.data;
	f.data[f.size] = 0;
	DEH_LoadDehackedFile(&f);
	Z_Free(f.data);
}

void DEH_LoadDehackedLump(lumpnum_t lumpnum)
{
	DEH_LoadDehackedLumpPwad(WADFILENUM(lumpnum),LUMPNUM(lumpnum));
}

////////////////////////////////////////////////////////////////////////////////
// CRAZY LIST OF STATE NAMES AND ALL FROM HERE DOWN
// TODO: Make this all a seperate file or something, like part of info.c??
// TODO: Read the list from a text lump in a WAD as necessary instead
// or something, don't just keep it all in memory like this.
// TODO: Make the lists public so we can start using actual mobj
// and state names in warning and error messages! :D

// RegEx to generate this from info.h: ^\tS_([^,]+), --> \t"S_\1",
// I am leaving the prefixes solely for clarity to programmers,
// because sadly no one remembers this place while searching for full state names.
static const char *const STATE_LIST[] = { // array length left dynamic for sanity testing later.
	"S_NULL",
	"S_UNKNOWN",
	"S_INVISIBLE", // state for invisible sprite

	"S_SPAWNSTATE",
	"S_SEESTATE",
	"S_MELEESTATE",
	"S_MISSILESTATE",
	"S_DEATHSTATE",
	"S_XDEATHSTATE",
	"S_RAISESTATE",

	// Thok
	"S_THOK",

	// Player
	"S_PLAY_STND",
	"S_PLAY_WAIT",
	"S_PLAY_WALK",
	"S_PLAY_SKID",
	"S_PLAY_RUN",
	"S_PLAY_DASH",
	"S_PLAY_PAIN",
	"S_PLAY_STUN",
	"S_PLAY_DEAD",
	"S_PLAY_DRWN",
	"S_PLAY_ROLL",
	"S_PLAY_GASP",
	"S_PLAY_JUMP",
	"S_PLAY_SPRING",
	"S_PLAY_FALL",
	"S_PLAY_EDGE",
	"S_PLAY_RIDE",

	// CA2_SPINDASH
	"S_PLAY_SPINDASH",

	// CA_FLY/SWIM
	"S_PLAY_FLY",
	"S_PLAY_SWIM",
	"S_PLAY_FLY_TIRED",

	// CA_GLIDEANDCLIMB
	"S_PLAY_GLIDE",
	"S_PLAY_CLING",
	"S_PLAY_CLIMB",

	// CA_FLOAT/CA_SLOWFALL
	"S_PLAY_FLOAT",
	"S_PLAY_FLOAT_RUN",

	// CA_BOUNCE
	"S_PLAY_BOUNCE",
	"S_PLAY_BOUNCE_LANDING",

	// CA2_GUNSLINGER
	"S_PLAY_FIRE",
	"S_PLAY_FIRE_FINISH",

	// CA_TWINSPIN
	"S_PLAY_TWINSPIN",

	// CA2_MELEE
	"S_PLAY_MELEE",
	"S_PLAY_MELEE_FINISH",
	"S_PLAY_MELEE_LANDING",

	// SF_SUPER
	"S_PLAY_SUPERTRANS1",
	"S_PLAY_SUPERTRANS2",
	"S_PLAY_SUPERTRANS3",
	"S_PLAY_SUPERTRANS4",
	"S_PLAY_SUPERTRANS5",
	"S_PLAY_SUPERTRANS6",
	"S_PLAY_SUPERTRANS7",
	"S_PLAY_SUPERTRANS8",
	"S_PLAY_SUPERTRANS9", // This has special significance in the code. If you add more frames, search for it and make the appropriate changes.

	// technically the player goes here but it's an infinite tic state
	"S_OBJPLACE_DUMMY",

	// 1-Up Box Sprites (uses player sprite)
	"S_PLAY_BOX1",
	"S_PLAY_BOX2",
	"S_PLAY_ICON1",
	"S_PLAY_ICON2",
	"S_PLAY_ICON3",

	// Level end sign (uses player sprite)
	"S_PLAY_SIGN",

	// NiGHTS character (uses player sprite)
	"S_PLAY_NIGHTS_TRANS",
	"S_PLAY_NIGHTS_TRANS2",
	"S_PLAY_NIGHTS_TRANS3",
	"S_PLAY_NIGHTS_TRANS4",
	"S_PLAY_NIGHTS_TRANS5",
	"S_PLAY_NIGHTS_TRANS6",
	"S_PLAY_NIGHTS_TRANS7",
	"S_PLAY_NIGHTS_TRANS8",
	"S_PLAY_NIGHTS_TRANS9",

	"S_PLAY_NIGHTS_STAND",
	"S_PLAY_NIGHTS_FLOAT",
	"S_PLAY_NIGHTS_STUN",
	"S_PLAY_NIGHTS_PULL",
	"S_PLAY_NIGHTS_ATTACK",

	"S_PLAY_NIGHTS_FLY0",
	"S_PLAY_NIGHTS_DRILL0",
	"S_PLAY_NIGHTS_FLY1",
	"S_PLAY_NIGHTS_DRILL1",
	"S_PLAY_NIGHTS_FLY2",
	"S_PLAY_NIGHTS_DRILL2",
	"S_PLAY_NIGHTS_FLY3",
	"S_PLAY_NIGHTS_DRILL3",
	"S_PLAY_NIGHTS_FLY4",
	"S_PLAY_NIGHTS_DRILL4",
	"S_PLAY_NIGHTS_FLY5",
	"S_PLAY_NIGHTS_DRILL5",
	"S_PLAY_NIGHTS_FLY6",
	"S_PLAY_NIGHTS_DRILL6",
	"S_PLAY_NIGHTS_FLY7",
	"S_PLAY_NIGHTS_DRILL7",
	"S_PLAY_NIGHTS_FLY8",
	"S_PLAY_NIGHTS_DRILL8",
	"S_PLAY_NIGHTS_FLY9",
	"S_PLAY_NIGHTS_DRILL9",
	"S_PLAY_NIGHTS_FLYA",
	"S_PLAY_NIGHTS_DRILLA",
	"S_PLAY_NIGHTS_FLYB",
	"S_PLAY_NIGHTS_DRILLB",
	"S_PLAY_NIGHTS_FLYC",
	"S_PLAY_NIGHTS_DRILLC",

	// Blue Crawla
	"S_POSS_STND",
	"S_POSS_RUN1",
	"S_POSS_RUN2",
	"S_POSS_RUN3",
	"S_POSS_RUN4",
	"S_POSS_RUN5",
	"S_POSS_RUN6",

	// Red Crawla
	"S_SPOS_STND",
	"S_SPOS_RUN1",
	"S_SPOS_RUN2",
	"S_SPOS_RUN3",
	"S_SPOS_RUN4",
	"S_SPOS_RUN5",
	"S_SPOS_RUN6",

	// Greenflower Fish
	"S_FISH1",
	"S_FISH2",
	"S_FISH3",
	"S_FISH4",

	// Buzz (Gold)
	"S_BUZZLOOK1",
	"S_BUZZLOOK2",
	"S_BUZZFLY1",
	"S_BUZZFLY2",

	// Buzz (Red)
	"S_RBUZZLOOK1",
	"S_RBUZZLOOK2",
	"S_RBUZZFLY1",
	"S_RBUZZFLY2",

	// AquaBuzz
	"S_BBUZZFLY1",
	"S_BBUZZFLY2",

	// Jetty-Syn Bomber
	"S_JETBLOOK1",
	"S_JETBLOOK2",
	"S_JETBZOOM1",
	"S_JETBZOOM2",

	// Jetty-Syn Gunner
	"S_JETGLOOK1",
	"S_JETGLOOK2",
	"S_JETGZOOM1",
	"S_JETGZOOM2",
	"S_JETGSHOOT1",
	"S_JETGSHOOT2",

	// Crawla Commander
	"S_CCOMMAND1",
	"S_CCOMMAND2",
	"S_CCOMMAND3",
	"S_CCOMMAND4",

	// Deton
	"S_DETON1",
	"S_DETON2",
	"S_DETON3",
	"S_DETON4",
	"S_DETON5",
	"S_DETON6",
	"S_DETON7",
	"S_DETON8",
	"S_DETON9",
	"S_DETON10",
	"S_DETON11",
	"S_DETON12",
	"S_DETON13",
	"S_DETON14",
	"S_DETON15",
	"S_DETON16",

	// Skim Mine Dropper
	"S_SKIM1",
	"S_SKIM2",
	"S_SKIM3",
	"S_SKIM4",

	// THZ Turret
	"S_TURRET",
	"S_TURRETFIRE",
	"S_TURRETSHOCK1",
	"S_TURRETSHOCK2",
	"S_TURRETSHOCK3",
	"S_TURRETSHOCK4",
	"S_TURRETSHOCK5",
	"S_TURRETSHOCK6",
	"S_TURRETSHOCK7",
	"S_TURRETSHOCK8",
	"S_TURRETSHOCK9",

	// Popup Turret
	"S_TURRETLOOK",
	"S_TURRETSEE",
	"S_TURRETPOPUP1",
	"S_TURRETPOPUP2",
	"S_TURRETPOPUP3",
	"S_TURRETPOPUP4",
	"S_TURRETPOPUP5",
	"S_TURRETPOPUP6",
	"S_TURRETPOPUP7",
	"S_TURRETPOPUP8",
	"S_TURRETSHOOT",
	"S_TURRETPOPDOWN1",
	"S_TURRETPOPDOWN2",
	"S_TURRETPOPDOWN3",
	"S_TURRETPOPDOWN4",
	"S_TURRETPOPDOWN5",
	"S_TURRETPOPDOWN6",
	"S_TURRETPOPDOWN7",
	"S_TURRETPOPDOWN8",

	// Sharp
	"S_SHARP_ROAM1",
	"S_SHARP_ROAM2",
	"S_SHARP_AIM1",
	"S_SHARP_AIM2",
	"S_SHARP_AIM3",
	"S_SHARP_AIM4",
	"S_SHARP_SPIN",

	// Jet Jaw
	"S_JETJAW_ROAM1",
	"S_JETJAW_ROAM2",
	"S_JETJAW_ROAM3",
	"S_JETJAW_ROAM4",
	"S_JETJAW_ROAM5",
	"S_JETJAW_ROAM6",
	"S_JETJAW_ROAM7",
	"S_JETJAW_ROAM8",
	"S_JETJAW_CHOMP1",
	"S_JETJAW_CHOMP2",
	"S_JETJAW_CHOMP3",
	"S_JETJAW_CHOMP4",
	"S_JETJAW_CHOMP5",
	"S_JETJAW_CHOMP6",
	"S_JETJAW_CHOMP7",
	"S_JETJAW_CHOMP8",
	"S_JETJAW_CHOMP9",
	"S_JETJAW_CHOMP10",
	"S_JETJAW_CHOMP11",
	"S_JETJAW_CHOMP12",
	"S_JETJAW_CHOMP13",
	"S_JETJAW_CHOMP14",
	"S_JETJAW_CHOMP15",
	"S_JETJAW_CHOMP16",

	// Snailer
	"S_SNAILER1",

	// Vulture
	"S_VULTURE_STND",
	"S_VULTURE_VTOL1",
	"S_VULTURE_VTOL2",
	"S_VULTURE_VTOL3",
	"S_VULTURE_VTOL4",
	"S_VULTURE_ZOOM1",
	"S_VULTURE_ZOOM2",
	"S_VULTURE_ZOOM3",
	"S_VULTURE_ZOOM4",
	"S_VULTURE_ZOOM5",

	// Pointy
	"S_POINTY1",
	"S_POINTYBALL1",

	// Robo-Hood
	"S_ROBOHOOD_LOOK",
	"S_ROBOHOOD_STND",
	"S_ROBOHOOD_SHOOT",
	"S_ROBOHOOD_JUMP",
	"S_ROBOHOOD_JUMP2",
	"S_ROBOHOOD_FALL",

	// CastleBot FaceStabber
	"S_FACESTABBER_STND1",
	"S_FACESTABBER_STND2",
	"S_FACESTABBER_STND3",
	"S_FACESTABBER_STND4",
	"S_FACESTABBER_STND5",
	"S_FACESTABBER_STND6",
	"S_FACESTABBER_CHARGE1",
	"S_FACESTABBER_CHARGE2",
	"S_FACESTABBER_CHARGE3",
	"S_FACESTABBER_CHARGE4",

	// Egg Guard
	"S_EGGGUARD_STND",
	"S_EGGGUARD_WALK1",
	"S_EGGGUARD_WALK2",
	"S_EGGGUARD_WALK3",
	"S_EGGGUARD_WALK4",
	"S_EGGGUARD_MAD1",
	"S_EGGGUARD_MAD2",
	"S_EGGGUARD_MAD3",
	"S_EGGGUARD_RUN1",
	"S_EGGGUARD_RUN2",
	"S_EGGGUARD_RUN3",
	"S_EGGGUARD_RUN4",

	// Egg Shield for Egg Guard
	"S_EGGSHIELD",

	// Green Snapper
	"S_GSNAPPER_STND",
	"S_GSNAPPER1",
	"S_GSNAPPER2",
	"S_GSNAPPER3",
	"S_GSNAPPER4",

	// Minus
	"S_MINUS_STND",
	"S_MINUS_DIGGING",
	"S_MINUS_POPUP",
	"S_MINUS_UPWARD1",
	"S_MINUS_UPWARD2",
	"S_MINUS_UPWARD3",
	"S_MINUS_UPWARD4",
	"S_MINUS_UPWARD5",
	"S_MINUS_UPWARD6",
	"S_MINUS_UPWARD7",
	"S_MINUS_UPWARD8",
	"S_MINUS_DOWNWARD1",
	"S_MINUS_DOWNWARD2",
	"S_MINUS_DOWNWARD3",
	"S_MINUS_DOWNWARD4",
	"S_MINUS_DOWNWARD5",
	"S_MINUS_DOWNWARD6",
	"S_MINUS_DOWNWARD7",
	"S_MINUS_DOWNWARD8",

	// Spring Shell
	"S_SSHELL_STND",
	"S_SSHELL_RUN1",
	"S_SSHELL_RUN2",
	"S_SSHELL_RUN3",
	"S_SSHELL_RUN4",
	"S_SSHELL_SPRING1",
	"S_SSHELL_SPRING2",
	"S_SSHELL_SPRING3",
	"S_SSHELL_SPRING4",

	// Spring Shell (yellow)
	"S_YSHELL_STND",
	"S_YSHELL_RUN1",
	"S_YSHELL_RUN2",
	"S_YSHELL_RUN3",
	"S_YSHELL_RUN4",
	"S_YSHELL_SPRING1",
	"S_YSHELL_SPRING2",
	"S_YSHELL_SPRING3",
	"S_YSHELL_SPRING4",

	// Unidus
	"S_UNIDUS_STND",
	"S_UNIDUS_RUN",
	"S_UNIDUS_BALL",

	// Boss Explosion
	"S_BPLD1",
	"S_BPLD2",
	"S_BPLD3",
	"S_BPLD4",
	"S_BPLD5",
	"S_BPLD6",
	"S_BPLD7",

	// S3&K Boss Explosion
	"S_SONIC3KBOSSEXPLOSION1",
	"S_SONIC3KBOSSEXPLOSION2",
	"S_SONIC3KBOSSEXPLOSION3",
	"S_SONIC3KBOSSEXPLOSION4",
	"S_SONIC3KBOSSEXPLOSION5",
	"S_SONIC3KBOSSEXPLOSION6",

	"S_JETFUME1",
	"S_JETFUME2",

	// Boss 1
	"S_EGGMOBILE_STND",
	"S_EGGMOBILE_LATK1",
	"S_EGGMOBILE_LATK2",
	"S_EGGMOBILE_LATK3",
	"S_EGGMOBILE_LATK4",
	"S_EGGMOBILE_LATK5",
	"S_EGGMOBILE_LATK6",
	"S_EGGMOBILE_LATK7",
	"S_EGGMOBILE_LATK8",
	"S_EGGMOBILE_LATK9",
	"S_EGGMOBILE_LATK10",
	"S_EGGMOBILE_RATK1",
	"S_EGGMOBILE_RATK2",
	"S_EGGMOBILE_RATK3",
	"S_EGGMOBILE_RATK4",
	"S_EGGMOBILE_RATK5",
	"S_EGGMOBILE_RATK6",
	"S_EGGMOBILE_RATK7",
	"S_EGGMOBILE_RATK8",
	"S_EGGMOBILE_RATK9",
	"S_EGGMOBILE_RATK10",
	"S_EGGMOBILE_PANIC1",
	"S_EGGMOBILE_PANIC2",
	"S_EGGMOBILE_PANIC3",
	"S_EGGMOBILE_PANIC4",
	"S_EGGMOBILE_PANIC5",
	"S_EGGMOBILE_PANIC6",
	"S_EGGMOBILE_PANIC7",
	"S_EGGMOBILE_PANIC8",
	"S_EGGMOBILE_PANIC9",
	"S_EGGMOBILE_PANIC10",
	"S_EGGMOBILE_PAIN",
	"S_EGGMOBILE_PAIN2",
	"S_EGGMOBILE_DIE1",
	"S_EGGMOBILE_DIE2",
	"S_EGGMOBILE_DIE3",
	"S_EGGMOBILE_DIE4",
	"S_EGGMOBILE_DIE5",
	"S_EGGMOBILE_DIE6",
	"S_EGGMOBILE_DIE7",
	"S_EGGMOBILE_DIE8",
	"S_EGGMOBILE_DIE9",
	"S_EGGMOBILE_DIE10",
	"S_EGGMOBILE_DIE11",
	"S_EGGMOBILE_DIE12",
	"S_EGGMOBILE_DIE13",
	"S_EGGMOBILE_DIE14",
	"S_EGGMOBILE_FLEE1",
	"S_EGGMOBILE_FLEE2",
	"S_EGGMOBILE_BALL",
	"S_EGGMOBILE_TARGET",

	// Boss 2
	"S_EGGMOBILE2_STND",
	"S_EGGMOBILE2_POGO1",
	"S_EGGMOBILE2_POGO2",
	"S_EGGMOBILE2_POGO3",
	"S_EGGMOBILE2_POGO4",
	"S_EGGMOBILE2_POGO5",
	"S_EGGMOBILE2_POGO6",
	"S_EGGMOBILE2_POGO7",
	"S_EGGMOBILE2_PAIN",
	"S_EGGMOBILE2_PAIN2",
	"S_EGGMOBILE2_DIE1",
	"S_EGGMOBILE2_DIE2",
	"S_EGGMOBILE2_DIE3",
	"S_EGGMOBILE2_DIE4",
	"S_EGGMOBILE2_DIE5",
	"S_EGGMOBILE2_DIE6",
	"S_EGGMOBILE2_DIE7",
	"S_EGGMOBILE2_DIE8",
	"S_EGGMOBILE2_DIE9",
	"S_EGGMOBILE2_DIE10",
	"S_EGGMOBILE2_DIE11",
	"S_EGGMOBILE2_DIE12",
	"S_EGGMOBILE2_DIE13",
	"S_EGGMOBILE2_DIE14",
	"S_EGGMOBILE2_FLEE1",
	"S_EGGMOBILE2_FLEE2",

	"S_BOSSTANK1",
	"S_BOSSTANK2",
	"S_BOSSSPIGOT",

	// Boss 2 Goop
	"S_GOOP1",
	"S_GOOP2",
	"S_GOOP3",
	"S_GOOPTRAIL",

	// Boss 3
	"S_EGGMOBILE3_STND",
	"S_EGGMOBILE3_ATK1",
	"S_EGGMOBILE3_ATK2",
	"S_EGGMOBILE3_ATK3A",
	"S_EGGMOBILE3_ATK3B",
	"S_EGGMOBILE3_ATK3C",
	"S_EGGMOBILE3_ATK3D",
	"S_EGGMOBILE3_ATK4",
	"S_EGGMOBILE3_ATK5",
	"S_EGGMOBILE3_LAUGH1",
	"S_EGGMOBILE3_LAUGH2",
	"S_EGGMOBILE3_LAUGH3",
	"S_EGGMOBILE3_LAUGH4",
	"S_EGGMOBILE3_LAUGH5",
	"S_EGGMOBILE3_LAUGH6",
	"S_EGGMOBILE3_LAUGH7",
	"S_EGGMOBILE3_LAUGH8",
	"S_EGGMOBILE3_LAUGH9",
	"S_EGGMOBILE3_LAUGH10",
	"S_EGGMOBILE3_LAUGH11",
	"S_EGGMOBILE3_LAUGH12",
	"S_EGGMOBILE3_LAUGH13",
	"S_EGGMOBILE3_LAUGH14",
	"S_EGGMOBILE3_LAUGH15",
	"S_EGGMOBILE3_LAUGH16",
	"S_EGGMOBILE3_LAUGH17",
	"S_EGGMOBILE3_LAUGH18",
	"S_EGGMOBILE3_LAUGH19",
	"S_EGGMOBILE3_LAUGH20",
	"S_EGGMOBILE3_PAIN",
	"S_EGGMOBILE3_PAIN2",
	"S_EGGMOBILE3_DIE1",
	"S_EGGMOBILE3_DIE2",
	"S_EGGMOBILE3_DIE3",
	"S_EGGMOBILE3_DIE4",
	"S_EGGMOBILE3_DIE5",
	"S_EGGMOBILE3_DIE6",
	"S_EGGMOBILE3_DIE7",
	"S_EGGMOBILE3_DIE8",
	"S_EGGMOBILE3_DIE9",
	"S_EGGMOBILE3_DIE10",
	"S_EGGMOBILE3_DIE11",
	"S_EGGMOBILE3_DIE12",
	"S_EGGMOBILE3_DIE13",
	"S_EGGMOBILE3_DIE14",
	"S_EGGMOBILE3_FLEE1",
	"S_EGGMOBILE3_FLEE2",

	// Boss 3 Propeller
	"S_PROPELLER1",
	"S_PROPELLER2",
	"S_PROPELLER3",
	"S_PROPELLER4",
	"S_PROPELLER5",
	"S_PROPELLER6",
	"S_PROPELLER7",

	// Boss 3 pinch
	"S_FAKEMOBILE_INIT",
	"S_FAKEMOBILE",
	"S_FAKEMOBILE_ATK1",
	"S_FAKEMOBILE_ATK2",
	"S_FAKEMOBILE_ATK3A",
	"S_FAKEMOBILE_ATK3B",
	"S_FAKEMOBILE_ATK3C",
	"S_FAKEMOBILE_ATK3D",
	"S_FAKEMOBILE_ATK4",
	"S_FAKEMOBILE_ATK5",

	// Boss 4
	"S_EGGMOBILE4_STND",
	"S_EGGMOBILE4_LATK1",
	"S_EGGMOBILE4_LATK2",
	"S_EGGMOBILE4_LATK3",
	"S_EGGMOBILE4_LATK4",
	"S_EGGMOBILE4_LATK5",
	"S_EGGMOBILE4_LATK6",
	"S_EGGMOBILE4_RATK1",
	"S_EGGMOBILE4_RATK2",
	"S_EGGMOBILE4_RATK3",
	"S_EGGMOBILE4_RATK4",
	"S_EGGMOBILE4_RATK5",
	"S_EGGMOBILE4_RATK6",
	"S_EGGMOBILE4_RAISE1",
	"S_EGGMOBILE4_RAISE2",
	"S_EGGMOBILE4_RAISE3",
	"S_EGGMOBILE4_RAISE4",
	"S_EGGMOBILE4_RAISE5",
	"S_EGGMOBILE4_RAISE6",
	"S_EGGMOBILE4_RAISE7",
	"S_EGGMOBILE4_RAISE8",
	"S_EGGMOBILE4_RAISE9",
	"S_EGGMOBILE4_RAISE10",
	"S_EGGMOBILE4_PAIN",
	"S_EGGMOBILE4_DIE1",
	"S_EGGMOBILE4_DIE2",
	"S_EGGMOBILE4_DIE3",
	"S_EGGMOBILE4_DIE4",
	"S_EGGMOBILE4_DIE5",
	"S_EGGMOBILE4_DIE6",
	"S_EGGMOBILE4_DIE7",
	"S_EGGMOBILE4_DIE8",
	"S_EGGMOBILE4_DIE9",
	"S_EGGMOBILE4_DIE10",
	"S_EGGMOBILE4_DIE11",
	"S_EGGMOBILE4_DIE12",
	"S_EGGMOBILE4_DIE13",
	"S_EGGMOBILE4_DIE14",
	"S_EGGMOBILE4_FLEE1",
	"S_EGGMOBILE4_FLEE2",
	"S_EGGMOBILE4_MACE",

	// Boss 4 jet flame
	"S_JETFLAME1",
	"S_JETFLAME2",

	// Black Eggman (Boss 7)
	"S_BLACKEGG_STND",
	"S_BLACKEGG_STND2",
	"S_BLACKEGG_WALK1",
	"S_BLACKEGG_WALK2",
	"S_BLACKEGG_WALK3",
	"S_BLACKEGG_WALK4",
	"S_BLACKEGG_WALK5",
	"S_BLACKEGG_WALK6",
	"S_BLACKEGG_SHOOT1",
	"S_BLACKEGG_SHOOT2",
	"S_BLACKEGG_PAIN1",
	"S_BLACKEGG_PAIN2",
	"S_BLACKEGG_PAIN3",
	"S_BLACKEGG_PAIN4",
	"S_BLACKEGG_PAIN5",
	"S_BLACKEGG_PAIN6",
	"S_BLACKEGG_PAIN7",
	"S_BLACKEGG_PAIN8",
	"S_BLACKEGG_PAIN9",
	"S_BLACKEGG_PAIN10",
	"S_BLACKEGG_PAIN11",
	"S_BLACKEGG_PAIN12",
	"S_BLACKEGG_PAIN13",
	"S_BLACKEGG_PAIN14",
	"S_BLACKEGG_PAIN15",
	"S_BLACKEGG_PAIN16",
	"S_BLACKEGG_PAIN17",
	"S_BLACKEGG_PAIN18",
	"S_BLACKEGG_PAIN19",
	"S_BLACKEGG_PAIN20",
	"S_BLACKEGG_PAIN21",
	"S_BLACKEGG_PAIN22",
	"S_BLACKEGG_PAIN23",
	"S_BLACKEGG_PAIN24",
	"S_BLACKEGG_PAIN25",
	"S_BLACKEGG_PAIN26",
	"S_BLACKEGG_PAIN27",
	"S_BLACKEGG_PAIN28",
	"S_BLACKEGG_PAIN29",
	"S_BLACKEGG_PAIN30",
	"S_BLACKEGG_PAIN31",
	"S_BLACKEGG_PAIN32",
	"S_BLACKEGG_PAIN33",
	"S_BLACKEGG_PAIN34",
	"S_BLACKEGG_PAIN35",
	"S_BLACKEGG_HITFACE1",
	"S_BLACKEGG_HITFACE2",
	"S_BLACKEGG_HITFACE3",
	"S_BLACKEGG_HITFACE4",
	"S_BLACKEGG_DIE1",
	"S_BLACKEGG_DIE2",
	"S_BLACKEGG_DIE3",
	"S_BLACKEGG_DIE4",
	"S_BLACKEGG_DIE5",
	"S_BLACKEGG_MISSILE1",
	"S_BLACKEGG_MISSILE2",
	"S_BLACKEGG_MISSILE3",
	"S_BLACKEGG_GOOP",
	"S_BLACKEGG_JUMP1",
	"S_BLACKEGG_JUMP2",
	"S_BLACKEGG_DESTROYPLAT1",
	"S_BLACKEGG_DESTROYPLAT2",
	"S_BLACKEGG_DESTROYPLAT3",

	"S_BLACKEGG_HELPER", // Collision helper

	"S_BLACKEGG_GOOP1",
	"S_BLACKEGG_GOOP2",
	"S_BLACKEGG_GOOP3",
	"S_BLACKEGG_GOOP4",
	"S_BLACKEGG_GOOP5",
	"S_BLACKEGG_GOOP6",
	"S_BLACKEGG_GOOP7",

	"S_BLACKEGG_MISSILE",

	// New Very-Last-Minute 2.1 Brak Eggman (Cy-Brak-demon)
	"S_CYBRAKDEMON_IDLE",
	"S_CYBRAKDEMON_WALK1",
	"S_CYBRAKDEMON_WALK2",
	"S_CYBRAKDEMON_WALK3",
	"S_CYBRAKDEMON_WALK4",
	"S_CYBRAKDEMON_WALK5",
	"S_CYBRAKDEMON_WALK6",
	"S_CYBRAKDEMON_CHOOSE_ATTACK1",
	"S_CYBRAKDEMON_MISSILE_ATTACK1", // Aim
	"S_CYBRAKDEMON_MISSILE_ATTACK2", // Fire
	"S_CYBRAKDEMON_MISSILE_ATTACK3", // Aim
	"S_CYBRAKDEMON_MISSILE_ATTACK4", // Fire
	"S_CYBRAKDEMON_MISSILE_ATTACK5", // Aim
	"S_CYBRAKDEMON_MISSILE_ATTACK6", // Fire
	"S_CYBRAKDEMON_FLAME_ATTACK1", // Reset
	"S_CYBRAKDEMON_FLAME_ATTACK2", // Aim
	"S_CYBRAKDEMON_FLAME_ATTACK3", // Fire
	"S_CYBRAKDEMON_FLAME_ATTACK4", // Loop
	"S_CYBRAKDEMON_CHOOSE_ATTACK2",
	"S_CYBRAKDEMON_VILE_ATTACK1",
	"S_CYBRAKDEMON_VILE_ATTACK2",
	"S_CYBRAKDEMON_VILE_ATTACK3",
	"S_CYBRAKDEMON_VILE_ATTACK4",
	"S_CYBRAKDEMON_VILE_ATTACK5",
	"S_CYBRAKDEMON_VILE_ATTACK6",
	"S_CYBRAKDEMON_NAPALM_ATTACK1",
	"S_CYBRAKDEMON_NAPALM_ATTACK2",
	"S_CYBRAKDEMON_NAPALM_ATTACK3",
	"S_CYBRAKDEMON_FINISH_ATTACK", // If just attacked, remove MF2_FRET w/out going back to spawnstate
	"S_CYBRAKDEMON_FINISH_ATTACK2", // Force a delay between attacks so you don't get bombarded with them back-to-back
	"S_CYBRAKDEMON_PAIN1",
	"S_CYBRAKDEMON_PAIN2",
	"S_CYBRAKDEMON_PAIN3",
	"S_CYBRAKDEMON_DIE1",
	"S_CYBRAKDEMON_DIE2",
	"S_CYBRAKDEMON_DIE3",
	"S_CYBRAKDEMON_DIE4",
	"S_CYBRAKDEMON_DIE5",
	"S_CYBRAKDEMON_DIE6",
	"S_CYBRAKDEMON_DIE7",
	"S_CYBRAKDEMON_DIE8",
	"S_CYBRAKDEMON_DEINVINCIBLERIZE",
	"S_CYBRAKDEMON_INVINCIBLERIZE",

	"S_CYBRAKDEMONMISSILE",
	"S_CYBRAKDEMONMISSILE_EXPLODE1",
	"S_CYBRAKDEMONMISSILE_EXPLODE2",
	"S_CYBRAKDEMONMISSILE_EXPLODE3",

	"S_CYBRAKDEMONFLAMESHOT_FLY1",
	"S_CYBRAKDEMONFLAMESHOT_FLY2",
	"S_CYBRAKDEMONFLAMESHOT_FLY3",
	"S_CYBRAKDEMONFLAMESHOT_DIE",

	"S_CYBRAKDEMONFLAMEREST",

	"S_CYBRAKDEMONELECTRICBARRIER_INIT1",
	"S_CYBRAKDEMONELECTRICBARRIER_INIT2",
	"S_CYBRAKDEMONELECTRICBARRIER_PLAYSOUND",
	"S_CYBRAKDEMONELECTRICBARRIER1",
	"S_CYBRAKDEMONELECTRICBARRIER2",
	"S_CYBRAKDEMONELECTRICBARRIER3",
	"S_CYBRAKDEMONELECTRICBARRIER4",
	"S_CYBRAKDEMONELECTRICBARRIER5",
	"S_CYBRAKDEMONELECTRICBARRIER6",
	"S_CYBRAKDEMONELECTRICBARRIER7",
	"S_CYBRAKDEMONELECTRICBARRIER8",
	"S_CYBRAKDEMONELECTRICBARRIER9",
	"S_CYBRAKDEMONELECTRICBARRIER10",
	"S_CYBRAKDEMONELECTRICBARRIER11",
	"S_CYBRAKDEMONELECTRICBARRIER12",
	"S_CYBRAKDEMONELECTRICBARRIER13",
	"S_CYBRAKDEMONELECTRICBARRIER14",
	"S_CYBRAKDEMONELECTRICBARRIER15",
	"S_CYBRAKDEMONELECTRICBARRIER16",
	"S_CYBRAKDEMONELECTRICBARRIER17",
	"S_CYBRAKDEMONELECTRICBARRIER18",
	"S_CYBRAKDEMONELECTRICBARRIER19",
	"S_CYBRAKDEMONELECTRICBARRIER20",
	"S_CYBRAKDEMONELECTRICBARRIER21",
	"S_CYBRAKDEMONELECTRICBARRIER22",
	"S_CYBRAKDEMONELECTRICBARRIER23",
	"S_CYBRAKDEMONELECTRICBARRIER24",
	"S_CYBRAKDEMONELECTRICBARRIER_DIE1",
	"S_CYBRAKDEMONELECTRICBARRIER_DIE2",
	"S_CYBRAKDEMONELECTRICBARRIER_DIE3",
	"S_CYBRAKDEMONELECTRICBARRIER_SPARK_RANDOMCHECK",
	"S_CYBRAKDEMONELECTRICBARRIER_SPARK_RANDOMSUCCESS",
	"S_CYBRAKDEMONELECTRICBARRIER_SPARK_RANDOMCHOOSE",
	"S_CYBRAKDEMONELECTRICBARRIER_SPARK_RANDOM1",
	"S_CYBRAKDEMONELECTRICBARRIER_SPARK_RANDOM2",
	"S_CYBRAKDEMONELECTRICBARRIER_SPARK_RANDOM3",
	"S_CYBRAKDEMONELECTRICBARRIER_SPARK_RANDOM4",
	"S_CYBRAKDEMONELECTRICBARRIER_SPARK_RANDOM5",
	"S_CYBRAKDEMONELECTRICBARRIER_SPARK_RANDOM6",
	"S_CYBRAKDEMONELECTRICBARRIER_SPARK_RANDOM7",
	"S_CYBRAKDEMONELECTRICBARRIER_SPARK_RANDOM8",
	"S_CYBRAKDEMONELECTRICBARRIER_SPARK_RANDOM9",
	"S_CYBRAKDEMONELECTRICBARRIER_SPARK_RANDOM10",
	"S_CYBRAKDEMONELECTRICBARRIER_SPARK_RANDOM11",
	"S_CYBRAKDEMONELECTRICBARRIER_SPARK_RANDOM12",
	"S_CYBRAKDEMONELECTRICBARRIER_SPARK_RANDOMFAIL",
	"S_CYBRAKDEMONELECTRICBARRIER_SPARK_RANDOMLOOP",
	"S_CYBRAKDEMONELECTRICBARRIER_REVIVE1",
	"S_CYBRAKDEMONELECTRICBARRIER_REVIVE2",
	"S_CYBRAKDEMONELECTRICBARRIER_REVIVE3",

	"S_CYBRAKDEMONTARGETRETICULE1",
	"S_CYBRAKDEMONTARGETRETICULE2",
	"S_CYBRAKDEMONTARGETRETICULE3",
	"S_CYBRAKDEMONTARGETRETICULE4",
	"S_CYBRAKDEMONTARGETRETICULE5",
	"S_CYBRAKDEMONTARGETRETICULE6",
	"S_CYBRAKDEMONTARGETRETICULE7",
	"S_CYBRAKDEMONTARGETRETICULE8",
	"S_CYBRAKDEMONTARGETRETICULE9",
	"S_CYBRAKDEMONTARGETRETICULE10",
	"S_CYBRAKDEMONTARGETRETICULE11",
	"S_CYBRAKDEMONTARGETRETICULE12",
	"S_CYBRAKDEMONTARGETRETICULE13",
	"S_CYBRAKDEMONTARGETRETICULE14",

	"S_CYBRAKDEMONTARGETDOT",

	"S_CYBRAKDEMONNAPALMBOMBLARGE_FLY1",
	"S_CYBRAKDEMONNAPALMBOMBLARGE_FLY2",
	"S_CYBRAKDEMONNAPALMBOMBLARGE_FLY3",
	"S_CYBRAKDEMONNAPALMBOMBLARGE_FLY4",
	"S_CYBRAKDEMONNAPALMBOMBLARGE_DIE1", // Explode
	"S_CYBRAKDEMONNAPALMBOMBLARGE_DIE2", // Outer ring
	"S_CYBRAKDEMONNAPALMBOMBLARGE_DIE3", // Center
	"S_CYBRAKDEMONNAPALMBOMBLARGE_DIE4", // Sound

	"S_CYBRAKDEMONNAPALMBOMBSMALL",
	"S_CYBRAKDEMONNAPALMBOMBSMALL_DIE1", // Explode
	"S_CYBRAKDEMONNAPALMBOMBSMALL_DIE2", // Outer ring
	"S_CYBRAKDEMONNAPALMBOMBSMALL_DIE3", // Inner ring
	"S_CYBRAKDEMONNAPALMBOMBSMALL_DIE4", // Center
	"S_CYBRAKDEMONNAPALMBOMBSMALL_DIE5", // Sound

	"S_CYBRAKDEMONNAPALMFLAME_FLY1",
	"S_CYBRAKDEMONNAPALMFLAME_FLY2",
	"S_CYBRAKDEMONNAPALMFLAME_FLY3",
	"S_CYBRAKDEMONNAPALMFLAME_FLY4",
	"S_CYBRAKDEMONNAPALMFLAME_FLY5",
	"S_CYBRAKDEMONNAPALMFLAME_FLY6",
	"S_CYBRAKDEMONNAPALMFLAME_DIE",

	"S_CYBRAKDEMONVILEEXPLOSION1",
	"S_CYBRAKDEMONVILEEXPLOSION2",
	"S_CYBRAKDEMONVILEEXPLOSION3",

	// Metal Sonic (Race)
	// S_PLAY_STND
	"S_METALSONIC_STAND",
	// S_PLAY_TAP1
	"S_METALSONIC_WAIT1",
	"S_METALSONIC_WAIT2",
	// S_PLAY_WALK
	"S_METALSONIC_WALK1",
	"S_METALSONIC_WALK2",
	"S_METALSONIC_WALK3",
	"S_METALSONIC_WALK4",
	"S_METALSONIC_WALK5",
	"S_METALSONIC_WALK6",
	"S_METALSONIC_WALK7",
	"S_METALSONIC_WALK8",
	// S_PLAY_SPD1
	"S_METALSONIC_RUN1",
	"S_METALSONIC_RUN2",
	"S_METALSONIC_RUN3",
	"S_METALSONIC_RUN4",
	// Metal Sonic (Battle)
	"S_METALSONIC_FLOAT",
	"S_METALSONIC_VECTOR",
	"S_METALSONIC_STUN",
	"S_METALSONIC_RAISE",
	"S_METALSONIC_GATHER",
	"S_METALSONIC_DASH",
	"S_METALSONIC_BOUNCE",
	"S_METALSONIC_BADBOUNCE",
	"S_METALSONIC_SHOOT",
	"S_METALSONIC_PAIN",
	"S_METALSONIC_DEATH",
	"S_METALSONIC_FLEE1",
	"S_METALSONIC_FLEE2",
	"S_METALSONIC_FLEE3",
	"S_METALSONIC_FLEE4",

	"S_MSSHIELD_F1",
	"S_MSSHIELD_F2",
	"S_MSSHIELD_F3",
	"S_MSSHIELD_F4",
	"S_MSSHIELD_F5",
	"S_MSSHIELD_F6",
	"S_MSSHIELD_F7",
	"S_MSSHIELD_F8",
	"S_MSSHIELD_F9",
	"S_MSSHIELD_F10",
	"S_MSSHIELD_F11",
	"S_MSSHIELD_F12",

	// Ring
	"S_RING",

	// Blue Sphere for special stages
	"S_BLUEBALL",
	"S_BLUEBALLSPARK",

	// Gravity Wells for special stages
	"S_GRAVWELLGREEN",
	"S_GRAVWELLGREEN2",
	"S_GRAVWELLGREEN3",

	"S_GRAVWELLRED",
	"S_GRAVWELLRED2",
	"S_GRAVWELLRED3",

	// Individual Team Rings
	"S_TEAMRING",

	// Special Stage Token
	"S_TOKEN",

	// CTF Flags
	"S_REDFLAG",
	"S_BLUEFLAG",

	// Emblem
	"S_EMBLEM1",
	"S_EMBLEM2",
	"S_EMBLEM3",
	"S_EMBLEM4",
	"S_EMBLEM5",
	"S_EMBLEM6",
	"S_EMBLEM7",
	"S_EMBLEM8",
	"S_EMBLEM9",
	"S_EMBLEM10",
	"S_EMBLEM11",
	"S_EMBLEM12",
	"S_EMBLEM13",
	"S_EMBLEM14",
	"S_EMBLEM15",
	"S_EMBLEM16",
	"S_EMBLEM17",
	"S_EMBLEM18",
	"S_EMBLEM19",
	"S_EMBLEM20",
	"S_EMBLEM21",
	"S_EMBLEM22",
	"S_EMBLEM23",
	"S_EMBLEM24",
	"S_EMBLEM25",
	"S_EMBLEM26",

	// Chaos Emeralds
	"S_CEMG1",
	"S_CEMG2",
	"S_CEMG3",
	"S_CEMG4",
	"S_CEMG5",
	"S_CEMG6",
	"S_CEMG7",

	// Emeralds (for hunt)
	"S_EMER1",

	"S_FAN",
	"S_FAN2",
	"S_FAN3",
	"S_FAN4",
	"S_FAN5",

	// Bubble Source
	"S_BUBBLES1",
	"S_BUBBLES2",
	"S_BUBBLES3",
	"S_BUBBLES4",

	// Level End Sign
	"S_SIGN1",
	"S_SIGN2",
	"S_SIGN3",
	"S_SIGN4",
	"S_SIGN5",
	"S_SIGN6",
	"S_SIGN7",
	"S_SIGN8",
	"S_SIGN9",
	"S_SIGN10",
	"S_SIGN11",
	"S_SIGN12",
	"S_SIGN13",
	"S_SIGN14",
	"S_SIGN15",
	"S_SIGN16",
	"S_SIGN17",
	"S_SIGN18",
	"S_SIGN19",
	"S_SIGN20",
	"S_SIGN21",
	"S_SIGN22",
	"S_SIGN23",
	"S_SIGN24",
	"S_SIGN25",
	"S_SIGN26",
	"S_SIGN27",
	"S_SIGN28",
	"S_SIGN29",
	"S_SIGN30",
	"S_SIGN31",
	"S_SIGN32",
	"S_SIGN33",
	"S_SIGN34",
	"S_SIGN35",
	"S_SIGN36",
	"S_SIGN37",
	"S_SIGN38",
	"S_SIGN39",
	"S_SIGN40",
	"S_SIGN41",
	"S_SIGN42",
	"S_SIGN43",
	"S_SIGN44",
	"S_SIGN45",
	"S_SIGN46",
	"S_SIGN47",
	"S_SIGN48",
	"S_SIGN49",
	"S_SIGN50",
	"S_SIGN51",
	"S_SIGN52", // Eggman
	"S_SIGN53",

	// Steam Riser
	"S_STEAM1",
	"S_STEAM2",
	"S_STEAM3",
	"S_STEAM4",
	"S_STEAM5",
	"S_STEAM6",
	"S_STEAM7",
	"S_STEAM8",

	// Spike Ball
	"S_SPIKEBALL1",
	"S_SPIKEBALL2",
	"S_SPIKEBALL3",
	"S_SPIKEBALL4",
	"S_SPIKEBALL5",
	"S_SPIKEBALL6",
	"S_SPIKEBALL7",
	"S_SPIKEBALL8",

	// Elemental Shield's Spawn
	"S_SPINFIRE1",
	"S_SPINFIRE2",
	"S_SPINFIRE3",
	"S_SPINFIRE4",
	"S_SPINFIRE5",
	"S_SPINFIRE6",

	// Spikes
	"S_SPIKE1",
	"S_SPIKE2",
	"S_SPIKE3",
	"S_SPIKE4",
	"S_SPIKE5",
	"S_SPIKE6",
	"S_SPIKED1",
	"S_SPIKED2",

	// Wall spikes
	"S_WALLSPIKE1",
	"S_WALLSPIKE2",
	"S_WALLSPIKE3",
	"S_WALLSPIKE4",
	"S_WALLSPIKE5",
	"S_WALLSPIKE6",
	"S_WALLSPIKEBASE",
	"S_WALLSPIKED1",
	"S_WALLSPIKED2",

	// Starpost
	"S_STARPOST_IDLE",
	"S_STARPOST_FLASH",
	"S_STARPOST_STARTSPIN",
	"S_STARPOST_SPIN",
	"S_STARPOST_ENDSPIN",

	// Big floating mine
	"S_BIGMINE1",
	"S_BIGMINE2",
	"S_BIGMINE3",
	"S_BIGMINE4",
	"S_BIGMINE5",
	"S_BIGMINE6",
	"S_BIGMINE7",
	"S_BIGMINE8",

	// Cannon Launcher
	"S_CANNONLAUNCHER1",
	"S_CANNONLAUNCHER2",
	"S_CANNONLAUNCHER3",

	// Monitor Miscellany
	"S_BOXSPARKLE1",
	"S_BOXSPARKLE2",
	"S_BOXSPARKLE3",

	"S_BOX_FLICKER",
	"S_BOX_POP1",
	"S_BOX_POP2",

	"S_GOLDBOX_FLICKER",
	"S_GOLDBOX_OFF1",
	"S_GOLDBOX_OFF2",
	"S_GOLDBOX_OFF3",
	"S_GOLDBOX_OFF4",
	"S_GOLDBOX_OFF5",
	"S_GOLDBOX_OFF6",
	"S_GOLDBOX_OFF7",

	// Monitor States (one per box)
	"S_MYSTERY_BOX",
	"S_RING_BOX",
	"S_PITY_BOX",
	"S_ATTRACT_BOX",
	"S_FORCE_BOX",
	"S_ARMAGEDDON_BOX",
	"S_WHIRLWIND_BOX",
	"S_ELEMENTAL_BOX",
	"S_SNEAKERS_BOX",
	"S_INVULN_BOX",
	"S_1UP_BOX",
	"S_EGGMAN_BOX",
	"S_MIXUP_BOX",
	"S_GRAVITY_BOX",
	"S_RECYCLER_BOX",
	"S_SCORE1K_BOX",
	"S_SCORE10K_BOX",
	"S_FLAMEAURA_BOX",
	"S_BUBBLEWRAP_BOX",
	"S_THUNDERCOIN_BOX",

	// Gold Repeat Monitor States (one per box)
	"S_PITY_GOLDBOX",
	"S_ATTRACT_GOLDBOX",
	"S_FORCE_GOLDBOX",
	"S_ARMAGEDDON_GOLDBOX",
	"S_WHIRLWIND_GOLDBOX",
	"S_ELEMENTAL_GOLDBOX",
	"S_SNEAKERS_GOLDBOX",
	"S_INVULN_GOLDBOX",
	"S_EGGMAN_GOLDBOX",
	"S_GRAVITY_GOLDBOX",
	"S_FLAMEAURA_GOLDBOX",
	"S_BUBBLEWRAP_GOLDBOX",
	"S_THUNDERCOIN_GOLDBOX",

	// Team Ring Boxes (these are special)
	"S_RING_REDBOX1",
	"S_RING_REDBOX2",
	"S_REDBOX_POP1",
	"S_REDBOX_POP2",

	"S_RING_BLUEBOX1",
	"S_RING_BLUEBOX2",
	"S_BLUEBOX_POP1",
	"S_BLUEBOX_POP2",

	// Box Icons -- 2 states each, animation and action
	"S_RING_ICON1",
	"S_RING_ICON2",

	"S_PITY_ICON1",
	"S_PITY_ICON2",

	"S_ATTRACT_ICON1",
	"S_ATTRACT_ICON2",

	"S_FORCE_ICON1",
	"S_FORCE_ICON2",

	"S_ARMAGEDDON_ICON1",
	"S_ARMAGEDDON_ICON2",

	"S_WHIRLWIND_ICON1",
	"S_WHIRLWIND_ICON2",

	"S_ELEMENTAL_ICON1",
	"S_ELEMENTAL_ICON2",

	"S_SNEAKERS_ICON1",
	"S_SNEAKERS_ICON2",

	"S_INVULN_ICON1",
	"S_INVULN_ICON2",

	"S_1UP_ICON1",
	"S_1UP_ICON2",

	"S_EGGMAN_ICON1",
	"S_EGGMAN_ICON2",

	"S_MIXUP_ICON1",
	"S_MIXUP_ICON2",

	"S_GRAVITY_ICON1",
	"S_GRAVITY_ICON2",

	"S_RECYCLER_ICON1",
	"S_RECYCLER_ICON2",

	"S_SCORE1K_ICON1",
	"S_SCORE1K_ICON2",

	"S_SCORE10K_ICON1",
	"S_SCORE10K_ICON2",

	"S_FLAMEAURA_ICON1",
	"S_FLAMEAURA_ICON2",

	"S_BUBBLEWRAP_ICON1",
	"S_BUBBLEWRAP_ICON2",

	"S_THUNDERCOIN_ICON1",
	"S_THUNDERCOIN_ICON2",

	"S_ROCKET",

	"S_LASER",

	"S_TORPEDO",

	"S_ENERGYBALL1",
	"S_ENERGYBALL2",

	// Skim Mine, also used by Jetty-Syn bomber
	"S_MINE1",
	"S_MINE_BOOM1",
	"S_MINE_BOOM2",
	"S_MINE_BOOM3",
	"S_MINE_BOOM4",

	// Jetty-Syn Bullet
	"S_JETBULLET1",
	"S_JETBULLET2",

	"S_TURRETLASER",
	"S_TURRETLASEREXPLODE1",
	"S_TURRETLASEREXPLODE2",

	// Cannonball
	"S_CANNONBALL1",

	// Arrow
	"S_ARROW",
	"S_ARROWUP",
	"S_ARROWDOWN",

	// Trapgoyle Demon fire
	"S_DEMONFIRE1",
	"S_DEMONFIRE2",
	"S_DEMONFIRE3",
	"S_DEMONFIRE4",
	"S_DEMONFIRE5",
	"S_DEMONFIRE6",

	// GFZ flowers
	"S_GFZFLOWERA",
	"S_GFZFLOWERB",
	"S_GFZFLOWERC",

	"S_BERRYBUSH",
	"S_BUSH",

	// Trees (both GFZ and misc)
	"S_GFZTREE",
	"S_GFZBERRYTREE",
	"S_GFZCHERRYTREE",
	"S_CHECKERTREE",
	"S_CHECKERSUNSETTREE",
	"S_FHZTREE", // Frozen Hillside
	"S_FHZPINKTREE",
	"S_POLYGONTREE",
	"S_BUSHTREE",
	"S_BUSHREDTREE",

	// THZ Plant
	"S_THZFLOWERA",
	"S_THZFLOWERB",

	// THZ Alarm
	"S_ALARM1",

	// Deep Sea Gargoyle
	"S_GARGOYLE",
	"S_BIGGARGOYLE",

	// DSZ Seaweed
	"S_SEAWEED1",
	"S_SEAWEED2",
	"S_SEAWEED3",
	"S_SEAWEED4",
	"S_SEAWEED5",
	"S_SEAWEED6",

	// Dripping Water
	"S_DRIPA1",
	"S_DRIPA2",
	"S_DRIPA3",
	"S_DRIPA4",
	"S_DRIPB1",
	"S_DRIPC1",
	"S_DRIPC2",

	// Coral 1
	"S_CORAL1",

	// Coral 2
	"S_CORAL2",

	// Coral 3
	"S_CORAL3",

	// Blue Crystal
	"S_BLUECRYSTAL1",

	// CEZ Chain
	"S_CEZCHAIN",

	// Flame
	"S_FLAME1",
	"S_FLAME2",
	"S_FLAME3",
	"S_FLAME4",
	"S_FLAME5",
	"S_FLAME6",
	"S_FLAMEPARTICLE",

	"S_FLAMEREST",

	// Eggman Statue
	"S_EGGSTATUE1",

	// CEZ hidden sling
	"S_SLING1",
	"S_SLING2",

	// CEZ maces and chains
	"S_SMALLMACECHAIN",
	"S_BIGMACECHAIN",
	"S_SMALLMACE",
	"S_BIGMACE",

	// Yellow spring on a ball
	"S_YELLOWSPRINGBALL",
	"S_YELLOWSPRINGBALL2",
	"S_YELLOWSPRINGBALL3",
	"S_YELLOWSPRINGBALL4",
	"S_YELLOWSPRINGBALL5",

	// Red spring on a ball
	"S_REDSPRINGBALL",
	"S_REDSPRINGBALL2",
	"S_REDSPRINGBALL3",
	"S_REDSPRINGBALL4",
	"S_REDSPRINGBALL5",

	// Small Firebar
	"S_SMALLFIREBAR1",
	"S_SMALLFIREBAR2",
	"S_SMALLFIREBAR3",
	"S_SMALLFIREBAR4",
	"S_SMALLFIREBAR5",
	"S_SMALLFIREBAR6",
	"S_SMALLFIREBAR7",
	"S_SMALLFIREBAR8",
	"S_SMALLFIREBAR9",
	"S_SMALLFIREBAR10",
	"S_SMALLFIREBAR11",
	"S_SMALLFIREBAR12",
	"S_SMALLFIREBAR13",
	"S_SMALLFIREBAR14",
	"S_SMALLFIREBAR15",
	"S_SMALLFIREBAR16",

	// Big Firebar
	"S_BIGFIREBAR1",
	"S_BIGFIREBAR2",
	"S_BIGFIREBAR3",
	"S_BIGFIREBAR4",
	"S_BIGFIREBAR5",
	"S_BIGFIREBAR6",
	"S_BIGFIREBAR7",
	"S_BIGFIREBAR8",
	"S_BIGFIREBAR9",
	"S_BIGFIREBAR10",
	"S_BIGFIREBAR11",
	"S_BIGFIREBAR12",
	"S_BIGFIREBAR13",
	"S_BIGFIREBAR14",
	"S_BIGFIREBAR15",
	"S_BIGFIREBAR16",

	"S_CEZFLOWER1",

	// Big Tumbleweed
	"S_BIGTUMBLEWEED",
	"S_BIGTUMBLEWEED_ROLL1",
	"S_BIGTUMBLEWEED_ROLL2",
	"S_BIGTUMBLEWEED_ROLL3",
	"S_BIGTUMBLEWEED_ROLL4",
	"S_BIGTUMBLEWEED_ROLL5",
	"S_BIGTUMBLEWEED_ROLL6",
	"S_BIGTUMBLEWEED_ROLL7",
	"S_BIGTUMBLEWEED_ROLL8",

	// Little Tumbleweed
	"S_LITTLETUMBLEWEED",
	"S_LITTLETUMBLEWEED_ROLL1",
	"S_LITTLETUMBLEWEED_ROLL2",
	"S_LITTLETUMBLEWEED_ROLL3",
	"S_LITTLETUMBLEWEED_ROLL4",
	"S_LITTLETUMBLEWEED_ROLL5",
	"S_LITTLETUMBLEWEED_ROLL6",
	"S_LITTLETUMBLEWEED_ROLL7",
	"S_LITTLETUMBLEWEED_ROLL8",

	// Cacti Sprites
	"S_CACTI1",
	"S_CACTI2",
	"S_CACTI3",
	"S_CACTI4",

	// Flame jet
	"S_FLAMEJETSTND",
	"S_FLAMEJETSTART",
	"S_FLAMEJETSTOP",
	"S_FLAMEJETFLAME1",
	"S_FLAMEJETFLAME2",
	"S_FLAMEJETFLAME3",

	// Spinning flame jets
	"S_FJSPINAXISA1", // Counter-clockwise
	"S_FJSPINAXISA2",
	"S_FJSPINAXISB1", // Clockwise
	"S_FJSPINAXISB2",

	// Blade's flame
	"S_FLAMEJETFLAMEB1",
	"S_FLAMEJETFLAMEB2",
	"S_FLAMEJETFLAMEB3",

	// Trapgoyles
	"S_TRAPGOYLE",
	"S_TRAPGOYLE_CHECK",
	"S_TRAPGOYLE_FIRE1",
	"S_TRAPGOYLE_FIRE2",
	"S_TRAPGOYLE_FIRE3",
	"S_TRAPGOYLEUP",
	"S_TRAPGOYLEUP_CHECK",
	"S_TRAPGOYLEUP_FIRE1",
	"S_TRAPGOYLEUP_FIRE2",
	"S_TRAPGOYLEUP_FIRE3",
	"S_TRAPGOYLEDOWN",
	"S_TRAPGOYLEDOWN_CHECK",
	"S_TRAPGOYLEDOWN_FIRE1",
	"S_TRAPGOYLEDOWN_FIRE2",
	"S_TRAPGOYLEDOWN_FIRE3",
	"S_TRAPGOYLELONG",
	"S_TRAPGOYLELONG_CHECK",
	"S_TRAPGOYLELONG_FIRE1",
	"S_TRAPGOYLELONG_FIRE2",
	"S_TRAPGOYLELONG_FIRE3",
	"S_TRAPGOYLELONG_FIRE4",
	"S_TRAPGOYLELONG_FIRE5",

	// ATZ's Red Crystal/Target
	"S_TARGET_IDLE",
	"S_TARGET_HIT1",
	"S_TARGET_HIT2",
	"S_TARGET_RESPAWN",
	"S_TARGET_ALLDONE",

	// Stalagmites
	"S_STG0",
	"S_STG1",
	"S_STG2",
	"S_STG3",
	"S_STG4",
	"S_STG5",
	"S_STG6",
	"S_STG7",
	"S_STG8",
	"S_STG9",

	// Xmas-specific stuff
	"S_XMASPOLE",
	"S_CANDYCANE",
	"S_SNOWMAN",    // normal
	"S_SNOWMANHAT", // with hat + scarf
	"S_LAMPPOST1",  // normal
	"S_LAMPPOST2",  // with snow
	"S_HANGSTAR",
	// Xmas GFZ bushes
	"S_XMASBERRYBUSH",
	"S_XMASBUSH",

	// Botanic Serenity's loads of scenery states
	"S_BSZTALLFLOWER_RED",
	"S_BSZTALLFLOWER_PURPLE",
	"S_BSZTALLFLOWER_BLUE",
	"S_BSZTALLFLOWER_CYAN",
	"S_BSZTALLFLOWER_YELLOW",
	"S_BSZTALLFLOWER_ORANGE",
	"S_BSZFLOWER_RED",
	"S_BSZFLOWER_PURPLE",
	"S_BSZFLOWER_BLUE",
	"S_BSZFLOWER_CYAN",
	"S_BSZFLOWER_YELLOW",
	"S_BSZFLOWER_ORANGE",
	"S_BSZSHORTFLOWER_RED",
	"S_BSZSHORTFLOWER_PURPLE",
	"S_BSZSHORTFLOWER_BLUE",
	"S_BSZSHORTFLOWER_CYAN",
	"S_BSZSHORTFLOWER_YELLOW",
	"S_BSZSHORTFLOWER_ORANGE",
	"S_BSZTULIP_RED",
	"S_BSZTULIP_PURPLE",
	"S_BSZTULIP_BLUE",
	"S_BSZTULIP_CYAN",
	"S_BSZTULIP_YELLOW",
	"S_BSZTULIP_ORANGE",
	"S_BSZCLUSTER_RED",
	"S_BSZCLUSTER_PURPLE",
	"S_BSZCLUSTER_BLUE",
	"S_BSZCLUSTER_CYAN",
	"S_BSZCLUSTER_YELLOW",
	"S_BSZCLUSTER_ORANGE",
	"S_BSZBUSH_RED",
	"S_BSZBUSH_PURPLE",
	"S_BSZBUSH_BLUE",
	"S_BSZBUSH_CYAN",
	"S_BSZBUSH_YELLOW",
	"S_BSZBUSH_ORANGE",
	"S_BSZVINE_RED",
	"S_BSZVINE_PURPLE",
	"S_BSZVINE_BLUE",
	"S_BSZVINE_CYAN",
	"S_BSZVINE_YELLOW",
	"S_BSZVINE_ORANGE",
	"S_BSZSHRUB",
	"S_BSZCLOVER",
	"S_BIG_PALMTREE_TRUNK",
	"S_BIG_PALMTREE_TOP",
	"S_PALMTREE_TRUNK",
	"S_PALMTREE_TOP",

	"S_DBALL1",
	"S_DBALL2",
	"S_DBALL3",
	"S_DBALL4",
	"S_DBALL5",
	"S_DBALL6",
	"S_EGGSTATUE2",

	// Shield Orb
	"S_ARMA1",
	"S_ARMA2",
	"S_ARMA3",
	"S_ARMA4",
	"S_ARMA5",
	"S_ARMA6",
	"S_ARMA7",
	"S_ARMA8",
	"S_ARMA9",
	"S_ARMA10",
	"S_ARMA11",
	"S_ARMA12",
	"S_ARMA13",
	"S_ARMA14",
	"S_ARMA15",
	"S_ARMA16",

	"S_ARMF1",
	"S_ARMF2",
	"S_ARMF3",
	"S_ARMF4",
	"S_ARMF5",
	"S_ARMF6",
	"S_ARMF7",
	"S_ARMF8",
	"S_ARMF9",
	"S_ARMF10",
	"S_ARMF11",
	"S_ARMF12",
	"S_ARMF13",
	"S_ARMF14",
	"S_ARMF15",
	"S_ARMF16",

	"S_ARMB1",
	"S_ARMB2",
	"S_ARMB3",
	"S_ARMB4",
	"S_ARMB5",
	"S_ARMB6",
	"S_ARMB7",
	"S_ARMB8",
	"S_ARMB9",
	"S_ARMB10",
	"S_ARMB11",
	"S_ARMB12",
	"S_ARMB13",
	"S_ARMB14",
	"S_ARMB15",
	"S_ARMB16",

	"S_WIND1",
	"S_WIND2",
	"S_WIND3",
	"S_WIND4",
	"S_WIND5",
	"S_WIND6",
	"S_WIND7",
	"S_WIND8",

	"S_MAGN1",
	"S_MAGN2",
	"S_MAGN3",
	"S_MAGN4",
	"S_MAGN5",
	"S_MAGN6",
	"S_MAGN7",
	"S_MAGN8",
	"S_MAGN9",
	"S_MAGN10",
	"S_MAGN11",
	"S_MAGN12",
	"S_MAGN13",

	"S_FORC1",
	"S_FORC2",
	"S_FORC3",
	"S_FORC4",
	"S_FORC5",
	"S_FORC6",
	"S_FORC7",
	"S_FORC8",
	"S_FORC9",
	"S_FORC10",

	"S_FORC11",
	"S_FORC12",
	"S_FORC13",
	"S_FORC14",
	"S_FORC15",
	"S_FORC16",
	"S_FORC17",
	"S_FORC18",
	"S_FORC19",
	"S_FORC20",

	"S_FORC21",

	"S_ELEM1",
	"S_ELEM2",
	"S_ELEM3",
	"S_ELEM4",
	"S_ELEM5",
	"S_ELEM6",
	"S_ELEM7",
	"S_ELEM8",
	"S_ELEM9",
	"S_ELEM10",
	"S_ELEM11",
	"S_ELEM12",

	"S_ELEM13",
	"S_ELEM14",

	"S_ELEMF1",
	"S_ELEMF2",
	"S_ELEMF3",
	"S_ELEMF4",
	"S_ELEMF5",
	"S_ELEMF6",
	"S_ELEMF7",
	"S_ELEMF8",
	"S_ELEMF9",
	"S_ELEMF10",

	"S_PITY1",
	"S_PITY2",
	"S_PITY3",
	"S_PITY4",
	"S_PITY5",
	"S_PITY6",

	"S_FIRS1",
	"S_FIRS2",
	"S_FIRS3",
	"S_FIRS4",
	"S_FIRS5",
	"S_FIRS6",
	"S_FIRS7",
	"S_FIRS8",
	"S_FIRS9",

	"S_FIRS10",
	"S_FIRS11",

	"S_FIRSB1",
	"S_FIRSB2",
	"S_FIRSB3",
	"S_FIRSB4",
	"S_FIRSB5",
	"S_FIRSB6",
	"S_FIRSB7",
	"S_FIRSB8",
	"S_FIRSB9",

	"S_FIRSB10",

	"S_BUBS1",
	"S_BUBS2",
	"S_BUBS3",
	"S_BUBS4",
	"S_BUBS5",
	"S_BUBS6",
	"S_BUBS7",
	"S_BUBS8",
	"S_BUBS9",

	"S_BUBS10",
	"S_BUBS11",

	"S_BUBSB1",
	"S_BUBSB2",
	"S_BUBSB3",
	"S_BUBSB4",

	"S_BUBSB5",
	"S_BUBSB6",

	"S_ZAPS1",
	"S_ZAPS2",
	"S_ZAPS3",
	"S_ZAPS4",
	"S_ZAPS5",
	"S_ZAPS6",
	"S_ZAPS7",
	"S_ZAPS8",
	"S_ZAPS9",
	"S_ZAPS10",
	"S_ZAPS11",
	"S_ZAPS12",
	"S_ZAPS13", // blank frame
	"S_ZAPS14",
	"S_ZAPS15",
	"S_ZAPS16",

	"S_ZAPSB1", // blank frame
	"S_ZAPSB2",
	"S_ZAPSB3",
	"S_ZAPSB4",
	"S_ZAPSB5",
	"S_ZAPSB6",
	"S_ZAPSB7",
	"S_ZAPSB8",
	"S_ZAPSB9",
	"S_ZAPSB10",
	"S_ZAPSB11", // blank frame

	// Thunder spark
	"S_THUNDERCOIN_SPARK",

	// Invincibility Sparkles
	"S_IVSP",

	// Super Sonic Spark
	"S_SSPK1",
	"S_SSPK2",
	"S_SSPK3",
	"S_SSPK4",
	"S_SSPK5",

	// Flicky-sized bubble
	"S_FLICKY_BUBBLE",

	// Bluebird
	"S_FLICKY_01_OUT",
	"S_FLICKY_01_FLAP1",
	"S_FLICKY_01_FLAP2",
	"S_FLICKY_01_FLAP3",

	// Rabbit
	"S_FLICKY_02_OUT",
	"S_FLICKY_02_AIM",
	"S_FLICKY_02_HOP",
	"S_FLICKY_02_UP",
	"S_FLICKY_02_DOWN",

	// Chicken
	"S_FLICKY_03_OUT",
	"S_FLICKY_03_AIM",
	"S_FLICKY_03_HOP",
	"S_FLICKY_03_UP",
	"S_FLICKY_03_FLAP1",
	"S_FLICKY_03_FLAP2",

	// Seal
	"S_FLICKY_04_OUT",
	"S_FLICKY_04_AIM",
	"S_FLICKY_04_HOP",
	"S_FLICKY_04_UP",
	"S_FLICKY_04_DOWN",
	"S_FLICKY_04_SWIM1",
	"S_FLICKY_04_SWIM2",
	"S_FLICKY_04_SWIM3",
	"S_FLICKY_04_SWIM4",

	// Pig
	"S_FLICKY_05_OUT",
	"S_FLICKY_05_AIM",
	"S_FLICKY_05_HOP",
	"S_FLICKY_05_UP",
	"S_FLICKY_05_DOWN",

	// Chipmunk
	"S_FLICKY_06_OUT",
	"S_FLICKY_06_AIM",
	"S_FLICKY_06_HOP",
	"S_FLICKY_06_UP",
	"S_FLICKY_06_DOWN",

	// Penguin
	"S_FLICKY_07_OUT",
	"S_FLICKY_07_AIML",
	"S_FLICKY_07_HOPL",
	"S_FLICKY_07_UPL",
	"S_FLICKY_07_DOWNL",
	"S_FLICKY_07_AIMR",
	"S_FLICKY_07_HOPR",
	"S_FLICKY_07_UPR",
	"S_FLICKY_07_DOWNR",
	"S_FLICKY_07_SWIM1",
	"S_FLICKY_07_SWIM2",
	"S_FLICKY_07_SWIM3",

	// Fish
	"S_FLICKY_08_OUT",
	"S_FLICKY_08_AIM",
	"S_FLICKY_08_HOP",
	"S_FLICKY_08_FLAP1",
	"S_FLICKY_08_FLAP2",
	"S_FLICKY_08_FLAP3",
	"S_FLICKY_08_FLAP4",
	"S_FLICKY_08_SWIM1",
	"S_FLICKY_08_SWIM2",
	"S_FLICKY_08_SWIM3",
	"S_FLICKY_08_SWIM4",

	// Ram
	"S_FLICKY_09_OUT",
	"S_FLICKY_09_AIM",
	"S_FLICKY_09_HOP",
	"S_FLICKY_09_UP",
	"S_FLICKY_09_DOWN",

	// Puffin
	"S_FLICKY_10_OUT",
	"S_FLICKY_10_FLAP1",
	"S_FLICKY_10_FLAP2",

	// Cow
	"S_FLICKY_11_OUT",
	"S_FLICKY_11_AIM",
	"S_FLICKY_11_RUN1",
	"S_FLICKY_11_RUN2",
	"S_FLICKY_11_RUN3",

	// Rat
	"S_FLICKY_12_OUT",
	"S_FLICKY_12_AIM",
	"S_FLICKY_12_RUN1",
	"S_FLICKY_12_RUN2",
	"S_FLICKY_12_RUN3",

	// Bear
	"S_FLICKY_13_OUT",
	"S_FLICKY_13_AIM",
	"S_FLICKY_13_HOP",
	"S_FLICKY_13_UP",
	"S_FLICKY_13_DOWN",

	// Dove
	"S_FLICKY_14_OUT",
	"S_FLICKY_14_FLAP1",
	"S_FLICKY_14_FLAP2",
	"S_FLICKY_14_FLAP3",

	// Cat
	"S_FLICKY_15_OUT",
	"S_FLICKY_15_AIM",
	"S_FLICKY_15_HOP",
	"S_FLICKY_15_UP",
	"S_FLICKY_15_DOWN",

	// Canary
	"S_FLICKY_16_OUT",
	"S_FLICKY_16_FLAP1",
	"S_FLICKY_16_FLAP2",
	"S_FLICKY_16_FLAP3",

	"S_YELLOWSPRING",
	"S_YELLOWSPRING2",
	"S_YELLOWSPRING3",
	"S_YELLOWSPRING4",
	"S_YELLOWSPRING5",

	"S_REDSPRING",
	"S_REDSPRING2",
	"S_REDSPRING3",
	"S_REDSPRING4",
	"S_REDSPRING5",

	// Blue Springs
	"S_BLUESPRING",
	"S_BLUESPRING2",
	"S_BLUESPRING3",
	"S_BLUESPRING4",
	"S_BLUESPRING5",

	// Yellow Diagonal Spring
	"S_YDIAG1",
	"S_YDIAG2",
	"S_YDIAG3",
	"S_YDIAG4",
	"S_YDIAG5",
	"S_YDIAG6",
	"S_YDIAG7",
	"S_YDIAG8",

	// Red Diagonal Spring
	"S_RDIAG1",
	"S_RDIAG2",
	"S_RDIAG3",
	"S_RDIAG4",
	"S_RDIAG5",
	"S_RDIAG6",
	"S_RDIAG7",
	"S_RDIAG8",

	// Yellow Side Spring
	"S_YHORIZ1",
	"S_YHORIZ2",
	"S_YHORIZ3",
	"S_YHORIZ4",
	"S_YHORIZ5",
	"S_YHORIZ6",
	"S_YHORIZ7",
	"S_YHORIZ8",

	// Red Side Spring
	"S_RHORIZ1",
	"S_RHORIZ2",
	"S_RHORIZ3",
	"S_RHORIZ4",
	"S_RHORIZ5",
	"S_RHORIZ6",
	"S_RHORIZ7",
	"S_RHORIZ8",

	// Blue Side Spring
	"S_BHORIZ1",
	"S_BHORIZ2",
	"S_BHORIZ3",
	"S_BHORIZ4",
	"S_BHORIZ5",
	"S_BHORIZ6",
	"S_BHORIZ7",
	"S_BHORIZ8",

	// Rain
	"S_RAIN1",
	"S_RAINRETURN",

	// Snowflake
	"S_SNOW1",
	"S_SNOW2",
	"S_SNOW3",

	// Water Splish
	"S_SPLISH1",
	"S_SPLISH2",
	"S_SPLISH3",
	"S_SPLISH4",
	"S_SPLISH5",
	"S_SPLISH6",
	"S_SPLISH7",
	"S_SPLISH8",
	"S_SPLISH9",

	// added water splash
	"S_SPLASH1",
	"S_SPLASH2",
	"S_SPLASH3",

	// lava/slime damage burn smoke
	"S_SMOKE1",
	"S_SMOKE2",
	"S_SMOKE3",
	"S_SMOKE4",
	"S_SMOKE5",

	// Bubbles
	"S_SMALLBUBBLE",
	"S_MEDIUMBUBBLE",
	"S_LARGEBUBBLE1",
	"S_LARGEBUBBLE2",
	"S_EXTRALARGEBUBBLE", // breathable

	"S_POP1", // Extra Large bubble goes POP!

	"S_WATERZAP",

	// Spindash dust
	"S_SPINDUST1",
	"S_SPINDUST2",
	"S_SPINDUST3",
	"S_SPINDUST4",
	"S_SPINDUST_BUBBLE1",
	"S_SPINDUST_BUBBLE2",
	"S_SPINDUST_BUBBLE3",
	"S_SPINDUST_BUBBLE4",
	"S_SPINDUST_FIRE1",
	"S_SPINDUST_FIRE2",
	"S_SPINDUST_FIRE3",
	"S_SPINDUST_FIRE4",

	"S_FOG1",
	"S_FOG2",
	"S_FOG3",
	"S_FOG4",
	"S_FOG5",
	"S_FOG6",
	"S_FOG7",
	"S_FOG8",
	"S_FOG9",
	"S_FOG10",
	"S_FOG11",
	"S_FOG12",
	"S_FOG13",
	"S_FOG14",

	"S_SEED",

	"S_PARTICLE",
	"S_PARTICLEGEN",

	// Score Logos
	"S_SCRA", // 100
	"S_SCRB", // 200
	"S_SCRC", // 500
	"S_SCRD", // 1000
	"S_SCRE", // 10000
	"S_SCRF", // 400 (mario)
	"S_SCRG", // 800 (mario)
	"S_SCRH", // 2000 (mario)
	"S_SCRI", // 4000 (mario)
	"S_SCRJ", // 8000 (mario)
	"S_SCRK", // 1UP (mario)

	// Drowning Timer Numbers
	"S_ZERO1",
	"S_ONE1",
	"S_TWO1",
	"S_THREE1",
	"S_FOUR1",
	"S_FIVE1",

	"S_ZERO2",
	"S_ONE2",
	"S_TWO2",
	"S_THREE2",
	"S_FOUR2",
	"S_FIVE2",

	"S_LOCKON1",
	"S_LOCKON2",

	// Tag Sign
	"S_TTAG",

	// Got Flag Sign
	"S_GOTFLAG",
	"S_GOTREDFLAG",
	"S_GOTBLUEFLAG",

	"S_CORK",

	// Red Ring
	"S_RRNG1",
	"S_RRNG2",
	"S_RRNG3",
	"S_RRNG4",
	"S_RRNG5",
	"S_RRNG6",
	"S_RRNG7",

	// Weapon Ring Ammo
	"S_BOUNCERINGAMMO",
	"S_RAILRINGAMMO",
	"S_INFINITYRINGAMMO",
	"S_AUTOMATICRINGAMMO",
	"S_EXPLOSIONRINGAMMO",
	"S_SCATTERRINGAMMO",
	"S_GRENADERINGAMMO",

	// Weapon pickup
	"S_BOUNCEPICKUP",
	"S_BOUNCEPICKUPFADE1",
	"S_BOUNCEPICKUPFADE2",
	"S_BOUNCEPICKUPFADE3",
	"S_BOUNCEPICKUPFADE4",
	"S_BOUNCEPICKUPFADE5",
	"S_BOUNCEPICKUPFADE6",
	"S_BOUNCEPICKUPFADE7",
	"S_BOUNCEPICKUPFADE8",

	"S_RAILPICKUP",
	"S_RAILPICKUPFADE1",
	"S_RAILPICKUPFADE2",
	"S_RAILPICKUPFADE3",
	"S_RAILPICKUPFADE4",
	"S_RAILPICKUPFADE5",
	"S_RAILPICKUPFADE6",
	"S_RAILPICKUPFADE7",
	"S_RAILPICKUPFADE8",

	"S_AUTOPICKUP",
	"S_AUTOPICKUPFADE1",
	"S_AUTOPICKUPFADE2",
	"S_AUTOPICKUPFADE3",
	"S_AUTOPICKUPFADE4",
	"S_AUTOPICKUPFADE5",
	"S_AUTOPICKUPFADE6",
	"S_AUTOPICKUPFADE7",
	"S_AUTOPICKUPFADE8",

	"S_EXPLODEPICKUP",
	"S_EXPLODEPICKUPFADE1",
	"S_EXPLODEPICKUPFADE2",
	"S_EXPLODEPICKUPFADE3",
	"S_EXPLODEPICKUPFADE4",
	"S_EXPLODEPICKUPFADE5",
	"S_EXPLODEPICKUPFADE6",
	"S_EXPLODEPICKUPFADE7",
	"S_EXPLODEPICKUPFADE8",

	"S_SCATTERPICKUP",
	"S_SCATTERPICKUPFADE1",
	"S_SCATTERPICKUPFADE2",
	"S_SCATTERPICKUPFADE3",
	"S_SCATTERPICKUPFADE4",
	"S_SCATTERPICKUPFADE5",
	"S_SCATTERPICKUPFADE6",
	"S_SCATTERPICKUPFADE7",
	"S_SCATTERPICKUPFADE8",

	"S_GRENADEPICKUP",
	"S_GRENADEPICKUPFADE1",
	"S_GRENADEPICKUPFADE2",
	"S_GRENADEPICKUPFADE3",
	"S_GRENADEPICKUPFADE4",
	"S_GRENADEPICKUPFADE5",
	"S_GRENADEPICKUPFADE6",
	"S_GRENADEPICKUPFADE7",
	"S_GRENADEPICKUPFADE8",

	// Thrown Weapon Rings
	"S_THROWNBOUNCE1",
	"S_THROWNBOUNCE2",
	"S_THROWNBOUNCE3",
	"S_THROWNBOUNCE4",
	"S_THROWNBOUNCE5",
	"S_THROWNBOUNCE6",
	"S_THROWNBOUNCE7",
	"S_THROWNINFINITY1",
	"S_THROWNINFINITY2",
	"S_THROWNINFINITY3",
	"S_THROWNINFINITY4",
	"S_THROWNINFINITY5",
	"S_THROWNINFINITY6",
	"S_THROWNINFINITY7",
	"S_THROWNAUTOMATIC1",
	"S_THROWNAUTOMATIC2",
	"S_THROWNAUTOMATIC3",
	"S_THROWNAUTOMATIC4",
	"S_THROWNAUTOMATIC5",
	"S_THROWNAUTOMATIC6",
	"S_THROWNAUTOMATIC7",
	"S_THROWNEXPLOSION1",
	"S_THROWNEXPLOSION2",
	"S_THROWNEXPLOSION3",
	"S_THROWNEXPLOSION4",
	"S_THROWNEXPLOSION5",
	"S_THROWNEXPLOSION6",
	"S_THROWNEXPLOSION7",
	"S_THROWNGRENADE1",
	"S_THROWNGRENADE2",
	"S_THROWNGRENADE3",
	"S_THROWNGRENADE4",
	"S_THROWNGRENADE5",
	"S_THROWNGRENADE6",
	"S_THROWNGRENADE7",
	"S_THROWNGRENADE8",
	"S_THROWNGRENADE9",
	"S_THROWNGRENADE10",
	"S_THROWNGRENADE11",
	"S_THROWNGRENADE12",
	"S_THROWNGRENADE13",
	"S_THROWNGRENADE14",
	"S_THROWNGRENADE15",
	"S_THROWNGRENADE16",
	"S_THROWNGRENADE17",
	"S_THROWNGRENADE18",
	"S_THROWNSCATTER",

	"S_RINGEXPLODE",

	"S_COIN1",
	"S_COIN2",
	"S_COIN3",
	"S_COINSPARKLE1",
	"S_COINSPARKLE2",
	"S_COINSPARKLE3",
	"S_COINSPARKLE4",
	"S_GOOMBA1",
	"S_GOOMBA1B",
	"S_GOOMBA2",
	"S_GOOMBA3",
	"S_GOOMBA4",
	"S_GOOMBA5",
	"S_GOOMBA6",
	"S_GOOMBA7",
	"S_GOOMBA8",
	"S_GOOMBA9",
	"S_GOOMBA_DEAD",
	"S_BLUEGOOMBA1",
	"S_BLUEGOOMBA1B",
	"S_BLUEGOOMBA2",
	"S_BLUEGOOMBA3",
	"S_BLUEGOOMBA4",
	"S_BLUEGOOMBA5",
	"S_BLUEGOOMBA6",
	"S_BLUEGOOMBA7",
	"S_BLUEGOOMBA8",
	"S_BLUEGOOMBA9",
	"S_BLUEGOOMBA_DEAD",

	// Mario-specific stuff
	"S_FIREFLOWER1",
	"S_FIREFLOWER2",
	"S_FIREFLOWER3",
	"S_FIREFLOWER4",
	"S_FIREBALL1",
	"S_FIREBALL2",
	"S_FIREBALL3",
	"S_FIREBALL4",
	"S_FIREBALLEXP1",
	"S_FIREBALLEXP2",
	"S_FIREBALLEXP3",
	"S_SHELL",
	"S_PUMA_START1",
	"S_PUMA_START2",
	"S_PUMA_UP1",
	"S_PUMA_UP2",
	"S_PUMA_UP3",
	"S_PUMA_DOWN1",
	"S_PUMA_DOWN2",
	"S_PUMA_DOWN3",
	"S_PUMATRAIL1",
	"S_PUMATRAIL2",
	"S_PUMATRAIL3",
	"S_PUMATRAIL4",
	"S_HAMMER",
	"S_KOOPA1",
	"S_KOOPA2",
	"S_KOOPAFLAME1",
	"S_KOOPAFLAME2",
	"S_KOOPAFLAME3",
	"S_AXE1",
	"S_AXE2",
	"S_AXE3",
	"S_MARIOBUSH1",
	"S_MARIOBUSH2",
	"S_TOAD",

	// Nights-specific stuff
	"S_NIGHTSDRONE1",
	"S_NIGHTSDRONE2",
	"S_NIGHTSDRONE_SPARKLING1",
	"S_NIGHTSDRONE_SPARKLING2",
	"S_NIGHTSDRONE_SPARKLING3",
	"S_NIGHTSDRONE_SPARKLING4",
	"S_NIGHTSDRONE_SPARKLING5",
	"S_NIGHTSDRONE_SPARKLING6",
	"S_NIGHTSDRONE_SPARKLING7",
	"S_NIGHTSDRONE_SPARKLING8",
	"S_NIGHTSDRONE_SPARKLING9",
	"S_NIGHTSDRONE_SPARKLING10",
	"S_NIGHTSDRONE_SPARKLING11",
	"S_NIGHTSDRONE_SPARKLING12",
	"S_NIGHTSDRONE_SPARKLING13",
	"S_NIGHTSDRONE_SPARKLING14",
	"S_NIGHTSDRONE_SPARKLING15",
	"S_NIGHTSDRONE_SPARKLING16",
	"S_NIGHTSGOAL1",
	"S_NIGHTSGOAL2",
	"S_NIGHTSGOAL3",
	"S_NIGHTSGOAL4",

	"S_NIGHTSPARKLE1",
	"S_NIGHTSPARKLE2",
	"S_NIGHTSPARKLE3",
	"S_NIGHTSPARKLE4",
	"S_NIGHTSPARKLESUPER1",
	"S_NIGHTSPARKLESUPER2",
	"S_NIGHTSPARKLESUPER3",
	"S_NIGHTSPARKLESUPER4",
	"S_NIGHTSLOOPHELPER",

	// NiGHTS bumper
	"S_NIGHTSBUMPER1",
	"S_NIGHTSBUMPER2",
	"S_NIGHTSBUMPER3",
	"S_NIGHTSBUMPER4",
	"S_NIGHTSBUMPER5",
	"S_NIGHTSBUMPER6",
	"S_NIGHTSBUMPER7",
	"S_NIGHTSBUMPER8",
	"S_NIGHTSBUMPER9",
	"S_NIGHTSBUMPER10",
	"S_NIGHTSBUMPER11",
	"S_NIGHTSBUMPER12",

	"S_HOOP",
	"S_HOOP_XMASA",
	"S_HOOP_XMASB",

	"S_NIGHTSCORE10",
	"S_NIGHTSCORE20",
	"S_NIGHTSCORE30",
	"S_NIGHTSCORE40",
	"S_NIGHTSCORE50",
	"S_NIGHTSCORE60",
	"S_NIGHTSCORE70",
	"S_NIGHTSCORE80",
	"S_NIGHTSCORE90",
	"S_NIGHTSCORE100",
	"S_NIGHTSCORE10_2",
	"S_NIGHTSCORE20_2",
	"S_NIGHTSCORE30_2",
	"S_NIGHTSCORE40_2",
	"S_NIGHTSCORE50_2",
	"S_NIGHTSCORE60_2",
	"S_NIGHTSCORE70_2",
	"S_NIGHTSCORE80_2",
	"S_NIGHTSCORE90_2",
	"S_NIGHTSCORE100_2",

	"S_NIGHTSWING",
	"S_NIGHTSWING_XMAS",

	// NiGHTS Paraloop Powerups
	"S_NIGHTSSUPERLOOP",
	"S_NIGHTSDRILLREFILL",
	"S_NIGHTSHELPER",
	"S_NIGHTSEXTRATIME",
	"S_NIGHTSLINKFREEZE",
	"S_EGGCAPSULE",

	// Orbiting Chaos Emeralds
	"S_ORBITEM1",
	"S_ORBITEM2",
	"S_ORBITEM3",
	"S_ORBITEM4",
	"S_ORBITEM5",
	"S_ORBITEM6",
	"S_ORBITEM7",
	"S_ORBITEM8",
	"S_ORBITEM9",
	"S_ORBITEM10",
	"S_ORBITEM11",
	"S_ORBITEM12",
	"S_ORBITEM13",
	"S_ORBITEM14",
	"S_ORBITEM15",
	"S_ORBITEM16",

	// "Flicky" helper
	"S_NIGHTOPIANHELPER1",
	"S_NIGHTOPIANHELPER2",
	"S_NIGHTOPIANHELPER3",
	"S_NIGHTOPIANHELPER4",
	"S_NIGHTOPIANHELPER5",
	"S_NIGHTOPIANHELPER6",
	"S_NIGHTOPIANHELPER7",
	"S_NIGHTOPIANHELPER8",
	"S_NIGHTOPIANHELPER9",

	"S_CRUMBLE1",
	"S_CRUMBLE2",

	// Spark
	"S_SPRK1",
	"S_SPRK2",
	"S_SPRK3",
	"S_SPRK4",
	"S_SPRK5",
	"S_SPRK6",
	"S_SPRK7",
	"S_SPRK8",
	"S_SPRK9",
	"S_SPRK10",
	"S_SPRK11",
	"S_SPRK12",
	"S_SPRK13",
	"S_SPRK14",
	"S_SPRK15",
	"S_SPRK16",

	// Robot Explosion
	"S_XPLD_FLICKY",
	"S_XPLD1",
	"S_XPLD2",
	"S_XPLD_EGGTRAP",

	// Underwater Explosion
	"S_WPLD1",
	"S_WPLD2",
	"S_WPLD3",
	"S_WPLD4",
	"S_WPLD5",
	"S_WPLD6",

	"S_ROCKSPAWN",

	"S_ROCKCRUMBLEA",
	"S_ROCKCRUMBLEB",
	"S_ROCKCRUMBLEC",
	"S_ROCKCRUMBLED",
	"S_ROCKCRUMBLEE",
	"S_ROCKCRUMBLEF",
	"S_ROCKCRUMBLEG",
	"S_ROCKCRUMBLEH",
	"S_ROCKCRUMBLEI",
	"S_ROCKCRUMBLEJ",
	"S_ROCKCRUMBLEK",
	"S_ROCKCRUMBLEL",
	"S_ROCKCRUMBLEM",
	"S_ROCKCRUMBLEN",
	"S_ROCKCRUMBLEO",
	"S_ROCKCRUMBLEP",

#ifdef SEENAMES
	"S_NAMECHECK",
#endif
};

// RegEx to generate this from info.h: ^\tMT_([^,]+), --> \t"MT_\1",
// I am leaving the prefixes solely for clarity to programmers,
// because sadly no one remembers this place while searching for full state names.
static const char *const MOBJTYPE_LIST[] = {  // array length left dynamic for sanity testing later.
	"MT_NULL",
	"MT_UNKNOWN",

	"MT_THOK", // Thok! mobj
	"MT_PLAYER",

	// Enemies
	"MT_BLUECRAWLA",
	"MT_REDCRAWLA",
	"MT_GFZFISH", // Greenflower Fish
	"MT_GOLDBUZZ",
	"MT_REDBUZZ",
	"MT_AQUABUZZ",
	"MT_JETTBOMBER", // Jetty-Syn Bomber
	"MT_JETTGUNNER", // Jetty-Syn Gunner
	"MT_CRAWLACOMMANDER", // Crawla Commander
	"MT_DETON", // Deton
	"MT_SKIM", // Skim mine dropper
	"MT_TURRET",
	"MT_POPUPTURRET",
	"MT_SHARP", // Sharp
	"MT_JETJAW", // Jet Jaw
	"MT_SNAILER", // Snailer
	"MT_VULTURE", // Vulture
	"MT_POINTY", // Pointy
	"MT_POINTYBALL", // Pointy Ball
	"MT_ROBOHOOD", // Robo-Hood
	"MT_FACESTABBER", // CastleBot FaceStabber
	"MT_EGGGUARD", // Egg Guard
	"MT_EGGSHIELD", // Egg Shield for Egg Guard
	"MT_GSNAPPER", // Green Snapper
	"MT_MINUS", // Minus
	"MT_SPRINGSHELL", // Spring Shell (no drop)
	"MT_YELLOWSHELL", // Spring Shell (yellow)
	"MT_UNIDUS", // Unidus
	"MT_UNIBALL", // Unidus Ball

	// Generic Boss Items
	"MT_BOSSEXPLODE",
	"MT_SONIC3KBOSSEXPLODE",
	"MT_BOSSFLYPOINT",
	"MT_EGGTRAP",
	"MT_BOSS3WAYPOINT",
	"MT_BOSS9GATHERPOINT",

	// Boss 1
	"MT_EGGMOBILE",
	"MT_JETFUME1",
	"MT_EGGMOBILE_BALL",
	"MT_EGGMOBILE_TARGET",
	"MT_EGGMOBILE_FIRE",

	// Boss 2
	"MT_EGGMOBILE2",
	"MT_EGGMOBILE2_POGO",
	"MT_BOSSTANK1",
	"MT_BOSSTANK2",
	"MT_BOSSSPIGOT",
	"MT_GOOP",
	"MT_GOOPTRAIL",

	// Boss 3
	"MT_EGGMOBILE3",
	"MT_PROPELLER",
	"MT_FAKEMOBILE",

	// Boss 4
	"MT_EGGMOBILE4",
	"MT_EGGMOBILE4_MACE",
	"MT_JETFLAME",

	// Black Eggman (Boss 7)
	"MT_BLACKEGGMAN",
	"MT_BLACKEGGMAN_HELPER",
	"MT_BLACKEGGMAN_GOOPFIRE",
	"MT_BLACKEGGMAN_MISSILE",

	// New Very-Last-Minute 2.1 Brak Eggman (Cy-Brak-demon)
	"MT_CYBRAKDEMON",
	"MT_CYBRAKDEMON_ELECTRIC_BARRIER",
	"MT_CYBRAKDEMON_MISSILE",
	"MT_CYBRAKDEMON_FLAMESHOT",
	"MT_CYBRAKDEMON_FLAMEREST",
	"MT_CYBRAKDEMON_TARGET_RETICULE",
	"MT_CYBRAKDEMON_TARGET_DOT",
	"MT_CYBRAKDEMON_NAPALM_BOMB_LARGE",
	"MT_CYBRAKDEMON_NAPALM_BOMB_SMALL",
	"MT_CYBRAKDEMON_NAPALM_FLAMES",
	"MT_CYBRAKDEMON_VILE_EXPLOSION",

	// Metal Sonic
	"MT_METALSONIC_RACE",
	"MT_METALSONIC_BATTLE",
	"MT_MSSHIELD_FRONT",
	"MT_MSGATHER",

	// Collectible Items
	"MT_RING",
	"MT_FLINGRING", // Lost ring
	"MT_BLUEBALL",  // Blue sphere replacement for special stages
	"MT_REDTEAMRING",  //Rings collectable by red team.
	"MT_BLUETEAMRING", //Rings collectable by blue team.
	"MT_TOKEN", // Special Stage Token
	"MT_REDFLAG", // Red CTF Flag
	"MT_BLUEFLAG", // Blue CTF Flag
	"MT_EMBLEM",
	"MT_EMERALD1",
	"MT_EMERALD2",
	"MT_EMERALD3",
	"MT_EMERALD4",
	"MT_EMERALD5",
	"MT_EMERALD6",
	"MT_EMERALD7",
	"MT_EMERHUNT", // Emerald Hunt
	"MT_EMERALDSPAWN", // Emerald spawner w/ delay
	"MT_FLINGEMERALD", // Lost emerald

	// Springs and others
	"MT_FAN",
	"MT_STEAM", // Steam riser
	"MT_BLUESPRING",
	"MT_YELLOWSPRING",
	"MT_REDSPRING",
	"MT_YELLOWDIAG", // Yellow Diagonal Spring
	"MT_REDDIAG", // Red Diagonal Spring
	"MT_YELLOWHORIZ", // Yellow Side Spring
	"MT_REDHORIZ", // Red Side Spring
	"MT_BLUEHORIZ", // Blue Side Spring

	// Interactive Objects
	"MT_BUBBLES", // Bubble source
	"MT_SIGN", // Level end sign
	"MT_SPIKEBALL", // Spike Ball
	"MT_SPECIALSPIKEBALL",
	"MT_SPINFIRE",
	"MT_SPIKE",
	"MT_WALLSPIKE",
	"MT_WALLSPIKEBASE",
	"MT_STARPOST",
	"MT_BIGMINE",
	"MT_BIGAIRMINE",
	"MT_CANNONLAUNCHER",

	// Monitor miscellany
	"MT_BOXSPARKLE",

	// Monitor boxes -- regular
	"MT_RING_BOX",
	"MT_PITY_BOX",
	"MT_ATTRACT_BOX",
	"MT_FORCE_BOX",
	"MT_ARMAGEDDON_BOX",
	"MT_WHIRLWIND_BOX",
	"MT_ELEMENTAL_BOX",
	"MT_SNEAKERS_BOX",
	"MT_INVULN_BOX",
	"MT_1UP_BOX",
	"MT_EGGMAN_BOX",
	"MT_MIXUP_BOX",
	"MT_MYSTERY_BOX",
	"MT_GRAVITY_BOX",
	"MT_RECYCLER_BOX",
	"MT_SCORE1K_BOX",
	"MT_SCORE10K_BOX",
	"MT_FLAMEAURA_BOX",
	"MT_BUBBLEWRAP_BOX",
	"MT_THUNDERCOIN_BOX",

	// Monitor boxes -- repeating (big) boxes
	"MT_PITY_GOLDBOX",
	"MT_ATTRACT_GOLDBOX",
	"MT_FORCE_GOLDBOX",
	"MT_ARMAGEDDON_GOLDBOX",
	"MT_WHIRLWIND_GOLDBOX",
	"MT_ELEMENTAL_GOLDBOX",
	"MT_SNEAKERS_GOLDBOX",
	"MT_INVULN_GOLDBOX",
	"MT_EGGMAN_GOLDBOX",
	"MT_GRAVITY_GOLDBOX",
	"MT_FLAMEAURA_GOLDBOX",
	"MT_BUBBLEWRAP_GOLDBOX",
	"MT_THUNDERCOIN_GOLDBOX",

	// Monitor boxes -- special
	"MT_RING_REDBOX",
	"MT_RING_BLUEBOX",

	// Monitor icons
	"MT_RING_ICON",
	"MT_PITY_ICON",
	"MT_ATTRACT_ICON",
	"MT_FORCE_ICON",
	"MT_ARMAGEDDON_ICON",
	"MT_WHIRLWIND_ICON",
	"MT_ELEMENTAL_ICON",
	"MT_SNEAKERS_ICON",
	"MT_INVULN_ICON",
	"MT_1UP_ICON",
	"MT_EGGMAN_ICON",
	"MT_MIXUP_ICON",
	"MT_GRAVITY_ICON",
	"MT_RECYCLER_ICON",
	"MT_SCORE1K_ICON",
	"MT_SCORE10K_ICON",
	"MT_FLAMEAURA_ICON",
	"MT_BUBBLEWRAP_ICON",
	"MT_THUNDERCOIN_ICON",

	// Projectiles
	"MT_ROCKET",
	"MT_LASER",
	"MT_TORPEDO",
	"MT_TORPEDO2", // silent
	"MT_ENERGYBALL",
	"MT_MINE", // Skim/Jetty-Syn mine
	"MT_JETTBULLET", // Jetty-Syn Bullet
	"MT_TURRETLASER",
	"MT_CANNONBALL", // Cannonball
	"MT_CANNONBALLDECOR", // Decorative/still cannonball
	"MT_ARROW", // Arrow
	"MT_DEMONFIRE", // Trapgoyle fire

	// Greenflower Scenery
	"MT_GFZFLOWER1",
	"MT_GFZFLOWER2",
	"MT_GFZFLOWER3",
	"MT_BERRYBUSH",
	"MT_BUSH",
	// Trees (both GFZ and misc)
	"MT_GFZTREE",
	"MT_GFZBERRYTREE",
	"MT_GFZCHERRYTREE",
	"MT_CHECKERTREE",
	"MT_CHECKERSUNSETTREE",
	"MT_FHZTREE", // Frozen Hillside
	"MT_FHZPINKTREE",
	"MT_POLYGONTREE",
	"MT_BUSHTREE",
	"MT_BUSHREDTREE",

	// Techno Hill Scenery
	"MT_THZFLOWER1",
	"MT_THZFLOWER2",
	"MT_ALARM",

	// Deep Sea Scenery
	"MT_GARGOYLE", // Deep Sea Gargoyle
	"MT_BIGGARGOYLE", // Deep Sea Gargoyle (Big)
	"MT_SEAWEED", // DSZ Seaweed
	"MT_WATERDRIP", // Dripping Water source
	"MT_WATERDROP", // Water drop from dripping water
	"MT_CORAL1", // Coral 1
	"MT_CORAL2", // Coral 2
	"MT_CORAL3", // Coral 3
	"MT_BLUECRYSTAL", // Blue Crystal

	// Castle Eggman Scenery
	"MT_CHAIN", // CEZ Chain
	"MT_FLAME", // Flame (has corona)
	"MT_FLAMEPARTICLE",
	"MT_EGGSTATUE", // Eggman Statue
	"MT_MACEPOINT", // Mace rotation point
	"MT_CHAINMACEPOINT", // Combination of chains and maces point
	"MT_SPRINGBALLPOINT", // Spring ball point
	"MT_CHAINPOINT", // Mace chain
	"MT_HIDDEN_SLING", // Spin mace chain (activatable)
	"MT_FIREBARPOINT", // Firebar
	"MT_CUSTOMMACEPOINT", // Custom mace
	"MT_SMALLMACECHAIN", // Small Mace Chain
	"MT_BIGMACECHAIN", // Big Mace Chain
	"MT_SMALLMACE", // Small Mace
	"MT_BIGMACE", // Big Mace
	"MT_YELLOWSPRINGBALL", // Yellow spring on a ball
	"MT_REDSPRINGBALL", // Red spring on a ball
	"MT_SMALLFIREBAR", // Small Firebar
	"MT_BIGFIREBAR", // Big Firebar
	"MT_CEZFLOWER",

	// Arid Canyon Scenery
	"MT_BIGTUMBLEWEED",
	"MT_LITTLETUMBLEWEED",
	"MT_CACTI1",
	"MT_CACTI2",
	"MT_CACTI3",
	"MT_CACTI4",

	// Red Volcano Scenery
	"MT_FLAMEJET",
	"MT_VERTICALFLAMEJET",
	"MT_FLAMEJETFLAME",

	"MT_FJSPINAXISA", // Counter-clockwise
	"MT_FJSPINAXISB", // Clockwise

	"MT_FLAMEJETFLAMEB", // Blade's flame

	// Dark City Scenery

	// Egg Rock Scenery

	// Azure Temple Scenery
	"MT_TRAPGOYLE",
	"MT_TRAPGOYLEUP",
	"MT_TRAPGOYLEDOWN",
	"MT_TRAPGOYLELONG",
	"MT_TARGET",

	// Stalagmites
	"MT_STALAGMITE0",
	"MT_STALAGMITE1",
	"MT_STALAGMITE2",
	"MT_STALAGMITE3",
	"MT_STALAGMITE4",
	"MT_STALAGMITE5",
	"MT_STALAGMITE6",
	"MT_STALAGMITE7",
	"MT_STALAGMITE8",
	"MT_STALAGMITE9",

	// Christmas Scenery
	"MT_XMASPOLE",
	"MT_CANDYCANE",
	"MT_SNOWMAN",    // normal
	"MT_SNOWMANHAT", // with hat + scarf
	"MT_LAMPPOST1",  // normal
	"MT_LAMPPOST2",  // with snow
	"MT_HANGSTAR",
	// Xmas GFZ bushes
	"MT_XMASBERRYBUSH",
	"MT_XMASBUSH",

	// Botanic Serenity
	"MT_BSZTALLFLOWER_RED",
	"MT_BSZTALLFLOWER_PURPLE",
	"MT_BSZTALLFLOWER_BLUE",
	"MT_BSZTALLFLOWER_CYAN",
	"MT_BSZTALLFLOWER_YELLOW",
	"MT_BSZTALLFLOWER_ORANGE",
	"MT_BSZFLOWER_RED",
	"MT_BSZFLOWER_PURPLE",
	"MT_BSZFLOWER_BLUE",
	"MT_BSZFLOWER_CYAN",
	"MT_BSZFLOWER_YELLOW",
	"MT_BSZFLOWER_ORANGE",
	"MT_BSZSHORTFLOWER_RED",
	"MT_BSZSHORTFLOWER_PURPLE",
	"MT_BSZSHORTFLOWER_BLUE",
	"MT_BSZSHORTFLOWER_CYAN",
	"MT_BSZSHORTFLOWER_YELLOW",
	"MT_BSZSHORTFLOWER_ORANGE",
	"MT_BSZTULIP_RED",
	"MT_BSZTULIP_PURPLE",
	"MT_BSZTULIP_BLUE",
	"MT_BSZTULIP_CYAN",
	"MT_BSZTULIP_YELLOW",
	"MT_BSZTULIP_ORANGE",
	"MT_BSZCLUSTER_RED",
	"MT_BSZCLUSTER_PURPLE",
	"MT_BSZCLUSTER_BLUE",
	"MT_BSZCLUSTER_CYAN",
	"MT_BSZCLUSTER_YELLOW",
	"MT_BSZCLUSTER_ORANGE",
	"MT_BSZBUSH_RED",
	"MT_BSZBUSH_PURPLE",
	"MT_BSZBUSH_BLUE",
	"MT_BSZBUSH_CYAN",
	"MT_BSZBUSH_YELLOW",
	"MT_BSZBUSH_ORANGE",
	"MT_BSZVINE_RED",
	"MT_BSZVINE_PURPLE",
	"MT_BSZVINE_BLUE",
	"MT_BSZVINE_CYAN",
	"MT_BSZVINE_YELLOW",
	"MT_BSZVINE_ORANGE",
	"MT_BSZSHRUB",
	"MT_BSZCLOVER",
	"MT_BIG_PALMTREE_TRUNK",
	"MT_BIG_PALMTREE_TOP",
	"MT_PALMTREE_TRUNK",
	"MT_PALMTREE_TOP",

	// Misc scenery
	"MT_DBALL",
	"MT_EGGSTATUE2",

	// Powerup Indicators
	"MT_ELEMENTAL_ORB", // Elemental shield mobj
	"MT_ATTRACT_ORB", // Attract shield mobj
	"MT_FORCE_ORB", // Force shield mobj
	"MT_ARMAGEDDON_ORB", // Armageddon shield mobj
	"MT_WHIRLWIND_ORB", // Whirlwind shield mobj
	"MT_PITY_ORB", // Pity shield mobj
	"MT_FLAMEAURA_ORB", // Flame shield mobj
	"MT_BUBBLEWRAP_ORB", // Bubble shield mobj
	"MT_THUNDERCOIN_ORB", // Thunder shield mobj
	"MT_THUNDERCOIN_SPARK", // Thunder spark
	"MT_IVSP", // Invincibility sparkles
	"MT_SUPERSPARK", // Super Sonic Spark

	// Flickies
	"MT_FLICKY_01", // Bluebird
	"MT_FLICKY_02", // Rabbit
	"MT_FLICKY_03", // Chicken
	"MT_FLICKY_04", // Seal
	"MT_FLICKY_05", // Pig
	"MT_FLICKY_06", // Chipmunk
	"MT_FLICKY_07", // Penguin
	"MT_FLICKY_08", // Fish
	"MT_FLICKY_09", // Ram
	"MT_FLICKY_10", // Puffin
	"MT_FLICKY_11", // Cow
	"MT_FLICKY_12", // Rat
	"MT_FLICKY_13", // Bear
	"MT_FLICKY_14", // Dove
	"MT_FLICKY_15", // Cat
	"MT_FLICKY_16", // Canary

	// Environmental Effects
	"MT_RAIN", // Rain
	"MT_SNOWFLAKE", // Snowflake
	"MT_SPLISH", // Water splish!
	"MT_SMOKE",
	"MT_SMALLBUBBLE", // small bubble
	"MT_MEDIUMBUBBLE", // medium bubble
	"MT_EXTRALARGEBUBBLE", // extra large bubble
	"MT_WATERZAP",
	"MT_SPINDUST", // Spindash dust
	"MT_TFOG",
	"MT_SEED",
	"MT_PARTICLE",
	"MT_PARTICLEGEN", // For fans, etc.

	// Game Indicators
	"MT_SCORE", // score logo
	"MT_DROWNNUMBERS", // Drowning Timer
	"MT_GOTEMERALD", // Chaos Emerald (intangible)
	"MT_LOCKON", // Target
	"MT_TAG", // Tag Sign
	"MT_GOTFLAG", // Got Flag sign

	// Ambient Sounds
	"MT_AWATERA", // Ambient Water Sound 1
	"MT_AWATERB", // Ambient Water Sound 2
	"MT_AWATERC", // Ambient Water Sound 3
	"MT_AWATERD", // Ambient Water Sound 4
	"MT_AWATERE", // Ambient Water Sound 5
	"MT_AWATERF", // Ambient Water Sound 6
	"MT_AWATERG", // Ambient Water Sound 7
	"MT_AWATERH", // Ambient Water Sound 8
	"MT_RANDOMAMBIENT",
	"MT_RANDOMAMBIENT2",

	"MT_CORK",

	// Ring Weapons
	"MT_REDRING",
	"MT_BOUNCERING",
	"MT_RAILRING",
	"MT_INFINITYRING",
	"MT_AUTOMATICRING",
	"MT_EXPLOSIONRING",
	"MT_SCATTERRING",
	"MT_GRENADERING",

	"MT_BOUNCEPICKUP",
	"MT_RAILPICKUP",
	"MT_AUTOPICKUP",
	"MT_EXPLODEPICKUP",
	"MT_SCATTERPICKUP",
	"MT_GRENADEPICKUP",

	"MT_THROWNBOUNCE",
	"MT_THROWNINFINITY",
	"MT_THROWNAUTOMATIC",
	"MT_THROWNSCATTER",
	"MT_THROWNEXPLOSION",
	"MT_THROWNGRENADE",

	// Mario-specific stuff
	"MT_COIN",
	"MT_FLINGCOIN",
	"MT_GOOMBA",
	"MT_BLUEGOOMBA",
	"MT_FIREFLOWER",
	"MT_FIREBALL",
	"MT_SHELL",
	"MT_PUMA",
	"MT_PUMATRAIL",
	"MT_HAMMER",
	"MT_KOOPA",
	"MT_KOOPAFLAME",
	"MT_AXE",
	"MT_MARIOBUSH1",
	"MT_MARIOBUSH2",
	"MT_TOAD",

	// NiGHTS Stuff
	"MT_AXIS",
	"MT_AXISTRANSFER",
	"MT_AXISTRANSFERLINE",
	"MT_NIGHTSDRONE",
	"MT_NIGHTSGOAL",
	"MT_NIGHTSPARKLE",
	"MT_NIGHTSLOOPHELPER",
	"MT_NIGHTSBUMPER", // NiGHTS Bumper
	"MT_HOOP",
	"MT_HOOPCOLLIDE", // Collision detection for NiGHTS hoops
	"MT_HOOPCENTER", // Center of a hoop
	"MT_NIGHTSCORE",
	"MT_NIGHTSWING",
	"MT_NIGHTSSUPERLOOP",
	"MT_NIGHTSDRILLREFILL",
	"MT_NIGHTSHELPER",
	"MT_NIGHTSEXTRATIME",
	"MT_NIGHTSLINKFREEZE",
	"MT_EGGCAPSULE",
	"MT_NIGHTOPIANHELPER", // the actual helper object that orbits you

	// Utility Objects
	"MT_TELEPORTMAN",
	"MT_ALTVIEWMAN",
	"MT_CRUMBLEOBJ", // Sound generator for crumbling platform
	"MT_TUBEWAYPOINT",
	"MT_PUSH",
	"MT_PULL",
	"MT_GHOST",
	"MT_OVERLAY",
	"MT_POLYANCHOR",
	"MT_POLYSPAWN",
	"MT_POLYSPAWNCRUSH",

	// Skybox objects
	"MT_SKYBOX",

	// Debris
	"MT_SPARK", //spark
	"MT_EXPLODE", // Robot Explosion
	"MT_UWEXPLODE", // Underwater Explosion
	"MT_ROCKSPAWNER",
	"MT_FALLINGROCK",
	"MT_ROCKCRUMBLE1",
	"MT_ROCKCRUMBLE2",
	"MT_ROCKCRUMBLE3",
	"MT_ROCKCRUMBLE4",
	"MT_ROCKCRUMBLE5",
	"MT_ROCKCRUMBLE6",
	"MT_ROCKCRUMBLE7",
	"MT_ROCKCRUMBLE8",
	"MT_ROCKCRUMBLE9",
	"MT_ROCKCRUMBLE10",
	"MT_ROCKCRUMBLE11",
	"MT_ROCKCRUMBLE12",
	"MT_ROCKCRUMBLE13",
	"MT_ROCKCRUMBLE14",
	"MT_ROCKCRUMBLE15",
	"MT_ROCKCRUMBLE16",

#ifdef SEENAMES
	"MT_NAMECHECK",
#endif
};

static const char *const MOBJFLAG_LIST[] = {
	"SPECIAL",
	"SOLID",
	"SHOOTABLE",
	"NOSECTOR",
	"NOBLOCKMAP",
	"PAPERCOLLISION",
	"PUSHABLE",
	"BOSS",
	"SPAWNCEILING",
	"NOGRAVITY",
	"AMBIENT",
	"SLIDEME",
	"NOCLIP",
	"FLOAT",
	"BOXICON",
	"MISSILE",
	"SPRING",
	"BOUNCE",
	"MONITOR",
	"NOTHINK",
	"FIRE",
	"NOCLIPHEIGHT",
	"ENEMY",
	"SCENERY",
	"PAIN",
	"STICKY",
	"NIGHTSITEM",
	"NOCLIPTHING",
	"GRENADEBOUNCE",
	"RUNSPAWNFUNC",
	NULL
};

// \tMF2_(\S+).*// (.+) --> \t"\1", // \2
static const char *const MOBJFLAG2_LIST[] = {
	"AXIS",			  // It's a NiGHTS axis! (For faster checking)
	"TWOD",			  // Moves like it's in a 2D level
	"DONTRESPAWN",	  // Don't respawn this object!
	"DONTDRAW",		  // Don't generate a vissprite
	"AUTOMATIC",	  // Thrown ring has automatic properties
	"RAILRING",		  // Thrown ring has rail properties
	"BOUNCERING",	  // Thrown ring has bounce properties
	"EXPLOSION",	  // Thrown ring has explosive properties
	"SCATTER",		  // Thrown ring has scatter properties
	"BEYONDTHEGRAVE", // Source of this missile has died and has since respawned.
	"SLIDEPUSH",	  // MF_PUSHABLE that pushes continuously.
	"CLASSICPUSH",	  // Drops straight down when object has negative Z.
	"STANDONME",	  // While not pushable, stand on me anyway.
	"INFLOAT",		  // Floating to a height for a move, don't auto float to target's height.
	"DEBRIS",		  // Splash ring from explosion ring
	"NIGHTSPULL",	  // Attracted from a paraloop
	"JUSTATTACKED",	  // can be pushed by other moving mobjs
	"FIRING",		  // turret fire
	"SUPERFIRE",	  // Firing something with Super Sonic-stopping properties. Or, if mobj has MF_MISSILE, this is the actual fire from it.
	"SHADOW",		  // Fuzzy draw, makes targeting harder.
	"STRONGBOX",	  // Flag used for "strong" random monitors.
	"OBJECTFLIP",	  // Flag for objects that always have flipped gravity.
	"SKULLFLY",		  // Special handling: skull in flight.
	"FRET",			  // Flashing from a previous hit
	"BOSSNOTRAP",	  // No Egg Trap after boss
	"BOSSFLEE",		  // Boss is fleeing!
	"BOSSDEAD",		  // Boss is dead! (Not necessarily fleeing, if a fleeing point doesn't exist.)
	"AMBUSH",         // Alternate behaviour typically set by MTF_AMBUSH
	"LINKDRAW",       // Draw vissprite of mobj immediately before/after tracer's vissprite (dependent on dispoffset and position)
	"SHIELD",         // Thinker calls P_AddShield/P_ShieldLook (must be partnered with MF_SCENERY to use)
	"MACEROTATE",     // Thinker calls P_MaceRotate around tracer
	NULL
};

static const char *const MOBJEFLAG_LIST[] = {
	"ONGROUND", // The mobj stands on solid floor (not on another mobj or in air)
	"JUSTHITFLOOR", // The mobj just hit the floor while falling, this is cleared on next frame
	"TOUCHWATER", // The mobj stands in a sector with water, and touches the surface
	"UNDERWATER", // The mobj stands in a sector with water, and his waist is BELOW the water surface
	"JUSTSTEPPEDDOWN", // used for ramp sectors
	"VERTICALFLIP", // Vertically flip sprite/allow upside-down physics
	"GOOWATER", // Goo water
	"PUSHED", // Mobj was already pushed this tic
	"SPRUNG", // Mobj was already sprung this tic
	"APPLYPMOMZ", // Platform movement
	NULL
};

#ifdef HAVE_BLUA
static const char *const MAPTHINGFLAG_LIST[4] = {
	NULL,
	"OBJECTFLIP", // Reverse gravity flag for objects.
	"OBJECTSPECIAL", // Special flag used with certain objects.
	"AMBUSH" // Deaf monsters/do not react to sound.
};
#endif

static const char *const PLAYERFLAG_LIST[] = {

	// Cvars
	"FLIPCAM", // Flip camera angle with gravity flip prefrence.
	"ANALOGMODE", // Analog mode?
	"DIRECTIONCHAR", // Directional character sprites?
	"AUTOBRAKE", // Autobrake?

	// Cheats
	"GODMODE",
	"NOCLIP",
	"INVIS",

	// True if button down last tic.
	"ATTACKDOWN",
	"USEDOWN",
	"JUMPDOWN",
	"WPNDOWN",

	// Unmoving states
	"STASIS", // Player is not allowed to move
	"JUMPSTASIS", // and that includes jumping.
	// (we don't include FULLSTASIS here I guess because it's just those two together...?)

	// Applying autobrake?
	"APPLYAUTOBRAKE",

	// Character action status
	"STARTJUMP",
	"JUMPED",
	"NOJUMPDAMAGE",

	"SPINNING",
	"STARTDASH",

	"THOKKED",
	"SHIELDABILITY",
	"GLIDING",
	"BOUNCING",

	// Sliding (usually in water) like Labyrinth/Oil Ocean
	"SLIDING",

	// NiGHTS stuff
	"TRANSFERTOCLOSEST",
	"DRILLING",

	// Gametype-specific stuff
	"GAMETYPEOVER", // Race time over, or H&S out-of-game
	"TAGIT", // The player is it! For Tag Mode

	/*** misc ***/
	"FORCESTRAFE", // Translate turn inputs into strafe inputs
	"CANCARRY", // Can carry?

	NULL // stop loop here.
};

#ifdef HAVE_BLUA
// Linedef flags
static const char *const ML_LIST[16] = {
	"IMPASSIBLE",
	"BLOCKMONSTERS",
	"TWOSIDED",
	"DONTPEGTOP",
	"DONTPEGBOTTOM",
	"EFFECT1",
	"NOCLIMB",
	"EFFECT2",
	"EFFECT3",
	"EFFECT4",
	"EFFECT5",
	"NOSONIC",
	"NOTAILS",
	"NOKNUX",
	"BOUNCY",
	"TFERLINE"
};
#endif

// This DOES differ from r_draw's Color_Names, unfortunately.
// Also includes Super colors
static const char *COLOR_ENUMS[] = {
	"NONE",			// SKINCOLOR_NONE,

	// Greyscale ranges
	"WHITE",     	// SKINCOLOR_WHITE,
	"BONE",     	// SKINCOLOR_BONE,
	"CLOUDY",     	// SKINCOLOR_CLOUDY,
	"GREY",     	// SKINCOLOR_GREY,
	"SILVER",     	// SKINCOLOR_SILVER,
	"CARBON",     	// SKINCOLOR_CARBON,
	"JET",     		// SKINCOLOR_JET,
	"BLACK",     	// SKINCOLOR_BLACK,

	// Desaturated
	"AETHER",     	// SKINCOLOR_AETHER,
	"SLATE",     	// SKINCOLOR_SLATE,
	"PINK",     	// SKINCOLOR_PINK,
	"YOGURT",     	// SKINCOLOR_YOGURT,
	"BROWN",     	// SKINCOLOR_BROWN,
	"TAN",     		// SKINCOLOR_TAN,
	"BEIGE",     	// SKINCOLOR_BEIGE,
	"MOSS",     	// SKINCOLOR_MOSS,
	"AZURE",     	// SKINCOLOR_AZURE,
	"LAVENDER",     // SKINCOLOR_LAVENDER,

	// Viv's vivid colours (toast 21/07/17)
	"RUBY",     	// SKINCOLOR_RUBY,
	"SALMON",     	// SKINCOLOR_SALMON,
	"RED",     		// SKINCOLOR_RED,
	"CRIMSON",     	// SKINCOLOR_CRIMSON,
	"FLAME",     	// SKINCOLOR_FLAME,
	"PEACHY",     	// SKINCOLOR_PEACHY,
	"QUAIL",     	// SKINCOLOR_QUAIL,
	"SUNSET",     	// SKINCOLOR_SUNSET,
	"APRICOT",     	// SKINCOLOR_APRICOT,
	"ORANGE",     	// SKINCOLOR_ORANGE,
	"RUST",     	// SKINCOLOR_RUST,
	"GOLD",     	// SKINCOLOR_GOLD,
	"SANDY",     	// SKINCOLOR_SANDY,
	"YELLOW",     	// SKINCOLOR_YELLOW,
	"OLIVE",     	// SKINCOLOR_OLIVE,
	"LIME",     	// SKINCOLOR_LIME,
	"PERIDOT",     	// SKINCOLOR_PERIDOT,
	"GREEN",     	// SKINCOLOR_GREEN,
	"FOREST",     	// SKINCOLOR_FOREST,
	"EMERALD",     	// SKINCOLOR_EMERALD,
	"MINT",     	// SKINCOLOR_MINT,
	"SEAFOAM",     	// SKINCOLOR_SEAFOAM,
	"AQUA",     	// SKINCOLOR_AQUA,
	"TEAL",     	// SKINCOLOR_TEAL,
	"WAVE",     	// SKINCOLOR_WAVE,
	"CYAN",     	// SKINCOLOR_CYAN,
	"SKY",     		// SKINCOLOR_SKY,
	"CERULEAN",     // SKINCOLOR_CERULEAN,
	"ICY",     		// SKINCOLOR_ICY,
	"SAPPHIRE",     // SKINCOLOR_SAPPHIRE,
	"CORNFLOWER",   // SKINCOLOR_CORNFLOWER,
	"BLUE",     	// SKINCOLOR_BLUE,
	"COBALT",     	// SKINCOLOR_COBALT,
	"VAPOR",     	// SKINCOLOR_VAPOR,
	"DUSK",     	// SKINCOLOR_DUSK,
	"PASTEL",     	// SKINCOLOR_PASTEL,
	"PURPLE",     	// SKINCOLOR_PURPLE,
	"BUBBLEGUM",    // SKINCOLOR_BUBBLEGUM,
	"MAGENTA",     	// SKINCOLOR_MAGENTA,
	"NEON",     	// SKINCOLOR_NEON,
	"VIOLET",     	// SKINCOLOR_VIOLET,
	"LILAC",     	// SKINCOLOR_LILAC,
	"PLUM",     	// SKINCOLOR_PLUM,
	"ROSY",     	// SKINCOLOR_ROSY,

	// Super special awesome Super flashing colors!
	"SUPERSILVER1",	// SKINCOLOR_SUPERSILVER1
	"SUPERSILVER2",	// SKINCOLOR_SUPERSILVER2,
	"SUPERSILVER3",	// SKINCOLOR_SUPERSILVER3,
	"SUPERSILVER4",	// SKINCOLOR_SUPERSILVER4,
	"SUPERSILVER5",	// SKINCOLOR_SUPERSILVER5,

	"SUPERRED1",	// SKINCOLOR_SUPERRED1
	"SUPERRED2",	// SKINCOLOR_SUPERRED2,
	"SUPERRED3",	// SKINCOLOR_SUPERRED3,
	"SUPERRED4",	// SKINCOLOR_SUPERRED4,
	"SUPERRED5",	// SKINCOLOR_SUPERRED5,

	"SUPERORANGE1",	// SKINCOLOR_SUPERORANGE1
	"SUPERORANGE2",	// SKINCOLOR_SUPERORANGE2,
	"SUPERORANGE3",	// SKINCOLOR_SUPERORANGE3,
	"SUPERORANGE4",	// SKINCOLOR_SUPERORANGE4,
	"SUPERORANGE5",	// SKINCOLOR_SUPERORANGE5,

	"SUPERGOLD1",	// SKINCOLOR_SUPERGOLD1
	"SUPERGOLD2",	// SKINCOLOR_SUPERGOLD2,
	"SUPERGOLD3",	// SKINCOLOR_SUPERGOLD3,
	"SUPERGOLD4",	// SKINCOLOR_SUPERGOLD4,
	"SUPERGOLD5",	// SKINCOLOR_SUPERGOLD5,

	"SUPERPERIDOT1",	// SKINCOLOR_SUPERPERIDOT1
	"SUPERPERIDOT2",	// SKINCOLOR_SUPERPERIDOT2,
	"SUPERPERIDOT3",	// SKINCOLOR_SUPERPERIDOT3,
	"SUPERPERIDOT4",	// SKINCOLOR_SUPERPERIDOT4,
	"SUPERPERIDOT5",	// SKINCOLOR_SUPERPERIDOT5,

	"SUPERSKY1",	// SKINCOLOR_SUPERSKY1
	"SUPERSKY2",	// SKINCOLOR_SUPERSKY2,
	"SUPERSKY3",	// SKINCOLOR_SUPERSKY3,
	"SUPERSKY4",	// SKINCOLOR_SUPERSKY4,
	"SUPERSKY5",	// SKINCOLOR_SUPERSKY5,

	"SUPERPURPLE1",	// SKINCOLOR_SUPERPURPLE1,
	"SUPERPURPLE2",	// SKINCOLOR_SUPERPURPLE2,
	"SUPERPURPLE3",	// SKINCOLOR_SUPERPURPLE3,
	"SUPERPURPLE4",	// SKINCOLOR_SUPERPURPLE4,
	"SUPERPURPLE5",	// SKINCOLOR_SUPERPURPLE5,

	"SUPERRUST1",	// SKINCOLOR_SUPERRUST1
	"SUPERRUST2",	// SKINCOLOR_SUPERRUST2,
	"SUPERRUST3",	// SKINCOLOR_SUPERRUST3,
	"SUPERRUST4",	// SKINCOLOR_SUPERRUST4,
	"SUPERRUST5",	// SKINCOLOR_SUPERRUST5,

	"SUPERTAN1",	// SKINCOLOR_SUPERTAN1
	"SUPERTAN2",	// SKINCOLOR_SUPERTAN2,
	"SUPERTAN3",	// SKINCOLOR_SUPERTAN3,
	"SUPERTAN4",	// SKINCOLOR_SUPERTAN4,
	"SUPERTAN5"		// SKINCOLOR_SUPERTAN5,
};

static const char *const POWERS_LIST[] = {
	"INVULNERABILITY",
	"SNEAKERS",
	"FLASHING",
	"SHIELD",
	"CARRY",
	"TAILSFLY", // tails flying
	"UNDERWATER", // underwater timer
	"SPACETIME", // In space, no one can hear you spin!
	"EXTRALIFE", // Extra Life timer
	"PUSHING",

	"SUPER", // Are you super?
	"GRAVITYBOOTS", // gravity boots

	// Weapon ammunition
	"INFINITYRING",
	"AUTOMATICRING",
	"BOUNCERING",
	"SCATTERRING",
	"GRENADERING",
	"EXPLOSIONRING",
	"RAILRING",

	// Power Stones
	"EMERALDS", // stored like global 'emeralds' variable

	// NiGHTS powerups
	"NIGHTS_SUPERLOOP",
	"NIGHTS_HELPER",
	"NIGHTS_LINKFREEZE",

	//for linedef exec 427
	"NOCONTROL"
};

static const char *const HUDITEMS_LIST[] = {
	"LIVESNAME",
	"LIVESPIC",
	"LIVESNUM",
	"LIVESX",

	"RINGS",
	"RINGSSPLIT",
	"RINGSNUM",
	"RINGSNUMSPLIT",

	"SCORE",
	"SCORENUM",

	"TIME",
	"TIMESPLIT",
	"MINUTES",
	"MINUTESSPLIT",
	"TIMECOLON",
	"TIMECOLONSPLIT",
	"SECONDS",
	"SECONDSSPLIT",
	"TIMETICCOLON",
	"TICS",

	"SS_TOTALRINGS",
	"SS_TOTALRINGS_SPLIT",

	"GETRINGS",
	"GETRINGSNUM",
	"TIMELEFT",
	"TIMELEFTNUM",
	"TIMEUP",
	"HUNTPICS",
	"GRAVBOOTSICO",
	"LAP"
};

struct {
	const char *n;
	// has to be able to hold both fixed_t and angle_t, so drastic measure!!
#ifdef HAVE_BLUA
	lua_Integer v;
#else
	INT64 v;
#endif
} const INT_CONST[] = {
	// If a mod removes some variables here,
	// please leave the names in-tact and just set
	// the value to 0 or something.

	// integer type limits, from doomtype.h
	// INT64 and UINT64 limits not included, they're too big for most purposes anyway
	// signed
	{"INT8_MIN",INT8_MIN},
	{"INT16_MIN",INT16_MIN},
	{"INT32_MIN",INT32_MIN},
	{"INT8_MAX",INT8_MAX},
	{"INT16_MAX",INT16_MAX},
	{"INT32_MAX",INT32_MAX},
	// unsigned
	{"UINT8_MAX",UINT8_MAX},
	{"UINT16_MAX",UINT16_MAX},
	{"UINT32_MAX",UINT32_MAX},

	// fixed_t constants, from m_fixed.h
	{"FRACUNIT",FRACUNIT},
	{"FRACBITS",FRACBITS},

	// doomdef.h constants
	{"TICRATE",TICRATE},
	{"RING_DIST",RING_DIST},
	{"PUSHACCEL",PUSHACCEL},
	{"MODID",MODID}, // I don't know, I just thought it would be cool for a wad to potentially know what mod it was loaded into.
	{"CODEBASE",CODEBASE}, // or what release of SRB2 this is.
	{"VERSION",VERSION}, // Grab the game's version!
	{"SUBVERSION",SUBVERSION}, // more precise version number

	// Special linedef executor tag numbers!
	{"LE_PINCHPHASE",LE_PINCHPHASE}, // A boss entered pinch phase (and, in most cases, is preparing their pinch phase attack!)
	{"LE_ALLBOSSESDEAD",LE_ALLBOSSESDEAD}, // All bosses in the map are dead (Egg capsule raise)
	{"LE_BOSSDEAD",LE_BOSSDEAD}, // A boss in the map died (Chaos mode boss tally)
	{"LE_BOSS4DROP",LE_BOSS4DROP}, // CEZ boss dropped its cage
	{"LE_BRAKVILEATACK",LE_BRAKVILEATACK}, // Brak's doing his LOS attack, oh noes

	/// \todo Get all this stuff into its own sections, maybe. Maybe.

	// Frame settings
	{"FF_FRAMEMASK",FF_FRAMEMASK},
	{"FF_SPR2SUPER",FF_SPR2SUPER},
	{"FF_SPR2ENDSTATE",FF_SPR2ENDSTATE},
	{"FF_SPR2MIDSTART",FF_SPR2MIDSTART},
	{"FF_ANIMATE",FF_ANIMATE},
	{"FF_RANDOMANIM",FF_RANDOMANIM},
	{"FF_GLOBALANIM",FF_GLOBALANIM},
	{"FF_FULLBRIGHT",FF_FULLBRIGHT},
	{"FF_VERTICALFLIP",FF_VERTICALFLIP},
	{"FF_PAPERSPRITE",FF_PAPERSPRITE},
	{"FF_TRANSMASK",FF_TRANSMASK},
	{"FF_TRANSSHIFT",FF_TRANSSHIFT},
	// new preshifted translucency (used in source)
	{"FF_TRANS10",FF_TRANS10},
	{"FF_TRANS20",FF_TRANS20},
	{"FF_TRANS30",FF_TRANS30},
	{"FF_TRANS40",FF_TRANS40},
	{"FF_TRANS50",FF_TRANS50},
	{"FF_TRANS60",FF_TRANS60},
	{"FF_TRANS70",FF_TRANS70},
	{"FF_TRANS80",FF_TRANS80},
	{"FF_TRANS90",FF_TRANS90},
	// compatibility
	// Transparency for SOCs is pre-shifted
	{"TR_TRANS10",tr_trans10<<FF_TRANSSHIFT},
	{"TR_TRANS20",tr_trans20<<FF_TRANSSHIFT},
	{"TR_TRANS30",tr_trans30<<FF_TRANSSHIFT},
	{"TR_TRANS40",tr_trans40<<FF_TRANSSHIFT},
	{"TR_TRANS50",tr_trans50<<FF_TRANSSHIFT},
	{"TR_TRANS60",tr_trans60<<FF_TRANSSHIFT},
	{"TR_TRANS70",tr_trans70<<FF_TRANSSHIFT},
	{"TR_TRANS80",tr_trans80<<FF_TRANSSHIFT},
	{"TR_TRANS90",tr_trans90<<FF_TRANSSHIFT},
	// Transparency for Lua is not, unless capitalized as above.
	{"tr_trans10",tr_trans10},
	{"tr_trans20",tr_trans20},
	{"tr_trans30",tr_trans30},
	{"tr_trans40",tr_trans40},
	{"tr_trans50",tr_trans50},
	{"tr_trans60",tr_trans60},
	{"tr_trans70",tr_trans70},
	{"tr_trans80",tr_trans80},
	{"tr_trans90",tr_trans90},
	{"NUMTRANSMAPS",NUMTRANSMAPS},

	// Type of levels
	{"TOL_SP",TOL_SP},
	{"TOL_COOP",TOL_COOP},
	{"TOL_COMPETITION",TOL_COMPETITION},
	{"TOL_RACE",TOL_RACE},
	{"TOL_MATCH",TOL_MATCH},
	{"TOL_TAG",TOL_TAG},
	{"TOL_CTF",TOL_CTF},
	{"TOL_CUSTOM",TOL_CUSTOM},
	{"TOL_2D",TOL_2D},
	{"TOL_MARIO",TOL_MARIO},
	{"TOL_NIGHTS",TOL_NIGHTS},
	{"TOL_ERZ3",TOL_ERZ3},
	{"TOL_XMAS",TOL_XMAS},

	// Level flags
	{"LF_SCRIPTISFILE",LF_SCRIPTISFILE},
	{"LF_SPEEDMUSIC",LF_SPEEDMUSIC},
	{"LF_NOSSMUSIC",LF_NOSSMUSIC},
	{"LF_NORELOAD",LF_NORELOAD},
	{"LF_NOZONE",LF_NOZONE},
	{"LF_SAVEGAME",LF_SAVEGAME},
	// And map flags
	{"LF2_HIDEINMENU",LF2_HIDEINMENU},
	{"LF2_HIDEINSTATS",LF2_HIDEINSTATS},
	{"LF2_RECORDATTACK",LF2_RECORDATTACK},
	{"LF2_NIGHTSATTACK",LF2_NIGHTSATTACK},
	{"LF2_NOVISITNEEDED",LF2_NOVISITNEEDED},
	{"LF2_WIDEICON",LF2_WIDEICON},

	// NiGHTS grades
	{"GRADE_F",GRADE_F},
	{"GRADE_E",GRADE_E},
	{"GRADE_D",GRADE_D},
	{"GRADE_C",GRADE_C},
	{"GRADE_B",GRADE_B},
	{"GRADE_A",GRADE_A},
	{"GRADE_S",GRADE_S},

	// Emeralds
	{"EMERALD1",EMERALD1},
	{"EMERALD2",EMERALD2},
	{"EMERALD3",EMERALD3},
	{"EMERALD4",EMERALD4},
	{"EMERALD5",EMERALD5},
	{"EMERALD6",EMERALD6},
	{"EMERALD7",EMERALD7},

	// SKINCOLOR_ doesn't include these..!
	{"MAXSKINCOLORS",MAXSKINCOLORS},
	{"MAXTRANSLATIONS",MAXTRANSLATIONS},

	// Precipitation
	{"PRECIP_NONE",PRECIP_NONE},
	{"PRECIP_STORM",PRECIP_STORM},
	{"PRECIP_SNOW",PRECIP_SNOW},
	{"PRECIP_RAIN",PRECIP_RAIN},
	{"PRECIP_BLANK",PRECIP_BLANK},
	{"PRECIP_STORM_NORAIN",PRECIP_STORM_NORAIN},
	{"PRECIP_STORM_NOSTRIKES",PRECIP_STORM_NOSTRIKES},

	// Shields
	{"SH_NONE",SH_NONE},
	// Shield flags
	{"SH_PROTECTFIRE",SH_PROTECTFIRE},
	{"SH_PROTECTWATER",SH_PROTECTWATER},
	{"SH_PROTECTELECTRIC",SH_PROTECTELECTRIC},
	{"SH_PROTECTSPIKE",SH_PROTECTSPIKE},
	// Indivisible shields
	{"SH_PITY",SH_PITY},
	{"SH_WHIRLWIND",SH_WHIRLWIND},
	{"SH_ARMAGEDDON",SH_ARMAGEDDON},
	// normal shields that use flags
	{"SH_ATTRACT",SH_ATTRACT},
	{"SH_ELEMENTAL",SH_ELEMENTAL},
	// Sonic 3 shields
	{"SH_FLAMEAURA",SH_FLAMEAURA},
	{"SH_BUBBLEWRAP",SH_BUBBLEWRAP},
	{"SH_THUNDERCOIN",SH_THUNDERCOIN},
	// The force shield uses the lower 8 bits to count how many extra hits are left.
	{"SH_FORCE",SH_FORCE},
	{"SH_FORCEHP",SH_FORCEHP}, // to be used as a bitmask only
	// Mostly for use with Mario mode.
	{"SH_FIREFLOWER", SH_FIREFLOWER},
	{"SH_STACK",SH_STACK},
	{"SH_NOSTACK",SH_NOSTACK},

	// Carrying
	{"CR_NONE",CR_NONE},
	{"CR_GENERIC",CR_GENERIC},
	{"CR_PLAYER",CR_PLAYER},
	{"CR_NIGHTSMODE",CR_NIGHTSMODE},
	{"CR_NIGHTSFALL",CR_NIGHTSFALL},
	{"CR_BRAKGOOP",CR_BRAKGOOP},
	{"CR_ZOOMTUBE",CR_ZOOMTUBE},
	{"CR_ROPEHANG",CR_ROPEHANG},
	{"CR_MACESPIN",CR_MACESPIN},

	// Ring weapons (ringweapons_t)
	// Useful for A_GiveWeapon
	{"RW_AUTO",RW_AUTO},
	{"RW_BOUNCE",RW_BOUNCE},
	{"RW_SCATTER",RW_SCATTER},
	{"RW_GRENADE",RW_GRENADE},
	{"RW_EXPLODE",RW_EXPLODE},
	{"RW_RAIL",RW_RAIL},

	// Character flags (skinflags_t)
	{"SF_SUPER",SF_SUPER},
	{"SF_NOSUPERSPIN",SF_NOSUPERSPIN},
	{"SF_NOSPINDASHDUST",SF_NOSPINDASHDUST},
	{"SF_HIRES",SF_HIRES},
	{"SF_NOSKID",SF_NOSKID},
	{"SF_NOSPEEDADJUST",SF_NOSPEEDADJUST},
	{"SF_RUNONWATER",SF_RUNONWATER},
	{"SF_NOJUMPSPIN",SF_NOJUMPSPIN},
	{"SF_NOJUMPDAMAGE",SF_NOJUMPDAMAGE},
	{"SF_STOMPDAMAGE",SF_STOMPDAMAGE},
	{"SF_MARIODAMAGE",SF_MARIODAMAGE},
	{"SF_MACHINE",SF_MACHINE},
	{"SF_DASHMODE",SF_DASHMODE},
	{"SF_FASTEDGE",SF_FASTEDGE},
	{"SF_MULTIABILITY",SF_MULTIABILITY},

	// Character abilities!
	// Primary
	{"CA_NONE",CA_NONE}, // now slot 0!
	{"CA_THOK",CA_THOK},
	{"CA_FLY",CA_FLY},
	{"CA_GLIDEANDCLIMB",CA_GLIDEANDCLIMB},
	{"CA_HOMINGTHOK",CA_HOMINGTHOK},
	{"CA_DOUBLEJUMP",CA_DOUBLEJUMP},
	{"CA_FLOAT",CA_FLOAT},
	{"CA_SLOWFALL",CA_SLOWFALL},
	{"CA_SWIM",CA_SWIM},
	{"CA_TELEKINESIS",CA_TELEKINESIS},
	{"CA_FALLSWITCH",CA_FALLSWITCH},
	{"CA_JUMPBOOST",CA_JUMPBOOST},
	{"CA_AIRDRILL",CA_AIRDRILL},
	{"CA_JUMPTHOK",CA_JUMPTHOK},
	{"CA_BOUNCE",CA_BOUNCE},
	{"CA_TWINSPIN",CA_TWINSPIN},
	// Secondary
	{"CA2_NONE",CA2_NONE}, // now slot 0!
	{"CA2_SPINDASH",CA2_SPINDASH},
	{"CA2_GUNSLINGER",CA2_GUNSLINGER},
	{"CA2_MELEE",CA2_MELEE},

	// Sound flags
	{"SF_TOTALLYSINGLE",SF_TOTALLYSINGLE},
	{"SF_NOMULTIPLESOUND",SF_NOMULTIPLESOUND},
	{"SF_OUTSIDESOUND",SF_OUTSIDESOUND},
	{"SF_X4AWAYSOUND",SF_X4AWAYSOUND},
	{"SF_X8AWAYSOUND",SF_X8AWAYSOUND},
	{"SF_NOINTERRUPT",SF_NOINTERRUPT},
	{"SF_X2AWAYSOUND",SF_X2AWAYSOUND},

	// Global emblem var flags
	{"GE_NIGHTSPULL",GE_NIGHTSPULL},
	{"GE_NIGHTSITEM",GE_NIGHTSITEM},

	// Map emblem var flags
	{"ME_ALLEMERALDS",ME_ALLEMERALDS},
	{"ME_ULTIMATE",ME_ULTIMATE},
	{"ME_PERFECT",ME_PERFECT},
	
#ifdef HAVE_BLUA
	// p_local.h constants
	{"FLOATSPEED",FLOATSPEED},
	{"MAXSTEPMOVE",MAXSTEPMOVE},
	{"USERANGE",USERANGE},
	{"MELEERANGE",MELEERANGE},
	{"MISSILERANGE",MISSILERANGE},
	{"ONFLOORZ",ONFLOORZ}, // INT32_MIN
	{"ONCEILINGZ",ONCEILINGZ}, //INT32_MAX
	// for P_FlashPal
	{"PAL_WHITE",PAL_WHITE},
	{"PAL_MIXUP",PAL_MIXUP},
	{"PAL_RECYCLE",PAL_RECYCLE},
	{"PAL_NUKE",PAL_NUKE},
	// for P_DamageMobj
	//// Damage types
	{"DMG_WATER",DMG_WATER},
	{"DMG_FIRE",DMG_FIRE},
	{"DMG_ELECTRIC",DMG_ELECTRIC},
	{"DMG_SPIKE",DMG_SPIKE},
	{"DMG_NUKE",DMG_NUKE},
	//// Death types
	{"DMG_INSTAKILL",DMG_INSTAKILL},
	{"DMG_DROWNED",DMG_DROWNED},
	{"DMG_SPACEDROWN",DMG_SPACEDROWN},
	{"DMG_DEATHPIT",DMG_DEATHPIT},
	{"DMG_CRUSHED",DMG_CRUSHED},
	{"DMG_SPECTATOR",DMG_SPECTATOR},
	//// Masks
	{"DMG_DEATHMASK",DMG_DEATHMASK},

	// Gametypes, for use with global var "gametype"
	{"GT_COOP",GT_COOP},
	{"GT_COMPETITION",GT_COMPETITION},
	{"GT_RACE",GT_RACE},
	{"GT_MATCH",GT_MATCH},
	{"GT_TEAMMATCH",GT_TEAMMATCH},
	{"GT_TAG",GT_TAG},
	{"GT_HIDEANDSEEK",GT_HIDEANDSEEK},
	{"GT_CTF",GT_CTF},

	// Player state (playerstate_t)
	{"PST_LIVE",PST_LIVE}, // Playing or camping.
	{"PST_DEAD",PST_DEAD}, // Dead on the ground, view follows killer.
	{"PST_REBORN",PST_REBORN}, // Ready to restart/respawn???

	// Player animation (panim_t)
	{"PA_ETC",PA_ETC},
	{"PA_IDLE",PA_IDLE},
	{"PA_EDGE",PA_EDGE},
	{"PA_WALK",PA_WALK},
	{"PA_RUN",PA_RUN},
	{"PA_DASH",PA_DASH},
	{"PA_PAIN",PA_PAIN},
	{"PA_ROLL",PA_ROLL},
	{"PA_JUMP",PA_JUMP},
	{"PA_SPRING",PA_SPRING},
	{"PA_FALL",PA_FALL},
	{"PA_ABILITY",PA_ABILITY},
	{"PA_ABILITY2",PA_ABILITY2},
	{"PA_RIDE",PA_RIDE},

	// Current weapon
	{"WEP_AUTO",WEP_AUTO},
	{"WEP_BOUNCE",WEP_BOUNCE},
	{"WEP_SCATTER",WEP_SCATTER},
	{"WEP_GRENADE",WEP_GRENADE},
	{"WEP_EXPLODE",WEP_EXPLODE},
	{"WEP_RAIL",WEP_RAIL},
	{"NUM_WEAPONS",NUM_WEAPONS},

	// Got Flags, for player->gotflag!
	// Used to be MF_ for some stupid reason, now they're GF_ to stop them looking like mobjflags
	{"GF_REDFLAG",GF_REDFLAG},
	{"GF_BLUEFLAG",GF_BLUEFLAG},

	// Customisable sounds for Skins, from sounds.h
	{"SKSSPIN",SKSSPIN},
	{"SKSPUTPUT",SKSPUTPUT},
	{"SKSPUDPUD",SKSPUDPUD},
	{"SKSPLPAN1",SKSPLPAN1}, // Ouchies
	{"SKSPLPAN2",SKSPLPAN2},
	{"SKSPLPAN3",SKSPLPAN3},
	{"SKSPLPAN4",SKSPLPAN4},
	{"SKSPLDET1",SKSPLDET1}, // Deaths
	{"SKSPLDET2",SKSPLDET2},
	{"SKSPLDET3",SKSPLDET3},
	{"SKSPLDET4",SKSPLDET4},
	{"SKSPLVCT1",SKSPLVCT1}, // Victories
	{"SKSPLVCT2",SKSPLVCT2},
	{"SKSPLVCT3",SKSPLVCT3},
	{"SKSPLVCT4",SKSPLVCT4},
	{"SKSTHOK",SKSTHOK},
	{"SKSSPNDSH",SKSSPNDSH},
	{"SKSZOOM",SKSZOOM},
	{"SKSSKID",SKSSKID},
	{"SKSGASP",SKSGASP},
	{"SKSJUMP",SKSJUMP},

	// 3D Floor/Fake Floor/FOF/whatever flags
	{"FF_EXISTS",FF_EXISTS},                   ///< Always set, to check for validity.
	{"FF_BLOCKPLAYER",FF_BLOCKPLAYER},         ///< Solid to player, but nothing else
	{"FF_BLOCKOTHERS",FF_BLOCKOTHERS},         ///< Solid to everything but player
	{"FF_SOLID",FF_SOLID},                     ///< Clips things.
	{"FF_RENDERSIDES",FF_RENDERSIDES},         ///< Renders the sides.
	{"FF_RENDERPLANES",FF_RENDERPLANES},       ///< Renders the floor/ceiling.
	{"FF_RENDERALL",FF_RENDERALL},             ///< Renders everything.
	{"FF_SWIMMABLE",FF_SWIMMABLE},             ///< Is a water block.
	{"FF_NOSHADE",FF_NOSHADE},                 ///< Messes with the lighting?
	{"FF_CUTSOLIDS",FF_CUTSOLIDS},             ///< Cuts out hidden solid pixels.
	{"FF_CUTEXTRA",FF_CUTEXTRA},               ///< Cuts out hidden translucent pixels.
	{"FF_CUTLEVEL",FF_CUTLEVEL},               ///< Cuts out all hidden pixels.
	{"FF_CUTSPRITES",FF_CUTSPRITES},           ///< Final step in making 3D water.
	{"FF_BOTHPLANES",FF_BOTHPLANES},           ///< Renders both planes all the time.
	{"FF_EXTRA",FF_EXTRA},                     ///< Gets cut by ::FF_CUTEXTRA.
	{"FF_TRANSLUCENT",FF_TRANSLUCENT},         ///< See through!
	{"FF_FOG",FF_FOG},                         ///< Fog "brush."
	{"FF_INVERTPLANES",FF_INVERTPLANES},       ///< Reverse the plane visibility rules.
	{"FF_ALLSIDES",FF_ALLSIDES},               ///< Render inside and outside sides.
	{"FF_INVERTSIDES",FF_INVERTSIDES},         ///< Only render inside sides.
	{"FF_DOUBLESHADOW",FF_DOUBLESHADOW},       ///< Make two lightlist entries to reset light?
	{"FF_FLOATBOB",FF_FLOATBOB},               ///< Floats on water and bobs if you step on it.
	{"FF_NORETURN",FF_NORETURN},               ///< Used with ::FF_CRUMBLE. Will not return to its original position after falling.
	{"FF_CRUMBLE",FF_CRUMBLE},                 ///< Falls 2 seconds after being stepped on, and randomly brings all touching crumbling 3dfloors down with it, providing their master sectors share the same tag (allows crumble platforms above or below, to also exist).
	{"FF_SHATTERBOTTOM",FF_SHATTERBOTTOM},     ///< Used with ::FF_BUSTUP. Like FF_SHATTER, but only breaks from the bottom. Good for springing up through rubble.
	{"FF_MARIO",FF_MARIO},                     ///< Acts like a question block when hit from underneath. Goodie spawned at top is determined by master sector.
	{"FF_BUSTUP",FF_BUSTUP},                   ///< You can spin through/punch this block and it will crumble!
	{"FF_QUICKSAND",FF_QUICKSAND},             ///< Quicksand!
	{"FF_PLATFORM",FF_PLATFORM},               ///< You can jump up through this to the top.
	{"FF_REVERSEPLATFORM",FF_REVERSEPLATFORM}, ///< A fall-through floor in normal gravity, a platform in reverse gravity.
	{"FF_INTANGABLEFLATS",FF_INTANGABLEFLATS}, ///< Both flats are intangable, but the sides are still solid.
	{"FF_SHATTER",FF_SHATTER},                 ///< Used with ::FF_BUSTUP. Thinks everyone's Knuckles.
	{"FF_SPINBUST",FF_SPINBUST},               ///< Used with ::FF_BUSTUP. Jump or fall onto it while curled in a ball.
	{"FF_ONLYKNUX",FF_ONLYKNUX},               ///< Used with ::FF_BUSTUP. Only Knuckles can break this rock.
	{"FF_RIPPLE",FF_RIPPLE},                   ///< Ripple the flats
	{"FF_COLORMAPONLY",FF_COLORMAPONLY},       ///< Only copy the colormap, not the lightlevel
	{"FF_GOOWATER",FF_GOOWATER},               ///< Used with ::FF_SWIMMABLE. Makes thick bouncey goop.

#ifdef HAVE_LUA_SEGS
	// Node flags
	{"NF_SUBSECTOR",NF_SUBSECTOR}, // Indicate a leaf.
#endif

	// Angles
	{"ANG1",ANG1},
	{"ANG2",ANG2},
	{"ANG10",ANG10},
	{"ANG15",ANG15},
	{"ANG20",ANG20},
	{"ANG30",ANG30},
	{"ANG60",ANG60},
	{"ANG64h",ANG64h},
	{"ANG105",ANG105},
	{"ANG210",ANG210},
	{"ANG255",ANG255},
	{"ANG340",ANG340},
	{"ANG350",ANG350},
	{"ANGLE_11hh",ANGLE_11hh},
	{"ANGLE_22h",ANGLE_22h},
	{"ANGLE_45",ANGLE_45},
	{"ANGLE_67h",ANGLE_67h},
	{"ANGLE_90",ANGLE_90},
	{"ANGLE_112h",ANGLE_112h},
	{"ANGLE_135",ANGLE_135},
	{"ANGLE_157h",ANGLE_157h},
	{"ANGLE_180",ANGLE_180},
	{"ANGLE_202h",ANGLE_202h},
	{"ANGLE_225",ANGLE_225},
	{"ANGLE_247h",ANGLE_247h},
	{"ANGLE_270",ANGLE_270},
	{"ANGLE_292h",ANGLE_292h},
	{"ANGLE_315",ANGLE_315},
	{"ANGLE_337h",ANGLE_337h},
	{"ANGLE_MAX",ANGLE_MAX},

	// P_Chase directions (dirtype_t)
	{"DI_NODIR",DI_NODIR},
	{"DI_EAST",DI_EAST},
	{"DI_NORTHEAST",DI_NORTHEAST},
	{"DI_NORTH",DI_NORTH},
	{"DI_NORTHWEST",DI_NORTHWEST},
	{"DI_WEST",DI_WEST},
	{"DI_SOUTHWEST",DI_SOUTHWEST},
	{"DI_SOUTH",DI_SOUTH},
	{"DI_SOUTHEAST",DI_SOUTHEAST},
	{"NUMDIRS",NUMDIRS},

	// Buttons (ticcmd_t)
	{"BT_WEAPONMASK",BT_WEAPONMASK}, //our first four bits.
	{"BT_WEAPONNEXT",BT_WEAPONNEXT},
	{"BT_WEAPONPREV",BT_WEAPONPREV},
	{"BT_ATTACK",BT_ATTACK}, // shoot rings
	{"BT_USE",BT_USE}, // spin
	{"BT_CAMLEFT",BT_CAMLEFT}, // turn camera left
	{"BT_CAMRIGHT",BT_CAMRIGHT}, // turn camera right
	{"BT_TOSSFLAG",BT_TOSSFLAG},
	{"BT_JUMP",BT_JUMP},
	{"BT_FIRENORMAL",BT_FIRENORMAL}, // Fire a normal ring no matter what
	{"BT_CUSTOM1",BT_CUSTOM1}, // Lua customizable
	{"BT_CUSTOM2",BT_CUSTOM2}, // Lua customizable
	{"BT_CUSTOM3",BT_CUSTOM3}, // Lua customizable

	// cvflags_t
	{"CV_SAVE",CV_SAVE},
	{"CV_CALL",CV_CALL},
	{"CV_NETVAR",CV_NETVAR},
	{"CV_NOINIT",CV_NOINIT},
	{"CV_FLOAT",CV_FLOAT},
	{"CV_NOTINNET",CV_NOTINNET},
	{"CV_MODIFIED",CV_MODIFIED},
	{"CV_SHOWMODIF",CV_SHOWMODIF},
	{"CV_SHOWMODIFONETIME",CV_SHOWMODIFONETIME},
	{"CV_NOSHOWHELP",CV_NOSHOWHELP},
	{"CV_HIDEN",CV_HIDEN},
	{"CV_HIDDEN",CV_HIDEN},
	{"CV_CHEAT",CV_CHEAT},

	// v_video flags
	{"V_NOSCALEPATCH",V_NOSCALEPATCH},
	{"V_SMALLSCALEPATCH",V_SMALLSCALEPATCH},
	{"V_MEDSCALEPATCH",V_MEDSCALEPATCH},
	{"V_6WIDTHSPACE",V_6WIDTHSPACE},
	{"V_OLDSPACING",V_OLDSPACING},
	{"V_MONOSPACE",V_MONOSPACE},
	{"V_PURPLEMAP",V_PURPLEMAP},
	{"V_YELLOWMAP",V_YELLOWMAP},
	{"V_GREENMAP",V_GREENMAP},
	{"V_BLUEMAP",V_BLUEMAP},
	{"V_REDMAP",V_REDMAP},
	{"V_GRAYMAP",V_GRAYMAP},
	{"V_ORANGEMAP",V_ORANGEMAP},
	{"V_TRANSLUCENT",V_TRANSLUCENT},
	{"V_10TRANS",V_10TRANS},
	{"V_20TRANS",V_20TRANS},
	{"V_30TRANS",V_30TRANS},
	{"V_40TRANS",V_40TRANS},
	{"V_50TRANS",V_TRANSLUCENT}, // alias
	{"V_60TRANS",V_60TRANS},
	{"V_70TRANS",V_70TRANS},
	{"V_80TRANS",V_80TRANS},
	{"V_90TRANS",V_90TRANS},
	{"V_STATIC",V_STATIC},
	{"V_HUDTRANSHALF",V_HUDTRANSHALF},
	{"V_HUDTRANS",V_HUDTRANS},
	{"V_HUDTRANSDOUBLE",V_HUDTRANSDOUBLE},
	{"V_AUTOFADEOUT",V_AUTOFADEOUT},
	{"V_RETURN8",V_RETURN8},
	{"V_OFFSET",V_OFFSET},
	{"V_ALLOWLOWERCASE",V_ALLOWLOWERCASE},
	{"V_FLIP",V_FLIP},
	{"V_SNAPTOTOP",V_SNAPTOTOP},
	{"V_SNAPTOBOTTOM",V_SNAPTOBOTTOM},
	{"V_SNAPTOLEFT",V_SNAPTOLEFT},
	{"V_SNAPTORIGHT",V_SNAPTORIGHT},
	{"V_WRAPX",V_WRAPX},
	{"V_WRAPY",V_WRAPY},
	{"V_NOSCALESTART",V_NOSCALESTART},
	{"V_SPLITSCREEN",V_SPLITSCREEN},

	{"V_PARAMMASK",V_PARAMMASK},
	{"V_SCALEPATCHMASK",V_SCALEPATCHMASK},
	{"V_SPACINGMASK",V_SPACINGMASK},
	{"V_CHARCOLORMASK",V_CHARCOLORMASK},
	{"V_ALPHAMASK",V_ALPHAMASK},

	{"V_CHARCOLORSHIFT",V_CHARCOLORSHIFT},
	{"V_ALPHASHIFT",V_ALPHASHIFT},
#endif

	{NULL,0}
};

static mobjtype_t get_mobjtype(const char *word)
{ // Returns the vlaue of MT_ enumerations
	mobjtype_t i;
	if (*word >= '0' && *word <= '9')
		return atoi(word);
	if (fastncmp("MT_",word,3))
		word += 3; // take off the MT_
	for (i = 0; i < NUMMOBJFREESLOTS; i++) {
		if (!FREE_MOBJS[i])
			break;
		if (fastcmp(word, FREE_MOBJS[i]))
			return MT_FIRSTFREESLOT+i;
	}
	for (i = 0; i < MT_FIRSTFREESLOT; i++)
		if (fastcmp(word, MOBJTYPE_LIST[i]+3))
			return i;
	deh_warning("Couldn't find mobjtype named 'MT_%s'",word);
	return MT_NULL;
}

static statenum_t get_state(const char *word)
{ // Returns the value of S_ enumerations
	statenum_t i;
	if (*word >= '0' && *word <= '9')
		return atoi(word);
	if (fastncmp("S_",word,2))
		word += 2; // take off the S_
	for (i = 0; i < NUMSTATEFREESLOTS; i++) {
		if (!FREE_STATES[i])
			break;
		if (fastcmp(word, FREE_STATES[i]))
			return S_FIRSTFREESLOT+i;
	}
	for (i = 0; i < S_FIRSTFREESLOT; i++)
		if (fastcmp(word, STATE_LIST[i]+2))
			return i;
	deh_warning("Couldn't find state named 'S_%s'",word);
	return S_NULL;
}

static spritenum_t get_sprite(const char *word)
{ // Returns the value of SPR_ enumerations
	spritenum_t i;
	if (*word >= '0' && *word <= '9')
		return atoi(word);
	if (fastncmp("SPR_",word,4))
		word += 4; // take off the SPR_
	for (i = 0; i < NUMSPRITES; i++)
		if (!sprnames[i][4] && memcmp(word,sprnames[i],4)==0)
			return i;
	deh_warning("Couldn't find sprite named 'SPR_%s'",word);
	return SPR_NULL;
}

static playersprite_t get_sprite2(const char *word)
{ // Returns the value of SPR2_ enumerations
	playersprite_t i;
	if (*word >= '0' && *word <= '9')
		return atoi(word);
	if (fastncmp("SPR2_",word,5))
		word += 5; // take off the SPR2_
	for (i = 0; i < NUMPLAYERSPRITES; i++)
		if (!spr2names[i][4] && memcmp(word,spr2names[i],4)==0)
			return i;
	deh_warning("Couldn't find sprite named 'SPR2_%s'",word);
	return SPR2_STND;
}

static sfxenum_t get_sfx(const char *word)
{ // Returns the value of SFX_ enumerations
	sfxenum_t i;
	if (*word >= '0' && *word <= '9')
		return atoi(word);
	if (fastncmp("SFX_",word,4))
		word += 4; // take off the SFX_
	else if (fastncmp("DS",word,2))
		word += 2; // take off the DS
	for (i = 0; i < NUMSFX; i++)
		if (S_sfx[i].name && fasticmp(word, S_sfx[i].name))
			return i;
	deh_warning("Couldn't find sfx named 'SFX_%s'",word);
	return sfx_None;
}

#ifdef MUSICSLOT_COMPATIBILITY
static UINT16 get_mus(const char *word, UINT8 dehacked_mode)
{ // Returns the value of MUS_ enumerations
	UINT16 i;
	char lumptmp[4];

	if (*word >= '0' && *word <= '9')
		return atoi(word);
	if (!word[2] && toupper(word[0]) >= 'A' && toupper(word[0]) <= 'Z')
		return (UINT16)M_MapNumber(word[0], word[1]);

	if (fastncmp("MUS_",word,4))
		word += 4; // take off the MUS_
	else if (fastncmp("O_",word,2) || fastncmp("D_",word,2))
		word += 2; // take off the O_ or D_

	strncpy(lumptmp, word, 4);
	lumptmp[3] = 0;
	if (fasticmp("MAP",lumptmp))
	{
		word += 3;
		if (toupper(word[0]) >= 'A' && toupper(word[0]) <= 'Z')
			return (UINT16)M_MapNumber(word[0], word[1]);
		else if ((i = atoi(word)))
			return i;

		word -= 3;
		if (dehacked_mode)
			deh_warning("Couldn't find music named 'MUS_%s'",word);
		return 0;
	}
	for (i = 0; compat_special_music_slots[i][0]; ++i)
		if (fasticmp(word, compat_special_music_slots[i]))
			return i + 1036;
	if (dehacked_mode)
		deh_warning("Couldn't find music named 'MUS_%s'",word);
	return 0;
}
#endif

static hudnum_t get_huditem(const char *word)
{ // Returns the value of HUD_ enumerations
	hudnum_t i;
	if (*word >= '0' && *word <= '9')
		return atoi(word);
	if (fastncmp("HUD_",word,4))
		word += 4; // take off the HUD_
	for (i = 0; i < NUMHUDITEMS; i++)
		if (fastcmp(word, HUDITEMS_LIST[i]))
			return i;
	deh_warning("Couldn't find huditem named 'HUD_%s'",word);
	return HUD_LIVESNAME;
}

#ifndef HAVE_BLUA
static powertype_t get_power(const char *word)
{ // Returns the vlaue of pw_ enumerations
	powertype_t i;
	if (*word >= '0' && *word <= '9')
		return atoi(word);
	if (fastncmp("PW_",word,3))
		word += 3; // take off the pw_
	for (i = 0; i < NUMPOWERS; i++)
		if (fastcmp(word, POWERS_LIST[i]))
			return i;
	deh_warning("Couldn't find power named 'pw_%s'",word);
	return pw_invulnerability;
}

/// \todo Make ANY of this completely over-the-top math craziness obey the order of operations.
static fixed_t op_mul(fixed_t a, fixed_t b) { return a*b; }
static fixed_t op_div(fixed_t a, fixed_t b) { return a/b; }
static fixed_t op_add(fixed_t a, fixed_t b) { return a+b; }
static fixed_t op_sub(fixed_t a, fixed_t b) { return a-b; }
static fixed_t op_or(fixed_t a, fixed_t b) { return a|b; }
static fixed_t op_and(fixed_t a, fixed_t b) { return a&b; }
static fixed_t op_lshift(fixed_t a, fixed_t b) { return a<<b; }
static fixed_t op_rshift(fixed_t a, fixed_t b) { return a>>b; }

struct {
	const char c;
	fixed_t (*v)(fixed_t,fixed_t);
} OPERATIONS[] = {
	{'*',op_mul},
	{'/',op_div},
	{'+',op_add},
	{'-',op_sub},
	{'|',op_or},
	{'&',op_and},
	{'<',op_lshift},
	{'>',op_rshift},
	{0,NULL}
};

// Returns the full word, cut at the first symbol or whitespace
static char *read_word(const char *line)
{
	// Part 1: You got the start of the word, now find the end.
  const char *p;
	INT32 i;
	for (p = line+1; *p; p++) {
		if (*p == ' ' || *p == '\t')
			break;
		for (i = 0; OPERATIONS[i].c; i++)
			if (*p == OPERATIONS[i].c) {
				i = -1;
				break;
			}
		if (i == -1)
			break;
	}

	// Part 2: Make a copy of the word and return it.
	{
		size_t len = (p-line);
		char *word = malloc(len+1);
		M_Memcpy(word,line,len);
		word[len] = '\0';
		return word;
	}
}

static INT32 operation_pad(const char **word)
{ // Brings word the next operation and returns the operation number.
	INT32 i;
	for (; **word; (*word)++) {
		if (**word == ' ' || **word == '\t')
			continue;
		for (i = 0; OPERATIONS[i].c; i++)
			if (**word == OPERATIONS[i].c)
			{
				if ((**word == '<' && *(*word+1) == '<') || (**word == '>' && *(*word+1) == '>')) (*word)++; // These operations are two characters long.
				else if (**word == '<' || **word == '>') continue; // ... do not accept one character long.
				(*word)++;
				return i;
			}
		deh_warning("Unknown operation '%c'",**word);
		return -1;
	}
	return -1;
}

static void const_warning(const char *type, const char *word)
{
	deh_warning("Couldn't find %s named '%s'",type,word);
}

static fixed_t find_const(const char **rword)
{ // Finds the value of constants and returns it, bringing word to the next operation.
	INT32 i;
	fixed_t r;
	char *word = read_word(*rword);
	*rword += strlen(word);
	if ((*word >= '0' && *word <= '9') || *word == '-') { // Parse a number
		r = atoi(word);
		free(word);
		return r;
	}
	if (!*(word+1) && // Turn a single A-z symbol into numbers, like sprite frames.
	 ((*word >= 'A' && *word <= 'Z') || (*word >= 'a' && *word <= 'z'))) {
		r = R_Char2Frame(*word);
		free(word);
		return r;
	}
	if (fastncmp("MF_", word, 3)) {
		char *p = word+3;
		for (i = 0; MOBJFLAG_LIST[i]; i++)
			if (fastcmp(p, MOBJFLAG_LIST[i])) {
				free(word);
				return (1<<i);
			}

		// Not found error
		const_warning("mobj flag",word);
		free(word);
		return 0;
	}
	else if (fastncmp("MF2_", word, 4)) {
		char *p = word+4;
		for (i = 0; MOBJFLAG2_LIST[i]; i++)
			if (fastcmp(p, MOBJFLAG2_LIST[i])) {
				free(word);
				return (1<<i);
			}

		// Not found error
		const_warning("mobj flag2",word);
		free(word);
		return 0;
	}
	else if (fastncmp("MFE_", word, 4)) {
		char *p = word+4;
		for (i = 0; MOBJEFLAG_LIST[i]; i++)
			if (fastcmp(p, MOBJEFLAG_LIST[i])) {
				free(word);
				return (1<<i);
			}

		// Not found error
		const_warning("mobj eflag",word);
		free(word);
		return 0;
	}
	else if (fastncmp("PF_", word, 3)) {
		char *p = word+3;
		for (i = 0; PLAYERFLAG_LIST[i]; i++)
			if (fastcmp(p, PLAYERFLAG_LIST[i])) {
				free(word);
				return (1<<i);
			}
		if (fastcmp(p, "FULLSTASIS"))
			return PF_FULLSTASIS;

		// Not found error
		const_warning("player flag",word);
		free(word);
		return 0;
	}
	else if (fastncmp("S_",word,2)) {
		r = get_state(word);
		free(word);
		return r;
	}
	else if (fastncmp("MT_",word,3)) {
		r = get_mobjtype(word);
		free(word);
		return r;
	}
	else if (fastncmp("SPR_",word,4)) {
		r = get_sprite(word);
		free(word);
		return r;
	}
	else if (fastncmp("SFX_",word,4) || fastncmp("DS",word,2)) {
		r = get_sfx(word);
		free(word);
		return r;
	}
#ifdef MUSICSLOT_COMPATIBILITY
	else if (fastncmp("MUS_",word,4) || fastncmp("O_",word,2)) {
		r = get_mus(word, true);
		free(word);
		return r;
	}
#endif
	else if (fastncmp("PW_",word,3)) {
		r = get_power(word);
		free(word);
		return r;
	}
	else if (fastncmp("HUD_",word,4)) {
		r = get_huditem(word);
		free(word);
		return r;
	}
	else if (fastncmp("SKINCOLOR_",word,10)) {
		char *p = word+10;
		for (i = 0; i < MAXTRANSLATIONS; i++)
			if (fastcmp(p, COLOR_ENUMS[i])) {
				free(word);
				return i;
			}
		const_warning("color",word);
		free(word);
		return 0;
	}
	for (i = 0; INT_CONST[i].n; i++)
		if (fastcmp(word,INT_CONST[i].n)) {
			free(word);
			return INT_CONST[i].v;
		}

	// Not found error.
	const_warning("constant",word);
	free(word);
	return 0;
}
#endif

// Loops through every constant and operation in word and performs its calculations, returning the final value.
fixed_t get_number(const char *word)
{
#ifdef HAVE_BLUA
	return LUA_EvalMath(word);
#else
	// DESPERATELY NEEDED: Order of operations support! :x
	fixed_t i = find_const(&word);
	INT32 o;
	while(*word) {
		o = operation_pad(&word);
		if (o != -1)
			i = OPERATIONS[o].v(i,find_const(&word));
		else
			break;
	}
	return i;
#endif
}

void FUNCMATH DEH_Check(void)
{
#if defined(_DEBUG) || defined(PARANOIA)
	const size_t dehstates = sizeof(STATE_LIST)/sizeof(const char*);
	const size_t dehmobjs  = sizeof(MOBJTYPE_LIST)/sizeof(const char*);
	const size_t dehpowers = sizeof(POWERS_LIST)/sizeof(const char*);
	const size_t dehcolors = sizeof(COLOR_ENUMS)/sizeof(const char*);

	if (dehstates != S_FIRSTFREESLOT)
		I_Error("You forgot to update the Dehacked states list, you dolt!\n(%d states defined, versus %s in the Dehacked list)\n", S_FIRSTFREESLOT, sizeu1(dehstates));

	if (dehmobjs != MT_FIRSTFREESLOT)
		I_Error("You forgot to update the Dehacked mobjtype list, you dolt!\n(%d mobj types defined, versus %s in the Dehacked list)\n", MT_FIRSTFREESLOT, sizeu1(dehmobjs));

	if (dehpowers != NUMPOWERS)
		I_Error("You forgot to update the Dehacked powers list, you dolt!\n(%d powers defined, versus %s in the Dehacked list)\n", NUMPOWERS, sizeu1(dehpowers));

	if (dehcolors != MAXTRANSLATIONS)
		I_Error("You forgot to update the Dehacked colors list, you dolt!\n(%d colors defined, versus %s in the Dehacked list)\n", MAXTRANSLATIONS, sizeu1(dehcolors));
#endif
}

#ifdef HAVE_BLUA
#include "lua_script.h"
#include "lua_libs.h"

// freeslot takes a name (string only!)
// and allocates it to the appropriate free slot.
// Returns the slot number allocated for it or nil if failed.
// ex. freeslot("MT_MYTHING","S_MYSTATE1","S_MYSTATE2")
// TODO: Error checking! @.@; There's currently no way to know which ones failed and why!
//
static inline int lib_freeslot(lua_State *L)
{
	int n = lua_gettop(L);
	int r = 0; // args returned
	char *s, *type,*word;

	if (!lua_lumploading)
		return luaL_error(L, "This function cannot be called from within a hook or coroutine!");

	while (n-- > 0)
	{
		s = Z_StrDup(luaL_checkstring(L,1));
		type = strtok(s, "_");
		if (type)
			strupr(type);
		else {
			Z_Free(s);
			return luaL_error(L, "Unknown enum type in '%s'\n", luaL_checkstring(L, 1));
		}

		word = strtok(NULL, "\n");
		if (word)
			strupr(word);
		else {
			Z_Free(s);
			return luaL_error(L, "Missing enum name in '%s'\n", luaL_checkstring(L, 1));
		}
		if (fastcmp(type, "SFX")) {
			sfxenum_t sfx;
			strlwr(word);
			CONS_Printf("Sound sfx_%s allocated.\n",word);
			sfx = S_AddSoundFx(word, false, 0, false);
			if (sfx != sfx_None) {
				lua_pushinteger(L, sfx);
				r++;
			} else
				CONS_Alert(CONS_WARNING, "Ran out of free SFX slots!\n");
		}
		else if (fastcmp(type, "SPR"))
		{
			char wad;
			spritenum_t j;
			lua_getfield(L, LUA_REGISTRYINDEX, "WAD");
			wad = (char)lua_tointeger(L, -1);
			lua_pop(L, 1);
			for (j = SPR_FIRSTFREESLOT; j <= SPR_LASTFREESLOT; j++)
			{
				if (used_spr[(j-SPR_FIRSTFREESLOT)/8] & (1<<(j%8)))
				{
					if (!sprnames[j][4] && memcmp(sprnames[j],word,4)==0)
						sprnames[j][4] = wad;
					continue; // Already allocated, next.
				}
				// Found a free slot!
				CONS_Printf("Sprite SPR_%s allocated.\n",word);
				strncpy(sprnames[j],word,4);
				//sprnames[j][4] = 0;
				used_spr[(j-SPR_FIRSTFREESLOT)/8] |= 1<<(j%8); // Okay, this sprite slot has been named now.
				lua_pushinteger(L, j);
				r++;
				break;
			}
			if (j > SPR_LASTFREESLOT)
				CONS_Alert(CONS_WARNING, "Ran out of free sprite slots!\n");
		}
		else if (fastcmp(type, "S"))
		{
			statenum_t i;
			for (i = 0; i < NUMSTATEFREESLOTS; i++)
				if (!FREE_STATES[i]) {
					CONS_Printf("State S_%s allocated.\n",word);
					FREE_STATES[i] = Z_Malloc(strlen(word)+1, PU_STATIC, NULL);
					strcpy(FREE_STATES[i],word);
					lua_pushinteger(L, i);
					r++;
					break;
				}
			if (i == NUMSTATEFREESLOTS)
				CONS_Alert(CONS_WARNING, "Ran out of free State slots!\n");
		}
		else if (fastcmp(type, "MT"))
		{
			mobjtype_t i;
			for (i = 0; i < NUMMOBJFREESLOTS; i++)
				if (!FREE_MOBJS[i]) {
					CONS_Printf("MobjType MT_%s allocated.\n",word);
					FREE_MOBJS[i] = Z_Malloc(strlen(word)+1, PU_STATIC, NULL);
					strcpy(FREE_MOBJS[i],word);
					lua_pushinteger(L, i);
					r++;
					break;
				}
			if (i == NUMMOBJFREESLOTS)
				CONS_Alert(CONS_WARNING, "Ran out of free MobjType slots!\n");
		}
		else if (fastcmp(type, "SPR2"))
		{
			// Search if we already have an SPR2 by that name...
			playersprite_t i;
			for (i = SPR2_FIRSTFREESLOT; i < free_spr2; i++)
				if (memcmp(spr2names[i],word,4) == 0)
					break;
			// We don't, so allocate a new one.
			if (i >= free_spr2) {
				if (free_spr2 < NUMPLAYERSPRITES)
				{
					CONS_Printf("Sprite SPR2_%s allocated.\n",word);
					strncpy(spr2names[free_spr2],word,4);
					spr2names[free_spr2++][4] = 0;
				} else
					CONS_Alert(CONS_WARNING, "Ran out of free SPR2 slots!\n");
			}
			r++;
		}
		Z_Free(s);
		lua_remove(L, 1);
		continue;
	}
	return r;
}

// Wrapper for ALL A_Action functions.
// Upvalue: actionf_t to represent
// Arguments: mobj_t actor, int var1, int var2
static inline int lib_action(lua_State *L)
{
	actionf_t *action = lua_touserdata(L,lua_upvalueindex(1));
	mobj_t *actor = *((mobj_t **)luaL_checkudata(L,1,META_MOBJ));
	var1 = (INT32)luaL_optinteger(L,2,0);
	var2 = (INT32)luaL_optinteger(L,3,0);
	if (!actor)
		return LUA_ErrInvalid(L, "mobj_t");
	action->acp1(actor);
	return 0;
}

// Hardcoded A_Action name to call for super() or NULL if super() would be invalid.
// Set in lua_infolib.
const char *superactions[MAXRECURSION];
UINT8 superstack = 0;

static int lib_dummysuper(lua_State *L)
{
	return luaL_error(L, "Can't call super() outside of hardcode-replacing A_Action functions being called by state changes!"); // convoluted, I know. @_@;;
}

static inline int lib_getenum(lua_State *L)
{
	const char *word, *p;
	fixed_t i;
	boolean mathlib = lua_toboolean(L, lua_upvalueindex(1));
	if (lua_type(L,2) != LUA_TSTRING)
		return 0;
	word = lua_tostring(L,2);
	if (strlen(word) == 1) { // Assume sprite frame if length 1.
		if (*word >= 'A' && *word <= '~')
		{
			lua_pushinteger(L, *word-'A');
			return 1;
		}
		if (mathlib) return luaL_error(L, "constant '%s' could not be parsed.\n", word);
		return 0;
	}
	else if (fastncmp("MF_", word, 3)) {
		p = word+3;
		for (i = 0; MOBJFLAG_LIST[i]; i++)
			if (fastcmp(p, MOBJFLAG_LIST[i])) {
				lua_pushinteger(L, ((lua_Integer)1<<i));
				return 1;
			}
		if (mathlib) return luaL_error(L, "mobjflag '%s' could not be found.\n", word);
		return 0;
	}
	else if (fastncmp("MF2_", word, 4)) {
		p = word+4;
		for (i = 0; MOBJFLAG2_LIST[i]; i++)
			if (fastcmp(p, MOBJFLAG2_LIST[i])) {
				lua_pushinteger(L, ((lua_Integer)1<<i));
				return 1;
			}
		if (mathlib) return luaL_error(L, "mobjflag2 '%s' could not be found.\n", word);
		return 0;
	}
	else if (fastncmp("MFE_", word, 4)) {
		p = word+4;
		for (i = 0; MOBJEFLAG_LIST[i]; i++)
			if (fastcmp(p, MOBJEFLAG_LIST[i])) {
				lua_pushinteger(L, ((lua_Integer)1<<i));
				return 1;
			}
		if (mathlib) return luaL_error(L, "mobjeflag '%s' could not be found.\n", word);
		return 0;
	}
	else if (fastncmp("MTF_", word, 4)) {
		p = word+4;
		for (i = 0; i < 4; i++)
			if (MAPTHINGFLAG_LIST[i] && fastcmp(p, MAPTHINGFLAG_LIST[i])) {
				lua_pushinteger(L, ((lua_Integer)1<<i));
				return 1;
			}
		if (mathlib) return luaL_error(L, "mapthingflag '%s' could not be found.\n", word);
		return 0;
	}
	else if (fastncmp("PF_", word, 3)) {
		p = word+3;
		for (i = 0; PLAYERFLAG_LIST[i]; i++)
			if (fastcmp(p, PLAYERFLAG_LIST[i])) {
				lua_pushinteger(L, ((lua_Integer)1<<i));
				return 1;
			}
		if (fastcmp(p, "FULLSTASIS"))
		{
			lua_pushinteger(L, (lua_Integer)PF_FULLSTASIS);
			return 1;
		}
		if (mathlib) return luaL_error(L, "playerflag '%s' could not be found.\n", word);
		return 0;
	}
	else if (fastncmp("ML_", word, 3)) {
		p = word+3;
		for (i = 0; i < 16; i++)
			if (ML_LIST[i] && fastcmp(p, ML_LIST[i])) {
				lua_pushinteger(L, ((lua_Integer)1<<i));
				return 1;
			}
		if (fastcmp(p, "NETONLY"))
		{
			lua_pushinteger(L, (lua_Integer)ML_NETONLY);
			return 1;
		}
		if (mathlib) return luaL_error(L, "linedef flag '%s' could not be found.\n", word);
		return 0;
	}
	else if (fastncmp("S_",word,2)) {
		p = word+2;
		for (i = 0; i < NUMSTATEFREESLOTS; i++) {
			if (!FREE_STATES[i])
				break;
			if (fastcmp(p, FREE_STATES[i])) {
				lua_pushinteger(L, S_FIRSTFREESLOT+i);
				return 1;
			}
		}
		for (i = 0; i < S_FIRSTFREESLOT; i++)
			if (fastcmp(p, STATE_LIST[i]+2)) {
				lua_pushinteger(L, i);
				return 1;
			}
		return luaL_error(L, "state '%s' does not exist.\n", word);
	}
	else if (fastncmp("MT_",word,3)) {
		p = word+3;
		for (i = 0; i < NUMMOBJFREESLOTS; i++) {
			if (!FREE_MOBJS[i])
				break;
			if (fastcmp(p, FREE_MOBJS[i])) {
				lua_pushinteger(L, MT_FIRSTFREESLOT+i);
				return 1;
			}
		}
		for (i = 0; i < MT_FIRSTFREESLOT; i++)
			if (fastcmp(p, MOBJTYPE_LIST[i]+3)) {
				lua_pushinteger(L, i);
				return 1;
			}
		return luaL_error(L, "mobjtype '%s' does not exist.\n", word);
	}
	else if (fastncmp("SPR_",word,4)) {
		p = word+4;
		for (i = 0; i < NUMSPRITES; i++)
			if (!sprnames[i][4] && fastncmp(p,sprnames[i],4)) {
				lua_pushinteger(L, i);
				return 1;
			}
		if (mathlib) return luaL_error(L, "sprite '%s' could not be found.\n", word);
		return 0;
	}
	else if (fastncmp("SPR2_",word,5)) {
		p = word+5;
		for (i = 0; i < (fixed_t)free_spr2; i++)
			if (!spr2names[i][4])
			{
				// special 3-char cases, e.g. SPR2_RUN
				// the spr2names entry will have "_" on the end, as in "RUN_"
				if (spr2names[i][3] == '_' && !p[3]) {
					if (fastncmp(p,spr2names[i],3)) {
						lua_pushinteger(L, i);
						return 1;
					}
				}
				else if (fastncmp(p,spr2names[i],4)) {
					lua_pushinteger(L, i);
					return 1;
				}
			}
		if (mathlib) return luaL_error(L, "player sprite '%s' could not be found.\n", word);
		return 0;
	}
	else if (!mathlib && fastncmp("sfx_",word,4)) {
		p = word+4;
		for (i = 0; i < NUMSFX; i++)
			if (S_sfx[i].name && fastcmp(p, S_sfx[i].name)) {
				lua_pushinteger(L, i);
				return 1;
			}
		return 0;
	}
	else if (mathlib && fastncmp("SFX_",word,4)) { // SOCs are ALL CAPS!
		p = word+4;
		for (i = 0; i < NUMSFX; i++)
			if (S_sfx[i].name && fasticmp(p, S_sfx[i].name)) {
				lua_pushinteger(L, i);
				return 1;
			}
		return luaL_error(L, "sfx '%s' could not be found.\n", word);
	}
	else if (mathlib && fastncmp("DS",word,2)) {
		p = word+2;
		for (i = 0; i < NUMSFX; i++)
			if (S_sfx[i].name && fasticmp(p, S_sfx[i].name)) {
				lua_pushinteger(L, i);
				return 1;
			}
		if (mathlib) return luaL_error(L, "sfx '%s' could not be found.\n", word);
		return 0;
	}
#ifdef MUSICSLOT_COMPATIBILITY
	else if (!mathlib && fastncmp("mus_",word,4)) {
		p = word+4;
		if ((i = get_mus(p, false)) == 0)
			return 0;
		lua_pushinteger(L, i);
		return 1;
	}
	else if (mathlib && fastncmp("MUS_",word,4)) { // SOCs are ALL CAPS!
		p = word+4;
		if ((i = get_mus(p, false)) == 0)
			return luaL_error(L, "music '%s' could not be found.\n", word);
		lua_pushinteger(L, i);
		return 1;
	}
	else if (mathlib && (fastncmp("O_",word,2) || fastncmp("D_",word,2))) {
		p = word+2;
		if ((i = get_mus(p, false)) == 0)
			return luaL_error(L, "music '%s' could not be found.\n", word);
		lua_pushinteger(L, i);
		return 1;
	}
#endif
	else if (!mathlib && fastncmp("pw_",word,3)) {
		p = word+3;
		for (i = 0; i < NUMPOWERS; i++)
			if (fasticmp(p, POWERS_LIST[i])) {
				lua_pushinteger(L, i);
				return 1;
			}
		return 0;
	}
	else if (mathlib && fastncmp("PW_",word,3)) { // SOCs are ALL CAPS!
		p = word+3;
		for (i = 0; i < NUMPOWERS; i++)
			if (fastcmp(p, POWERS_LIST[i])) {
				lua_pushinteger(L, i);
				return 1;
			}
		return luaL_error(L, "power '%s' could not be found.\n", word);
	}
	else if (fastncmp("HUD_",word,4)) {
		p = word+4;
		for (i = 0; i < NUMHUDITEMS; i++)
			if (fastcmp(p, HUDITEMS_LIST[i])) {
				lua_pushinteger(L, i);
				return 1;
			}
		if (mathlib) return luaL_error(L, "huditem '%s' could not be found.\n", word);
		return 0;
	}
	else if (fastncmp("SKINCOLOR_",word,10)) {
		p = word+10;
		for (i = 0; i < MAXTRANSLATIONS; i++)
			if (fastcmp(p, COLOR_ENUMS[i])) {
				lua_pushinteger(L, i);
				return 1;
			}
		if (mathlib) return luaL_error(L, "skincolor '%s' could not be found.\n", word);
		return 0;
	}
	else if (!mathlib && fastncmp("A_",word,2)) {
		char *caps;
		// Try to get a Lua action first.
		/// \todo Push a closure that sets superactions[] and superstack.
		lua_getfield(L, LUA_REGISTRYINDEX, LREG_ACTIONS);
		// actions are stored in all uppercase.
		caps = Z_StrDup(word);
		strupr(caps);
		lua_getfield(L, -1, caps);
		Z_Free(caps);
		if (!lua_isnil(L, -1))
			return 1; // Success! :D That was easy.
		// Welp, that failed.
		lua_pop(L, 2); // pop nil and LREG_ACTIONS

		// Hardcoded actions as callable Lua functions!
		// Retrieving them from this metatable allows them to be case-insensitive!
		for (i = 0; actionpointers[i].name; i++)
			if (fasticmp(word, actionpointers[i].name)) {
				// push lib_action as a C closure with the actionf_t* as an upvalue.
				lua_pushlightuserdata(L, &actionpointers[i].action);
				lua_pushcclosure(L, lib_action, 1);
				return 1;
			}
		return 0;
	}
	else if (!mathlib && fastcmp("super",word))
	{
		if (!superstack)
		{
			lua_pushcfunction(L, lib_dummysuper);
			return 1;
		}
		for (i = 0; actionpointers[i].name; i++)
			if (fasticmp(superactions[superstack-1], actionpointers[i].name)) {
				lua_pushlightuserdata(L, &actionpointers[i].action);
				lua_pushcclosure(L, lib_action, 1);
				return 1;
			}
		return 0;
	}

	for (i = 0; INT_CONST[i].n; i++)
		if (fastcmp(word,INT_CONST[i].n)) {
			lua_pushinteger(L, INT_CONST[i].v);
			return 1;
		}

	if (mathlib) return luaL_error(L, "constant '%s' could not be parsed.\n", word);

	// DYNAMIC variables too!!
	// Try not to add anything that would break netgames or timeattack replays here.
	// You know, like consoleplayer, displayplayer, secondarydisplayplayer, or gametime.
	if (fastcmp(word,"gamemap")) {
		lua_pushinteger(L, gamemap);
		return 1;
	} else if (fastcmp(word,"maptol")) {
		lua_pushinteger(L, maptol);
		return 1;
	} else if (fastcmp(word,"mariomode")) {
		lua_pushboolean(L, mariomode != 0);
		return 1;
	} else if (fastcmp(word,"twodlevel")) {
		lua_pushboolean(L, twodlevel != 0);
		return 1;
	} else if (fastcmp(word,"circuitmap")) {
		lua_pushboolean(L, circuitmap);
		return 1;
	} else if (fastcmp(word,"netgame")) {
		lua_pushboolean(L, netgame);
		return 1;
	} else if (fastcmp(word,"multiplayer")) {
		lua_pushboolean(L, multiplayer);
		return 1;
	} else if (fastcmp(word,"modeattacking")) {
		lua_pushboolean(L, modeattacking);
		return 1;
	} else if (fastcmp(word,"splitscreen")) {
		lua_pushboolean(L, splitscreen);
		return 1;
	} else if (fastcmp(word,"gamecomplete")) {
		lua_pushboolean(L, gamecomplete);
		return 1;
	} else if (fastcmp(word,"devparm")) {
		lua_pushboolean(L, devparm);
		return 1;
	} else if (fastcmp(word,"modifiedgame")) {
		lua_pushboolean(L, modifiedgame && !savemoddata);
		return 1;
	} else if (fastcmp(word,"menuactive")) {
		lua_pushboolean(L, menuactive);
		return 1;
	} else if (fastcmp(word,"paused")) {
		lua_pushboolean(L, paused);
		return 1;
	} else if (fastcmp(word,"titlemap")) {
		lua_pushinteger(L, titlemap);
		return 1;
	} else if (fastcmp(word,"titlemapinaction")) {
		lua_pushboolean(L, (titlemapinaction != TITLEMAP_OFF));
		return 1;
	} else if (fastcmp(word,"gametype")) {
		lua_pushinteger(L, gametype);
		return 1;
	} else if (fastcmp(word,"leveltime")) {
		lua_pushinteger(L, leveltime);
		return 1;
	} else if (fastcmp(word,"curWeather")) {
		lua_pushinteger(L, curWeather);
		return 1;
	} else if (fastcmp(word,"globalweather")) {
		lua_pushinteger(L, globalweather);
		return 1;
	} else if (fastcmp(word,"levelskynum")) {
		lua_pushinteger(L, levelskynum);
		return 1;
	} else if (fastcmp(word,"globallevelskynum")) {
		lua_pushinteger(L, globallevelskynum);
		return 1;
	} else if (fastcmp(word,"mapmusname")) {
		lua_pushstring(L, mapmusname);
		return 1;
	} else if (fastcmp(word,"mapmusflags")) {
		lua_pushinteger(L, mapmusflags);
		return 1;
	} else if (fastcmp(word,"server")) {
		if ((!multiplayer || !netgame) && !playeringame[serverplayer])
			return 0;
		LUA_PushUserdata(L, &players[serverplayer], META_PLAYER);
		return 1;
	} else if (fastcmp(word,"admin")) {
		if (!playeringame[adminplayer] || adminplayer == serverplayer)
			return 0;
		LUA_PushUserdata(L, &players[adminplayer], META_PLAYER);
		return 1;
	} else if (fastcmp(word,"emeralds")) {
		lua_pushinteger(L, emeralds);
		return 1;
	} else if (fastcmp(word,"gravity")) {
		lua_pushinteger(L, gravity);
		return 1;
	} else if (fastcmp(word,"VERSIONSTRING")) {
		lua_pushstring(L, VERSIONSTRING);
		return 1;
	} else if (fastcmp(word, "token")) {
		lua_pushinteger(L, token);
		return 1;
	}

	return 0;
}

int LUA_EnumLib(lua_State *L)
{
	if (lua_gettop(L) == 0)
		lua_pushboolean(L, 0);

	// Set the global metatable
	lua_createtable(L, 0, 1);
	lua_pushvalue(L, 1); // boolean passed to LUA_EnumLib as first argument.
	lua_pushcclosure(L, lib_getenum, 1);
	lua_setfield(L, -2, "__index");
	lua_setmetatable(L, LUA_GLOBALSINDEX);
	return 0;
}

int LUA_SOCLib(lua_State *L)
{
	lua_register(L,"freeslot",lib_freeslot);
	return 0;
}

const char *LUA_GetActionName(void *action)
{
	actionf_t *act = (actionf_t *)action;
	size_t z;
	for (z = 0; actionpointers[z].name; z++)
	{
		if (actionpointers[z].action.acv == act->acv)
			return actionpointers[z].name;
	}
	return NULL;
}

void LUA_SetActionByName(void *state, const char *actiontocompare)
{
	state_t *st = (state_t *)state;
	size_t z;
	for (z = 0; actionpointers[z].name; z++)
	{
		if (fasticmp(actiontocompare, actionpointers[z].name))
		{
			st->action = actionpointers[z].action;
			st->action.acv = actionpointers[z].action.acv; // assign
			st->action.acp1 = actionpointers[z].action.acp1;
			return;
		}
	}
}

#endif // HAVE_BLUA

// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 1993-1996 by id Software, Inc.
// Copyright (C) 1998-2000 by DooM Legacy Team.
// Copyright (C) 1999-2016 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  r_things.c
/// \brief Refresh of things, i.e. objects represented by sprites

#include "doomdef.h"
#include "console.h"
#include "g_game.h"
#include "r_local.h"
#include "st_stuff.h"
#include "w_wad.h"
#include "z_zone.h"
#include "m_menu.h" // character select
#include "m_misc.h"
#include "info.h" // spr2names
#include "i_video.h" // rendermode
#include "r_things.h"
#include "r_plane.h"
#include "p_tick.h"
#include "p_local.h"
#include "p_slopes.h"
#include "dehacked.h" // get_number (for thok)
#include "d_netfil.h" // blargh. for nameonly().
#include "m_cheat.h" // objectplace
#include "m_cond.h"
#include "fastcmp.h"
#ifdef HWRENDER
#include "hardware/hw_md2.h"
#endif

#ifdef PC_DOS
#include <stdio.h> // for snprintf
int	snprintf(char *str, size_t n, const char *fmt, ...);
//int	vsnprintf(char *str, size_t n, const char *fmt, va_list ap);
#endif

static void R_InitSkins(void);

#define MINZ (FRACUNIT*4)
#define BASEYCENTER (BASEVIDHEIGHT/2)

typedef struct
{
	INT32 x1, x2;
	INT32 column;
	INT32 topclip, bottomclip;
} maskdraw_t;

//
// Sprite rotation 0 is facing the viewer,
//  rotation 1 is one angle turn CLOCKWISE around the axis.
// This is not the same as the angle,
//  which increases counter clockwise (protractor).
// There was a lot of stuff grabbed wrong, so I changed it...
//
static lighttable_t **spritelights;

// constant arrays used for psprite clipping and initializing clipping
INT16 negonearray[MAXVIDWIDTH];
INT16 screenheightarray[MAXVIDWIDTH];

//
// INITIALIZATION FUNCTIONS
//

// variables used to look up and range check thing_t sprites patches
spritedef_t *sprites;
size_t numsprites;

static spriteframe_t sprtemp[64];
static size_t maxframe;
static const char *spritename;

// ==========================================================================
//
// Sprite loading routines: support sprites in pwad, dehacked sprite renaming,
// replacing not all frames of an existing sprite, add sprites at run-time,
// add wads at run-time.
//
// ==========================================================================

//
//
//
static void R_InstallSpriteLump(UINT16 wad,            // graphics patch
                                UINT16 lump,
                                size_t lumpid,      // identifier
                                UINT8 frame,
                                UINT8 rotation,
                                UINT8 flipped)
{
	char cn = R_Frame2Char(frame); // for debugging

	INT32 r;
	lumpnum_t lumppat = wad;
	lumppat <<= 16;
	lumppat += lump;

	if (frame >= 64 || !(R_ValidSpriteAngle(rotation)))
		I_Error("R_InstallSpriteLump: Bad frame characters in lump %s", W_CheckNameForNum(lumppat));

	if (maxframe ==(size_t)-1 || frame > maxframe)
		maxframe = frame;

	if (rotation == 0)
	{
		// the lump should be used for all rotations
		if (sprtemp[frame].rotate == SRF_SINGLE)
			CONS_Debug(DBG_SETUP, "R_InitSprites: Sprite %s frame %c has multiple rot = 0 lump\n", spritename, cn);
		else if (sprtemp[frame].rotate != SRF_NONE) // Let's bundle 1-8 and L/R rotations into one debug message.
			CONS_Debug(DBG_SETUP, "R_InitSprites: Sprite %s frame %c has rotations and a rot = 0 lump\n", spritename, cn);

		sprtemp[frame].rotate = SRF_SINGLE;
		for (r = 0; r < 8; r++)
		{
			sprtemp[frame].lumppat[r] = lumppat;
			sprtemp[frame].lumpid[r] = lumpid;
		}
		sprtemp[frame].flip = flipped ? 0xFF : 0; // 11111111 in binary
		return;
	}

	if (rotation == ROT_L || rotation == ROT_R)
	{
		UINT8 rightfactor = ((rotation == ROT_R) ? 4 : 0);

		// the lump should be used for half of all rotations
		if (sprtemp[frame].rotate == SRF_SINGLE)
			CONS_Debug(DBG_SETUP, "R_InitSprites: Sprite %s frame %c has L/R rotations and a rot = 0 lump\n", spritename, cn);
		else if (sprtemp[frame].rotate == SRF_3D)
			CONS_Debug(DBG_SETUP, "R_InitSprites: Sprite %s frame %c has both L/R and 1-8 rotations\n", spritename, cn);
		else if ((sprtemp[frame].rotate & SRF_LEFT) && (rotation == ROT_L))
			CONS_Debug(DBG_SETUP, "R_InitSprites: Sprite %s frame %c has multiple L rotations\n", spritename, cn);
		else if ((sprtemp[frame].rotate & SRF_RIGHT) && (rotation == ROT_R))
			CONS_Debug(DBG_SETUP, "R_InitSprites: Sprite %s frame %c has multiple R rotations\n", spritename, cn);

		if (sprtemp[frame].rotate == SRF_NONE)
			sprtemp[frame].rotate = SRF_SINGLE;

		sprtemp[frame].rotate |= ((rotation == ROT_R) ? SRF_RIGHT : SRF_LEFT);
		if (sprtemp[frame].rotate == (SRF_3D|SRF_2D))
			sprtemp[frame].rotate = SRF_2D; // SRF_3D|SRF_2D being enabled at the same time doesn't HURT in the current sprite angle implementation, but it DOES mean more to check in some of the helper functions. Let's not allow this scenario to happen.

		for (r = 0; r < 4; r++) // Thanks to R_PrecacheLevel, we can't leave sprtemp[*].lumppat[*] == LUMPERROR... so we load into the front/back angle too.
		{
			sprtemp[frame].lumppat[r + rightfactor] = lumppat;
			sprtemp[frame].lumpid[r + rightfactor] = lumpid;
		}

		if (flipped)
			sprtemp[frame].flip |= (0x0F<<rightfactor); // 00001111 or 11110000 in binary, depending on rotation being ROT_L or ROT_R
		else
			sprtemp[frame].flip &= ~(0x0F<<rightfactor); // ditto

		return;
	}

	// the lump is only used for one rotation
	if (sprtemp[frame].rotate == SRF_SINGLE)
		CONS_Debug(DBG_SETUP, "R_InitSprites: Sprite %s frame %c has 1-8 rotations and a rot = 0 lump\n", spritename, cn);
	else if ((sprtemp[frame].rotate != SRF_3D) && (sprtemp[frame].rotate != SRF_NONE))
		CONS_Debug(DBG_SETUP, "R_InitSprites: Sprite %s frame %c has both L/R and 1-8 rotations\n", spritename, cn);

	// make 0 based
	rotation--;

	if (rotation == 0 || rotation == 4) // Front or back...
		sprtemp[frame].rotate = SRF_3D; // Prevent L and R changeover
	else if (rotation > 3) // Right side
		sprtemp[frame].rotate = (SRF_3D | (sprtemp[frame].rotate & SRF_LEFT)); // Continue allowing L frame changeover
	else // if (rotation <= 3) // Left side
		sprtemp[frame].rotate = (SRF_3D | (sprtemp[frame].rotate & SRF_RIGHT)); // Continue allowing R frame changeover

	if (sprtemp[frame].lumppat[rotation] != LUMPERROR)
		CONS_Debug(DBG_SETUP, "R_InitSprites: Sprite %s: %c%c has two lumps mapped to it\n", spritename, cn, '1'+rotation);

	// lumppat & lumpid are the same for original Doom, but different
	// when using sprites in pwad : the lumppat points the new graphics
	sprtemp[frame].lumppat[rotation] = lumppat;
	sprtemp[frame].lumpid[rotation] = lumpid;
	if (flipped)
		sprtemp[frame].flip |= (1<<rotation);
	else
		sprtemp[frame].flip &= ~(1<<rotation);
}

// Install a single sprite, given its identifying name (4 chars)
//
// (originally part of R_AddSpriteDefs)
//
// Pass: name of sprite : 4 chars
//       spritedef_t
//       wadnum         : wad number, indexes wadfiles[], where patches
//                        for frames are found
//       startlump      : first lump to search for sprite frames
//       endlump        : AFTER the last lump to search
//
// Returns true if the sprite was succesfully added
//
static boolean R_AddSingleSpriteDef(const char *sprname, spritedef_t *spritedef, UINT16 wadnum, UINT16 startlump, UINT16 endlump)
{
	UINT16 l;
	UINT8 frame;
	UINT8 rotation;
	lumpinfo_t *lumpinfo;
	patch_t patch;
	UINT8 numadded = 0;

	memset(sprtemp,0xFF, sizeof (sprtemp));
	maxframe = (size_t)-1;

	// are we 'patching' a sprite already loaded ?
	// if so, it might patch only certain frames, not all
	if (spritedef->numframes) // (then spriteframes is not null)
	{
		// copy the already defined sprite frames
		M_Memcpy(sprtemp, spritedef->spriteframes,
		 spritedef->numframes * sizeof (spriteframe_t));
		maxframe = spritedef->numframes - 1;
	}

	// scan the lumps,
	//  filling in the frames for whatever is found
	lumpinfo = wadfiles[wadnum]->lumpinfo;
	if (endlump > wadfiles[wadnum]->numlumps)
		endlump = wadfiles[wadnum]->numlumps;

	for (l = startlump; l < endlump; l++)
	{
		if (memcmp(lumpinfo[l].name,sprname,4)==0)
		{
			frame = R_Char2Frame(lumpinfo[l].name[4]);
			rotation = (UINT8)(lumpinfo[l].name[5] - '0');

			if (frame >= 64 || !(R_ValidSpriteAngle(rotation))) // Give an actual NAME error -_-...
			{
				CONS_Alert(CONS_WARNING, M_GetText("Bad sprite name: %s\n"), W_CheckNameForNumPwad(wadnum,l));
				continue;
			}

			// skip NULL sprites from very old dmadds pwads
			if (W_LumpLengthPwad(wadnum,l)<=8)
				continue;

			// store sprite info in lookup tables
			//FIXME : numspritelumps do not duplicate sprite replacements
			W_ReadLumpHeaderPwad(wadnum, l, &patch, sizeof (patch_t), 0);
			spritecachedinfo[numspritelumps].width = SHORT(patch.width)<<FRACBITS;
			spritecachedinfo[numspritelumps].offset = SHORT(patch.leftoffset)<<FRACBITS;
			spritecachedinfo[numspritelumps].topoffset = SHORT(patch.topoffset)<<FRACBITS;
			spritecachedinfo[numspritelumps].height = SHORT(patch.height)<<FRACBITS;

			//BP: we cannot use special tric in hardware mode because feet in ground caused by z-buffer
			if (rendermode != render_none) // not for psprite
				spritecachedinfo[numspritelumps].topoffset += 4<<FRACBITS;
			// Being selective with this causes bad things. :( Like the special stage tokens breaking apart.
			/*if (rendermode != render_none // not for psprite
			 && SHORT(patch.topoffset)>0 && SHORT(patch.topoffset)<SHORT(patch.height))
				// perfect is patch.height but sometime it is too high
				spritecachedinfo[numspritelumps].topoffset = min(SHORT(patch.topoffset)+4,SHORT(patch.height))<<FRACBITS;*/

			//----------------------------------------------------

			R_InstallSpriteLump(wadnum, l, numspritelumps, frame, rotation, 0);

			if (lumpinfo[l].name[6])
			{
				frame = R_Char2Frame(lumpinfo[l].name[6]);
				rotation = (UINT8)(lumpinfo[l].name[7] - '0');
				R_InstallSpriteLump(wadnum, l, numspritelumps, frame, rotation, 1);
			}

			if (++numspritelumps >= max_spritelumps)
			{
				max_spritelumps *= 2;
				Z_Realloc(spritecachedinfo, max_spritelumps*sizeof(*spritecachedinfo), PU_STATIC, &spritecachedinfo);
			}

			++numadded;
		}
	}

	//
	// if no frames found for this sprite
	//
	if (maxframe == (size_t)-1)
	{
		// the first time (which is for the original wad),
		// all sprites should have their initial frames
		// and then, patch wads can replace it
		// we will skip non-replaced sprite frames, only if
		// they have already have been initially defined (original wad)

		//check only after all initial pwads added
		//if (spritedef->numframes == 0)
		//    I_Error("R_AddSpriteDefs: no initial frames found for sprite %s\n",
		//             namelist[i]);

		// sprite already has frames, and is not replaced by this wad
		return false;
	}
	else if (!numadded)
	{
		// Nothing related to this spritedef has been changed
		// so there is no point going back through these checks again.
		return false;
	}

	maxframe++;

	//
	//  some checks to help development
	//
	for (frame = 0; frame < maxframe; frame++)
	{
		switch (sprtemp[frame].rotate)
		{
			case SRF_NONE:
			// no rotations were found for that frame at all
			I_Error("R_AddSingleSpriteDef: No patches found for %.4s frame %c", sprname, R_Frame2Char(frame));
			break;

			case SRF_SINGLE:
			// only the first rotation is needed
			break;

			case SRF_2D: // both Left and Right rotations
			// we test to see whether the left and right slots are present
			if ((sprtemp[frame].lumppat[2] == LUMPERROR) || (sprtemp[frame].lumppat[6] == LUMPERROR))
				I_Error("R_AddSingleSpriteDef: Sprite %.4s frame %c is missing rotations",
				sprname, R_Frame2Char(frame));
			break;

			default:
			// must have all 8 frames
			for (rotation = 0; rotation < 8; rotation++)
				// we test the patch lump, or the id lump whatever
				// if it was not loaded the two are LUMPERROR
				if (sprtemp[frame].lumppat[rotation] == LUMPERROR)
					I_Error("R_AddSingleSpriteDef: Sprite %.4s frame %c is missing rotations",
					        sprname, R_Frame2Char(frame));
			break;
		}
	}

	// allocate space for the frames present and copy sprtemp to it
	if (spritedef->numframes &&             // has been allocated
		spritedef->numframes < maxframe)   // more frames are defined ?
	{
		Z_Free(spritedef->spriteframes);
		spritedef->spriteframes = NULL;
	}

	// allocate this sprite's frames
	if (!spritedef->spriteframes)
		spritedef->spriteframes =
		 Z_Malloc(maxframe * sizeof (*spritedef->spriteframes), PU_STATIC, NULL);

	spritedef->numframes = maxframe;
	M_Memcpy(spritedef->spriteframes, sprtemp, maxframe*sizeof (spriteframe_t));

	return true;
}

//
// Search for sprites replacements in a wad whose names are in namelist
//
void R_AddSpriteDefs(UINT16 wadnum)
{
	size_t i, addsprites = 0;
	UINT16 start, end;
	char wadname[MAX_WADPATH];

	// find the sprites section in this pwad
	// we need at least the S_END
	// (not really, but for speedup)

	start = W_CheckNumForNamePwad("S_START", wadnum, 0);
	if (start == INT16_MAX)
		start = W_CheckNumForNamePwad("SS_START", wadnum, 0); //deutex compatib.
	if (start == INT16_MAX)
		start = 0; //let say S_START is lump 0
	else
		start++;   // just after S_START

	// ignore skin wads (we don't want skin sprites interfering with vanilla sprites)
	if (start == 0 && W_CheckNumForNamePwad("S_SKIN", wadnum, 0) != INT16_MAX)
		return;

	end = W_CheckNumForNamePwad("S_END",wadnum,start);
	if (end == INT16_MAX)
		end = W_CheckNumForNamePwad("SS_END",wadnum,start);     //deutex compatib.
	if (end == INT16_MAX)
	{
		CONS_Debug(DBG_SETUP, "no sprites in pwad %d\n", wadnum);
		return;
	}

	//
	// scan through lumps, for each sprite, find all the sprite frames
	//
	for (i = 0; i < numsprites; i++)
	{
		spritename = sprnames[i];
		if (spritename[4] && wadnum >= (UINT16)spritename[4])
			continue;

		if (R_AddSingleSpriteDef(spritename, &sprites[i], wadnum, start, end))
		{
#ifdef HWRENDER
			if (rendermode == render_opengl)
				HWR_AddSpriteMD2(i);
#endif
			// if a new sprite was added (not just replaced)
			addsprites++;
#ifndef ZDEBUG
			CONS_Debug(DBG_SETUP, "sprite %s set in pwad %d\n", spritename, wadnum);
#endif
		}
	}

	nameonly(strcpy(wadname, wadfiles[wadnum]->filename));
	CONS_Printf(M_GetText("%s added %d frames in %s sprites\n"), wadname, end-start, sizeu1(addsprites));
}

//
// GAME FUNCTIONS
//
static UINT32 visspritecount;
static UINT32 clippedvissprites;
static vissprite_t *visspritechunks[MAXVISSPRITES >> VISSPRITECHUNKBITS] = {NULL};


//
// R_InitSprites
// Called at program start.
//
void R_InitSprites(void)
{
	size_t i;

	for (i = 0; i < MAXVIDWIDTH; i++)
	{
		negonearray[i] = -1;
	}

	//
	// count the number of sprite names, and allocate sprites table
	//
	numsprites = 0;
	for (i = 0; i < NUMSPRITES + 1; i++)
		if (sprnames[i][0] != '\0') numsprites++;

	if (!numsprites)
		I_Error("R_AddSpriteDefs: no sprites in namelist\n");

	sprites = Z_Calloc(numsprites * sizeof (*sprites), PU_STATIC, NULL);

	// find sprites in each -file added pwad
	for (i = 0; i < numwadfiles; i++)
		R_AddSpriteDefs((UINT16)i);

	//
	// now check for skins
	//

	// it can be is do before loading config for skin cvar possible value
	R_InitSkins();
	for (i = 0; i < numwadfiles; i++)
		R_AddSkins((UINT16)i);

	//
	// check if all sprites have frames
	//
	/*
	for (i = 0; i < numsprites; i++)
		if (sprites[i].numframes < 1)
			CONS_Debug(DBG_SETUP, "R_InitSprites: sprite %s has no frames at all\n", sprnames[i]);
	*/
}

//
// R_ClearSprites
// Called at frame start.
//
void R_ClearSprites(void)
{
	visspritecount = clippedvissprites = 0;
}

//
// R_NewVisSprite
//
static vissprite_t overflowsprite;

static vissprite_t *R_GetVisSprite(UINT32 num)
{
		UINT32 chunk = num >> VISSPRITECHUNKBITS;

		// Allocate chunk if necessary
		if (!visspritechunks[chunk])
			Z_Malloc(sizeof(vissprite_t) * VISSPRITESPERCHUNK, PU_LEVEL, &visspritechunks[chunk]);

		return visspritechunks[chunk] + (num & VISSPRITEINDEXMASK);
}

static vissprite_t *R_NewVisSprite(void)
{
	if (visspritecount == MAXVISSPRITES)
		return &overflowsprite;

	return R_GetVisSprite(visspritecount++);
}

//
// R_DrawMaskedColumn
// Used for sprites and masked mid textures.
// Masked means: partly transparent, i.e. stored
//  in posts/runs of opaque pixels.
//
INT16 *mfloorclip;
INT16 *mceilingclip;

fixed_t spryscale = 0, sprtopscreen = 0, sprbotscreen = 0;
fixed_t windowtop = 0, windowbottom = 0;

void R_DrawMaskedColumn(column_t *column)
{
	INT32 topscreen;
	INT32 bottomscreen;
	fixed_t basetexturemid;
	INT32 topdelta, prevdelta = 0;

	basetexturemid = dc_texturemid;

	for (; column->topdelta != 0xff ;)
	{
		// calculate unclipped screen coordinates
		// for post
		topdelta = column->topdelta;
		if (topdelta <= prevdelta)
			topdelta += prevdelta;
		prevdelta = topdelta;
		topscreen = sprtopscreen + spryscale*topdelta;
		bottomscreen = topscreen + spryscale*column->length;

		dc_yl = (topscreen+FRACUNIT-1)>>FRACBITS;
		dc_yh = (bottomscreen-1)>>FRACBITS;

		if (windowtop != INT32_MAX && windowbottom != INT32_MAX)
		{
			if (windowtop > topscreen)
				dc_yl = (windowtop + FRACUNIT - 1)>>FRACBITS;
			if (windowbottom < bottomscreen)
				dc_yh = (windowbottom - 1)>>FRACBITS;
		}

		if (dc_yh >= mfloorclip[dc_x])
			dc_yh = mfloorclip[dc_x]-1;
		if (dc_yl <= mceilingclip[dc_x])
			dc_yl = mceilingclip[dc_x]+1;
		if (dc_yl < 0)
			dc_yl = 0;
		if (dc_yh >= vid.height)
			dc_yh = vid.height - 1;

		if (dc_yl <= dc_yh && dc_yl < vid.height && dc_yh > 0)
		{
			dc_source = (UINT8 *)column + 3;
			dc_texturemid = basetexturemid - (topdelta<<FRACBITS);

			// Drawn by R_DrawColumn.
			// This stuff is a likely cause of the splitscreen water crash bug.
			// FIXTHIS: Figure out what "something more proper" is and do it.
			// quick fix... something more proper should be done!!!
			if (ylookup[dc_yl])
				colfunc();
			else if (colfunc == R_DrawColumn_8
#ifdef USEASM
			|| colfunc == R_DrawColumn_8_ASM || colfunc == R_DrawColumn_8_MMX
#endif
			)
			{
				static INT32 first = 1;
				if (first)
				{
					CONS_Debug(DBG_RENDER, "WARNING: avoiding a crash in %s %d\n", __FILE__, __LINE__);
					first = 0;
				}
			}
		}
		column = (column_t *)((UINT8 *)column + column->length + 4);
	}

	dc_texturemid = basetexturemid;
}

void R_DrawFlippedMaskedColumn(column_t *column, INT32 texheight)
{
	INT32 topscreen;
	INT32 bottomscreen;
	fixed_t basetexturemid = dc_texturemid;
	INT32 topdelta, prevdelta = -1;
	UINT8 *d,*s;

	for (; column->topdelta != 0xff ;)
	{
		// calculate unclipped screen coordinates
		// for post
		topdelta = column->topdelta;
		if (topdelta <= prevdelta)
			topdelta += prevdelta;
		prevdelta = topdelta;
		topdelta = texheight-column->length-topdelta;
		topscreen = sprtopscreen + spryscale*topdelta;
		bottomscreen = sprbotscreen == INT32_MAX ? topscreen + spryscale*column->length
		                                      : sprbotscreen + spryscale*column->length;

		dc_yl = (topscreen+FRACUNIT-1)>>FRACBITS;
		dc_yh = (bottomscreen-1)>>FRACBITS;

		if (windowtop != INT32_MAX && windowbottom != INT32_MAX)
		{
			if (windowtop > topscreen)
				dc_yl = (windowtop + FRACUNIT - 1)>>FRACBITS;
			if (windowbottom < bottomscreen)
				dc_yh = (windowbottom - 1)>>FRACBITS;
		}

		if (dc_yh >= mfloorclip[dc_x])
			dc_yh = mfloorclip[dc_x]-1;
		if (dc_yl <= mceilingclip[dc_x])
			dc_yl = mceilingclip[dc_x]+1;
		if (dc_yl < 0)
			dc_yl = 0;
		if (dc_yh >= vid.height)
			dc_yh = vid.height - 1;

		if (dc_yl <= dc_yh && dc_yl < vid.height && dc_yh > 0)
		{
			dc_source = ZZ_Alloc(column->length);
			for (s = (UINT8 *)column+2+column->length, d = dc_source; d < dc_source+column->length; --s)
				*d++ = *s;
			dc_texturemid = basetexturemid - (topdelta<<FRACBITS);

			// Still drawn by R_DrawColumn.
			if (ylookup[dc_yl])
				colfunc();
			else if (colfunc == R_DrawColumn_8
#ifdef USEASM
			|| colfunc == R_DrawColumn_8_ASM || colfunc == R_DrawColumn_8_MMX
#endif
			)
			{
				static INT32 first = 1;
				if (first)
				{
					CONS_Debug(DBG_RENDER, "WARNING: avoiding a crash in %s %d\n", __FILE__, __LINE__);
					first = 0;
				}
			}
			Z_Free(dc_source);
		}
		column = (column_t *)((UINT8 *)column + column->length + 4);
	}

	dc_texturemid = basetexturemid;
}

//
// R_DrawVisSprite
//  mfloorclip and mceilingclip should also be set.
//
static void R_DrawVisSprite(vissprite_t *vis)
{
	column_t *column;
#ifdef RANGECHECK
	INT32 texturecolumn;
#endif
	fixed_t frac;
	patch_t *patch = W_CacheLumpNum(vis->patch, PU_CACHE);
	fixed_t this_scale = vis->mobj->scale;
	INT32 x1, x2;
	INT64 overflow_test;

	if (!patch)
		return;

	// Check for overflow
	overflow_test = (INT64)centeryfrac - (((INT64)vis->texturemid*vis->scale)>>FRACBITS);
	if (overflow_test < 0) overflow_test = -overflow_test;
	if ((UINT64)overflow_test&0xFFFFFFFF80000000ULL) return; // fixed point mult would overflow

	if (vis->scalestep) // handles right edge too
	{
		overflow_test = (INT64)centeryfrac - (((INT64)vis->texturemid*(vis->scale + (vis->scalestep*(vis->x2 - vis->x1))))>>FRACBITS);
		if (overflow_test < 0) overflow_test = -overflow_test;
		if ((UINT64)overflow_test&0xFFFFFFFF80000000ULL) return; // ditto
	}

	colfunc = basecolfunc; // hack: this isn't resetting properly somewhere.
	dc_colormap = vis->colormap;
	if (!(vis->cut & SC_PRECIP) && (vis->mobj->flags & MF_BOSS) && (vis->mobj->flags2 & MF2_FRET) && (leveltime & 1)) // Bosses "flash"
	{
		// translate certain pixels to white
		colfunc = transcolfunc;
		if (vis->mobj->type == MT_CYBRAKDEMON)
			dc_translation = R_GetTranslationColormap(TC_ALLWHITE, 0, GTC_CACHE);
		else if (vis->mobj->type == MT_METALSONIC_BATTLE)
			dc_translation = R_GetTranslationColormap(TC_METALSONIC, 0, GTC_CACHE);
		else
			dc_translation = R_GetTranslationColormap(TC_BOSS, 0, GTC_CACHE);
	}
	else if (vis->mobj->color && vis->transmap) // Color mapping
	{
		colfunc = transtransfunc;
		dc_transmap = vis->transmap;
		if (!(vis->cut & SC_PRECIP) && vis->mobj->skin && vis->mobj->sprite == SPR_PLAY) // MT_GHOST LOOKS LIKE A PLAYER SO USE THE PLAYER TRANSLATION TABLES. >_>
		{
			size_t skinnum = (skin_t*)vis->mobj->skin-skins;
			dc_translation = R_GetTranslationColormap((INT32)skinnum, vis->mobj->color, GTC_CACHE);
		}
		else // Use the defaults
			dc_translation = R_GetTranslationColormap(TC_DEFAULT, vis->mobj->color, GTC_CACHE);
	}
	else if (vis->transmap)
	{
		colfunc = fuzzcolfunc;
		dc_transmap = vis->transmap;    //Fab : 29-04-98: translucency table
	}
	else if (vis->mobj->color)
	{
		// translate green skin to another color
		colfunc = transcolfunc;

		// New colormap stuff for skins Tails 06-07-2002
		if (!(vis->cut & SC_PRECIP) && vis->mobj->skin && vis->mobj->sprite == SPR_PLAY) // This thing is a player!
		{
			size_t skinnum = (skin_t*)vis->mobj->skin-skins;
			dc_translation = R_GetTranslationColormap((INT32)skinnum, vis->mobj->color, GTC_CACHE);
		}
		else // Use the defaults
			dc_translation = R_GetTranslationColormap(TC_DEFAULT, vis->mobj->color, GTC_CACHE);
	}
	else if (vis->mobj->sprite == SPR_PLAY) // Looks like a player, but doesn't have a color? Get rid of green sonic syndrome.
	{
		colfunc = transcolfunc;
		dc_translation = R_GetTranslationColormap(TC_DEFAULT, SKINCOLOR_BLUE, GTC_CACHE);
	}

	if (vis->extra_colormap)
	{
		if (!dc_colormap)
			dc_colormap = vis->extra_colormap->colormap;
		else
			dc_colormap = &vis->extra_colormap->colormap[dc_colormap - colormaps];
	}
	if (!dc_colormap)
		dc_colormap = colormaps;

	dc_texturemid = vis->texturemid;
	dc_texheight = 0;

	frac = vis->startfrac;
	windowtop = windowbottom = sprbotscreen = INT32_MAX;

	if (!(vis->cut & SC_PRECIP) && vis->mobj->skin && ((skin_t *)vis->mobj->skin)->flags & SF_HIRES)
		this_scale = FixedMul(this_scale, ((skin_t *)vis->mobj->skin)->highresscale);
	if (this_scale <= 0)
		this_scale = 1;
	if (this_scale != FRACUNIT)
	{
		if (!(vis->cut & SC_ISSCALED))
		{
			vis->scale = FixedMul(vis->scale, this_scale);
			vis->scalestep = FixedMul(vis->scalestep, this_scale);
			vis->xiscale = FixedDiv(vis->xiscale,this_scale);
			vis->cut |= SC_ISSCALED;
		}
		dc_texturemid = FixedDiv(dc_texturemid,this_scale);
	}

	spryscale = vis->scale;

	if (!(vis->scalestep))
	{
		sprtopscreen = centeryfrac - FixedMul(dc_texturemid, spryscale);
		dc_iscale = FixedDiv(FRACUNIT, vis->scale);
	}

	x1 = vis->x1;
	x2 = vis->x2;

	if (vis->x1 < 0)
	{
		spryscale += vis->scalestep*(-vis->x1);
		vis->x1 = 0;
	}

	if (vis->x2 >= vid.width)
		vis->x2 = vid.width-1;

	for (dc_x = vis->x1; dc_x <= vis->x2; dc_x++, frac += vis->xiscale)
	{
#ifdef RANGECHECK

		texturecolumn = frac>>FRACBITS;

		if (texturecolumn < 0 || texturecolumn >= SHORT(patch->width))
			I_Error("R_DrawSpriteRange: bad texturecolumn at %d from end", vis->x2 - dc_x);
		column = (column_t *)((UINT8 *)patch + LONG(patch->columnofs[texturecolumn]));
#else
		column = (column_t *)((UINT8 *)patch + LONG(patch->columnofs[frac>>FRACBITS]));
#endif
		if (vis->scalestep)
		{
			sprtopscreen = (centeryfrac - FixedMul(dc_texturemid, spryscale));
			dc_iscale = (0xffffffffu / (unsigned)spryscale);
		}
		if (vis->cut & SC_VFLIP)
			R_DrawFlippedMaskedColumn(column, patch->height);
		else
			R_DrawMaskedColumn(column);
		spryscale += vis->scalestep;
	}

	colfunc = basecolfunc;
	dc_hires = 0;

	vis->x1 = x1;
	vis->x2 = x2;
}

// Special precipitation drawer Tails 08-18-2002
static void R_DrawPrecipitationVisSprite(vissprite_t *vis)
{
	column_t *column;
#ifdef RANGECHECK
	INT32 texturecolumn;
#endif
	fixed_t frac;
	patch_t *patch;
	INT64 overflow_test;

	//Fab : R_InitSprites now sets a wad lump number
	patch = W_CacheLumpNum(vis->patch, PU_CACHE);
	if (!patch)
		return;

	// Check for overflow
	overflow_test = (INT64)centeryfrac - (((INT64)vis->texturemid*vis->scale)>>FRACBITS);
	if (overflow_test < 0) overflow_test = -overflow_test;
	if ((UINT64)overflow_test&0xFFFFFFFF80000000ULL) return; // fixed point mult would overflow

	if (vis->transmap)
	{
		colfunc = fuzzcolfunc;
		dc_transmap = vis->transmap;    //Fab : 29-04-98: translucency table
	}

	dc_colormap = colormaps;

	dc_iscale = FixedDiv(FRACUNIT, vis->scale);
	dc_texturemid = vis->texturemid;
	dc_texheight = 0;

	frac = vis->startfrac;
	spryscale = vis->scale;
	sprtopscreen = centeryfrac - FixedMul(dc_texturemid,spryscale);
	windowtop = windowbottom = sprbotscreen = INT32_MAX;

	if (vis->x1 < 0)
		vis->x1 = 0;

	if (vis->x2 >= vid.width)
		vis->x2 = vid.width-1;

	for (dc_x = vis->x1; dc_x <= vis->x2; dc_x++, frac += vis->xiscale)
	{
#ifdef RANGECHECK
		texturecolumn = frac>>FRACBITS;

		if (texturecolumn < 0 || texturecolumn >= SHORT(patch->width))
			I_Error("R_DrawPrecipitationSpriteRange: bad texturecolumn");

		column = (column_t *)((UINT8 *)patch + LONG(patch->columnofs[texturecolumn]));
#else
		column = (column_t *)((UINT8 *)patch + LONG(patch->columnofs[frac>>FRACBITS]));
#endif
		R_DrawMaskedColumn(column);
	}

	colfunc = basecolfunc;
}

//
// R_SplitSprite
// runs through a sector's lightlist and
static void R_SplitSprite(vissprite_t *sprite)
{
	INT32 i, lightnum, lindex;
	INT16 cutfrac;
	sector_t *sector;
	vissprite_t *newsprite;

	sector = sprite->sector;

	for (i = 1; i < sector->numlights; i++)
	{
		fixed_t testheight = sector->lightlist[i].height;

		if (!(sector->lightlist[i].caster->flags & FF_CUTSPRITES))
			continue;

#ifdef ESLOPE
		if (sector->lightlist[i].slope)
			testheight = P_GetZAt(sector->lightlist[i].slope, sprite->gx, sprite->gy);
#endif

		if (testheight >= sprite->gzt)
			continue;
		if (testheight <= sprite->gz)
			return;

		cutfrac = (INT16)((centeryfrac - FixedMul(testheight - viewz, sprite->sortscale))>>FRACBITS);
		if (cutfrac < 0)
			continue;
		if (cutfrac > viewheight)
			return;

		// Found a split! Make a new sprite, copy the old sprite to it, and
		// adjust the heights.
		newsprite = M_Memcpy(R_NewVisSprite(), sprite, sizeof (vissprite_t));

		newsprite->cut |= (sprite->cut & SC_FLAGMASK);

		sprite->cut |= SC_BOTTOM;
		sprite->gz = testheight;

		newsprite->gzt = sprite->gz;

		sprite->sz = cutfrac;
		newsprite->szt = (INT16)(sprite->sz - 1);

		if (testheight < sprite->pzt && testheight > sprite->pz)
			sprite->pz = newsprite->pzt = testheight;
		else
		{
			newsprite->pz = newsprite->gz;
			newsprite->pzt = newsprite->gzt;
		}

		newsprite->szt -= 8;

		newsprite->cut |= SC_TOP;
		if (!(sector->lightlist[i].caster->flags & FF_NOSHADE))
		{
			lightnum = (*sector->lightlist[i].lightlevel >> LIGHTSEGSHIFT);

			if (lightnum < 0)
				spritelights = scalelight[0];
			else if (lightnum >= LIGHTLEVELS)
				spritelights = scalelight[LIGHTLEVELS-1];
			else
				spritelights = scalelight[lightnum];

			newsprite->extra_colormap = sector->lightlist[i].extra_colormap;

			if (!((newsprite->cut & SC_FULLBRIGHT) && (!newsprite->extra_colormap || !newsprite->extra_colormap->fog)))
			{
				lindex = FixedMul(sprite->xscale, FixedDiv(640, vid.width))>>(LIGHTSCALESHIFT);

				if (lindex >= MAXLIGHTSCALE)
					lindex = MAXLIGHTSCALE-1;
				newsprite->colormap = spritelights[lindex];
			}
		}
		sprite = newsprite;
	}
}

//
// R_ProjectSprite
// Generates a vissprite for a thing
// if it might be visible.
//
static void R_ProjectSprite(mobj_t *thing)
{
	mobj_t *oldthing = thing;
	fixed_t tr_x, tr_y;
	fixed_t gxt, gyt;
	fixed_t tx, tz;
	fixed_t xscale, yscale, sortscale; //added : 02-02-98 : aaargll..if I were a math-guy!!!

	INT32 x1, x2;

	spritedef_t *sprdef;
	spriteframe_t *sprframe;
	size_t lump;

	size_t rot;
	UINT8 flip;
	boolean vflip = (!(thing->eflags & MFE_VERTICALFLIP) != !(thing->frame & FF_VERTICALFLIP));

	INT32 lindex;

	vissprite_t *vis;

	spritecut_e cut = SC_NONE;

	angle_t ang = 0; // compiler complaints
	fixed_t iscale;
	fixed_t scalestep;
	fixed_t offset, offset2;
	boolean papersprite = !!(thing->frame & FF_PAPERSPRITE);

	INT32 dispoffset = thing->info->dispoffset;

	//SoM: 3/17/2000
	fixed_t gz, gzt;
	INT32 heightsec, phs;
	INT32 light = 0;
	fixed_t this_scale = thing->scale;

	fixed_t ang_scale = FRACUNIT;

	// transform the origin point
	tr_x = thing->x - viewx;
	tr_y = thing->y - viewy;

	gxt = FixedMul(tr_x, viewcos);
	gyt = -FixedMul(tr_y, viewsin);

	tz = gxt-gyt;

	// thing is behind view plane?
	if (!(papersprite) && (tz < FixedMul(MINZ, this_scale))) // papersprite clipping is handled later
		return;

	gxt = -FixedMul(tr_x, viewsin);
	gyt = FixedMul(tr_y, viewcos);
	tx = -(gyt + gxt);

	// too far off the side?
	if (abs(tx) > tz<<2)
		return;

	// aspect ratio stuff
	xscale = FixedDiv(projection, tz);
	sortscale = FixedDiv(projectiony, tz);

	// decide which patch to use for sprite relative to player
#ifdef RANGECHECK
	if ((size_t)(thing->sprite) >= numsprites)
		I_Error("R_ProjectSprite: invalid sprite number %d ", thing->sprite);
#endif

	rot = thing->frame&FF_FRAMEMASK;

	//Fab : 02-08-98: 'skin' override spritedef currently used for skin
	if (thing->skin && thing->sprite == SPR_PLAY)
	{
		sprdef = &((skin_t *)thing->skin)->sprites[thing->sprite2];
		if (rot >= sprdef->numframes) {
			CONS_Alert(CONS_ERROR, M_GetText("R_ProjectSprite: invalid skins[\"%s\"].sprites[%sSPR2_%s] frame %s\n"), ((skin_t *)thing->skin)->name, ((thing->sprite2 & FF_SPR2SUPER) ? "FF_SPR2SUPER|": ""), spr2names[(thing->sprite2 & ~FF_SPR2SUPER)], sizeu5(rot));
			thing->sprite = states[S_UNKNOWN].sprite;
			thing->frame = states[S_UNKNOWN].frame;
			sprdef = &sprites[thing->sprite];
			rot = thing->frame&FF_FRAMEMASK;
		}
	}
	else
		sprdef = &sprites[thing->sprite];

	if (rot >= sprdef->numframes)
	{
		CONS_Alert(CONS_ERROR, M_GetText("R_ProjectSprite: invalid sprite frame %s/%s for %s\n"),
			sizeu1(rot), sizeu2(sprdef->numframes), sprnames[thing->sprite]);
		thing->sprite = states[S_UNKNOWN].sprite;
		thing->frame = states[S_UNKNOWN].frame;
		sprdef = &sprites[thing->sprite];
		rot = thing->frame&FF_FRAMEMASK;
		if (!thing->skin)
		{
			thing->state->sprite = thing->sprite;
			thing->state->frame = thing->frame;
		}
	}

	sprframe = &sprdef->spriteframes[rot];

#ifdef PARANOIA
	if (!sprframe)
		I_Error("R_ProjectSprite: sprframes NULL for sprite %d\n", thing->sprite);
#endif

	if (sprframe->rotate != SRF_SINGLE || papersprite)
	{
		ang = R_PointToAngle (thing->x, thing->y) - (thing->player ? thing->player->drawangle : thing->angle);
		if (papersprite)
			ang_scale = abs(FINESINE(ang>>ANGLETOFINESHIFT));
	}

	if (sprframe->rotate == SRF_SINGLE)
	{
		// use single rotation for all views
		rot = 0;                        //Fab: for vis->patch below
		lump = sprframe->lumpid[0];     //Fab: see note above
		flip = sprframe->flip; // Will only be 0x00 or 0xFF
	}
	else
	{
		// choose a different rotation based on player view
		//ang = R_PointToAngle (thing->x, thing->y) - thing->angle;

		if ((sprframe->rotate & SRF_RIGHT) && (ang < ANGLE_180)) // See from right
			rot = 6; // F7 slot
		else if ((sprframe->rotate & SRF_LEFT) && (ang >= ANGLE_180)) // See from left
			rot = 2; // F3 slot
		else // Normal behaviour
			rot = (ang+ANGLE_202h)>>29;

		//Fab: lumpid is the index for spritewidth,spriteoffset... tables
		lump = sprframe->lumpid[rot];
		flip = sprframe->flip & (1<<rot);
	}

	I_Assert(lump < max_spritelumps);

	if (thing->skin && ((skin_t *)thing->skin)->flags & SF_HIRES)
		this_scale = FixedMul(this_scale, ((skin_t *)thing->skin)->highresscale);

	// calculate edges of the shape
	if (flip)
		offset = spritecachedinfo[lump].offset - spritecachedinfo[lump].width;
	else
		offset = -spritecachedinfo[lump].offset;
	offset = FixedMul(offset, this_scale);
	tx += FixedMul(offset, ang_scale);
	x1 = (centerxfrac + FixedMul (tx,xscale)) >>FRACBITS;

	// off the right side?
	if (x1 > viewwidth)
		return;

	offset2 = FixedMul(spritecachedinfo[lump].width, this_scale);
	tx += FixedMul(offset2, ang_scale);
	x2 = ((centerxfrac + FixedMul (tx,xscale)) >> FRACBITS) - (papersprite ? 2 : 1);

	// off the left side
	if (x2 < 0)
		return;

	if (papersprite)
	{
		fixed_t yscale2, cosmul, sinmul, tz2;
		INT32 range;

		if (ang >= ANGLE_180)
		{
			offset *= -1;
			offset2 *= -1;
		}

		cosmul = FINECOSINE(thing->angle>>ANGLETOFINESHIFT);
		sinmul = FINESINE(thing->angle>>ANGLETOFINESHIFT);

		tr_x += FixedMul(offset, cosmul);
		tr_y += FixedMul(offset, sinmul);
		gxt = FixedMul(tr_x, viewcos);
		gyt = -FixedMul(tr_y, viewsin);
		tz = gxt-gyt;
		yscale = FixedDiv(projectiony, tz);
		if (yscale < 64) return; // Fix some funky visuals

		tr_x += FixedMul(offset2, cosmul);
		tr_y += FixedMul(offset2, sinmul);
		gxt = FixedMul(tr_x, viewcos);
		gyt = -FixedMul(tr_y, viewsin);
		tz2 = gxt-gyt;
		yscale2 = FixedDiv(projectiony, tz2);
		if (yscale2 < 64) return; // ditto

		if (max(tz, tz2) < FixedMul(MINZ, this_scale)) // non-papersprite clipping is handled earlier
			return;

		if (x2 > x1)
			range = (x2 - x1);
		else
			range = 1;

		scalestep = (yscale2 - yscale)/range;

		// The following two are alternate sorting methods which might be more applicable in some circumstances. TODO - maybe enable via MF2?
		// sortscale = max(yscale, yscale2);
		// sortscale = min(yscale, yscale2);
	}
	else
	{
		scalestep = 0;
		yscale = sortscale;
	}

	xscale = FixedMul(xscale, ang_scale);

	if ((thing->flags2 & MF2_LINKDRAW) && thing->tracer) // toast 16/09/16 (SYMMETRY)
	{
		fixed_t linkscale;

		thing = thing->tracer;

		if (thing->sprite == SPR_NULL || thing->flags2 & MF2_DONTDRAW)
			return;

		tr_x = thing->x - viewx;
		tr_y = thing->y - viewy;
		gxt = FixedMul(tr_x, viewcos);
		gyt = -FixedMul(tr_y, viewsin);
		tz = gxt-gyt;
		linkscale = FixedDiv(projectiony, tz);

		if (tz < FixedMul(MINZ, this_scale))
			return;

		if (sortscale < linkscale)
			dispoffset *= -1; // if it's physically behind, make sure it's ordered behind (if dispoffset > 0)

		sortscale = linkscale; // now make sure it's linked
		cut = SC_LINKDRAW;
	}

	// PORTAL SPRITE CLIPPING
	if (portalrender)
	{
		if (x2 < portalclipstart || x1 > portalclipend)
			return;

		if (P_PointOnLineSide(thing->x, thing->y, portalclipline) != 0)
			return;
	}

	//SoM: 3/17/2000: Disregard sprites that are out of view..
	if (vflip)
	{
		// When vertical flipped, draw sprites from the top down, at least as far as offsets are concerned.
		// sprite height - sprite topoffset is the proper inverse of the vertical offset, of course.
		// remember gz and gzt should be seperated by sprite height, not thing height - thing height can be shorter than the sprite itself sometimes!
		gz = oldthing->z + oldthing->height - FixedMul(spritecachedinfo[lump].topoffset, this_scale);
		gzt = gz + FixedMul(spritecachedinfo[lump].height, this_scale);
	}
	else
	{
		gzt = oldthing->z + FixedMul(spritecachedinfo[lump].topoffset, this_scale);
		gz = gzt - FixedMul(spritecachedinfo[lump].height, this_scale);
	}

	if (thing->subsector->sector->cullheight)
	{
		if (R_DoCulling(thing->subsector->sector->cullheight, viewsector->cullheight, viewz, gz, gzt))
			return;
	}

	if (thing->subsector->sector->numlights)
	{
		INT32 lightnum;
#ifdef ESLOPE // R_GetPlaneLight won't work on sloped lights!
		light = thing->subsector->sector->numlights - 1;

		for (lightnum = 1; lightnum < thing->subsector->sector->numlights; lightnum++) {
			fixed_t h = thing->subsector->sector->lightlist[lightnum].slope ? P_GetZAt(thing->subsector->sector->lightlist[lightnum].slope, thing->x, thing->y)
			            : thing->subsector->sector->lightlist[lightnum].height;
			if (h <= gzt) {
				light = lightnum - 1;
				break;
			}
		}
#else
		light = R_GetPlaneLight(thing->subsector->sector, gzt, false);
#endif
		lightnum = (*thing->subsector->sector->lightlist[light].lightlevel >> LIGHTSEGSHIFT);

		if (lightnum < 0)
			spritelights = scalelight[0];
		else if (lightnum >= LIGHTLEVELS)
			spritelights = scalelight[LIGHTLEVELS-1];
		else
			spritelights = scalelight[lightnum];
	}

	heightsec = thing->subsector->sector->heightsec;
	if (viewplayer->mo && viewplayer->mo->subsector)
		phs = viewplayer->mo->subsector->sector->heightsec;
	else
		phs = -1;

	if (heightsec != -1 && phs != -1) // only clip things which are in special sectors
	{
		if (viewz < sectors[phs].floorheight ?
		thing->z >= sectors[heightsec].floorheight :
		gzt < sectors[heightsec].floorheight)
			return;
		if (viewz > sectors[phs].ceilingheight ?
		gzt < sectors[heightsec].ceilingheight && viewz >= sectors[heightsec].ceilingheight :
		thing->z >= sectors[heightsec].ceilingheight)
			return;
	}

	// store information in a vissprite
	vis = R_NewVisSprite();
	vis->heightsec = heightsec; //SoM: 3/17/2000
	vis->mobjflags = thing->flags;
	vis->scale = yscale; //<<detailshift;
	vis->sortscale = sortscale;
	vis->dispoffset = dispoffset; // Monster Iestyn: 23/11/15
	vis->gx = thing->x;
	vis->gy = thing->y;
	vis->gz = gz;
	vis->gzt = gzt;
	vis->thingheight = thing->height;
	vis->pz = thing->z;
	vis->pzt = vis->pz + vis->thingheight;
	vis->texturemid = vis->gzt - viewz;
	vis->scalestep = scalestep;

	vis->mobj = thing; // Easy access! Tails 06-07-2002

	vis->x1 = x1 < 0 ? 0 : x1;
	vis->x2 = x2 >= viewwidth ? viewwidth-1 : x2;

	// PORTAL SEMI-CLIPPING
	if (portalrender)
	{
		if (vis->x1 < portalclipstart)
			vis->x1 = portalclipstart;
		if (vis->x2 > portalclipend)
			vis->x2 = portalclipend;
	}

	vis->xscale = xscale; //SoM: 4/17/2000
	vis->sector = thing->subsector->sector;
	vis->szt = (INT16)((centeryfrac - FixedMul(vis->gzt - viewz, sortscale))>>FRACBITS);
	vis->sz = (INT16)((centeryfrac - FixedMul(vis->gz - viewz, sortscale))>>FRACBITS);
	vis->cut = cut;
	if (thing->subsector->sector->numlights)
		vis->extra_colormap = thing->subsector->sector->lightlist[light].extra_colormap;
	else
		vis->extra_colormap = thing->subsector->sector->extra_colormap;

	iscale = FixedDiv(FRACUNIT, xscale);

	if (flip)
	{
		vis->startfrac = spritecachedinfo[lump].width-1;
		vis->xiscale = -iscale;
	}
	else
	{
		vis->startfrac = 0;
		vis->xiscale = iscale;
	}

	if (vis->x1 > x1)
	{
		vis->startfrac += FixedDiv(vis->xiscale, this_scale)*(vis->x1-x1);
		vis->scale += scalestep*(vis->x1 - x1);
	}

	//Fab: lumppat is the lump number of the patch to use, this is different
	//     than lumpid for sprites-in-pwad : the graphics are patched
	vis->patch = sprframe->lumppat[rot];

//
// determine the colormap (lightlevel & special effects)
//
	vis->transmap = NULL;

	// specific translucency
	if (!cv_translucency.value)
		; // no translucency
	else if (oldthing->flags2 & MF2_SHADOW) // actually only the player should use this (temporary invisibility)
		vis->transmap = transtables + ((tr_trans80-1)<<FF_TRANSSHIFT); // because now the translucency is set through FF_TRANSMASK
	else if (oldthing->frame & FF_TRANSMASK)
		vis->transmap = transtables + (oldthing->frame & FF_TRANSMASK) - 0x10000;

	if ((oldthing->frame & FF_FULLBRIGHT) || (oldthing->flags2 & MF2_SHADOW))
		vis->cut |= SC_FULLBRIGHT;

	if (vis->cut & SC_FULLBRIGHT
		&& (!vis->extra_colormap || !vis->extra_colormap->fog))
	{
		// full bright: goggles
		vis->colormap = colormaps;
	}
	else
	{
		// diminished light
		lindex = FixedMul(xscale, FixedDiv(640, vid.width))>>(LIGHTSCALESHIFT);

		if (lindex >= MAXLIGHTSCALE)
			lindex = MAXLIGHTSCALE-1;

		vis->colormap = spritelights[lindex];
	}

	if (vflip)
		vis->cut |= SC_VFLIP;

	if (thing->subsector->sector->numlights)
		R_SplitSprite(vis);

	// Debug
	++objectsdrawn;
}

static void R_ProjectPrecipitationSprite(precipmobj_t *thing)
{
	fixed_t tr_x, tr_y;
	fixed_t gxt, gyt;
	fixed_t tx, tz;
	fixed_t xscale, yscale; //added : 02-02-98 : aaargll..if I were a math-guy!!!

	INT32 x1, x2;

	spritedef_t *sprdef;
	spriteframe_t *sprframe;
	size_t lump;

	vissprite_t *vis;

	fixed_t iscale;

	//SoM: 3/17/2000
	fixed_t gz, gzt;

	// transform the origin point
	tr_x = thing->x - viewx;
	tr_y = thing->y - viewy;

	gxt = FixedMul(tr_x, viewcos);
	gyt = -FixedMul(tr_y, viewsin);

	tz = gxt - gyt;

	// thing is behind view plane?
	if (tz < MINZ)
		return;

	gxt = -FixedMul(tr_x, viewsin);
	gyt = FixedMul(tr_y, viewcos);
	tx = -(gyt + gxt);

	// too far off the side?
	if (abs(tx) > tz<<2)
		return;

	// aspect ratio stuff :
	xscale = FixedDiv(projection, tz);
	yscale = FixedDiv(projectiony, tz);

	// decide which patch to use for sprite relative to player
#ifdef RANGECHECK
	if ((unsigned)thing->sprite >= numsprites)
		I_Error("R_ProjectPrecipitationSprite: invalid sprite number %d ",
			thing->sprite);
#endif

	sprdef = &sprites[thing->sprite];

#ifdef RANGECHECK
	if ((UINT8)(thing->frame&FF_FRAMEMASK) >= sprdef->numframes)
		I_Error("R_ProjectPrecipitationSprite: invalid sprite frame %d : %d for %s",
			thing->sprite, thing->frame, sprnames[thing->sprite]);
#endif

	sprframe = &sprdef->spriteframes[thing->frame & FF_FRAMEMASK];

#ifdef PARANOIA
	if (!sprframe)
		I_Error("R_ProjectPrecipitationSprite: sprframes NULL for sprite %d\n", thing->sprite);
#endif

	// use single rotation for all views
	lump = sprframe->lumpid[0];     //Fab: see note above

	// calculate edges of the shape
	tx -= spritecachedinfo[lump].offset;
	x1 = (centerxfrac + FixedMul (tx,xscale)) >>FRACBITS;

	// off the right side?
	if (x1 > viewwidth)
		return;

	tx += spritecachedinfo[lump].width;
	x2 = ((centerxfrac + FixedMul (tx,xscale)) >>FRACBITS) - 1;

	// off the left side
	if (x2 < 0)
		return;

	// PORTAL SPRITE CLIPPING
	if (portalrender)
	{
		if (x2 < portalclipstart || x1 > portalclipend)
			return;

		if (P_PointOnLineSide(thing->x, thing->y, portalclipline) != 0)
			return;
	}

	//SoM: 3/17/2000: Disregard sprites that are out of view..
	gzt = thing->z + spritecachedinfo[lump].topoffset;
	gz = gzt - spritecachedinfo[lump].height;

	if (thing->subsector->sector->cullheight)
	{
		if (R_DoCulling(thing->subsector->sector->cullheight, viewsector->cullheight, viewz, gz, gzt))
			return;
	}

	// store information in a vissprite
	vis = R_NewVisSprite();
	vis->scale = vis->sortscale = yscale; //<<detailshift;
	vis->dispoffset = 0; // Monster Iestyn: 23/11/15
	vis->gx = thing->x;
	vis->gy = thing->y;
	vis->gz = gz;
	vis->gzt = gzt;
	vis->thingheight = 4*FRACUNIT;
	vis->pz = thing->z;
	vis->pzt = vis->pz + vis->thingheight;
	vis->texturemid = vis->gzt - viewz;
	vis->scalestep = 0;

	vis->x1 = x1 < 0 ? 0 : x1;
	vis->x2 = x2 >= viewwidth ? viewwidth-1 : x2;

	// PORTAL SEMI-CLIPPING
	if (portalrender)
	{
		if (vis->x1 < portalclipstart)
			vis->x1 = portalclipstart;
		if (vis->x2 > portalclipend)
			vis->x2 = portalclipend;
	}

	vis->xscale = xscale; //SoM: 4/17/2000
	vis->sector = thing->subsector->sector;
	vis->szt = (INT16)((centeryfrac - FixedMul(vis->gzt - viewz, yscale))>>FRACBITS);
	vis->sz = (INT16)((centeryfrac - FixedMul(vis->gz - viewz, yscale))>>FRACBITS);

	iscale = FixedDiv(FRACUNIT, xscale);

	vis->startfrac = 0;
	vis->xiscale = iscale;

	if (vis->x1 > x1)
		vis->startfrac += vis->xiscale*(vis->x1-x1);

	//Fab: lumppat is the lump number of the patch to use, this is different
	//     than lumpid for sprites-in-pwad : the graphics are patched
	vis->patch = sprframe->lumppat[0];

	// specific translucency
	if (thing->frame & FF_TRANSMASK)
		vis->transmap = (thing->frame & FF_TRANSMASK) - 0x10000 + transtables;
	else
		vis->transmap = NULL;

	vis->mobj = (mobj_t *)thing;
	vis->mobjflags = 0;
	vis->cut = SC_PRECIP;
	vis->extra_colormap = thing->subsector->sector->extra_colormap;
	vis->heightsec = thing->subsector->sector->heightsec;

	// Fullbright
	vis->colormap = colormaps;
}

// R_AddSprites
// During BSP traversal, this adds sprites by sector.
//
void R_AddSprites(sector_t *sec, INT32 lightlevel)
{
	mobj_t *thing;
	precipmobj_t *precipthing; // Tails 08-25-2002
	INT32 lightnum;
	fixed_t approx_dist, limit_dist;

	if (rendermode != render_soft)
		return;

	// BSP is traversed by subsector.
	// A sector might have been split into several
	//  subsectors during BSP building.
	// Thus we check whether its already added.
	if (sec->validcount == validcount)
		return;

	// Well, now it will be done.
	sec->validcount = validcount;

	if (!sec->numlights)
	{
		if (sec->heightsec == -1) lightlevel = sec->lightlevel;

		lightnum = (lightlevel >> LIGHTSEGSHIFT);

		if (lightnum < 0)
			spritelights = scalelight[0];
		else if (lightnum >= LIGHTLEVELS)
			spritelights = scalelight[LIGHTLEVELS-1];
		else
			spritelights = scalelight[lightnum];
	}

	// Handle all things in sector.
	// If a limit exists, handle things a tiny bit different.
	if ((limit_dist = (fixed_t)((maptol & TOL_NIGHTS) ? cv_drawdist_nights.value : cv_drawdist.value) << FRACBITS))
	{
		for (thing = sec->thinglist; thing; thing = thing->snext)
		{
			if (thing->sprite == SPR_NULL || thing->flags2 & MF2_DONTDRAW)
				continue;

			approx_dist = P_AproxDistance(viewx-thing->x, viewy-thing->y);

			if (approx_dist <= limit_dist)
				R_ProjectSprite(thing);
		}
	}
	else
	{
		// Draw everything in sector, no checks
		for (thing = sec->thinglist; thing; thing = thing->snext)
			if (!(thing->sprite == SPR_NULL || thing->flags2 & MF2_DONTDRAW))
				R_ProjectSprite(thing);
	}

	// Someone seriously wants infinite draw distance for precipitation?
	if ((limit_dist = (fixed_t)cv_drawdist_precip.value << FRACBITS))
	{
		for (precipthing = sec->preciplist; precipthing; precipthing = precipthing->snext)
		{
			if (precipthing->precipflags & PCF_INVISIBLE)
				continue;

			approx_dist = P_AproxDistance(viewx-precipthing->x, viewy-precipthing->y);

			if (approx_dist <= limit_dist)
				R_ProjectPrecipitationSprite(precipthing);
		}
	}
	else
	{
		// Draw everything in sector, no checks
		for (precipthing = sec->preciplist; precipthing; precipthing = precipthing->snext)
			if (!(precipthing->precipflags & PCF_INVISIBLE))
				R_ProjectPrecipitationSprite(precipthing);
	}
}

//
// R_SortVisSprites
//
static vissprite_t vsprsortedhead;

void R_SortVisSprites(void)
{
	UINT32       i, linkedvissprites = 0;
	vissprite_t *ds, *dsprev, *dsnext, *dsfirst;
	vissprite_t *best = NULL;
	vissprite_t  unsorted;
	fixed_t      bestscale;
	INT32        bestdispoffset;

	if (!visspritecount)
		return;

	unsorted.next = unsorted.prev = &unsorted;

	dsfirst = R_GetVisSprite(0);

	// The first's prev and last's next will be set to
	// nonsense, but are fixed in a moment
	for (i = 0, dsnext = dsfirst, ds = NULL; i < visspritecount; i++)
	{
		dsprev = ds;
		ds = dsnext;
		if (i < visspritecount - 1) dsnext = R_GetVisSprite(i + 1);

		ds->next = dsnext;
		ds->prev = dsprev;
		ds->linkdraw = NULL;
	}

	// Fix first and last. ds still points to the last one after the loop
	dsfirst->prev = &unsorted;
	unsorted.next = dsfirst;
	if (ds)
	{
		ds->next = &unsorted;
		ds->linkdraw = NULL;
	}
	unsorted.prev = ds;

	// bundle linkdraw
	for (ds = unsorted.prev; ds != &unsorted; ds = ds->prev)
	{
		if (!(ds->cut & SC_LINKDRAW))
			continue;

		// reuse dsfirst...
		for (dsfirst = unsorted.prev; dsfirst != &unsorted; dsfirst = dsfirst->prev)
		{
			// don't connect if it's also a link
			if (dsfirst->cut & SC_LINKDRAW)
				continue;

			// don't connect if it's not the tracer
			if (dsfirst->mobj != ds->mobj)
				continue;

			// don't connect if the tracer's top is cut off, but lower than the link's top
			if ((dsfirst->cut & SC_TOP)
			&& dsfirst->szt > ds->szt)
				continue;

			// don't connect if the tracer's bottom is cut off, but higher than the link's bottom
			if ((dsfirst->cut & SC_BOTTOM)
			&& dsfirst->sz < ds->sz)
				continue;

			break;
		}

		// remove from chain
		ds->next->prev = ds->prev;
		ds->prev->next = ds->next;
		linkedvissprites++;

		if (dsfirst != &unsorted)
		{
			if (!(ds->cut & SC_FULLBRIGHT))
				ds->colormap = dsfirst->colormap;
			ds->extra_colormap = dsfirst->extra_colormap;

			// reusing dsnext...
			dsnext = dsfirst->linkdraw;

			if (!dsnext || ds->dispoffset < dsnext->dispoffset)
			{
				ds->next = dsnext;
				dsfirst->linkdraw = ds;
			}
			else
			{
				for (; dsnext->next != NULL; dsnext = dsnext->next)
					if (ds->dispoffset < dsnext->next->dispoffset)
						break;
				ds->next = dsnext->next;
				dsnext->next = ds;
			}
		}
	}

	// pull the vissprites out by scale
	vsprsortedhead.next = vsprsortedhead.prev = &vsprsortedhead;
	for (i = 0; i < visspritecount-linkedvissprites; i++)
	{
		bestscale = bestdispoffset = INT32_MAX;
		for (ds = unsorted.next; ds != &unsorted; ds = ds->next)
		{
#ifdef PARANOIA
			if (ds->cut & SC_LINKDRAW)
				I_Error("R_SortVisSprites: no link or discardal made for linkdraw!");
#endif

			if (ds->sortscale < bestscale)
			{
				bestscale = ds->sortscale;
				bestdispoffset = ds->dispoffset;
				best = ds;
			}
			// order visprites of same scale by dispoffset, smallest first
			else if (ds->sortscale == bestscale && ds->dispoffset < bestdispoffset)
			{
				bestdispoffset = ds->dispoffset;
				best = ds;
			}
		}
		best->next->prev = best->prev;
		best->prev->next = best->next;
		best->next = &vsprsortedhead;
		best->prev = vsprsortedhead.prev;
		vsprsortedhead.prev->next = best;
		vsprsortedhead.prev = best;
	}
}

//
// R_CreateDrawNodes
// Creates and sorts a list of drawnodes for the scene being rendered.
static drawnode_t *R_CreateDrawNode(drawnode_t *link);

static drawnode_t nodebankhead;
static drawnode_t nodehead;

static void R_CreateDrawNodes(void)
{
	drawnode_t *entry;
	drawseg_t *ds;
	INT32 i, p, best, x1, x2;
	fixed_t bestdelta, delta;
	vissprite_t *rover;
	drawnode_t *r2;
	visplane_t *plane;
	INT32 sintersect;
	fixed_t scale = 0;

	// Add the 3D floors, thicksides, and masked textures...
	for (ds = ds_p; ds-- > drawsegs ;)
	{
		if (ds->numthicksides)
		{
			for (i = 0; i < ds->numthicksides; i++)
			{
				entry = R_CreateDrawNode(&nodehead);
				entry->thickseg = ds;
				entry->ffloor = ds->thicksides[i];
			}
		}
#ifdef POLYOBJECTS_PLANES
		// Check for a polyobject plane, but only if this is a front line
		if (ds->curline->polyseg && ds->curline->polyseg->visplane && !ds->curline->side) {
			plane = ds->curline->polyseg->visplane;
			R_PlaneBounds(plane);

			if (plane->low < con_clipviewtop || plane->high > vid.height || plane->high > plane->low)
				;
			else {
				// Put it in!
				entry = R_CreateDrawNode(&nodehead);
				entry->plane = plane;
				entry->seg = ds;
			}
			ds->curline->polyseg->visplane = NULL;
		}
#endif
		if (ds->maskedtexturecol)
		{
			entry = R_CreateDrawNode(&nodehead);
			entry->seg = ds;
		}
		if (ds->numffloorplanes)
		{
			for (i = 0; i < ds->numffloorplanes; i++)
			{
				best = -1;
				bestdelta = 0;
				for (p = 0; p < ds->numffloorplanes; p++)
				{
					if (!ds->ffloorplanes[p])
						continue;
					plane = ds->ffloorplanes[p];
					R_PlaneBounds(plane);

					if (plane->low < con_clipviewtop || plane->high > vid.height || plane->high > plane->low || plane->polyobj)
					{
						ds->ffloorplanes[p] = NULL;
						continue;
					}

					delta = abs(plane->height - viewz);
					if (delta > bestdelta)
					{
						best = p;
						bestdelta = delta;
					}
				}
				if (best != -1)
				{
					entry = R_CreateDrawNode(&nodehead);
					entry->plane = ds->ffloorplanes[best];
					entry->seg = ds;
					ds->ffloorplanes[best] = NULL;
				}
				else
					break;
			}
		}
	}

#ifdef POLYOBJECTS_PLANES
	// find all the remaining polyobject planes and add them on the end of the list
	// probably this is a terrible idea if we wanted them to be sorted properly
	// but it works getting them in for now
	for (i = 0; i < numPolyObjects; i++)
	{
		if (!PolyObjects[i].visplane)
			continue;
		plane = PolyObjects[i].visplane;
		R_PlaneBounds(plane);

		if (plane->low < con_clipviewtop || plane->high > vid.height || plane->high > plane->low)
		{
			PolyObjects[i].visplane = NULL;
			continue;
		}
		entry = R_CreateDrawNode(&nodehead);
		entry->plane = plane;
		// note: no seg is set, for what should be obvious reasons
		PolyObjects[i].visplane = NULL;
	}
#endif

	if (visspritecount == 0)
		return;

	R_SortVisSprites();
	for (rover = vsprsortedhead.prev; rover != &vsprsortedhead; rover = rover->prev)
	{
		if (rover->szt > vid.height || rover->sz < 0)
			continue;

		sintersect = (rover->x1 + rover->x2) / 2;

		for (r2 = nodehead.next; r2 != &nodehead; r2 = r2->next)
		{
			if (r2->plane)
			{
				fixed_t planeobjectz, planecameraz;
				if (r2->plane->minx > rover->x2 || r2->plane->maxx < rover->x1)
					continue;
				if (rover->szt > r2->plane->low || rover->sz < r2->plane->high)
					continue;

#ifdef ESLOPE
				// Effective height may be different for each comparison in the case of slopes
				if (r2->plane->slope) {
					planeobjectz = P_GetZAt(r2->plane->slope, rover->gx, rover->gy);
					planecameraz = P_GetZAt(r2->plane->slope, viewx, viewy);
				} else
#endif
					planeobjectz = planecameraz = r2->plane->height;

				if (rover->mobjflags & MF_NOCLIPHEIGHT)
				{
					//Objects with NOCLIPHEIGHT can appear halfway in.
					if (planecameraz < viewz && rover->pz+(rover->thingheight/2) >= planeobjectz)
						continue;
					if (planecameraz > viewz && rover->pzt-(rover->thingheight/2) <= planeobjectz)
						continue;
				}
				else
				{
					if (planecameraz < viewz && rover->pz >= planeobjectz)
						continue;
					if (planecameraz > viewz && rover->pzt <= planeobjectz)
						continue;
				}

				// SoM: NOTE: Because a visplane's shape and scale is not directly
				// bound to any single linedef, a simple poll of it's frontscale is
				// not adequate. We must check the entire frontscale array for any
				// part that is in front of the sprite.

				x1 = rover->x1;
				x2 = rover->x2;
				if (x1 < r2->plane->minx) x1 = r2->plane->minx;
				if (x2 > r2->plane->maxx) x2 = r2->plane->maxx;

				if (r2->seg) // if no seg set, assume the whole thing is in front or something stupid
				{
					for (i = x1; i <= x2; i++)
					{
						if (r2->seg->frontscale[i] > rover->sortscale)
							break;
					}
					if (i > x2)
						continue;
				}

				entry = R_CreateDrawNode(NULL);
				(entry->prev = r2->prev)->next = entry;
				(entry->next = r2)->prev = entry;
				entry->sprite = rover;
				break;
			}
			else if (r2->thickseg)
			{
				fixed_t topplaneobjectz, topplanecameraz, botplaneobjectz, botplanecameraz;
				if (rover->x1 > r2->thickseg->x2 || rover->x2 < r2->thickseg->x1)
					continue;

				scale = r2->thickseg->scale1 > r2->thickseg->scale2 ? r2->thickseg->scale1 : r2->thickseg->scale2;
				if (scale <= rover->sortscale)
					continue;
				scale = r2->thickseg->scale1 + (r2->thickseg->scalestep * (sintersect - r2->thickseg->x1));
				if (scale <= rover->sortscale)
					continue;

#ifdef ESLOPE
				if (*r2->ffloor->t_slope) {
					topplaneobjectz = P_GetZAt(*r2->ffloor->t_slope, rover->gx, rover->gy);
					topplanecameraz = P_GetZAt(*r2->ffloor->t_slope, viewx, viewy);
				} else
#endif
					topplaneobjectz = topplanecameraz = *r2->ffloor->topheight;

#ifdef ESLOPE
				if (*r2->ffloor->b_slope) {
					botplaneobjectz = P_GetZAt(*r2->ffloor->b_slope, rover->gx, rover->gy);
					botplanecameraz = P_GetZAt(*r2->ffloor->b_slope, viewx, viewy);
				} else
#endif
					botplaneobjectz = botplanecameraz = *r2->ffloor->bottomheight;

				if ((topplanecameraz > viewz && botplanecameraz < viewz) ||
				    (topplanecameraz < viewz && rover->gzt < topplaneobjectz) ||
				    (botplanecameraz > viewz && rover->gz > botplaneobjectz))
				{
					entry = R_CreateDrawNode(NULL);
					(entry->prev = r2->prev)->next = entry;
					(entry->next = r2)->prev = entry;
					entry->sprite = rover;
					break;
				}
			}
			else if (r2->seg)
			{
				if (rover->x1 > r2->seg->x2 || rover->x2 < r2->seg->x1)
					continue;

				scale = r2->seg->scale1 > r2->seg->scale2 ? r2->seg->scale1 : r2->seg->scale2;
				if (scale <= rover->sortscale)
					continue;
				scale = r2->seg->scale1 + (r2->seg->scalestep * (sintersect - r2->seg->x1));

				if (rover->sortscale < scale)
				{
					entry = R_CreateDrawNode(NULL);
					(entry->prev = r2->prev)->next = entry;
					(entry->next = r2)->prev = entry;
					entry->sprite = rover;
					break;
				}
			}
			else if (r2->sprite)
			{
				if (r2->sprite->x1 > rover->x2 || r2->sprite->x2 < rover->x1)
					continue;
				if (r2->sprite->szt > rover->sz || r2->sprite->sz < rover->szt)
					continue;

				if (r2->sprite->sortscale > rover->sortscale
				 || (r2->sprite->sortscale == rover->sortscale && r2->sprite->dispoffset > rover->dispoffset))
				{
					entry = R_CreateDrawNode(NULL);
					(entry->prev = r2->prev)->next = entry;
					(entry->next = r2)->prev = entry;
					entry->sprite = rover;
					break;
				}
			}
		}
		if (r2 == &nodehead)
		{
			entry = R_CreateDrawNode(&nodehead);
			entry->sprite = rover;
		}
	}
}

static drawnode_t *R_CreateDrawNode(drawnode_t *link)
{
	drawnode_t *node = nodebankhead.next;

	if (node == &nodebankhead)
	{
		node = malloc(sizeof (*node));
		if (!node)
			I_Error("No more free memory to CreateDrawNode");
	}
	else
		(nodebankhead.next = node->next)->prev = &nodebankhead;

	if (link)
	{
		node->next = link;
		node->prev = link->prev;
		link->prev->next = node;
		link->prev = node;
	}

	node->plane = NULL;
	node->seg = NULL;
	node->thickseg = NULL;
	node->ffloor = NULL;
	node->sprite = NULL;
	return node;
}

static void R_DoneWithNode(drawnode_t *node)
{
	(node->next->prev = node->prev)->next = node->next;
	(node->next = nodebankhead.next)->prev = node;
	(node->prev = &nodebankhead)->next = node;
}

static void R_ClearDrawNodes(void)
{
	drawnode_t *rover;
	drawnode_t *next;

	for (rover = nodehead.next; rover != &nodehead ;)
	{
		next = rover->next;
		R_DoneWithNode(rover);
		rover = next;
	}

	nodehead.next = nodehead.prev = &nodehead;
}

void R_InitDrawNodes(void)
{
	nodebankhead.next = nodebankhead.prev = &nodebankhead;
	nodehead.next = nodehead.prev = &nodehead;
}

//
// R_DrawSprite
//
//Fab : 26-04-98:
// NOTE : uses con_clipviewtop, so that when console is on,
//        don't draw the part of sprites hidden under the console
static void R_DrawSprite(vissprite_t *spr)
{
	mfloorclip = spr->clipbot;
	mceilingclip = spr->cliptop;
	R_DrawVisSprite(spr);
}

// Special drawer for precipitation sprites Tails 08-18-2002
static void R_DrawPrecipitationSprite(vissprite_t *spr)
{
	mfloorclip = spr->clipbot;
	mceilingclip = spr->cliptop;
	R_DrawPrecipitationVisSprite(spr);
}

// R_ClipSprites
// Clips vissprites without drawing, so that portals can work. -Red
void R_ClipSprites(void)
{
	vissprite_t *spr;
	for (; clippedvissprites < visspritecount; clippedvissprites++)
	{
		drawseg_t *ds;
		INT32		x;
		INT32		r1;
		INT32		r2;
		fixed_t		scale;
		fixed_t		lowscale;
		INT32		silhouette;

		spr = R_GetVisSprite(clippedvissprites);

		for (x = spr->x1; x <= spr->x2; x++)
			spr->clipbot[x] = spr->cliptop[x] = -2;

		// Scan drawsegs from end to start for obscuring segs.
		// The first drawseg that has a greater scale
		//  is the clip seg.
		//SoM: 4/8/2000:
		// Pointer check was originally nonportable
		// and buggy, by going past LEFT end of array:

		//    for (ds = ds_p-1; ds >= drawsegs; ds--)    old buggy code
		for (ds = ds_p; ds-- > drawsegs ;)
		{
			// determine if the drawseg obscures the sprite
			if (ds->x1 > spr->x2 ||
			    ds->x2 < spr->x1 ||
			    (!ds->silhouette
			     && !ds->maskedtexturecol))
			{
				// does not cover sprite
				continue;
			}

			if (ds->portalpass > 0 && ds->portalpass <= portalrender)
				continue; // is a portal

			r1 = ds->x1 < spr->x1 ? spr->x1 : ds->x1;
			r2 = ds->x2 > spr->x2 ? spr->x2 : ds->x2;

			if (ds->scale1 > ds->scale2)
			{
				lowscale = ds->scale2;
				scale = ds->scale1;
			}
			else
			{
				lowscale = ds->scale1;
				scale = ds->scale2;
			}

			if (scale < spr->sortscale ||
			    (lowscale < spr->sortscale &&
			     !R_PointOnSegSide (spr->gx, spr->gy, ds->curline)))
			{
				// masked mid texture?
				/*if (ds->maskedtexturecol)
					R_RenderMaskedSegRange (ds, r1, r2);*/
				// seg is behind sprite
				continue;
			}

			// clip this piece of the sprite
			silhouette = ds->silhouette;

			if (spr->gz >= ds->bsilheight)
				silhouette &= ~SIL_BOTTOM;

			if (spr->gzt <= ds->tsilheight)
				silhouette &= ~SIL_TOP;

			if (silhouette == SIL_BOTTOM)
			{
				// bottom sil
				for (x = r1; x <= r2; x++)
					if (spr->clipbot[x] == -2)
						spr->clipbot[x] = ds->sprbottomclip[x];
			}
			else if (silhouette == SIL_TOP)
			{
				// top sil
				for (x = r1; x <= r2; x++)
					if (spr->cliptop[x] == -2)
						spr->cliptop[x] = ds->sprtopclip[x];
			}
			else if (silhouette == (SIL_TOP|SIL_BOTTOM))
			{
				// both
				for (x = r1; x <= r2; x++)
				{
					if (spr->clipbot[x] == -2)
						spr->clipbot[x] = ds->sprbottomclip[x];
					if (spr->cliptop[x] == -2)
						spr->cliptop[x] = ds->sprtopclip[x];
				}
			}
		}
		//SoM: 3/17/2000: Clip sprites in water.
		if (spr->heightsec != -1)  // only things in specially marked sectors
		{
			fixed_t mh, h;
			INT32 phs = viewplayer->mo->subsector->sector->heightsec;
			if ((mh = sectors[spr->heightsec].floorheight) > spr->gz &&
				(h = centeryfrac - FixedMul(mh -= viewz, spr->sortscale)) >= 0 &&
				(h >>= FRACBITS) < viewheight)
			{
				if (mh <= 0 || (phs != -1 && viewz > sectors[phs].floorheight))
				{                          // clip bottom
					for (x = spr->x1; x <= spr->x2; x++)
						if (spr->clipbot[x] == -2 || h < spr->clipbot[x])
							spr->clipbot[x] = (INT16)h;
				}
				else						// clip top
				{
					for (x = spr->x1; x <= spr->x2; x++)
						if (spr->cliptop[x] == -2 || h > spr->cliptop[x])
							spr->cliptop[x] = (INT16)h;
				}
			}

			if ((mh = sectors[spr->heightsec].ceilingheight) < spr->gzt &&
			    (h = centeryfrac - FixedMul(mh-viewz, spr->sortscale)) >= 0 &&
			    (h >>= FRACBITS) < viewheight)
			{
				if (phs != -1 && viewz >= sectors[phs].ceilingheight)
				{                         // clip bottom
					for (x = spr->x1; x <= spr->x2; x++)
						if (spr->clipbot[x] == -2 || h < spr->clipbot[x])
							spr->clipbot[x] = (INT16)h;
				}
				else                       // clip top
				{
					for (x = spr->x1; x <= spr->x2; x++)
						if (spr->cliptop[x] == -2 || h > spr->cliptop[x])
							spr->cliptop[x] = (INT16)h;
				}
			}
		}
		if (spr->cut & SC_TOP && spr->cut & SC_BOTTOM)
		{
			for (x = spr->x1; x <= spr->x2; x++)
			{
				if (spr->cliptop[x] == -2 || spr->szt > spr->cliptop[x])
					spr->cliptop[x] = spr->szt;

				if (spr->clipbot[x] == -2 || spr->sz < spr->clipbot[x])
					spr->clipbot[x] = spr->sz;
			}
		}
		else if (spr->cut & SC_TOP)
		{
			for (x = spr->x1; x <= spr->x2; x++)
			{
				if (spr->cliptop[x] == -2 || spr->szt > spr->cliptop[x])
					spr->cliptop[x] = spr->szt;
			}
		}
		else if (spr->cut & SC_BOTTOM)
		{
			for (x = spr->x1; x <= spr->x2; x++)
			{
				if (spr->clipbot[x] == -2 || spr->sz < spr->clipbot[x])
					spr->clipbot[x] = spr->sz;
			}
		}

		// all clipping has been performed, so store the values - what, did you think we were drawing them NOW?

		// check for unclipped columns
		for (x = spr->x1; x <= spr->x2; x++)
		{
			if (spr->clipbot[x] == -2)
				spr->clipbot[x] = (INT16)viewheight;

			if (spr->cliptop[x] == -2)
				//Fab : 26-04-98: was -1, now clips against console bottom
				spr->cliptop[x] = (INT16)con_clipviewtop;
		}
	}
}

//
// R_DrawMasked
//
void R_DrawMasked(void)
{
	drawnode_t *r2;
	drawnode_t *next;

	R_CreateDrawNodes();

	for (r2 = nodehead.next; r2 != &nodehead; r2 = r2->next)
	{
		if (r2->plane)
		{
			next = r2->prev;
			R_DrawSinglePlane(r2->plane);
			R_DoneWithNode(r2);
			r2 = next;
		}
		else if (r2->seg && r2->seg->maskedtexturecol != NULL)
		{
			next = r2->prev;
			R_RenderMaskedSegRange(r2->seg, r2->seg->x1, r2->seg->x2);
			r2->seg->maskedtexturecol = NULL;
			R_DoneWithNode(r2);
			r2 = next;
		}
		else if (r2->thickseg)
		{
			next = r2->prev;
			R_RenderThickSideRange(r2->thickseg, r2->thickseg->x1, r2->thickseg->x2, r2->ffloor);
			R_DoneWithNode(r2);
			r2 = next;
		}
		else if (r2->sprite)
		{
			next = r2->prev;

			// Tails 08-18-2002
			if (r2->sprite->cut & SC_PRECIP)
				R_DrawPrecipitationSprite(r2->sprite);
			else if (!r2->sprite->linkdraw)
				R_DrawSprite(r2->sprite);
			else // unbundle linkdraw
			{
				vissprite_t *ds = r2->sprite->linkdraw;

				for (;
				(ds != NULL && r2->sprite->dispoffset > ds->dispoffset);
				ds = ds->next)
					R_DrawSprite(ds);

				R_DrawSprite(r2->sprite);

				for (; ds != NULL; ds = ds->next)
					R_DrawSprite(ds);
			}

			R_DoneWithNode(r2);
			r2 = next;
		}
	}
	R_ClearDrawNodes();
}

// ==========================================================================
//
//                              SKINS CODE
//
// ==========================================================================

INT32 numskins = 0;
skin_t skins[MAXSKINS+1];
// FIXTHIS: don't work because it must be inistilised before the config load
//#define SKINVALUES
#ifdef SKINVALUES
CV_PossibleValue_t skin_cons_t[MAXSKINS+1];
#endif

//
// P_GetSkinSprite2
// For non-super players, tries each sprite2's immediate predecessor until it finds one with a number of frames or ends up at standing.
// For super players, does the same as above - but tries the super equivalent for each sprite2 before the non-super version.
//

UINT8 P_GetSkinSprite2(skin_t *skin, UINT8 spr2, player_t *player)
{
	UINT8 super = 0, i = 0;

	if (!skin)
		return 0;

	while (!(skin->sprites[spr2].numframes)
		&& spr2 != SPR2_STND
		&& ++i != 32) // recursion limiter
	{
		if (spr2 & FF_SPR2SUPER)
		{
			super = FF_SPR2SUPER;
			spr2 &= ~FF_SPR2SUPER;
			continue;
		}

		switch(spr2)
		{

		// Normal special cases.
		case SPR2_JUMP:
			spr2 = ((player
					? player->charflags
					: skin->flags)
					& SF_NOJUMPSPIN) ? SPR2_SPNG : SPR2_ROLL;
			break;
		case SPR2_TIRE:
			spr2 = (player && player->charability == CA_SWIM) ? SPR2_SWIM : SPR2_FLY;
			break;

		// Use the handy list, that's what it's there for!
		default:
			spr2 = spr2defaults[spr2];
			break;
		}

		spr2 |= super;
	}

	return spr2;
}

static void Sk_SetDefaultValue(skin_t *skin)
{
	INT32 i;
	//
	// set default skin values
	//
	memset(skin, 0, sizeof (skin_t));
	snprintf(skin->name,
		sizeof skin->name, "skin %u", (UINT32)(skin-skins));
	skin->name[sizeof skin->name - 1] = '\0';
	skin->wadnum = INT16_MAX;

	skin->flags = 0;

	strcpy(skin->realname, "Someone");
#ifdef SKINNAMEPADDING
	strcpy(skin->hudname, "  ???");
#else
	strcpy(skin->hudname, "???");
#endif
	strncpy(skin->charsel, "CHRSONIC", 8);
	strncpy(skin->face, "MISSING", 8);
	strncpy(skin->superface, "MISSING", 8);

	skin->starttranscolor = 96;
	skin->prefcolor = SKINCOLOR_GREEN;
	skin->supercolor = SKINCOLOR_SUPERGOLD1;
	skin->prefoppositecolor = 0; // use tables

	skin->normalspeed = 36<<FRACBITS;
	skin->runspeed = 28<<FRACBITS;
	skin->thrustfactor = 5;
	skin->accelstart = 96;
	skin->acceleration = 40;

	skin->ability = CA_NONE;
	skin->ability2 = CA2_SPINDASH;
	skin->jumpfactor = FRACUNIT;
	skin->actionspd = 30<<FRACBITS;
	skin->mindash = 15<<FRACBITS;
	skin->maxdash = 70<<FRACBITS;

	skin->radius = mobjinfo[MT_PLAYER].radius;
	skin->height = mobjinfo[MT_PLAYER].height;
	skin->spinheight = FixedMul(skin->height, 2*FRACUNIT/3);

	skin->shieldscale = FRACUNIT;
	skin->camerascale = FRACUNIT;

	skin->thokitem = -1;
	skin->spinitem = -1;
	skin->revitem = -1;

	skin->highresscale = FRACUNIT;

	skin->availability = 0;

	for (i = 0; i < sfx_skinsoundslot0; i++)
		if (S_sfx[i].skinsound != -1)
			skin->soundsid[S_sfx[i].skinsound] = i;
}

//
// Initialize the basic skins
//
void R_InitSkins(void)
{
#ifdef SKINVALUES
	INT32 i;

	for (i = 0; i <= MAXSKINS; i++)
	{
		skin_cons_t[i].value = 0;
		skin_cons_t[i].strvalue = NULL;
	}
#endif

	// no default skin!
	numskins = 0;
}

UINT32 R_GetSkinAvailabilities(void)
{
	INT32 s;
	UINT32 response = 0;

	for (s = 0; s < MAXSKINS; s++)
	{
		if (skins[s].availability && unlockables[skins[s].availability - 1].unlocked)
			response |= (1 << s);
	}
	return response;
}

// returns true if available in circumstances, otherwise nope
// warning don't use with an invalid skinnum other than -1 which always returns true
boolean R_SkinUsable(INT32 playernum, INT32 skinnum)
{
	return ((skinnum == -1) // Simplifies things elsewhere, since there's already plenty of checks for less-than-0...
		|| (!skins[skinnum].availability)
		|| ((playernum != -1) ? (players[playernum].availabilities & (1 << skinnum)) : (unlockables[skins[skinnum].availability - 1].unlocked))
		|| (modeattacking) // If you have someone else's run you might as well take a look
		|| (Playing() && (R_SkinAvailable(mapheaderinfo[gamemap-1]->forcecharacter) == skinnum)) // Force 1.
		|| (netgame && (cv_forceskin.value == skinnum)) // Force 2.
		);
}

// returns true if the skin name is found (loaded from pwad)
// warning return -1 if not found
INT32 R_SkinAvailable(const char *name)
{
	INT32 i;

	for (i = 0; i < numskins; i++)
	{
		// search in the skin list
		if (stricmp(skins[i].name,name)==0)
			return i;
	}
	return -1;
}

// network code calls this when a 'skin change' is received
void SetPlayerSkin(INT32 playernum, const char *skinname)
{
	INT32 i = R_SkinAvailable(skinname);
	player_t *player = &players[playernum];

	if ((i != -1) && R_SkinUsable(playernum, i))
	{
		SetPlayerSkinByNum(playernum, i);
		return;
	}

	if (P_IsLocalPlayer(player))
		CONS_Alert(CONS_WARNING, M_GetText("Skin '%s' not found.\n"), skinname);
	else if(server || adminplayer == consoleplayer)
		CONS_Alert(CONS_WARNING, M_GetText("Player %d (%s) skin '%s' not found\n"), playernum, player_names[playernum], skinname);

	SetPlayerSkinByNum(playernum, 0);
}

// Same as SetPlayerSkin, but uses the skin #.
// network code calls this when a 'skin change' is received
void SetPlayerSkinByNum(INT32 playernum, INT32 skinnum)
{
	player_t *player = &players[playernum];
	skin_t *skin = &skins[skinnum];
	UINT8 newcolor = 0;

	if (skinnum >= 0 && skinnum < numskins && R_SkinUsable(playernum, skinnum)) // Make sure it exists!
	{
		player->skin = skinnum;

		player->camerascale = skin->camerascale;
		player->shieldscale = skin->shieldscale;

		player->charability = (UINT8)skin->ability;
		player->charability2 = (UINT8)skin->ability2;

		player->charflags = (UINT32)skin->flags;

		player->thokitem = skin->thokitem < 0 ? (UINT32)mobjinfo[MT_PLAYER].painchance : (UINT32)skin->thokitem;
		player->spinitem = skin->spinitem < 0 ? (UINT32)mobjinfo[MT_PLAYER].damage : (UINT32)skin->spinitem;
		player->revitem = skin->revitem < 0 ? (mobjtype_t)mobjinfo[MT_PLAYER].raisestate : (UINT32)skin->revitem;

		player->actionspd = skin->actionspd;
		player->mindash = skin->mindash;
		player->maxdash = skin->maxdash;

		player->normalspeed = skin->normalspeed;
		player->runspeed = skin->runspeed;
		player->thrustfactor = skin->thrustfactor;
		player->accelstart = skin->accelstart;
		player->acceleration = skin->acceleration;

		player->jumpfactor = skin->jumpfactor;

		player->height = skin->height;
		player->spinheight = skin->spinheight;

		if (!(cv_debug || devparm) && !(netgame || multiplayer || demoplayback))
		{
			if (playernum == consoleplayer)
				CV_StealthSetValue(&cv_playercolor, skin->prefcolor);
			else if (playernum == secondarydisplayplayer)
				CV_StealthSetValue(&cv_playercolor2, skin->prefcolor);
			player->skincolor = newcolor = skin->prefcolor;
		}

		if (player->mo)
		{
			fixed_t radius = FixedMul(skin->radius, player->mo->scale);
			if ((player->powers[pw_carry] == CR_NIGHTSMODE) && (skin->sprites[SPR2_NGT0].numframes == 0)) // If you don't have a sprite for flying horizontally, use the default NiGHTS skin.
			{
				skin = &skins[DEFAULTNIGHTSSKIN];
				newcolor = skin->prefcolor; // will be updated in thinker to flashing
			}
			player->mo->skin = skin;
			if (newcolor)
				player->mo->color = newcolor;
			P_SetScale(player->mo, player->mo->scale);
			player->mo->radius = radius;

			P_SetPlayerMobjState(player->mo, player->mo->state-states); // Prevent visual errors when switching between skins with differing number of frames
		}
		return;
	}

	if (P_IsLocalPlayer(player))
		CONS_Alert(CONS_WARNING, M_GetText("Requested skin not found\n"));
	else if(server || adminplayer == consoleplayer)
		CONS_Alert(CONS_WARNING, "Player %d (%s) skin not found\n", playernum, player_names[playernum]);
	SetPlayerSkinByNum(playernum, 0); // not found put the sonic skin
}

//
// Add skins from a pwad, each skin preceded by 'S_SKIN' marker
//

// Does the same is in w_wad, but check only for
// the first 6 characters (this is so we can have S_SKIN1, S_SKIN2..
// for wad editors that don't like multiple resources of the same name)
//
static UINT16 W_CheckForSkinMarkerInPwad(UINT16 wadid, UINT16 startlump)
{
	UINT16 i;
	const char *S_SKIN = "S_SKIN";
	lumpinfo_t *lump_p;

	// scan forward, start at <startlump>
	if (startlump < wadfiles[wadid]->numlumps)
	{
		lump_p = wadfiles[wadid]->lumpinfo + startlump;
		for (i = startlump; i < wadfiles[wadid]->numlumps; i++, lump_p++)
			if (memcmp(lump_p->name,S_SKIN,6)==0)
				return i;
	}
	return INT16_MAX; // not found
}

#ifdef SKINNAMEPADDING
#define HUDNAMEWRITE(value) snprintf(skin->hudname, sizeof(skin->hudname), "%5s", value)
#else
#define HUDNAMEWRITE(value) STRBUFCPY(skin->hudname, value)
#endif

// turn _ into spaces and . into katana dot
#define SYMBOLCONVERT(name) for (value = name; *value; value++)\
					{\
						if (*value == '_') *value = ' ';\
						else if (*value == '.') *value = '\x1E';\
					}

//
// Patch skins from a pwad, each skin preceded by 'P_SKIN' marker
//

// Does the same is in w_wad, but check only for
// the first 6 characters (this is so we can have P_SKIN1, P_SKIN2..
// for wad editors that don't like multiple resources of the same name)
//
static UINT16 W_CheckForPatchSkinMarkerInPwad(UINT16 wadid, UINT16 startlump)
{
	UINT16 i;
	const char *P_SKIN = "P_SKIN";
	lumpinfo_t *lump_p;

	// scan forward, start at <startlump>
	if (startlump < wadfiles[wadid]->numlumps)
	{
		lump_p = wadfiles[wadid]->lumpinfo + startlump;
		for (i = startlump; i < wadfiles[wadid]->numlumps; i++, lump_p++)
			if (memcmp(lump_p->name,P_SKIN,6)==0)
				return i;
	}
	return INT16_MAX; // not found
}

static void R_LoadSkinSprites(UINT16 wadnum, UINT16 *lump, UINT16 *lastlump, skin_t *skin)
{
	UINT16 newlastlump;
	UINT8 sprite2;

	*lump += 1; // start after S_SKIN
	*lastlump = W_CheckNumForNamePwad("S_END",wadnum,*lump); // stop at S_END

	// old wadding practices die hard -- stop at S_SKIN (or P_SKIN) or S_START if they come before S_END.
	newlastlump = W_CheckForSkinMarkerInPwad(wadnum,*lump);
	if (newlastlump < *lastlump) *lastlump = newlastlump;
	newlastlump = W_CheckForPatchSkinMarkerInPwad(wadnum,*lump);
	if (newlastlump < *lastlump) *lastlump = newlastlump;
	newlastlump = W_CheckNumForNamePwad("S_START",wadnum,*lump);
	if (newlastlump < *lastlump) *lastlump = newlastlump;

	// ...and let's handle super, too
	newlastlump = W_CheckNumForNamePwad("S_SUPER",wadnum,*lump);
	if (newlastlump < *lastlump)
	{
		newlastlump++;
		// load all sprite sets we are aware of... for super!
		for (sprite2 = 0; sprite2 < free_spr2; sprite2++)
			R_AddSingleSpriteDef((spritename = spr2names[sprite2]), &skin->sprites[FF_SPR2SUPER|sprite2], wadnum, newlastlump, *lastlump);

		newlastlump--;
		*lastlump = newlastlump; // okay, make the normal sprite set loading end there
	}

	// load all sprite sets we are aware of... for normal stuff.
	for (sprite2 = 0; sprite2 < free_spr2; sprite2++)
		R_AddSingleSpriteDef((spritename = spr2names[sprite2]), &skin->sprites[sprite2], wadnum, *lump, *lastlump);

}

// returns whether found appropriate property
static boolean R_ProcessPatchableFields(skin_t *skin, char *stoken, char *value)
{
	// custom translation table
	if (!stricmp(stoken, "startcolor"))
		skin->starttranscolor = atoi(value);

#define FULLPROCESS(field) else if (!stricmp(stoken, #field)) skin->field = get_number(value);
	// character type identification
	FULLPROCESS(flags)
	FULLPROCESS(ability)
	FULLPROCESS(ability2)

	FULLPROCESS(thokitem)
	FULLPROCESS(spinitem)
	FULLPROCESS(revitem)
#undef FULLPROCESS

#define GETFRACBITS(field) else if (!stricmp(stoken, #field)) skin->field = atoi(value)<<FRACBITS;
	GETFRACBITS(normalspeed)
	GETFRACBITS(runspeed)

	GETFRACBITS(mindash)
	GETFRACBITS(maxdash)
	GETFRACBITS(actionspd)

	GETFRACBITS(radius)
	GETFRACBITS(height)
	GETFRACBITS(spinheight)
#undef GETFRACBITS

#define GETINT(field) else if (!stricmp(stoken, #field)) skin->field = atoi(value);
	GETINT(thrustfactor)
	GETINT(accelstart)
	GETINT(acceleration)
#undef GETINT

#define GETSKINCOLOR(field) else if (!stricmp(stoken, #field)) skin->field = R_GetColorByName(value);
	GETSKINCOLOR(prefcolor)
	GETSKINCOLOR(prefoppositecolor)
#undef GETSKINCOLOR
	else if (!stricmp(stoken, "supercolor"))
		skin->supercolor = R_GetSuperColorByName(value);

#define GETFLOAT(field) else if (!stricmp(stoken, #field)) skin->field = FLOAT_TO_FIXED(atof(value));
	GETFLOAT(jumpfactor)
	GETFLOAT(highresscale)
	GETFLOAT(shieldscale)
	GETFLOAT(camerascale)
#undef GETFLOAT

#define GETFLAG(field) else if (!stricmp(stoken, #field)) { \
	strupr(value); \
	if (atoi(value) || value[0] == 'T' || value[0] == 'Y') \
		skin->flags |= (SF_##field); \
	else \
		skin->flags &= ~(SF_##field); \
}
	// parameters for individual character flags
	// these are uppercase so they can be concatenated with SF_
	// 1, true, yes are all valid values
	GETFLAG(SUPER)
	GETFLAG(NOSUPERSPIN)
	GETFLAG(NOSPINDASHDUST)
	GETFLAG(HIRES)
	GETFLAG(NOSKID)
	GETFLAG(NOSPEEDADJUST)
	GETFLAG(RUNONWATER)
	GETFLAG(NOJUMPSPIN)
	GETFLAG(NOJUMPDAMAGE)
	GETFLAG(STOMPDAMAGE)
	GETFLAG(MARIODAMAGE)
	GETFLAG(MACHINE)
	GETFLAG(DASHMODE)
	GETFLAG(FASTEDGE)
	GETFLAG(MULTIABILITY)
#undef GETFLAG

	else // let's check if it's a sound, otherwise error out
	{
		boolean found = false;
		sfxenum_t i;
		size_t stokenadjust;

		// Remove the prefix. (We need to affect an adjusting variable so that we can print error messages if it's not actually a sound.)
		if ((stoken[0] == 'D' || stoken[0] == 'd') && (stoken[1] == 'S' || stoken[1] == 's')) // DS*
			stokenadjust = 2;
		else // sfx_*
			stokenadjust = 4;

		// Remove the prefix. (We can affect this directly since we're not going to use it again.)
		if ((value[0] == 'D' || value[0] == 'd') && (value[1] == 'S' || value[1] == 's')) // DS*
			value += 2;
		else // sfx_*
			value += 4;

		// copy name of sounds that are remapped
		// for this skin
		for (i = 0; i < sfx_skinsoundslot0; i++)
		{
			if (!S_sfx[i].name)
				continue;
			if (S_sfx[i].skinsound != -1
				&& !stricmp(S_sfx[i].name,
					stoken + stokenadjust))
			{
				skin->soundsid[S_sfx[i].skinsound] =
					S_AddSoundFx(value, S_sfx[i].singularity, S_sfx[i].pitch, true);
				found = true;
			}
		}
		return found;
	}
	return true;
}

//
// Find skin sprites, sounds & optional status bar face, & add them
//
void R_AddSkins(UINT16 wadnum)
{
	UINT16 lump, lastlump = 0;
	char *buf;
	char *buf2;
	char *stoken;
	char *value;
	size_t size;
	skin_t *skin;
	boolean hudname, realname, superface;

	//
	// search for all skin markers in pwad
	//

	while ((lump = W_CheckForSkinMarkerInPwad(wadnum, lastlump)) != INT16_MAX)
	{
		// advance by default
		lastlump = lump + 1;

		if (numskins > MAXSKINS)
		{
			CONS_Debug(DBG_RENDER, "ignored skin (%d skins maximum)\n", MAXSKINS);
			continue; // so we know how many skins couldn't be added
		}
		buf = W_CacheLumpNumPwad(wadnum, lump, PU_CACHE);
		size = W_LumpLengthPwad(wadnum, lump);

		// for strtok
		buf2 = malloc(size+1);
		if (!buf2)
			I_Error("R_AddSkins: No more free memory\n");
		M_Memcpy(buf2,buf,size);
		buf2[size] = '\0';

		// set defaults
		skin = &skins[numskins];
		Sk_SetDefaultValue(skin);
		skin->wadnum = wadnum;
		hudname = realname = superface = false;
		// parse
		stoken = strtok (buf2, "\r\n= ");
		while (stoken)
		{
			if ((stoken[0] == '/' && stoken[1] == '/')
				|| (stoken[0] == '#'))// skip comments
			{
				stoken = strtok(NULL, "\r\n"); // skip end of line
				goto next_token;              // find the real next token
			}

			value = strtok(NULL, "\r\n= ");

			if (!value)
				I_Error("R_AddSkins: syntax error in S_SKIN lump# %d(%s) in WAD %s\n", lump, W_CheckNameForNumPwad(wadnum,lump), wadfiles[wadnum]->filename);

			// Some of these can't go in R_ProcessPatchableFields because they have side effects for future lines.
			// Others can't go in there because we don't want them to be patchable.
			if (!stricmp(stoken, "name"))
			{
				INT32 skinnum = R_SkinAvailable(value);
				strlwr(value);
				if (skinnum == -1)
					STRBUFCPY(skin->name, value);
				// the skin name must uniquely identify a single skin
				// if the name is already used I make the name 'namex'
				// using the default skin name's number set above
				else
				{
					const size_t stringspace =
						strlen(value) + sizeof (numskins) + 1;
					char *value2 = Z_Malloc(stringspace, PU_STATIC, NULL);
					snprintf(value2, stringspace,
						"%s%d", value, numskins);
					value2[stringspace - 1] = '\0';
					if (R_SkinAvailable(value2) == -1)
						// I'm lazy so if NEW name is already used I leave the 'skin x'
						// default skin name set in Sk_SetDefaultValue
						STRBUFCPY(skin->name, value2);
					Z_Free(value2);
				}

				// copy to hudname and fullname as a default.
				if (!realname)
				{
					STRBUFCPY(skin->realname, skin->name);
					for (value = skin->realname; *value; value++)
					{
						if (*value == '_') *value = ' '; // turn _ into spaces.
						else if (*value == '.') *value = '\x1E'; // turn . into katana dot.
					}
				}
				if (!hudname)
				{
					HUDNAMEWRITE(skin->name);
					strupr(skin->hudname);
					SYMBOLCONVERT(skin->hudname)
				}
			}
			else if (!stricmp(stoken, "realname"))
			{ // Display name (eg. "Knuckles")
				realname = true;
				STRBUFCPY(skin->realname, value);
				SYMBOLCONVERT(skin->realname)
				if (!hudname)
					HUDNAMEWRITE(skin->realname);
			}
			else if (!stricmp(stoken, "hudname"))
			{ // Life icon name (eg. "K.T.E")
				hudname = true;
				HUDNAMEWRITE(value);
				SYMBOLCONVERT(skin->hudname)
				if (!realname)
					STRBUFCPY(skin->realname, skin->hudname);
			}
			else if (!stricmp(stoken, "charsel"))
			{
				strupr(value);
				strncpy(skin->charsel, value, sizeof skin->charsel);
			}
			else if (!stricmp(stoken, "face"))
			{
				strupr(value);
				strncpy(skin->face, value, sizeof skin->face);
				if (!superface)
					strncpy(skin->superface, value, sizeof skin->superface);
			}
			else if (!stricmp(stoken, "superface"))
			{
				superface = true;
				strupr(value);
				strncpy(skin->superface, value, sizeof skin->superface);
			}
			else if (!stricmp(stoken, "availability"))
			{
				skin->availability = atoi(value);
				if (skin->availability >= MAXUNLOCKABLES)
					skin->availability = 0;
				if (skin->availability)
					STRBUFCPY(unlockables[skin->availability - 1].name, skin->realname);
			}
			else if (!R_ProcessPatchableFields(skin, stoken, value))
				CONS_Debug(DBG_SETUP, "R_AddSkins: Unknown keyword '%s' in S_SKIN lump #%d (WAD %s)\n", stoken, lump, wadfiles[wadnum]->filename);

next_token:
			stoken = strtok(NULL, "\r\n= ");
		}
		free(buf2);

		// Add sprites
		R_LoadSkinSprites(wadnum, &lump, &lastlump, skin);

		R_FlushTranslationColormapCache();

		if (!skin->availability) // Safe to print...
			CONS_Printf(M_GetText("Added skin '%s'\n"), skin->name);
#ifdef SKINVALUES
		skin_cons_t[numskins].value = numskins;
		skin_cons_t[numskins].strvalue = skin->name;
#endif

		// add face graphics
		ST_LoadFaceGraphics(skin->face, skin->superface, numskins);

#ifdef HWRENDER
		if (rendermode == render_opengl)
			HWR_AddPlayerMD2(numskins);
#endif

		numskins++;
	}
	return;
}

//
// Patch skin sprites
//
void R_PatchSkins(UINT16 wadnum)
{
	UINT16 lump, lastlump = 0;
	char *buf;
	char *buf2;
	char *stoken;
	char *value;
	size_t size;
	skin_t *skin;
	boolean noskincomplain, realname, hudname;

	//
	// search for all skin patch markers in pwad
	//

	while ((lump = W_CheckForPatchSkinMarkerInPwad(wadnum, lastlump)) != INT16_MAX)
	{
		INT32 skinnum = 0;

		// advance by default
		lastlump = lump + 1;

		buf = W_CacheLumpNumPwad(wadnum, lump, PU_CACHE);
		size = W_LumpLengthPwad(wadnum, lump);

		// for strtok
		buf2 = malloc(size+1);
		if (!buf2)
			I_Error("R_PatchSkins: No more free memory\n");
		M_Memcpy(buf2,buf,size);
		buf2[size] = '\0';

		skin = NULL;
		noskincomplain = realname = hudname = false;

		/*
		Parse. Has more phases than the parser in R_AddSkins because it needs to have the patching name first (no default skin name is acceptible for patching, unlike skin creation)
		*/

		stoken = strtok(buf2, "\r\n= ");
		while (stoken)
		{
			if ((stoken[0] == '/' && stoken[1] == '/')
				|| (stoken[0] == '#'))// skip comments
			{
				stoken = strtok(NULL, "\r\n"); // skip end of line
				goto next_token;              // find the real next token
			}

			value = strtok(NULL, "\r\n= ");

			if (!value)
				I_Error("R_PatchSkins: syntax error in P_SKIN lump# %d(%s) in WAD %s\n", lump, W_CheckNameForNumPwad(wadnum,lump), wadfiles[wadnum]->filename);

			if (!skin) // Get the name!
			{
				if (!stricmp(stoken, "name"))
				{
					strlwr(value);
					skinnum = R_SkinAvailable(value);
					if (skinnum != -1)
						skin = &skins[skinnum];
					else
					{
						CONS_Debug(DBG_SETUP, "R_PatchSkins: unknown skin name in P_SKIN lump# %d(%s) in WAD %s\n", lump, W_CheckNameForNumPwad(wadnum,lump), wadfiles[wadnum]->filename);
						noskincomplain = true;
					}
				}
			}
			else // Get the properties!
			{
				// Some of these can't go in R_ProcessPatchableFields because they have side effects for future lines.
				if (!stricmp(stoken, "realname"))
				{ // Display name (eg. "Knuckles")
					realname = true;
					STRBUFCPY(skin->realname, value);
					SYMBOLCONVERT(skin->realname)
					if (!hudname)
						HUDNAMEWRITE(skin->realname);
				}
				else if (!stricmp(stoken, "hudname"))
				{ // Life icon name (eg. "K.T.E")
					hudname = true;
					HUDNAMEWRITE(value);
					SYMBOLCONVERT(skin->hudname)
					if (!realname)
						STRBUFCPY(skin->realname, skin->hudname);
				}
				else if (!R_ProcessPatchableFields(skin, stoken, value))
					CONS_Debug(DBG_SETUP, "R_PatchSkins: Unknown keyword '%s' in P_SKIN lump #%d (WAD %s)\n", stoken, lump, wadfiles[wadnum]->filename);
			}

			if (!skin)
				break;

next_token:
			stoken = strtok(NULL, "\r\n= ");
		}
		free(buf2);

		if (!skin) // Didn't include a name parameter? What a waste.
		{
			if (!noskincomplain)
				CONS_Debug(DBG_SETUP, "R_PatchSkins: no skin name given in P_SKIN lump #%d (WAD %s)\n", lump, wadfiles[wadnum]->filename);
			continue;
		}

		// Patch sprites
		R_LoadSkinSprites(wadnum, &lump, &lastlump, skin);

		if (!skin->availability) // Safe to print...
			CONS_Printf(M_GetText("Patched skin '%s'\n"), skin->name);
	}
	return;
}

#undef HUDNAMEWRITE
#undef SYMBOLCONVERT

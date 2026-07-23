// NOTE: This is an implementation of the SMPS playback system based on the 
// Sonic 2 Z80 disassembly by Xenowhirl.
//	-- The PAL flag timing adjustment is also missing (basically "double update"
//	   every 6 frames on a PAL system; doesn't apply to any non-TV platform)

#include <stdio.h>
#include <malloc.h>
#include <memory.h>
#include <string.h>
#include <assert.h>
#include "types.h"
#include "smps.h"

#ifndef GENESIS_MEGADRIVE
	// These are defined in system-specific sound.c:
	void DACPlay(u8 note);						// Plays a DAC sound (not using YM2612 DAC emulation)
	void DACStop();								// Stops DAC output (not using YM2612 DAC emulation)
	void ym2612_fm0_regdata(u8 reg, u8 data);	// Use to write to 0x[A0]4000 and then 0x[A0]4001 (or DAC if reg == 0x2A)
	void ym2612_fm1_regdata(u8 reg, u8 data);	// Use to write to 0x[A0]4002 and then 0x[A0]4003
	void sn76496_write(u8 data);				// Use to write to 0xC00011/0x7F11
	void StartSignal(void);
	void StopSignal(void);
	void LoopStartSignal(void);
	void LoopEndSignal(void);
#else
	// Genesis/Megadrive actually has the hardware!

	// We must actually wait for FM busy on hardware:
	inlinefunc void FMBusyWait() { while(*((volatile u8 *)0xA04000) & 0x80) {} }

	inlinefunc void DACPlay(u8 note) { note; }
	inlinefunc void DACStop() { }
	inlinefunc void ym2612_fm0_regdata(u8 reg, u8 data) { FMBusyWait(); *((volatile u8 *)0xA04000) = reg; FMBusyWait(); *((volatile u8 *)0xA04001) = data; }
	inlinefunc void ym2612_fm1_regdata(u8 reg, u8 data) { FMBusyWait(); *((volatile u8 *)0xA04002) = reg; FMBusyWait(); *((volatile u8 *)0xA04003) = data; }
	inlinefunc void sn76496_write(u8 data) { *((volatile u8 *)0xC00011) = data; }
	inlinefunc void StartSignal(void) { }
	inlinefunc void StopSignal(void) { }
	inlinefunc void LoopStartSignal(void) { }
	inlinefunc void LoopEndSignal(void) { }
#endif

extern unsigned char VGM_IgnoreWrt;

typedef const u8 voice[25];			// Quick typedef for FM voices, always 25 bytes wide

enum SMODE_PTRS
{
	PTR_68K = 0x00,
	PTR_Z80 = 0x01
};
enum SMODE_CFLAGS
{
	CFLAG_S12 = 0x00,
	CFLAG_S3K = 0x01,
	CFLAG_TRS = 0x02	// SMPS 68k Treasure
};
enum SMODE_TEMPO_CLOCK
{
	TCLK_S1 = 0x00,
	TCLK_S2 = 0x01,
	TCLK_S3K = 0x02
};
enum SMODE_INS_MODE
{
	INS_STD = 0x00,
	INS_S2 = 0x01
};
struct smps_settings
{
	u8  ptr_mode;
	u8  ins_mode;
	u8  tempo_mode;
	u8  coord_mode;
	u16 offset;
};

// Coordination flags
enum COORD_FLAGS
{
	CF_START	= 0xE0,		// Where coordination flags begin
	CF_PANAMSFMS= CF_START,	// Panning, AMS, FMS
	CF_FREQADJ,				// Alter note frequency
	CF_FADEIN_S3,			// Okay, so here's the lowdown -- in Sonic 1 & 2, this sets an arbitrary value for no known purpose.  In Sonic 3, this value, if set as FF, is used as the fade-in routine
	CF_RETURN,				// Return from CF_GOSUB (S1/S2 ONLY)
	CF_FADEIN,				// Fade-in to previous song (S1/S2 ONLY); put on DAC channel
	CF_SETTEMPODIV,			// Set this track's tempo divider
	CF_CHANGEVOLUME,		// Changes track volume
	CF_NOATTACK,			// Prevent next note from attacking
	CF_SETNOTEFILL,			// Set note fill amount to byte
	CF_TRANSPOSE,			// Add transposition to channel key (S1/S2 ONLY)
	CF_SETTEMPO,			// Change song tempo to unsigned byte
	CF_SETTEMPODIVS,		// Set ALL per-track tempo dividers
	CF_CHANGEVOLUME_LATER,	// Like CF_CHANGEVOLUME, except for FM tracks, volume change does not occur until next voice change
	CF_UNUSED_ED,			// Nothing
	CF_UNUSED_EE,			// Nothing
	CF_CHANGE_VOICE,		// Change FM voice (NOT PSG tone!)
	CF_MODULATION,			// Setup modulation
	CF_MODULATIONON,		// Turn on modulation
	CF_STOPTRACK,			// Stops this track from playing (non-looping BGMs only)
	CF_PSGSETNOISE,			// Set current PSG noise (effects noise channel only)
	CF_MODULATIONOFF,		// Turn off modulation
	CF_CHANGE_PSGTONE,		// Change PSG tone (NOT FM voice!)
	CF_JUMP,				// Jump to offset
	CF_REPEAT,				// Repeat section of music
	CF_GOSUB,				// Jump to offset, but keep return address in memory
	CF_RETURN_S3,			// Return from CF_GOSUB (S3/S&K/3K ONLY)
	CF_UNUSED_FA,			// Nothing
	CF_TRANSPOSE_S3,		// Add transposition to channel key (S3/S&K/3K ONLY)
};


// This more or less reflects the track layout of the Z80
// (which starts at $1B98)
enum TRACK_INDICES
{
	// RAS: Do NOT change the order of these unless you fix 
	// the GET_COOR_* macros!!

	TRACK_DAC,			// Music DAC track
	TRACK_FM1,			// Music FM Channel 1 track
	TRACK_FM2,			// Music FM Channel 2 track
	TRACK_FM3,			// Music FM Channel 3 track
	TRACK_FM4,			// Music FM Channel 4 track
	TRACK_FM5,			// Music FM Channel 5 track
	TRACK_FM6,			// Music FM Channel 6 track
	TRACK_PSG1,			// Music PSG Channel 1 track
	TRACK_PSG2,			// Music PSG Channel 2 track
	TRACK_PSG3,			// Music PSG Channel 3 track

	MUSIC_TRACK_TOTAL,

	TRACK_SFX_FM3 = MUSIC_TRACK_TOTAL,		// SFX FM Channel 3 track
	TRACK_SFX_FM4,		// SFX FM Channel 4 track
	TRACK_SFX_FM5,		// SFX FM Channel 5 track
	TRACK_SFX_PSG1,		// SFX PSG Channel 1 track
	TRACK_SFX_PSG2,		// SFX PSG Channel 2 track
	TRACK_SFX_PSG3,		// SFX PSG Channel 3 track

	TRACK_TOTAL
};


enum PLAYCTL_BITS
{
	// Bits defined for the "playback_control" byte of smps_track
	PLAY_ATREST			= 0x02,	// track is at rest
	PLAY_SFXOVERRIDE	= 0x04, // SFX is overriding this track
	PLAY_MODULATIONON	= 0x08,	// Modulation turned on
	PLAY_NOATTACK		= 0x10,	// do not attack next note
	PLAY_ISPLAYING		= 0x80,	// track is playing
};


enum VOICECTL_BITS
{
	// Bits defined for the "voice control" byte of smps_track
	// NOTE: Theses bits of "voice_control" are for FM/PSG channel 
	// assignments, so you really can't add any other bits here...
	// They're really only used in code for clarity only.
	VOICE_FM1			= 0x04,	// Assigned to FM1 (Channels 4-6) vs FM) (Channels 1-3)
	VOICE_ISPSG_TRACK	= 0x80	// This track uses the PSG
};


//////////////////////////////////////////////////////////////////////
// SMPS PLAYER MEMORY -- contains various variables used by player;
// basically a subset of what is known as "zComRange" in Sonic 2 Z80
//////////////////////////////////////////////////////////////////////

// This more or less reflects the Z80 "zComRange", sans
// variables that do not apply here.
#define FADE_LEVELS			40	// Volume levels to decrease before fade in/out complete
#define FADEOUT_TICKRESET	3	// Fade-out runs this many frames before decrementing fade level
#define FADEIN_TICKRESET	2	// Fade-in runs this many frames before decrementing fade level (little faster than fade-out)
static struct _smps_data
{
	u8	sfx_priority;		// 00
	u8	tempo_clock;		// 01 Current count for tempo; every overflow causes music update
	u8	tempo;				// 02 Actual current tempo
	u8	pause_mode;			// 03
	
	u8	vol_fadeout;		// 04 total volume levels to continue decreasing volume before fade out considered complete (starts at FADE_LEVELS, works downward)
	u8	vol_fadeouttick;	// 05 delay ticker before next volume decrease (reset to FADEOUT_TICKRESET whenever it hits zero)
	u8	unused_06;			// 06
	u8	DACupdating;		// 07
	
	u8	SoundRequest;		// 08
	u8	queueSFX;			// 09
	u8	queueSFXstereo;		// 0A
	u8	queueSFXunknown;	// 0B
	
	voice *voice_table;		// 0C Offset to common music voice table
	u8	fadein_flag;		// 0E Set while fading in (disabling SFX)
	u8	vol_fadeintick;		// 0F delay ticker before next volume increase (reset to FADEIN_TICKRESET whenever it hits zero)
	u8	vol_fadein;			// 10 total volume levels to continue increasing volume before fade in considered complete
	
	u8	oneUp_flag;			// 11 Flag for when 1-up song is playing (disables sounds)
	u8	main_tempo;			// 12
	u8	speedshoe_backup;	// 13
	u8	speedshoe_tempo;	// 14 Speed shoes tempo
	
	u8	DAC_enabled;		// 15 Set when DAC is in use
	u8	unused;				// 16 "deleted index hold that enabled the secondary bank"
	u8	PAL_flag;			// 17
	// -> 18 bytes
	
	
	// Stuff not part of zComRange
	u8	DAC_on;				// not in Z80 source (but something similar must be in the Sonic 1 driver)
	s8	SpindashKeyOffset;		// Offset value for spindash (set by smps_sfx_setspindashfreq)
	u8  SpindashPlayingCounter;	// Decrements to zero to change spindash sound rate
	struct smps_settings smps_set;
	struct smps_settings smps_sfx_set[TRACK_TOTAL-MUSIC_TRACK_TOTAL];
	const u8 *smps_start;		// Pointer to start of current music data (didn't exist in Z80; only absolute addressing in use)
	const u8 *smps_sfx_track_starts[TRACK_TOTAL-MUSIC_TRACK_TOTAL];		// Pointers for each track of SFX that dictates their start-of-data
}	smps_data,		// Main player memory
	smps_backup;	// Backup memory (when 1-up plays)


//////////////////////////////////////////////////////////////////////
// SMPS TRACK DEFINITIONS -- defines the tracks' memory, which 
// includes both music, SFX, and backup of the music tracks
// (for 1-up song that overrides all music temporarily)
//////////////////////////////////////////////////////////////////////

#define TRACK_SCRATCH_MEMORY	10	// How many bytes are allocated to track's scratch space (loop counters and gosub returns)

static struct smps_track
{
	u8  playback_control;	// 00 "playback control"; see PLAYCTL_BITS
	u8  voice_control;		// 01 "voice control"; see VOICECTL_BITS; handles hardware assignment of sound channel for FM/PSG
	u8  timing_divisor;		// 02 Divides tempo by this amount (i.e. 1 = normal, 2 = half, 3 = third...)
	u16 pos;				// 03/04 Byte position in track (relative to start of playback memory)
	s8  tranpose;			// 05 Key offset (from coord flag E9)
	u8  vol;				// 06 channel volume (NOTE for FM: only applied at voice changes or if CF_CHANGEVOLUME is used)
	u8  pan_ams_fms;		// 07 Panning / AMS / FMS settings; see Sega Tech Doc (or a YM2612 manual I guess)
	u8  current_voice;		// 08 Current voice in use OR current PSG tone
	u8  psg_flutter;		// 09 PSG flutter (dynamically effects PSG volume for decay/special effects)
	u8  gosub_stack_loc;	// 0A "Gosub" stack position offset (starts at end of this track, and each CF_GOSUB decrements by 2)
	u8  current_duration;	// 0B current duration timeout; counting down to zero
	u8  last_duration;		// 0C last set duration (if a note follows a note, this is reapplied to current_duration)
	u16 frequency;			// 0D/0E Frequency value of FM / PSG (was also DAC patch/rate on Z80)
	u8  current_notefill;	// 0F Currently set note fill; counts down to zero and then cuts off note
	u8  last_notefill;		// 10 Reset value for current note fill
	const u8 *modcfg;		// 11/12 Points to current modulation configuration data
	u8  modwait;			// 13 Wait period of time before modulation starts
	u8  modspeed;			// 14 Modulation Speed
	s8  moddelta;			// 15 Modulation change per Mod. Step
	u8  modsteps;			// 16 Number of steps in modulation (divided by 2)
	u16 modulation;			// 17/18 Current modulation settings
	s8  freq_adjust;		// 19 Set by "alter notes" coord flag E1; used to add directly to FM/PSG frequency
	u8  TL_mask;			// 1A Bitfield of which FM TL values are to be set to adjust volume (see VolTLMasks)
	u8  psg_noise;			// 1B PSG noise setting
	
	voice *custom_voices;	// 1C/1D Offset to a voice table (only used by SFX that define their own FM voices)
	const u8 *TL_bytes;		// 1E/1F Every time a voice change is met, this points to the TL bytes used for quick volume setting later
	u8  scratch_memory[TRACK_SCRATCH_MEMORY];	// 20-29 Scratch memory used by loop and gosub coord flags
	// -> 2A bytes
}	smps_tracks[TRACK_TOTAL],				// Main track memory
	smps_tracks_backup[MUSIC_TRACK_TOTAL];	// Backup (music only) track memory
	
static struct loop_state
{
	u8 Activated;
	u16 TrkMask;
	u16 TrkPos[MUSIC_TRACK_TOTAL];
	u16 TrkMinPos[MUSIC_TRACK_TOTAL];
} LoopState;

static u8 SMPS_MODE = SMODE_INVALID;	// semi-constant (changed when loading)
static u16 SMPS_OFFSET = 0x0000;

// Generic "get 16-bit value" from 8-bit datastream
inlinefunc u16 get16(const u8 **ptr)
{
	u16 result;

	result = ((*ptr)[0] << 0) | ((*ptr)[1] << 8);

	*ptr += 2;

	return result;
}

inlinefunc u16 get16_set(const u8 **ptr, const struct smps_settings* smps_set)
{
	const u8* p = *ptr;
	u16 result;

	if (smps_set->ptr_mode == PTR_68K)
		result = (p[0] << 8) | (p[1] << 0);
	else
		result = (p[0] << 0) | (p[1] << 8);
	result -= smps_set->offset;

	*ptr += 2;

	return result;
}

inlinefunc const struct smps_settings* get_track_settings(const struct smps_track *t);
// this function reads a pointer and returns the position where to jump
inlinefunc u16 get16_jump(struct smps_track *t)
{
	const u8 *jmpBytes = &smps_data.smps_start[t->pos];
	const struct smps_settings* smps_set = get_track_settings(t);
	
	if (smps_set->ptr_mode == PTR_Z80)
		return get16_set(&jmpBytes, smps_set);
	else //if (smps_set->ptr_mode == PTR_68K)
		return t->pos + 1 + get16_set(&jmpBytes, smps_set);
}

// Generic "put 16-bit value" to 8-bit datastream
inlinefunc void put16(u8 **ptr, u16 value)
{
	// little-endian
	*((*ptr)++) = (value & 0x00FF) >> 0;	// LSB first
	*((*ptr)++) = (value & 0xFF00) >> 8;	// MSB second
}


// A couple of readability-increasing helpful macros
#define GET_TRACK_INDEX(t)		(u8)((t) - smps_tracks)	// Returns index to track from pointer 't'
#define IS_SFX_TRACK(t)			(GET_TRACK_INDEX(t) >= MUSIC_TRACK_TOTAL)	// Returns whether 't' points to the SFX tracks


// This function returns the cooresponding music track for a given SFX track
static const u8 ct_sfx_to_music[] = { TRACK_FM3, TRACK_FM4, TRACK_FM5, TRACK_PSG1, TRACK_PSG2, TRACK_PSG3 };
inlinefunc struct smps_track *get_coor_music_track(struct smps_track *t)
{
	u8 this_track = GET_TRACK_INDEX(t);

	DEBUGASSERT(this_track >= TRACK_SFX_FM3 && this_track <= TRACK_SFX_PSG3);
	if(this_track >= TRACK_SFX_FM3 && this_track <= TRACK_SFX_PSG3)
		return &smps_tracks[ct_sfx_to_music[this_track - TRACK_SFX_FM3]];

	return NULL;
}


// This function returns the cooresponding SFX track for a given music track
static const u8 ct_music_to_sfx[] = { TRACK_SFX_FM3, TRACK_SFX_FM4, TRACK_SFX_FM5, 0xFF, TRACK_SFX_PSG1, TRACK_SFX_PSG2, TRACK_SFX_PSG3 };
inlinefunc struct smps_track *get_coor_sfx_track(struct smps_track *t)
{
	u8 this_track = GET_TRACK_INDEX(t);

	DEBUGASSERT(this_track >= TRACK_FM3 && this_track <= TRACK_PSG3 && this_track != TRACK_FM6);
	if(this_track >= TRACK_FM3 && this_track <= TRACK_PSG3 && this_track != TRACK_FM6)
		return &smps_tracks[ct_music_to_sfx[this_track - TRACK_FM3]];

	return NULL;
}


// This function returns the cooresponding SMPS Setting structure for a given track
inlinefunc const struct smps_settings* get_track_settings(const struct smps_track *t)
{
	if (! IS_SFX_TRACK(t))
		return &smps_data.smps_set;
	else
		return &smps_data.smps_sfx_set[GET_TRACK_INDEX(t) - MUSIC_TRACK_TOTAL];
}


//////////////////////////////////////////////////////////////////////
// FM DATA/FUNCTIONS -- Functions and data which support playback of
// music or sound effects on the FM channels
//////////////////////////////////////////////////////////////////////

// lookup table of note frequencies for instruments and sound effects
// RAS: To clarify, this is the FM frequency table (was "zFrequencies")
static const u16 FMFrequencies[96] = {
	0x025E, 0x0284, 0x02AB, 0x02D3, 0x02FE, 0x032D, 0x035C, 0x038F,
	0x03C5, 0x03FF, 0x043C, 0x047C, 0x0A5E, 0x0A84, 0x0AAB, 0x0AD3,
	0x0AFE, 0x0B2D, 0x0B5C, 0x0B8F, 0x0BC5, 0x0BFF, 0x0C3C, 0x0C7C,
	0x125E, 0x1284, 0x12AB, 0x12D3, 0x12FE, 0x132D, 0x135C, 0x138F,
	0x13C5, 0x13FF, 0x143C, 0x147C, 0x1A5E, 0x1A84, 0x1AAB, 0x1AD3,
	0x1AFE, 0x1B2D, 0x1B5C, 0x1B8F, 0x1BC5, 0x1BFF, 0x1C3C, 0x1C7C,
	0x225E, 0x2284, 0x22AB, 0x22D3, 0x22FE, 0x232D, 0x235C, 0x238F,
	0x23C5, 0x23FF, 0x243C, 0x247C, 0x2A5E, 0x2A84, 0x2AAB, 0x2AD3,
	0x2AFE, 0x2B2D, 0x2B5C, 0x2B8F, 0x2BC5, 0x2BFF, 0x2C3C, 0x2C7C,
	0x325E, 0x3284, 0x32AB, 0x32D3, 0x32FE, 0x332D, 0x335C, 0x338F,
	0x33C5, 0x33FF, 0x343C, 0x347C, 0x3A5E, 0x3A84, 0x3AAB, 0x3AD3,
	0x3AFE, 0x3B2D, 0x3B5C, 0x3B8F, 0x3BC5, 0x3BFF, 0x3C3C, 0x3C7C
};

#define HIGHEST_FM_NOTE		((sizeof(FMFrequencies)/sizeof(u16))-1)


// Original disasm said "lower = louder, although this may not exactly be volume control"
// Table was named "zGain"...
// RAS: More precisely, this controls which TL registers are set for a particular
// algorithm; it actually makes more sense to look at a zVolTLMaskTbl entry as a bitfield.
// Bit 0-3 set which TL operators are actually effected for setting a volume;
// this table helps implement the following from the Sega Tech reference:
// "To make a note softer, only change the TL of the slots (the output operators).  
// Changing the other operators will affect the flavor of the note."
static const u8 VolTLMasks[8] = { 0x08, 0x08, 0x08, 0x08, 0x0C, 0x0E, 0x0E, 0x0F };	// Actually contains one more value than possible algorithms?

// RAS: was "unknown data: zbyte_916"
// But these are the FM channel assignment bits!
static const u8 FMDACInitBytes[7] = { 6, 0, 1, 2, 4, 5, 6 }; // first byte is for DAC; then notice the 0, 1, 2 then 4, 5, 6; this is the gap between FM0 and FM1 for YM2612 port writes


// Quick utility to write to appropriate FM chip decided by track memory
inlinefunc void WriteFM0or1(const struct smps_track *t, u8 reg, u8 data)
{
	// Decides which FM chip to write to based on channel assignment
	if(t->voice_control & VOICE_FM1)
		ym2612_fm1_regdata(reg, data);
	else
		ym2612_fm0_regdata(reg, data);
}


// This reinitializes the FM hardware with all keys off
// and all FM channel registers set to known dummy values
static void FMSilenceAll()
{
	u8 loop;
	u8 reg = 0x28;		// Start at FM KEY ON/OFF register

	for(loop=0; loop<3; loop++)		// Three key off per FM0 / FM1
	{
		ym2612_fm0_regdata(reg,     loop);	// FM 0
		ym2612_fm0_regdata(reg, 4 | loop);	// FM 1
	}

	// From FM register 30h up to register 90h, write kill-all values
	for(reg=0x30; reg<0x90; reg++)
	{
		ym2612_fm0_regdata(reg, 0xFF);	// on FM0
		ym2612_fm1_regdata(reg, 0xFF);	// and FM1
	}
}



// Set correct "Total Levels" (general volume control)
static void FMSetVolume(const struct smps_track *t)
{
	const struct smps_settings* smps_set = get_track_settings(t);
	const u8 *TL_settings = t->TL_bytes;
	const u8 TL_mask = t->TL_mask;
	u8 reg = 0x40 + (t->voice_control & 3);	// TL register appropriate for this channel
	u8 loop;
	u8 regid;

	// This SHOULDN'T happen, but can happen if a volume change is attempted
	// before a voice is set (which would be bad practice anyway because FM
	// would not yet be properly configured...)
	if(TL_settings == NULL)
		return;

	// If track is being overridden, don't let it do anything
	if(t->playback_control & PLAY_SFXOVERRIDE)
		return;

	for(loop=0, regid=0; loop<4; loop++)	// Loop 4 times (for each Total Level register on this channel)
	{
		// Get next TL value
		u8 this_TL = *TL_settings++;

		// Only adjust TL levels that are appropriate by the mask ("slots" only)
		if(TL_mask & (1<<regid))
			// This one is a "slot"; adjust it!
			this_TL += t->vol;

		// Write this TL!
		WriteFM0or1(t, reg, this_TL);

		// Next TL reg...
		if (smps_set->ins_mode == INS_S2)
		{
			reg += 4;
			regid ++;
		}
		else
		{
			if (reg & 0x08)
			{
				reg = (reg + 4) & ~0x08;
				regid = (regid + 1) & 1;
			}
			else
			{
				reg += 8;
				regid += 2;
			}
		}
	}
}


// This sets a specific FM voice from a voice table into FM
// hardware, unless this track is overridden by SFX
static void FMSetVoice(struct smps_track *t, voice *voice_table)
{
	// zSetVoice:
	// RAS: This does the actual setting of the FM registers for the specific voice
	const struct smps_settings* smps_set = get_track_settings(t);
	const u8 *voice_ptr = voice_table[t->current_voice];		// Point to voice data
	u8 loop, reg, data;
	u8 algorithm;

	// Just in case we get in here in a bad situation...
	if(voice_table == NULL)
		return;

	// If track is being overridden, don't let it do anything
	if(t->playback_control & PLAY_SFXOVERRIDE)
		return;

	// RAS: Sets up a value for future Total Level setting...
	data = *voice_ptr++;					// Get feedback/algorithm -> a
	reg = 0xB0 + (t->voice_control & 3);	// only keep bits 0-2 (FM channel assignment) and add to get appropriate feedback/algorithm register
	WriteFM0or1(t, reg, data);				// Write new value to FM0/FM1

	// algorithm is used later...
	algorithm = data & 7;	// Only retains "algorithm" part of this voice

	// detune/coarse freq, all channels
	reg -= 0x80;			// Subtract 80 (now Detune/coarse frequency of operator 1 register)

	// Do next 4 bytes (operator 1, 2, 3, and 4)
	for(loop=0; loop<4; loop++)
	{
		data = *voice_ptr++;		// Get next detune/coarse freq
		WriteFM0or1(t, reg, data);	// Write new value to FM0/FM1
		if (smps_set->ins_mode == INS_S2)
		{
			reg += 4;					// Next detune/coarse freq register
		}
		else
		{
			if (reg & 0x08)
				reg = (reg + 4) & ~0x08;
			else
				reg += 8;
		}
	}

	// other regs up to just before "Total Level", all channels
	reg += 0x10;					// now at 50h+ (RS/AR of operator 1 register)


	// Perform 16 writes (basically goes through RS/AR, AM/D1R, D2R, D1L)
	for(loop=0; loop<16; loop++)
	{
		data = *voice_ptr++;		// Get next reg data value
		WriteFM0or1(t, reg, data);	// Write new value to FM0/FM1
		if (smps_set->ins_mode == INS_S2)
		{
			reg += 4;					// Next register
		}
		else
		{
			if (reg & 0x08)
				reg = (reg + 4) & ~0x08;
			else
				reg += 8;
		}
	}

	// RAS: Now going to set "stereo output control and LFO sensitivity"
	reg += 0x24;					// Sets to reg B4h+ (stereo output control and LFO sensitivity)
	data = t->pan_ams_fms;			// Panning / AMS / FMS settings from track
	WriteFM0or1(t, reg, data);		// Write new value to FM0/FM1

	// Save current position (TL bytes begin) for easier volume adjustments later
	t->TL_bytes = voice_ptr;

	// Get TL_Mask based on algorithm
	t->TL_mask = VolTLMasks[algorithm];

	// Set volume
	FMSetVolume(t);
}


// Set SSG-EG data
static void FMSetSSGEG(const struct smps_track *t)
{
	const struct smps_settings* smps_set = get_track_settings(t);
	const u8* SE_data = &smps_data.smps_start[t->pos];
	u8 reg = 0x90 + (t->voice_control & 3);
	u8 loop;
	u8 regid;
	
	for (loop = 0, regid = 0; loop < 4; loop ++)
	{
		// Write SSG-EG reg
		WriteFM0or1(t, reg, *SE_data);
		SE_data ++;
		
		// Next reg ...
		if (smps_set->ins_mode == INS_S2)
		{
			reg += 4;
			regid ++;
		}
		else
		{
			if (reg & 0x08)
			{
				reg = (reg + 4) & ~0x08;
				regid = (regid + 1) & 1;
			}
			else
			{
				reg += 8;
				regid += 2;
			}
		}
	}
	
	return;
}


// This sends a Key Off to hardware event, unless overridden by SFX
inlinefunc void FMNoteOff(const struct smps_track *t)
{
	if(!(t->playback_control & (PLAY_NOATTACK | PLAY_SFXOVERRIDE)))
	{
		// RAS: Format of key on/off:
		// 4321 .ccc
		// Where 4321 are the bits for which operator,
		// and ccc is which channel (0-2 for channels 1-3, 4-6 for channels 4-6 WATCH BIT GAP)

		// -ALL- FM Key On/Off commands go to -FM0- reg 28h
		ym2612_fm0_regdata(0x28, t->voice_control);
	}
}


// This sets the frequency in track memory, but does not modify hardware
inlinefunc void FMSetFreq(struct smps_track *t, u8 note)
{
	if(note != 0x80)	
	{
		// Change note to index value
		note -= 0x80;

		// Add current channel key offset (coord flag E9)
		note += t->tranpose;

		// Added protection against "over transpose" (not in Z80 source)
		if(note > HIGHEST_FM_NOTE)
			note = HIGHEST_FM_NOTE;

		// Store FM frequency setting for this note
		t->frequency = FMFrequencies[note];
	}
	else
	{
		const struct smps_settings* smps_set = get_track_settings(t);
		t->playback_control |= PLAY_ATREST;		// Set "track is at rest" bit
		if (smps_set->coord_mode != CFLAG_S3K)
			t->frequency = 0;					// Zero out FM Frequency
	}
}


// Update the FM hardware's frequency, if not overridden by SFX
static void FMUpdateFreq(const struct smps_track *t, u16 frequency)
{
	if(!(t->playback_control & PLAY_SFXOVERRIDE))
	{
		u16 freq = frequency + t->freq_adjust;		// Frequency with tweak value
		u8 reg = 0xA4 + (t->voice_control & 3);		// calculate appropriate register for frequency high byte

		// Write frequency high byte
		WriteFM0or1(t, reg, (freq & 0xFF00) >> 8);
		
		// Change to low byte register
		reg -= 4;

		// Write frequency low byte
		WriteFM0or1(t, reg, (freq & 0x00FF));
	}
}


//////////////////////////////////////////////////////////////////////
// PSG DATA/FUNCTIONS -- Functions and data which support playback of
// music or sound effects on the PSG channels
//////////////////////////////////////////////////////////////////////

// "seems to be an array of values for the PSG (was "PSGWaveArray")
// the same array is found at $729CE in Sonic 1, and at $C9C44 in Ristar"
// RAS: Specifically, this the note -> frequency setting lookup
// Note there are fewer valid note values here...
const u16 PSGFrequencies[70] = {
	0x0356, 0x0326, 0x02F9, 0x02CE, 0x02A5, 0x0280, 0x025C, 0x023A,
	0x021A, 0x01FB, 0x01DF, 0x01C4, 0x01AB, 0x0193, 0x017D, 0x0167,
	0x0153, 0x0140, 0x012E, 0x011D, 0x010D, 0x00FE, 0x00EF, 0x00E2,
	0x00D6, 0x00C9, 0x00BE, 0x00B4, 0x00A9, 0x00A0, 0x0097, 0x008F,
	0x0087, 0x007F, 0x0078, 0x0071, 0x006B, 0x0065, 0x005F, 0x005A,
	0x0055, 0x0050, 0x004B, 0x0047, 0x0043, 0x0040, 0x003C, 0x0039,
	0x0036, 0x0033, 0x0030, 0x002D, 0x002B, 0x0028, 0x0026, 0x0024,
	0x0022, 0x0020, 0x001F, 0x001D, 0x001B, 0x001A, 0x0018, 0x0017,
	0x0016, 0x0015, 0x0013, 0x0012, 0x0011, 0x0000
};
const u16 PSGFrequencies_S3K[88] = {
	0x03FF, 0x03FF, 0x03FF, 0x03FF, 0x03FF, 0x03FF, 0x03FF, 0x03FF,
	0x03FF, 0x03F7, 0x03BE, 0x0388,
	0x0356, 0x0326, 0x02F9, 0x02CE, 0x02A5, 0x0280, 0x025C, 0x023A,
	0x021A, 0x01FB, 0x01DF, 0x01C4, 0x01AB, 0x0193, 0x017D, 0x0167,
	0x0153, 0x0140, 0x012E, 0x011D, 0x010D, 0x00FE, 0x00EF, 0x00E2,
	0x00D6, 0x00C9, 0x00BE, 0x00B4, 0x00A9, 0x00A0, 0x0097, 0x008F,
	0x0087, 0x007F, 0x0078, 0x0071, 0x006B, 0x0065, 0x005F, 0x005A,
	0x0055, 0x0050, 0x004B, 0x0047, 0x0043, 0x0040, 0x003C, 0x0039,
	0x0036, 0x0033, 0x0030, 0x002D, 0x002B, 0x0028, 0x0026, 0x0024,
	0x0022, 0x0020, 0x001F, 0x001D, 0x001B, 0x001A, 0x0018, 0x0017,
	0x0016, 0x0015, 0x0013, 0x0012, 0x0011, 0x0010, 0x0000, 0x0000
};

// RAS: was "zPSG_Index"; I call this the volume flutter effect
// Basically, for any PSG tone, dynamic volume adjustments are applied to produce a pseudo-decay,
// or sometimes a ramp up for "soft" sounds, or really any other volume effect you might want!
#define DEFAULT_FLUTTER_COUNT	0x0D
const u8 default_flutter00[] = { 0,0,0,1,1,1,2,2,2,3,3,3,4,4,4,5,5,5,6,6,6,7,0x80 };
const u8 default_flutter01[] = { 0,2,4,6,8,16,0x80 };
const u8 default_flutter02[] = { 0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,0x80 };
const u8 default_flutter03[] = { 0,0,2,3,4,4,5,5,5,6,0x80 };
const u8 default_flutter04[] = { 3,3,3,2,2,2,2,1,1,1,0,0,0,0,0x80 };
const u8 default_flutter05[] = { 0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,2,2,2,2,2,2,2,3,3,3,3,3,3,3,3,4,0x80 };
const u8 default_flutter06[] = { 0,0,0,0,0,0,1,1,1,1,1,2,2,2,2,2,3,3,3,4,4,4,5,5,5,6,7,0x80 };
const u8 default_flutter07[] = { 0,0,0,0,0,1,1,1,1,1,2,2,2,2,2,2,3,3,3,3,3,4,4,4,4,4,5,5,5,5,5,6,6,6,6,6,7,7,7,0x80 };
const u8 default_flutter08[] = { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,0x80 };
const u8 default_flutter09[] = { 0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,2,2,2,2,2,2,2,2,2,3,3,3,3,3,3,3,3,3,3,4,0x80 };
const u8 default_flutter0A[] = { 4,4,4,3,3,3,2,2,2,1,1,1,1,1,1,1,2,2,2,2,2,3,3,3,3,3,4,0x80 };
const u8 default_flutter0B[] = { 4,4,3,3,2,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,7,0x80 };
const u8 default_flutter0C[] = { 14,13,12,11,10,9,8,7,6,5,4,3,2,1,0,0x80 };
const u8 default_flutter_empty[] = {0x83};

/*const u8 *zPSG_FlutterTbl[FLUTTER_COUNT] = {
	// Remember on PSG that the higher the value, the quieter it gets (it's attenuation, not volume); 
	// 0 is thus loudest, and increasing values decay, until level $F (silent)
	flutter00, flutter01, flutter02, flutter03, flutter04, flutter05, flutter06,
	flutter07, flutter08, flutter09, flutter0A, flutter0B, flutter0C
};*/

// Note: I added an additional flutter effect with ID FLUTTER_COUNT that doesn't output anything.
//       (instead of crashing or playing wrong flutter effects)
static u8 FLUTTER_COUNT;
static u8** FlutterData;
static const u8** zPSG_FlutterTbl;

// RAS: was "unknown data: zbyte_91D"
// But these are the default values for PSG tracks!
const u8 PSGInitBytes[3] = { 0x80, 0xA0, 0xC0 }; // Specifically, these configure writes to the PSG port for each channel


// Sets all PSG channels to absolute silence
static void PSGSilenceAll()
{
	sn76496_write(0x80 | 0x1F);	// Stop channel 1
	sn76496_write(0xA0 | 0x1F);	// Stop channel 2
	sn76496_write(0xC0 | 0x1F);	// Stop channel 3
	sn76496_write(0xE0 | 0x1F);	// Stop noise channel
}


// Simply silences the PSG channel based on track memory
inlinefunc void PSGNoteOff(const struct smps_track *t)
{
	if(!(t->playback_control & PLAY_SFXOVERRIDE))	// If "SFX override" bit set, quit!
	{
		// "voice control" byte loads upper bits which specify PSG channel setting
		//                |a| |1Fh|
		// VOL1    0x90	= 100 1xxxx	vol 4b xxxx = attenuation value
		// VOL2    0xb0	= 101 1xxxx	vol 4b
		// VOL3    0xd0	= 110 1xxxx	vol 4b
		sn76496_write(t->voice_control | 0x1F);	// 0x1F is complete "attenuation off"
	}
}


// Update the PSG hardware's frequency, if not overridden by SFX
static void PSGUpdateFreq(const struct smps_track *t, u16 frequency)
{
	if(!(t->playback_control & (PLAY_ATREST | PLAY_SFXOVERRIDE)))	// If either bit 1 ("track in rest") and 2 ("SFX overriding this track"), quit!
	{
		u16 freq = frequency + t->freq_adjust;		// Frequency with tweak value
		u8 reg = t->voice_control;
	
		// This picks out the reg to write to the PSG
		if(reg == 0xE0)	// Is it E0h?
			// If so, correct it to C0h (E0h marks a noise channel, but really it's still C0h (Channel 3) in noise mode)
			reg = 0xC0;

		// Write lower four bits of frequency
		sn76496_write(reg | ((u8)(freq & 0x000F)));			// first PSG reg write only applies d0-d3 of freq

		// Write upper 6 bits of frequency
		sn76496_write((u8)((freq & 0x03F0) >> 4));	// second PSG reg write applies d4-d9 of freq
	}
}


// Updates PSG volume, mindful of overflow; does work as
// long as the track isn't being overridden or resting
static void PSGUpdateVol(struct smps_track *t, u8 vol)
{
	// zPSGUpdateVol:
	if(!(t->playback_control & (PLAY_ATREST | PLAY_SFXOVERRIDE)))
	{
		u8 reg;

		if(t->playback_control & PLAY_NOATTACK)
		{
			// zloc_515:
			if(t->last_notefill != 0 && t->current_notefill == 0)
				// If notefill is active and has just run out, then quit
				return;
		}

		// zloc_505:
		if(vol > 0xF)	// Did the level get pushed below silence level? (i.e. a > 0Fh)
			vol = 0xF;	// If so, fix it!

		// Apply channel info (which PSG to set!) and mark it as an attenuation level assignment
		reg = t->voice_control | 0x10 | vol;	
		sn76496_write(reg);	// Write to PSG!!
	}
}


// Updates PSG "flutter" volume effects
static void PSGDoVolFX(struct smps_track *t)
{
	u8 flutter_vol = t->vol;

	if (t->frequency == 0xFFFF)
		return;
	
	// PSG tone 0 is just a constant tone
	if(t->current_voice > 0)
	{
		// Point to appropriate flutter table values
		const u8 *flutter = zPSG_FlutterTbl[t->current_voice-1];

		// Get this flutter value and increment flutter location
		u8 flutter_val = flutter[t->psg_flutter++];

		// If we've hit the terminator, then leave; no volume adjustments here
		if (flutter_val == 0x80 || flutter_val == 0x81)
		{
			// 0x80 is Sonic 1/2, 0x81 is Sonic 3K
			t->psg_flutter--;	// back up so we hit flutter again
			return;
		}
		else if (flutter_val == 0x83)
		{
			t->psg_flutter--;
			if (! (t->playback_control & PLAY_ATREST))
			{
				t->playback_control |= PLAY_ATREST;
				PSGNoteOff(t);
			}
			return;
		}

		// Otherwise, apply flutter to current volume
		flutter_vol += flutter_val;
	}

	// Update PSG volume!
	PSGUpdateVol(t, flutter_vol);
}


static void PSGSetFreq(struct smps_track *t, u8 note)
{
	const struct smps_settings* smps_set = get_track_settings(t);
	if(note != 0x80)
	{
		// Change note to index value
		note -= 0x81;	// a = a-$81 (zero-based index from lowest note)

		// Add current channel key offset (coord flag E9)
		note += t->tranpose;
		if (smps_set->coord_mode != CFLAG_S3K)
		{
			if (note >= 70)	// array bound check
				note = 69;

			// Store PSG frequency setting for this note
			t->frequency = PSGFrequencies[note];
		}
		else
		{
			if (note >= 88)	// array bound check
				note = 87;

			// Store PSG frequency setting for this note
			t->frequency = PSGFrequencies_S3K[note];
		}
	}
	else
	{
		// RAS: If you get here, we're doing a PSG rest
		t->playback_control |= PLAY_ATREST;		// Set "track is at rest" bit
		if (smps_set->coord_mode != CFLAG_S3K)
			t->frequency = 0xFFFF;				// PSG Frequency = 0xFFFF (RAS: Just a dummy value)
		PSGNoteOff(t);
	}
}

//////////////////////////////////////////////////////////////////////
// FM OR PSG
//////////////////////////////////////////////////////////////////////

static void SetModulation(struct smps_track *t)
{
	// RAS: Sets up modulation for this track; expects t->modcfg to 
	// altready point to modulation configuration info...
	const u8 *mod_data_ptr = t->modcfg;

	// Copy in modulation config data
	t->modwait  =  *mod_data_ptr++;			// Wait for ww period of time before modulation starts
	t->modspeed =  *mod_data_ptr++;			// Modulation Speed
	t->moddelta =  (s8)*mod_data_ptr++;		// Modulation change per Mod. Step
	t->modsteps = (*mod_data_ptr++) >> 1;	// Get Number of steps in modulation, divide by 2

	if(!(t->playback_control & PLAY_NOATTACK))
	{
		// If "do not attack" bit is NOT set...
		t->modulation = 0;	// Clear modulation value
	}
}


// This actually performs modulation effects; returns 0 if modulation
// is not being performed, thus to alert the caller not to bother
// updating the frequency.  Otherwise returns new frequency.
static u16 DoModulation(struct smps_track *t)
{
	// Note: Sonic 1 does NOT check for PLAY_ATREST, so modulation is also applied to rests, causing high pitched sounds
	if(!(t->playback_control & PLAY_ATREST) && (t->playback_control & PLAY_MODULATIONON))
	{
		if(t->modwait == 0)
		{
			if(--t->modspeed == 0)
			{
				// Point to modulation configuration data in the track
				const u8 *moddata = t->modcfg;

				// skip passed 'ww' period of time
				moddata++;	

				// Restore speed counter
				t->modspeed = *moddata;

				if(t->modsteps == 0)
				{	
					// If steps have reached zero...
					moddata++;	// passed mod speed
					moddata++;	// passed mod change per mod step

					// restore modulation steps
					t->modsteps = *moddata;	

					// negate the modulation change per mod step
					t->moddelta = -t->moddelta;
				}
				else
				{
					// Decrement the step
					t->modsteps--;	

					// Apply the current modulation delta
					t->modulation += t->moddelta;

					// Return the modulated frequency
					return t->frequency + t->modulation;
				}
			}
		}
		else
			// Still waiting...
			t->modwait--;
	}

	// Modulation not performed
	return 0;
}


static void FinishTrackUpdate(struct smps_track *t)
{
	// RAS: Common finish-up routine used by FM or PSG
	t->current_duration = t->last_duration;	// Ensure current duration is last set duration (in case no duration was provided)

	if(!(t->playback_control & PLAY_NOATTACK))	// If "do not attack next note" set, don't do anything
	{
		t->current_notefill = t->last_notefill;	// Reset note fill to last value
		t->psg_flutter = 0;						// Reset PSG flutter (used or not)

		if(t->playback_control & PLAY_MODULATIONON)
			// If modulation is on, reset it!
			SetModulation(t);
	}
}


inlinefunc void SetDuration(struct smps_track *t, u8 new_duration)
{
	// zSetDuration:
	new_duration *= t->timing_divisor;		// Slow down by multiplying duration by timing divisor (e.g. 2 is half speed, 3 is third...)
	t->last_duration	= new_duration;		// Used to reset current_duration if a note follows a note
	t->current_duration	= new_duration;		// Counts down to zero before updating track again
}


// This updates a track's "note fill" value, decrementing it until it
// hits zero; if it does, then this instructs the caller to not do anything
// else with the track since it just performed a Note Off
inlinefunc int NoteFillUpdate(struct smps_track *t)
{
	if(t->current_notefill > 0)
	{
		if(--t->current_notefill == 0)
		{
			t->playback_control |= PLAY_ATREST;	// Set "track is at rest" bit

			if(t->voice_control & VOICE_ISPSG_TRACK)
				PSGNoteOff(t);
			else
				FMNoteOff(t);

			// 1=Note-fill cancellation
			return 1;
		}
	}

	// 0=Not performing note-fill cancel
	return 0;
}


//////////////////////////////////////////////////////////////////////
// BEGIN SMPS ENGINE
//////////////////////////////////////////////////////////////////////


static void ClearTrackPlaybackMem();
void smps_init()
{
	// Clear all track memory / initialize hardware
	ClearTrackPlaybackMem();
}

static void SetSMPSSettings(struct smps_settings* Set)
{
	switch(SMPS_MODE)
	{
	case SMODE_S1:
		Set->ptr_mode = PTR_68K;
		Set->ins_mode = INS_STD;
		Set->coord_mode = CFLAG_S12;
		Set->tempo_mode = TCLK_S1;
		break;
	case SMODE_S2:
		Set->ptr_mode = PTR_Z80;
		Set->ins_mode = INS_S2;
		Set->coord_mode = CFLAG_S12;
		Set->tempo_mode = TCLK_S2;
		break;
	case SMODE_S3K:
		Set->ptr_mode = PTR_Z80;
		Set->ins_mode = INS_STD;
		Set->coord_mode = CFLAG_S3K;
		Set->tempo_mode = TCLK_S3K;
		break;
	default:
		Set->ptr_mode = PTR_68K;
		Set->ins_mode = INS_STD;
		Set->coord_mode = CFLAG_TRS;
		Set->tempo_mode = TCLK_S1;
		break;
	}
	Set->offset = SMPS_OFFSET;
	return;
}

static void UpdateFadeout();
static void UpdateFadein();
static void DACUpdateTrack(struct smps_track *t);
static void FMUpdateTrack(struct smps_track *t);
static void PSGUpdateTrack(struct smps_track *t);
void smps_update()	// zUpdateEverything
{
	const u8 *musicPtr;
	int i;

	if(smps_data.vol_fadeout != 0)	// are we fading out?
		UpdateFadeout();	// If so, update that

	if(smps_data.vol_fadein != 0)	// are we fading in?
		UpdateFadein();

	// RAS: Spindash update
	if(smps_data.SpindashPlayingCounter > 0)
		smps_data.SpindashPlayingCounter--;		// decrease the spindash sound playing counter


	// RAS: Tempo works as divisions of the 60Hz clock (there is a fix supplied for
	// PAL that "kind of" keeps it on track.)  Every time the internal music clock
	// overflows, it updates.  So a tempo of 80h will update every other frame, or 
	// 30 times a second.  0x40 = 15 updates/sec, 0x80 = 30, 0xC0 = 45, 0xFF = ~60

	// If tempo added to tempo_clock does NOT overflow, we add 1 to all current durations
	// Pretty much the same thing as not running the update functions at all this frame.
	switch(smps_data.smps_set.tempo_mode)
	{
	case TCLK_S1:
		if (smps_data.tempo_clock == 0x01)
		{
			for(i=TRACK_DAC; i<MUSIC_TRACK_TOTAL; i++)
				smps_tracks[i].current_duration++;
			smps_data.tempo_clock = smps_data.tempo;
		}
		else
		{
			// update tempo_clock
			smps_data.tempo_clock --;
		}
		break;
	case TCLK_S2:
		if( (smps_data.tempo_clock + smps_data.tempo) <= 0xFF )
		{
			for(i=TRACK_DAC; i<MUSIC_TRACK_TOTAL; i++)
				smps_tracks[i].current_duration++;
		}

		// Either way, update tempo_clock
		smps_data.tempo_clock += smps_data.tempo;
		break;
	case TCLK_S3K:
		if ((smps_data.tempo_clock + smps_data.tempo) > 0xFF )
		{
			for (i=TRACK_DAC; i<MUSIC_TRACK_TOTAL; i++)
				smps_tracks[i].current_duration++;
		}

		// Either way, update tempo_clock
		smps_data.tempo_clock += smps_data.tempo;
		break;
	}

	// Update DAC track
	if(smps_tracks[TRACK_DAC].playback_control & PLAY_ISPLAYING)	// Is it playing?
		DACUpdateTrack(&smps_tracks[TRACK_DAC]);			// If it is, go update it

	// Update music FM tracks
	for(i=TRACK_FM1; i<=TRACK_FM6; i++)
	{
		if(smps_tracks[i].playback_control & PLAY_ISPLAYING)	// Is it playing?
			FMUpdateTrack(&smps_tracks[i]);			// If it is, go update it
	}

	// Update music PSG tracks
	for(i=TRACK_PSG1; i<=TRACK_PSG3; i++)
	{
		if(smps_tracks[i].playback_control & PLAY_ISPLAYING)	// Is it playing?
			PSGUpdateTrack(&smps_tracks[i]);			// If it is, go update it
	}


	////////////////////////////
	musicPtr = smps_data.smps_start;	// Remember SMPS music pointer!

	// Update SFX FM tracks
	for(i=TRACK_SFX_FM3; i<=TRACK_SFX_FM5; i++)
	{
		// Is it playing?
		if(smps_tracks[i].playback_control & PLAY_ISPLAYING)
		{
			// If it is...
			smps_data.smps_start = smps_data.smps_sfx_track_starts[i-MUSIC_TRACK_TOTAL];	// Each SFX track may have its own unique pointer to memory
			FMUpdateTrack(&smps_tracks[i]);						// go update it
		}
	}


	// Update SFX PSG tracks
	for(i=TRACK_SFX_PSG1; i<=TRACK_SFX_PSG3; i++)
	{
		// Is it playing?
		if(smps_tracks[i].playback_control & PLAY_ISPLAYING)
		{
			// If it is...
			smps_data.smps_start = smps_data.smps_sfx_track_starts[i-MUSIC_TRACK_TOTAL];	// Each SFX track may have its own unique pointer to memory
			PSGUpdateTrack(&smps_tracks[i]);					// go update it
		}
	}

	// Restore music pointer
	smps_data.smps_start = musicPtr;

}


void smps_playSong(const u8 *smps_song, u8 speedshoe_tempo)
{
	u8 tempo_divider;
	u8 loop, channels_dacfm, channels_psg;
	struct smps_settings* mus_set;
	u16 addr;

	// zBGMLoad:

	// Inline of zInitMusicPlayback (sort of)
	{
		// Some values to save before obliterating playback memory
		u8 save_oneUp_flag = smps_data.oneUp_flag;
		u8 save_speedshoe_tempo = smps_data.speedshoe_tempo;
		u8 save_vol_fadein = smps_data.vol_fadein;

		memset(&smps_data, 0, sizeof(smps_data));						// Clear all player data
		memset(&smps_tracks, 0, sizeof(struct smps_track)*TRACK_TOTAL);	// Clear all track data
		if (LoopState.Activated != 0xFF)
			LoopState.Activated = 0x00;
		LoopState.TrkMask = 0x00;
		memset(LoopState.TrkPos, 0x00, sizeof(u16) * MUSIC_TRACK_TOTAL);

		// Restore those values
		smps_data.oneUp_flag = save_oneUp_flag;
		smps_data.speedshoe_tempo = save_speedshoe_tempo;
		smps_data.vol_fadein = smps_data.vol_fadein;

		VGM_IgnoreWrt = 0x01;	// silencing the chips is not needed at VGM start
		FMSilenceAll();			// Silence FM
		PSGSilenceAll();		// Silence PSG
		VGM_IgnoreWrt = 0x00;
	}


	mus_set = &smps_data.smps_set;
	SetSMPSSettings(mus_set);
	// Set speedshoe tempo (note: I allowed the option of '0' to be used
	// to just use the SMPS's tempo; usually speedshoe tempos are supplied
	// from a LUT however.)
	smps_data.speedshoe_tempo = speedshoe_tempo;


	// Set the main track pointer!!
	smps_data.smps_start = smps_song;

	// Point to voice table
	addr = get16_set(&smps_song, mus_set);
	smps_data.voice_table = (voice *)&smps_data.smps_start[addr];

	// Channel setup
	channels_dacfm = *smps_song++;	// Channel setup (DAC+FM)
	channels_psg   = *smps_song++;	// Channel setup (PSG)
	
	// As long as there's at least 1 channel or less than 7, we need DAC!
	// Note: Sonic 1 does this, too, but sends an additional DAC enable before every DAC sound.
	if(channels_dacfm >= 1 && channels_dacfm < 7)
	{
		ym2612_fm0_regdata(0x2B, 0x80);	// Enable DAC!
		smps_data.DAC_on = TRUE;
	}
	else
	{
		ym2612_fm0_regdata(0x2B, 0x00);	// Disable DAC!
		smps_data.DAC_on = FALSE;
	}

	// Hold on to tempo divider
	tempo_divider = *smps_song++;

	// Get tempo (optionally patch speedshoe_tempo)
	smps_data.tempo = *smps_song++;
	if(smps_data.speedshoe_tempo == 0)
		smps_data.speedshoe_tempo = smps_data.tempo;
	
	if (mus_set->tempo_mode == TCLK_S1)
		smps_data.tempo_clock = smps_data.tempo;
	else
		smps_data.tempo_clock = 0;

	// We're at DAC pointer (+06h)

	// DAC+FM channels we need to initialize!
	for(loop=TRACK_DAC; loop<TRACK_DAC+channels_dacfm; loop++)
	{	
		struct smps_track *t = &smps_tracks[loop];
		
		t->timing_divisor	= tempo_divider;				// Set tempo divider
		t->playback_control |= PLAY_ISPLAYING;				// This track is playing!
		t->voice_control	= FMDACInitBytes[loop];			// Initialize voice setting to appropriate value
		t->timing_divisor	= tempo_divider;				// Store timing divisor
		t->gosub_stack_loc	= sizeof(t->scratch_memory);	// Set stack position to end of scratch block
		t->pan_ams_fms		= 0xC0;							// Default Panning / AMS / FMS (only stereo L/R enabled)
		t->current_duration	= 1;							// Set duration to expire immediately next frame (force update)
		t->pos				= get16_set(&smps_song, mus_set);	// Get this track position
		t->tranpose			= *smps_song++;					// default key offset, typically 0, can be set later by coord flag E9
		t->vol				= *smps_song++;					// track default volume
	}

	if(channels_dacfm != 7)
	{
		u8 reg;

		// RAS: Silence FM Channel 6 specifically if it's not in use
		ym2612_fm0_regdata(0x28, 6);	// Key on/off FM register FM channel 6; All operators off

		// Starting at FM Channel 6 Operator 1 Total Level register
		reg = 0x42;	

		// All 4 operators -- silence
		for(reg=0x42; reg<=0x4E; reg+=4)
			ym2612_fm1_regdata(reg, 0xFF);

		// Set Panning / AMS / FMS -- only stereo L/R enabled
		ym2612_fm1_regdata(0xB6, 0xC0);	
	}

	// End of DAC/FM init, begin PSG init
	
	//zloc_884:

	// PSG channels we need to initialize!
	for(loop=TRACK_PSG1; loop<TRACK_PSG1+channels_psg; loop++)
	{	
		struct smps_track *t = &smps_tracks[loop];
		
		t->timing_divisor	= tempo_divider;					// Set tempo divider
		t->playback_control |= PLAY_ISPLAYING;					// This track is playing!
		t->voice_control	= PSGInitBytes[loop-TRACK_PSG1];	// Initialize PSG setting to appropriate value
		t->timing_divisor	= tempo_divider;					// Store timing divisor
		t->gosub_stack_loc	= sizeof(t->scratch_memory);		// Set stack position to end of scratch block
		t->current_duration	= 1;								// Set duration to expire immediately next frame (force update)
		t->pos				= get16_set(&smps_song, mus_set);	// Get this track position
		t->tranpose			= (s8)*smps_song++;					// default key offset, typically 0, can be set later by coord flag E9
		t->vol				= *smps_song++;						// track default volume
		t->current_voice	= *smps_song++;						// default PSG voice (Sonic 2 uses this byte)

		// I'm doing it this way because at least one song in Sonic 2 actually
		// has a value in the "unused" spot that causes just say OR'ing these
		// together to fail!  Weird cases...
		if(t->current_voice == 0x00)
			t->current_voice = *smps_song;					// default PSG voice (Sonic 1 uses this byte)
		if (t->current_voice > FLUTTER_COUNT)
			t->current_voice = FLUTTER_COUNT;

		smps_song++;
	}


	// End of PSG tracks init, begin SFX tracks init

	for(loop=TRACK_SFX_FM3; loop<TRACK_TOTAL; loop++)
	{
		struct smps_track *t = &smps_tracks[loop];

		if(t->playback_control & PLAY_ISPLAYING)
		{
			// Get cooresponding music track
			struct smps_track *m = get_coor_music_track(t);

			// Clear the "SFX override" bit; this SFX track isn't overriding that music track now
			// (All SFX are cancelled when music loads anew)
			m->playback_control &= ~PLAY_SFXOVERRIDE;	
		}
	}
	
	// End of SFX tracks init...

	// Note off ALL FM channels
	for(loop=TRACK_FM1; loop<=TRACK_FM6; loop++)
	{
		struct smps_track *t = &smps_tracks[loop];
		FMNoteOff(t);
	}

	// Note off ALL PSG channels
	for(loop=TRACK_PSG1; loop<=TRACK_PSG3; loop++)
	{
		struct smps_track *t = &smps_tracks[loop];
		PSGNoteOff(t);
	}
	
	StartSignal();
}


void smps_playSFX(const u8 *sfx_start, u8 sfx_flags)
{
	// Note that in my rendition of the SFX in absolute-relative,
	// they are stored little endian, so I'm making that assumption.
	u16 addr;
	const u8 *sfx = sfx_start;
	voice *voice_table;
	struct smps_settings sfx_set;

	u8 timing_divisor;
	u8 total_channels;

	SetSMPSSettings(&sfx_set);
	// Get custom voice table address (may be zero)
	addr = get16_set(&sfx, &sfx_set);
	if(addr != 0x0000)
		voice_table = (voice *)&sfx_start[addr];
	else
		// Not really explicitly needed, but will help catch errors
		voice_table = NULL;

	// Get timing divisor and total channels of this sound effect
	timing_divisor = *sfx++;
	total_channels = *sfx++;

	while(total_channels-- > 0)
	{
		u8 voice_control = *(sfx+1);
		u8 sfx_track_index;
		struct smps_track *sfx_track, *music_track;

		// The voice_control byte will determine if this is one of the FM
		// SFX tracks or a PSG one...
		if(voice_control & VOICE_ISPSG_TRACK)	// PSG uses 0x80, 0xA0, 0xC0/0xE0
		{
			// This is a PSG track!  Determine which one (0xC0 / 0xE0 are
			// the same track, just one is tone and other is noise)
			sfx_track_index = (voice_control == 0xE0) ? TRACK_SFX_PSG3 :
				(TRACK_SFX_PSG1 + ((voice_control & 0x7F) >> 5));

			// PSG3 must silence both tone and noise
			if(voice_control == 0xC0)
			{
				sn76496_write(0xC0 | 0x1F);		// Silences PSG3
				sn76496_write(0xE0 | 0x1F);		// Silence noise
			}
		}
		else
		{
			// This is an FM track!
			// FM voice_control values are 0x00, 0x01, 0x02, 0x04, 0x05, 0x06 
			// (notice gap) based on FM chip selection values.  SFX begins on
			// FM3 (2).  (FM1, FM2, and FM6 are not allowed!)
			
			if(voice_control < 0x02 || voice_control > 0x05)
				// Prevent invalid FM tracks (shouldn't be
				// needed, but just in case!)
				return;

			sfx_track_index = TRACK_SFX_FM3;
			if(voice_control >= 0x04)
				sfx_track_index += (voice_control - 3);
		}

		smps_data.smps_sfx_set[sfx_track_index - MUSIC_TRACK_TOTAL] = sfx_set;
		// Point to proper SFX track!
		sfx_track = &smps_tracks[sfx_track_index];

		// Remember this memory start
		smps_data.smps_sfx_track_starts[sfx_track_index-MUSIC_TRACK_TOTAL] = sfx_start;

		// Set "SFX is overriding this track" bit on cooresponding music track
		music_track = get_coor_music_track(sfx_track);		// Get cooresponding track
		music_track->playback_control |= PLAY_SFXOVERRIDE;	// Set it

		// Now we're going to clear this SFX track...
		memset(sfx_track, 0, sizeof(struct smps_track));

		// setup track
		sfx_track->playback_control = *sfx++;					// playback control
		sfx_track->voice_control	= *sfx++;					// voice control
		sfx_track->timing_divisor	= timing_divisor;			// timing divisor
		sfx_track->current_duration	= 1;						// current duration timeout to 1 (will expire immediately and thus update)
		sfx_track->gosub_stack_loc	= sizeof(sfx_track->scratch_memory);	// Set stack position to end of scratch block
		sfx_track->pos				= get16_set(&sfx, &sfx_set);	// Track pos low/high byte
		sfx_track->tranpose			= *sfx++;					// key offset
	
		// RAS: If spindash active, the following block updates its transpose value:
		if(sfx_flags & SFX_ISSPINDASH)
			// Done!
			sfx_track->tranpose += smps_data.SpindashKeyOffset;

		sfx_track->vol = *sfx++;						// channel volume
		sfx_track->custom_voices	= voice_table;		// Custom voice table, if any

		if( (sfx_flags & (SFX_STEREOLEFT | SFX_STEREORIGHT)) == 0)
			sfx_track->pan_ams_fms		= 0xC0;			// Default panning / AMS / FMS settings (just L/R Stereo enabled)
		else
			sfx_track->pan_ams_fms		= (sfx_flags & (SFX_STEREOLEFT | SFX_STEREORIGHT)) << 6;	// Set L/R according to options
	}
}


void smps_sfx_setspindashfreq(s8 keyOffset)
{
	smps_data.SpindashKeyOffset = keyOffset;
}


static void ClearTrackPlaybackMem()
{
	// RAS: This totally wipes out the track memory and resets playback hardware

	// Clear all Channel 3 special settings
	ym2612_fm0_regdata(0x27, 0);
	
	// This performs a full clear across all track/playback memory
	memset(&smps_data, 0, sizeof(smps_data));
	memset(&smps_tracks, 0, sizeof(smps_tracks));

	FMSilenceAll();			// Silence FM
	PSGSilenceAll();		// Silence PSG
}


#ifdef _DEBUG
void ClearLine(void);
#endif

static void print_debug_msg(struct smps_track *t, u8 Cmd, u8 CmdLen)
{
#ifdef _DEBUG
	char TempStr[0x10];
	u8 CurByt;
	
	strcpy(TempStr, "");
	for (CurByt = 0x00; CurByt < CmdLen; CurByt ++)
		sprintf(TempStr, "%s %02X", TempStr, smps_data.smps_start[t->pos + CurByt]);
	
	ClearLine();
	printf("Track %u, Pos 0x%04X: Command %02X%s\n", GET_TRACK_INDEX(t), t->pos, Cmd, TempStr);
#endif
	return;
}

static void vgm_loop_start_check(struct smps_track *t);
static void vgm_loop_end_check(struct smps_track *t);

static void doCoordFlag(struct smps_track *t, u8 cmd)
{
	const u8 *data = smps_data.smps_start;	// Load track position
	const struct smps_settings* smps_set = get_track_settings(t);

	// Condition for Breakpoint:
	// cmd != 0xE0 && cmd != 0xE1 && cmd != 0xE4 && cmd != 0xE6 && cmd != 0xE7 && cmd != 0xE8 && cmd != 0xE9 && cmd != 0xEA && cmd != 0xEB && cmd != 0xEC && cmd != 0xEF && cmd != 0xF0 && cmd != 0xF2 && cmd != 0xF3 && cmd != 0xF5 && cmd != 0xF6 && cmd != 0xF7 && cmd != 0xF8 && cmd != 0xF9 && cmd != 0xFB && cmd != 0xFD
	switch(cmd)
	{
	case CF_PANAMSFMS:			// E0 Panning: AMS: FMS
		//cfPanningAMSFMS:

		// RAS (via Saxman's doc): panning, AMS, FMS  
		//Panning, AMS, FMS
		//* xx - Value (reg a)
		//      o Bit 7 - Left channel status
		//      o Bit 6 - Right channel Status
		//      o Bit 5-3 - AMS
		//      o Bit 2 - 0
		//      o Bit 1-0 - FMS 

		// RAS: Subject to verification, but even though you COULD set
		// AMS/FMS values, it does not appear that's what they intended
		// here; instead it appears they only meant for panning control.
		// I say this because it retains prior AMS/FMS settings ("& 0x37")

		if(	!(t->voice_control & VOICE_ISPSG_TRACK) &&	// Must not be a PSG track
			!(t->playback_control & PLAY_NOATTACK) )	// And must not be set to not attack
		{
			u8 reg = 0xB4 + (t->voice_control & 3);		// Proper stereo output / LFO sensitivity register
			u8 oldPAF = t->pan_ams_fms;					// old PAF value
			u8 newPAF = data[t->pos++];					// new PAF

			// OR them together
			newPAF = (oldPAF & 0x37) | newPAF;			// 0x37 = Keep AMS/FMS settings!

			// Store new PAF value
			t->pan_ams_fms = newPAF;

			// Write it to FM!
			WriteFM0or1(t, reg, newPAF);
		}
		else
			t->pos++;

		break;

	case CF_FREQADJ:			// E1 Alter note frequency
		// cfAlterNotesUNK:

		// More or less a pitch bend; this is applied to the frequency as a signed value
		t->freq_adjust = (s8)data[t->pos++];	// set new frequency adjust
		break;

	case CF_RETURN:				// E3 Return from case CF_GOSUB (S1/S2 ONLY)
		if (smps_set->coord_mode == CFLAG_S3K)
		{
			doCoordFlag(t, CF_STOPTRACK);
			break;
		}
		else if (smps_set->coord_mode == CFLAG_TRS)
		{
			// for Phantasy Star IV
			print_debug_msg(t, cmd, 0x01);
			t->pos ++;
			break;
		}
		// fall through
	case CF_RETURN_S3:			// F9 Return from case CF_GOSUB (S3/S&K/3K ONLY)
		// Return (Sonic 1 & 2)
		// cfJumpReturn:
		if (cmd == CF_RETURN_S3 && smps_set->coord_mode == CFLAG_S12)
		{
			ym2612_fm0_regdata(0x88, 0x0F);
			ym2612_fm0_regdata(0x8C, 0x0F);
			break;
		}

		{
			// Point to current area within stack
			u8 *stack_pos = &t->scratch_memory[t->gosub_stack_loc];

			// Get word from here -> track's position
			t->pos = get16(&stack_pos);

			// Pop stack
			t->gosub_stack_loc += 2;
		}

		break;

	case CF_FADEIN_S3:			// E2 Fade-in to previous song (S3/S&K/3K ONLY); put on DAC channel

		// This also appears in Sonic 1 & 2, but I don't know what it's supposed to do
		// In the Sonic 2 Z80 driver, it just sets the supplied value to zComRange+6 ???!
		// Maybe for some kind of music timing??  (Appears near the end-of-track in the
		// ending song for example... revisit this later...)

		// Since Sonic 3 apparently uses 'FF' as a parameter to denote fade-in, that's
		// the only time we'll allow it down into the fade-in routine, otherwise we're
		// just going to eat this event for now...
		if(data[t->pos++] != 0xFF)
		{
			// So if not DAC track, we just eat this.
			break;
		}

	case CF_FADEIN:				// E4 Fade-in to previous song (S1/S2 ONLY); put on DAC channel
		if (smps_set->coord_mode == CFLAG_S12)
		{
			int i;

			// Fade-in to previous song (needed on DAC channel, Sonic 1 & 2)
			// cfFadeInToPrevious:
			StopSignal();

			// RAS: This performs a "massive" restoration of all of the current 
			// track positions as they were prior to 1-up BGM
			memcpy(&smps_data,		&smps_backup,			sizeof(smps_backup));			// Player data
			memcpy(&smps_tracks,	&smps_tracks_backup,	sizeof(smps_tracks_backup));	// Music tracks


			// DAC doesn't fade in with music (although it probably COULD on PC...)
			smps_tracks[TRACK_DAC].playback_control |= PLAY_SFXOVERRIDE;	// Set "SFX is overriding" on it (not normal, but will work for this purpose)

			// It was like this in code... I guess to prevent fade-ins from overlapping?
			// Usually smps_data.vol_fadein is zero, so you can typically expect this
			// to be the same as if the subtraction weren't there.
			smps_data.vol_fadein = FADE_LEVELS - smps_data.vol_fadein;

			// Now all FM/PSG tracks need their hardware values reset
			for(i=TRACK_FM1; i<=TRACK_FM6; i++)
			{
				struct smps_track *track = &smps_tracks[i];
				if(track->playback_control & PLAY_ISPLAYING)	// Is this track playing?
				{
					track->playback_control |= PLAY_ATREST;		// Mark track at rest
					track->vol += smps_data.vol_fadein;			// Apply current fade value

					// Update voice and set volume (if allowed)
					FMSetVoice(t, smps_data.voice_table);
				}
			}

			for(i=TRACK_PSG1; i<=TRACK_PSG3; i++)
			{
				struct smps_track *track = &smps_tracks[i];
				if(track->playback_control & PLAY_ISPLAYING)	// Is this track playing?
				{
					track->playback_control |= PLAY_ATREST;		// Mark track at rest
					track->vol += smps_data.vol_fadein;			// Apply current fade value

					// --- Not done by the actual driver --v--
					if (track->voice_control == 0xE0)
						sn76496_write(track->psg_noise);	// restore PSG noise setting
					// --- Not done by the actual driver --^--

					// Note off all PSG tracks (if allowed)
					PSGNoteOff(t);
				}
			}


			// Prepare fade-in...
			smps_data.fadein_flag = TRUE;			// Stop any SFX during fade-in
			smps_data.vol_fadein = FADE_LEVELS;		// Reset fade counter
			smps_data.oneUp_flag = FALSE;			// 1-up ain't playin' no more
			smps_data.DAC_enabled = FALSE;			// DAC not yet enabled...

			// Stop DAC track (not exactly the same as Z80 behavior, but I can't
			// really screw with the return address for the same effect like it did :)
			t->playback_control &= ~PLAY_ISPLAYING;
		}
		else if (smps_set->coord_mode == CFLAG_S3K)
		{
			// E4 Set track volume (S3K)
			if(!(t->voice_control & VOICE_ISPSG_TRACK))
			{
				t->vol = (data[t->pos++] & 0x7F) ^ 0x7F;
				FMSetVolume(t);
			}
			else
			{
				t->vol = ((data[t->pos++] >> 3) & 0x0F) ^ 0x0F;
			}
		}
		else //if (smps_set->coord_mode == CFLAG_TRS)
		{
			u8 Param1 = data[t->pos];
			print_debug_msg(t, cmd, Param1 ? 0x05 : 0x01);
			t->pos ++;
			if (Param1)
				t->pos += 4;
		}

		break;

	case CF_SETTEMPODIV:		// E5 Change tempo divider
		if (smps_set->coord_mode == CFLAG_S12)
		{
			// RAS: Change tempo divider to xx
			// cfSetTempoDivider:
			t->timing_divisor = data[t->pos++];
			break;
		}
		else
		{
			t->pos ++;
			// fall through
		}

	case CF_CHANGEVOLUME:		// E6 Changes track volume
		if (smps_set->coord_mode == CFLAG_S12)
		{
			// RAS (via Saxman's doc): Change channel volume BY xx; xx is signed 
			// cfSetVolume:
			t->vol += (s8)data[t->pos++];	// Add to current volume

			if(!(t->voice_control & VOICE_ISPSG_TRACK))
				// Only FM requires special handling to change the volume
				FMSetVolume(t);
		}
		else
		{
			// S3K-Method
			s8 VolChange = (s8)data[t->pos++];
			
			if (! (t->voice_control & VOICE_ISPSG_TRACK))
			{
				if (t->vol + VolChange < 0x00)
					t->vol = 0;
				else if (t->vol + VolChange > 0x7F)
					t->vol = 0x7F;
				else
					t->vol += VolChange;
				FMSetVolume(t);
			}
		}
		break;

	case CF_NOATTACK:			// E7 Prevent next note from attacking
		// cfPreventAttack:
		t->playback_control |= PLAY_NOATTACK;
		break;

	case CF_SETNOTEFILL:		// E8 Set note fill amount to byte
		// RAS (via Saxman's doc): set note fill amount to xx 
		// cfNoteFill:
		if (smps_set->coord_mode == CFLAG_S12)
		{
			t->current_notefill = t->last_notefill = data[t->pos++];
		}
		else
		{
			t->current_notefill = t->last_notefill = data[t->pos++] * t->timing_divisor;
		}
		break;

	case CF_TRANSPOSE:			// E9 Add transposition to channel key (S1/S2 ONLY)
		if (smps_set->coord_mode == CFLAG_TRS)
		{
			// It seems that this one has some important meaning
			print_debug_msg(t, cmd, 0x02);
			t->pos += 2;
			break;
		}
		else if (smps_set->coord_mode != CFLAG_S12)
		{
			// SpindashRev (no arguments)
			break;
		}
		// else fall through
	case CF_TRANSPOSE_S3:		// FB Add transposition to channel key (S3/S&K/3K ONLY)
		// RAS (via Saxman's doc): add xx to channel key 
		//cfAddKey:
		t->tranpose += (s8)data[t->pos++];	//Add to current transpose value
		break;

	case CF_SETTEMPO:			// EA Change song tempo to unsigned byte
		if (smps_set->coord_mode == CFLAG_S12)
		{
			// RAS (via Saxman's doc): set music tempo to xx
			// cfSetTempo:
			smps_data.tempo = data[t->pos++];
		}
		else
		{
			DACPlay(data[t->pos++]);
		}
		break;

	case CF_SETTEMPODIVS:		// EB Set ALL per-track tempo dividers
		if (smps_set->coord_mode == CFLAG_S12)
		{
			// RAS (via Saxman's doc): Change Tempo Modifier to xx for ALL channels 
			// cfSetTempoMod:
			u8 divisor = data[t->pos++];
			u8 loop;

			for(loop=TRACK_DAC; loop<MUSIC_TRACK_TOTAL; loop++)
				t->timing_divisor = divisor;
		}
		else if (smps_set->coord_mode == CFLAG_S3K)
		{
			u8 loop_index = data[t->pos++];
			u8 loop_count = t->scratch_memory[loop_index];

			loop_count --;
			if (! loop_count)
			{
				t->scratch_memory[loop_index] = 0;
				// If still greater than zero, jump to the target address!
				t->pos = get16_jump(t);
			}
			else
			{
				// Otherwise, just skip the jump
				t->pos += 2;
			}
		}
		else //if (smps_set->coord_mode == CFLAG_TRS)
		{
			// not Phantasy Star IV, but SMPS 68k Treasure
			print_debug_msg(t, cmd, 0x02);
			t->pos ++;
			break;
		}
		break;

	case CF_CHANGEVOLUME_LATER:	// EC Like case CF_CHANGEVOLUME: except for FM tracks: volume change does not occur until next voice change
		if (smps_set->coord_mode == CFLAG_S12)
		{
			// RAS (via Saxman's doc): Change channel volume TO xx; xx is signed (Incorrect, see below)
			// However, I've noticed this is incorrect; first of all, you'll notice
			// it's still doing an addition, not a forced set.  Furthermore, it's
			// not actually altering the FM yet; basically, until the next voice
			// switch, this volume change will not come into effect.  Maybe a better
			// description of it is "change volume by xx when voice changes", which
			// makes sense given some voices are quieter/louder than others, and a
			// volume change at voice may be necessary... or my guess anyway.

			// Alternatively, just think of it as a volume setting optimized for PSG :P
			// cfChangeVolume:
			t->vol += (s8)data[t->pos++];	// Add to current volume
		}
		else
		{
			// in S3K this a PSG-only volume change flag
			s8 VolChange = (s8)data[t->pos++];
			
			if (t->voice_control & VOICE_ISPSG_TRACK)
			{
				t->playback_control &= ~PLAY_ATREST;
				t->psg_flutter --;
				t->vol += VolChange;
				if (t->vol + VolChange > 0x0F)
					t->vol = 0x0F;
			}
		}
		break;

	case CF_UNUSED_ED:
		if (smps_set->coord_mode == CFLAG_S12)
		{
			if (smps_set->ptr_mode == PTR_68K)
				;			// Sonic 1: clear 'pushing block' flag (no arguments)
			else
				t->pos ++;	// Sonic 2: do nothing (1 argument)
		}
		else if (smps_set->coord_mode == CFLAG_S3K)
		{
			// Set channel transposition
			t->tranpose = data[t->pos++] - 0x40;
		}
		else //if (smps_set->coord_mode == CFLAG_TRS)
		{
			// for Phantasy Star IV
			print_debug_msg(t, cmd, 0x01);
			t->pos ++;
		}
		break;

	case CF_UNUSED_EE:
		if (smps_set->coord_mode == CFLAG_S12)
		{
			if (smps_set->ptr_mode == PTR_68K)
			{
				// Sonic 1: Stop the track (for FM4 only, no arguments)
				t->playback_control &= ~PLAY_ISPLAYING;
			}
			//else
			//	;	// Sonic 2: do nothing (no arguments)
		}
		else if (smps_set->coord_mode == CFLAG_S3K)
		{
			// Write YM2612 register (write to 4000 and 4001)
			u8 Reg = data[t->pos ++];
			u8 Data = data[t->pos ++];
			
			ym2612_fm0_regdata(Reg, Data);
		}
		else //if (smps_set->coord_mode == CFLAG_TRS)
		{
			// for Phantasy Star IV
			// in PS4 Title Theme, this bounces between 01 and 02
			// seems to be some sort of DAC sound selection
			if (! t->current_voice)
				print_debug_msg(t, cmd, 0x01);
			t->current_voice = data[t->pos];
			t->pos ++;
		}
		break;

	case CF_CHANGE_VOICE:		// EF Change FM voice (NOT PSG tone!)
		if(!(t->voice_control & VOICE_ISPSG_TRACK))
		{
			voice *voice_table;

			// RAS (via Saxman's doc): set voice selection to xx 
			// cfSetVoice:
			t->current_voice = data[t->pos++];	// Set current voice
			if (smps_set->coord_mode == CFLAG_S3K)
			{
				if (t->current_voice & 0x80)
				{
					t->current_voice &= 0x7F;
					// actually this would be FM voices from other songs
					t->pos ++;
				}
			}

			// Trick: This may be a sound effect, which uses its own
			// private table... select appropriate one here
			if(IS_SFX_TRACK(t))
				voice_table = t->custom_voices;
			else
				voice_table = smps_data.voice_table;

			FMSetVoice(t, voice_table);
		}

		break;

	case CF_MODULATION:			// F0 Setup modulation
		// RAS (via Saxman's doc): F0wwxxyyzz - modulation 
		//							o	ww - Wait for ww period of time before modulation starts 
		//							o	xx - Modulation Speed 
		//							o	yy - Modulation change per Mod. Step 
		//							o	zz - Number of steps in modulation 
		//cfModulation:
		t->playback_control |= PLAY_MODULATIONON;

		// Store this address for present and future settings
		t->modcfg = &data[t->pos];

		// Now do it!
		SetModulation(t);

		// Jump passed parameter bytes...
		t->pos += 4;
		break;

	case CF_MODULATIONON:		// F1 Turn on modulation
		if (smps_set->coord_mode == CFLAG_S12)
		{
			// RAS (via Saxman's doc): Turn on modulation 
			//cfEnableModulation:
			t->playback_control |= PLAY_MODULATIONON;
		}
		else
		{
			// Set modulation type (FM mod, PSG mod)
			if (! (t->voice_control & VOICE_ISPSG_TRACK))
			{
				// FM modulation
				if (data[t->pos] & 0x80)
					t->playback_control |= PLAY_MODULATIONON;
				else
					t->playback_control &= ~PLAY_MODULATIONON;
			}
			t->pos ++;
			
			if (t->voice_control & VOICE_ISPSG_TRACK)
			{
				// PSG modulation
				if (data[t->pos] & 0x80)
					t->playback_control |= PLAY_MODULATIONON;
				else
					t->playback_control &= ~PLAY_MODULATIONON;
			}
			t->pos ++;
		}
		break;

	case CF_STOPTRACK:			// F2 Stops this track from playing (non-looping BGMs only)
		// RAS (via Saxman's doc): stop the track 
		//cfStopTrack:
		t->playback_control &= ~(PLAY_ISPLAYING | PLAY_NOATTACK);	// Clear "currently playing" and "no attack" bits

		if(!(t->voice_control & VOICE_ISPSG_TRACK))
		{
			// As long as this isn't the DAC track...
			if(GET_TRACK_INDEX(t) != TRACK_DAC)	
				FMNoteOff(t);	// Stop this FM track
		}
		else
		{
			// PSG version
			// The easy part...
			PSGNoteOff(t);		// Silence PSG!
		}

		// If track is an SFX track, there's more work to do regarding
		// restoration of the previous track...
		if(IS_SFX_TRACK(t))
		{
			if(!(t->voice_control & VOICE_ISPSG_TRACK))
			{
				// This is an FM SFX track that's trying to stop
				struct smps_track *m = get_coor_music_track(t);	// Get cooresponding music track

				if(m->playback_control & PLAY_SFXOVERRIDE)	// If "SFX is overriding this track" is not set there's nothing to do!
				{
					m->playback_control &= ~PLAY_SFXOVERRIDE;	// Clear SFX is overriding this track from playback control
					m->playback_control |= PLAY_ATREST;			// Set track as resting bit

					FMSetVoice(m, smps_data.voice_table);		// Reset music FM voice for this track!
				}
			}
			else
			{
				//zStopPSGSFXTrack:

				// This is an PSG SFX track that's trying to stop
				struct smps_track *m = get_coor_music_track(t);	// Get cooresponding music track

				m->playback_control &= ~PLAY_SFXOVERRIDE;	// Clear SFX is overriding this track from playback control
				m->playback_control |= PLAY_ATREST;			// Set track as resting bit

				// Only PSG "noise" type tracks require any restoration
				if(m->voice_control == 0xE0)	// Is this a PSG 3 noise (not tone) track?
					sn76496_write(m->psg_noise);	// Write PSG noise setting
			}
		}
		else
		{
			// some non-sound-driver code for playback control
			u8 CurChn;
			u8 IsPlaying;
			
			IsPlaying = 0x00;
			for (CurChn = TRACK_DAC; CurChn < MUSIC_TRACK_TOTAL; CurChn ++)
				IsPlaying |= (smps_tracks[CurChn].playback_control & PLAY_ISPLAYING);
			
			if (! IsPlaying)
				StopSignal();
		}
		break;

	case CF_PSGSETNOISE:			// F3 Set current PSG noise (effects noise channel only)
		// RAS (via Saxman's doc): Change current PSG noise to xx (For noise channel, E0-E7) 
		// cfSetPSGNoise:

		if (smps_set->coord_mode == CFLAG_S12)
		{
			// This is a PSG noise track now!
			t->voice_control = 0xE0;

			// Get PSG noise setting
			t->psg_noise = data[t->pos++];

			// If SFX is currently overriding it, don't actually set it!
			if(!(t->playback_control & PLAY_SFXOVERRIDE))	// Is this a PSG 3 noise (not tone) track?
			{
				sn76496_write(0xC0 | 0x1F);		// Stop channel 3 tone
				sn76496_write(t->psg_noise);	// Write PSG noise setting
			}
		}
		else
		{
			if (t->voice_control & VOICE_ISPSG_TRACK)
			{
				t->psg_noise = data[t->pos++];
				
				if (! t->psg_noise)
					t->voice_control = 0xC0;
				else
					t->voice_control = 0xE0;
				
				if (! (t->playback_control & PLAY_SFXOVERRIDE))
				{
					sn76496_write(0xC0 | 0x1F);		// Stop channel 3 tone
					
					if (! t->psg_noise)
						sn76496_write(0xE0 | 0x1F);		// Stop channel 4 tone
					else
						sn76496_write(t->psg_noise);	// Write PSG noise setting
				}
			}
		}
		break;

	case CF_MODULATIONOFF:		// F4 Turn off modulation
		if (smps_set->coord_mode == CFLAG_S12)
		{
			// RAS (via Saxman's doc): Turn off modulation 
			// cfDisableModulation:
			t->playback_control &= ~PLAY_MODULATIONON;	// Clear "modulation on" bit!
		}
		else
		{
			// Set modulation type (actually this should do more)
			if (data[t->pos] & 0x80)
				t->playback_control |= PLAY_MODULATIONON;
			else
				t->playback_control &= ~PLAY_MODULATIONON;
			t->pos ++;
		}
		break;

	case CF_CHANGE_PSGTONE:		// F5 Change PSG tone (NOT FM voice!)
		// RAS (via Saxman's doc): Change current PSG tone to xx 
		// cfSetPSGTone:
		t->current_voice = data[t->pos++];
		t->current_voice %= FLUTTER_COUNT;	// the % is not in the actual sound driver
		break;

	case CF_JUMP:				// F6 Jump to offset
		{
			// RAS (via Saxman's doc): jump to position yyyy 
			// cfJumpTo:
			t->pos = get16_jump(t);
			vgm_loop_end_check(t);
		}
		break;

	case CF_REPEAT:				// F7 Repeat section of music
		{
			// RAS (via Saxman's doc): $F7xxyyzzzz - repeat section of music
			//    * xx - loop index, for loops within loops without confusing the engine.
			//          o EXAMPLE: Some notes, then a section that is looped twice, then some more notes, and finally the whole thing is looped three times.
			//            The "inner" loop (the section that is looped twice) would have an xx of 01, looking something along the lines of F70102zzzz, whereas the "outside" loop (the whole thing loop) would have an xx of 00, looking something like F70003zzzz. 
			//    * yy - number of times to repeat
			//          o NOTE: This includes the initial encounter of the F7 flag, not number of times to repeat AFTER hitting the flag. 
			//    * zzzz - position to loop back to   
			// cfRepeatAtPos:
			u8 loop_index = data[t->pos++];					// Loop index (can have a few more than one loop...)
			u8 num_repeats = data[t->pos++];				// Number of times this loop is supposed to repeat
			u8 loop_count = t->scratch_memory[loop_index];	// Which loop we're currently on

			// If loop_count is zero NOW, this is a NEW loop at this index!
			if(loop_count == 0)
				t->scratch_memory[loop_index] = num_repeats;

			// Either way, decrement it...
			if(--t->scratch_memory[loop_index] > 0)
			{
				// If still greater than zero, jump to the target address!
				t->pos = get16_jump(t);
			}
			else
				// Otherwise, just skip the jump
				t->pos += 2;
		}

		break;

	case CF_GOSUB:				// F8 Jump to offset: but keep return address in memory
		// RAS (via Saxman's doc): jump to position yyyy (keep previous position in memory for returning) 
		// cfJumpToGosub:
		{
			u8 *stackPtr;
			const u8 *jmpBytes = &data[t->pos];
			const struct smps_settings* smps_set = get_track_settings(t);

			// Subtract 2 (push); we need to store a new address on the stack!
			t->gosub_stack_loc -= 2;

			// Get current stack pointer
			stackPtr = &t->scratch_memory[t->gosub_stack_loc];

			// Store the return address into the stack!
			put16(&stackPtr, t->pos+2);

			// Get jump position
			t->pos = get16_jump(t);
			break;
		}

	case CF_UNUSED_FA:
		if (smps_set->coord_mode == CFLAG_S3K)
		{
			// actually this should clear bit 7 of the modulation type
			t->playback_control &= ~PLAY_MODULATIONON;
		}
		else //if (smps_set->coord_mode == CFLAG_TRS)
		{
			// for Phantasy Star IV
			print_debug_msg(t, cmd, 0x01);
			t->pos += 1;
		}
		break;

	case 0xFC:
		if (smps_set->coord_mode == CFLAG_S3K)
		{
			// for continuous SFX only
			if (0)
			{
				t->pos = get16_jump(t);
			}
			else
			{
				t->pos += 2;
			}
		}
		else if (smps_set->coord_mode == CFLAG_TRS)
		{
			// for Phantasy Star IV: A Happy Settlement (DAC channel)
			// controls DAC volume - I guess it's 00 (silent) 0F (max)
			//print_debug_msg(t, cmd, 0x02);
			t->pos ++;
		}
		break;

	case 0xFD:
		if (smps_set->coord_mode == CFLAG_S3K)
		{
			// alternate SMPS mode
			u8 AltMode = data[t->pos ++];
			if (AltMode == 0x01)
			{
				// enable alternate SMPS mode
				t->playback_control &= ~PLAY_ISPLAYING;	// not supported ... stop the track
			}
			else
			{
				// disable alternate SMPS mode
			}
		}
		else
		{
			// Turn off modulation (for newer SMPS 68k (e.g. Treasure)
			t->playback_control &= ~PLAY_MODULATIONON;	// Clear "modulation on" bit!
		}
		break;

	case 0xFE:
		if (smps_set->coord_mode == CFLAG_S3K)
		{
			if (t->voice_control == 0x02)
			{
				// put FM3 in special mode
				t->pos += 4;
				ym2612_fm0_regdata(0x27, 0x4F);
			}
			else
			{
				t->pos += 4;
			}
		}
		break;

	case 0xFF:
		if (smps_set->coord_mode == CFLAG_S3K)
		{
			cmd = data[t->pos ++];
			switch(cmd)
			{
			case 0x00:	// Change song tempo to unsigned byte
				smps_data.tempo = data[t->pos++];
				break;
			case 0x01:	// Play sound by index
				t->pos ++;
				break;
			case 0x02:	// Halt or resume music tracks
				// zero - halt, non-zero - resume
				if (! (t->pos ++))
				{
					int i;
					
					for (i = TRACK_DAC; i < MUSIC_TRACK_TOTAL; i ++)
					{
						struct smps_track *track = &smps_tracks[i];
						
						track->playback_control &= ~PLAY_ISPLAYING;
						if (! (t->voice_control & VOICE_ISPSG_TRACK))
							FMNoteOff(t);
					}
					PSGSilenceAll();
				}
				else
				{
					int i;
					
					for (i = TRACK_DAC; i < MUSIC_TRACK_TOTAL; i ++)
						smps_tracks[i].playback_control |= PLAY_ISPLAYING;
				}
				break;
			case 0x03:	// copy data
				t->pos += 3;
				break;
			case 0x04:	// Set per-track tempo dividers for all music tracks
				{
					u8 divisor = data[t->pos++];
					u8 loop;

					for(loop=TRACK_DAC; loop<MUSIC_TRACK_TOTAL; loop++)
						t->timing_divisor = divisor;
				}
				break;
			case 0x05:	// set SSG-EG data
				FMSetSSGEG(t);
				t->pos += 4;
				break;
			case 0x06:	// Start/end FM flutter
				t->pos += 2;
				break;
			case 0x07:	// Reset spindash rev.
				break;
			case 0x08:	// Set Tempo Divider (Flamewing's S&K driver only)
				t->timing_divisor = data[t->pos++];
				break;
			case 0x09:	// Send YM2612 channel command (Flamewing's S&K driver only)
				{
					u8 Reg = data[t->pos ++];
					u8 Data = data[t->pos ++];
					
					WriteFM0or1(t, Reg, Data);
				}
				break;
			case 0x0A:	// set cfNoteFillSet (Flamewing's S&K driver only)
				t->current_notefill = t->last_notefill = data[t->pos++];
				break;
			}
		}
		else if (smps_set->coord_mode == CFLAG_TRS)
		{
			print_debug_msg(t, cmd, 0x02);
			cmd = data[t->pos ++];
			switch(cmd)
			{
			case 0x02:
				t->pos ++;
				break;
			}
		}
		break;

	default:
		DEBUGASSERT(0);
		break;
	}
}


static void UpdateFadeout()
{
	u8 loop;

	if(smps_data.vol_fadeouttick == 0)
	{
		if(--smps_data.vol_fadeout > 0)
		{
			// Reload tick count
			smps_data.vol_fadeouttick = FADEOUT_TICKRESET;	

			// For all 6 FM tracks...
			for(loop=TRACK_FM1; loop<=TRACK_FM6; loop++)
			{
				struct smps_track *t = &smps_tracks[loop];

				// Only do anything if track is playing
				if(t->playback_control & PLAY_ISPLAYING)
				{
					if(t->vol < 0x7F)	
					{
						// increment channel volume (remember -- higher is quieter!)
						t->vol++;

						// update hardware
						FMSetVolume(t);
					}
					else
						// If we hit the quietest volume on this track, stop playing!
						t->playback_control &= ~PLAY_ISPLAYING;
				}
			}
	

			// For all 3 PSG tracks...
			for(loop=TRACK_PSG1; loop<=TRACK_PSG3; loop++)
			{
				struct smps_track *t = &smps_tracks[loop];

				// Only do anything if track is playing
				if(t->playback_control & PLAY_ISPLAYING)
				{
					if(t->vol < 0xF)	
					{
						// increment channel volume (remember -- higher is quieter!)
						t->vol++;

						// update hardware
						PSGUpdateVol(t, t->vol);
					}
					else
						// If we hit the quietest volume on this track, stop playing!
						t->playback_control &= ~PLAY_ISPLAYING;
				}
			}
		}
		else
			// If fade out hits zero, clear all memory and reset hardware!
			ClearTrackPlaybackMem();
	}
	else
		// Not zero yet, decrement it
		smps_data.vol_fadeouttick--;
}


static void UpdateFadein()
{
	u8 loop;
	if(smps_data.vol_fadeintick == 0)
	{
		if(--smps_data.vol_fadein > 0)
		{
			// Reload tick count
			smps_data.vol_fadeintick = FADEIN_TICKRESET;	

			// For all 6 FM tracks...
			for(loop=TRACK_FM1; loop<=TRACK_FM6; loop++)
			{
				struct smps_track *t = &smps_tracks[loop];

				// Only do anything if track is playing
				if(t->playback_control & PLAY_ISPLAYING)
				{
					// decrement channel volume (remember -- lower is louder!)
					t->vol--;

					// update hardware
					FMSetVolume(t);
				}
			}
	

			// For all 3 PSG tracks...
			for(loop=TRACK_PSG1; loop<=TRACK_PSG3; loop++)
			{
				struct smps_track *t = &smps_tracks[loop];

				// Only do anything if track is playing
				if(t->playback_control & PLAY_ISPLAYING)
				{
					// decrement channel volume (remember -- lower is louder!)
					t->vol--;

					// update hardware
					PSGUpdateVol(t, t->vol);
				}
			}
		}
		else
		{
			// If fade in hits zero, reenable DAC and SFX
			smps_data.DAC_enabled = TRUE;
		}
	}
	else
		// Not zero yet, decrement it
		smps_data.vol_fadeintick--;
}


static void DACUpdateTrack(struct smps_track *t)
{
	if(--t->current_duration == 0)
	{
		u8 cmd;
		const u8 *data = smps_data.smps_start;	// Load track position

		t->playback_control &= ~PLAY_NOATTACK;	// Clear "do not attack" but

		//////////////////////
		// Inline of zFMDoNext
		//////////////////////
		t->playback_control &= ~PLAY_ATREST;	// Clear bit 1 (02h) "track is rest" from track

		// Perform ALL coordination flags until we run out of them
		do
		{
			vgm_loop_start_check(t);
			if((cmd = data[t->pos++]) >= CF_START)
				doCoordFlag(t, cmd);
		} while(
			(cmd >= CF_START) &&						// Loop until all coordination flags are processed
			(t->playback_control & PLAY_ISPLAYING));	// ... and track must still be playing (new; not in Z80.  Instead they just pop'ed a few return addresses :))

		// Track must still be playing (related to the fact I can't pop return addresses)
		if(t->playback_control & PLAY_ISPLAYING)
		{
			u8 JustPlayed = FALSE;
			
			// Is this a note?
			if(cmd >= 0x80)
			{
				const struct smps_settings* smps_set = get_track_settings(t);
				if ((smps_set->ptr_mode == PTR_68K && smps_set->coord_mode == CFLAG_S12) && ! smps_data.DAC_on)
				{
					// Actually Sonic 1 sends this before EVERY DAC sound it plays.
					// DAC_on is only used to make cleaner VGM logs.
					ym2612_fm0_regdata(0x2B, 0x80);
					smps_data.DAC_on = TRUE;
				}
				
				// Play DAC sound!
				if (cmd > 0x80 && smps_set->coord_mode == CFLAG_TRS && t->current_voice)
					cmd = 0x80 + t->current_voice;
				DACPlay(cmd);
				JustPlayed = TRUE;

				// Remember what note this is...
				t->frequency = cmd;

				// Look ahead to next byte to immediately set
				// a duration if it follows; this CAN be another
				// note, however; you may reuse the previous
				// duration several times for the same note!
				cmd = data[t->pos];

				// We're about to handle a duration; so we'll
				// maintain this read!
				if(cmd < 0x80)
				{
					vgm_loop_start_check(t);
					t->pos++;
				}
			}

			if(cmd < 0x80)
			{
				// On DAC track only, the previous note is replayed
				// on every duration command
				if (! JustPlayed)
					DACPlay((u8)t->frequency);

				// Set it!
				SetDuration(t, cmd);
			}


			// Generic finishing work on the track update
			FinishTrackUpdate(t);
		}
	}
}


void stupid(int x);
static void FMUpdateTrack(struct smps_track *t)
{
	u16 mod_freq;

	//zFMUpdateTrack:
	if(--t->current_duration == 0)
	{
		u8 cmd;
		const u8 *data = smps_data.smps_start;	// Load track position

		t->playback_control &= ~PLAY_NOATTACK;	// Clear "do not attack" but

		//////////////////////
		// Inline of zFMDoNext
		//////////////////////
		t->playback_control &= ~PLAY_ATREST;	// Clear bit 1 (02h) "track is rest" from track

		// Perform ALL coordination flags until we run out of them
		do
		{
			vgm_loop_start_check(t);
			if((cmd = data[t->pos++]) >= CF_START)
				doCoordFlag(t, cmd);
		} while(
			(cmd >= CF_START) &&						// Loop until all coordination flags are processed
			(t->playback_control & PLAY_ISPLAYING));	// ... and track must still be playing (new; not in Z80.  Instead they just pop'ed a few return addresses :))

		// Track must still be playing (related to the fact I can't pop return addresses)
		if(t->playback_control & PLAY_ISPLAYING)
		{
			FMNoteOff(t);	// Send key off

			// Is this a note?
			if(cmd >= 0x80)
			{
				// Set FM Frequency!  (Or put track at rest)
				FMSetFreq(t, cmd);

				// Look ahead to next byte to immediately set
				// a duration if it follows; this CAN be another
				// note, however; you may reuse the previous
				// duration several times for the same note!
				cmd = data[t->pos];

				// We're about to handle a duration; so we'll
				// maintain this read!
				if(cmd < 0x80)
				{
					vgm_loop_start_check(t);
					t->pos++;
				}
			}

			if(cmd < 0x80)
				// Set it!
				SetDuration(t, cmd);


			// Generic finishing work on the track update
			FinishTrackUpdate(t);

			//////////////////////
			// End inline of zFMDoNext
			// Begin inline of zFMPrepareNote
			//////////////////////

			// zFMPrepareNote:
			if(!(t->playback_control & PLAY_ATREST))
			{
				// Not at rest, but if frequency is zero,
				// it will go to rest...
				if(t->frequency != 0x0000)
					// Update frequency!
					FMUpdateFreq(t, t->frequency);
				else
					// Frequency was zero
					t->playback_control |= PLAY_ATREST;
			}

			//////////////////////
			// End inline of zFMPrepareNote
			// Begin inline of zFMNoteOn
			//////////////////////
			
			// Note on!  (Except if at rest or being overridden)
			if(!(t->playback_control & (PLAY_ATREST | PLAY_SFXOVERRIDE)))
			{
				// -ALL- FM Key On/Off commands go to -FM0- reg 28h
				ym2612_fm0_regdata(0x28, t->voice_control | 0xF0);	// 0xF0 -- turn on all operators
			}
		}
	}
	else
	{
		// Applies "note fill" (time until cut-off); NOTE: Will quit here if "note fill" expires
		if(NoteFillUpdate(t))
			return;		
	}

	// If modulation takes place, a non-zero new frequency is returned
	mod_freq = DoModulation(t);
	if(mod_freq != 0)
		FMUpdateFreq(t, mod_freq);
}


static void PSGUpdateTrack(struct smps_track *t)
{
	u16 mod_freq;

	// zPSGUpdateTrack:
	if(--t->current_duration == 0)
	{
		u8 cmd;
		const u8 *data = smps_data.smps_start;	// Load track position

		// Reset "do not attack next note" bit
		t->playback_control &= ~PLAY_NOATTACK;	

		//////////////////////
		// Inline of zPSGDoNext
		//////////////////////
		t->playback_control &= ~PLAY_ATREST;	// Clear bit 1 (02h) "track is rest" from track

		// Perform ALL coordination flags until we run out of them
		do
		{
			vgm_loop_start_check(t);
			if((cmd = data[t->pos++]) >= CF_START)
				doCoordFlag(t, cmd);
		} while(
			(cmd >= CF_START) &&						// Loop until all coordination flags are processed
			(t->playback_control & PLAY_ISPLAYING));	// ... and track must still be playing (new; not in Z80.  Instead they just pop'ed a few return addresses :))

		// Track must still be playing (related to the fact I can't pop return addresses)
		if(t->playback_control & PLAY_ISPLAYING)
		{
			// Is this a note?
			if(cmd >= 0x80)
			{
				// Set FM Frequency!  (Or put track at rest)
				PSGSetFreq(t, cmd);

				// Look ahead to next byte to immediately set
				// a duration if it follows; this CAN be another
				// note, however; you may reuse the previous
				// duration several times for the same note!
				cmd = data[t->pos];

				// We're about to handle a duration; so we'll
				// maintain this read!
				if(cmd < 0x80)
				{
					vgm_loop_start_check(t);
					t->pos++;
				}
			}

			if(cmd < 0x80)
			{
				// Set it!
				SetDuration(t, cmd);
			}

			// Generic finishing work on the track update
			FinishTrackUpdate(t);

			//////////////////////
			// End inline of zPSGDoNext
			// Begin inline of zPSGNoteOn
			//////////////////////
			if(t->frequency != 0xFFFF)	// Check if track is at rest (frequency was set to FFFFh)...
				PSGUpdateFreq(t, t->frequency);

			PSGDoVolFX(t);	//  This applies PSG volume as well as its special volume-based effects that I call "flutter"
		}
	}
	else
	{
		// Applies "note fill" (time until cut-off); NOTE: Will quit here if "note fill" expires
		if(NoteFillUpdate(t))
			return;	

		// Inline of zPSGUpdateVolFX
		// Update PSG flutter volume effects, but only if not tone 0
		// zPSGUpdateVolFX:
		if(t->current_voice != 0)	// Is tone non-zero?
			PSGDoVolFX(t);			// Apply PSG volume as well as its special volume-based effects
	}
	
	// If modulation takes place, a non-zero new frequency is returned
	mod_freq = DoModulation(t);
	if(mod_freq != 0)
		PSGUpdateFreq(t, mod_freq);
}


void SetSMPSMode(const u8 Mode)
{
	SMPS_MODE = Mode;
	
	return;
}

void SetSMPSOffset(const u16 offset)
{
	SMPS_OFFSET = offset;
	
	return;
}

static void vgm_loop_start_check(struct smps_track *t)
{
	u8 TrkID;
	u8 CurTrk;
	struct smps_track* TempTrk;
	
	if (LoopState.Activated)
		return;
	if (IS_SFX_TRACK(t))
		return;
	
	TrkID = GET_TRACK_INDEX(t);
	if (LoopState.TrkMinPos[TrkID] && t->pos != LoopState.TrkMinPos[TrkID])
		return;
	
	LoopState.TrkMask |= (1 << TrkID);
	for (CurTrk = TRACK_DAC; CurTrk < MUSIC_TRACK_TOTAL; CurTrk ++)
	{
		TempTrk = &smps_tracks[CurTrk];
		if ((TempTrk->playback_control & PLAY_ISPLAYING) &&
			! (LoopState.TrkMask & (1 << CurTrk)))
			return;	// One channel is still outside of a loop
		
		LoopState.TrkPos[CurTrk] = TempTrk->pos;
	}
	
	LoopState.Activated = 0x01;
	LoopState.TrkMask = TrkID;
	LoopStartSignal();
	
	return;
}

static void vgm_loop_end_check(struct smps_track *t)
{
	u8 CurTrk;
	struct smps_track* TempTrk;
	
	if (LoopState.Activated < 0x01)
		return;
	if (IS_SFX_TRACK(t))
		return;
	
	if (GET_TRACK_INDEX(t) != LoopState.TrkMask)
		return;
	
	for (CurTrk = TRACK_DAC; CurTrk < MUSIC_TRACK_TOTAL; CurTrk ++)
	{
		TempTrk = &smps_tracks[CurTrk];
		if ((TempTrk->playback_control & PLAY_ISPLAYING) &&
			smps_tracks[CurTrk].pos != LoopState.TrkPos[CurTrk])
			return;
	}
	
	LoopState.Activated = 0x02;
	LoopEndSignal();
	
	return;
}

void SetLoopBasePos(u8 TrkID, u16 Address)
{
	DEBUGASSERT(TrkID == 0xFF || (TrkID & 0x7F) < MUSIC_TRACK_TOTAL);
	if (TrkID == 0xFF)
	{
		LoopState.Activated = 0xFF;
		LoopState.TrkMask = 0x00;
		for (TrkID = 0x00; TrkID < MUSIC_TRACK_TOTAL; TrkID ++)
			LoopState.TrkMinPos[TrkID] = 0x0000;
	}
	else if ((TrkID & 0x7F) < MUSIC_TRACK_TOTAL)
	{
		LoopState.Activated = 0x00;
		if (TrkID & 0x80)
		{
			TrkID &= 0x7F;
			DEBUGASSERT(Address < MUSIC_TRACK_TOTAL);
			Address = LoopState.TrkMinPos[Address];
		}
		DEBUGASSERT(! LoopState.TrkMinPos[TrkID]);
		LoopState.TrkMinPos[TrkID] = Address;
	}
	
	return;
}


void LoadFlutterData(const char* FileName)
{
	const char SIG_ENV[] = "LST_ENV";
	FILE* hFile;
	u8 CurFlt;
	u8 TempByt;
	char TempStr[0x07];
	
	hFile = fopen(FileName, "rb");
	if (hFile == NULL)
	{
		printf("Error opening %s\n", FileName);
		printf("Loading default PSG envelopes.\n");
		goto BadFile;
	}
	
	fread(TempStr, 0x01, 0x07, hFile);
	if (strncmp(TempStr, SIG_ENV, 0x07))
	{
		fclose(hFile);
		printf("%s is invalid.\n", FileName);
		printf("Loading default PSG envelopes.\n");
		goto BadFile;
	}
	
	fread(&FLUTTER_COUNT, 0x01, 0x01, hFile);
	FlutterData = (u8**)malloc(FLUTTER_COUNT * sizeof(u8*));
	zPSG_FlutterTbl = (u8**)malloc((FLUTTER_COUNT + 1) * sizeof(u8*));
	
	for (CurFlt = 0x00; CurFlt < FLUTTER_COUNT; CurFlt ++)
	{
		fread(&TempByt, 0x01, 0x01, hFile);	// Envelope Name Length
		fseek(hFile, TempByt, SEEK_CUR);	// Envelope Name (skip)
		fread(&TempByt, 0x01, 0x01, hFile);	// Envelope Data Length
		FlutterData[CurFlt] = (u8*)malloc((TempByt + 0x01) * sizeof(u8));
		fread(FlutterData[CurFlt], 0x01, TempByt, hFile);
		FlutterData[CurFlt][TempByt] = 0x83;	// just to be safe
		
		zPSG_FlutterTbl[CurFlt] = FlutterData[CurFlt];
	}
	
	zPSG_FlutterTbl[CurFlt] = default_flutter_empty;
	fclose(hFile);
	
	return;

BadFile:
	
	FLUTTER_COUNT = DEFAULT_FLUTTER_COUNT;
	FlutterData = NULL;
	zPSG_FlutterTbl = (u8**)malloc((FLUTTER_COUNT + 1) * sizeof(u8*));
	zPSG_FlutterTbl[0x00] = default_flutter00;
	zPSG_FlutterTbl[0x01] = default_flutter01;
	zPSG_FlutterTbl[0x02] = default_flutter02;
	zPSG_FlutterTbl[0x03] = default_flutter03;
	zPSG_FlutterTbl[0x04] = default_flutter04;
	zPSG_FlutterTbl[0x05] = default_flutter05;
	zPSG_FlutterTbl[0x06] = default_flutter06;
	zPSG_FlutterTbl[0x07] = default_flutter07;
	zPSG_FlutterTbl[0x08] = default_flutter08;
	zPSG_FlutterTbl[0x09] = default_flutter09;
	zPSG_FlutterTbl[0x0A] = default_flutter0A;
	zPSG_FlutterTbl[0x0B] = default_flutter0B;
	zPSG_FlutterTbl[0x0C] = default_flutter0C;
	zPSG_FlutterTbl[0x0D] = default_flutter_empty;
	
	return;
}

void FreeFlutterData(void)
{
	free((void*)zPSG_FlutterTbl);	zPSG_FlutterTbl = NULL;
	
	if (FlutterData != NULL)
	{
		u8 CurFlt;
	
		for (CurFlt = 0x00; CurFlt < FLUTTER_COUNT; CurFlt ++)
			free(FlutterData[CurFlt]);
		
		free(FlutterData);	FlutterData = NULL;
	}
	
	FLUTTER_COUNT = 0x00;
	
	return;
}

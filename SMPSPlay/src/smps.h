#ifndef _S2RAS_SMPS_H
#define _S2RAS_SMPS_H

// ---------------------------------------------------------------------------
// smps_init
//
// Initializes the system for playing SMPS data.  Clears all internal track
// memories and resets the sound hardware
// ---------------------------------------------------------------------------
void smps_init();


// ---------------------------------------------------------------------------
// smps_playSong
//
// Plays an SMPS song.
// Note that speedshoe tempo is not part of the SMPS format, so you must supply 
// that yourself.  If you set it to zero, this function will simply equate it to 
// the song's normal tempo.
// ---------------------------------------------------------------------------
void smps_playSong(const u8 *smps_song, u8 speedshoe_tempo);


// ---------------------------------------------------------------------------
// smps_playSFX
//
// Plays a "simple SMPS" sound effect.
// Specify sfx_flags from the flag values below to emulate special cases
// that the sound driver provided.
// Setting neither STEREOLEFT nor STEREORIGHT assumes both speakers enabled
// ---------------------------------------------------------------------------
enum SFX_FLAGS
{
	// NOTE: SFX_STEREOLEFT / SFX_STEREORIGHT are used programatically;
	// do not change their order in this list!
	SFX_STEREORIGHT	= 0x01,	// Pan right (FM channels only)
	SFX_STEREOLEFT	= 0x02,	// Pan left (FM channels only)
	SFX_ISSPINDASH	= 0x04,	// Is a spindash; increase frequency each time played
};
void smps_playSFX(const u8 *smps_song, u8 sfx_flags);


// ---------------------------------------------------------------------------
// smps_sfx_setspindashfreq
//
// Called to update the key offset for the spindash only.
// Must specify SFX_SPINDASH in smps_playSFX for this to take effect.
// ---------------------------------------------------------------------------
void smps_sfx_setspindashfreq(s8 keyOffset);

// ---------------------------------------------------------------------------
// smps_update
//
// Call every frame to advance SMPS playback.
// ---------------------------------------------------------------------------
void smps_update();


enum SMPS_MODES
{
	SMODE_S1 = 0x00,
	SMODE_S2 = 0x01,
	SMODE_S3K = 0x02,
	SMODE_INVALID = 0xFF
};

void SetSMPSMode(const u8 Mode);
void SetSMPSOffset(const u16 offset);

void LoadFlutterData(const char* FileName);
void FreeFlutterData(void);

#endif

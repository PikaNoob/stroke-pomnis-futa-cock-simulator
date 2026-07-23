#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "common.h"
#include "smps.h"

// Internal flags used when loading SMPS music
#define SMPSLF_SAXMAN	0x01	// Indicates this song was Saxman compressed; assume $1380 offset
#define SMPSLF_Z80		0x02	// Specifies that the song was targeted for the Z80; uses little-endian absolute addresses
#define SMPSLF_SONIC1	0x04	// Sonic 1 format; the header addresses are big-endian absolute, but jumps are relative
#define SMPSLF_ROM		0x08	// Uncompressed hardcoded ROM offset format for Z80 play-from-ROM; the trickiest to load...
#define SMPSLF_SFX		0x80	// smps sound effect

#define EXT_MUSIC_ROM		"rom"
#define EXT_MUSIC_SAXMAN	"sax"
#define EXT_MUSIC_S1		"smp"
#define EXT_MUSIC_S3K		"s3k"
#define EXT_MUSIC_TREASURE	"trs"
#define EXT_SFX_S1			"sfx"
#define EXT_SFX_S2			"sf2"
#define EXT_SFX_S3K			"sf3"

enum SMPS_TYPE
{
	MUSIC_START = 0x00,
	MUSIC_S1 = MUSIC_START,
	MUSIC_S2_ROM,
	MUSIC_S2_SAXMAN,
	MUSIC_S3K,
	MUSIC_TREASURE,
	SFX_START = 0x80,
	SFX_S1 = SFX_START,
	SFX_S2,
	SFX_S3K
};

// Under Win32, you must explicitly say you are reading BINARY
// files; most other platforms do not have any distinction.
#ifdef WIN32
#	define READFILE	"rb"
#else
#	define READFILE	"r"
#endif

// from sound.c
void SetDACUsage(u8 SampleID);

// from smps.c
void SetLoopBasePos(u8 TrkID, u16 Address);

// Special routines only for Saxman files!
static u8 *SaxmanDec(const char *fname, u16 *totalSize);
static void fix_SMPS_ptrs(u8 *smps, u16 size, u8 SMPS_load_flags);

static u8 LoadedSFX;

u8 *loadSMPS(const char *name)
{
	char* fullname;
	u8 *newSong;
	long size;
	u8 FileType;

	// Hold onto the extension of the file to optionally apply fixes after data is loaded
	// (little endian ONLY) or to do the Saxman exception (Saxman compressed data is actually
	// not useful to anyone in this mode; load from disk and decompress!)
	char ext[4] = { 0 };
	const char *ptrExt;
	
	fullname = (char*)malloc(strlen(name) + 7);
	sprintf(fullname, "music\\%s", name);
	
	ptrExt = strchr(name, '.');
	if(ptrExt != NULL)				// As long as nobody's messing around, this shouldn't fail :)
		strncpy(ext, ptrExt+1, 3);	// Grab the extension!
	
	if (! _stricmp(ext, EXT_MUSIC_S1))
		FileType = MUSIC_S1;
	else if (! _stricmp(ext, EXT_MUSIC_ROM))
		FileType = MUSIC_S2_ROM;
	else if (! _stricmp(ext, EXT_MUSIC_SAXMAN))
		FileType = MUSIC_S2_SAXMAN;
	else if (! _stricmp(ext, EXT_MUSIC_S3K))
		FileType = MUSIC_S3K;
	else if (! _stricmp(ext, EXT_MUSIC_TREASURE))
		FileType = MUSIC_TREASURE;
	else if (! _stricmp(ext, EXT_SFX_S1))
		FileType = SFX_S1;
	else if (! _stricmp(ext, EXT_SFX_S2))
		FileType = SFX_S2;
	else if (! _stricmp(ext, EXT_SFX_S3K))
		FileType = SFX_S3K;
	else
		FileType = MUSIC_S1;	// assume Sonic 1
	
	// As long as this data is ANYTHING BUT Saxman data...
	if (FileType != MUSIC_S2_SAXMAN)
	{
		FILE *f = fopen(fullname, READFILE);
		free(fullname);

		if(f != NULL)
		{
			fseek(f, 0, SEEK_END);
			size = ftell(f);
			fseek(f, 0, SEEK_SET);

			newSong = (u8*)sysmem_alloc(size);
			fread(newSong, size, 1, f);

			fclose(f);
		}
		else
			return NULL;
	}
	else
	{
		// Otherwise, we need to run it through the Saxman decompressor!
		u16 size;
		u8 *saxData = SaxmanDec(fullname, &size);
		free(fullname);

		// If no error occurred...
		DEBUGASSERT(saxData != NULL);
		if(saxData != NULL)
		{
            // Allocate and assign the new resource!
			newSong = (u8*)sysmem_alloc(size);

			// Before copying in the data, there's one more problem we have to take care of --
			// we do not support the $1380 fixed starting address that the Z80 engine is
			// looking for.  So we need to fix all addresses to be absolute-relative to the 
			// start of the memory block...
			fix_SMPS_ptrs(saxData, size, SMPSLF_SAXMAN | SMPSLF_Z80);

			// Copy the decompressed Saxman data into here
			memcpy(newSong, saxData, size);
		}
	}


	if (FileType == MUSIC_S1 || FileType == MUSIC_TREASURE)
		// Special exceptions: Sonic 1 SMPS music imports with big-endian 
		// relative addressing, but I use platform-specific relative-absolute
		// addressing (i.e. relative to the memory block, but an absolute
		// offset within it.)
		fix_SMPS_ptrs(newSong, (u16)size, SMPSLF_SONIC1);
	else if (FileType == MUSIC_S2_ROM || FileType == MUSIC_S3K)
		// Hardcoded ROM offsets targeted for Z80 banks obviously isn't
		// going to work either.  A tricky, but not impossible, problem
		// to solve...
		fix_SMPS_ptrs(newSong, (u16)size, SMPSLF_ROM | SMPSLF_Z80);
	else if (FileType == SFX_S1)
		fix_SMPS_ptrs(newSong, (u16)size, SMPSLF_SFX | SMPSLF_SONIC1);
	else if (FileType == SFX_S2 || FileType == SFX_S3K)
		fix_SMPS_ptrs(newSong, (u16)size, SMPSLF_SFX | SMPSLF_ROM | SMPSLF_Z80);
	
	if (FileType < SFX_START)
		LoadedSFX = 0x00;
	else
		LoadedSFX = 0x01;
	switch(FileType)
	{
	case MUSIC_S1:
	case SFX_S1:
		SetSMPSMode(SMODE_S1);
		break;
	case MUSIC_S2_ROM:
	case MUSIC_S2_SAXMAN:
	case SFX_S2:
		SetSMPSMode(SMODE_S2);
		break;
	case MUSIC_S3K:
	case SFX_S3K:
		SetSMPSMode(SMODE_S3K);
		break;
	case MUSIC_TREASURE:
	default:
		SetSMPSMode(SMODE_INVALID);
		break;
	}
	
	return newSong;
}

void closeSMPS(u8** Song)
{
	free(*Song);
	*Song = NULL;
	
	return;
}

u8 LastLoadedFileMode(void)
{
	return LoadedSFX;
}

// ---------------------------------------------------------------------------
// SaxmanDec (based on the Z80 code from Sonic 2 and a piece of KENS)
//
// Used exclusively for decoding Sonic 2 "Saxman" compressed music into RAM;
// this function is implemented in the Z80 music engine in Sonic 2, so this
// function is not currently part of the "main code."  Given it's really
// limited nature, I don't think it will ever be (or need to be.)  It also
// doesn't belong in a Genesis 68K binary, and neither does this entire source,
// so this seems a fitting place to tuck it away...
//
// This routine will actually get the last few missing bytes that KENS v1.4 rc3 
// always misses, so yay.  :)  (FYI, I think they're missing the last loop!!)
//
// Currently decomps to a fixed buffer and only used when a Saxman file is
// loaded in the resource_disk_get routine (as one exception where the input 
// file data is not directly useful to anyone!!)
//
// Takes an input filename and returns the size of the Saxman file (in the
// totalSize pointer) along with a return of the decompressed memory block;
// will return NULL on failure (either missing file or data getting too big!)
// in which case the "totalSize" return is meaningless.
// ---------------------------------------------------------------------------
#define MAX_SAXMAN		0x800			// The absolute maximum size that Saxman data ought to ever uncompress to (I think it's actually slightly less than this)
static u8 *SaxmanDec(const char *fname, u16 *totalSize)
{
	static u8 out[MAX_SAXMAN];	// A fixed buffer for the decomp process

	FILE *saxFile;
	u16 size;					// Size of song
	u16 outOff = 0;				// Current output offset
	u8 ctlByte = 0;				// Current "control" byte (bitfield of data/flags)

	u8 bitsRead = 8;			// How many bits read so far (8 means get next byte)

	*totalSize = 0;				// Just in case an error occurs...

	saxFile = fopen(fname, "rb");
	if(!saxFile)
		// Couldn't open the file...
		return NULL;

	size  = (fgetc(saxFile));
	size |= (fgetc(saxFile)) << 8;	// Size of compressed song (not including this) are first two bytes

	if(size >= MAX_SAXMAN)
	{
		DEBUGASSERT(size < MAX_SAXMAN);

		// Just the compression data is going too far!  Abort!
		fclose(saxFile);
		return NULL;
	}

	do
	{
		if(outOff >= MAX_SAXMAN)
		{
			DEBUGASSERT(outOff < MAX_SAXMAN);

			// Stop!  Data is getting too large!!
			*totalSize = 0;
			fclose(saxFile);
			return NULL;
		}


		ctlByte >>= 1;						// shift right one (next bit from right-to-left)
		if(bitsRead == 8)					// If we've read 8 bits already, reload!
		{
			ctlByte = fgetc(saxFile);		// Get next control byte
			bitsRead = 1;					// Reset read bits counter
		}
		else
			bitsRead++;


		// Based on bit zero, we either read bytes as-is or copy them from somewhere else...
		if(ctlByte & 1)
		{
			// Non-compression bit; copy byte as-is
			out[outOff++] = fgetc(saxFile);
		}
		else
		{
			u8 lowByte, highByte, count;
			u16 destAddr;

			// Flag field; weird stuff to follow
			// First comes a target address that contains a count...
			lowByte  = (fgetc(saxFile));
			highByte = (fgetc(saxFile));

			// the "highByte" contains the rest of the address in is UPPER 4 bits,
			// while the LOWER 4 bits contains a count (which starts at 3) of bytes
			// to write ... starting 0x12 offset to the address for some reason
			destAddr = ((lowByte | (((u16)(highByte & 0xF0)) << 4)) + 0x12) | (outOff & 0xF000);
			count = (highByte & 0xF) + 3;

			if(destAddr >= outOff)
				destAddr -= 0x1000;

			if (destAddr < outOff)
			{
				int i;

				for (i=0; i<count; i++)
					out[outOff++] = out[destAddr + i];
			}
			else
			{
				int i;

				for (i=0; i<count; i++)
					out[outOff++] = 0;
			}
		}

	} while(ftell(saxFile) <= size+1);

	// We're done!
	fclose(saxFile);

	// This will return the size...
	*totalSize = outOff;

	// This returns a pointer to the block!
	return out;
}


inlinefunc u16 get16(u8 **ptr, int SMPS_load_flags)
{
	u8 byte1 = *((*ptr)++);
	u8 byte2 = *((*ptr)++);
	u16 result;

	if(SMPS_load_flags & SMPSLF_Z80)	
		// If it's from the Z80, expect little endian (byte1 is LSB, byte 2 is MSB)
		result = ((byte1) | (byte2 << 8));

	else
		// If it's from the 68K, expect big endian (byte 1 is MSB, byte 2 is LSB)
		result = ((byte2) | (byte1 << 8));

	return result;
}


static int qsort_compare( const void *arg1, const void *arg2 )
{
   u16 x = *(u16 *)arg1;
   u16 y = *(u16 *)arg2;
   return x - y;
}


#define MAX_DACFM_TRACKS	7	// Maximum number of tracks the $02 byte can specify
#define MAX_PSG_TRACKS		3	// Maximum number of tracks the $03 byte can specify
#define MAX_POINTERS		(MAX_DACFM_TRACKS + MAX_PSG_TRACKS + 1)	// Max pointers we'll find in the header (+1 includes voice pointer)

static void fix_SMPS_ptrs(u8 *smps_start, u16 size, u8 SMPS_load_flags)
{
	// This is a table of the size of each kind of "coordination flag."
	// This list actually also includes codes only valid for Sonic 3
	// music (since I do hope to support that...) that shouldn't appear
	// in Sonic 2 music ... hopefully ...
	// ED is apparently "empty and unused" in either game...

	// NOTES for Sonic Retro:
	// "E2" exists in Sonic 1/2 and does take a parameter (in S2, sets zComRange+6; for timing??)  Explains why Sonic 3 supplies 2F FF probably.
	// "E7" has no parameters!
	// "EE" has NOTHING "to do with voice selection" in Sonic 2; it does abosolutely nothing...
	const u8 cf_parm_size[28] = {
		// E0 E1 E2 E3 E4 E5 E6 E7 E8 E9 EA EB EC ED EE EF F0 F1 F2 F3 F4 F5 F6 F7 F8 F9 FA FB
			1, 1, 1, 0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 0, 0, 1, 4, 0, 0, 1, 0, 1, 2, 4, 2, 0, 0, 1
	};

	u8 *smps = smps_start;			// Make moving pointer
	u8 *smps_mask, *smsk;
	u16 addr_starts[MAX_POINTERS];	// Track and voice starting positions, sorted, for figuring out their ranges
	u16 addr_st_bak[MAX_POINTERS];
	u16 voice_table = 0;			// Address of the voice track (usually ends up as last one after sorting); just need to track because we don't process this one
	u8 cur_track = 0;				// Current track we're on
	u16 base_addr = 0xFFFF;			// 0x0000 (and offsets) in Sonic 1 music, 0x1380 in normal Sonic 2 music, variant in banked music
	u16 header_end_off = 0;			// Offset to end of header

	u8 dacfm_tracks;				// How many DAC+FM tracks
	u8 psg_tracks;					// How many PSG tracks

	//////////////////////////////////////////////////////////////////////
	// PART 1 -- COLLECT ALL ADDRESSES TO FIND BASE ADDRESS
	//////////////////////////////////////////////////////////////////////

	// Okay, first thing to do is run through the header and figure out the 
	// different ranges of addresses of each track and the voice pointer.  
	// This will attempt to figure out the ranges of all of the tracks by
	// sorting the addresses (which then logically deduces the end of each.)

	// Anyway, first comes the voice table pointer...
	voice_table = get16(&smps, SMPS_load_flags);
	addr_starts[cur_track++] = voice_table;

	// Next comes channel setup; we'll total the next two bytes and see how many tracks
	// are to follow.  Note there may never be more than MAX_DACFM_TRACKS + MAX_PSG_TRACKS.
	if (! (SMPS_load_flags & SMPSLF_SFX))
	{
		dacfm_tracks = *smps++;
		psg_tracks = *smps++;
	}
	else
	{
		smps++;
		dacfm_tracks = *smps++;
		psg_tracks = 0;
	}

	DEBUGASSERT(dacfm_tracks <= MAX_DACFM_TRACKS);
	DEBUGASSERT(psg_tracks <= MAX_PSG_TRACKS);

	// Ensure track counts are good
	if( (dacfm_tracks > MAX_DACFM_TRACKS) || (psg_tracks > MAX_PSG_TRACKS) )
		// Encountered a very serious problem!!
		return;

	if (! (SMPS_load_flags & SMPSLF_SFX))
	{
		// Okay, our track counts are good!  Now to get the rest of the pointers...
		smps++;		// Ignore timing divider
		smps++;		// Ignore tempo

		// FM tracks
		for( ; cur_track < (dacfm_tracks+1); cur_track++)	// +1 includes voice pointer
		{
			addr_starts[cur_track] = get16(&smps, SMPS_load_flags);			// Get address, store in pointer list
			addr_st_bak[cur_track - 1] = addr_starts[cur_track];
			smps += 2;														// Ignore pitch/volume adjustment
		}

		// PSG tracks
		for( ; cur_track < (dacfm_tracks+psg_tracks+1); cur_track++)		// +1 includes voice pointer
		{
			addr_starts[cur_track] = get16(&smps, SMPS_load_flags);			// Get address, store in pointer list
			addr_st_bak[cur_track - 1] = addr_starts[cur_track];
			smps += 4;														// Ignore pitch/volume adjustment and instrument
		}
	}
	else
	{
		// SFX tracks
		for( ; cur_track < (dacfm_tracks+1); cur_track++)	// +1 includes voice pointer
		{
			smps += 2;														// Ignore playback control and channel ID
			addr_starts[cur_track] = get16(&smps, SMPS_load_flags);			// Get/fix address, store in pointer list
			addr_st_bak[cur_track - 1] = addr_starts[cur_track];
			smps += 2;														// Ignore pitch/volume adjustment
		}
	}

	// Mark where header has ended; this is already relative!
	header_end_off = (u16)(smps - smps_start);

	if(SMPS_load_flags & SMPSLF_SAXMAN)
		// If Saxman, we KNOW that the base address is 0x1380
		base_addr = 0x1380;

	else if(SMPS_load_flags & SMPSLF_SONIC1)
		// Sonic 1 songs are all relative, though header addresses are
		// absolute, so the base address is effectively zero
		base_addr = 0x0000;

	else if(SMPS_load_flags & SMPSLF_ROM)
	{
		//u8 total_voices = 0;
		//const u8 *end;

		// A song which existed in a ROM bank and therefore is arbitrarily
		// (by this POV) offset in its addressing.  But still using absolute
		// addressing that pertained to its ROM bank.

		// Find the base address by figuring out which address is the smallest.
		// The first addresses we can consider are the header supplied ones.
		// After that, we'll need to consider all jumps within the song.  (Yes,
		// there ARE some songs that have data between the header and the first
		// actual track start... an example is Sonic 2's ARZ.)

		// So go through all addresses the header specifies and try to find the
		// smallest one... note that we will NOT include the voice table address
		// as part of the search, and that furthermore the voice set is REQUIRED
		// to be the largest pointer (voices at the end of the song); this is
		// typically true anyway, but they always could not be.  This will enable
		// us to start just after the header and end at the voice track; processing
		// the voice track obviously makes no sense since it is not note/command/CF.

		// check Instrument Table address (some SFX place it before any data) -VB
		if(addr_starts[0] && addr_starts[cur_track] < base_addr)
			base_addr = addr_starts[0];
		// Doing the header thing... only DAC+FM thru PSG!
		for(cur_track = 1; cur_track < (dacfm_tracks+psg_tracks+1); cur_track++)
		{
			// Verify that the voice table is still further away, otherwise we'll
			// have to abort because this procedure won't work!  (REMEMBER -- we
			// NEED to go through music data to check jump addresses, but if the
			// voice table is in the middle of it, we can't tell we're IN the voice
			// table, because we're not sure where exactly the voice table is!!)
			/*DEBUGASSERT(addr_starts[cur_track] <= voice_table);
			if(addr_starts[cur_track] > voice_table)
				return;*/

			// Keep looking for that smallest address...
			if(addr_starts[cur_track] < base_addr)
				// This address is smaller!
				base_addr = addr_starts[cur_track];
		}


		// Next problem: We don't really know where the voice table begins!
		// We have an arbitrary ROM bank starting address and we don't know
		// the offset yet, but we need to know where the voice table actually
		// begins in relative space!  The best guess I can provide is to take
		// start after the header and find EF (set FM voice) values.  The
		// highest value plus one should tell you how many voices there are...
		// NOT GUARANTEED, but it's worth a shot!!

		/*smps = smps_start + header_end_off;		// Reset pointer
		while(smps < (smps_start + size))
		{
			// Get next byte...
			u8 cmd = *smps++;
			if(cmd >= 0xE0)		// Coordination flag!
			{
				// 0xFB is the highest supported flag... we'll assume if we
				// hit something out of range we're probably in the voice
				// table, so we can quit now.
				if(cmd > 0xFB)
					break;

				smps += cf_parm_size[cmd - 0xE0];	// Must jump additionally by some amount...

				// Checking voice change CFs!!
				if(cmd == 0xEF)
				{
					u8 this_voice;

					smps -= 1;				// Back to voice...
					this_voice = *smps++;	// Get it

					// Is this voice the highest???!
					if(this_voice >= total_voices)
						total_voices = this_voice + 1;
				}
			}
		}

		
		// Each voice is 25 bytes, so back up 25 bytes for each.
		// Now we (hopefully) have an ending pointer!!
		end = smps_start + size - (total_voices * 25);

		// Now go through all data from the header end until the voice table...
		smps = smps_start + header_end_off;		// Reset pointer
		while(smps < end)
		{
			// Get next byte...
			u8 cmd = *smps++;
			if(cmd >= 0xE0)		// Coordination flag!
			{
				DEBUGASSERT(cmd <= 0xFB);
				if(cmd <= 0xFB)		// 0xFB is the highest supported flag...
				{
					smps += cf_parm_size[cmd - 0xE0];	// Must jump additionally by some amount...

					// Exceptions!  Coordination flags which use addresses!
					if( (cmd == 0xF6) ||	// Jump to position (jump address)
						(cmd == 0xF7) ||	// Repeat section of music (loop address)
						(cmd == 0xF8) )		// Jump to position, keep previous position in memory for returning (jump address)
					{
						u16 addr;

						smps -= 2;								// Back up to jump address
						addr = get16(&smps, SMPS_load_flags);	// Get it

						if(addr < base_addr)
							// Set if lowest address!
							base_addr = addr;
					}
				}
			}
		}*/

		// Finally, subtract the header ending offset to get the absolute base address!
		base_addr -= header_end_off;
	}

	// Now tell the SMPS driver the base address for the next song
	SetSMPSOffset(base_addr);
	// If this is a sound effect, we're done.
	if (SMPS_load_flags & SMPSLF_SFX)
		return;
	// If not, we're doing some additional, but actually optional work.

	//////////////////////////////////////////////////////////////////////
	// PART 2 -- FIX ALL HEADER ADDRESSES TO BE ABSOLUTE-RELATIVE
	//////////////////////////////////////////////////////////////////////
	// Note: Originally this fixed the pointers in the actual SMPS file.
	//       But it's more handy so it does just some playback-supporting stuff. -Valley Bell

	// Now we process all of the tracks, except the voice track,
	// to fix their addresses.  Go back to the beginning of the
	// header to fix all of the pointers...
	smps = smps_start;		// Reset pointer
	cur_track = 0;			// Reset track counter

	// Patching voice table pointer first
	voice_table					-= base_addr;	// Correct the voice pointer here...
	addr_starts[cur_track++]	-= base_addr;	// And here!
	smps += 2;

	// Skipping everything to FM this time...
	smps += 4;

	// Patching FM track pointers
	for( ; cur_track < (dacfm_tracks+1); cur_track++)	// +1 includes voice pointer
	{
		addr_starts[cur_track] -= base_addr;		// Fix address
		addr_st_bak[cur_track - 1] -= base_addr;
		smps += 2;
		smps += 2;									// Ignore pitch/volume adjustment
	}

	// Patching PSG track pointers
	for( ; cur_track < (dacfm_tracks+psg_tracks+1); cur_track++)		// +1 includes voice pointer
	{
		addr_starts[cur_track] -= base_addr;		// Fix address
		addr_st_bak[cur_track - 1] -= base_addr;
		smps += 2;
		smps += 4;									// Ignore pitch/volume adjustment and instrument
	}

	//////////////////////////////////////////////////////////////////////
	// PART 3 -- PREPARSE THE SMPS MUSIC DATA
	//////////////////////////////////////////////////////////////////////

	// Note by Valley Bell: The instrument table format is now handled in smps.c,
	//                       because it makes it easier to support SFX.
	
	// We sort the collected addresses so that we know the start/end range 
	// of all areas, most importantly the voice table (which is USUALLY at 
	// the end, but nothing says it HAS to be!)
	qsort((void *)addr_starts, dacfm_tracks+psg_tracks+1, sizeof(u16), qsort_compare);

	// So for all of the pointers we've collected EXCEPT the voice pointer,
	// we process each one and run through its music data, enumerating the
	// jump addresses and DAC sounds.  Remember the window will run from
	// this starting address up to the next starting address or, for the
	// last track, the end of song.
	smps_mask = (u8*)malloc(size);
	memset(smps_mask, 0xFF, size);
	SetDACUsage(0x00);	// Reset DAC Usage
	SetLoopBasePos(0xFF, 0x0000);
	
	for(cur_track = 0; cur_track < (dacfm_tracks+psg_tracks+1); cur_track++)
	{
		const u8 /* *start,*/ *end;
		const u8 IsDAC = (addr_st_bak[0] == addr_starts[cur_track]);

		// Start at this track's initial offset
		//start = &smps_start[addr_starts[cur_track]];
		smps = &smps_start[addr_starts[cur_track]];
		smsk = &smps_mask[addr_starts[cur_track]];

		// End at next track's start, or size of song if this is the last track
		if(cur_track < (dacfm_tracks+psg_tracks))	end = &smps_start[addr_starts[cur_track+1]];
		else										end = &smps_start[size];

		// If this is the voice track, skip it
		if (addr_starts[cur_track] != voice_table)
		{
			// Now go through entire track and enumerate DAC sounds and jumps for
			// VGM logging and advanced playback features.
			while(smps < end)
			{
				u8 cmd = *smps++;	// Get next command byte
				(*smsk ++) = cur_track;
				if(cmd >= 0xE0)		// Coordination flag!
				{
					//DEBUGASSERT(cmd <= 0xFB);
					if(cmd <= 0xFB)		// 0xFB is the highest supported flag...
					{
						smps += cf_parm_size[cmd - 0xE0];	// Must jump additionally by some amount...
						smsk += cf_parm_size[cmd - 0xE0];

						if (cmd == 0xF6)
						{
							u16 addr;
							
							smps -= 2;
							if (SMPS_load_flags & SMPSLF_SONIC1)
								base_addr = (u16)(smps_start - smps) - 1;
							addr = get16(&smps, SMPS_load_flags) - base_addr;
							if (smps_mask[addr] < 0xFF)
							{
								u8 TrkID;
								
								for (TrkID = 0; TrkID < (dacfm_tracks+psg_tracks); TrkID ++)
								{
									if (addr_st_bak[TrkID] == addr_starts[cur_track])
										break;
								}
								if (TrkID >= dacfm_tracks)
									TrkID = TrkID - dacfm_tracks + MAX_DACFM_TRACKS;
								//printf("Track %d: Loop Start at 0x%04X\n", TrkID, addr);
								if (smps_mask[addr] == cur_track)
								{
									SetLoopBasePos(TrkID, addr);
								}
								else
								{
									u8 TrkID2;
									
									for (TrkID2 = 0; TrkID2 < (dacfm_tracks+psg_tracks); TrkID2 ++)
									{
										if (addr_st_bak[TrkID2] == addr_starts[smps_mask[addr]])
											break;
									}
									if (TrkID2 >= dacfm_tracks)
										TrkID2 = TrkID2 - dacfm_tracks + MAX_DACFM_TRACKS;
									
									SetLoopBasePos(0x80 | TrkID, TrkID2);
								}
							}
						}
					}
				}
				else if (IsDAC && cmd >= 0x81)
				{
					// count all DAC sounds (for VGM ripping)
					SetDACUsage(cmd);
				}
			}
		}
	}
	free(smps_mask);

	// Complete!
	/*
	{
		FILE *f = fopen("Boss_Decomp.bin", "wb");
		fwrite(smps_start, 1, size+1, f);
		fclose(f);
	}
	*/
}

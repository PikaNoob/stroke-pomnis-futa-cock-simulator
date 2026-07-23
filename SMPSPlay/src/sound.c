#ifdef _MSC_VER
#pragma warning (disable : 4312)
#endif

//#include <allegro.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include "common.h"

#include <windows.h>
#include "stdtype.h"
#include "Stream.h"

#include "smps.h"
//#include "ym2612.h"
//#include "sn76496.h"
#include "mamedef.h"
#include "2612intf.h"
#include "sn764intf.h"
#include "sound.h"
#include "vgmwrite.h"

#define TRUE	1
#define FALSE	0

// Define this to dump GYM type data (useful for debugging...)
//#define GYM_DUMP
// We don't need gym, we have vgm now! ;) -Valley Bell

#ifdef GYM_DUMP
static FILE *gym;
#endif

// Virtual clock rates for the YM2612 / SN76496
//#define CLOCK_YM2612	7670442
//#define CLOCK_SN76496	3579580
#define CLOCK_YM2612	7670453
#define CLOCK_SN76496	3579545

static const u8 DACDecodeTbl[] = {	0,1,2,4,8,0x10,0x20,0x40,0x80,0xFF,0xFE,0xFC,0xF8,0xF0,0xE0,0xC0 };
	
// Audio stream for YM2612 / SN76496 audio output
//static AUDIOSTREAM	*sndhw_stream;
static u8			is_stereo;
static s16			*ym_buffer[2], *sn_buffer;
static u8			*dac_data;
static u32			rateFMUpdate, rateDACUpdate;		// Different rate divisions; rateFMUpdate is how often YM2612 and SN76496 update, and rateDACUpdate is the DAC
static u32			dac_max = 0;		// Data index into DAC

// DAC Samples
/*static struct _DAC
{
	u8 *sample;
	u16 size;
} DAC[7];*/
typedef struct _DAC_SAMPLE
{
	char* File;
	u8* sample;
	u16 size;
	u8 compr;	// compression - 0 = raw PCM, 1 - jman2050
	u8 UsageID;
} DAC_SAMPLE;
typedef struct _dac_table
{
	u8 Sample;
	u8 Rate;
} DAC_TABLE;

static u8 DACSmplCount;
static DAC_SAMPLE DACSmpls[0x5F];	// 0x5F different samples are maximum
static DAC_TABLE DACMasterPlaylist[0x5F];	// 0x5F = Sounds from 0x81 to 0xDF
static u32 DACBaseRate;
static u32 DACDivider;		// Note: .2 (decimal) Fixed Point, i.e. 123 means 1.23

unsigned int SampleRate;

unsigned int PlayingTimer;
signed int LoopCntr;
signed int WaitCntr;

static volatile u8 IsBusy;

int sound_init()
{
	//int i;
	int init_OK = TRUE;
	u32 rate;
	UINT8 RetVal;

	// FIXME: Is there a way to tell what Allegro is playing at??
	// This code is based on MSP, so I'll have to investigate it later...
    /*rate = get_config_int(NULL, "samprate", 48000);
	if(rate != 48000 && rate != 44100 && rate != 24000 && rate != 22050)
	{
		allegro_message("Error: invalid sample rate %li.\n", rate);
        init_OK = FALSE;
    }*/
	rate = SampleRate = 48000;

	// These were 15 and 30 in MSP, respectively.  But I think
	// it needs to be faster to keep up with SMPS properly.
    rateFMUpdate  = rate / 30;
    rateDACUpdate = rate / 60;


	// FIXME make this a config file or something
	/*if(install_sound(DIGI_AUTODETECT, MIDI_AUTODETECT, NULL) != 0)
	{
		allegro_message("Failed to init sound: %s\n", allegro_error);
		init_OK = FALSE;
	}*/


	// Based on whether to use stereo, begin an auto stream for YM2612/SN76496 audio output
	/*is_stereo = (get_config_int(NULL, "stereo", 1) == 1);
    sndhw_stream = play_audio_stream(rateDACUpdate, 16, is_stereo, rate, 255, 128);
	DEBUGASSERT(sndhw_stream != NULL);
	init_OK &= (sndhw_stream != NULL);*/
	is_stereo = TRUE;


	// Allocate buffers for the audio output of the different devices:
    ym_buffer[0]	= (s16 *)malloc(rateFMUpdate);	// YM2612 LEFT
    ym_buffer[1]	= (s16 *)malloc(rateFMUpdate);	// YM2612 RIGHT
    sn_buffer		= (s16 *)malloc(rateFMUpdate);	// SN76496
    dac_data		= (u8  *)malloc(rateDACUpdate);	// DAC

	DEBUGASSERT(ym_buffer[0] && ym_buffer[1] && sn_buffer && dac_data);
	init_OK &= (ym_buffer[0] && ym_buffer[1] && sn_buffer && dac_data);


	//init_OK &= (YM2612Init(1, CLOCK_YM2612, rate, NULL, NULL) == 0);
	//init_OK &= (SN76496_init(0, CLOCK_SN76496, rate) == 0);
	init_OK &= (device_start_ym2612(0, CLOCK_YM2612, rate) == 0);
	init_OK &= (device_start_sn764xx(0, CLOCK_SN76496, rate, 0x10, 0x09) == 0);

#ifdef GYM_DUMP
	// Start logging GYM...
	gym = fopen("dump.gym", "wb");
#endif

	// Load DAC Samples
	/*for(i=0; i<7; i++)
	{
		long size;
		FILE *DACSample;
		char buffer[256];
		sprintf(buffer, "music\\DAC\\Sample%i.dac", i+1);

		DACSample = fopen(buffer, "rb");

		DEBUGASSERT(DACSample != NULL);

		fseek(DACSample, 0, SEEK_END);
		size = ftell(DACSample);
		fseek(DACSample, 0, SEEK_SET);

		// Allocate
		DAC[i].sample = (u8*)malloc(size);
		DAC[i].size = (u16)(size-1);

		// Read
		fread((void *)DAC[i].sample, size, 1, DACSample);

		fclose(DACSample);
	}*/


	// Perform SMPS player initialization too
	smps_init();

	RetVal = StartStream(0x00);
	if (RetVal)
	{
		printf("Error 0x%02X initialiting Stream!\n", RetVal);
		init_OK = FALSE;
	}

	PlayingTimer = 0;
	LoopCntr = -1;
	WaitCntr = -1;
	IsBusy = 0x00;

	return init_OK;
}


void DumpDACSounds(void)
{
	u8 CurSnd;
	u32 CurSmpl;
	DAC_SAMPLE* TempSmpl;
	const u8* SmplPnt;
	u32 SndLen;
	u8* SndBuffer;
	u8 jman2050_d;
	u8 SmplID;
	
	SmplID = 0x00;
	for (CurSnd = 0; CurSnd < DACSmplCount; CurSnd ++)
	{
		TempSmpl = &DACSmpls[CurSnd];
		if (! TempSmpl->size || DACSmpls[CurSnd].UsageID == 0xFF)
			continue;
		
		if (DACSmpls[CurSnd].UsageID == 0x80)
		{
			DACSmpls[CurSnd].UsageID = SmplID;
			SmplID ++;
		}
		
		if (! TempSmpl->compr)
		{
			vgm_write_large_data(VGMC_YM2612, 0x00, TempSmpl->size, 0, 0, TempSmpl->sample);
		}
		else
		{
			SndLen = TempSmpl->size << 1;
			SmplPnt = TempSmpl->sample;
			SndBuffer = (u8*)malloc(SndLen);
		
			jman2050_d = 0x80;
			for (CurSmpl = 0; CurSmpl < SndLen; CurSmpl ++)
			{
				u8 rawByte = SmplPnt[CurSmpl >> 1];
				u8 nibbleShift = (~CurSmpl & 1) << 2;
				u8 nibbleMask = 0xF << nibbleShift;
				
				jman2050_d += DACDecodeTbl[(rawByte & nibbleMask) >> nibbleShift];
				
				SndBuffer[CurSmpl] = jman2050_d;
			}
		
			vgm_write_large_data(VGMC_YM2612, 0x00, SndLen, 0, 0, SndBuffer);
			free(SndBuffer);
		}
	}
	
	vgm_write_stream_data_command(0x00, 0x00, 0x02002A);
	vgm_write_stream_data_command(0x00, 0x01, 0x00);
	
	WaitCntr = -1;
	
	return;
}

void FillBuffer(WAVE_16BS* Buffer, UINT32 BufferSize)
{
	sound_update((unsigned short*)Buffer, BufferSize);
}

void sound_pause(unsigned char PauseOn)
{
	PauseStream(PauseOn);
}

static void DACUpdate();
void FinishedSongSignal(void);

void sound_update(unsigned short *stream_buf, unsigned int samples)
{
	// RAS: More or less ripped directly from MSP
	u32 loop, loop2;
	//unsigned short *stream_buf;
	s16 *stream_ptr[2];

	IsBusy = 0x01;
	
	// Update SMPS playback
	smps_update();
	vgm_update();
	if (WaitCntr != -1)
	{
		if (WaitCntr == 2*60)
			FinishedSongSignal();
		WaitCntr ++;
	}
	else
	{
		PlayingTimer ++;
	}

#ifdef GYM_DUMP
	// Write GYM no-op
	fputc(0x00, gym);
#endif

	// When get_audio_stream_buffer does NOT return NULL,
	// it needs more data...
	//if( (stream_buf = get_audio_stream_buffer(sndhw_stream)) != NULL )
	{
		// If DAC has data, it needs to be inserted now!
		// This is a fix to enable DAC data to be streamed at a fixed rate
		// (in this case, the rate of the stream, rateDACUpdate) since we
		// don't actually have a "free-running" DAC in this emulation...
		DACUpdate();
		if(dac_max > 0)
		{
			// RAS: Fix this to remove double and replace with fixed!!
			double stream_cnt = 0;
			double update_cycle = rateDACUpdate / dac_max; // dac port write cycle
			for(loop = 0; loop < dac_max; loop++) 
			{
				int old_cnt = (int)stream_cnt;
				int step;                       // update DAC port
				
				//YM2612Write(0, 0, 0x2a);
				//YM2612Write(0, 1, dac_data[loop]);
				ym2612_w(0, 0, 0x2a);
				ym2612_w(0, 1, dac_data[loop]);
				
				if(loop == dac_max-1)
					step = rateDACUpdate - old_cnt;
				else 
				{
					stream_cnt += update_cycle;
					step = (int)stream_cnt - old_cnt;
				}
				
				// update stream 
				stream_ptr[0] = &ym_buffer[0][old_cnt];
				stream_ptr[1] = &ym_buffer[1][old_cnt];
				//YM2612UpdateOne(0, (void **)stream_ptr, step);
				ym2612_stream_update(0, stream_ptr, step);
			}

			dac_max = 0;
		}
		else
			//YM2612UpdateOne(0, (void **)ym_buffer, rateDACUpdate);
			ym2612_stream_update(0, ym_buffer, rateDACUpdate);

		//SN76496Update_16(0, sn_buffer, rateDACUpdate);
		sn764xx_stream_update(0, sn_buffer, rateDACUpdate);

		if (rateDACUpdate < samples)
			samples = rateDACUpdate;
		if(is_stereo) 
		{
			// Update loop for stereo sound
			loop2 = 0;
			for(loop = 0; loop < samples; loop++) 
			{
				// Left/Right interleve
				stream_buf[loop2++] = sn_buffer[loop]/2 + ym_buffer[0][loop];	// Left
				stream_buf[loop2++] = sn_buffer[loop]/2 + ym_buffer[1][loop];	// Right
			}
		}
		else 
		{
			// Update loop for monaural sound 
			for(loop = 0; loop < samples; loop++)
				stream_buf[loop] = sn_buffer[loop]/4 + (ym_buffer[0][loop] + ym_buffer[1][loop]) / 2;
		}
	
		// Free the buffer we just used
		//free_audio_stream_buffer(sndhw_stream);
	}
	
	IsBusy = 0x00;
	
	return;
}



void sound_cleanup()
{
#ifdef GYM_DUMP
	// Close GYM
	fclose(gym);
#endif

	// Destroy the audio stream
	//stop_audio_stream(sndhw_stream);
	StopStream();

	// Cleanup the YM2612 emulator
    //YM2612Shutdown();
	device_stop_ym2612(0);

	// Free the buffers
    free(ym_buffer[0]);
    free(ym_buffer[1]);
    free(sn_buffer);
	free(dac_data);
}


// The following functions are not to be exported,
// but must be linked to smps.c
// Emulation of YM2612/SN76496 writes:
void ym2612_fm0_regdata(u8 reg, u8 data)		// Use to write to 0x[A0]4000 and then 0x[A0]4001
{
	// NOTE -- Do not write DAC data (reg 0x2A) directly with 
	// this function!  Although the YM2612 emulator DOES 
	// eventually accept DAC data, writing with this function 
	// has no "timing" to it, and really it'll be just as if all 
	// DAC bytes were written simultaneously (and thus no sound 
	// is heard); instead, use the DAC functions; they write to 
	// a buffer which streams the bytes at 1/60th of a second 
	// each, providing a max rate of (CLOCK_SN76496 / 60), or
	// about +/- 59660Hz (ideally; in reality you are also limited 
	// by Allegro's stream rate, which defaults at 48000Hz.)

	// Write FM0 data
	//YM2612Write(0, 0, reg);
	//YM2612Write(0, 1, data);
	ym2612_w(0, 0, reg);
	ym2612_w(0, 1, data);

#ifdef GYM_DUMP
	// Write GYM FM0
	fputc(0x01, gym);
	fputc(reg, gym);
	fputc(data, gym);
#endif
	vgm_write(VGMC_YM2612, 0, reg, data);
}


void ym2612_fm1_regdata(u8 reg, u8 data)		// Use to write to 0x[A0]4002 and then 0x[A0]4003
{
	// Write FM1 data
	//YM2612Write(0, 2, reg);
	//YM2612Write(0, 3, data);
	ym2612_w(0, 2, reg);
	ym2612_w(0, 3, data);

#ifdef GYM_DUMP
	// Write GYM FM1
	fputc(0x02, gym);
	fputc(reg, gym);
	fputc(data, gym);
#endif
	vgm_write(VGMC_YM2612, 1, reg, data);
}


void sn76496_write(u8 data)	// Use to write to 0xC00011/0x7F11
{
	//SN76496Write(0, data);
	sn764xx_w(0, 0, data);

#ifdef GYM_DUMP
	// Write GYM PSG
	fputc(0x03, gym);
	fputc(data, gym);
#endif
	vgm_write(VGMC_SN76496, 0, data, 0);
}


// FIXME: Should probably make this dynamic somehow
/*const u8 DACMasterPlaylist[][2] = {
	{ 0x81, 0x17},
	{ 0x82, 0x1 },
	{ 0x83, 0x6 },
	{ 0x84, 0x8 },
	{ 0x85, 0x1B },
	{ 0x86, 0x0A},
	{ 0x87, 0x1B },
	{ 0x85, 0x12 },
	{ 0x85, 0x15 },
	{ 0x85, 0x1C },
	{ 0x85, 0x1D },
	{ 0x86, 0x2 },
	{ 0x86, 0x5 },
	{ 0x86, 0x8 },
	{ 0x87, 0x8 },
	{ 0x87, 0x0B },
	{ 0x87, 0x12 },
};*/

// DAC control variables
static struct _DAC_state
{
	const u8	*curDAC;		// Currently playing DAC (NULL for none)
	u32			curDACPos;		// DAC pos, in NIBBLES 16.16 FP
	u32			curDACPosDelta;	// 16.16FP step value for above, specifies number of NIBBLES per frame
	u16			curDACLen;		// Length, in NIBBLES, of sample
	u8			jman2050_d;		// The jman2050 magic "d" value!  (Last decoded byte)
	u8			nibbleSide;		// Which "side" of the nibbles are we on?  ("Left", or upper 4 bits, first, then "Right", or lower 4 bits)
								// Bit 7 (80) = compressed-flag
} DAC_state = { NULL };


static u32 CalcDACFreq(u16 ratePrim)
{
	u32 Dividend;
	
	Dividend = DACDivider + ratePrim * 100;
	return (DACBaseRate * 100 + Dividend / 2) / Dividend;
}

static u32 CalcDACFreq24_8(u16 ratePrim)
{
	u32 Dividend;
	
	// Calculating with *100 causes an integer overflow
	// with even the default values, so I use 32
	Dividend = (DACDivider * 32 / 100) + ratePrim * 32;
	return ((DACBaseRate << 8) * 32 + Dividend / 2) / Dividend;
}

// These start a new DAC sound, and update loops pump in DAC data
void DACPlay(u8 note)
{
	// New DAC sound request!
	if(note >= 0x81)	// 0x80 is a "rest", which doesn't quite make sense on DAC
	{
		note -= 0x81;

		//if(note < sizeof(DACMasterPlaylist)/2)
		if (DACMasterPlaylist[note].Sample != 0xFF)
		{
			//u8 drum		= DACMasterPlaylist[note][0] - 0x81;	// Get the DAC sound to use
			//u8 ratePrim	= DACMasterPlaylist[note][1];			// Get the DAC sound rate "primitive"
			
			u8 drum		= DACMasterPlaylist[note].Sample;		// Get the DAC sound to use
			u16 ratePrim= DACMasterPlaylist[note].Rate;			// Get the DAC sound rate "primitive"
			if (! ratePrim)
				ratePrim |= 0x100;

			// Assign current DAC sample
			DAC_state.curDAC = DACSmpls[drum].sample;

			// Set the length (in nibbles)
			if (! DACSmpls[drum].compr)
				DAC_state.curDACLen = DACSmpls[drum].size;
			else
				DAC_state.curDACLen = DACSmpls[drum].size << 1;

			// Starting from the beginning
			DAC_state.curDACPos = 0;

			// The magic jman2050 'd' value!
			DAC_state.jman2050_d = 0x80;

			// Force update on first loop
			if (! DACSmpls[drum].compr)
				DAC_state.nibbleSide = 0x80;
			else
				DAC_state.nibbleSide = 1;

			// RAS: My guess rate calculation from the "primitive" value to
			// normal PCM speed goes: CLOCK_SN76496 / (60Hz + (prim * 4)) / 2
			// That would specify the speed in Hz, or bytes per second.  Since
			// I'm working in NIBBLES/sec, we'll leave off the divide by 2.
			// Also, we don't update an entire SECOND, only a FRAME of audio
			// data!  So we need to divide by 60.  But there's the caveat of
			// the system's mixer speed... watch and see!
			
			// FreqHz = DACBaseRate / (DACDivider/100.0 + ratePrim)
			if (! (DACSmpls[drum].UsageID & 0x80))
			{
				vgm_write_stream_data_command(0x00, 0x02, CalcDACFreq(ratePrim));
				vgm_write_stream_data_command(0x00, 0x05, DACSmpls[drum].UsageID);
			}

			// This produces NIBBLES/sec in 24.8FP (not enough room in 32-bit
			// to shift the clock value, and I'm trying to stay off 64-bit
			// integers at this time!)
			DAC_state.curDACPosDelta = CalcDACFreq24_8(ratePrim);

			// But now the rate cannot be higher than ~59660 Nib/sec (as
			// an integer) so it's safe to get back those 8-bits of precision
			// once again, reforming this into a 16.16FP value
			DAC_state.curDACPosDelta <<= 8;

			// Now finally divide by 60 to get Nibbles/frame
			DAC_state.curDACPosDelta /= 60;

			// Final piece of the puzzle: Technically, we have a rate that
			// we would write to the DAC, but in reality, we are constrained
			// to the rate of the system mixer.  So we need to adjust our
			// current play value to be relative to one frame of this mixer.

			// rateDACUpdate specifies the number of bytes written to the
			// audio stream in a single frame, which we can easily turn into
			// a nibble count and compare against the number of nibbles we
			// want to be writing per frame.

			// So with this final step, we have nibbles per byte of the
			// DAC sound buffer... which we can use to loop across the
			// entire DAC update.
			DAC_state.curDACPosDelta = DAC_state.curDACPosDelta / rateDACUpdate;
		}
#ifdef _DEBUG
		else
			printf("Warning! Trying to play unused DAC sound %02X\n", 0x81 + note);
#endif
	}
}

void DACStop(void)
{
	DAC_state.curDAC = NULL;
	vgm_write_stream_data_command(0x00, 0x04, 0x00);
	
	return;
}


static void DACUpdate()
{
	// Stream out DAC data at appropriate data rate and fill up the buffer, if you have enough!
	//const u8 DACDecodeTbl[] = {	0,1,2,4,8,0x10,0x20,0x40,0x80,0xFF,0xFE,0xFC,0xF8,0xF0,0xE0,0xC0 };

	if(DAC_state.curDAC != NULL)
	{
		// The players:
		// rateDACUpdate	- specifies the number of bytes we need to cover this frame
		// curDACPos		- current nibble position within the DAC sample (16.16FP)
		// curDACPosDelta	- lets us know the fractional amount of nibbles we move across for each byte of DAC update. (16.16FP)
		// curDACLen		- total length, in nibbles, of DAC sample
		
		while(dac_max < rateDACUpdate)
		{
			u16 curDACPosInt = (DAC_state.curDACPos >> 16);		// Integer (whole part) of our current position
			
			// If we hit the end of the sample, stop!!
			if(curDACPosInt >= DAC_state.curDACLen)
			{
				// Stop playing
				DAC_state.curDAC = NULL;
				break;
			}
			
			if (! (DAC_state.nibbleSide & 0x80))
			{
				// Check if we've advanced to the next nibble or not -- every odd/even
				// change in our position indicates that we've gone to the next nibble
				// and thus need to decode it!
				if( (curDACPosInt & 1) != DAC_state.nibbleSide )
				{
					u8 rawByte = DAC_state.curDAC[curDACPosInt >> 1];	// Divide nibble position by 2 to get byte offset into the DAC sample
					u8 nibbleShift;
					u8 nibbleMask;
					
					// Update our "nibble side"
					DAC_state.nibbleSide = curDACPosInt & 1;
					
					// Proper shift amount for this nibble
					nibbleShift = (DAC_state.nibbleSide ^ 1) << 2;
					
					// Proper mask for this nibble
					nibbleMask = 0xF << nibbleShift;	// Get upper or lower 4 bits of next byte (upper first, then lower, opposite of what our nibble position suggests)
					
					// Decode next byte!
					DAC_state.jman2050_d += DACDecodeTbl[(rawByte & nibbleMask) >> nibbleShift];
				}
			}
			else
			{
				DAC_state.jman2050_d = DAC_state.curDAC[curDACPosInt];
			}
			
			// Update nibble position...
			DAC_state.curDACPos += DAC_state.curDACPosDelta;
			
			// Write DAC data until we run out of update room!
			dac_data[dac_max++] = DAC_state.jman2050_d;
		}
	}
	else
		// If not playing DAC sample, flatline DAC
		dac_data[dac_max++] = 0x80;
};


// called when the last SMPS channel received an F2 flag
void StopSignal(void)
{
	vgm_dump_stop();
	LoopCntr = -1;
	WaitCntr = 0;
	
	return;
}

void LoopStartSignal(void)
{
	vgm_set_loop();
	LoopCntr = 1;
	
	return;
}

void LoopEndSignal(void)
{
	vgm_dump_stop();
	if (LoopCntr >= 2)
		FinishedSongSignal();
	LoopCntr ++;
	
	return;
}

void StartSignal(void)
{
	PlayingTimer = 0;
	LoopCntr = 0;
	WaitCntr = -1;
	
	return;
}


void LoadDACSample(const u8 DACSnd, const char* FileName, const u8 Compr, const u8 Rate);

void LoadDACData(const char* FileName)
{
	FILE* hFile;
	char BasePath[0x100];
	char TempStr[0x100];
	size_t TempInt;
	char* TempPnt;
	u8 IniSection;
	
	char DACFile[0x100];
	u8 DACCompr;
	u8 DACRate;
	
	DACSmplCount = 0x00;
	memset(DACSmpls, 0x00, sizeof(DACSmpls));
	memset(DACMasterPlaylist, 0x00, sizeof(DACMasterPlaylist));
	for (IniSection = 0x00; IniSection < 0x5F; IniSection ++)
		DACMasterPlaylist[IniSection].Sample = 0xFF;
	DACBaseRate = 272624;
	DACDivider = 796;
	
	
	strcpy(BasePath, FileName);
	TempPnt = strrchr(BasePath, '\\');
	if (BasePath != NULL)
	{
		TempPnt ++;
		*TempPnt = '\0';
	}
	
	hFile = fopen(FileName, "rt");
	if (hFile == NULL)
	{
		printf("Error opening %s\n", FileName);
		return;
	}
	
	IniSection = 0x01;
	while(! feof(hFile))
	{
		TempPnt = fgets(TempStr, 0x100, hFile);
		if (TempPnt == NULL)
			break;
		if (TempStr[0x00] == '\n' || TempStr[0x00] == '\0')
			continue;
		if (TempStr[0x00] == ';')
		{
			// skip comment lines
			// fgets has set a null-terminator at char 0xFF
			while(TempStr[strlen(TempStr) - 1] != '\n')
			{
				fgets(TempStr, 0x100, hFile);
				if (TempStr[0x00] == '\0')
					break;
			}
			continue;
		}
		
		if (TempStr[0x00] == '[')
		{
			TempPnt = strchr(TempStr, ']');
			if (TempPnt == NULL)
				continue;
			*TempPnt = '\0';
			
			if (IniSection & 0x80)
				LoadDACSample(IniSection, DACFile, DACCompr, DACRate);
			
			IniSection = (u8)strtoul(TempStr + 1, NULL, 0x10);
			if (IniSection >= 0x81 && IniSection <= 0xDF)
			{
				strcpy(DACFile, "");
				DACCompr = 0x00;
				DACRate = 0x00;
			}
			else
			{
				IniSection = 0x00;
			}
			continue;
		}
		
		if (! IniSection)	// ignore all invalid sections
			continue;
		
		TempPnt = strchr(TempStr, '=');
		if (TempPnt == NULL || TempPnt == TempStr)
			continue;	// invalid line
		
		TempInt = strlen(TempPnt) - 1;
		if (TempPnt[TempInt] == '\n')
			TempPnt[TempInt] = '\0';
		
		*TempPnt = '\0';
		TempPnt ++;
		while(*TempPnt == ' ')
			TempPnt ++;
		
		TempInt = strlen(TempStr) - 1;
		while(TempInt > 0 && TempStr[TempInt] == ' ')
		{
			TempStr[TempInt] = '\0';
			TempInt --;
		}
		
		if (IniSection == 0x01)
		{
			if (! _stricmp(TempStr, "BaseRate"))
			{
				DACBaseRate = (u32)strtoul(TempPnt, NULL, 0);
			}
			else if (! _stricmp(TempStr, "RateDiv"))
			{
				DACDivider = (u32)(strtod(TempPnt, NULL) * 100 + 0.5);
			}
		}
		else
		{
			if (! _stricmp(TempStr, "File"))
			{
				strcpy(DACFile, BasePath);
				strcat(DACFile, TempPnt);
			}
			else if (! _stricmp(TempStr, "Compr"))
			{
				if (! _stricmp(TempPnt, "True"))
					DACCompr = 0x01;
				else if (! _stricmp(TempPnt, "False"))
					DACCompr = 0x00;
				else
					DACCompr = strtol(TempPnt, NULL, 0) ? 0x01 : 0x00;
			}
			else if (! _stricmp(TempStr, "Rate"))
			{
				DACRate = (u8)strtol(TempPnt, NULL, 0);
			}
		}
	}
	if (IniSection & 0x80)
		LoadDACSample(IniSection, DACFile, DACCompr, DACRate);
	
	fclose(hFile);
	
	return;
}

void LoadDACSample(const u8 DACSnd, const char* FileName, const u8 Compr, const u8 Rate)
{
	u8 CurSmpl;
	DAC_SAMPLE* TempSmpl;
	DAC_TABLE* TempTbl;
	size_t TempInt;
	FILE* hFile;
	
	if (*FileName == '\0')
		return;
	if (DACSnd < 0x81 || DACSnd > 0xDF)
		return;
	
	TempTbl = &DACMasterPlaylist[DACSnd - 0x81];
	TempTbl->Rate = Rate;
	
	if (TempTbl->Sample != 0xFF)
	{
		TempSmpl = &DACSmpls[TempTbl->Sample];
	}
	else
	{
		for (CurSmpl = 0x00; CurSmpl < DACSmplCount; CurSmpl ++)
		{
			if (! _stricmp(FileName, DACSmpls[CurSmpl].File))
				break;
		}
		TempSmpl = &DACSmpls[CurSmpl];
		if (CurSmpl >= DACSmplCount)
			DACSmplCount ++;
		
		TempTbl->Sample = CurSmpl;
	}
	
	if (TempSmpl->File != NULL && ! _stricmp(FileName, TempSmpl->File))
		return;	// already loaded
	
	if (TempSmpl->File != NULL)
		free(TempSmpl->File);
	TempInt = strlen(FileName) + 1;
	TempSmpl->File = (char*)malloc(TempInt * sizeof(char));
	strcpy(TempSmpl->File, FileName);
	TempSmpl->compr = Compr;
	
	hFile = fopen(TempSmpl->File, "rb");
	if (hFile == NULL)
	{
		printf("Error opening %s\n", TempSmpl->File);
		
		free(TempSmpl->File);	TempSmpl->File = NULL;
		TempTbl->Sample = 0xFF;
		
		if (CurSmpl == (DACSmplCount - 1))
			DACSmplCount --;
		return;
	}
	
	fseek(hFile, 0x00, SEEK_END);
	TempInt = ftell(hFile);
	if (TempInt > 0xFFFF)
		TempInt = 0xFFFF;	// limit to 0xFFFF because of 16.16 FP calculations
	TempSmpl->size = (u16)TempInt;
	
	fseek(hFile, 0x00, SEEK_SET);
	TempSmpl->sample = (u8*)malloc(TempSmpl->size);
	fread(TempSmpl->sample, 0x01, TempSmpl->size, hFile);
	
	fclose(hFile);
	
	return;
}

void FreeDACData(void)
{
	u8 CurSmpl;
	DAC_SAMPLE* TempSmpl;
	
	while(IsBusy)
		Sleep(1);
	DAC_state.curDAC = NULL;
	
	for (CurSmpl = 0x00; CurSmpl < DACSmplCount; CurSmpl ++)
	{
		TempSmpl = &DACSmpls[CurSmpl];
		free(TempSmpl->File);	TempSmpl->File = NULL;
		TempSmpl->size = 0;
		free(TempSmpl->sample);	TempSmpl->sample = NULL;
	}
	DACSmplCount = 0x00;
	
	return;
}

void SetDACUsage(u8 SampleID)
{
	u8 CurSmpl;
	
	if (! SampleID)
	{
		for (CurSmpl = 0x00; CurSmpl < DACSmplCount; CurSmpl ++)
			DACSmpls[CurSmpl].UsageID = 0xFF;
	}
	else
	{
		CurSmpl = DACMasterPlaylist[SampleID - 0x81].Sample;
		if (CurSmpl != 0xFF)
			DACSmpls[CurSmpl].UsageID = 0x80;
	}
	
	return;
}

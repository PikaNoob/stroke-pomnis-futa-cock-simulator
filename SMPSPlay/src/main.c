#ifdef _MSC_VER
#pragma warning (disable : 4312)
#endif

//#include <allegro.h>
#include <stdlib.h>
#include <ctype.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <conio.h>
#include <windows.h>
#include "common.h"
#include "dirent.h"
#include "smps.h"
#include "sound.h"
#include "vgmwrite.h"
#include "stdbool.h"

//////////////////////////////////////////////////
// The following are used for framerate control:
//////////////////////////////////////////////////
/*static volatile unsigned int frames, fps;
static void second_counter(void)
{
	fps = frames;
	frames = 0;
} //END_OF_FUNCTION(second_counter);

static volatile unsigned int ticks;
static void ticker(void)
{
	ticks++;
} //END_OF_FUNCTION(ticker);*/

//////////////////////////////////////////////////

// from loader.c
u8 *loadSMPS(const char *name);
void closeSMPS(u8** Song);
u8 LastLoadedFileMode(void);


#define MAX_FILES	256
static u32 smps_count = 0;
static u32 cursor = 0;
//static RGB white = { 63, 63, 63 };
//static RGB yellow = { 63, 63, 0 };
static char files[MAX_FILES][256];
static char IniPath[0x02][256];
static int smps_playing;
static bool GoToNextSong;

extern bool PauseThread;
bool PauseMode;
static bool AutoProgress;

extern unsigned int PlayingTimer;
extern signed int LoopCntr;

static void get_file_list()
{
	DIR *dir;
	struct dirent *d;
	//int i;
	struct stat statbuf;
	char tempfile[256];

	//for(i=1; i<255; i++)
	//	set_color(i, &white);

	dir = opendir("music");

	while( (d = readdir(dir)) != NULL )
	{
		if (smps_count >= MAX_FILES)
		{
			printf("Too many files! Stopped reading directory!\n");
			break;
		}
		
		sprintf(tempfile, "music\\%s", d->d_name);
		strcpy(files[smps_count], d->d_name);

		if(stat(tempfile, &statbuf) != -1)
		{
			if(!(statbuf.st_mode & _S_IFDIR))
			{
				smps_count++;
				printf("%2u %.75s\n", smps_count, d->d_name);
			}
		}
	}

	closedir(dir);
}


void LoadINIFile(const char* INIFileName)
{
	FILE* hFile;
	char BasePath[0x100];
	char TempStr[0x100];
	size_t TempInt;
	char* TempPnt;
	
	strcpy(IniPath[0x00], "");
	strcpy(IniPath[0x01], "");
	
	strcpy(BasePath, INIFileName);
	TempPnt = strrchr(BasePath, '\\');
	if (BasePath != NULL)
	{
		TempPnt ++;
		*TempPnt = '\0';
	}
	
	hFile = fopen(INIFileName, "rt");
	if (hFile == NULL)
	{
		printf("Error opening %s\n", INIFileName);
		return;
	}
	
	while(! feof(hFile))
	{
		TempPnt = fgets(TempStr, 0x40, hFile);
		if (TempPnt == NULL)
			break;
		if (TempStr[0x00] == '\n' || TempStr[0x00] == '\0')
			continue;
		if (TempStr[0x00] == ';')
		{
			// skip comment lines
			// fgets has set a null-terminator at char 0x3F
			while(TempStr[strlen(TempStr) - 1] != '\n')
			{
				fgets(TempStr, 0x40, hFile);
				if (TempStr[0x00] == '\0')
					break;
			}
			continue;
		}
		
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
		
		if (! _stricmp(TempStr, "PSGFlt"))
		{
			strcpy(IniPath[0x00], BasePath);
			strcat(IniPath[0x00], TempPnt);
		}
		else if (! _stricmp(TempStr, "DACSnd"))
		{
			strcpy(IniPath[0x01], BasePath);
			strcat(IniPath[0x01], TempPnt);
		}
	}
	
	fclose(hFile);
	
	return;
}

/*void DisplayLine(const char* Text, ...)
{
	va_list args;
	static char TempText[0x50];
	
	va_start(args, Text);
	vsnprintf(TempText, 0x4D, Text, args);
	TempText[0x4D] = '\0';
	strcat("\r");
	printf("%s", TempText);
	
	return;
}*/

void ClearLine(void)
{
	printf("%78s", "\r");
	return;
}

void ReDisplayFileID(int FileID);

void DisplayFileID(int FileID)
{
	ClearLine();
	
	ReDisplayFileID(FileID);
	
	return;
}

void ReDisplayFileID(int FileID)
{
	char TempBuf[0x20];	// maximum is 0x18 chars (for 2 loop digits) + \0
	char* TempPnt;
	
	TempBuf[0x00] = '\0';
	if (FileID == smps_playing)
	{
		u16 Min;
		u8 Sec;
		u8 Frame;
		
		TempPnt = TempBuf;
		strcpy(TempPnt, " (");	TempPnt += 0x02;
		
		Frame = PlayingTimer % 60;
		Sec = (PlayingTimer / 60) % 60;
		Min = PlayingTimer / 3600;
		TempPnt += sprintf(TempPnt, "%02u:%02u.%02u", Min, Sec, (Frame * 100 + 30) / 60);
		if (LoopCntr == -1)
		{
			TempPnt += sprintf(TempPnt, " %s", "finished");
		}
		else
		{
			TempPnt += sprintf(TempPnt, " %s", PauseMode ? "paused" : "playing");
			if (LoopCntr > 0)
				TempPnt += sprintf(TempPnt, " L %d", LoopCntr);
		}
		strcpy(TempPnt, ")");	TempPnt += 0x01;
	}
	// The console under Windows displays 80 chars in a line.
	// I calc with 78 chars per line and 4 chars for the track number.
	// So I limit the file name length to: 78 - 4 - Len(time + "playing" + etc.)
	printf("%d %.*s%s\r", FileID + 1, 74 - strlen(TempBuf), files[FileID], TempBuf);
	
	return;
}

static void WaitTimeForKey(unsigned int MSec)
{
	DWORD CurTime;
	
	CurTime = timeGetTime() + MSec;
	while(timeGetTime() < CurTime)
	{
		Sleep(20);
		if (_kbhit())
			break;
	}
	
	return;
}

static void WaitForKey(void)
{
	while(! GoToNextSong && ! PauseMode)
	{
		Sleep(20);
		if ((s32)cursor == smps_playing)
			ReDisplayFileID(cursor);
		if (_kbhit())
			break;
	}
	
	return;
}

void FinishedSongSignal(void)
{
	if (AutoProgress && (u32)smps_playing < smps_count - 1)
		GoToNextSong = true;
	
	return;
}

int main(int argc, char *argv[])
{
	/*if(allegro_init() == 0)
	{
		if(set_gfx_mode(GFX_DIRECTX_WIN, 640, 480, 0, 0) != 0)
			allegro_message("Failed to init graphics: %s\n", allegro_error);
		else
		{
			set_color_depth(8);

			get_file_list();

			install_timer();
			install_keyboard();

			install_int_ex(second_counter, BPS_TO_TIMER(1));
			install_int_ex(ticker, BPS_TO_TIMER(60));

			if(sound_init())
			{
				u8 *smps = NULL;

				set_color(cursor+1, &yellow);

				while(!key[KEY_ESC])
				{
					sound_update();

					if(keypressed())
					{
						switch(readkey() >> 8)
						{
						case KEY_UP:
							if(cursor > 0)
							{
								set_color(cursor+1, &white);
								set_color(cursor, &yellow);
								cursor--;
							}
							break;

						case KEY_DOWN:
							if(cursor < smps_count-1)
							{
								cursor++;
								set_color(cursor, &white);
								set_color(cursor+1, &yellow);
							}
							break;

						case KEY_ENTER:
							if(smps != NULL)
								sysmem_free(smps);

							smps = loadSMPS(files[cursor]);
							smps_playSong(smps, 0);

							break;
						}
					}

					// control timing
					while(ticks<1)
						rest(0);
					
					ticks=0;
					frames++;
				}

				sound_cleanup();
			}
			else
				allegro_message("Failed to init sound: %s\n", allegro_error);
		}
	}
	else
		allegro_message("Failed to init: %s\n", allegro_error);*/
	
	Enable_VGMDumping = 0x00;
	if (argc >= 2)
	{
		if (argv[0x01][0x00] == '-')
		{
			if (toupper(argv[0x01][0x01] == 'V'))
				Enable_VGMDumping = 0x01;
		}
	}
	
	printf("C-based SMPS Engine Demo\n");
	printf("by Robert Andrew Showalter (AKA Epoch Man, Rob Jinnai, RobS)\n");
	printf("Improved by Valley Bell\n");

	LoadINIFile("data\\config.ini");
	LoadFlutterData(IniPath[0x00]);
	LoadDACData(IniPath[0x01]);
	
	get_file_list();
	
	if (smps_count && sound_init())
	{
		u8 *smps = NULL;
		u8 *sfx[8];
		u8 sfxid = 0;
		u8 *loaded;
		int inkey;
		
		memset(sfx, 0x00, sizeof(u8*) * 8);
		
		AutoProgress = false;
		PauseMode = false;
		GoToNextSong = false;
		smps_playing = -1;
		DisplayFileID(cursor);
		
		inkey = 0x00;
		while(inkey != 0x1B)
		{
			switch(inkey)
			{
			case 0xE0:
				inkey = _getch();
				switch(inkey)
				{
				case 0x48:	// Cursor Up
					if (cursor > 0)
					{
						cursor--;
						DisplayFileID(cursor);
					}
					break;
				case 0x50:	// Cursor Down
					if (cursor < smps_count-1)
					{
						cursor ++;
						DisplayFileID(cursor);
					}
					break;
				}
				break;
			case 'n':
				if (cursor >= smps_count - 1)
					break;
				
				cursor ++;
				// fall through
			case 0x0D:
				PauseThread = true;
				
				loaded = loadSMPS(files[cursor]);
				if (loaded != NULL)
				{
					if (! LastLoadedFileMode())
					{
						if (smps != NULL)
							closeSMPS(&smps);
						vgm_dump_stop();
						
						MakeVgmFileName(files[cursor]);
						vgm_dump_start();
						DumpDACSounds();
						
						smps_playing = cursor;
						smps = loaded;
						smps_playSong(smps, 0);
					}
					else
					{
						if (sfx[sfxid] != NULL)
							closeSMPS(&sfx[sfxid]);
						
						sfx[sfxid] = loaded;
						smps_playSFX(sfx[sfxid], 0);
						sfxid ++;	sfxid &= 7;
					}
				}
				
				PauseMode = false;
				sound_pause(PauseMode);
				DisplayFileID(cursor);	// erase line and redraw text
				if (loaded == NULL)
				{
					ClearLine();
					printf("Error opening %s.\r", files[cursor]);
					WaitForKey();
				}
				
				break;
			case 'V':
				Enable_VGMDumping = ! Enable_VGMDumping;
				ClearLine();
				printf("VGM Logging %s.\r", Enable_VGMDumping ? "enabled" : "disabled");
				WaitTimeForKey(1000);
				DisplayFileID(cursor);
				break;
			case 'P':
			case ' ':
				PauseMode = ! PauseMode;
				sound_pause(PauseMode);
				DisplayFileID(cursor);
				break;
			case 'A':
				AutoProgress = ! AutoProgress;
				ClearLine();
				printf("Automatic Progressing %s.\r", AutoProgress ? "enabled" : "disabled");
				WaitTimeForKey(1000);
				DisplayFileID(cursor);
				break;
			case 'R':
				PauseThread = true;
				FreeDACData();
				FreeFlutterData();
				
				LoadINIFile("data\\config.ini");
				LoadFlutterData(IniPath[0x00]);
				LoadDACData(IniPath[0x01]);
				PauseThread = false;
				
				ClearLine();
				printf("Data reloaded.\r");
				WaitTimeForKey(1000);
				DisplayFileID(cursor);
				break;
			}
			
			WaitForKey();
			if (GoToNextSong)
			{
				GoToNextSong = false;
				inkey = 'n';
			}
			else
			{
				inkey = toupper(_getch());
			}
		}
		
		sound_cleanup();
		
		for (sfxid = 0; sfxid < 8; sfxid ++)
		{
			if (sfx != NULL)
				closeSMPS(&sfx[sfxid]);
		}
		if (smps != NULL)
			closeSMPS(&smps);
		vgm_dump_stop();
	}
	
	FreeFlutterData();
	FreeDACData();
	
	ClearLine();
	printf("Quit.\n");
#ifdef _DEBUG
	_getch();
#endif
	
	return 0;
}

void RedrawStatusLine(void)
{
	DisplayFileID(cursor);
	
	return;
}

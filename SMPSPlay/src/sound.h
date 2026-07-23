// PLEASE NOTE: This header should ALWAYS remain system independent,
// (using #if/#else blocks is OK, as long as it'll compile unmodified
// on all target platforms), though sound.c is platform specific; 
// please try to keep this order as much as possible!!
#ifndef _S2RAS_SOUND_H
#define _S2RAS_SOUND_H

// Initialization function (do not export these)
int sound_init();
//void sound_update();
void sound_pause(unsigned char PauseOn);
void sound_update(unsigned short *stream_buf, unsigned int samples);
void sound_cleanup();

void LoadDACData(const char* FileName);
void FreeDACData(void);

void DumpDACSounds(void);

#endif

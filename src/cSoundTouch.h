#ifndef CSOUNDTOUCH_H
#define CSOUNDTOUCH_H
#include"config.h"
#if AC_MODULE_MEDIA
#ifdef __cplusplus
extern "C" {
#endif
	typedef void* cSoundTouch;
	const char* cSoundTouch_getVersionString();
	int cSoundTouch_getVersionId();
	cSoundTouch cSoundTouch_create();
	void cSoundTouch_destroy(cSoundTouch soundTouch);
	void cSoundTouch_setRate(cSoundTouch soundTouch, double newRate);
	void cSoundTouch_setTempo(cSoundTouch soundTouch, double newTempo);
	void cSoundTouch_setRateChange(cSoundTouch soundTouch, double newRate);
	void cSoundTouch_setTempoChange(cSoundTouch soundTouch, double newTempo);
	void cSoundTouch_setPitch(cSoundTouch soundTouch, double newPitch);
	void cSoundTouch_setPitchOctaves(cSoundTouch soundTouch, double newPitch);
	void cSoundTouch_setPitchSemiTones(cSoundTouch soundTouch, int newPitch);
	void cSoundTouch_setChannels(cSoundTouch soundTouch, int numChannels);
	void cSoundTouch_setSampleRate(cSoundTouch soundTouch, int srate);
	double cSoundTouch_getInputOutputSampleRatio(cSoundTouch soundTouch);
	void cSoundTouch_flush(cSoundTouch soundTouch);
	void cSoundTouch_putSamples(cSoundTouch soundTouch, float* samples, int numSamples);
	void cSoundTouch_putSamples_i16(cSoundTouch soundTouch, short* samples, int numSamples);
	int cSoundTouch_receiveSamples(cSoundTouch soundTouch, float* output, int maxSamples);
	int cSoundTouch_receiveSamples_i16(cSoundTouch soundTouch, short* output, int maxSamples);
	void cSoundTouch_clear(cSoundTouch soundTouch);
	int cSoundTouch_setSetting(cSoundTouch soundTouch, int settingId, int value);
	int cSoundTouch_getSetting(cSoundTouch soundTouch, int settingId);
	int cSoundTouch_numUnprocessedSamples(cSoundTouch soundTouch);
	int cSoundTouch_numChannels(cSoundTouch soundTouch);
	int cSoundTouch_numSamples(cSoundTouch soundTouch);
#ifdef __cplusplus
};
#endif
#endif
#endif
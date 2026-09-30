#include "cSoundTouch.h"

#include"soundtouch/SoundTouch.h"

#ifdef __cplusplus
extern "C" {
#endif
const char* cSoundTouch_getVersionString()
{

	return soundtouch::SoundTouch::getVersionString();
}
int cSoundTouch_getVersionId() {
	return soundtouch::SoundTouch::getVersionId();
}
cSoundTouch cSoundTouch_create()
{
	return new soundtouch::SoundTouch;
}
void cSoundTouch_destroy(cSoundTouch soundTouch){
	delete (soundtouch::SoundTouch*)soundTouch;
}

void cSoundTouch_setRate(cSoundTouch soundTouch, double newRate){	
	((soundtouch::SoundTouch*)soundTouch)->setRate(newRate);
}
void cSoundTouch_setTempo(cSoundTouch soundTouch, double newTempo){	
	((soundtouch::SoundTouch*)soundTouch)->setTempo(newTempo);

}
void cSoundTouch_setRateChange(cSoundTouch soundTouch, double newRate){
	((soundtouch::SoundTouch*)soundTouch)->setRateChange( newRate);
}
void cSoundTouch_setTempoChange(cSoundTouch soundTouch, double newTempo){
	((soundtouch::SoundTouch*)soundTouch)->setTempoChange(   newTempo);
}
void cSoundTouch_setPitch(cSoundTouch soundTouch, double newPitch){
	((soundtouch::SoundTouch*)soundTouch)->setPitch( newPitch);
}
void cSoundTouch_setPitchOctaves(cSoundTouch soundTouch, double newPitch){
	((soundtouch::SoundTouch*)soundTouch)->setPitchOctaves(   newPitch);
}
void cSoundTouch_setPitchSemiTones(cSoundTouch soundTouch, int newPitch){
	((soundtouch::SoundTouch*)soundTouch)->setPitchSemiTones( newPitch);
}

void cSoundTouch_setChannels(cSoundTouch soundTouch, int numChannels){	
	((soundtouch::SoundTouch*)soundTouch)->setChannels( numChannels);
}
void cSoundTouch_setSampleRate(cSoundTouch soundTouch, int srate){
	((soundtouch::SoundTouch*)soundTouch)->setSampleRate( srate);
}
double cSoundTouch_getInputOutputSampleRatio(cSoundTouch soundTouch){
	
	return ((soundtouch::SoundTouch*)soundTouch)->getInputOutputSampleRatio();
}
void cSoundTouch_flush(cSoundTouch soundTouch){
	((soundtouch::SoundTouch*)soundTouch)->flush( );
}
void cSoundTouch_putSamples(cSoundTouch soundTouch, float* samples, int numSamples){
	((soundtouch::SoundTouch*)soundTouch)->putSamples(samples, numSamples);
}

void cSoundTouch_putSamples_i16(cSoundTouch soundTouch, short* samples, int numSamples)
{
	//((soundtouch::SoundTouch*)soundTouch)->putSamples_i16(soundTouch, samples, numSamples);
}

int cSoundTouch_receiveSamples(cSoundTouch soundTouch, float* output, int maxSamples){	
	return ((soundtouch::SoundTouch*)soundTouch)->receiveSamples( output, maxSamples);
}

int cSoundTouch_receiveSamples_i16(cSoundTouch soundTouch, short* output, int maxSamples) {
	return 0;
}

void cSoundTouch_clear(cSoundTouch soundTouch){
	((soundtouch::SoundTouch*)soundTouch)->clear();
}
int cSoundTouch_setSetting(cSoundTouch soundTouch, int settingId, int value){
	return ((soundtouch::SoundTouch*)soundTouch)->setSetting( settingId,  value);
}
int cSoundTouch_getSetting(cSoundTouch soundTouch, int settingId){
	return ((soundtouch::SoundTouch*)soundTouch)->getSetting(settingId);
}
int cSoundTouch_numUnprocessedSamples(cSoundTouch soundTouch){
	return ((soundtouch::SoundTouch*)soundTouch)->numUnprocessedSamples();
}
int cSoundTouch_numChannels(cSoundTouch soundTouch){
	return ((soundtouch::SoundTouch*)soundTouch) ->numChannels();
}

int cSoundTouch_numSamples(cSoundTouch soundTouch)
{
	return ((soundtouch::SoundTouch*)soundTouch)->numSamples();
}

#ifdef __cplusplus
};


#endif
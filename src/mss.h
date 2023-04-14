#pragma once

#include "depends.h"
#include "aout_sys.h"

VLC_EXTERN int Open(vlc_object_t *obj);
VLC_EXTERN void Close(vlc_object_t *obj);
VLC_EXTERN static int ReloadAudioDevices(vlc_object_t *obj, char const *name, char ***values, char ***descs);

// A stream must have been started when called.
VLC_EXTERN int Start(audio_output_t *aout, audio_sample_format_t *fmt);
VLC_EXTERN void Stop(audio_output_t *aout);
VLC_EXTERN int TimeGet(audio_output_t *aout, mtime_t *delay);
VLC_EXTERN void Play(audio_output_t *aout, block_t *block);
VLC_EXTERN void Pause(audio_output_t *aout, bool pause, mtime_t date);
VLC_EXTERN void Flush(audio_output_t *aout, bool wait);

// A stream may or may not have been started when called.
VLC_EXTERN int VolumeSet(audio_output_t *aout, float volume);
VLC_EXTERN int MuteSet(audio_output_t *aout, bool mute);
VLC_EXTERN int DeviceSelect(audio_output_t *aout, const char *id);

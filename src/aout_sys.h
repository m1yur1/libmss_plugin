#pragma once

#include "depends.h"

#include <Windows.h>
#include <mmdeviceapi.h>
#include <spatialaudioclient.h>

#include <array>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#pragma warning(disable: 4996)
#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_aout.h>

struct aout_sys_t
{
	enum
	{
		kThreadInitialized,
		kStopRequest,
		kGetPositionRequest,
		kGetPositionCompleted,
		kPauseRequest,
		kPauseCompleted,
		kFlushRequest,
		kFlushCompleted,
		kVolumeRequest,
		kVolumeCompleted,
		kEventsNum
	};

	std::mutex mutex_;

	audio_sample_format_t input_format_;
	WAVEFORMATEX output_format_;
	std::array<uint8_t, 8> channel_reorder_table_;

	std::wstring device_id_;
	DWORD wait_timeout_;
	int flush_wait_;
	int stop_wait_;

	// audio data frame
	std::queue<block_t *> audio_data_queue_;
	int64_t audio_data_frames_;

	// audio process thread
	bool thread_initialized_;
	std::thread audio_process_thread_;
	HRESULT thread_request_result_;
	std::array<HANDLE, aout_sys_t::kEventsNum> events_;

	// TimeGet
	int64_t frames_written_;
	LARGE_INTEGER qpc_frequency_;
	int64_t device_second_position_;
	int64_t device_micro_socond_position_;
	UINT64 qpc_position;

	// Pause
	bool pause_;

	// VolumeSet / MuteSet
	float volume_;

	// MuteSet
	bool mute_;
};

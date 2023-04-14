#include "AudioProcessThread.h"

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

#include <wil/com.h>

struct LocalVariables
{
	wil::com_ptr<IMMDevice> device_;
	wil::com_ptr<ISpatialAudioClient> spatioal_audio_client_;
	wil::com_ptr<ISpatialAudioObjectRenderStream> spatial_render_stream_;
	std::array<wil::com_ptr<ISpatialAudioObject>, 8> spacial_audio_objects_;
	wil::unique_handle stream_event_;
	wil::com_ptr<IAudioClock> audio_clock_;
	wil::com_ptr<IAudioStreamVolume> audio_stream_volume_;
	UINT64 device_frequency_;
};

static bool CreateLocalVariables(LocalVariables *local_obj, aout_sys_t *sys);
static void ReleaseLocalVariables(LocalVariables *local_obj);
static HRESULT CreateSpatialAudioObjects(std::array<wil::com_ptr<ISpatialAudioObject>, 8>& spacial_audio_objects, wil::com_ptr<ISpatialAudioObjectRenderStream>& spatial_render_stream, uint16_t physical_channels);
static void ReleaseSpatialAudioObjects(LocalVariables *local_obj);

static void Stream(aout_sys_t *sys, LocalVariables *local_obj);
static void GetPosition(aout_sys_t *sys, LocalVariables *local_obj);
static void Pause(aout_sys_t *sys, LocalVariables *local_obj);
static void Flush(aout_sys_t *sys, LocalVariables *local_obj);
static void Volume(aout_sys_t *sys, LocalVariables *local_obj);

static void ForwardAudioData(float *buffers[8], aout_sys_t *sys, size_t frames);
static void ForwardAudioDataBlock(float *const buffers[8], aout_sys_t *sys, block_t *block, size_t frames);
static void StreamWait(const aout_sys_t *sys, const LocalVariables *local_obj, int wait_loops);


void AudioProcessThread(aout_sys_t *sys)
{
	bool thread_initialized = false;
	HRESULT com_result;
	LocalVariables local;

	enum
	{
		kStream,
		kStop,
		kGetPosition,
		kPause,
		kFlush,
		kVolume,
		kEventsNum
	};
	HANDLE events[kEventsNum] {nullptr};

	com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED| COINIT_DISABLE_OLE1DDE);
	if (SUCCEEDED(com_result))
	{
		thread_initialized = CreateLocalVariables(&local, sys);

		events[kStream] = local.stream_event_.get();
		events[kStop] = sys->events_[aout_sys_t::kStopRequest];
		events[kGetPosition] = sys->events_[aout_sys_t::kGetPositionRequest];
		events[kPause] = sys->events_[aout_sys_t::kPauseRequest];
		events[kFlush] = sys->events_[aout_sys_t::kFlushRequest];
		events[kVolume] = sys->events_[aout_sys_t::kVolumeRequest];
	}

	sys->thread_initialized_ = thread_initialized;
	SetEvent(sys->events_[aout_sys_t::kThreadInitialized]);

	bool do_exit = false;

	if (!thread_initialized)
		goto EXIT;

	local.spatial_render_stream_->Start();

	while (!do_exit)
	{
		DWORD wait_result = WaitForMultipleObjects(kEventsNum, events, FALSE, sys->wait_timeout_);

		switch (wait_result)
		{
		case WAIT_OBJECT_0 + kStream:
			Stream(sys, &local);
			break;

		case WAIT_OBJECT_0 + kStop:
			do_exit = true;
			break;

		case WAIT_OBJECT_0 + kGetPosition:
			GetPosition(sys, &local);
			break;

		case WAIT_OBJECT_0 + kPause:
			Pause(sys, &local);
			break;

		case WAIT_OBJECT_0 + kFlush:
			Flush(sys, &local);
			break;

		case WAIT_OBJECT_0 + kVolume:
			Volume(sys, &local);
			break;
		}
	}

	StreamWait(sys, &local, sys->stop_wait_);
	local.spatial_render_stream_->Stop();
	local.spatial_render_stream_->Reset();
	ReleaseLocalVariables(&local);
	
EXIT:
	CoUninitialize();
}

bool CreateLocalVariables(LocalVariables *local_obj, aout_sys_t *sys)
{
	try
	{
		wil::com_ptr<IMMDeviceEnumerator> device_enumerator;
		wil::com_ptr<IMMDevice> device;
		wil::com_ptr<ISpatialAudioClient> client;
		wil::com_ptr<ISpatialAudioObjectRenderStream> stream;
		wil::com_ptr<IAudioClock> audio_clock;
		wil::com_ptr<IAudioStreamVolume> audio_stream_volume;
		PROPVARIANT stream_property;
		SpatialAudioObjectRenderStreamActivationParams stream_parameter {};
		wil::unique_handle stream_event;

		device_enumerator = wil::CoCreateInstance<MMDeviceEnumerator, IMMDeviceEnumerator>(CLSCTX_INPROC_SERVER);
		THROW_IF_FAILED(device_enumerator->GetDevice(sys->device_id_.c_str(), &device));
		THROW_IF_FAILED(device->Activate(__uuidof(ISpatialAudioClient), CLSCTX_INPROC_SERVER, nullptr, client.put_void()));

		stream_event.reset(CreateEvent(nullptr, FALSE, FALSE, nullptr));
		THROW_IF_NULL_ALLOC(stream_event.get());

		stream_parameter.ObjectFormat = &sys->output_format_;
		stream_parameter.StaticObjectTypeMask =
			AudioObjectType_FrontLeft| AudioObjectType_FrontRight|
			AudioObjectType_FrontCenter|
			AudioObjectType_LowFrequency|
			AudioObjectType_BackLeft| AudioObjectType_BackRight|
			AudioObjectType_SideLeft| AudioObjectType_SideRight;
		stream_parameter.MinDynamicObjectCount = 0;
		stream_parameter.MaxDynamicObjectCount = 0;
		stream_parameter.Category = AudioCategory_Movie;
		stream_parameter.EventHandle = stream_event.get();
		stream_parameter.NotifyObject = nullptr;

		PropVariantInit(&stream_property);
		stream_property.vt = VT_BLOB;
		stream_property.blob.cbSize = sizeof (stream_parameter);
		stream_property.blob.pBlobData = reinterpret_cast<BYTE *>(&stream_parameter);

		THROW_IF_FAILED(client->ActivateSpatialAudioStream(&stream_property, IID_PPV_ARGS(&stream)));
		THROW_IF_FAILED(stream->GetService(IID_PPV_ARGS(&audio_clock)));
		THROW_IF_FAILED(audio_clock->GetFrequency(&local_obj->device_frequency_));
		THROW_IF_FAILED(stream->GetService(IID_PPV_ARGS(&audio_stream_volume)));
		THROW_IF_FAILED(CreateSpatialAudioObjects(local_obj->spacial_audio_objects_, stream, sys->input_format_.i_physical_channels));

		local_obj->device_ = device;
		local_obj->spatioal_audio_client_ = client;
		local_obj->spatial_render_stream_ = stream;
		local_obj->audio_clock_ = audio_clock;
		local_obj->audio_stream_volume_ = audio_stream_volume;
		local_obj->stream_event_ = std::move(stream_event);
	}
	catch (wil::ResultException& e)
	{
		return false;
	}

	return true;
}

void ReleaseLocalVariables(LocalVariables *local_obj)
{
	ReleaseSpatialAudioObjects(local_obj);
	local_obj->device_.reset();
	local_obj->spatioal_audio_client_.reset();
	local_obj->spatial_render_stream_.reset();
	local_obj->audio_clock_.reset();
	local_obj->audio_stream_volume_.reset();
	local_obj->stream_event_.reset();
}

static HRESULT CreateSpatialAudioObjects(std::array<wil::com_ptr<ISpatialAudioObject>, 8>& spacial_audio_objects, wil::com_ptr<ISpatialAudioObjectRenderStream>& spatial_render_stream, uint16_t physical_channels)
{
	std::array<wil::com_ptr<ISpatialAudioObject>, 8> temp_spacial_audio_objects;
	std::vector<AudioObjectType> avilable_channel_types;
	static constexpr std::pair<uint32_t, AudioObjectType> channel_type_relations[]
	{
		std::make_pair(AOUT_CHAN_LEFT, AudioObjectType_FrontLeft),
		std::make_pair(AOUT_CHAN_RIGHT, AudioObjectType_FrontRight),
		std::make_pair(AOUT_CHAN_CENTER, AudioObjectType_FrontCenter),
		std::make_pair(AOUT_CHAN_LFE, AudioObjectType_LowFrequency),
		std::make_pair(AOUT_CHAN_REARLEFT, AudioObjectType_BackLeft),
		std::make_pair(AOUT_CHAN_REARRIGHT, AudioObjectType_BackRight),
		std::make_pair(AOUT_CHAN_MIDDLELEFT, AudioObjectType_SideLeft),
		std::make_pair(AOUT_CHAN_MIDDLERIGHT, AudioObjectType_SideRight)
	};

	// 使用するチャネルのAudioObjectTypeを挙げる。
	for (auto& relation: channel_type_relations)
	{
		if (physical_channels & relation.first)
			avilable_channel_types.push_back(relation.second);
	}

	// AudioObjectTypeの値で昇べきの順にすると、channel_reorder_table_ と spacial_audio_objects の整合性がとれるようにしてある。
	std::sort(avilable_channel_types.begin(), avilable_channel_types.end());

	for (int i=0; i<avilable_channel_types.size(); ++i)
		RETURN_IF_FAILED(spatial_render_stream->ActivateSpatialAudioObject(avilable_channel_types[i], temp_spacial_audio_objects[i].put()));

	for (int i=0; i<avilable_channel_types.size(); ++i)
		spacial_audio_objects[i] = temp_spacial_audio_objects[i];

	return S_OK;
}

void ReleaseSpatialAudioObjects(LocalVariables *local_obj)
{
	for (auto& audio_obj: local_obj->spacial_audio_objects_)
	{
		if (audio_obj.get())
			audio_obj.reset();
	}
}

void Stream(aout_sys_t *sys, LocalVariables *local_obj)
{
	UINT32 dynamic_objects;
	UINT32 frames;

	if (SUCCEEDED(local_obj->spatial_render_stream_->BeginUpdatingAudioObjects(&dynamic_objects, &frames)))
	{
		std::array<float *, 8> buffers;

		for (int i=0; i<sys->input_format_.i_channels; ++i)
		{
			BYTE *buffer = nullptr;
			UINT32 buffer_length = 0;

			if (SUCCEEDED(local_obj->spacial_audio_objects_[i]->GetBuffer(&buffer, &buffer_length)))
				buffers[i] = reinterpret_cast<float *>(buffer);
			else
				buffers[i] = nullptr;
		}

		{
			std::lock_guard lock(sys->mutex_);

			if (frames <= sys->audio_data_frames_)
				ForwardAudioData(buffers.data(), sys, frames);
			else
				// TimeGetでのディレイ算出のため、キュー内のデータが不足してバッファに書込まない場合でもframes_written_に加算しておく
				sys->frames_written_ += frames;
		}

		local_obj->spatial_render_stream_->EndUpdatingAudioObjects();
	}
}

void GetPosition(aout_sys_t *sys, LocalVariables *local_obj)
{
	HRESULT com_result;
	UINT64 device_position;
	UINT64 qpc_position;

	com_result = local_obj->audio_clock_->GetPosition(&device_position, &qpc_position);
	sys->thread_request_result_ = com_result;

	if (SUCCEEDED(com_result))
	{
		sys->device_second_position_ =  device_position / local_obj->device_frequency_;
		sys->device_micro_socond_position_ = ((device_position % local_obj->device_frequency_) * 1000 * 1000) / local_obj->device_frequency_;
		sys->qpc_position = qpc_position;
	}

	SetEvent(sys->events_[aout_sys_t::kGetPositionCompleted]);
}

void Pause(aout_sys_t *sys, LocalVariables *local_obj)
{
	if (sys->pause_)
		local_obj->spatial_render_stream_->Stop();
	else
		local_obj->spatial_render_stream_->Start();

	SetEvent(sys->events_[aout_sys_t::kPauseCompleted]);
}

void Flush(aout_sys_t *sys, LocalVariables *local_obj)
{
	{
		std::lock_guard lock(sys->mutex_);

		while (!sys->audio_data_queue_.empty())
		{
			block_Release(sys->audio_data_queue_.front());
			sys->audio_data_queue_.pop();
		}

		sys->audio_data_frames_ = 0;
		sys->frames_written_ = 0;
	}

	StreamWait(sys, local_obj, sys->flush_wait_);
	local_obj->spatial_render_stream_->Stop();
	local_obj->spatial_render_stream_->Reset();

	// ResetするとSpatialAudioObjectが非アクティブになるため、再度有効にする
	ReleaseSpatialAudioObjects(local_obj);
	CreateSpatialAudioObjects(local_obj->spacial_audio_objects_, local_obj->spatial_render_stream_, sys->input_format_.i_physical_channels);
	
	local_obj->spatial_render_stream_->Start();

	SetEvent(sys->events_[aout_sys_t::kFlushCompleted]);
}

void Volume(aout_sys_t *sys, LocalVariables *local_obj)
{
	HRESULT com_result;
	float volume;
	UINT32 channels;

	if (sys->mute_)
		volume = 0.0f;
	else
		volume = sys->volume_;

	com_result = local_obj->audio_stream_volume_->GetChannelCount(&channels);

	if (SUCCEEDED(com_result))
	{
		for (UINT32 i=0; i<channels; ++i)
		{
			com_result = local_obj->audio_stream_volume_->SetChannelVolume(i, volume);
			if (FAILED(com_result))
				break;
		}
	}

	sys->thread_request_result_ = com_result;
	SetEvent(sys->events_[aout_sys_t::kVolumeCompleted]);
}

void ForwardAudioData(float *buffers[8], aout_sys_t *sys, size_t frames)
{
	while (frames)
	{
		block_t *block = sys->audio_data_queue_.front();

		while (0 == block->i_nb_samples)
		{
			block_Release(block);
			sys->audio_data_queue_.pop();
			block = sys->audio_data_queue_.front();
		}

		size_t copy_frames = std::min(frames, static_cast<size_t>(block->i_nb_samples));

		ForwardAudioDataBlock(buffers, sys, block, copy_frames);

		// 書込み先ポインタの更新
		for (int channel=0; channel<sys->input_format_.i_channels; ++channel)
		{
			if (buffers[channel])
				buffers[channel] += copy_frames;
		}

		frames -= copy_frames;
	}
}

void ForwardAudioDataBlock(float *const buffers[8], aout_sys_t *sys, block_t *block, size_t frames)
{
	float *src = reinterpret_cast<float *>(block->p_buffer);
	const uint8_t channels = sys->input_format_.i_channels;

	for (size_t frame=0; frame<frames; ++frame)
	{
		for (int channel=0; channel<channels; ++channel)
		{
			if (buffers[channel])
				*(buffers[channel] + frame) = src[sys->channel_reorder_table_[channel]];
		}

		src += sys->input_format_.i_channels;
	}

	block->p_buffer += sizeof(float) * channels * frames;
	block->i_buffer -= sizeof(float) * channels * frames;
	block->i_nb_samples -= frames;
	sys->audio_data_frames_ -= frames;
	sys->frames_written_ += frames;
}

// ISpatialAudioObjectRenderStreamBase::Reset を呼んで成功しても
// プロセス外のバッファをフラッシュしてくれず、雑音の元となるので、
// データを書込まず待つことで、その不具合を解消する。
static void StreamWait(const aout_sys_t *sys, const LocalVariables *local_obj, int wait_loops)
{
	for (int n=0; n<wait_loops; ++n)
	{
		UINT32 dynamic_objects;
		UINT32 frames;
		BYTE *buffer;
		UINT32 buffer_length;

		if (WAIT_OBJECT_0 != WaitForSingleObject(local_obj->stream_event_.get(), sys->wait_timeout_))
			continue;

		local_obj->spatial_render_stream_->BeginUpdatingAudioObjects(&dynamic_objects, &frames);
		
		for (int i=0; i<sys->input_format_.i_channels; ++i)
			local_obj->spacial_audio_objects_[i]->GetBuffer(&buffer, &buffer_length);

		local_obj->spatial_render_stream_->EndUpdatingAudioObjects();
	}
}


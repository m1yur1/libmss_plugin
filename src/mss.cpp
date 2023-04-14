#include "mss.h"
#include "AudioProcessThread.h"

#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <wil/com.h>

static const char *kVolumeSaveConfig = "volume-save";
static const char *kDeviceConfig = "mss-audio-device";
static const char *kVolumeConfig = "mss-volume";
static const char *kMuteConfig = "mss-mute";
static const char *kWaitTimeoutConfig = "mss-wait-timeout";
static const char *kFlushWaitConfig = "mss-flush-wait";
static const char *kStopWaitConfig = "mss-stop-wait";

static unsigned MakeChannelReorderTable(uint8_t *table, uint16_t physical_channels);
static void MakeDeviceIdTable(std::vector<std::string>& ids, std::vector<std::string>& descs);
static std::string CreateUtf8StringFromWideCharString(LPCWCH wide_char_string);
static std::wstring CreateWideCharStringFromUtf8String(LPCCH Utf8_string);
static void GetSupportedFormats(std::vector<WAVEFORMATEX>& formats, const std::wstring& device_id);
static vlc_fourcc_t WaveFormatToVlcFourcc(const WAVEFORMATEX& wave_format);

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
	UNREFERENCED_PARAMETER(lpvReserved);

	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		DisableThreadLibraryCalls(hinstDLL);
		break;

	case DLL_PROCESS_DETACH:
		break;
	}

	return TRUE;
}

VLC_EXTERN int Open(vlc_object_t *obj)
{
	audio_output_t *aout = reinterpret_cast<audio_output_t *>(obj);
	aout_sys_t *sys;
	std::vector<std::string> device_ids;
	std::vector<std::string> device_descriptions;

	if (!InitializeMsvcrtMalloc())
		return VLC_EGENERIC;

	sys = new aout_sys_t;
	sys->thread_initialized_ = false;
	sys->volume_ = var_InheritFloat(aout, kVolumeConfig);
	sys->mute_ = var_InheritBool(aout, kMuteConfig);
	MakeDeviceIdTable(device_ids, device_descriptions);

	aout->sys = sys;
	aout->start = Start;
	aout->volume_set = VolumeSet;
	aout->mute_set = MuteSet;
	aout->device_select = DeviceSelect;

	for (size_t i=0; i<device_ids.size(); ++i)
		aout_HotplugReport(aout, device_ids[i].c_str(), device_descriptions[i].c_str());

	aout_VolumeReport(aout, sys->volume_);
	aout_MuteReport(aout, sys->mute_);

	vlc_value_t value;
	var_Inherit(obj, kDeviceConfig, VLC_VAR_STRING, &value);
	std::wstring device_id = CreateWideCharStringFromUtf8String(value.psz_string);
	sys->device_id_ = device_id;
	aout_DeviceReport(aout, value.psz_string);
	msvcrt_free(value.psz_string);

	return VLC_SUCCESS;
}

VLC_EXTERN void Close(vlc_object_t *obj)
{
	audio_output_t *aout = reinterpret_cast<audio_output_t *>(obj);
	aout_sys_t *sys = aout->sys;

	delete aout->sys;
}

VLC_EXTERN static int ReloadAudioDevices(vlc_object_t *obj, char const *name, char ***values, char ***descs)
{
	audio_output_t *aout = reinterpret_cast<audio_output_t *>(obj);
	aout_sys_t *sys = aout->sys;
	std::vector<std::string> ids;
	std::vector<std::string> descriptions;

	if (!InitializeMsvcrtMalloc())
		return 0;

	MakeDeviceIdTable(ids, descriptions);

	char **vs = static_cast<char **>(msvcrt_malloc(sizeof(char **) * ids.size()));
	for (size_t i=0; i<ids.size(); ++i)
		vs[i] = msvcrt_strdup(ids[i].c_str());

	char **ds = static_cast<char **>(msvcrt_malloc(sizeof(char **) * descriptions.size()));
	for (size_t i=0; i<descriptions.size(); ++i)
		ds[i] = msvcrt_strdup(descriptions[i].c_str());

	*values = vs;
	*descs = ds;

	return ids.size();
}

VLC_EXTERN int Start(audio_output_t *aout, audio_sample_format_t *fmt)
{
	aout_sys_t *sys = aout->sys;

	vlc_fourcc_t input_fourcc = fmt->i_format;
	vlc_fourcc_t output_fourcc = VLC_CODEC_UNKNOWN;
	WAVEFORMATEX output_format;
	std::vector<WAVEFORMATEX> output_formats;

	GetSupportedFormats(output_formats, sys->device_id_);
	
	// 出力フォーマットを決定する
	for (const auto& format: output_formats)
	{
		output_fourcc = WaveFormatToVlcFourcc(format);

		if (VLC_CODEC_UNKNOWN != output_fourcc)
		{
			output_format = format;
			break;
		}
	}

	// 適切な出力フォーマットが無い場合
	if (VLC_CODEC_UNKNOWN == output_fourcc)
		return VLC_EGENERIC;

	// 入力フォーマットと出力フォーマットが異なる場合にVLC_EGENERICを返すことで、
	// 次回の呼出し時にフォーマット変換を挿んでくれることを期待する。
	if (input_fourcc != output_fourcc)
		return VLC_EGENERIC;

	// サンプリングレートと構成チャネルを書き換えると、
	// 本体側で上手く変換してくれるようだ。
	fmt->i_format = output_fourcc;
	fmt->i_rate = output_format.nSamplesPerSec;
	fmt->i_physical_channels &= AOUT_CHANS_7_1;
	fmt->channel_type = AUDIO_CHANNEL_TYPE_BITMAP;
	aout_FormatPrepare(fmt);

	sys->input_format_ = *fmt;
	sys->output_format_ = output_format;
	MakeChannelReorderTable(sys->channel_reorder_table_.data(), fmt->i_physical_channels);

	std::array<wil::unique_handle, aout_sys_t::kEventsNum> handles;
	for (auto& handle: handles)
	{
		HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (!event)
			return VLC_EGENERIC;

		handle.reset(event);
	}

	for (int i=0; i<aout_sys_t::kEventsNum; ++i)
		sys->events_[i] = handles[i].release();

	aout->stop = Stop;
	aout->time_get = TimeGet;
	aout->play = Play;
	aout->pause = Pause;
	aout->flush = Flush;

	sys->wait_timeout_ = var_InheritInteger(aout, kWaitTimeoutConfig);
	sys->flush_wait_ = var_InheritInteger(aout, kFlushWaitConfig);
	sys->stop_wait_ = var_InheritInteger(aout, kStopWaitConfig);

	sys->audio_data_frames_ = 0;
	sys->frames_written_ = 0;
	QueryPerformanceFrequency(&sys->qpc_frequency_);

	sys->thread_initialized_ = false;
	sys->audio_process_thread_ = std::thread(AudioProcessThread, sys);
	WaitForSingleObject(sys->events_[aout_sys_t::kThreadInitialized], INFINITE);

	if (!sys->thread_initialized_)
	{
		sys->audio_process_thread_.join();
		std::for_each(sys->events_.begin(), sys->events_.end(),
			[](HANDLE h)
			{
				CloseHandle(h);
			}
		);
		return VLC_EGENERIC;
	}

	VolumeSet(aout, sys->volume_);
	MuteSet(aout, sys->mute_);

	return VLC_SUCCESS;
}

VLC_EXTERN void Stop(audio_output_t *aout)
{
	aout_sys_t *sys = aout->sys;

	SetEvent(sys->events_[aout_sys_t::kStopRequest]);
	sys->audio_process_thread_.join();

	while (!sys->audio_data_queue_.empty())
	{
		block_Release(sys->audio_data_queue_.front());
		sys->audio_data_queue_.pop();
	}

	std::for_each(sys->events_.begin(), sys->events_.end(),
		[](HANDLE h)
		{
			CloseHandle(h);
		}
	);
}

VLC_EXTERN int TimeGet(audio_output_t *aout, mtime_t *delay)
{
	aout_sys_t *sys = aout->sys;

	SetEvent(sys->events_[aout_sys_t::kGetPositionRequest]);
	WaitForSingleObject(sys->events_[aout_sys_t::kGetPositionCompleted], INFINITE);
	if (FAILED(sys->thread_request_result_))
		return VLC_EGENERIC;

	sys->mutex_.lock();
	const int64_t total_frames = sys->frames_written_ + sys->audio_data_frames_;
	sys->mutex_.unlock();

	const int64_t frequency = sys->input_format_.i_rate;
	const int64_t total_sec = total_frames / frequency;
	const int64_t total_micro_sec = ((total_frames % frequency) * 1000 * 1000) / frequency;

	*delay = (total_sec - sys->device_second_position_) * 1000 * 1000;
	*delay += total_micro_sec;
	*delay -= sys->device_micro_socond_position_;

	LARGE_INTEGER perf_count;
	QueryPerformanceCounter(&perf_count);
	if (perf_count.QuadPart <= sys->qpc_position)
		return VLC_SUCCESS;

	const UINT64 between_qpc = perf_count.QuadPart - sys->qpc_position;
	const UINT64 between_delay = (between_qpc * 1000 * 1000) / sys->qpc_frequency_.QuadPart;

	*delay -= between_delay;

	return VLC_SUCCESS;
}

VLC_EXTERN void Play(audio_output_t *aout, block_t *block)
{
	aout_sys_t *sys = aout->sys;

	{
		std::lock_guard lock(sys->mutex_);

		sys->audio_data_queue_.push(block);
		sys->audio_data_frames_ += block->i_nb_samples;
	}
}

VLC_EXTERN void Pause(audio_output_t *aout, bool pause, mtime_t date)
{
	UNREFERENCED_PARAMETER(date);
	aout_sys_t *sys = aout->sys;

	sys->pause_ = pause;
	SetEvent(sys->events_[aout_sys_t::kPauseRequest]);
	WaitForSingleObject(sys->events_[aout_sys_t::kPauseCompleted], INFINITE);
}

VLC_EXTERN void Flush(audio_output_t *aout, bool wait)
{
	aout_sys_t *sys = aout->sys;

	if (wait)
	{
		mtime_t delay = 0;

		if ((VLC_SUCCESS == TimeGet(aout, &delay)) && delay)
			Sleep(delay / 1000);
	}
	else
	{
		SetEvent(sys->events_[aout_sys_t::kFlushRequest]);
		WaitForSingleObject(sys->events_[aout_sys_t::kFlushCompleted], INFINITE);
	}
}

VLC_EXTERN int VolumeSet(audio_output_t *aout, float volume)
{
	aout_sys_t *sys = aout->sys;

	sys->volume_ = std::clamp(volume, 0.0f, 1.0f);
	if (var_InheritBool(aout, kVolumeSaveConfig))
		config_PutFloat(aout, kVolumeConfig, sys->volume_);

	aout_VolumeReport(aout, sys->volume_);

	if (!sys->thread_initialized_)
		return VLC_EGENERIC;

	SetEvent(sys->events_[aout_sys_t::kVolumeRequest]);
	WaitForSingleObject(sys->events_[aout_sys_t::kVolumeCompleted], INFINITE);

	if (FAILED(sys->thread_request_result_))
		return VLC_EGENERIC;

	return VLC_SUCCESS;
}

VLC_EXTERN int MuteSet(audio_output_t *aout, bool mute)
{
	aout_sys_t *sys = aout->sys;

	sys->mute_ = mute? true: false;
	if (var_InheritBool(aout, kVolumeSaveConfig))
		config_PutFloat(aout, kMuteConfig, sys->mute_);

	aout_MuteReport(aout, mute);

	if (!sys->thread_initialized_)
		return VLC_EGENERIC;

	SetEvent(sys->events_[aout_sys_t::kVolumeRequest]);
	WaitForSingleObject(sys->events_[aout_sys_t::kVolumeCompleted], INFINITE);

	if (FAILED(sys->thread_request_result_))
		return VLC_EGENERIC;

	return VLC_SUCCESS;
}

VLC_EXTERN int DeviceSelect(audio_output_t *aout, const char *id)
{
	aout_sys_t *sys = aout->sys;

	std::wstring device_id = CreateWideCharStringFromUtf8String(id);
	sys->device_id_ = device_id;

	aout_DeviceReport(aout, id);
	aout_RestartRequest(aout, AOUT_RESTART_OUTPUT);

	return VLC_SUCCESS;
}

static unsigned MakeChannelReorderTable(uint8_t *table, uint16_t physical_channels)
{
	static constexpr uint32_t output_order[] =
	{
		AOUT_CHAN_LEFT, AOUT_CHAN_RIGHT,
		AOUT_CHAN_CENTER,
		AOUT_CHAN_LFE,
		AOUT_CHAN_REARLEFT, AOUT_CHAN_REARRIGHT,
		AOUT_CHAN_MIDDLELEFT, AOUT_CHAN_MIDDLERIGHT
	};

	return aout_CheckChannelReorder(nullptr, output_order, physical_channels, table);
}

static void MakeDeviceIdTable(std::vector<std::string>& ids, std::vector<std::string>& descs)
{
	std::thread t([&]()
		{
			if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED| COINIT_DISABLE_OLE1DDE)))
				return;

			try
			{
				wil::com_ptr<IMMDeviceEnumerator> device_enumerator;
				wil::com_ptr<IMMDeviceCollection> device_collection;
				UINT devices;

				device_enumerator = wil::CoCreateInstance<MMDeviceEnumerator, IMMDeviceEnumerator>();
				THROW_IF_FAILED(device_enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, device_collection.put()));
				THROW_IF_FAILED(device_collection->GetCount(&devices));

				for (UINT item_index=0; item_index<devices; ++item_index)
				{
					wil::com_ptr<IMMDevice> device;
					wil::unique_cotaskmem_string device_id;
					wil::com_ptr<IPropertyStore> properties;
					wil::unique_prop_variant friendly_name;

					THROW_IF_FAILED(device_collection->Item(item_index, &device));
					THROW_IF_FAILED(device->GetId(device_id.put()));
					THROW_IF_FAILED(device->OpenPropertyStore(STGM_READ, properties.put()));
					THROW_IF_FAILED(properties->GetValue(PKEY_Device_FriendlyName, &friendly_name));

					ids.push_back(CreateUtf8StringFromWideCharString(device_id.get()));
					descs.push_back(CreateUtf8StringFromWideCharString(friendly_name.pwszVal));
				}
			}
			catch (wil::ResultException& e)
			{
			}

			CoUninitialize();
		});

	t.join();
}

static std::string CreateUtf8StringFromWideCharString(LPCWCH wide_char_string)
{
	int result_chars = WideCharToMultiByte(CP_UTF8, 0, wide_char_string, -1, nullptr, 0, nullptr, nullptr);
	if (!result_chars)
		return std::string();

	std::unique_ptr<CHAR []> buffer(new CHAR[result_chars]);
	WideCharToMultiByte(CP_UTF8, 0, wide_char_string, -1, buffer.get(), result_chars, nullptr, nullptr);

	return std::string(buffer.get());
}

static std::wstring CreateWideCharStringFromUtf8String(LPCCH utf8_string)
{
	int result_wchars = MultiByteToWideChar(CP_ACP, 0, utf8_string, -1, nullptr, 0);
	if (!result_wchars)
		return std::wstring();

	std::unique_ptr<WCHAR[]> buffer(new WCHAR[result_wchars]);
	MultiByteToWideChar(CP_UTF8, 0, utf8_string, -1, buffer.get(), result_wchars);

	return std::wstring(buffer.get());
}

static void GetSupportedFormats(std::vector<WAVEFORMATEX>& formats, const std::wstring& device_id)
{
	std::thread t([&]()
		{
			if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED| COINIT_DISABLE_OLE1DDE)))
				return;

			try
			{
				wil::com_ptr<IMMDeviceEnumerator> device_enumerator;
				wil::com_ptr<IMMDevice> device;
				wil::com_ptr<ISpatialAudioClient> client;
				wil::com_ptr<IAudioFormatEnumerator> enumerator;
				UINT32 format_counts;

				device_enumerator = wil::CoCreateInstance<MMDeviceEnumerator, IMMDeviceEnumerator>(CLSCTX_INPROC_SERVER);
				THROW_IF_FAILED(device_enumerator->GetDevice(device_id.c_str(), device.put()));
				THROW_IF_FAILED(device->Activate(__uuidof(ISpatialAudioClient), CLSCTX_INPROC_SERVER, nullptr, client.put_void()));
				THROW_IF_FAILED(client->GetSupportedAudioObjectFormatEnumerator(enumerator.put()));
				THROW_IF_FAILED(enumerator->GetCount(&format_counts));

				for (UINT32 index=0; index<format_counts; ++index)
				{
					WAVEFORMATEX *format;

					if (SUCCEEDED(enumerator->GetFormat(index, &format)))
						formats.push_back(*format);
				}
			}
			catch (wil::ResultException& e)
			{
			}
		
			CoUninitialize();
		});

	t.join();
}

static vlc_fourcc_t WaveFormatToVlcFourcc(const WAVEFORMATEX& wave_format)
{
	vlc_fourcc_t vlc_fourcc = VLC_CODEC_UNKNOWN;

	switch (wave_format.wFormatTag)
	{
	case WAVE_FORMAT_IEEE_FLOAT:
		vlc_fourcc = VLC_CODEC_FL32;
		break;

	case WAVE_FORMAT_PCM:
		switch (wave_format.wBitsPerSample)
		{
		case 16:
			vlc_fourcc = VLC_CODEC_S16N;
			break;

		case 24:
			vlc_fourcc = VLC_CODEC_S24N;
			break;

		case 32:
			vlc_fourcc = VLC_CODEC_S32N;
			break;
		}

		break;
	}

	return vlc_fourcc;
}


vlc_module_begin()
set_shortname("MSS")
set_description("Microsoft Spatial Sound audio output")
set_capability("audio output", 60)
set_callbacks(Open, Close)
set_category(CAT_AUDIO)
set_subcategory(SUBCAT_AUDIO_AOUT)
add_string(kDeviceConfig, nullptr, "Output device", "", false)
change_string_cb(ReloadAudioDevices)
add_float_with_range(kVolumeConfig, 0.5f, 0.0f, 1.0f, "Audio volume", "", false)
add_bool(kMuteConfig, false, "Audio mute", "", false)
add_integer_with_range(kWaitTimeoutConfig, 10, 1, 100, "Wait Timeout Value", "", false)
add_integer_with_range(kFlushWaitConfig, 0, 0, 100, "Flush Wait", "Number of times the buffer is cleared when the callback function flush is called.", false)
add_integer_with_range(kStopWaitConfig, 10, 0, 100, "Stop Wait", "Number of times the buffer is cleared when the callback function stop is called.", false)
vlc_module_end()

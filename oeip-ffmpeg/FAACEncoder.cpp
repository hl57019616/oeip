#include "FAACEncoder.h"
#include <memory>

inline static bool check_sample_fmt(AVCodec* codec, enum AVSampleFormat sample_fmt) {
	const enum AVSampleFormat* p = codec->sample_fmts;
	int i = 0;
	while (p[i] != AV_SAMPLE_FMT_NONE) {
		if (p[i] == sample_fmt) {
			return true;
		}
		i++;
	}
	return false;
}

inline static void buildAdts(int size, uint8_t* buffer, int samplerate, int channels) {
	char* padts = (char*)buffer;
	int profile = 2;                                            //AAC LC
	int freqIdx = get_sr_index(samplerate);                     //44.1KHz
	int chanCfg = channels;            //MPEG-4 Audio Channel Configuration. 1 Channel front-center
	padts[0] = (char)0xFF;      // 11111111     = syncword
	padts[1] = (char)0xF1;      // 1111 1 00 1  = syncword MPEG-2 Layer CRC
	padts[2] = (char)(((profile - 1) << 6) + (freqIdx << 2) + (chanCfg >> 2));
	padts[6] = (char)0xFC;
	padts[3] = (char)(((chanCfg & 3) << 6) + ((7 + size) >> 11));
	padts[4] = (char)(((7 + size) & 0x7FF) >> 3);
	padts[5] = (char)((((7 + size) & 7) << 5) + 0x1F);
}


FAACEncoder::FAACEncoder(const OeipAudioEncoder& encoderDesc) {
	this->encoderDesc = encoderDesc;
	openEncode();
}

FAACEncoder::~FAACEncoder() {
	if (samples) {
		av_free(samples);
		samples = nullptr;
	}
}

int32_t FAACEncoder::openEncode() {
	auto codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
	if (!codec) {
		logMessage(OEIP_ERROR, "could not find acc encoder.");
		return -1;
	}
	AVCodecContext* temp = avcodec_alloc_context3(codec);
	if (!temp) {
		logMessage(OEIP_ERROR, "acc avcodec_alloc_context3 failed!");
		return -2;
	}
	//设置ACC编码属性
	cdeCtx = getUniquePtr(temp);
	cdeCtx->profile = FF_PROFILE_AAC_LOW;
	cdeCtx->codec_type = AVMEDIA_TYPE_AUDIO;
	cdeCtx->bit_rate = encoderDesc.bitrate;
	cdeCtx->sample_fmt = outSampleFormat;
	cdeCtx->sample_rate = encoderDesc.frequency;
	cdeCtx->channel_layout = encoderDesc.channel == 1 ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO; // 双声道
	cdeCtx->channels = av_get_channel_layout_nb_channels(cdeCtx->channel_layout);
	if (!check_sample_fmt(codec, cdeCtx->sample_fmt)) {
		logMessage(OEIP_ERROR, "acc encoder does not support sample format AV_SAMPLE_FMT_FLTP");
		return -3;
	}
	if (avcodec_open2(cdeCtx.get(), codec, nullptr) < 0) {
		logMessage(OEIP_ERROR, "acc avcodec_open2 failed!");
		return -4;
	}
	//分配音频桢
	frame = getUniquePtr(av_frame_alloc());
	frame->nb_samples = cdeCtx->frame_size;
	frame->format = cdeCtx->sample_fmt;
	frame->channel_layout = cdeCtx->channel_layout;
	frame->channels = cdeCtx->channels;
	frame->sample_rate = cdeCtx->sample_rate;
	//分配音频重采样(原始数据格式转化成AAC所需要数据格式)
	auto tempSwr = swr_alloc_set_opts(nullptr,
		av_get_default_channel_layout(cdeCtx->channels),
		cdeCtx->sample_fmt, cdeCtx->sample_rate,
		av_get_default_channel_layout(cdeCtx->channels),
		inSampleFormat, cdeCtx->sample_rate, 0, nullptr);
	if (!tempSwr) {
		logMessage(OEIP_ERROR, "could not allocate resampler context!");
		return -5;
	}
	swrCtx = getUniquePtr(tempSwr);
	int32_t ret = swr_init(swrCtx.get());
	if (ret < 0) {
		logRetffmpeg("failed to initialize the resampling context!", ret);
		return ret;
	}
	bufferSize = av_samples_get_buffer_size(nullptr, cdeCtx->channels, cdeCtx->frame_size, cdeCtx->sample_fmt, 0);
	if (bufferSize < 0) {
		logMessage(OEIP_ERROR, "acc av_samples_get_buffer_size failed!");
		return -7;
	}
	samples = static_cast<uint8_t*>(av_malloc(bufferSize));
	ret = avcodec_fill_audio_frame(frame.get(), cdeCtx->channels, cdeCtx->sample_fmt, reinterpret_cast<const uint8_t*>(samples), bufferSize, 0);
	if (ret < 0) {
		logRetffmpeg("acc avcodec_fill_audio_frame failed!", ret);
		return ret;
	}
	pcmBuffer.resize(OEIP_AAC_BUFFER_MAX_SIZE);
	bInit = true;
}

int FAACEncoder::encoder(const uint8_t* indata, int length, uint64_t timestamp) {
	if (!bInit)
		return -1;
	int ret = 0;
	memcpy(pcmBuffer.data() + pcmBufferSize, indata, length);
	pcmBufferSize += length;
	//传入需要的数据，单位是byte(2表示AV_SAMPLE_FMT_S16)
	int frameSize = cdeCtx->frame_size * cdeCtx->channels * 2;
	while (pcmBufferSize >= frameSize) {
		pcmBufferSize -= frameSize;
		frame->data[0] = const_cast<uint8_t*>(pcmBuffer.data());
		OAVFrame cframe = getUniquePtr(av_frame_alloc());
		cframe->nb_samples = cdeCtx->frame_size;
		cframe->format = cdeCtx->sample_fmt;
		cframe->channel_layout = cdeCtx->channel_layout;
		cframe->channels = av_get_channel_layout_nb_channels(cdeCtx->channel_layout);
		cframe->sample_rate = cdeCtx->sample_rate;
		int ret = av_frame_get_buffer(cframe.get(), 0);
		if (ret < 0) {
			logRetffmpeg("error allocating an audio buffer.", ret);
			return ret;
		}
		swr_convert(swrCtx.get(), (uint8_t**)cframe->data, cframe->nb_samples,
			(const uint8_t**)frame->data, frame->nb_samples);
		cframe->linesize[0] = frameSize;
		cframe->linesize[1] = frameSize;
		cframe->pkt_dts = timestamp;
		cframe->pts = timestamp;
		ret = avcodec_send_frame(cdeCtx.get(), cframe.get());
		if (ret < 0) {
			logRetffmpeg("aac avcodec_send_frame error.", ret);
			return ret;
		}
		memmove(pcmBuffer.data(), pcmBuffer.data() + frameSize, pcmBufferSize);
		timestamp += 5;
	}
	return 0;
}

int FAACEncoder::readPacket(uint8_t* outData, int& outLength, uint64_t& timestamp) {
	if (!bInit)
		return -1;
	int ret = 0;
	av_init_packet(&packet);
	ret = avcodec_receive_packet(cdeCtx.get(), &packet);
	if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
		return -100;
	}
	else if (ret < 0) {
		logRetffmpeg("acc avcodec_receive_packet error.", ret);
		av_packet_unref(&packet);
		return ret;
	}
	buildAdts(packet.size, outData, cdeCtx->sample_rate, cdeCtx->channels);
	memcpy(outData + 7, packet.data, packet.size);
	outLength = packet.size + 7;
	timestamp = packet.pts;
	av_packet_unref(&packet);
	return 0;
}
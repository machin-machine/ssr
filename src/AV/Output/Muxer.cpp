/*
Copyright (c) 2012-2014 Maarten Baert <maarten-baert@hotmail.com>

This file is part of SimpleScreenRecorder.

SimpleScreenRecorder is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

SimpleScreenRecorder is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with SimpleScreenRecorder.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "Global.h"
#include "Muxer.h"

#include "Logger.h"
#include "AVWrapper.h"
#include "BaseEncoder.h"

static const unsigned int INVALID_STREAM = std::numeric_limits<unsigned int>::max();
static const double NOPTS_DOUBLE = -std::numeric_limits<double>::max();

Muxer::Muxer(const QString& container_name, const QString& output_file) {

	m_container_name = container_name;
	m_output_file = output_file;

	m_format_context = NULL;
	m_started = false;

	// initialize stream data
	for(int i = 0; i < MUXER_MAX_STREAMS; ++i) {
		StreamLock lock(&m_stream_data[i]);
		lock->m_is_done = false;
		m_encoders[i] = NULL;
	}

	// initialize shared data
	{
		SharedLock lock(&m_shared_data);
		lock->m_total_bytes = 0;
		lock->m_stats_actual_bit_rate = 0.0;
		lock->m_stats_previous_pts = NOPTS_DOUBLE;
		lock->m_stats_previous_bytes = 0;
	}

	// initialize thread signals
	m_is_done = false;
	m_error_occurred = false;

	try {
		Init();
	} catch(...) {
		Free();
		throw;
	}

}

Muxer::~Muxer() {

	if(m_started) {

		// stop the encoders
		Logger::LogInfo("[Muxer::~Muxer] " + Logger::tr("Stopping encoders ..."));
		for(unsigned int i = 0; i < m_format_context->nb_streams; ++i) {
			m_encoders[i]->Stop(); // no deadlock: nothing in Muxer is locked in this thread (and BaseEncoder::Stop is lock-free, but that could change)
		}

		// wait for the thread to stop
		if(m_thread.joinable()) {
			Logger::LogInfo("[Muxer::~Muxer] " + Logger::tr("Waiting for muxer thread to stop ..."));
			m_thread.join();
		}

	}

	// free everything
	Free();

}

void Muxer::Start() {
	assert(!m_started);

	// make sure all encoders have registered
	for(unsigned int i = 0; i < m_format_context->nb_streams; ++i) {
		assert(m_encoders[i] != NULL);
	}

	// write header
	if(avformat_write_header(m_format_context, NULL) != 0) {
		Logger::LogError("[Muxer::Start] " + Logger::tr("Error: Can't write header!", "Don't translate 'header'"));
		throw LibavException();
	}

	m_started = true;
	m_thread = std::thread(&Muxer::MuxerThread, this);

}

void Muxer::Finish() {
	assert(m_started);
	Logger::LogInfo("[Muxer::Finish] " + Logger::tr("Finishing encoders ..."));
	for(unsigned int i = 0; i < m_format_context->nb_streams; ++i) {
		assert(m_encoders[i] != NULL);
		m_encoders[i]->Finish(); // no deadlock: nothing in Muxer is locked in this thread (and BaseEncoder::Finish is lock-free, but that could change)
	}
}

bool Muxer::IsStarted() {
	return m_started;
}

double Muxer::GetActualBitRate() {
	SharedLock lock(&m_shared_data);
	return lock->m_stats_actual_bit_rate;
}

uint64_t Muxer::GetTotalBytes() {
	SharedLock lock(&m_shared_data);
	return lock->m_total_bytes;
}

AVStream* Muxer::CreateStream(AVCodec* codec) {
	assert(!m_started);
	assert(m_format_context->nb_streams < MUXER_MAX_STREAMS);

	// create a new stream
#if SSR_USE_AVFORMAT_NEW_STREAM
	AVStream *stream = avformat_new_stream(m_format_context, codec);
#else
	AVStream *stream = av_new_stream(m_format_context, m_format_context->nb_streams);
#endif
	if(stream == NULL) {
		Logger::LogError("[Muxer::AddStream] " + Logger::tr("Error: Can't create new stream!"));
		throw LibavException();
	}

#if !SSR_USE_AVFORMAT_NEW_STREAM
	if(avcodec_get_context_defaults3(stream->codec, codec) < 0) {
		Logger::LogError("[Muxer::AddStream] " + Logger::tr("Error: Can't get codec context defaults!"));
		throw LibavException();
	}
	stream->codec->codec_id = codec->id;
	stream->codec->codec_type = codec->type;
#endif

	// not sure why this is needed, but it's in the example code and it doesn't work without this
	if(m_format_context->oformat->flags & AVFMT_GLOBALHEADER)
		stream->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;

	return stream;
}

void Muxer::RegisterEncoder(unsigned int stream_index, BaseEncoder* encoder) {
	assert(!m_started);
	assert(stream_index < m_format_context->nb_streams);
	assert(m_encoders[stream_index] == NULL);
	m_encoders[stream_index] = encoder;
}

void Muxer::EndStream(unsigned int stream_index) {
	assert(stream_index < m_format_context->nb_streams);
	StreamLock lock(&m_stream_data[stream_index]);
	lock->m_is_done = true;
}

void Muxer::AddPacket(unsigned int stream_index, std::unique_ptr<AVPacketWrapper> packet) {
	assert(m_started);
	assert(stream_index < m_format_context->nb_streams);
	StreamLock lock(&m_stream_data[stream_index]);
	lock->m_packet_queue.push_back(std::move(packet));
}

unsigned int Muxer::GetQueuedPacketCount(unsigned int stream_index) {
	assert(m_started);
	assert(stream_index < m_format_context->nb_streams);
	StreamLock lock(&m_stream_data[stream_index]);
	return lock->m_packet_queue.size();
}

void Muxer::Init() {

	// get the format we want (this is just a pointer, we don't have to free this)
	AVOutputFormat *format = av_guess_format(m_container_name.toAscii().constData(), NULL, NULL);
	if(format == NULL) {
		Logger::LogError("[Muxer::Init] " + Logger::tr("Error: Can't find chosen output format!"));
		throw LibavException();
	}

	Logger::LogInfo("[Muxer::Init] " + Logger::tr("Using format %1 (%2).").arg(format->name).arg(format->long_name));

	// allocate format context
	m_format_context = avformat_alloc_context();
	if(m_format_context == NULL) {
		Logger::LogError("[Muxer::Init] " + Logger::tr("Error: Can't allocate format context!"));
		throw LibavException();
	}
	m_format_context->oformat = format;

	// open file
	if(avio_open(&m_format_context->pb, m_output_file.toLocal8Bit().constData(), AVIO_FLAG_WRITE) < 0) {
		Logger::LogError("[Muxer::Init] " + Logger::tr("Error: Can't open output file!"));
		throw LibavException();
	}

}

void Muxer::Free() {
	if(m_format_context != NULL) {

		// write trailer (needed to free private muxer data)
		if(m_started) {
			if(av_write_trailer(m_format_context) != 0) {
				// we can't throw exceptions here because this is called from the destructor
				Logger::LogError("[Muxer::Free] " + Logger::tr("Error: Can't write trailer, continuing anyway.", "Don't translate 'trailer'"));
			}
			m_started = false;
		}

		// destroy the encoders
		for(unsigned int i = 0; i < m_format_context->nb_streams; ++i) {
			if(m_encoders[i] != NULL) {
				delete m_encoders[i]; // no deadlock: nothing in Muxer is locked in this thread
				m_encoders[i] = NULL;
			}
		}

		// close file
		if(m_format_context->pb != NULL) {
			avio_close(m_format_context->pb);
			m_format_context->pb = NULL;
		}

		// free everything
		for(unsigned int i = 0; i < m_format_context->nb_streams; ++i) {
			av_freep(&m_format_context->streams[i]->codec);
			av_freep(&m_format_context->streams[i]);
		}
		av_free(m_format_context);
		m_format_context = NULL;

	}
}

void Muxer::MuxerThread() {
	try {

		Logger::LogInfo("[Muxer::MuxerThread] " + Logger::tr("Muxer thread started."));

		// start muxing
		for( ; ; ) {

			// find the oldest stream that isn't done yet
			unsigned int oldest_stream = INVALID_STREAM;
			double oldest_pts = std::numeric_limits<double>::max();
			for(unsigned int i = 0; i < m_format_context->nb_streams; ++i) {
				StreamLock lock(&m_stream_data[i]);
				if(!lock->m_is_done || !lock->m_packet_queue.empty()) {
					double pts = ToDouble(m_format_context->streams[i]->pts) * ToDouble(m_format_context->streams[i]->time_base);
					if(pts < oldest_pts) {
						oldest_stream = i;
						oldest_pts = pts;
					}
				}
			}

			// if there are no packets left, we're done
			if(oldest_stream == INVALID_STREAM) {
				break;
			}

			// get the packet
			std::unique_ptr<AVPacketWrapper> packet;
			{
				StreamLock lock(&m_stream_data[oldest_stream]);
				if(!lock->m_packet_queue.empty()) {
					packet = std::move(lock->m_packet_queue.front());
					lock->m_packet_queue.pop_front();
				}
			}

			// if there is no packet, wait and try again later
			if(packet == NULL) {
				usleep(10000);
				continue;
			}

			// prepare packet
			AVStream *st = m_format_context->streams[oldest_stream];
			packet->GetPacket()->stream_index = oldest_stream;
			if(packet->GetPacket()->pts != (int64_t) AV_NOPTS_VALUE) {
				packet->GetPacket()->pts = av_rescale_q(packet->GetPacket()->pts, st->codec->time_base, st->time_base);
			}
			if(packet->GetPacket()->dts != (int64_t) AV_NOPTS_VALUE) {
				packet->GetPacket()->dts = av_rescale_q(packet->GetPacket()->dts, st->codec->time_base, st->time_base);
			}

			// write the packet (again, why does libav/ffmpeg call this a frame?)
			// The packet should already be interleaved now, but containers can have custom interleaving specifications,
			// so it's a good idea to call av_interleaved_write_frame anyway.
			if(av_interleaved_write_frame(m_format_context, packet->GetPacket()) != 0) {
				Logger::LogError("[Muxer::MuxerThread] " + Logger::tr("Error: Can't write frame to muxer!"));
				throw LibavException();
			}

			// the data is now owned by libav/ffmpeg, so don't free it
			packet->SetFreeOnDestruct(false);

			// update the byte counter
			{
				SharedLock lock(&m_shared_data);
				lock->m_total_bytes = m_format_context->pb->pos + (m_format_context->pb->buf_ptr - m_format_context->pb->buffer);
				if(lock->m_stats_previous_pts == NOPTS_DOUBLE) {
					lock->m_stats_previous_pts = oldest_pts;
					lock->m_stats_previous_bytes = lock->m_total_bytes;
				}
				double timedelta = oldest_pts - lock->m_stats_previous_pts;
				if(timedelta > 0.999999) {
					lock->m_stats_actual_bit_rate = (double) ((lock->m_total_bytes - lock->m_stats_previous_bytes) * 8) / timedelta;
					lock->m_stats_previous_pts = oldest_pts;
					lock->m_stats_previous_bytes = lock->m_total_bytes;
				}
			}

		}

		// tell the others that we're done
		m_is_done = true;

		Logger::LogInfo("[Muxer::MuxerThread] " + Logger::tr("Muxer thread stopped."));

	} catch(const std::exception& e) {
		m_error_occurred = true;
		Logger::LogError("[Muxer::MuxerThread] " + Logger::tr("Exception '%1' in muxer thread.").arg(e.what()));
	} catch(...) {
		m_error_occurred = true;
		Logger::LogError("[Muxer::MuxerThread] " + Logger::tr("Unknown exception in muxer thread."));
	}
}

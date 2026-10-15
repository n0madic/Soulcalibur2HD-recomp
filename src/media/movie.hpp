/*
 * A movie file played in the background: an MP4 with H.264 video and AAC audio, decoded by a worker thread into
 * small queues that the consumer drains in real time. Independent of the PS3 side (see sail_hle.cpp).
 *
 * Copyright (C) the sc2hd authors. SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SC2HD_MOVIE_HPP
#define SC2HD_MOVIE_HPP

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "aac.h"
#include "h264.h"
#include "mp4.h"

class Movie {
public:
    /* nullptr (and *err filled) if the file is missing or not something this player decodes. */
    static std::unique_ptr<Movie> open(const std::string& path, std::string* err);
    ~Movie();

    int width() const { return width_; }
    int height() const { return height_; }
    bool has_audio() const { return audio_ != nullptr && audio_on_; }
    int audio_rate() const { return audio_rate_; }
    int audio_channels() const { return audio_channels_; }

    /* Which tracks have a consumer. A track without one is not decoded: its queue would never drain and the movie would
     * never be finished. Call before start(). */
    void set_outputs(bool video, bool audio);

    /* Starts the worker thread. */
    void start();

    /* The newest decoded picture presented at or before `clock_us`, converted to RGBA (R, G, B, 255 in memory
     * order) into dst. Older pictures that are due are dropped. False if none is due. */
    bool pop_video_rgba(int64_t clock_us, uint8_t* dst, int pitch, int64_t* pts_us);

    /* Interleaved float samples. Returns the number of frames (samples per channel) copied, at most `frames`. */
    size_t read_audio(float* dst, size_t frames, int64_t* pts_us);
    size_t audio_available();

    /* No more pictures or samples will ever be produced and the queues are empty. */
    bool finished();
    /* The same for the sound alone. */
    bool audio_finished();
    /* Presentation time of the last picture, microseconds (0 until the worker has seen the end of the file). */
    int64_t video_end_us() const { return video_end_us_.load(); }

private:
    struct Picture {
        std::vector<uint8_t> yuv;   /* Y, U, V planes, tightly packed */
        int64_t pts_us = 0;
        int matrix = 2, full_range = 0;
    };

    Movie() = default;
    void worker();
    void push_picture(Picture&& p);

    mp4* mp4_ = nullptr;
    const mp4_track* vtrack_ = nullptr;
    const mp4_track* atrack_ = nullptr;
    h264_dec* video_ = nullptr;
    aac_dec* audio_ = nullptr;
    int width_ = 0, height_ = 0, audio_rate_ = 0, audio_channels_ = 0;
    int reorder_depth_ = 0;
    bool video_on_ = true, audio_on_ = true;
    size_t max_packet_ = 0;                 /* the largest sample of either track */

    std::thread thread_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool stop_ = false, eof_ = false;
    std::deque<Picture> pictures_;          /* in display order */
    std::vector<float> pcm_;                /* interleaved */
    size_t pcm_read_ = 0;                   /* frames already consumed from the front of pcm_ */
    int64_t audio_pts_us_ = 0;              /* presentation time of the first unread frame */
    std::atomic<int64_t> video_end_us_{0};
};

#endif

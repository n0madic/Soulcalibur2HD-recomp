/*
 * See movie.hpp. Copyright (C) the sc2hd authors. SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "movie.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>


namespace {

/* How far ahead of the consumer the worker decodes. */
constexpr size_t kMaxPictures = 10;
constexpr int64_t kMaxAudioAheadUs = 1'500'000;

int64_t to_us(int64_t ticks, uint32_t timescale) { return ticks * 1'000'000 / (timescale ? timescale : 1); }

uint8_t clip8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* YUV 4:2:0 to RGBA; matrix is the VUI matrix_coefficients (1 BT.709, 5 and 6 BT.601, anything else: by picture height). */
void yuv_to_rgba(const uint8_t* y, const uint8_t* u, const uint8_t* v, int w, int h, int matrix, int full_range,
                 uint8_t* dst, int pitch)
{
    if (matrix != 1 && matrix != 5 && matrix != 6) matrix = h >= 720 ? 1 : 6;   /* unspecified: HD is BT.709, SD BT.601 */
    int ky, kr, kgu, kgv, kb;      /* 16.16 fixed point */
    if (matrix == 1) { kr = 117377; kgu = 13962; kgv = 34890; kb = 138412; }   /* 1.793 .213 .533 2.112 */
    else             { kr = 104597; kgu = 25675; kgv = 53279; kb = 132201; }   /* 1.596 .392 .813 2.018 */
    ky = full_range ? 65536 : 76309;                                            /* 1.164 for limited range */
    if (full_range) {
        if (matrix == 1) { kr = 103206; kgu = 12276; kgv = 30679; kb = 121609; }
        else             { kr = 91881;  kgu = 22554; kgv = 46802; kb = 116130; }
    }
    const int yoff = full_range ? 0 : 16;
    for (int j = 0; j < h; j++) {
        const uint8_t* yr = y + (size_t)j * w;
        const uint8_t* ur = u + (size_t)(j >> 1) * (w >> 1);
        const uint8_t* vr = v + (size_t)(j >> 1) * (w >> 1);
        uint8_t* o = dst + (size_t)j * pitch;
        for (int i = 0; i < w; i++) {
            const int c = (yr[i] - yoff) * ky, du = ur[i >> 1] - 128, dv = vr[i >> 1] - 128;
            o[4 * i + 0] = clip8((c + kr * dv + 32768) >> 16);
            o[4 * i + 1] = clip8((c - kgu * du - kgv * dv + 32768) >> 16);
            o[4 * i + 2] = clip8((c + kb * du + 32768) >> 16);
            o[4 * i + 3] = 255;
        }
    }
}

}  // namespace

std::unique_ptr<Movie> Movie::open(const std::string& path, std::string* err)
{
    std::unique_ptr<Movie> m(new Movie());
    auto fail = [&](const char* what) { if (err) *err = what; return std::unique_ptr<Movie>(); };
    m->mp4_ = mp4_open(path.c_str());
    if (!m->mp4_) return fail("cannot open the file as MP4");
    m->vtrack_ = mp4_find(m->mp4_, MP4_VIDEO);
    m->atrack_ = mp4_find(m->mp4_, MP4_AUDIO);
    if (!m->vtrack_ || !m->vtrack_->extra) return fail("no video track");
    m->video_ = h264_open(m->vtrack_->extra, m->vtrack_->extra_size);
    if (!m->video_) return fail("unsupported H.264 stream");
    m->width_ = h264_width(m->video_);
    m->height_ = h264_height(m->video_);
    m->reorder_depth_ = mp4_reorder_depth(m->vtrack_);
    m->max_packet_ = std::max<size_t>(mp4_max_sample(m->vtrack_), m->atrack_ ? mp4_max_sample(m->atrack_) : 0);
    if (m->atrack_ && m->atrack_->extra) {
        m->audio_ = aac_open(m->atrack_->extra, m->atrack_->extra_size);
        if (m->audio_) {
            m->audio_rate_ = aac_rate(m->audio_);
            m->audio_channels_ = aac_channels(m->audio_);
        }
    }
    return m;
}

Movie::~Movie()
{
    {
        std::lock_guard<std::mutex> g(mtx_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    h264_close(video_);
    aac_close(audio_);
    mp4_close(mp4_);
}

void Movie::set_outputs(bool video, bool audio)
{
    video_on_ = video;
    audio_on_ = audio;
}

void Movie::start()
{
    if (!thread_.joinable()) thread_ = std::thread([this] { worker(); });
}

void Movie::push_picture(Picture&& p)
{
    std::unique_lock<std::mutex> lk(mtx_);
    cv_.wait(lk, [&] { return stop_ || pictures_.size() < kMaxPictures; });
    if (stop_) return;
    pictures_.push_back(std::move(p));
}

void Movie::worker()
{
    const mp4_track* vt = video_on_ ? vtrack_ : nullptr;
    const mp4_track* at = has_audio() ? atrack_ : nullptr;
    std::vector<uint8_t> packet(max_packet_);
    std::vector<float> pcm((size_t)AAC_FRAME_SAMPLES * (audio_channels_ ? audio_channels_ : 1));
    std::deque<Picture> pending;          /* decoded, waiting for earlier pictures (display order) */
    uint32_t vi = 0, ai = 0;
    int64_t last_pts = 0;

    auto emit_oldest = [&]() {
        auto it = std::min_element(pending.begin(), pending.end(), [](const Picture& a, const Picture& b) { return a.pts_us < b.pts_us; });
        last_pts = std::max(last_pts, it->pts_us);
        Picture p = std::move(*it);
        pending.erase(it);
        push_picture(std::move(p));
    };

    while (true) {
        const bool more_v = vt && vi < vt->nsamples, more_a = at && ai < at->nsamples;
        if (!more_v && !more_a) break;
        /* Neither track may hold up the other: the consumer can stop taking sound (or pictures) at any time. A track
         * is decoded only while its queue has room; when neither has, wait for the consumer. */
        bool want_video, want_audio;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            auto room = [&](bool& v, bool& a) {
                v = more_v && pictures_.size() < kMaxPictures;
                a = more_a && (int64_t)((pcm_.size() / audio_channels_ - pcm_read_) * 1'000'000LL / audio_rate_) < kMaxAudioAheadUs;
            };
            cv_.wait(lk, [&] { room(want_video, want_audio); return stop_ || want_video || want_audio; });
            if (stop_) return;
        }
        const bool do_video = want_video && (!want_audio || to_us(vt->dts[vi], vt->timescale) <= to_us(at->dts[ai], at->timescale));
        if (do_video) {
            const int n = mp4_read(mp4_, vt, vi, packet.data(), packet.size());
            const int64_t cts = to_us(vt->cts[vi], vt->timescale);
            vi++;
            h264_frame fr;
            /* TODO: a change of the picture size inside the stream is not followed (the game's buffers are sized once). */
            if (n > 0 && h264_decode(video_, packet.data(), (size_t)n, &fr) == 1 && fr.width == width_ && fr.height == height_) {
                Picture p;
                p.pts_us = cts;
                p.matrix = fr.matrix;
                p.full_range = fr.full_range;
                p.yuv.resize((size_t)fr.width * fr.height * 3 / 2);
                uint8_t* o = p.yuv.data();
                for (int j = 0; j < fr.height; j++, o += fr.width) std::memcpy(o, fr.plane[0] + (size_t)j * fr.stride[0], (size_t)fr.width);
                for (int c = 1; c < 3; c++)
                    for (int j = 0; j < fr.height / 2; j++, o += fr.width / 2) std::memcpy(o, fr.plane[c] + (size_t)j * fr.stride[c], (size_t)fr.width / 2);
                pending.push_back(std::move(p));
                while ((int)pending.size() > reorder_depth_) emit_oldest();
            }
        } else {
            const int n = mp4_read(mp4_, at, ai, packet.data(), packet.size());
            const int64_t pts = to_us(at->cts[ai], at->timescale);
            ai++;
            const int frames = n > 0 ? aac_decode(audio_, packet.data(), (size_t)n, pcm.data()) : -1;
            /* the edit list can start the presentation inside the first frame (encoder delay): drop what precedes time zero */
            size_t skip = 0;
            int64_t start_us = pts;
            if (frames > 0 && pts < 0) {
                skip = std::min<size_t>((size_t)frames, (size_t)(-pts * audio_rate_ / 1'000'000));
                start_us = pts + (int64_t)(skip * 1'000'000ULL / audio_rate_);
            }
            if (frames > 0 && (size_t)frames > skip) {
                std::lock_guard<std::mutex> lk(mtx_);
                if (pcm_.size() == pcm_read_ * audio_channels_) audio_pts_us_ = start_us;
                pcm_.insert(pcm_.end(), pcm.begin() + skip * audio_channels_, pcm.begin() + (size_t)frames * audio_channels_);
            }
        }
    }
    while (!pending.empty()) emit_oldest();
    std::lock_guard<std::mutex> g(mtx_);
    video_end_us_ = last_pts;
    eof_ = true;
}

bool Movie::pop_video_rgba(int64_t clock_us, uint8_t* dst, int pitch, int64_t* pts_us)
{
    std::unique_lock<std::mutex> lk(mtx_);
    if (pictures_.empty() || pictures_.front().pts_us > clock_us) return false;
    while (pictures_.size() > 1 && pictures_[1].pts_us <= clock_us) pictures_.pop_front();
    Picture p = std::move(pictures_.front());
    pictures_.pop_front();
    lk.unlock();
    cv_.notify_all();
    const size_t ys = (size_t)width_ * height_;
    if (p.yuv.size() < ys * 3 / 2) return false;   /* cannot happen: only pictures of the stream's size are queued */
    yuv_to_rgba(p.yuv.data(), p.yuv.data() + ys, p.yuv.data() + ys * 5 / 4, width_, height_, p.matrix, p.full_range, dst, pitch);
    if (pts_us) *pts_us = p.pts_us;
    return true;
}

size_t Movie::audio_available()
{
    std::lock_guard<std::mutex> g(mtx_);
    return audio_channels_ ? pcm_.size() / audio_channels_ - pcm_read_ : 0;
}

size_t Movie::read_audio(float* dst, size_t frames, int64_t* pts_us)
{
    std::unique_lock<std::mutex> lk(mtx_);
    if (!audio_channels_) return 0;
    const size_t avail = pcm_.size() / audio_channels_ - pcm_read_;
    const size_t n = std::min(frames, avail);
    if (pts_us) *pts_us = audio_pts_us_;
    std::memcpy(dst, pcm_.data() + pcm_read_ * audio_channels_, n * audio_channels_ * sizeof(float));
    pcm_read_ += n;
    audio_pts_us_ += (int64_t)(n * 1'000'000ULL / audio_rate_);
    if (pcm_read_ > 48000) {       /* compact the consumed front */
        pcm_.erase(pcm_.begin(), pcm_.begin() + pcm_read_ * audio_channels_);
        pcm_read_ = 0;
    }
    lk.unlock();
    cv_.notify_all();
    return n;
}

bool Movie::finished()
{
    std::lock_guard<std::mutex> g(mtx_);
    return eof_ && pictures_.empty() && (!has_audio() || pcm_.size() / audio_channels_ == pcm_read_);
}

bool Movie::audio_finished()
{
    std::lock_guard<std::mutex> g(mtx_);
    return eof_ && (!has_audio() || pcm_.size() / audio_channels_ == pcm_read_);
}

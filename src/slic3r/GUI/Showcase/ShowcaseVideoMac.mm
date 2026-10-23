// Showcase tab, S4 (D5): MP4 on macOS through AVFoundation. AVAssetWriter with H.264 (VideoToolbox,
// in hardware where the Mac has it), fed BGRA pixel buffers from the frames the GUI renders. The app
// ships no ffmpeg on the Mac (decision 2026-10-09), so this is the Mac's video encoder; WebM, GIF and
// WebP need a Homebrew ffmpeg (see Showcase::find_ffmpeg).
//
// Manual reference counting, as the other .mm files of the app.

#include "ShowcaseVideoSinks.hpp"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

namespace Slic3r {
namespace GUI {

namespace {

class AvFoundationSink : public Showcase::FrameSink
{
public:
    AvFoundationSink(std::string path, int width, int height, int fps, int bitrate_kbps)
        : m_path(std::move(path)), m_width(width), m_height(height), m_fps(std::max(1, fps)), m_bitrate(std::max(300, bitrate_kbps))
    {}
    ~AvFoundationSink() override { release(); }

    bool open(std::string &error) override
    {
        @autoreleasepool {
            NSString *file = [NSString stringWithUTF8String:m_path.c_str()];
            if (file == nil) {
                error = "the file name cannot be used";
                return false;
            }
            NSURL *url = [NSURL fileURLWithPath:file];
            [[NSFileManager defaultManager] removeItemAtURL:url error:nil];
            NSError *err = nil;
            m_writer     = [[AVAssetWriter alloc] initWithURL:url fileType:AVFileTypeMPEG4 error:&err];
            if (m_writer == nil) {
                error = std::string("AVAssetWriter: ") + (err != nil ? [[err localizedDescription] UTF8String] : "could not be created");
                return false;
            }
            NSDictionary *compression = @{
                AVVideoAverageBitRateKey : @(double(m_bitrate) * 1000.),
                AVVideoMaxKeyFrameIntervalKey : @(m_fps * 2),
                AVVideoProfileLevelKey : AVVideoProfileLevelH264HighAutoLevel,
                AVVideoAllowFrameReorderingKey : @YES,
            };
            NSDictionary *color = @{
                AVVideoColorPrimariesKey : AVVideoColorPrimaries_ITU_R_709_2,
                AVVideoTransferFunctionKey : AVVideoTransferFunction_ITU_R_709_2,
                AVVideoYCbCrMatrixKey : AVVideoYCbCrMatrix_ITU_R_709_2,
            };
            NSDictionary *settings = @{
                AVVideoCodecKey : AVVideoCodecTypeH264,
                AVVideoWidthKey : @(m_width),
                AVVideoHeightKey : @(m_height),
                AVVideoCompressionPropertiesKey : compression,
                AVVideoColorPropertiesKey : color,
            };
            m_input = [[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeVideo outputSettings:settings];
            m_input.expectsMediaDataInRealTime = NO;
            NSDictionary *attrs = @{
                (id) kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
                (id) kCVPixelBufferWidthKey : @(m_width),
                (id) kCVPixelBufferHeightKey : @(m_height),
            };
            m_adaptor = [[AVAssetWriterInputPixelBufferAdaptor alloc] initWithAssetWriterInput:m_input sourcePixelBufferAttributes:attrs];
            if (![m_writer canAddInput:m_input]) {
                error = "AVAssetWriter does not take an H.264 input of this size";
                return false;
            }
            [m_writer addInput:m_input];
            if (![m_writer startWriting]) {
                NSError *e = m_writer.error;
                error = std::string("AVAssetWriter: ") + (e != nil ? [[e localizedDescription] UTF8String] : "could not start");
                return false;
            }
            [m_writer startSessionAtSourceTime:kCMTimeZero];
        }
        return true;
    }

    bool write(const Showcase::VideoFrame &frame, std::string &error) override
    {
        if (frame.width != m_width || frame.height != m_height || frame.rgba.size() != size_t(m_width) * size_t(m_height) * 4) {
            error = "a frame of the wrong size";
            return false;
        }
        @autoreleasepool {
            // Encoding runs behind: wait for room, without spinning hard.
            const auto start = std::chrono::steady_clock::now();
            while (!m_input.readyForMoreMediaData) {
                if (m_interrupted || m_writer.status == AVAssetWriterStatusFailed) {
                    error = writer_error("the encoder stopped");
                    return false;
                }
                if (std::chrono::steady_clock::now() - start > std::chrono::seconds(60)) {
                    error = "the encoder did not take the frame";
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            CVPixelBufferRef buffer = nullptr;
            CVPixelBufferPoolRef pool = m_adaptor.pixelBufferPool;
            if (pool == nullptr || CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &buffer) != kCVReturnSuccess || buffer == nullptr) {
                if (CVPixelBufferCreate(kCFAllocatorDefault, size_t(m_width), size_t(m_height), kCVPixelFormatType_32BGRA, nullptr, &buffer) !=
                        kCVReturnSuccess || buffer == nullptr) {
                    error = "no pixel buffer";
                    return false;
                }
            }
            CVPixelBufferLockBaseAddress(buffer, 0);
            uint8_t     *base   = static_cast<uint8_t *>(CVPixelBufferGetBaseAddress(buffer));
            const size_t stride = CVPixelBufferGetBytesPerRow(buffer);
            for (int y = 0; y < m_height; ++y) {
                const uint8_t *src = frame.rgba.data() + size_t(y) * size_t(m_width) * 4;
                uint8_t       *dst = base + size_t(y) * stride;
                for (int x = 0; x < m_width; ++x, src += 4, dst += 4) {
                    dst[0] = src[2];
                    dst[1] = src[1];
                    dst[2] = src[0];
                    dst[3] = src[3];
                }
            }
            CVPixelBufferUnlockBaseAddress(buffer, 0);
            const BOOL appended = [m_adaptor appendPixelBuffer:buffer withPresentationTime:CMTimeMake(frame.index, int32_t(m_fps))];
            CVPixelBufferRelease(buffer);
            if (!appended) {
                error = writer_error("the frame was refused");
                return false;
            }
        }
        return true;
    }

    bool finish(std::string &error) override
    {
        @autoreleasepool {
            [m_input markAsFinished];
            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            [m_writer finishWritingWithCompletionHandler:^{
                dispatch_semaphore_signal(done);
            }];
            const long timed_out = dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, int64_t(600) * NSEC_PER_SEC));
            dispatch_release(done);
            if (timed_out != 0 || m_writer.status != AVAssetWriterStatusCompleted) {
                error = writer_error("the video could not be completed");
                return false;
            }
        }
        BOOST_LOG_TRIVIAL(info) << "Showcase video: AVFoundation wrote " << m_path;
        return true;
    }

    void interrupt() override { m_interrupted = true; }

    void abort() override
    {
        @autoreleasepool {
            if (m_writer != nil && m_writer.status == AVAssetWriterStatusWriting)
                [m_writer cancelWriting];
            NSString *file = [NSString stringWithUTF8String:m_path.c_str()];
            if (file != nil)
                [[NSFileManager defaultManager] removeItemAtPath:file error:nil];
        }
    }

    std::string description() const override { return "AVFoundation H.264"; }

private:
    std::string writer_error(const char *fallback) const
    {
        NSError *e = m_writer != nil ? m_writer.error : nil;
        return e != nil ? std::string("AVFoundation: ") + [[e localizedDescription] UTF8String] : std::string(fallback);
    }

    void release()
    {
        [m_adaptor release];
        [m_input release];
        [m_writer release];
        m_adaptor = nil;
        m_input   = nil;
        m_writer  = nil;
    }

    std::string                           m_path;
    int                                   m_width   = 0;
    int                                   m_height  = 0;
    int                                   m_fps     = 30;
    int                                   m_bitrate = 12000;
    std::atomic<bool>                     m_interrupted{ false };
    AVAssetWriter                        *m_writer  = nil;
    AVAssetWriterInput                   *m_input   = nil;
    AVAssetWriterInputPixelBufferAdaptor *m_adaptor = nil;
};

} // namespace

bool showcase_has_native_mp4() { return true; }

std::unique_ptr<Showcase::FrameSink> showcase_make_native_mp4_sink(const std::string &path, int width, int height, int fps, int bitrate_kbps)
{
    return std::make_unique<AvFoundationSink>(path, width, height, fps, bitrate_kbps);
}

} // namespace GUI
} // namespace Slic3r

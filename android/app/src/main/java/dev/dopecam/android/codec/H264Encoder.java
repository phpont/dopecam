package dev.dopecam.android.codec;

import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.os.Bundle;
import android.os.Handler;
import android.util.Size;
import android.view.Surface;

import java.io.IOException;
import java.nio.ByteBuffer;

import dev.dopecam.android.model.Preset;
import dev.dopecam.android.net.RtpH264Sender;

public final class H264Encoder {
    private static final String MIME = MediaFormat.MIMETYPE_VIDEO_AVC;

    private final Handler callbackHandler;
    private final RtpH264Sender sender;
    private final Size size;
    private final Preset preset;

    private MediaCodec codec;
    private Surface inputSurface;
    private volatile boolean active;

    public H264Encoder(Handler callbackHandler, RtpH264Sender sender, Size size, Preset preset) {
        this.callbackHandler = callbackHandler;
        this.sender = sender;
        this.size = size;
        this.preset = preset;
    }

    public Surface start() throws Exception {
        MediaCodecInfo codecInfo = findHardwareEncoder(size, preset.fps);
        if (codecInfo == null) {
            throw new IllegalStateException("No hardware H.264 encoder supports " + size + "@" + preset.fps);
        }

        codec = MediaCodec.createByCodecName(codecInfo.getName());
        active = true;
        codec.setCallback(new MediaCodec.Callback() {
            @Override
            public void onInputBufferAvailable(MediaCodec mediaCodec, int index) {
                // Surface-input encoder: input buffers are not used.
            }

            @Override
            public void onOutputBufferAvailable(MediaCodec mediaCodec, int index, MediaCodec.BufferInfo info) {
                if (!active) {
                    safeRelease(mediaCodec, index);
                    return;
                }
                ByteBuffer buffer = mediaCodec.getOutputBuffer(index);
                if (buffer == null || info.size <= 0) {
                    safeRelease(mediaCodec, index);
                    return;
                }

                ByteBuffer view = buffer.duplicate();
                view.position(info.offset);
                view.limit(info.offset + info.size);
                view = view.slice();

                if ((info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) {
                    sender.updateCodecConfig(view);
                } else {
                    boolean keyFrame = (info.flags & MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0;
                    sender.sendAccessUnit(view, info.presentationTimeUs, keyFrame);
                }
                safeRelease(mediaCodec, index);
            }

            @Override
            public void onError(MediaCodec mediaCodec, MediaCodec.CodecException e) {
                active = false;
            }

            @Override
            public void onOutputFormatChanged(MediaCodec mediaCodec, MediaFormat format) {
                ByteBuffer csd0 = format.getByteBuffer("csd-0");
                ByteBuffer csd1 = format.getByteBuffer("csd-1");
                sender.updateCodecConfig(csd0, csd1);
            }
        }, callbackHandler);

        MediaFormat format = MediaFormat.createVideoFormat(MIME, size.getWidth(), size.getHeight());
        format.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
        format.setInteger(MediaFormat.KEY_BIT_RATE, preset.bitrate);
        format.setInteger(MediaFormat.KEY_FRAME_RATE, preset.fps);
        format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 2);
        format.setInteger(MediaFormat.KEY_PRIORITY, 0);
        format.setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0);

        MediaCodecInfo.CodecCapabilities caps = codecInfo.getCapabilitiesForType(MIME);
        MediaCodecInfo.EncoderCapabilities encoderCaps = caps.getEncoderCapabilities();
        if (encoderCaps != null && encoderCaps.isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR)) {
            format.setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR);
        }

        codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
        inputSurface = codec.createInputSurface();
        codec.start();
        return inputSurface;
    }

    public void requestIdr() {
        MediaCodec current = codec;
        if (!active || current == null) {
            return;
        }
        try {
            Bundle bundle = new Bundle();
            bundle.putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0);
            current.setParameters(bundle);
        } catch (IllegalStateException ignored) {
        }
    }

    public void stop() {
        active = false;
        MediaCodec current = codec;
        codec = null;
        if (current != null) {
            try {
                current.stop();
            } catch (Exception ignored) {
            }
            try {
                current.release();
            } catch (Exception ignored) {
            }
        }
        if (inputSurface != null) {
            inputSurface.release();
            inputSurface = null;
        }
    }

    private static void safeRelease(MediaCodec codec, int index) {
        try {
            codec.releaseOutputBuffer(index, false);
        } catch (Exception ignored) {
        }
    }

    private static MediaCodecInfo findHardwareEncoder(Size size, int fps) throws IOException {
        MediaCodecList list = new MediaCodecList(MediaCodecList.ALL_CODECS);
        for (MediaCodecInfo info : list.getCodecInfos()) {
            if (!info.isEncoder() || !info.isHardwareAccelerated()) {
                continue;
            }
            boolean supportsMime = false;
            for (String type : info.getSupportedTypes()) {
                if (MIME.equalsIgnoreCase(type)) {
                    supportsMime = true;
                    break;
                }
            }
            if (!supportsMime) {
                continue;
            }

            try {
                MediaCodecInfo.VideoCapabilities vc = info.getCapabilitiesForType(MIME).getVideoCapabilities();
                if (vc != null
                        && vc.isSizeSupported(size.getWidth(), size.getHeight())
                        && vc.areSizeAndRateSupported(size.getWidth(), size.getHeight(), fps)) {
                    return info;
                }
            } catch (IllegalArgumentException ignored) {
            }
        }
        return null;
    }
}

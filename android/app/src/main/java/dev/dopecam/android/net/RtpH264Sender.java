package dev.dopecam.android.net;

import java.net.InetSocketAddress;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.DatagramChannel;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ThreadLocalRandom;

public final class RtpH264Sender implements AutoCloseable {
    public static final int PAYLOAD_TYPE = 96;
    public static final int MAX_DATAGRAM = 1200;

    private static final int RTP_HEADER = 12;
    private static final int MAX_SINGLE_NAL = MAX_DATAGRAM - RTP_HEADER;
    private static final int MAX_FU_PAYLOAD = MAX_DATAGRAM - RTP_HEADER - 2;

    private final DatagramChannel channel;
    private final ByteBuffer header = ByteBuffer.allocateDirect(RTP_HEADER + 2).order(ByteOrder.BIG_ENDIAN);
    private final ByteBuffer[] gather = new ByteBuffer[2];
    private final int ssrc = ThreadLocalRandom.current().nextInt();
    private final List<byte[]> codecConfig = new ArrayList<>();

    private int sequence = ThreadLocalRandom.current().nextInt(0, 65536);

    public RtpH264Sender(InetSocketAddress target) throws Exception {
        channel = DatagramChannel.open();
        channel.configureBlocking(false);
        channel.connect(target);
    }

    public synchronized void updateCodecConfig(ByteBuffer... buffers) {
        codecConfig.clear();
        if (buffers == null) {
            return;
        }
        for (ByteBuffer buffer : buffers) {
            if (buffer == null) {
                continue;
            }
            for (ByteBuffer nal : splitNalUnits(buffer.duplicate())) {
                if (!nal.hasRemaining()) {
                    continue;
                }
                byte[] copy = new byte[nal.remaining()];
                nal.get(copy);
                codecConfig.add(copy);
            }
        }
    }

    public synchronized void sendAccessUnit(ByteBuffer accessUnit, long ptsUs, boolean keyFrame) {
        try {
            List<ByteBuffer> nals = splitNalUnits(accessUnit.duplicate());
            if (nals.isEmpty()) {
                return;
            }

            int timestamp = (int) ((ptsUs * 90L / 1000L) & 0xffffffffL);
            if (keyFrame) {
                for (byte[] configNal : codecConfig) {
                    sendNal(ByteBuffer.wrap(configNal), timestamp, false);
                }
            }

            for (int i = 0; i < nals.size(); i++) {
                sendNal(nals.get(i), timestamp, i == nals.size() - 1);
            }
        } catch (Exception ignored) {
            // UDP is intentionally lossy. The PC requests a new IDR when needed.
        }
    }

    private void sendNal(ByteBuffer nal, int timestamp, boolean markerOnLastPacket) throws Exception {
        if (nal.remaining() <= MAX_SINGLE_NAL) {
            beginHeader(timestamp, markerOnLastPacket);
            writeGathered(nal.duplicate());
            return;
        }

        ByteBuffer src = nal.duplicate();
        int nalHeader = src.get() & 0xff;
        int fuIndicator = (nalHeader & 0xe0) | 28;
        int nalType = nalHeader & 0x1f;
        boolean first = true;

        while (src.hasRemaining()) {
            int chunk = Math.min(src.remaining(), MAX_FU_PAYLOAD);
            boolean last = chunk == src.remaining();

            beginHeader(timestamp, markerOnLastPacket && last);
            header.put((byte) fuIndicator);
            int fuHeader = nalType;
            if (first) {
                fuHeader |= 0x80;
            }
            if (last) {
                fuHeader |= 0x40;
            }
            header.put((byte) fuHeader);

            ByteBuffer part = src.duplicate();
            part.limit(src.position() + chunk);
            writeGathered(part);
            src.position(src.position() + chunk);
            first = false;
        }
    }

    private void beginHeader(int timestamp, boolean marker) {
        header.clear();
        header.put((byte) 0x80);
        header.put((byte) ((marker ? 0x80 : 0x00) | PAYLOAD_TYPE));
        header.putShort((short) (sequence++ & 0xffff));
        header.putInt(timestamp);
        header.putInt(ssrc);
    }

    private void writeGathered(ByteBuffer payload) throws Exception {
        header.flip();
        gather[0] = header;
        gather[1] = payload;
        channel.write(gather, 0, 2);
    }

    private static List<ByteBuffer> splitNalUnits(ByteBuffer input) {
        ByteBuffer buffer = input.slice();
        List<ByteBuffer> result = new ArrayList<>();
        int limit = buffer.limit();
        int firstStart = findStartCode(buffer, 0);

        if (firstStart >= 0) {
            int cursor = firstStart;
            while (cursor >= 0 && cursor < limit) {
                int prefix = startCodeLength(buffer, cursor);
                int nalStart = cursor + prefix;
                int next = findStartCode(buffer, nalStart);
                int nalEnd = next >= 0 ? next : limit;
                if (nalEnd > nalStart) {
                    result.add(slice(buffer, nalStart, nalEnd));
                }
                cursor = next;
            }
            return result;
        }

        int cursor = 0;
        boolean avcc = true;
        while (cursor + 4 <= limit) {
            long len = ((long) (buffer.get(cursor) & 0xff) << 24)
                    | ((long) (buffer.get(cursor + 1) & 0xff) << 16)
                    | ((long) (buffer.get(cursor + 2) & 0xff) << 8)
                    | (long) (buffer.get(cursor + 3) & 0xff);
            cursor += 4;
            if (len <= 0 || len > Integer.MAX_VALUE || cursor + len > limit) {
                avcc = false;
                break;
            }
            result.add(slice(buffer, cursor, cursor + (int) len));
            cursor += (int) len;
        }

        if (avcc && cursor == limit && !result.isEmpty()) {
            return result;
        }

        result.clear();
        if (limit > 0) {
            result.add(slice(buffer, 0, limit));
        }
        return result;
    }

    private static int findStartCode(ByteBuffer buffer, int from) {
        for (int i = Math.max(0, from); i + 3 <= buffer.limit(); i++) {
            if ((buffer.get(i) & 0xff) != 0 || (buffer.get(i + 1) & 0xff) != 0) {
                continue;
            }
            if ((buffer.get(i + 2) & 0xff) == 1) {
                return i;
            }
            if (i + 3 < buffer.limit()
                    && (buffer.get(i + 2) & 0xff) == 0
                    && (buffer.get(i + 3) & 0xff) == 1) {
                return i;
            }
        }
        return -1;
    }

    private static int startCodeLength(ByteBuffer buffer, int index) {
        return (index + 3 < buffer.limit()
                && (buffer.get(index + 2) & 0xff) == 0
                && (buffer.get(index + 3) & 0xff) == 1) ? 4 : 3;
    }

    private static ByteBuffer slice(ByteBuffer source, int start, int end) {
        ByteBuffer dup = source.duplicate();
        dup.position(start);
        dup.limit(end);
        return dup.slice();
    }

    @Override
    public synchronized void close() {
        try {
            channel.close();
        } catch (Exception ignored) {
        }
    }
}

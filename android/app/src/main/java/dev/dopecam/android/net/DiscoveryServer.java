package dev.dopecam.android.net;

import android.os.Build;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.atomic.AtomicBoolean;

public final class DiscoveryServer implements AutoCloseable {
    public static final int PORT = 39510;
    public static final int CONTROL_PORT = 39511;
    private static final String DISCOVER = "DOPECAM_DISCOVER_V1";

    private final AtomicBoolean running = new AtomicBoolean(false);
    private DatagramSocket socket;
    private Thread thread;

    public void start() throws Exception {
        if (!running.compareAndSet(false, true)) {
            return;
        }
        socket = new DatagramSocket(null);
        socket.setReuseAddress(true);
        socket.bind(new InetSocketAddress(PORT));
        thread = new Thread(this::loop, "DopeCam-Discovery");
        thread.start();
    }

    private void loop() {
        byte[] buffer = new byte[256];
        while (running.get()) {
            try {
                DatagramPacket packet = new DatagramPacket(buffer, buffer.length);
                socket.receive(packet);
                String request = new String(packet.getData(), packet.getOffset(), packet.getLength(), StandardCharsets.UTF_8);
                if (!DISCOVER.equals(request)) {
                    continue;
                }
                String model = sanitize(Build.MODEL);
                String response = "DOPECAM_HERE\t" + model + "\t" + CONTROL_PORT + "\t1";
                byte[] data = response.getBytes(StandardCharsets.UTF_8);
                InetAddress address = packet.getAddress();
                DatagramPacket reply = new DatagramPacket(data, data.length, address, packet.getPort());
                socket.send(reply);
            } catch (Exception e) {
                if (running.get()) {
                    // Continue listening after transient network errors.
                }
            }
        }
    }

    private static String sanitize(String value) {
        if (value == null || value.isBlank()) {
            return "Android";
        }
        return value.replace('\t', ' ').replace('\r', ' ').replace('\n', ' ');
    }

    @Override
    public void close() {
        running.set(false);
        if (socket != null) {
            socket.close();
        }
        if (thread != null) {
            try {
                thread.join(1000);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        }
    }
}

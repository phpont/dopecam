package dev.dopecam.android.net;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;

import dev.dopecam.android.camera.CameraCatalog;
import dev.dopecam.android.camera.CameraController;
import dev.dopecam.android.model.CameraInfo;
import dev.dopecam.android.model.Preset;
import dev.dopecam.android.model.StreamConfig;

public final class ControlServer implements AutoCloseable {
    private final CameraCatalog catalog;
    private final CameraController controller;
    private final AtomicBoolean running = new AtomicBoolean(false);

    private ServerSocket serverSocket;
    private Socket clientSocket;
    private Thread thread;

    public ControlServer(CameraCatalog catalog, CameraController controller) {
        this.catalog = catalog;
        this.controller = controller;
    }

    public void start() throws Exception {
        if (!running.compareAndSet(false, true)) {
            return;
        }
        serverSocket = new ServerSocket(DiscoveryServer.CONTROL_PORT);
        serverSocket.setReuseAddress(true);
        thread = new Thread(this::acceptLoop, "DopeCam-Control");
        thread.start();
    }

    private void acceptLoop() {
        while (running.get()) {
            try (Socket socket = serverSocket.accept()) {
                synchronized (this) {
                    clientSocket = socket;
                }
                socket.setTcpNoDelay(true);
                handleClient(socket);
            } catch (Exception e) {
                if (running.get()) {
                    controller.stopStream();
                }
            } finally {
                synchronized (this) {
                    clientSocket = null;
                }
                controller.stopStream();
            }
        }
    }

    private void handleClient(Socket socket) throws Exception {
        BufferedReader in = new BufferedReader(new InputStreamReader(socket.getInputStream(), StandardCharsets.UTF_8));
        BufferedWriter out = new BufferedWriter(new OutputStreamWriter(socket.getOutputStream(), StandardCharsets.UTF_8));

        writeLine(out, "HELLO\tDOPECAM\t1");
        String line;
        while (running.get() && (line = in.readLine()) != null) {
            try {
                handleCommand(line, socket, out);
            } catch (Exception e) {
                writeLine(out, "ERR\t" + sanitize(e.getMessage()));
            }
        }
    }

    private void handleCommand(String line, Socket socket, BufferedWriter out) throws Exception {
        String[] parts = line.split("\\t", -1);
        if (parts.length == 0) {
            return;
        }
        String command = parts[0].toUpperCase(Locale.ROOT);
        switch (command) {
            case "PING":
                writeLine(out, "PONG");
                break;
            case "CAPS":
                sendCapabilities(out);
                break;
            case "START": {
                if (parts.length != 5) {
                    throw new IllegalArgumentException("START expects cameraId, preset, udpPort, zoom");
                }
                String cameraId = parts[1];
                Preset preset = Preset.fromWire(parts[2]);
                int udpPort = Integer.parseInt(parts[3]);
                if (udpPort < 1024 || udpPort > 65535) {
                    throw new IllegalArgumentException("Invalid UDP port");
                }
                float zoom = Float.parseFloat(parts[4]);
                StreamConfig config = new StreamConfig(cameraId, preset, udpPort, zoom);
                CameraController.StartResult result = controller.startStream(config, socket.getInetAddress());
                writeLine(out, String.format(Locale.US, "OK\tSTART\t%d\t%d\t%d\t%d",
                        result.width, result.height, result.fps, result.bitrate));
                break;
            }
            case "STOP":
                controller.stopStream();
                writeLine(out, "OK\tSTOP");
                break;
            case "SET_ZOOM":
                if (parts.length != 2) {
                    throw new IllegalArgumentException("SET_ZOOM expects a ratio");
                }
                controller.setZoom(Float.parseFloat(parts[1]));
                writeLine(out, "OK\tSET_ZOOM");
                break;
            case "REQUEST_IDR":
                controller.requestIdr();
                writeLine(out, "OK\tREQUEST_IDR");
                break;
            default:
                throw new IllegalArgumentException("Unknown command: " + command);
        }
    }

    private void sendCapabilities(BufferedWriter out) throws Exception {
        List<CameraInfo> cameras = catalog.list();
        writeLine(out, "CAPS_BEGIN\t1");
        for (CameraInfo camera : cameras) {
            writeLine(out, String.format(Locale.US, "CAMERA\t%s\t%d\t%.4f\t%.4f\t%d",
                    sanitize(camera.id), camera.lensFacing, camera.zoomMin, camera.zoomMax, camera.logical ? 1 : 0));
        }
        writeLine(out, "PRESET\tbudget\t1280\t720\t30\t2500000");
        writeLine(out, "PRESET\tnormal\t1920\t1080\t30\t6000000");
        writeLine(out, "PRESET\tquality\t1920\t1080\t30\t12000000");
        writeLine(out, "CAPS_END");
    }

    private static void writeLine(BufferedWriter out, String value) throws Exception {
        out.write(value);
        out.write('\n');
        out.flush();
    }

    private static String sanitize(String value) {
        if (value == null || value.isBlank()) {
            return "unknown";
        }
        return value.replace('\t', ' ').replace('\r', ' ').replace('\n', ' ');
    }

    @Override
    public void close() {
        running.set(false);
        synchronized (this) {
            if (clientSocket != null) {
                try {
                    clientSocket.close();
                } catch (Exception ignored) {
                }
            }
        }
        if (serverSocket != null) {
            try {
                serverSocket.close();
            } catch (Exception ignored) {
            }
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

package com.trebledot.stone.cit;

import android.Manifest;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothManager;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanResult;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.graphics.SurfaceTexture;
import android.hardware.Camera;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioRecord;
import android.media.AudioTrack;
import android.media.MediaRecorder;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.View;
import android.widget.Button;
import java.io.IOException;
import java.util.Locale;

import static com.trebledot.stone.cit.CitResult.State.*;

@SuppressWarnings("deprecation")
final class HardwareChecks {
    private static final int PERMISSION_REQUEST = 1;
    private final CitActivity activity;
    private final Handler main = new Handler(Looper.getMainLooper());
    private Operation active;
    private PermissionRequest permission;
    private Button audible;
    private Button inaudible;

    HardwareChecks(CitActivity activity) {
        this.activity = activity;
    }

    void addControls() {
        activity.heading("Hardware checks", 20);
        add("Camera", "camera", "Receive a preview frame", this::camera, Manifest.permission.CAMERA);
        add("Microphone", "microphone", "Speak while recording a two-second sample", this::microphone,
                Manifest.permission.RECORD_AUDIO);
        CitResult speaker = add("Speaker", "speaker", "Play a short tone", this::speaker);
        audible = activity.addButton("I heard the tone", () -> confirmSpeaker(speaker, true));
        inaudible = activity.addButton("I could not hear the tone", () -> confirmSpeaker(speaker, false));
        showSpeakerConfirmation(false);
        add("Wi-Fi", "wifi", "Scan for nearby networks", this::wifi,
                Manifest.permission.ACCESS_COARSE_LOCATION, Manifest.permission.ACCESS_FINE_LOCATION);
        String[] bluetoothPermissions = Build.VERSION.SDK_INT >= 31
                ? new String[] {Manifest.permission.BLUETOOTH_CONNECT, Manifest.permission.BLUETOOTH_SCAN}
                : new String[] {Manifest.permission.ACCESS_FINE_LOCATION};
        add("Bluetooth LE", "bluetooth", "Listen for advertisements", this::bluetooth, bluetoothPermissions);
        add("Accelerometer", "accelerometer", "Test motion readings", this::accelerometer);
    }

    private CitResult add(String title, String id, String button, Check check, String... permissions) {
        CitResult result = activity.addResult(title, id);
        result.set(READY, "Run the test to collect a result");
        activity.addButton(button, () -> request(result, check, permissions));
        return result;
    }

    private void request(CitResult result, Check check, String[] permissions) {
        if (permission != null || (active != null && active.result == result)) return;
        pause();
        PermissionRequest request = new PermissionRequest(result, check, permissions);
        try {
            if (hasPermissions(permissions)) start(request);
            else {
                permission = request;
                activity.requestPermissions(permissions, PERMISSION_REQUEST);
            }
        } catch (RuntimeException | LinkageError error) {
            permission = null;
            result.error(error);
        }
    }

    void permissionsReturned(int code) {
        if (code != PERMISSION_REQUEST || permission == null) return;
        PermissionRequest request = permission;
        permission = null;
        if (hasPermissions(request.permissions)) start(request);
        else request.result.set(BLOCKED, "Android did not grant the required permission");
    }

    private boolean hasPermissions(String[] permissions) {
        for (String name : permissions) {
            if (activity.checkSelfPermission(name) != PackageManager.PERMISSION_GRANTED) return false;
        }
        return true;
    }

    private void start(PermissionRequest request) {
        Operation operation = new Operation(request.result);
        active = operation;
        operation.result.set(RUNNING, "Waiting for the Android API");
        try {
            request.check.run(operation);
        } catch (IOException | RuntimeException | LinkageError error) {
            operation.error(error);
        }
    }

    void pause() {
        if (active != null) active.finish(CANCELLED, "Test interrupted");
    }

    private void camera(Operation operation) throws IOException {
        int count = Camera.getNumberOfCameras();
        if (count == 0) {
            operation.finish(UNAVAILABLE, "Android exposed zero cameras");
            return;
        }
        Camera camera = Camera.open(0);
        operation.cleanup = camera::release;
        SurfaceTexture texture = new SurfaceTexture(0);
        operation.cleanup = () -> {
            try { camera.release(); }
            finally { texture.release(); }
        };
        Camera.Size size = camera.getParameters().getPreviewSize();
        camera.setPreviewTexture(texture);
        camera.setOneShotPreviewCallback((data, source) -> main.post(() -> {
            if (data != null && data.length >= (long) size.width * size.height) {
                operation.finish(PASS, "Camera 0 delivered a " + size.width + " × " + size.height
                        + " preview frame. Android exposed " + count + " camera(s).");
            } else operation.finish(FAIL, "Camera delivered an incomplete preview frame");
        }));
        operation.timeout(5000, FAIL, "Camera opened but delivered no preview frame");
        camera.startPreview();
    }

    private void microphone(Operation operation) {
        operation.result.set(RUNNING, "Speak now. Recording for two seconds.");
        operation.worker(() -> record(operation));
    }

    private void record(Operation operation) throws InterruptedException {
        int rate = 16000;
        int minimum = AudioRecord.getMinBufferSize(rate, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT);
        if (minimum <= 0) {
            operation.post(UNAVAILABLE, "AudioRecord rejected mono PCM: " + minimum);
            return;
        }
        AudioRecord record = new AudioRecord(MediaRecorder.AudioSource.MIC, rate, AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT, Math.max(minimum, 4096));
        try {
            if (record.getState() != AudioRecord.STATE_INITIALIZED) {
                operation.post(UNAVAILABLE, "AudioRecord could not open an input");
                return;
            }
            record.startRecording();
            short[] buffer = new short[1024];
            int frames = 0;
            int low = Short.MAX_VALUE;
            int high = Short.MIN_VALUE;
            long end = SystemClock.elapsedRealtime() + 2000;
            while (SystemClock.elapsedRealtime() < end) {
                int read = record.read(buffer, 0, buffer.length, AudioRecord.READ_NON_BLOCKING);
                if (read < 0) {
                    operation.post(FAIL, "AudioRecord.read returned " + read);
                    return;
                }
                for (int i = 0; i < read; i++) {
                    low = Math.min(low, buffer[i]);
                    high = Math.max(high, buffer[i]);
                }
                frames += read;
                Thread.sleep(20);
            }
            if (frames == 0) operation.post(FAIL, "AudioRecord returned no PCM frames");
            else if (high - low < 64) operation.post(UNAVAILABLE, frames + " PCM frames with no clear signal. Retry while speaking.");
            else operation.post(PASS, frames + " PCM frames with peak-to-peak amplitude " + (high - low));
        } finally {
            record.release();
        }
    }

    private void speaker(Operation operation) {
        showSpeakerConfirmation(false);
        operation.worker(() -> playTone(operation));
    }

    private void playTone(Operation operation) throws InterruptedException {
        int rate = 44100;
        int minimum = AudioTrack.getMinBufferSize(rate, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT);
        if (minimum <= 0) {
            operation.post(UNAVAILABLE, "AudioTrack rejected mono PCM: " + minimum);
            return;
        }
        AudioTrack track = new AudioTrack(AudioManager.STREAM_MUSIC, rate, AudioFormat.CHANNEL_OUT_MONO,
                AudioFormat.ENCODING_PCM_16BIT, minimum, AudioTrack.MODE_STREAM);
        try {
            if (track.getState() != AudioTrack.STATE_INITIALIZED) {
                operation.post(UNAVAILABLE, "AudioTrack could not open an output");
                return;
            }
            short[] tone = new short[rate / 2];
            for (int i = 0; i < tone.length; i++) {
                double envelope = Math.min(1, Math.min(i, tone.length - 1 - i) / 512.0);
                tone[i] = (short) (4000 * envelope * Math.sin(2 * Math.PI * 440 * i / rate));
            }
            track.play();
            int submitted = 0;
            int played = 0;
            long end = SystemClock.elapsedRealtime() + 3000;
            while (played < tone.length && SystemClock.elapsedRealtime() < end) {
                if (submitted < tone.length) {
                    int written = track.write(tone, submitted, tone.length - submitted, AudioTrack.WRITE_NON_BLOCKING);
                    if (written < 0) {
                        operation.post(FAIL, "AudioTrack.write returned " + written);
                        return;
                    }
                    submitted += written;
                }
                played = track.getPlaybackHeadPosition();
                Thread.sleep(20);
            }
            if (played < tone.length) operation.post(FAIL, "Playback reached " + played + " of " + tone.length + " frames");
            else main.post(() -> {
                if (active == operation) {
                    operation.finish(CONFIRM, "AudioTrack played the PCM frames. Did you hear the tone?");
                    showSpeakerConfirmation(true);
                }
            });
        } finally {
            track.release();
        }
    }

    private void confirmSpeaker(CitResult result, boolean heard) {
        result.set(heard ? PASS : FAIL, heard ? "Tone heard by the tester" : "Tester did not hear the tone");
        showSpeakerConfirmation(false);
    }

    private void showSpeakerConfirmation(boolean show) {
        audible.setVisibility(show ? View.VISIBLE : View.GONE);
        inaudible.setVisibility(show ? View.VISIBLE : View.GONE);
    }

    private void wifi(Operation operation) {
        WifiManager wifi = (WifiManager) activity.getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        if (wifi == null || !wifi.isWifiEnabled()) {
            operation.finish(UNAVAILABLE, "Wi-Fi service is missing or Wi-Fi is disabled");
            return;
        }
        ConnectivityManager connectivity = (ConnectivityManager) activity.getSystemService(Context.CONNECTIVITY_SERVICE);
        Network network = connectivity == null ? null : connectivity.getActiveNetwork();
        NetworkCapabilities caps = network == null ? null : connectivity.getNetworkCapabilities(network);
        String transport = caps == null ? "Default transport unknown"
                : caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) ? "Default transport Wi-Fi" : "Default transport other";
        BroadcastReceiver receiver = new BroadcastReceiver() {
            @Override public void onReceive(Context context, Intent intent) {
                if (active != operation || !WifiManager.SCAN_RESULTS_AVAILABLE_ACTION.equals(intent.getAction())) return;
                try {
                    if (intent.getBooleanExtra(WifiManager.EXTRA_RESULTS_UPDATED, false)) {
                        operation.finish(PASS, "Fresh scan returned " + wifi.getScanResults().size() + " network(s). " + transport);
                    } else operation.finish(FAIL, "Scan completed without refreshed results. " + transport);
                } catch (RuntimeException | LinkageError error) {
                    operation.error(error);
                }
            }
        };
        activity.registerReceiver(receiver, new IntentFilter(WifiManager.SCAN_RESULTS_AVAILABLE_ACTION));
        operation.cleanup = () -> activity.unregisterReceiver(receiver);
        if (!wifi.startScan()) {
            operation.finish(UNAVAILABLE, "Android rejected the scan request. " + transport);
            return;
        }
        operation.timeout(10000, FAIL, "No Wi-Fi scan completion received. " + transport);
    }

    private void bluetooth(Operation operation) {
        BluetoothManager manager = (BluetoothManager) activity.getSystemService(Context.BLUETOOTH_SERVICE);
        BluetoothAdapter adapter = manager == null ? null : manager.getAdapter();
        if (adapter == null || !adapter.isEnabled()) {
            operation.finish(UNAVAILABLE, "Bluetooth adapter is missing or disabled");
            return;
        }
        BluetoothLeScanner scanner = adapter.getBluetoothLeScanner();
        if (scanner == null) {
            operation.finish(UNAVAILABLE, "Android did not expose a BLE scanner");
            return;
        }
        ScanCallback callback = new ScanCallback() {
            @Override public void onScanResult(int type, ScanResult result) {
                operation.post(PASS, "Received a BLE advertisement");
            }

            @Override public void onScanFailed(int error) {
                operation.post(FAIL, "BLE scan error " + error);
            }
        };
        operation.cleanup = () -> scanner.stopScan(callback);
        operation.timeout(8000, UNAVAILABLE, "No BLE advertisement received. Place an advertising device nearby.");
        scanner.startScan(callback);
    }

    private void accelerometer(Operation operation) {
        SensorManager manager = (SensorManager) activity.getSystemService(Context.SENSOR_SERVICE);
        Sensor sensor = manager == null ? null : manager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
        if (sensor == null) {
            operation.finish(UNAVAILABLE, "Android exposed no accelerometer");
            return;
        }
        operation.result.set(RUNNING, "Tilt the device to change the accelerometer reading.");
        SensorEventListener listener = new SensorEventListener() {
            private float[] first;

            @Override public void onSensorChanged(SensorEvent event) {
                if (active != operation) return;
                float[] values = event.values;
                if (values.length < 3) {
                    operation.finish(FAIL, "Received an incomplete accelerometer event");
                } else if (first == null) {
                    first = values.clone();
                    operation.timeout(6000, UNAVAILABLE, "Android delivered events with no clear change in motion");
                } else {
                    float x = values[0] - first[0];
                    float y = values[1] - first[1];
                    float z = values[2] - first[2];
                    if (x * x + y * y + z * z >= 1) operation.finish(PASS, String.format(Locale.ROOT,
                            "Motion changed readings to x=%.2f, y=%.2f, z=%.2f m/s²", values[0], values[1], values[2]));
                }
            }

            @Override public void onAccuracyChanged(Sensor source, int accuracy) {}
        };
        operation.cleanup = () -> manager.unregisterListener(listener);
        operation.timeout(4000, FAIL, "No accelerometer event received");
        if (!manager.registerListener(listener, sensor, SensorManager.SENSOR_DELAY_UI, main)) {
            operation.finish(UNAVAILABLE, "Android rejected the accelerometer listener");
        }
    }

    private interface Check { void run(Operation operation) throws IOException; }
    private interface Worker { void run() throws InterruptedException; }

    private static final class PermissionRequest {
        final CitResult result;
        final Check check;
        final String[] permissions;

        PermissionRequest(CitResult result, Check check, String[] permissions) {
            this.result = result;
            this.check = check;
            this.permissions = permissions;
        }
    }

    private final class Operation {
        final CitResult result;
        Runnable cleanup;
        Runnable deadline;

        Operation(CitResult result) { this.result = result; }

        void worker(Worker work) {
            Thread thread = new Thread(() -> {
                try {
                    work.run();
                } catch (InterruptedException ignored) {
                    Thread.currentThread().interrupt();
                } catch (RuntimeException | LinkageError error) {
                    main.post(() -> error(error));
                }
            }, "Stone CIT audio");
            thread.setDaemon(true);
            cleanup = thread::interrupt;
            thread.start();
        }

        void timeout(long millis, CitResult.State state, String detail) {
            if (deadline != null) main.removeCallbacks(deadline);
            deadline = () -> finish(state, detail);
            main.postDelayed(deadline, millis);
        }

        void post(CitResult.State state, String detail) {
            main.post(() -> finish(state, detail));
        }

        void error(Throwable error) {
            if (end()) result.error(error);
        }

        void finish(CitResult.State state, String detail) {
            if (end()) result.set(state, detail);
        }

        private boolean end() {
            if (active != this) return false;
            active = null;
            if (deadline != null) main.removeCallbacks(deadline);
            try {
                if (cleanup != null) cleanup.run();
            } catch (RuntimeException | LinkageError error) {
                result.error(error);
                return false;
            }
            return true;
        }
    }
}

package org.hisi.phoneunlock;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.hardware.Camera;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.provider.Settings;
import android.util.Log;
import android.view.Gravity;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import com.google.zxing.BinaryBitmap;
import com.google.zxing.DecodeHintType;
import com.google.zxing.PlanarYUVLuminanceSource;
import com.google.zxing.ReaderException;
import com.google.zxing.common.HybridBinarizer;
import com.google.zxing.qrcode.QRCodeReader;

import org.hisi.phoneunlock.proto.PairUri;

import java.io.IOException;
import java.util.EnumMap;
import java.util.List;
import java.util.Map;

/** 扫配对二维码：Camera1 预览 + zxing QRCodeReader 解 NV21 帧（不依赖 Google 服务）。 */
@SuppressWarnings("deprecation")
public class ScanActivity extends Activity implements SurfaceHolder.Callback, Camera.PreviewCallback {
    private static final int REQ_CAM = 51;

    private final Handler ui = new Handler(Looper.getMainLooper());
    private FrameLayout root;
    private SurfaceView sv;
    private TextView hint;
    private Button settingsBtn;
    private Camera cam;
    private int camId = -1;
    private int pw;
    private int ph;
    private boolean surfaceReady;
    private boolean previewing;
    private boolean done;
    private boolean manualFocus;
    private HandlerThread decodeThread;
    private Handler decoder;
    private final QRCodeReader reader = new QRCodeReader();
    private final Map<DecodeHintType, Object> hints = new EnumMap<>(DecodeHintType.class);

    private final Runnable focusTick = new Runnable() {
        @Override
        public void run() {
            if (cam != null && previewing && manualFocus) {
                try {
                    cam.autoFocus(null);
                } catch (RuntimeException ignored) {
                }
                ui.postDelayed(this, 2000);
            }
        }
    };

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        hints.put(DecodeHintType.TRY_HARDER, Boolean.TRUE);
        hints.put(DecodeHintType.CHARACTER_SET, "UTF-8");

        root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        sv = new SurfaceView(this);
        sv.getHolder().addCallback(this);
        root.addView(sv, new FrameLayout.LayoutParams(-1, -1, Gravity.CENTER));

        View frame = new View(this);
        GradientDrawable g = new GradientDrawable();
        g.setColor(0x00000000);
        g.setStroke(Ui.dp(this, 3), 0xCCFFFFFF);
        g.setCornerRadius(Ui.dp(this, 16));
        frame.setBackground(g);
        int fs = Ui.dp(this, 250);
        root.addView(frame, new FrameLayout.LayoutParams(fs, fs, Gravity.CENTER));

        LinearLayout bottom = Ui.vbox(this);
        bottom.setGravity(Gravity.CENTER_HORIZONTAL);
        int p = Ui.dp(this, 24);
        bottom.setPadding(p, p, p, Ui.dp(this, 48));
        hint = Ui.text(this, "把电脑上显示的配对二维码放进框里", 16, Color.WHITE, false);
        hint.setGravity(Gravity.CENTER);
        hint.setShadowLayer(4, 0, 0, Color.BLACK);
        bottom.addView(hint);
        settingsBtn = Ui.lightButton(this, "去设置里授权相机");
        settingsBtn.setVisibility(View.GONE);
        settingsBtn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                startActivity(new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:" + getPackageName())));
            }
        });
        bottom.addView(settingsBtn, Ui.lpTop(this, -2, Ui.dp(this, 44), 16));
        root.addView(bottom, new FrameLayout.LayoutParams(-1, -2, Gravity.BOTTOM));

        TextView title = Ui.text(this, "扫码配对", 20, Color.WHITE, true);
        title.setShadowLayer(4, 0, 0, Color.BLACK);
        FrameLayout.LayoutParams tl = new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        tl.topMargin = Ui.dp(this, 48);
        root.addView(title, tl);

        setContentView(root);

        decodeThread = new HandlerThread("pu-qr");
        decodeThread.start();
        decoder = new Handler(decodeThread.getLooper());
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
            settingsBtn.setVisibility(View.GONE);
            openCamera();
        } else {
            requestPermissions(new String[] {Manifest.permission.CAMERA}, REQ_CAM);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        if (requestCode != REQ_CAM) return;
        if (grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
            settingsBtn.setVisibility(View.GONE);
            openCamera();
        } else {
            hint.setText("没有相机权限，没法扫码。\n可以去设置里授权，或回到主界面用“粘贴配对链接”。");
            settingsBtn.setVisibility(View.VISIBLE);
        }
    }

    @Override
    protected void onPause() {
        closeCamera();
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        decodeThread.quitSafely();
        super.onDestroy();
    }

    private void openCamera() {
        if (cam != null || done) return;
        try {
            Camera.CameraInfo info = new Camera.CameraInfo();
            int n = Camera.getNumberOfCameras();
            camId = -1;
            for (int i = 0; i < n; i++) {
                Camera.getCameraInfo(i, info);
                if (info.facing == Camera.CameraInfo.CAMERA_FACING_BACK) {
                    camId = i;
                    break;
                }
            }
            if (camId < 0 && n > 0) camId = 0;
            if (camId < 0) {
                hint.setText("这部手机没有相机");
                return;
            }
            cam = Camera.open(camId);
            Camera.Parameters prm = cam.getParameters();
            Camera.Size best = choosePreviewSize(prm.getSupportedPreviewSizes());
            pw = best.width;
            ph = best.height;
            prm.setPreviewSize(pw, ph);
            List<String> fm = prm.getSupportedFocusModes();
            manualFocus = false;
            if (fm != null && fm.contains(Camera.Parameters.FOCUS_MODE_CONTINUOUS_PICTURE)) {
                prm.setFocusMode(Camera.Parameters.FOCUS_MODE_CONTINUOUS_PICTURE);
            } else if (fm != null && fm.contains(Camera.Parameters.FOCUS_MODE_CONTINUOUS_VIDEO)) {
                prm.setFocusMode(Camera.Parameters.FOCUS_MODE_CONTINUOUS_VIDEO);
            } else if (fm != null && fm.contains(Camera.Parameters.FOCUS_MODE_AUTO)) {
                prm.setFocusMode(Camera.Parameters.FOCUS_MODE_AUTO);
                manualFocus = true;
            }
            cam.setParameters(prm);
            Camera.getCameraInfo(camId, info);
            cam.setDisplayOrientation(displayOrientation(info));
            Log.i(App.TAG, "相机 " + camId + " 预览 " + pw + "x" + ph + " 方向 " + info.orientation);
            layoutSurface(info);
            if (surfaceReady) startPreview();
        } catch (RuntimeException e) {
            Log.w(App.TAG, "打开相机失败", e);
            hint.setText("打开相机失败：" + e.getMessage());
            closeCamera();
        }
    }

    private static Camera.Size choosePreviewSize(List<Camera.Size> sizes) {
        Camera.Size best = sizes.get(0);
        long bestScore = Long.MAX_VALUE;
        for (Camera.Size s : sizes) {
            if (s.width < 640 || s.height < 480) continue;
            long score = Math.abs((long) s.width * s.height - 1280L * 720L);
            if (score < bestScore) {
                bestScore = score;
                best = s;
            }
        }
        return best;
    }

    private int displayOrientation(Camera.CameraInfo info) {
        int rot = getWindowManager().getDefaultDisplay().getRotation();
        int deg = rot == Surface.ROTATION_90 ? 90 : rot == Surface.ROTATION_180 ? 180 : rot == Surface.ROTATION_270 ? 270 : 0;
        if (info.facing == Camera.CameraInfo.CAMERA_FACING_FRONT) {
            return (360 - (info.orientation + deg) % 360) % 360;
        }
        return (info.orientation - deg + 360) % 360;
    }

    /** 按预览宽高比铺满屏幕宽度（多出来的被裁掉），避免画面变形。 */
    private void layoutSurface(Camera.CameraInfo info) {
        int sw = root.getWidth() > 0 ? root.getWidth() : getResources().getDisplayMetrics().widthPixels;
        int sh = root.getHeight() > 0 ? root.getHeight() : getResources().getDisplayMetrics().heightPixels;
        boolean rotated = displayOrientation(info) % 180 != 0;
        float aspect = rotated ? (float) ph / pw : (float) pw / ph; /* 显示出来的 宽/高 */
        int w = sw;
        int h = Math.round(sw / aspect);
        if (h < sh) {
            h = sh;
            w = Math.round(sh * aspect);
        }
        sv.setLayoutParams(new FrameLayout.LayoutParams(w, h, Gravity.CENTER));
    }

    private void startPreview() {
        if (cam == null || previewing) return;
        try {
            cam.setPreviewDisplay(sv.getHolder());
            cam.setPreviewCallbackWithBuffer(this);
            cam.addCallbackBuffer(new byte[pw * ph * 3 / 2]);
            cam.startPreview();
            previewing = true;
            if (manualFocus) ui.postDelayed(focusTick, 500);
        } catch (IOException | RuntimeException e) {
            Log.w(App.TAG, "开预览失败", e);
            hint.setText("相机预览失败：" + e.getMessage());
        }
    }

    private void closeCamera() {
        ui.removeCallbacks(focusTick);
        if (cam != null) {
            try {
                cam.setPreviewCallbackWithBuffer(null);
                cam.stopPreview();
            } catch (RuntimeException ignored) {
            }
            cam.release();
            cam = null;
        }
        previewing = false;
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        surfaceReady = true;
        if (cam != null) startPreview();
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        surfaceReady = false;
        if (cam != null && previewing) {
            try {
                cam.stopPreview();
            } catch (RuntimeException ignored) {
            }
            previewing = false;
        }
    }

    /** 主线程回调；解码丢给 decoder 线程，解完再把缓冲还给相机（相当于自动丢帧）。 */
    @Override
    public void onPreviewFrame(final byte[] data, final Camera camera) {
        if (done || data == null) return;
        final int w = pw;
        final int h = ph;
        decoder.post(new Runnable() {
            @Override
            public void run() {
                final String text = decode(data, w, h);
                ui.post(new Runnable() {
                    @Override
                    public void run() {
                        if (done) return;
                        if (text != null) {
                            onCode(text);
                            if (done) return;
                        }
                        if (cam == camera && cam != null) cam.addCallbackBuffer(data);
                    }
                });
            }
        });
    }

    private String decode(byte[] data, int w, int h) {
        try {
            PlanarYUVLuminanceSource src = new PlanarYUVLuminanceSource(data, w, h, 0, 0, w, h, false);
            return reader.decode(new BinaryBitmap(new HybridBinarizer(src)), hints).getText();
        } catch (ReaderException | RuntimeException e) {
            return null;
        } finally {
            reader.reset();
        }
    }

    private void onCode(String text) {
        if (PairUri.looksLike(text)) {
            done = true;
            Log.i(App.TAG, "扫到配对二维码");
            closeCamera();
            startActivity(new Intent(this, PairActivity.class).setData(Uri.parse(text.trim())));
            finish();
        } else {
            hint.setText("这不是“电脑解锁”的配对二维码。\n请扫电脑上 " + Ui.PAIR_CMD + " 显示的二维码。");
        }
    }
}

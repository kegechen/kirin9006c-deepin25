package org.hisi.phoneunlock;

import android.app.KeyguardManager;
import android.content.Context;
import android.hardware.biometrics.BiometricManager;
import android.hardware.fingerprint.FingerprintManager;
import android.os.Build;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Log;

import java.security.GeneralSecurityException;
import java.security.Key;
import java.security.KeyPairGenerator;
import java.security.KeyStore;
import java.security.PrivateKey;
import java.security.PublicKey;
import java.security.Signature;
import java.security.spec.ECGenParameterSpec;

/**
 * Android Keystore 里的 EC P-256 签名密钥（每次使用都要过强生物识别；测试用的一键模式例外），
 * 以及“能不能用指纹”的检查。
 */
final class KeyHelper {
    private KeyHelper() {}

    private static final String KS = "AndroidKeyStore";

    static final class MissingKeyException extends GeneralSecurityException {
        MissingKeyException() {
            super("手机里找不到这台电脑的密钥");
        }
    }

    /** 能配对 / 能用指纹就返回 null，否则返回给用户看的原因。 */
    @SuppressWarnings("deprecation")
    static String notReadyReason(Context c) {
        KeyguardManager km = c.getSystemService(KeyguardManager.class);
        if (km == null || !km.isDeviceSecure()) {
            return "手机还没有设置锁屏密码。请先在系统设置里设置锁屏密码并录入指纹，再来配对。";
        }
        if (Build.VERSION.SDK_INT >= 29) {
            BiometricManager bm = c.getSystemService(BiometricManager.class);
            if (bm == null) return "这部手机不支持指纹。";
            int r = Build.VERSION.SDK_INT >= 30
                    ? bm.canAuthenticate(BiometricManager.Authenticators.BIOMETRIC_STRONG)
                    : bm.canAuthenticate();
            switch (r) {
                case BiometricManager.BIOMETRIC_SUCCESS:
                    return null;
                case BiometricManager.BIOMETRIC_ERROR_NONE_ENROLLED:
                    return "手机还没有录入指纹。请先在系统设置里录入指纹，再来配对。";
                case BiometricManager.BIOMETRIC_ERROR_NO_HARDWARE:
                    return "这部手机没有可用的指纹（强生物识别）硬件。";
                case BiometricManager.BIOMETRIC_ERROR_HW_UNAVAILABLE:
                    return "指纹硬件暂时不可用，请稍后再试。";
                default:
                    return "手机暂时不能用指纹（代码 " + r + "）。";
            }
        }
        FingerprintManager fm = c.getSystemService(FingerprintManager.class);
        if (fm == null || !fm.isHardwareDetected()) return "这部手机没有指纹硬件。";
        if (!fm.hasEnrolledFingerprints()) return "手机还没有录入指纹。请先在系统设置里录入指纹，再来配对。";
        return null;
    }

    private static KeyStore load() throws GeneralSecurityException {
        try {
            KeyStore ks = KeyStore.getInstance(KS);
            ks.load(null);
            return ks;
        } catch (java.io.IOException e) {
            throw new GeneralSecurityException(e);
        }
    }

    /**
     * 生成（已存在先删）别名为 alias 的签名密钥，返回公钥。
     * oneTap = true 是测试用的“一键解锁”：密钥不要求用户认证（手机没设锁屏 / 没录指纹时也能用），
     * 拿到这部手机的人点一下就能解锁电脑。正式使用必须是 false（每次签名都要过强生物识别）。
     */
    static PublicKey generate(String alias, boolean oneTap) throws GeneralSecurityException {
        delete(alias);
        KeyGenParameterSpec.Builder b = new KeyGenParameterSpec.Builder(alias, KeyProperties.PURPOSE_SIGN)
                .setAlgorithmParameterSpec(new ECGenParameterSpec("secp256r1"))
                .setDigests(KeyProperties.DIGEST_SHA256);
        if (!oneTap) {
            b.setUserAuthenticationRequired(true).setInvalidatedByBiometricEnrollment(true);
            if (Build.VERSION.SDK_INT >= 30) {
                b.setUserAuthenticationParameters(0, KeyProperties.AUTH_BIOMETRIC_STRONG);
            }
        }
        KeyPairGenerator g = KeyPairGenerator.getInstance(KeyProperties.KEY_ALGORITHM_EC, KS);
        g.initialize(b.build());
        PublicKey pub = g.generateKeyPair().getPublic();
        Log.i(App.TAG, "已生成密钥 " + alias + (oneTap ? "（一键测试模式，不要求指纹）" : ""));
        return pub;
    }

    static void delete(String alias) {
        try {
            KeyStore ks = load();
            if (ks.containsAlias(alias)) {
                ks.deleteEntry(alias);
                Log.i(App.TAG, "已删除密钥 " + alias);
            }
        } catch (GeneralSecurityException e) {
            Log.w(App.TAG, "删除密钥失败：" + e);
        }
    }

    /**
     * 拿私钥初始化 SHA256withECDSA 签名器（之后交给 BiometricPrompt）。
     * 新录入指纹后会抛 KeyPermanentlyInvalidatedException；密钥不在抛 MissingKeyException。
     */
    static Signature initSignature(String alias) throws GeneralSecurityException {
        KeyStore ks = load();
        Key k = ks.getKey(alias, null);
        if (!(k instanceof PrivateKey)) throw new MissingKeyException();
        Signature s = Signature.getInstance("SHA256withECDSA");
        s.initSign((PrivateKey) k);
        return s;
    }
}

package com.paoloanzn.airgap;

import android.app.Activity;
import android.hardware.biometrics.BiometricManager.Authenticators;
import android.hardware.biometrics.BiometricPrompt;
import android.os.CancellationSignal;

import javax.crypto.Cipher;

// Shows the system authentication prompt for one Keystore cipher operation.
// Native code cannot subclass BiometricPrompt.AuthenticationCallback, so it calls
// show() and wallet_store.cpp registers nativeResult() to receive the outcome.
final class AuthPrompt extends BiometricPrompt.AuthenticationCallback {
    private static final int AUTHENTICATORS =
            Authenticators.BIOMETRIC_STRONG | Authenticators.DEVICE_CREDENTIAL;

    private final long id;

    private AuthPrompt(long id) {
        this.id = id;
    }

    static CancellationSignal show(Activity activity, Cipher cipher, String title,
                                   String subtitle, long id) {
        final CancellationSignal cancel = new CancellationSignal();

        activity.runOnUiThread(() -> {
            try {
                final BiometricPrompt prompt = new BiometricPrompt.Builder(activity)
                        .setTitle(title)
                        .setSubtitle(subtitle)
                        .setAllowedAuthenticators(AUTHENTICATORS)
                        .build();

                prompt.authenticate(new BiometricPrompt.CryptoObject(cipher), cancel,
                                    activity.getMainExecutor(), new AuthPrompt(id));

            } catch (RuntimeException error) {
                nativeResult(id, false, error.getMessage());
            }
        });

        return cancel;
    }

    @Override
    public void onAuthenticationSucceeded(BiometricPrompt.AuthenticationResult result) {
        nativeResult(id, true, null);
    }

    // A rejected fingerprint keeps the prompt open; only final errors end it.
    @Override
    public void onAuthenticationError(int code, CharSequence message) {
        nativeResult(id, false, message == null ? null : message.toString());
    }

    private static native void nativeResult(long id, boolean success, String message);
}

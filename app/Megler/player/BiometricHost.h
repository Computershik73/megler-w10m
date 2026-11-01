// The phone's check of its user for an Android app's BiometricPrompt, BiometricManager and FingerprintManager:
// megler_biometric_host (core/include/megler.h) over Windows Hello, Windows.Security.Credentials.UI's
// UserConsentVerifier -- the iris, a fingerprint or the PIN, however the user set Windows Hello up.
//
// Whether the phone can is CheckAvailabilityAsync, asked on a thread of the multithreaded apartment while the caller
// waits: the core's calls come on the app's own threads, which have none. A check is RequestVerificationAsync, started on
// the UI thread, where Windows can show its dialog with the app's message; the app's thread waits for the answer however
// long the user takes. Cancel cancels that operation, on the UI thread too, which closes the dialog.
#pragma once

#include "megler.h"

namespace megler::app::biometricHost {

/// The callbacks for megler_run_config.biometric, with nothing as their user.
megler_biometric_host callbacks();

} // namespace megler::app::biometricHost

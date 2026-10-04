// Sign-in page: Google popup via Firebase, then trade the ID token for a
// server session (login.php re-verifies it and checks the email whitelist).
import { initializeApp } from 'https://www.gstatic.com/firebasejs/12.8.0/firebase-app.js';
import { getAuth, signInWithPopup, GoogleAuthProvider } from 'https://www.gstatic.com/firebasejs/12.8.0/firebase-auth.js';

const config = JSON.parse(document.getElementById('firebaseConfig').textContent);
const auth = getAuth(initializeApp(config));
const provider = new GoogleAuthProvider();
provider.setCustomParameters({ prompt: 'select_account' });

const button = document.getElementById('googleSignIn');
const status = document.getElementById('loginStatus');

function show(message, kind) {
    status.textContent = message;
    status.className = 'status ' + kind;
    status.hidden = false;
}

const SERVER_ERRORS = {
    not_authorized: (d) => `${d.email || 'That account'} is not authorized for this panel.`,
    invalid_token: () => 'Google sign-in could not be verified. Try again.',
    not_google_verified: () => 'That account is not a verified Google sign-in.',
    firebase_not_configured: () => 'Firebase is not configured on the server (.env).',
    verify_network_error: () => 'The server could not reach Google to verify the sign-in.',
    rate_limited: (d) => `Too many attempts. Try again in ${d.retry_after || 60}s.`,
};

button.addEventListener('click', async () => {
    button.disabled = true;
    show('Signing in…', 'info');
    try {
        const result = await signInWithPopup(auth, provider);
        show('Verifying…', 'info');
        const idToken = await result.user.getIdToken();
        const res = await fetch('login.php', {
            method: 'POST',
            credentials: 'same-origin',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ idToken }),
        });
        const data = await res.json().catch(() => ({}));
        if (res.ok && data.ok) {
            window.location.reload();
            return;
        }
        // The server session is what counts; don't leave a Firebase one behind.
        await auth.signOut().catch(() => {});
        const explain = SERVER_ERRORS[data.error];
        show(explain ? explain(data) : `Sign-in failed (${data.error || res.status}).`, 'error');
    } catch (err) {
        const messages = {
            'auth/popup-closed-by-user': 'Sign-in cancelled.',
            'auth/cancelled-popup-request': 'Sign-in cancelled.',
            'auth/popup-blocked': 'The popup was blocked. Allow popups for this site and try again.',
            'auth/unauthorized-domain': `This domain is not authorized in Firebase. Add ${location.hostname} under Authentication → Settings → Authorized domains.`,
            'auth/operation-not-allowed': 'Google sign-in is not enabled in this Firebase project.',
        };
        show(messages[err.code] || err.message || 'Sign-in failed.', 'error');
    }
    button.disabled = false;
});

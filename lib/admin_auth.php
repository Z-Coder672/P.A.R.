<?php
declare(strict_types=1);

// Admin-panel authentication: Firebase Google sign-in -> server-side token
// verification -> whitelisted email -> PHP session. Same model as the QR
// Formatter project (verify-auth.php + config.php there).
//
// Never requested directly (404 guard + lib/ denied in .htaccess).
if (isset($_SERVER['SCRIPT_FILENAME']) && @realpath($_SERVER['SCRIPT_FILENAME']) === __FILE__) {
    http_response_code(404);
    exit;
}

require_once __DIR__ . '/private_store.php';

const ADMIN_SESSION_NAME     = 'PAR_ADMIN_SESSION';
const ADMIN_SESSION_LIFETIME = 2592000; // 30 days, as in QR Formatter

function admin_is_https(): bool
{
    return (!empty($_SERVER['HTTPS']) && $_SERVER['HTTPS'] !== 'off')
        || (string) ($_SERVER['SERVER_PORT'] ?? '') === '443'
        || strtolower((string) ($_SERVER['HTTP_X_FORWARDED_PROTO'] ?? '')) === 'https';
}

/** Lowercased whitelist from ADMIN_EMAILS (comma-separated) in .env. */
function admin_emails(): array
{
    $raw = par_env('ADMIN_EMAILS') ?? '';
    $out = [];
    foreach (explode(',', $raw) as $e) {
        $e = strtolower(trim($e));
        if ($e !== '') {
            $out[] = $e;
        }
    }
    return $out;
}

function admin_email_allowed(string $email): bool
{
    return in_array(strtolower(trim($email)), admin_emails(), true);
}

/**
 * Firebase web config for the login page, or null if .env is incomplete.
 * These values are public by design (they ship to every browser); the
 * whitelist + server-side verification are what gate access.
 */
function admin_firebase_config(): ?array
{
    $apiKey    = par_env('FIREBASE_API_KEY') ?? '';
    $projectId = par_env('FIREBASE_PROJECT_ID') ?? '';
    if ($apiKey === '' || $projectId === '') {
        return null;
    }
    $authDomain = par_env('FIREBASE_AUTH_DOMAIN') ?? '';
    if ($authDomain === '') {
        $authDomain = $projectId . '.firebaseapp.com';
    }
    return ['apiKey' => $apiKey, 'authDomain' => $authDomain, 'projectId' => $projectId];
}

function admin_security_headers(): void
{
    header('X-Frame-Options: DENY');
    header('X-Content-Type-Options: nosniff');
    header('Referrer-Policy: strict-origin-when-cross-origin');
    header('Cache-Control: no-store');
    // signInWithPopup needs to keep a handle on the popup it opened.
    header('Cross-Origin-Opener-Policy: same-origin-allow-popups');
    if (admin_is_https()) {
        header('Strict-Transport-Security: max-age=31536000');
    }
    $cfg = admin_firebase_config();
    $frame = $cfg !== null && preg_match('/^[A-Za-z0-9.-]+$/', $cfg['authDomain'])
        ? 'https://' . $cfg['authDomain']
        : "'none'";
    header(
        "Content-Security-Policy: default-src 'self'; "
        . "script-src 'self' https://www.gstatic.com https://apis.google.com; "
        . "style-src 'self' 'unsafe-inline'; "
        . "img-src 'self' data: https:; "
        . "connect-src 'self' https://identitytoolkit.googleapis.com https://securetoken.googleapis.com https://www.googleapis.com; "
        . "frame-src $frame; object-src 'none'; base-uri 'self'; form-action 'self'"
    );
}

/**
 * Start the admin session. With $create=false a request that carries no
 * session cookie does not start one — so anonymous hits (scanners, the login
 * page itself) never leave a session file behind. Session files live in
 * PRIVATE_DIR/sessions, off the webroot and out of the host's shared tmp.
 */
function admin_session_start(bool $create): bool
{
    if (session_status() === PHP_SESSION_ACTIVE) {
        return true;
    }
    if (!$create && empty($_COOKIE[ADMIN_SESSION_NAME])) {
        return false;
    }
    $dir = par_private_dir() . '/sessions';
    if (!is_dir($dir)) {
        @mkdir($dir, 0700, true);
    }
    if (is_dir($dir) && is_writable($dir)) {
        session_save_path($dir);
    }
    ini_set('session.gc_maxlifetime', (string) ADMIN_SESSION_LIFETIME);
    ini_set('session.use_strict_mode', '1');
    ini_set('session.use_only_cookies', '1');
    session_name(ADMIN_SESSION_NAME);
    session_set_cookie_params([
        'lifetime' => ADMIN_SESSION_LIFETIME,
        'path'     => '/admin',
        'secure'   => admin_is_https(),
        'httponly' => true,
        'samesite' => 'Strict',
    ]);
    return session_start();
}

/**
 * Email of the signed-in admin, or null. The whitelist is re-checked on every
 * request, so removing an address from ADMIN_EMAILS revokes it immediately
 * rather than at session expiry.
 */
function admin_current_email(): ?string
{
    if (!admin_session_start(false)) {
        return null;
    }
    $email = $_SESSION['email'] ?? null;
    $login = $_SESSION['login_time'] ?? null;
    if (($_SESSION['authenticated'] ?? false) !== true || !is_string($email) || !is_int($login)) {
        // A stale or made-up cookie: strict mode just minted an empty session
        // for it. Drop that rather than leave a file behind per request.
        admin_session_destroy();
        return null;
    }
    if (time() - $login > ADMIN_SESSION_LIFETIME || !admin_email_allowed($email)) {
        admin_session_destroy();
        return null;
    }
    return $email;
}

function admin_session_destroy(): void
{
    if (session_status() !== PHP_SESSION_ACTIVE) {
        return;
    }
    $_SESSION = [];
    $p = session_get_cookie_params();
    setcookie(session_name(), '', [
        'expires'  => time() - 42000,
        'path'     => $p['path'],
        'secure'   => $p['secure'],
        'httponly' => $p['httponly'],
        'samesite' => 'Strict',
    ]);
    session_destroy();
}

function admin_csrf_token(): string
{
    if (empty($_SESSION['csrf_token'])) {
        $_SESSION['csrf_token'] = bin2hex(random_bytes(32));
    }
    return $_SESSION['csrf_token'];
}

function admin_csrf_ok(): bool
{
    $sent = $_SERVER['HTTP_X_CSRF_TOKEN'] ?? '';
    return is_string($sent) && !empty($_SESSION['csrf_token'])
        && hash_equals($_SESSION['csrf_token'], $sent);
}

/** @return never */
function admin_json(int $status, array $body): void
{
    http_response_code($status);
    header('Content-Type: application/json; charset=UTF-8');
    header('Cache-Control: no-store');
    echo json_encode($body, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
    exit;
}

/** JSON body of a POST, enforcing application/json (forces a CORS preflight). */
function admin_json_body(): array
{
    if (($_SERVER['REQUEST_METHOD'] ?? '') !== 'POST') {
        header('Allow: POST');
        admin_json(405, ['ok' => false, 'error' => 'post_only']);
    }
    $mime = strtolower(trim(explode(';', $_SERVER['CONTENT_TYPE'] ?? '', 2)[0]));
    if ($mime !== 'application/json') {
        admin_json(415, ['ok' => false, 'error' => 'json_only']);
    }
    $data = json_decode(file_get_contents('php://input') ?: '', true);
    if (!is_array($data)) {
        admin_json(400, ['ok' => false, 'error' => 'bad_json']);
    }
    return $data;
}

/**
 * Verify a Firebase ID token via Identity Toolkit accounts:lookup (Google
 * checks the signature and that the token belongs to this API key's project).
 * On top of QR Formatter's check this also requires a verified email backed by
 * the Google provider, so an email/password account registered under an
 * admin's address in the same Firebase project can't pass.
 *
 * @return array{ok:bool, email?:string, uid?:string, error?:string}
 */
function admin_verify_firebase_token(string $idToken): array
{
    $cfg = admin_firebase_config();
    if ($cfg === null) {
        return ['ok' => false, 'error' => 'firebase_not_configured'];
    }
    $ch = curl_init('https://identitytoolkit.googleapis.com/v1/accounts:lookup?key=' . rawurlencode($cfg['apiKey']));
    curl_setopt_array($ch, [
        CURLOPT_POST           => true,
        CURLOPT_HTTPHEADER     => ['Content-Type: application/json'],
        CURLOPT_POSTFIELDS     => json_encode(['idToken' => $idToken]),
        CURLOPT_RETURNTRANSFER => true,
        CURLOPT_SSL_VERIFYPEER => true,
        CURLOPT_TIMEOUT        => 10,
    ]);
    $response = curl_exec($ch);
    $code = (int) curl_getinfo($ch, CURLINFO_HTTP_CODE);
    $err  = curl_error($ch);
    curl_close($ch);

    if ($response === false || $err !== '') {
        error_log('admin auth: verification network error: ' . $err);
        return ['ok' => false, 'error' => 'verify_network_error'];
    }
    if ($code !== 200) {
        error_log('admin auth: Firebase verification failed, HTTP ' . $code);
        return ['ok' => false, 'error' => 'invalid_token'];
    }
    $user = json_decode((string) $response, true)['users'][0] ?? null;
    if (!is_array($user) || !is_string($user['email'] ?? null) || $user['email'] === '') {
        return ['ok' => false, 'error' => 'invalid_token'];
    }
    $email = strtolower($user['email']);
    $google = false;
    foreach ($user['providerUserInfo'] ?? [] as $p) {
        if (($p['providerId'] ?? '') === 'google.com' && strtolower((string) ($p['email'] ?? '')) === $email) {
            $google = true;
        }
    }
    if (($user['emailVerified'] ?? false) !== true || !$google) {
        return ['ok' => false, 'error' => 'not_google_verified'];
    }
    return ['ok' => true, 'email' => $email, 'uid' => (string) ($user['localId'] ?? '')];
}

/** Append one line to PRIVATE_DIR/admin-audit.log. Best-effort. */
function admin_audit(string $email, string $action, array $detail = []): void
{
    $line = json_encode(
        ['ts' => date('c'), 'email' => $email, 'ip' => $_SERVER['REMOTE_ADDR'] ?? '', 'action' => $action] + $detail,
        JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES
    );
    @file_put_contents(par_private_dir() . '/admin-audit.log', $line . PHP_EOL, FILE_APPEND | LOCK_EX);
}

<?php
declare(strict_types=1);

// Exchange a Firebase ID token for an admin session.
// POST {idToken} -> verify with Google -> email must be in ADMIN_EMAILS.

require_once __DIR__ . '/../lib/ratelimit.php';
require_once __DIR__ . '/../lib/admin_auth.php';

admin_security_headers();

// Each attempt costs an outbound call to Google, so keep this tight.
par_rate_limit('admin-login', [
    'ip_rate'      => 5 / 60,
    'ip_burst'     => 5,
    'global_rate'  => 0.5,
    'global_burst' => 20,
]);

$data = admin_json_body();
$idToken = $data['idToken'] ?? '';
if (!is_string($idToken) || $idToken === '' || strlen($idToken) > 8192) {
    admin_json(400, ['ok' => false, 'error' => 'missing_token']);
}

$result = admin_verify_firebase_token($idToken);
if (!$result['ok']) {
    $status = $result['error'] === 'firebase_not_configured' ? 500
        : ($result['error'] === 'verify_network_error' ? 502 : 401);
    admin_json($status, ['ok' => false, 'error' => $result['error']]);
}

$email = $result['email'];
if (!admin_email_allowed($email)) {
    admin_audit($email, 'login_denied');
    admin_json(403, ['ok' => false, 'error' => 'not_authorized', 'email' => $email]);
}

admin_session_start(true);
session_regenerate_id(true);
$_SESSION = [
    'authenticated' => true,
    'email'         => $email,
    'uid'           => $result['uid'],
    'login_time'    => time(),
];
admin_csrf_token();
admin_audit($email, 'login');

admin_json(200, ['ok' => true, 'email' => $email]);

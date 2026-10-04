<?php
declare(strict_types=1);

require_once __DIR__ . '/../lib/admin_auth.php';

admin_security_headers();
admin_json_body();

$email = admin_current_email();
if ($email === null) {
    admin_json(200, ['ok' => true]);
}
if (!admin_csrf_ok()) {
    admin_json(403, ['ok' => false, 'error' => 'bad_csrf']);
}
admin_audit($email, 'logout');
admin_session_destroy();
admin_json(200, ['ok' => true]);

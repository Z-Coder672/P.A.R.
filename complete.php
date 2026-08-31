<?php
declare(strict_types=1);

// REDUNDANT FALLBACK. The gallery entry is now finalized by snapshot-request.php
// (the single post-display "print done" event), so by the time the Arduino's
// complete.php call lands the rename below has usually already happened and this
// just confirms info.json exists. Kept because the Arduino still calls it and
// expects a 2xx; it also covers the legacy/ad-hoc path where snapshot-request
// fired with no id. The system no longer DEPENDS on this call succeeding.

header('Content-Type: text/plain; charset=UTF-8');
header('Cache-Control: no-store, no-cache, must-revalidate, max-age=0');

require_once __DIR__ . '/lib/private_store.php';

foreach (@file(__DIR__ . '/.env', FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) ?: [] as $line) {
    if (strpos($line, '=') !== false) {
        [$k, $v] = explode('=', $line, 2);
        putenv(trim($k) . '=' . trim(trim($v), '"\''));
    }
}

$secret = getenv('SNAPSHOT_SECRET') ?: '';
$provided = $_SERVER['HTTP_X_SNAPSHOT_SECRET'] ?? '';
if (!is_string($provided)) {
    $provided = '';
}
if ($secret === '' || !hash_equals($secret, $provided)) {
    http_response_code(401);
    echo 'unauthorized';
    exit;
}

$id = $_GET['id'] ?? '';
if (!is_string($id) || !ctype_digit($id) || $id === '') {
    http_response_code(400);
    echo 'bad id';
    exit;
}

$pendingPath = __DIR__ . '/gallery/' . $id . '/pending.json';
$infoPath    = __DIR__ . '/gallery/' . $id . '/info.json';

if (file_exists($infoPath)) {
    // Fallback notify path (usually a no-op — snapshot-request.php already
    // claimed and sent). Idempotent via the atomic claim.
    par_notify_gallery_complete((int) $id);
    echo 'ok';
    exit;
}

if (!file_exists($pendingPath) || !rename($pendingPath, $infoPath)) {
    http_response_code(404);
    echo 'no pending';
    exit;
}

// We just performed the pending -> info rename, which means snapshot-request.php
// did NOT finalize this entry first (it writes its flag and then renames, so an
// already-info entry — the common case handled above — implies its flag was
// already armed). A rename happening HERE therefore means the snapshot flag was
// likely never armed (a dropped snapshot-request POST), which would otherwise
// leave any in-flight recording running to the RECORD_MAX cap. Arm it now so the
// snapshot poller stops the recording (and grabs the photo): the redundant
// recording-stop path the unified snapshot trigger otherwise lacked. Mirrors
// snapshot-request.php's write; $id is validated digits-only above.
@file_put_contents(__DIR__ . '/snapshot-pending.flag', $id);

par_notify_gallery_complete((int) $id);
echo 'ok';

<?php
if (($_SERVER['REQUEST_METHOD'] ?? '') !== 'POST') {
    http_response_code(405);
    exit;
}

require_once __DIR__ . '/lib/private_store.php';

foreach (@file(__DIR__ . '/.env', FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) ?: [] as $line) {
    if (strpos($line, '=') !== false) {
        [$k, $v] = explode('=', $line, 2);
        putenv(trim($k) . '=' . trim(trim($v), '"\''));
    }
}

$secret = getenv('SNAPSHOT_SECRET') ?: '';
$provided = $_POST['secret'] ?? ($_SERVER['HTTP_X_SNAPSHOT_SECRET'] ?? '');
if (!is_string($provided)) {
    $provided = '';
}
if ($secret === '' || !hash_equals($secret, $provided)) {
    http_response_code(401);
    exit;
}

// Write the gallery id into the flag so snapshot-next.php can hand it to the
// poller (which uploads to gallery/<id>/image.jpg). The Arduino POSTs id=<N>
// after its check pass; an absent/invalid id falls back to the legacy empty
// flag (IDless ad-hoc capture).
$id = $_POST['id'] ?? '';
$contents = (is_string($id) && ctype_digit($id)) ? $id : '';
file_put_contents(__DIR__ . '/snapshot-pending.flag', $contents);

// "Snapshot requested" is the single server-side "print done" event: this one
// post-display call drives the photo AND the recording stop (via the flag
// above), so it also finalizes the gallery entry here. That makes the Arduino's
// separate complete.php call redundant — the system no longer depends on it, so
// a mid-job device reboot that drops complete.php (it retries for 5 min, a wide
// window for a brownout) can't strand the entry as pending. complete.php still
// runs and just finds info.json already present and no-ops. Mirror its rename
// logic exactly: idempotent (skip if already complete), no-op if there's no
// pending entry yet (e.g. legacy IDless capture).
if ($contents !== '') {
    $pendingPath = __DIR__ . '/gallery/' . $contents . '/pending.json';
    $infoPath    = __DIR__ . '/gallery/' . $contents . '/info.json';
    if (!file_exists($infoPath) && file_exists($pendingPath)) {
        @rename($pendingPath, $infoPath);
    }

    // Print done: notify the submitter if they left an email. Idempotent and
    // atomic (see par_notify_gallery_complete) so device retries and the
    // redundant complete.php call can't double-send.
    par_notify_gallery_complete((int) $contents);
}

http_response_code(204);
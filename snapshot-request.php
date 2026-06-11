<?php
if (($_SERVER['REQUEST_METHOD'] ?? '') !== 'POST') {
    http_response_code(405);
    exit;
}

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
http_response_code(204);
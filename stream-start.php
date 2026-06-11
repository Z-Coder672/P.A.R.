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

header('Content-Type: application/json; charset=UTF-8');
header('Cache-Control: no-store');

$flag = __DIR__ . '/stream-pending.flag';
if (!file_exists($flag)) {
    http_response_code(204);
    exit;
}

// Flags older than 10 minutes are expired — delete and report empty so a
// stale print doesn't trigger a stream restart long after the fact.
$mtime = @filemtime($flag);
if ($mtime !== false && (time() - $mtime) > 600) {
    @unlink($flag);
    http_response_code(204);
    exit;
}

$raw = (string)@file_get_contents($flag);
$parsed = json_decode($raw, true);
if (is_array($parsed)) {
    $id   = isset($parsed['id']) ? (int)$parsed['id'] : null;
    $name = (string)($parsed['name'] ?? '');
} else {
    // Legacy plain-text flag — name only, no gallery id.
    $id   = null;
    $name = trim($raw);
}

// Pop the flag atomically — caller is responsible for restarting the stream.
@unlink($flag);
echo json_encode(['ok' => true, 'id' => $id, 'name' => $name, 'entry' => date('c')]);

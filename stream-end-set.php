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

$idRaw = $_POST['id'] ?? '';
if (!is_string($idRaw) || !preg_match('/^\d{1,12}$/', $idRaw)) {
    http_response_code(400);
    echo json_encode(['ok' => false, 'error' => 'bad_id']);
    exit;
}
$id = (int)$idRaw;

$flag = __DIR__ . '/stream-end.flag';

// Stale-flag prophylactic: if an old flag is lingering past its 10-min
// useful window, drop it so the fresh signal can't be overridden later by
// a stuck file.
$existing = @filemtime($flag);
if ($existing !== false && (time() - $existing) > 600) {
    @unlink($flag);
}

file_put_contents($flag, json_encode(['id' => $id]));
echo json_encode(['ok' => true]);

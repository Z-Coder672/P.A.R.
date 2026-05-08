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

$flag = __DIR__ . '/snapshot-pending.flag';
if (!file_exists($flag)) {
    http_response_code(204);
    exit;
}

$contents = trim((string)@file_get_contents($flag));
$galleryId = ctype_digit($contents) ? (int)$contents : null;

// Pop the flag atomically — caller is responsible for taking and uploading the snapshot
unlink($flag);
echo json_encode(['ok' => true, 'entry' => date('c'), 'id' => $galleryId]);

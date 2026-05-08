<?php
declare(strict_types=1);

header('Content-Type: text/plain; charset=UTF-8');
header('Cache-Control: no-store, no-cache, must-revalidate, max-age=0');

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
    echo 'ok';
    exit;
}

if (!file_exists($pendingPath) || !rename($pendingPath, $infoPath)) {
    http_response_code(404);
    echo 'no pending';
    exit;
}

echo 'ok';

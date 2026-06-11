<?php
declare(strict_types=1);

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
if (!is_string($provided) || $secret === '' || !hash_equals($secret, $provided)) {
    http_response_code(401);
    exit;
}

$id       = $_POST['id'] ?? '';
$videoId  = $_POST['video_id'] ?? '';
if (!is_string($id) || !ctype_digit($id) || !is_string($videoId) || $videoId === '') {
    http_response_code(400);
    echo 'bad id or video_id';
    exit;
}
// YouTube video ids are 11 chars of [A-Za-z0-9_-]; reject anything else
// so this endpoint can't be coerced into writing arbitrary strings into
// gallery JSON that later gets interpolated into <iframe src>.
if (!preg_match('/^[A-Za-z0-9_-]{11}$/', $videoId)) {
    http_response_code(400);
    echo 'malformed video_id';
    exit;
}

$entryDir = __DIR__ . '/gallery/' . $id;
$pendingPath = $entryDir . '/pending.json';
$infoPath    = $entryDir . '/info.json';
$target = file_exists($pendingPath) ? $pendingPath : (file_exists($infoPath) ? $infoPath : null);
if ($target === null) {
    http_response_code(404);
    echo 'gallery entry not found';
    exit;
}

$raw = file_get_contents($target);
$json = is_string($raw) ? json_decode($raw, true) : null;
if (!is_array($json)) {
    http_response_code(500);
    echo 'corrupt entry';
    exit;
}
$json['video_id'] = $videoId;
file_put_contents($target, json_encode($json, JSON_UNESCAPED_UNICODE));

// Mirror the newest upload into latest-video.json so the Latest tab can embed
// it without hitting the YouTube Data API. Atomic-ish write via rename.
$latestPath = __DIR__ . '/latest-video.json';
$tmpPath    = $latestPath . '.tmp';
$latestPayload = [
    'id'       => (int)$id,
    'video_id' => $videoId,
    'name'     => (string)($json['name'] ?? ''),
    'ts'       => time(),
];
if (file_put_contents($tmpPath, json_encode($latestPayload, JSON_UNESCAPED_UNICODE)) !== false) {
    @rename($tmpPath, $latestPath);
}

header('Content-Type: application/json; charset=UTF-8');
echo json_encode(['ok' => true]);

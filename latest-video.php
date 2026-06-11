<?php
declare(strict_types=1);

header('Content-Type: application/json; charset=UTF-8');
header('Cache-Control: no-store');

$path = __DIR__ . '/latest-video.json';
if (!file_exists($path)) {
    http_response_code(204);
    exit;
}

$raw = @file_get_contents($path);
$data = is_string($raw) ? json_decode($raw, true) : null;
if (!is_array($data) || !isset($data['video_id'])
        || !preg_match('/^[A-Za-z0-9_-]{11}$/', (string)$data['video_id'])) {
    http_response_code(204);
    exit;
}

echo json_encode([
    'video_id' => $data['video_id'],
    'name'     => (string)($data['name'] ?? ''),
    'id'       => isset($data['id']) ? (int)$data['id'] : null,
]);

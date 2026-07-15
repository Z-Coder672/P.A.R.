<?php
declare(strict_types=1);

require_once __DIR__ . '/lib/ratelimit.php';
par_rate_limit('latest', [
    'ip_rate'     => 0.34, // ~20/min per IP
    'ip_burst'    => 20,
    'global_rate' => 5.0,
    'global_burst' => 60,
]);

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

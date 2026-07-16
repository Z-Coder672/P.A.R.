<?php
declare(strict_types=1);

require_once __DIR__ . '/lib/ratelimit.php';
// Also shields the YouTube Data API quota from being burned by request floods.
par_rate_limit('video-status', [
    'ip_rate'     => 0.25, // ~15/min per IP
    'ip_burst'    => 15,
    'global_rate' => 2.0,
    'global_burst' => 30,
]);

header('Content-Type: application/json; charset=UTF-8');
header('Cache-Control: no-store');

$videoId = $_GET['id'] ?? '';
if (!is_string($videoId) || !preg_match('/^[A-Za-z0-9_-]{11}$/', $videoId)) {
    http_response_code(400);
    echo json_encode(['error' => 'bad id']);
    exit;
}

$apiKey = '';
foreach (@file(__DIR__ . '/.env', FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) ?: [] as $line) {
    if (strpos($line, '=') === false) continue;
    [$k, $v] = explode('=', $line, 2);
    if (trim($k) === 'YT_DATA_KEY') {
        $apiKey = trim(trim($v), '"\'');
    }
}
if ($apiKey === '') {
    http_response_code(500);
    echo json_encode(['error' => 'API key not configured']);
    exit;
}

$url = 'https://www.googleapis.com/youtube/v3/videos?part=snippet,contentDetails&id='
     . urlencode($videoId) . '&key=' . urlencode($apiKey);
$resp = @file_get_contents($url);
$data = is_string($resp) ? json_decode($resp, true) : null;

$exists = is_array($data) && !empty($data['items']);
$liveBroadcastContent = $exists ? ($data['items'][0]['snippet']['liveBroadcastContent'] ?? 'none') : 'none';

// Parse the ISO 8601 duration (e.g. "PT1H2M30S") into whole seconds so the
// client can start playback near the end of the recording. null if absent.
$duration = null;
if ($exists) {
    $iso = $data['items'][0]['contentDetails']['duration'] ?? '';
    if (is_string($iso) && preg_match('/^P(?:(\d+)D)?T(?:(\d+)H)?(?:(\d+)M)?(?:(\d+)S)?$/', $iso, $m)) {
        $duration = (int)($m[1] ?? 0) * 86400
                  + (int)($m[2] ?? 0) * 3600
                  + (int)($m[3] ?? 0) * 60
                  + (int)($m[4] ?? 0);
    }
}

echo json_encode([
    'exists'   => $exists,
    'live'     => $liveBroadcastContent === 'live',
    'state'    => $liveBroadcastContent,  // 'live' | 'upcoming' | 'none'
    'duration' => $duration,              // whole seconds, or null if unknown
]);

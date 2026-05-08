<?php
header('Content-Type: application/json');

// Load environment variables from .env
$envFile = __DIR__ . '/.env';
if (file_exists($envFile)) {
    $lines = file($envFile, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) ?: [];
    foreach ($lines as $line) {
        if (strpos($line, '=') === false) {
            continue;
        }
        [$key, $value] = explode('=', $line, 2);
        if (trim($key) === 'YT_DATA_KEY') {
            $apiKey = trim(trim($value), '"\'');
        }
    }
}

if (!isset($apiKey)) {
    http_response_code(500);
    echo json_encode(['error' => 'API key not configured']);
    exit;
}

$channelId = 'UCeI47pYDX2iKt3h3iv5FLyA';

// Search for live broadcasts from the channel with "P.A.R." in title
$searchUrl = 'https://www.googleapis.com/youtube/v3/search?part=snippet&channelId=' . urlencode($channelId) . 
             '&type=video&eventType=live&order=date&relevanceLanguage=en&q=P.A.R.&key=' . urlencode($apiKey);

$response = @file_get_contents($searchUrl);
$data = json_decode($response, true);

if (isset($data['items']) && count($data['items']) > 0) {
    $livestream = $data['items'][0];
    echo json_encode([
        'videoId' => $livestream['id']['videoId'],
        'title' => $livestream['snippet']['title'],
        'description' => $livestream['snippet']['description'],
        'thumbnail' => $livestream['snippet']['thumbnails']['high']['url'] ?? null,
        'found' => true
    ]);
} else {
    echo json_encode(['videoId' => null, 'message' => 'No active livestream found', 'found' => false]);
}

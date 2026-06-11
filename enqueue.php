<?php
declare(strict_types=1);

const EXPECTED_BITMAP_BYTES = 84;
const MAX_QUEUE_LENGTH = 20;
const MAX_MOD_QUEUE_LENGTH = 50;
const MAX_CONCURRENT_REQUESTS = 5;

/**
 * @return resource|null
 */
function acquireConcurrencySlot(string $scriptName)
{
    $lockDir = __DIR__ . '/locks';
    if (!is_dir($lockDir) && !mkdir($lockDir, 0777, true) && !is_dir($lockDir)) {
        return null;
    }

    for ($slot = 1; $slot <= MAX_CONCURRENT_REQUESTS; $slot++) {
        $lockPath = sprintf('%s/%s.%d.lock', $lockDir, $scriptName, $slot);
        $handle = fopen($lockPath, 'c+');
        if ($handle === false) {
            continue;
        }

        if (flock($handle, LOCK_EX | LOCK_NB)) {
            return $handle;
        }

        fclose($handle);
    }

    return false;
}


$requestMethod = $_SERVER['REQUEST_METHOD'] ?? (PHP_SAPI === 'cli' ? 'GET' : null);

if ($requestMethod !== 'POST') {
    http_response_code(405);
    header('Allow: POST');
    header('Content-Type: application/json; charset=UTF-8');
    echo json_encode(['ok' => false, 'error' => 'POST only']);
    exit;
}

header('Content-Type: application/json; charset=UTF-8');
header('Cache-Control: no-store, no-cache, must-revalidate, max-age=0');

// Require application/json to force a CORS preflight on cross-origin requests
// (text/plain and form content-types are "simple" and bypass preflight, which
// would otherwise let any site submit on a victim's behalf).
$contentType = $_SERVER['CONTENT_TYPE'] ?? '';
$mimeType = strtolower(trim(explode(';', $contentType, 2)[0]));
if ($mimeType !== 'application/json') {
    http_response_code(415);
    echo json_encode(['ok' => false, 'error' => 'Content-Type must be application/json']);
    exit;
}

$concurrencyHandle = acquireConcurrencySlot('enqueue');
if ($concurrencyHandle === null) {
    http_response_code(500);
    echo json_encode(['ok' => false, 'error' => 'Could not initialize concurrency guard']);
    exit;
}

if ($concurrencyHandle === false) {
    http_response_code(429);
    echo json_encode(['ok' => false, 'error' => 'Server overloaded. Try again in a few seconds.']);
    exit;
}

register_shutdown_function(static function () use ($concurrencyHandle): void {
    flock($concurrencyHandle, LOCK_UN);
    fclose($concurrencyHandle);
});

$rawBody = file_get_contents('php://input');
$data = json_decode($rawBody ?: '', true);

if (!is_array($data) || !isset($data['item']) || !is_string($data['item'])) {
    http_response_code(400);
    echo json_encode(['ok' => false, 'error' => 'Missing queue item']);
    exit;
}

$decoded = base64_decode($data['item'], true);
if ($decoded === false || strlen($decoded) !== EXPECTED_BITMAP_BYTES) {
    http_response_code(400);
    echo json_encode(['ok' => false, 'error' => 'Invalid bitmap payload']);
    exit;
}

$name = '';
if (isset($data['name']) && is_string($data['name'])) {
    $name = mb_substr(trim($data['name']), 0, 100);
}

// Submissions now land in the moderation queue first. An external daemon
// (YT-Streamer/YT_streamer.py) runs Claude-based image + name checks and
// either auto-approves (promoting the entry into queue.txt via
// mod-action.php) or emails a human moderator.
$queueFile = __DIR__ . '/mod_queue.txt';
$mainQueueFile = __DIR__ . '/queue.txt';

if (!file_exists($queueFile)) {
    file_put_contents($queueFile, '');
}

$handle = fopen($queueFile, 'c+');
if ($handle === false) {
    http_response_code(500);
    echo json_encode(['ok' => false, 'error' => 'Could not open queue']);
    exit;
}

if (!flock($handle, LOCK_EX)) {
    fclose($handle);
    http_response_code(500);
    echo json_encode(['ok' => false, 'error' => 'Could not lock queue']);
    exit;
}

$contents = stream_get_contents($handle);
$lines = preg_split('/\R/', $contents ?: '') ?: [];
$lines = array_values(array_filter($lines, static fn (string $line): bool => trim($line) !== ''));

$existingBitmaps = array_map(static function (string $line): string {
    $decoded = json_decode($line, true);
    return is_array($decoded) ? ($decoded['item'] ?? $line) : $line;
}, $lines);

// Also reject if the same bitmap is already approved & waiting in the main queue.
$mainBitmaps = [];
foreach (@file($mainQueueFile, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) ?: [] as $mline) {
    $decoded = json_decode($mline, true);
    $mainBitmaps[] = is_array($decoded) ? ($decoded['item'] ?? $mline) : $mline;
}

if (in_array($data['item'], $existingBitmaps, true) || in_array($data['item'], $mainBitmaps, true)) {
    flock($handle, LOCK_UN);
    fclose($handle);
    http_response_code(409);
    echo json_encode(['ok' => false, 'error' => 'duplicate_queue_item']);
    exit;
}

if (count($lines) >= MAX_MOD_QUEUE_LENGTH) {
    flock($handle, LOCK_UN);
    fclose($handle);
    http_response_code(409);
    echo json_encode(['ok' => false, 'error' => 'queue_full']);
    exit;
}

$queueEntry = json_encode([
    'id'        => uniqid('', true),
    'item'      => $data['item'],
    'name'      => $name,
    'ts'        => time(),
    'status'    => 'pending',
    'status_ts' => time(),
], JSON_UNESCAPED_UNICODE);

fseek($handle, 0, SEEK_END);
fwrite($handle, $queueEntry . PHP_EOL);
fflush($handle);
flock($handle, LOCK_UN);
fclose($handle);

echo json_encode(['ok' => true]);

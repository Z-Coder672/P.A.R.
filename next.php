<?php
declare(strict_types=1);

const MAX_CONCURRENT_REQUESTS = 5;
const EXPECTED_BITMAP_BYTES = 84;

function nextGalleryIndex(string $galleryDir): int
{
    $max = 0;
    $entries = glob($galleryDir . '/*', GLOB_ONLYDIR) ?: [];
    foreach ($entries as $dir) {
        $base = basename($dir);
        if (ctype_digit($base)) {
            $max = max($max, (int)$base);
        }
    }
    return $max + 1;
}

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

// POST-only: this endpoint pops a queue item per call, so it must not be
// replay-safe at the HTTP layer. GET is spec-allowed to be retried by any
// intermediate (Cloudflare retries on origin connect-fail/5xx); a silent
// retry would pop a second item that the Arduino never sees, leaving an
// orphan gallery/<N>/pending.json. POST is not retried by default.
$requestMethod = $_SERVER['REQUEST_METHOD'] ?? (PHP_SAPI === 'cli' ? 'POST' : null);

if ($requestMethod !== 'POST') {
    http_response_code(405);
    header('Allow: POST');
    header('Content-Type: text/plain; charset=UTF-8');
    echo 'POST only';
    exit;
}

header('Content-Type: text/plain; charset=UTF-8');
header('Cache-Control: no-store, no-cache, must-revalidate, max-age=0');

$concurrencyHandle = acquireConcurrencySlot('next');
if ($concurrencyHandle === null) {
    http_response_code(500);
    echo 'NONE';
    exit;
}

if ($concurrencyHandle === false) {
    http_response_code(429);
    echo 'Server overloaded. Try again in a few seconds.';
    exit;
}

register_shutdown_function(static function () use ($concurrencyHandle): void {
    flock($concurrencyHandle, LOCK_UN);
    fclose($concurrencyHandle);
});

$queueFile = __DIR__ . '/queue.txt';

if (!file_exists($queueFile)) {
    file_put_contents($queueFile, '');
}

$handle = fopen($queueFile, 'c+');
if ($handle === false) {
    http_response_code(500);
    echo 'NONE';
    exit;
}

if (!flock($handle, LOCK_EX)) {
    fclose($handle);
    http_response_code(500);
    echo 'NONE';
    exit;
}

$contents = stream_get_contents($handle);
$lines = preg_split('/\R/', $contents ?: '') ?: [];
$lines = array_values(array_filter($lines, static fn (string $line): bool => trim($line) !== ''));

if ($lines === []) {
    flock($handle, LOCK_UN);
    fclose($handle);
    echo 'NONE';
    exit;
}

$nextItem = array_shift($lines);

rewind($handle);
ftruncate($handle, 0);
if ($lines !== []) {
    fwrite($handle, implode(PHP_EOL, $lines) . PHP_EOL);
}

fflush($handle);
flock($handle, LOCK_UN);
fclose($handle);

$parsed = json_decode($nextItem, true);
$bitmapBase64 = is_array($parsed) ? ($parsed['item'] ?? $nextItem) : $nextItem;
$itemName = is_array($parsed) ? ($parsed['name'] ?? '') : '';

$galleryDir = __DIR__ . '/gallery';
if (!is_dir($galleryDir)) {
    mkdir($galleryDir, 0777, true);
}
$galleryIndex = nextGalleryIndex($galleryDir);
$entryDir = $galleryDir . '/' . $galleryIndex;
if (mkdir($entryDir, 0777, true)) {
    $decoded = base64_decode($bitmapBase64, true);
    if ($decoded !== false && strlen($decoded) === EXPECTED_BITMAP_BYTES) {
        file_put_contents(
            $entryDir . '/pending.json',
            json_encode(['name' => $itemName, 'bitmap' => $bitmapBase64], JSON_UNESCAPED_UNICODE)
        );
        header('X-Gallery-Id: ' . $galleryIndex);
        file_put_contents(__DIR__ . '/snapshot-pending.flag', (string)$galleryIndex);
        // Stream-restart flag: YT-Streamer polls this to retitle the
        // YouTube broadcast with the art-piece name when a new print
        // starts. Stale flags (>10 min) get dropped on the next write or
        // by stream-start.php on read, so a queued-but-never-picked-up
        // print doesn't retitle a much-later stream.
        $streamFlag = __DIR__ . '/stream-pending.flag';
        $existing = @filemtime($streamFlag);
        if ($existing !== false && (time() - $existing) > 600) {
            @unlink($streamFlag);
        }
        file_put_contents($streamFlag, json_encode(
            ['id' => $galleryIndex, 'name' => $itemName],
            JSON_UNESCAPED_UNICODE
        ));
    }
}

echo $bitmapBase64;

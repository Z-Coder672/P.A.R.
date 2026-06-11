<?php
declare(strict_types=1);

// Returns up to BATCH_SIZE pending submissions from mod_queue.txt, marks
// them as "processing" so they aren't returned again on subsequent polls.
// Stale "processing" entries (>10 min) are auto-reset to "pending" in case
// the moderator daemon crashed mid-check.

const BATCH_SIZE = 5;
const STALE_PROCESSING_SECONDS = 600;

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

$queueFile = __DIR__ . '/mod_queue.txt';
if (!file_exists($queueFile)) {
    http_response_code(204);
    exit;
}

$handle = fopen($queueFile, 'c+');
if ($handle === false || !flock($handle, LOCK_EX)) {
    http_response_code(500);
    echo json_encode(['ok' => false, 'error' => 'lock_failed']);
    exit;
}

$contents = stream_get_contents($handle);
$lines = preg_split('/\R/', $contents ?: '') ?: [];
$lines = array_values(array_filter($lines, static fn (string $l): bool => trim($l) !== ''));

$now = time();
$returned = [];
$updated = [];

foreach ($lines as $line) {
    $entry = json_decode($line, true);
    if (!is_array($entry)) {
        $updated[] = $line;
        continue;
    }

    // Backfill legacy entries (pre-moderation schema: just item + name).
    // Without this they'd be skipped forever.
    if (!isset($entry['id'])) {
        $entry['id']        = uniqid('', true);
        $entry['status']    = 'pending';
        $entry['status_ts'] = $now;
        if (!isset($entry['ts'])) {
            $entry['ts'] = $now;
        }
    }

    $status = $entry['status'] ?? 'pending';
    $statusTs = (int)($entry['status_ts'] ?? 0);

    if ($status === 'processing' && ($now - $statusTs) > STALE_PROCESSING_SECONDS) {
        $status = 'pending';
    }

    if ($status === 'pending' && count($returned) < BATCH_SIZE) {
        $returned[] = [
            'id'        => (string)$entry['id'],
            'image_b64' => (string)($entry['item'] ?? ''),
            'name'      => (string)($entry['name'] ?? ''),
        ];
        $entry['status']    = 'processing';
        $entry['status_ts'] = $now;
    } elseif ($status !== ($entry['status'] ?? 'pending')) {
        $entry['status']    = $status;
        $entry['status_ts'] = $now;
    }

    $updated[] = json_encode($entry, JSON_UNESCAPED_UNICODE);
}

if (empty($returned)) {
    flock($handle, LOCK_UN);
    fclose($handle);
    http_response_code(204);
    exit;
}

ftruncate($handle, 0);
rewind($handle);
fwrite($handle, implode(PHP_EOL, $updated) . PHP_EOL);
fflush($handle);
flock($handle, LOCK_UN);
fclose($handle);

echo json_encode(['ok' => true, 'items' => $returned]);

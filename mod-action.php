<?php
declare(strict_types=1);

// Apply a moderation verdict to an item in mod_queue.txt.
//
// Accepts GET (for one-click moderator email links) or POST. Auth via
// `secret` query/form param or X-Snapshot-Secret header — reused from the
// snapshot endpoints since it's the same trust boundary (the Mac Mini).
//
// Verdicts:
//   approve     — remove from mod_queue.txt, append to queue.txt
//   reject      — remove from mod_queue.txt
//   email_sent  — mark status=email_sent so mod-next.php stops returning it
//                 while a human reviews via the email link (called by Python)

const MAX_MAIN_QUEUE_LENGTH = 20;

foreach (@file(__DIR__ . '/.env', FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) ?: [] as $line) {
    if (strpos($line, '=') !== false) {
        [$k, $v] = explode('=', $line, 2);
        putenv(trim($k) . '=' . trim(trim($v), '"\''));
    }
}

$secret = getenv('SNAPSHOT_SECRET') ?: '';
$provided = $_REQUEST['secret'] ?? ($_SERVER['HTTP_X_SNAPSHOT_SECRET'] ?? '');
if (!is_string($provided)) {
    $provided = '';
}
if ($secret === '' || !hash_equals($secret, $provided)) {
    http_response_code(401);
    exit;
}

$id      = $_REQUEST['id']      ?? '';
$verdict = $_REQUEST['verdict'] ?? '';

if (!is_string($id) || $id === '' || !preg_match('/^[A-Za-z0-9._-]{1,64}$/', $id)) {
    http_response_code(400);
    echo 'bad id';
    exit;
}
if (!in_array($verdict, ['approve', 'reject', 'email_sent'], true)) {
    http_response_code(400);
    echo 'bad verdict';
    exit;
}

$modFile  = __DIR__ . '/mod_queue.txt';
$mainFile = __DIR__ . '/queue.txt';

$handle = fopen($modFile, 'c+');
if ($handle === false || !flock($handle, LOCK_EX)) {
    http_response_code(500);
    echo 'lock_failed';
    exit;
}

$contents = stream_get_contents($handle);
$lines = preg_split('/\R/', $contents ?: '') ?: [];
$lines = array_values(array_filter($lines, static fn (string $l): bool => trim($l) !== ''));

$found = null;
$remaining = [];
foreach ($lines as $line) {
    $entry = json_decode($line, true);
    if (is_array($entry) && ($entry['id'] ?? '') === $id) {
        $found = $entry;
        if ($verdict === 'email_sent') {
            $entry['status']    = 'email_sent';
            $entry['status_ts'] = time();
            $remaining[] = json_encode($entry, JSON_UNESCAPED_UNICODE);
        }
        // approve/reject: drop from mod queue
        continue;
    }
    $remaining[] = $line;
}

if ($found === null) {
    flock($handle, LOCK_UN);
    fclose($handle);
    http_response_code(404);
    echo 'not_found';
    exit;
}

if ($verdict === 'approve') {
    // Append to main queue. We bypass MAX_QUEUE_LENGTH because moderation
    // is an explicit admin action — but we still cap it generously so a
    // bug can't blow it up.
    if (!file_exists($mainFile)) {
        file_put_contents($mainFile, '');
    }
    $mainHandle = fopen($mainFile, 'c+');
    if ($mainHandle !== false && flock($mainHandle, LOCK_EX)) {
        $mainContents = stream_get_contents($mainHandle);
        $mainLines = preg_split('/\R/', $mainContents ?: '') ?: [];
        $mainLines = array_values(array_filter($mainLines, static fn (string $l): bool => trim($l) !== ''));
        if (count($mainLines) < MAX_MAIN_QUEUE_LENGTH) {
            $promoted = json_encode(
                ['item' => $found['item'] ?? '', 'name' => $found['name'] ?? ''],
                JSON_UNESCAPED_UNICODE
            );
            fseek($mainHandle, 0, SEEK_END);
            fwrite($mainHandle, $promoted . PHP_EOL);
            fflush($mainHandle);
        }
        flock($mainHandle, LOCK_UN);
        fclose($mainHandle);
    }
}

ftruncate($handle, 0);
rewind($handle);
if (!empty($remaining)) {
    fwrite($handle, implode(PHP_EOL, $remaining) . PHP_EOL);
}
fflush($handle);
flock($handle, LOCK_UN);
fclose($handle);

// Friendly response for the email-link case.
$accept = $_SERVER['HTTP_ACCEPT'] ?? '';
if (strpos($accept, 'text/html') !== false || ($_SERVER['REQUEST_METHOD'] ?? '') === 'GET') {
    header('Content-Type: text/html; charset=UTF-8');
    $verb = htmlspecialchars($verdict, ENT_QUOTES);
    $safeId = htmlspecialchars($id, ENT_QUOTES);
    echo "<!doctype html><meta charset=utf-8><title>Mod action</title>
<body style='font-family:system-ui;padding:2rem;max-width:36rem'>
<h2>Action recorded</h2>
<p>Submission <code>{$safeId}</code> → <strong>{$verb}</strong></p>
</body>";
    exit;
}

header('Content-Type: application/json; charset=UTF-8');
echo json_encode(['ok' => true, 'id' => $id, 'verdict' => $verdict]);

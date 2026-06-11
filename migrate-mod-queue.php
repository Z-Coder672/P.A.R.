<?php
declare(strict_types=1);

// One-shot: backfill missing id/status/status_ts/ts on legacy mod_queue.txt
// entries so mod-next.php stops skipping them.
//
// Usage: upload, hit https://par.zimmzimm.com/migrate-mod-queue.php?secret=...
// in a browser, confirm the count, DELETE THIS FILE.

foreach (@file(__DIR__ . '/.env', FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) ?: [] as $line) {
    if (strpos($line, '=') !== false) {
        [$k, $v] = explode('=', $line, 2);
        putenv(trim($k) . '=' . trim(trim($v), '"\''));
    }
}

$secret   = getenv('SNAPSHOT_SECRET') ?: '';
$provided = $_GET['secret'] ?? $_POST['secret'] ?? '';
if (!is_string($provided) || $secret === '' || !hash_equals($secret, $provided)) {
    http_response_code(401);
    exit('unauthorized');
}

header('Content-Type: text/plain; charset=UTF-8');

echo "script dir:  " . __DIR__ . "\n";
echo "script path: " . __FILE__ . "\n";

$file = __DIR__ . '/mod_queue.txt';
echo "target file: {$file}\n";
echo "file exists: " . (file_exists($file) ? "yes (" . filesize($file) . " bytes)" : "no") . "\n\n";

if (!file_exists($file)) {
    exit("mod_queue.txt does not exist at this path — nothing to migrate.\n"
       . "Check that this script is in the SAME folder as mod-next.php.\n");
}

$handle = fopen($file, 'c+');
if ($handle === false || !flock($handle, LOCK_EX)) {
    http_response_code(500);
    exit("lock_failed\n");
}

$contents = stream_get_contents($handle);
$lines = preg_split('/\R/', $contents ?: '') ?: [];
$lines = array_values(array_filter($lines, static fn (string $l): bool => trim($l) !== ''));

$now = time();
$out = [];
$migrated = 0;
$untouched = 0;
$dropped = 0;

foreach ($lines as $line) {
    $entry = json_decode($line, true);
    if (!is_array($entry) || !isset($entry['item'])) {
        $dropped++;
        continue;
    }
    $changed = false;
    if (!isset($entry['id']))        { $entry['id']        = uniqid('', true); $changed = true; }
    if (!isset($entry['status']))    { $entry['status']    = 'pending';        $changed = true; }
    if (!isset($entry['status_ts'])) { $entry['status_ts'] = $now;             $changed = true; }
    if (!isset($entry['ts']))        { $entry['ts']        = $now;             $changed = true; }
    $changed ? $migrated++ : $untouched++;
    $out[] = json_encode($entry, JSON_UNESCAPED_UNICODE);
}

ftruncate($handle, 0);
rewind($handle);
if (!empty($out)) {
    fwrite($handle, implode(PHP_EOL, $out) . PHP_EOL);
}
fflush($handle);
flock($handle, LOCK_UN);
fclose($handle);

echo "migrated:  {$migrated}\n";
echo "untouched: {$untouched}\n";
echo "dropped:   {$dropped} (no 'item' field)\n";
echo "total now: " . count($out) . "\n";
echo "\nDELETE THIS FILE NOW.\n";

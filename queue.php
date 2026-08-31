<?php
declare(strict_types=1);

require_once __DIR__ . '/lib/ratelimit.php';
par_rate_limit('queue', [
    'ip_rate'     => 0.5,
    'ip_burst'    => 30,
    'global_rate' => 5.0,
    'global_burst' => 100,
]);

header('Content-Type: application/json; charset=UTF-8');
header('Cache-Control: no-store, no-cache, must-revalidate, max-age=0');

$queueFile = __DIR__ . '/queue.txt';
$items = [];

if (file_exists($queueFile)) {
    $lines = @file($queueFile, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) ?: [];
    foreach ($lines as $i => $line) {
        $decoded = json_decode($line, true);
        if (!is_array($decoded)) {
            continue;
        }
        $bitmap = (string)($decoded['item'] ?? '');
        if ($bitmap === '') {
            continue;
        }
        $items[] = [
            'position' => $i + 1,
            'bitmap'   => $bitmap,
            'name'     => (string)($decoded['name'] ?? ''),
            'artist'   => (string)($decoded['artist'] ?? ''),
        ];
    }
}

echo json_encode(['ok' => true, 'items' => $items]);

<?php
declare(strict_types=1);

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
        ];
    }
}

echo json_encode(['ok' => true, 'items' => $items]);

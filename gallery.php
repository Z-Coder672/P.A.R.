<?php
declare(strict_types=1);

header('Content-Type: application/json; charset=UTF-8');
header('Cache-Control: no-store, no-cache, must-revalidate, max-age=0');

$galleryDir = __DIR__ . '/gallery';
$items = [];

if (is_dir($galleryDir)) {
    $entries = glob($galleryDir . '/*', GLOB_ONLYDIR) ?: [];
    foreach ($entries as $dir) {
        $id = basename($dir);
        if (!ctype_digit($id)) {
            continue;
        }

        $infoPath    = $dir . '/info.json';
        $pendingPath = $dir . '/pending.json';

        if (file_exists($infoPath)) {
            $jsonPath  = $infoPath;
            $isPending = false;
        } elseif (file_exists($pendingPath)) {
            $jsonPath  = $pendingPath;
            $isPending = true;
        } else {
            continue;
        }

        $raw = file_get_contents($jsonPath);
        if ($raw === false) {
            continue;
        }

        $info = json_decode($raw, true);
        if (!is_array($info)) {
            continue;
        }

        $entry = [
            'id'       => (int)$id,
            'pending'  => $isPending,
            'bitmap'   => (string)($info['bitmap'] ?? ''),
            'name'     => (string)($info['name'] ?? ''),
            'video_id' => isset($info['video_id']) && is_string($info['video_id'])
                ? $info['video_id'] : null,
        ];

        $entry['image'] = null;
        foreach (['image.jpg', 'image.png'] as $imageName) {
            $imagePath = $dir . '/' . $imageName;
            if (file_exists($imagePath)) {
                $entry['image'] = 'gallery/' . $id . '/' . $imageName . '?v=' . filemtime($imagePath);
                break;
            }
        }

        $items[] = $entry;
    }

    usort($items, static fn(array $a, array $b): int => $b['id'] - $a['id']);
}

echo json_encode(['ok' => true, 'items' => $items]);

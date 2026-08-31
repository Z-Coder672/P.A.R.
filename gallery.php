<?php
declare(strict_types=1);

require_once __DIR__ . '/lib/ratelimit.php';
par_rate_limit('gallery', [
    'ip_rate'     => 0.5,  // ~30/min per IP (frontend polls every 5s = 12/min)
    'ip_burst'    => 30,
    'global_rate' => 5.0,
    'global_burst' => 100,
]);

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
            // '' for legacy entries and for submissions that left it blank --
            // the frontend omits the "by ..." line entirely in that case.
            'artist'   => (string)($info['artist'] ?? ''),
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

        // A photo only ever lands after the board has displayed and been shot
        // (post check-pass), so image.jpg present means the print is done — treat
        // the entry as completed even if its file is still pending.json (e.g. the
        // pending->info rename was lost to a dropped/failed call). Without this,
        // the modal would show "In progress" on top of a finished print that
        // already has a real photo.
        if ($entry['image'] !== null) {
            $entry['pending'] = false;
        }

        $items[] = $entry;
    }

    usort($items, static fn(array $a, array $b): int => $b['id'] - $a['id']);
}

echo json_encode(['ok' => true, 'items' => $items]);

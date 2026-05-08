<?php
declare(strict_types=1);

// One-shot migration: rewrite legacy 38×18 (86-byte / 116 base64 chars) bitmaps
// in gallery/<N>/info.json or pending.json into the new 37×18 (84-byte / 112
// base64 chars) packing by dropping the leftmost column. Idempotent —
// already-37-wide entries are skipped, anything else is left untouched and
// reported.
//
// CLI usage:
//   php migrate-bitmap-37.php           # dry run
//   php migrate-bitmap-37.php --apply   # write changes back
//
// HTTP usage (for servers without shell access):
//   POST /migrate-bitmap-37.php
//     X-Snapshot-Secret: <SNAPSHOT_SECRET from .env>   (or `secret` form field)
//     apply=1                                          (omit for dry run)
//
//   curl -X POST -H "X-Snapshot-Secret: $SECRET" \
//        -d 'apply=1' https://par.zimmzimm.com/migrate-bitmap-37.php
//
// Returns text/plain log + summary; HTTP 401 on bad/missing secret.

const OLD_W = 38;
const NEW_W = 37;
const GRID_H = 18;
const OLD_BYTES = 86;  // ceil(38*18/8)
const NEW_BYTES = 84;  // ceil(37*18/8)

$isCli = (PHP_SAPI === 'cli');

if (!$isCli) {
    // HTTP path: require POST + matching SNAPSHOT_SECRET, same pattern as
    // snapshot-clear.php / snapshot-next.php / snapshot-request.php.
    if (($_SERVER['REQUEST_METHOD'] ?? '') !== 'POST') {
        http_response_code(405);
        header('Content-Type: text/plain; charset=utf-8');
        echo "POST only.\n";
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
        header('Content-Type: text/plain; charset=utf-8');
        echo "Unauthorized.\n";
        exit;
    }

    header('Content-Type: text/plain; charset=utf-8');
    $apply = !empty($_POST['apply']) && $_POST['apply'] !== '0';
} else {
    $apply = in_array('--apply', $argv ?? [], true);
}

function convert38to37(string $b64): string
{
    $raw = base64_decode($b64, true);
    if ($raw === false || strlen($raw) !== OLD_BYTES) {
        throw new RuntimeException('expected ' . OLD_BYTES . '-byte bitmap');
    }

    // Drop column x=0 from each row, repack MSB-first.
    $newBits = [];
    for ($y = 0; $y < GRID_H; $y++) {
        for ($x = 1; $x < OLD_W; $x++) {
            $i = $y * OLD_W + $x;
            $newBits[] = (ord($raw[$i >> 3]) >> (7 - ($i & 7))) & 1;
        }
    }
    $totalBits = GRID_H * NEW_W;

    $out = '';
    for ($byte = 0; $byte < NEW_BYTES; $byte++) {
        $b = 0;
        for ($k = 0; $k < 8; $k++) {
            $idx = $byte * 8 + $k;
            if ($idx < $totalBits && $newBits[$idx]) {
                $b |= 1 << (7 - $k);
            }
        }
        $out .= chr($b);
    }
    return base64_encode($out);
}

$galleryDir = __DIR__ . '/gallery';
if (!is_dir($galleryDir)) {
    echo "No gallery/ directory found.\n";
    exit($isCli ? 1 : 0);
}

$converted = 0;
$skipped = 0;
$bad = 0;

$entries = glob($galleryDir . '/*', GLOB_ONLYDIR) ?: [];
foreach ($entries as $dir) {
    foreach (['info.json', 'pending.json'] as $name) {
        $path = $dir . '/' . $name;
        if (!is_file($path)) {
            continue;
        }
        $raw = file_get_contents($path);
        if ($raw === false) {
            $bad++;
            echo "skip (read failed): $path\n";
            continue;
        }
        $obj = json_decode($raw, true);
        if (!is_array($obj) || !isset($obj['bitmap']) || !is_string($obj['bitmap'])) {
            $bad++;
            echo "skip (no bitmap field): $path\n";
            continue;
        }
        $decoded = base64_decode($obj['bitmap'], true);
        if ($decoded === false) {
            $bad++;
            echo "skip (bad base64): $path\n";
            continue;
        }
        $len = strlen($decoded);
        if ($len === NEW_BYTES) {
            $skipped++;
            continue;
        }
        if ($len !== OLD_BYTES) {
            $bad++;
            echo "skip ({$len}-byte bitmap, expected " . OLD_BYTES . " or " . NEW_BYTES . "): $path\n";
            continue;
        }

        $obj['bitmap'] = convert38to37($obj['bitmap']);
        $newJson = json_encode($obj, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
        if ($newJson === false) {
            $bad++;
            echo "skip (re-encode failed): $path\n";
            continue;
        }

        echo ($apply ? '[apply] ' : '[dry  ] ') . $path . "\n";
        if ($apply) {
            $tmp = $path . '.tmp';
            $ok = file_put_contents($tmp, $newJson) !== false && rename($tmp, $path);
            if (!$ok) {
                @unlink($tmp);
                $bad++;
                echo "write failed: $path\n";
                continue;
            }
        }
        $converted++;
    }
}

echo "\n";
echo "Converted: $converted\n";
echo "Skipped (already 37-wide): $skipped\n";
echo "Bad / unknown: $bad\n";
echo $apply ? "Applied.\n" : "Dry run — re-run with apply=1 (HTTP) or --apply (CLI) to write.\n";

<?php
// qr-count.php — tracks QR-code visits.
//
// QR codes point at the site with `?q=1` in the query string. Both the dev
// router and production .htaccess route any request carrying that param here.
// This endpoint atomically bumps an integer counter in qr-visits.txt and then
// serves the normal SPA (index.html), so the visitor lands on the site exactly
// as if the param weren't there — the count is a transparent side effect.
//
// Reading the count is PASSWORD-PROTECTED: GET qr-count.php?stats=1 prompts for
// HTTP Basic Auth (any username; the password must equal QR_STATS_SECRET in the
// webroot .env) and returns the current integer as text/plain. For scripts, the
// password may instead be passed as ?key=<secret>. The raw qr-visits.txt is
// denied by .htaccess, so this is the only web-facing way to read the count —
// and without the secret it reveals nothing.

require_once __DIR__ . '/lib/env.php';

$counterFile = __DIR__ . '/qr-visits.txt';

// --- Stats read: return the current count without incrementing -------------
if (isset($_GET['stats'])) {
    $secret = par_env('QR_STATS_SECRET');

    // Fail closed: if no secret is configured, the count is never readable.
    if ($secret === null || $secret === '') {
        header('Cache-Control: no-store');
        http_response_code(503);
        echo 'stats disabled';
        exit;
    }

    // Password may arrive via Basic Auth (browser prompt) or ?key= (scripts).
    $supplied = $_SERVER['PHP_AUTH_PW'] ?? ($_GET['key'] ?? null);

    if (!is_string($supplied) || !hash_equals($secret, $supplied)) {
        header('WWW-Authenticate: Basic realm="QR stats"');
        header('Cache-Control: no-store');
        http_response_code(401);
        echo 'unauthorized';
        exit;
    }

    header('Content-Type: text/plain; charset=utf-8');
    header('Cache-Control: no-store');
    $raw = @file_get_contents($counterFile);
    echo (string) (int) ($raw === false ? 0 : $raw);
    exit;
}

// --- Increment (atomic, fail-open) -----------------------------------------
// Never let a counter problem block the page: any FS error just skips the
// bump and still serves the site.
$fh = @fopen($counterFile, 'c+');
if ($fh !== false) {
    if (flock($fh, LOCK_EX)) {
        $raw = stream_get_contents($fh);
        $count = (int) $raw + 1;
        rewind($fh);
        ftruncate($fh, 0);
        fwrite($fh, (string) $count);
        fflush($fh);
        flock($fh, LOCK_UN);
    }
    fclose($fh);
}

// --- Serve the normal single-page app --------------------------------------
require __DIR__ . '/index.html';

<?php
declare(strict_types=1);

// "Text a friend about P.A.R." — the https landing the completion email's SMS
// button points at.
//
// Why this file exists: Gmail (and most webmail) sanitize anchor hrefs down to
// http/https/mailto and silently strip every other scheme, so a bare
// `sms:?&body=...` href renders as styled text that isn't a link at all. The
// email links here instead, and the handoff to the messaging app happens from
// a real page, where the scheme is allowed.
//
// Nothing here is user-controlled: the body is built server-side and the only
// input is a digits-only gallery id, so this cannot be turned into an open
// redirect or a way to make the site send arbitrary text.

require_once __DIR__ . '/lib/ratelimit.php';
require_once __DIR__ . '/lib/private_store.php';

par_rate_limit('share-sms', [
    'ip_rate'      => 0.5,   // ~30/min per IP; this is a one-shot click
    'ip_burst'     => 15,
    'global_rate'  => 5.0,
    'global_burst' => 60,
]);

$base = par_env('SITE_BASE_URL') ?: 'https://par.zimmzimm.com';
$base = rtrim($base, '/');

$galleryUrl = $base . '/gallery';
$id = $_GET['id'] ?? '';
if (is_string($id) && $id !== '' && preg_match('/^\d{1,9}$/', $id)) {
    $galleryUrl .= '#' . $id;
}
$uploadUrl = $base . '/upload';

// No recipient number — the messaging app opens with the body prefilled and
// the sender picks who to send it to. `?&body=` is the form both iOS and
// Android accept; the bare `?body=` / `&body=` variants each miss one.
$body   = par_share_blurb($galleryUrl, $uploadUrl);
$smsUrl = 'sms:?&body=' . rawurlencode($body);

$safeSms  = htmlspecialchars($smsUrl, ENT_QUOTES, 'UTF-8');
$safeBody = htmlspecialchars($body, ENT_QUOTES, 'UTF-8');
$safeSite = htmlspecialchars($base, ENT_QUOTES, 'UTF-8');

header('Content-Type: text/html; charset=UTF-8');
header('Cache-Control: no-store');
// A page, not a 302: redirecting straight to a non-http scheme is blocked by
// some browsers, and on desktop (where sms: does nothing at all) it would look
// like a dead link. The tap below is a real user gesture, and the copy box is
// the desktop fallback.
?>
<!doctype html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="robots" content="noindex">
<title>Text a friend about P.A.R.</title>
<style>
    body { margin: 0; padding: 2rem 1.25rem; background: #111; color: #fff;
           font-family: system-ui, -apple-system, sans-serif; text-align: center; }
    .wrap { max-width: 32rem; margin: 0 auto; }
    h1 { color: #02b2d9; font-size: 1.4rem; margin: 0 0 1rem; }
    p { color: #bbb; line-height: 1.5; }
    .btn { display: inline-block; margin: 1.25rem 0; padding: 0.85rem 1.5rem;
           background: #02b2d9; color: #fff; border-radius: 6px;
           text-decoration: none; font-weight: 600; font-size: 1.05rem; }
    .msg { background: #1a1a1a; border: 1px solid #333; border-radius: 6px;
           padding: 1rem; color: #ddd; text-align: left;
           /* Long unbroken URLs must wrap rather than widen the page. */
           overflow-wrap: anywhere; word-break: break-word;
           white-space: pre-wrap; font-size: 0.95rem; }
    .hint { font-size: 0.85rem; color: #888; margin-top: 1.5rem; }
    a.home { color: #02b2d9; }
</style>
</head>
<body>
<div class="wrap">
    <h1>Text a friend about P.A.R.</h1>
    <p>Tap below to open your messaging app with this already written &mdash;
       just pick who to send it to.</p>
    <p><a class="btn" id="smsLink" href="<?= $safeSms ?>">Open Messages</a></p>
    <div class="msg" id="msgText"><?= $safeBody ?></div>
    <p class="hint">On a computer? Copy the text above and send it however you like.<br>
       <a class="home" href="<?= $safeSite ?>/">Back to P.A.R.</a></p>
</div>
<script>
// Best-effort auto-open on touch devices; the button remains the reliable path
// (and the only one on desktop, where the sms: scheme has no handler).
(function () {
    if (!window.matchMedia || !window.matchMedia('(pointer: coarse)').matches) return;
    setTimeout(function () {
        try { window.location.href = document.getElementById('smsLink').href; } catch (e) {}
    }, 350);
})();
</script>
</body>
</html>

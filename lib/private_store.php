<?php
declare(strict_types=1);

// Secure, off-webroot store for submitter email addresses + the completion
// notifier. Emails are NEVER written to any web-served file (queue.txt,
// mod_queue.txt, gallery/<N>/*.json) — only an opaque submission id travels
// through those. The address itself lives encrypted (libsodium secretbox)
// under PRIVATE_DIR, which resolves ABOVE the document root by default.
//
// Never requested directly (404 guard + lib/ denied in .htaccess).
if (isset($_SERVER['SCRIPT_FILENAME']) && @realpath($_SERVER['SCRIPT_FILENAME']) === __FILE__) {
    http_response_code(404);
    exit;
}

require_once __DIR__ . '/env.php';

/**
 * Absolute path to the private (off-webroot) data directory. Configurable via
 * PRIVATE_DIR in .env; defaults to a sibling of the document root so it is not
 * reachable by any URL. Created on first use with a hardening .htaccess as a
 * defense-in-depth second line in case it ever ends up inside a served tree.
 */
function par_private_dir(): string
{
    static $dir = null;
    if ($dir !== null) {
        return $dir;
    }
    $configured = par_env('PRIVATE_DIR');
    $dir = (is_string($configured) && $configured !== '')
        ? rtrim($configured, '/')
        : dirname(__DIR__) . '/../par-private';

    if (!is_dir($dir)) {
        @mkdir($dir, 0700, true);
    }
    // Defense-in-depth: if this dir is ever served, deny everything.
    $deny = $dir . '/.htaccess';
    if (is_dir($dir) && !is_file($deny)) {
        @file_put_contents($deny, "Require all denied\n");
    }
    return $dir;
}

function par_emails_dir(): string
{
    $d = par_private_dir() . '/emails';
    if (!is_dir($d)) {
        @mkdir($d, 0700, true);
    }
    return $d;
}

function par_valid_subid(string $id): bool
{
    return (bool) preg_match('/^[A-Za-z0-9._-]{1,64}$/', $id);
}

/** Path to the encryption key file — lives ABOVE the document root. */
function par_email_key_path(): string
{
    return par_private_dir() . '/email.key';
}

/**
 * 32-byte secretbox key, read from PRIVATE_DIR/email.key (base64) — a file that
 * lives ABOVE the document root, deliberately NOT the webroot .env. A
 * webroot-only exposure (a misconfigured server serving .env, a scanner hitting
 * /.env) therefore can never reach the key. Returns null if the file is
 * missing/malformed, in which case email storage + notification are silently
 * disabled (enqueue still succeeds — fail-safe). Read once per request.
 */
function par_email_key(): ?string
{
    static $key = false; // false = not yet attempted
    if ($key !== false) {
        return $key;
    }
    $key = null;
    $raw = @file_get_contents(par_email_key_path());
    if ($raw === false) {
        return null;
    }
    $decoded = base64_decode(trim($raw), true);
    if ($decoded === false || strlen($decoded) !== SODIUM_CRYPTO_SECRETBOX_KEYBYTES) {
        return null;
    }
    $key = $decoded;
    return $key;
}

/** Returns base64(nonce . ciphertext), or null if encryption is unavailable. */
function par_email_encrypt(string $plaintext): ?string
{
    $key = par_email_key();
    if ($key === null) {
        return null;
    }
    $nonce = random_bytes(SODIUM_CRYPTO_SECRETBOX_NONCEBYTES);
    $cipher = sodium_crypto_secretbox($plaintext, $nonce, $key);
    return base64_encode($nonce . $cipher);
}

function par_email_decrypt(string $blob): ?string
{
    $key = par_email_key();
    if ($key === null) {
        return null;
    }
    $raw = base64_decode($blob, true);
    if ($raw === false || strlen($raw) <= SODIUM_CRYPTO_SECRETBOX_NONCEBYTES) {
        return null;
    }
    $nonce = substr($raw, 0, SODIUM_CRYPTO_SECRETBOX_NONCEBYTES);
    $cipher = substr($raw, SODIUM_CRYPTO_SECRETBOX_NONCEBYTES);
    $plain = sodium_crypto_secretbox_open($cipher, $nonce, $key);
    return $plain === false ? null : $plain;
}

/**
 * Store the encrypted email for a submission (keyed by the opaque submission
 * id). Returns false if crypto/storage is unavailable — callers treat that as
 * "no notification will be sent" and continue normally.
 */
function par_store_submission_email(string $subId, string $email, string $name): bool
{
    if (!par_valid_subid($subId)) {
        return false;
    }
    $blob = par_email_encrypt((string) json_encode(
        ['email' => $email, 'name' => $name, 'ts' => time()],
        JSON_UNESCAPED_UNICODE
    ));
    if ($blob === null) {
        return false;
    }
    $path = par_emails_dir() . '/sub-' . $subId . '.enc';
    return file_put_contents($path, $blob, LOCK_EX) !== false;
}

/** Drop a stored submission email (e.g. on moderation reject). */
function par_delete_submission_email(string $subId): void
{
    if (par_valid_subid($subId)) {
        @unlink(par_emails_dir() . '/sub-' . $subId . '.enc');
    }
}

/**
 * Bind a stored submission email to a gallery id once the Arduino has picked
 * the item up. Renames sub-<subId>.enc -> gid-<N>.enc; no-op if there was no
 * email for this submission.
 */
function par_bind_email_to_gallery(string $subId, int $galleryId): void
{
    if (!par_valid_subid($subId) || $galleryId <= 0) {
        return;
    }
    $src = par_emails_dir() . '/sub-' . $subId . '.enc';
    if (is_file($src)) {
        @rename($src, par_emails_dir() . '/gid-' . $galleryId . '.enc');
    }
}

/**
 * Atomically claim the email owed for a completed gallery entry. The claim is
 * an atomic rename (gid-N.enc -> gid-N.sending) so concurrent/duplicate
 * "print done" calls (snapshot-request.php + the redundant complete.php, plus
 * device retries) can't double-send: only the caller that wins the rename gets
 * the record back. Returns ['email'=>..,'name'=>..] or null if nothing is owed
 * / already claimed. On a send failure, call par_unclaim_gallery_email().
 */
function par_claim_gallery_email(int $galleryId): ?array
{
    if ($galleryId <= 0) {
        return null;
    }
    $enc   = par_emails_dir() . '/gid-' . $galleryId . '.enc';
    $claim = par_emails_dir() . '/gid-' . $galleryId . '.sending';
    if (!is_file($enc) || !@rename($enc, $claim)) {
        return null;
    }
    $blob = @file_get_contents($claim);
    if ($blob === false) {
        return null;
    }
    $plain = par_email_decrypt($blob);
    if ($plain === null) {
        return null;
    }
    $rec = json_decode($plain, true);
    return is_array($rec) ? $rec : null;
}

function par_mark_gallery_email_sent(int $galleryId): void
{
    @rename(
        par_emails_dir() . '/gid-' . $galleryId . '.sending',
        par_emails_dir() . '/gid-' . $galleryId . '.sent'
    );
}

/** Put a claimed record back so a later "print done" retry can send it. */
function par_unclaim_gallery_email(int $galleryId): void
{
    @rename(
        par_emails_dir() . '/gid-' . $galleryId . '.sending',
        par_emails_dir() . '/gid-' . $galleryId . '.enc'
    );
}

/**
 * Idempotent "print done" hook: if an email is owed for this gallery id, claim
 * it atomically, send the notification, and mark it sent. Safe to call from
 * multiple completion paths (snapshot-request.php + complete.php) and across
 * device retries — the atomic claim guarantees at most one send. No-op if no
 * email is owed. On send failure the record is returned to the queue so a later
 * retry can try again.
 */
function par_notify_gallery_complete(int $galleryId): void
{
    $rec = par_claim_gallery_email($galleryId);
    if ($rec === null) {
        return;
    }
    $email = (string) ($rec['email'] ?? '');
    $name  = (string) ($rec['name'] ?? '');
    if ($email !== '' && par_send_completion_email($email, $name, $galleryId)) {
        par_mark_gallery_email_sent($galleryId);
    } else {
        par_unclaim_gallery_email($galleryId);
    }
}

/**
 * The share blurb, shared by every channel so they all read identically.
 * Both links are inline in the sentence because most share intents give us
 * exactly one free-text field and no second slot for a URL.
 */
function par_share_blurb(string $galleryUrl, string $uploadUrl): string
{
    return 'Look what I made on P.A.R.: ' . $galleryUrl
        . '. P.A.R. is an interactive pixel art robot, and you can submit art'
        . ' for free here: ' . $uploadUrl;
}

/**
 * The social buttons in the completion email, in display order.
 * `file` is a transparent-background PNG under media/social/, attached inline
 * (Content-ID) rather than hotlinked or inlined as a data: URI — Gmail strips
 * data: URIs outright, and remote images are blocked by default in most
 * clients, so CID is the only form that renders without the reader opting in.
 *
 * The blurb already carries the gallery link, so X gets `text` with no `url`
 * (the intent would otherwise append the same link twice). The rest need a
 * `url` param to build a link post at all.
 *
 * Facebook's sharer takes only `u` — it dropped support for prefilled text
 * years ago, so that button shares the gallery link and nothing else. There
 * is no parameter that would fix this.
 */
function par_share_targets(string $galleryUrl, string $uploadUrl, string $imageUrl): array
{
    $g = rawurlencode($galleryUrl);
    $t = rawurlencode(par_share_blurb($galleryUrl, $uploadUrl));

    return [
        ['key' => 'x', 'label' => 'X', 'file' => 'x.png',
         'url' => 'https://twitter.com/intent/tweet?text=' . $t],
        ['key' => 'facebook', 'label' => 'Facebook', 'file' => 'facebook.png',
         'url' => 'https://www.facebook.com/sharer/sharer.php?u=' . $g],
        ['key' => 'pinterest', 'label' => 'Pinterest', 'file' => 'pinterest.png',
         'url' => 'https://pinterest.com/pin/create/button/?url=' . $g
                  . '&media=' . rawurlencode($imageUrl) . '&description=' . $t],
        ['key' => 'linkedin', 'label' => 'LinkedIn', 'file' => 'linkedin.png',
         'url' => 'https://www.linkedin.com/shareArticle?mini=true&url=' . $g . '&title=' . $t],
        ['key' => 'reddit', 'label' => 'Reddit', 'file' => 'reddit.png',
         'url' => 'https://www.reddit.com/submit?url=' . $g . '&title=' . $t],
    ];
}

/**
 * Best public URL for this entry's photo of the matrix, for Pinterest's
 * required `media` parameter. Falls back to the site's own preview image when
 * the print has no photo yet (the snapshot upload is asynchronous, so the
 * completion email can genuinely beat it).
 */
function par_gallery_image_url(string $base, int $galleryId): string
{
    $root = dirname(__DIR__);
    // This entry first; then walk back a short way, because the snapshot upload
    // is asynchronous and can legitimately land after this email goes out. Any
    // recent print is a better Pin than the site icon.
    for ($id = $galleryId; $id > 0 && $id > $galleryId - 20; $id--) {
        foreach (['jpg', 'png'] as $ext) {
            $rel = '/gallery/' . $id . '/image.' . $ext;
            if (is_file($root . $rel)) {
                return rtrim($base, '/') . $rel . '?v=' . (string) @filemtime($root . $rel);
            }
        }
    }
    return rtrim($base, '/') . '/favicon.webp';
}

/**
 * Send the "your piece is done" notification. All header-bearing values are
 * stripped of CR/LF to prevent header injection; the recipient is re-validated
 * here as a last line of defense. Uses PHP mail() (local Exim on Site5); the
 * envelope sender is pinned so SPF/DKIM align to the site domain.
 *
 * Body is multipart/alternative { text/plain, multipart/related { html, PNGs } }
 * so the logos travel with the message as inline attachments.
 */
function par_send_completion_email(string $to, string $pieceName, int $galleryId = 0): bool
{
    $to = trim($to);
    if ($to === '' || preg_match('/[\r\n]/', $to) || !filter_var($to, FILTER_VALIDATE_EMAIL)) {
        return false;
    }

    $base = par_env('SITE_BASE_URL') ?: 'https://par.zimmzimm.com';
    $host = parse_url($base, PHP_URL_HOST) ?: 'par.zimmzimm.com';
    $from = par_env('NOTIFY_FROM') ?: ('no-reply@' . $host);
    $from = preg_replace('/[\r\n]/', '', $from);

    $name = trim(preg_replace('/\s+/', ' ', $pieceName));
    // Deep-link straight to this entry's modal on the gallery page (script.js
    // reads the `#<id>` hash on load and opens it). Falls back to the gallery
    // index if we somehow don't have a valid id.
    $galleryUrl = rtrim($base, '/') . '/gallery';
    if ($galleryId > 0) {
        $galleryUrl .= '#' . $galleryId;
    }
    $uploadUrl = rtrim($base, '/') . '/upload';

    $subjectText = $name !== ''
        ? sprintf('Your P.A.R. piece "%s" is done!', $name)
        : 'Your P.A.R. piece is done!';
    // RFC 2047 encode so non-ASCII names survive and no raw CR/LF can slip in.
    $subject = '=?UTF-8?B?' . base64_encode($subjectText) . '?=';

    $safeName = htmlspecialchars($name, ENT_QUOTES, 'UTF-8');
    $piece = $safeName !== '' ? '&ldquo;' . $safeName . '&rdquo;' : 'your pixel art';
    $safeUrl = htmlspecialchars($galleryUrl, ENT_QUOTES, 'UTF-8');
    $safeUpload = htmlspecialchars($uploadUrl, ENT_QUOTES, 'UTF-8');

    // MUST be an https URL, not a bare `sms:` href. Gmail sanitizes anchors
    // down to http/https/mailto and silently drops everything else, which
    // renders the button as unclickable styled text. share-sms.php is a thin
    // https hop that hands off to the sms: URI from a real page instead.
    $smsUrl = rtrim($base, '/') . '/share-sms.php';
    if ($galleryId > 0) {
        $smsUrl .= '?id=' . $galleryId;
    }
    $safeSms = htmlspecialchars($smsUrl, ENT_QUOTES, 'UTF-8');

    $targets = par_share_targets($galleryUrl, $uploadUrl, par_gallery_image_url($base, $galleryId));

    // Attach each logo, skipping any that is missing on disk so a deleted file
    // degrades to a text link rather than a broken image.
    $socialDir = dirname(__DIR__) . '/media/social/';
    $attachments = [];
    $socialCells = '';
    foreach ($targets as $t) {
        $path = $socialDir . $t['file'];
        $href = htmlspecialchars($t['url'], ENT_QUOTES, 'UTF-8');
        $label = htmlspecialchars($t['label'], ENT_QUOTES, 'UTF-8');
        $data = is_file($path) ? @file_get_contents($path) : false;

        if ($data === false || $data === '') {
            $socialCells .= '<td style="padding:0 8px"><a href="' . $href
                . '" style="color:#02b2d9;font-size:0.9rem;text-decoration:none">'
                . $label . '</a></td>';
            continue;
        }

        $cid = 'par-' . $t['key'] . '@' . $host;
        $attachments[] = ['cid' => $cid, 'name' => $t['file'], 'data' => $data];
        $socialCells .= '<td style="padding:0 8px"><a href="' . $href
            . '" title="' . $label . '"><img src="cid:' . $cid . '" alt="' . $label
            . '" width="36" height="36" style="display:block;width:36px;height:36px;'
            . 'border:0;outline:none;text-decoration:none"></a></td>';
    }

    $btn = 'display:inline-block;background:#02b2d9;color:#ffffff;padding:0.6rem 1.1rem;'
        . 'border-radius:6px;text-decoration:none;font-weight:600';
    $btnGhost = 'display:inline-block;background:#ffffff;color:#02b2d9;padding:0.6rem 1.1rem;'
        . 'border:2px solid #02b2d9;border-radius:6px;text-decoration:none;font-weight:600';

    $html = '<!doctype html><html><body style="font-family:system-ui,-apple-system,sans-serif;'
        . 'max-width:34rem;margin:0 auto;color:#111">'
        . '<h2 style="color:#02b2d9">Your P.A.R. piece is done! &#127881;</h2>'
        . '<p>The display just finished printing ' . $piece . '.</p>'
        . '<p><a href="' . $safeUrl . '" style="' . $btn . '">View it in the gallery</a></p>'
        . '<p style="color:#555;font-size:0.9rem">The recording of your print will appear on that '
        . 'page shortly after it finishes uploading.</p>'
        . '<hr style="border:none;border-top:1px solid #ddd;margin:1.5rem 0">'
        . '<p style="margin:0 0 0.9rem"><a href="' . $safeUpload . '" style="' . $btn . '">'
        . 'Submit more art</a></p>'
        . '<p style="margin:0 0 1.5rem"><a href="' . $safeSms . '" style="' . $btnGhost . '">'
        . 'Text a friend about P.A.R.</a></p>'
        . '<p style="margin:0 0 0.6rem;font-weight:600">Post on your socials:</p>'
        . '<table role="presentation" cellpadding="0" cellspacing="0" border="0" '
        . 'style="border-collapse:collapse"><tr>' . $socialCells . '</tr></table>'
        . '<hr style="border:none;border-top:1px solid #ddd;margin:1.5rem 0">'
        . '<p style="color:#888;font-size:0.8rem">You received this because you asked to be notified '
        . 'when your P.A.R. submission printed. This is a one-time, unmonitored message.</p>'
        . '</body></html>';

    $textLines = [
        $name !== '' ? 'Your P.A.R. piece "' . $name . '" is done!' : 'Your P.A.R. piece is done!',
        '',
        'View it in the gallery: ' . $galleryUrl,
        'The recording of your print will appear there shortly after it uploads.',
        '',
        'Submit more art: ' . $uploadUrl,
        '',
        'Share P.A.R.: ' . par_share_blurb($galleryUrl, $uploadUrl),
    ];
    foreach ($targets as $t) {
        $textLines[] = '  ' . $t['label'] . ': ' . $t['url'];
    }
    $textLines[] = '';
    $textLines[] = 'You received this because you asked to be notified when your '
        . 'P.A.R. submission printed. This is a one-time, unmonitored message.';
    $text = implode("\r\n", $textLines);

    // Boundaries must not appear in any part; random hex can't collide with the
    // base64/quoted-printable payloads below.
    $altBoundary = '=_par_alt_' . bin2hex(random_bytes(12));
    $relBoundary = '=_par_rel_' . bin2hex(random_bytes(12));

    $body = "--$altBoundary\r\n"
        . "Content-Type: text/plain; charset=UTF-8\r\n"
        . "Content-Transfer-Encoding: base64\r\n\r\n"
        . chunk_split(base64_encode($text)) . "\r\n"
        . "--$altBoundary\r\n"
        . "Content-Type: multipart/related; type=\"text/html\"; boundary=\"$relBoundary\"\r\n\r\n"
        . "--$relBoundary\r\n"
        . "Content-Type: text/html; charset=UTF-8\r\n"
        . "Content-Transfer-Encoding: base64\r\n\r\n"
        . chunk_split(base64_encode($html)) . "\r\n";

    foreach ($attachments as $a) {
        $body .= "--$relBoundary\r\n"
            . "Content-Type: image/png; name=\"" . $a['name'] . "\"\r\n"
            . "Content-Transfer-Encoding: base64\r\n"
            . "Content-ID: <" . $a['cid'] . ">\r\n"
            . "Content-Disposition: inline; filename=\"" . $a['name'] . "\"\r\n\r\n"
            . chunk_split(base64_encode($a['data'])) . "\r\n";
    }

    $body .= "--$relBoundary--\r\n"
        . "--$altBoundary--\r\n";

    $headers = implode("\r\n", [
        'MIME-Version: 1.0',
        'Content-Type: multipart/alternative; boundary="' . $altBoundary . '"',
        'From: P.A.R. <' . $from . '>',
        'Reply-To: ' . $from,
        'X-Mailer: PAR-Notifier',
    ]);

    // -f pins the envelope-from for SPF alignment.
    return @mail($to, $subject, $body, $headers, '-f' . $from);
}

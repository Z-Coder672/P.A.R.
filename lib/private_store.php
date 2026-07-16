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
 * Send the "your piece is done" notification. All header-bearing values are
 * stripped of CR/LF to prevent header injection; the recipient is re-validated
 * here as a last line of defense. Uses PHP mail() (local Exim on Site5); the
 * envelope sender is pinned so SPF/DKIM align to the site domain.
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

    $subjectText = $name !== ''
        ? sprintf('Your P.A.R. piece "%s" is done!', $name)
        : 'Your P.A.R. piece is done!';
    // RFC 2047 encode so non-ASCII names survive and no raw CR/LF can slip in.
    $subject = '=?UTF-8?B?' . base64_encode($subjectText) . '?=';

    $safeName = htmlspecialchars($name, ENT_QUOTES, 'UTF-8');
    $piece = $safeName !== '' ? '&ldquo;' . $safeName . '&rdquo;' : 'your pixel art';
    $safeUrl = htmlspecialchars($galleryUrl, ENT_QUOTES, 'UTF-8');

    $html = '<!doctype html><html><body style="font-family:system-ui,-apple-system,sans-serif;'
        . 'max-width:34rem;margin:0 auto;color:#111">'
        . '<h2 style="color:#02b2d9">Your P.A.R. piece is done! 🎉</h2>'
        . '<p>The display just finished printing ' . $piece . '.</p>'
        . '<p><a href="' . $safeUrl . '" style="display:inline-block;background:#02b2d9;color:#fff;'
        . 'padding:0.6rem 1.1rem;border-radius:6px;text-decoration:none">View it in the gallery</a></p>'
        . '<p style="color:#555;font-size:0.9rem">The recording of your print will appear on that '
        . 'page shortly after it finishes uploading.</p>'
        . '<hr style="border:none;border-top:1px solid #ddd;margin:1.5rem 0">'
        . '<p style="color:#888;font-size:0.8rem">You received this because you asked to be notified '
        . 'when your P.A.R. submission printed. This is a one-time, unmonitored message.</p>'
        . '</body></html>';

    $headers = implode("\r\n", [
        'MIME-Version: 1.0',
        'Content-Type: text/html; charset=UTF-8',
        'From: P.A.R. <' . $from . '>',
        'Reply-To: ' . $from,
        'X-Mailer: PAR-Notifier',
    ]);

    // -f pins the envelope-from for SPF alignment.
    return @mail($to, $subject, $html, $headers, '-f' . $from);
}

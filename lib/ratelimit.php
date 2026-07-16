<?php
declare(strict_types=1);

// File-based token-bucket rate limiting. Two buckets per protected endpoint:
//   1. per-IP  — best-effort fairness (keyed on CF-Connecting-IP, since the
//      Site5 origin only ever sees a handful of Cloudflare edge IPs as
//      REMOTE_ADDR). Spoofable by an attacker hitting the origin directly, so
//      it is NOT the load ceiling.
//   2. global  — a single shared bucket that caps TOTAL requests to an endpoint
//      regardless of source IP. This is the real protection against tripping
//      Site5's shared-hosting resource limits, and it holds even if the per-IP
//      key is spoofed/rotated.
// Buckets live under PRIVATE_DIR/ratelimit (off-webroot). Fail-open on any FS
// error so a storage hiccup never takes the site down.
//
// Never requested directly.
if (isset($_SERVER['SCRIPT_FILENAME']) && @realpath($_SERVER['SCRIPT_FILENAME']) === __FILE__) {
    http_response_code(404);
    exit;
}

require_once __DIR__ . '/private_store.php';

function par_client_ip(): string
{
    $cf = $_SERVER['HTTP_CF_CONNECTING_IP'] ?? '';
    if (is_string($cf) && filter_var($cf, FILTER_VALIDATE_IP)) {
        return $cf;
    }
    $ra = $_SERVER['REMOTE_ADDR'] ?? '0.0.0.0';
    return (is_string($ra) && $ra !== '') ? $ra : '0.0.0.0';
}

function par_ratelimit_dir(): string
{
    $d = par_private_dir() . '/ratelimit';
    if (!is_dir($d)) {
        @mkdir($d, 0700, true);
    }
    return $d;
}

/**
 * Consume one token from the bucket stored at $file. Refills at $ratePerSec up
 * to a ceiling of $burst. Returns [allowed(bool), retryAfterSeconds(int)].
 * Fails open (allowed) on any filesystem error.
 */
function par_token_bucket(string $file, float $ratePerSec, float $burst): array
{
    $h = @fopen($file, 'c+');
    if ($h === false) {
        return [true, 0];
    }
    if (!flock($h, LOCK_EX)) {
        fclose($h);
        return [true, 0];
    }

    $raw = stream_get_contents($h);
    $now = microtime(true);
    $tokens = $burst;
    $ts = $now;
    if (is_string($raw) && $raw !== '') {
        $d = json_decode($raw, true);
        if (is_array($d)) {
            $tokens = (float) ($d['t'] ?? $burst);
            $ts = (float) ($d['ts'] ?? $now);
        }
    }

    // Refill based on elapsed time, then try to spend one token.
    $tokens = min($burst, $tokens + max(0.0, $now - $ts) * $ratePerSec);
    $allowed = $tokens >= 1.0;
    if ($allowed) {
        $tokens -= 1.0;
    }

    ftruncate($h, 0);
    rewind($h);
    fwrite($h, (string) json_encode(['t' => $tokens, 'ts' => $now]));
    fflush($h);
    flock($h, LOCK_UN);
    fclose($h);

    $retry = $allowed ? 0 : (int) max(1, ceil((1.0 - $tokens) / max($ratePerSec, 0.0001)));
    return [$allowed, $retry];
}

function par_ratelimit_reject(int $retryAfter): void
{
    http_response_code(429);
    header('Retry-After: ' . max(1, $retryAfter));
    header('Content-Type: application/json; charset=UTF-8');
    header('Cache-Control: no-store');
    echo json_encode(['ok' => false, 'error' => 'rate_limited', 'retry_after' => max(1, $retryAfter)]);
    exit;
}

/**
 * Enforce per-IP then global limits for a named endpoint bucket. On breach,
 * emits a 429 (with Retry-After) and exits. $opts keys:
 *   ip_rate, ip_burst, global_rate, global_burst  (rates are per-second).
 * Per-IP is checked first so a single abuser is turned away without spending
 * global capacity that legitimate other visitors still need.
 */
function par_rate_limit(string $bucket, array $opts): void
{
    $dir = par_ratelimit_dir();

    $iph = substr(hash('sha256', par_client_ip()), 0, 24);
    [$ipOk, $ipRetry] = par_token_bucket(
        $dir . '/' . $bucket . '-ip-' . $iph . '.bucket',
        (float) $opts['ip_rate'],
        (float) $opts['ip_burst']
    );
    if (!$ipOk) {
        par_ratelimit_reject($ipRetry);
    }

    [$gOk, $gRetry] = par_token_bucket(
        $dir . '/' . $bucket . '-global.bucket',
        (float) $opts['global_rate'],
        (float) $opts['global_burst']
    );
    if (!$gOk) {
        par_ratelimit_reject($gRetry);
    }

    // Opportunistic GC of stale per-IP buckets (cheap, ~1/500 requests).
    if (function_exists('random_int') && random_int(1, 500) === 1) {
        par_ratelimit_gc($dir);
    }
}

function par_ratelimit_gc(string $dir): void
{
    $cutoff = time() - 86400;
    foreach (@glob($dir . '/*-ip-*.bucket') ?: [] as $f) {
        if (@filemtime($f) < $cutoff) {
            @unlink($f);
        }
    }
}

<?php
declare(strict_types=1);

// Shared .env reader. Included by other lib/ files and endpoints.
// Never meant to be requested directly — a direct hit 404s so the raw source
// (and any accidental output) can't be fetched over the web. `lib/` is also
// denied in .htaccess; this guard covers the php -S / misconfig case.
if (isset($_SERVER['SCRIPT_FILENAME']) && @realpath($_SERVER['SCRIPT_FILENAME']) === __FILE__) {
    http_response_code(404);
    exit;
}

/**
 * Read a single key from the webroot .env (parsed once per request).
 * Mirrors the existing ` = `-with-spaces format used elsewhere in the repo;
 * values may legitimately contain '=' (e.g. base64 padding), so we split on
 * the first '=' only and never strip trailing '='.
 */
function par_env(string $key): ?string
{
    static $env = null;
    if ($env === null) {
        $env = [];
        $file = dirname(__DIR__) . '/.env';
        foreach (@file($file, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) ?: [] as $line) {
            if ($line === '' || $line[0] === '#' || strpos($line, '=') === false) {
                continue;
            }
            [$k, $v] = explode('=', $line, 2);
            $env[trim($k)] = trim(trim($v), "\"'");
        }
    }
    return $env[$key] ?? null;
}

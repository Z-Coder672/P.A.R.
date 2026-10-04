<?php
declare(strict_types=1);

// Data operations behind the admin panel (admin/api.php): the moderation
// queue, the print queue, and gallery entries. No auth here — callers gate.
//
// Nothing is destroyed outright: removed gallery entries are MOVED to
// PRIVATE_DIR/trash/, and every removed queue line is recorded in full in
// PRIVATE_DIR/admin-audit.log by the caller, so any action can be undone by
// hand.
//
// Lock order is always mod_queue.txt THEN queue.txt (same as mod-action.php),
// so the two can never deadlock against each other.
if (isset($_SERVER['SCRIPT_FILENAME']) && @realpath($_SERVER['SCRIPT_FILENAME']) === __FILE__) {
    http_response_code(404);
    exit;
}

require_once __DIR__ . '/private_store.php';

const ADMIN_MAX_MAIN_QUEUE = 100; // mirrors MAX_MAIN_QUEUE_LENGTH in mod-action.php
const ADMIN_BITMAP_BYTES   = 84;
const ADMIN_MOD_STATUSES   = ['pending', 'processing', 'email_sent'];

final class AdminError extends RuntimeException
{
    public int $status;

    public function __construct(string $code, int $status = 400)
    {
        parent::__construct($code);
        $this->status = $status;
    }
}

function admin_root(): string
{
    return dirname(__DIR__);
}

function admin_list_file(string $list): string
{
    if ($list === 'mod') {
        return admin_root() . '/mod_queue.txt';
    }
    if ($list === 'queue') {
        return admin_root() . '/queue.txt';
    }
    throw new AdminError('bad_list');
}

/**
 * Queue lines have no stable id (queue.txt entries carry none at all), so the
 * panel addresses a line by a hash of its exact text. A line that changed or
 * was popped since the panel last loaded simply stops matching -> not_found,
 * instead of the action landing on whatever now sits at that index.
 */
function admin_line_key(string $line): string
{
    return substr(sha1($line), 0, 16);
}

/** @return array{0: resource, 1: string[]} locked handle + non-empty lines */
function admin_lines_open(string $file): array
{
    if (!file_exists($file)) {
        file_put_contents($file, '');
    }
    $h = fopen($file, 'c+');
    if ($h === false || !flock($h, LOCK_EX)) {
        if ($h !== false) {
            fclose($h);
        }
        throw new AdminError('lock_failed', 500);
    }
    $lines = preg_split('/\R/', stream_get_contents($h) ?: '') ?: [];
    return [$h, array_values(array_filter($lines, static fn (string $l): bool => trim($l) !== ''))];
}

/** @param resource $h */
function admin_lines_save($h, array $lines): void
{
    rewind($h);
    ftruncate($h, 0);
    if ($lines !== [] && fwrite($h, implode(PHP_EOL, $lines) . PHP_EOL) === false) {
        throw new AdminError('write_failed', 500);
    }
    fflush($h);
}

/** @param resource $h */
function admin_lines_close($h): void
{
    flock($h, LOCK_UN);
    fclose($h);
}

function admin_find(array $lines, string $key): int
{
    foreach ($lines as $i => $line) {
        if (admin_line_key($line) === $key) {
            return $i;
        }
    }
    throw new AdminError('not_found', 404);
}

/** A queue line as an array; legacy lines are a bare base64 bitmap. */
function admin_decode_line(string $line): array
{
    $e = json_decode($line, true);
    return is_array($e) ? $e : ['item' => $line];
}

function admin_encode(array $entry): string
{
    return (string) json_encode($entry, JSON_UNESCAPED_UNICODE);
}

function admin_clean_text($v): string
{
    return is_string($v) ? mb_substr(trim($v), 0, 100) : '';
}

function admin_has_sub_email(string $subId): bool
{
    return $subId !== '' && par_valid_subid($subId) && is_file(par_emails_dir() . '/sub-' . $subId . '.enc');
}

// ---------------------------------------------------------------- state ---

function admin_state(): array
{
    $mod = [];
    foreach (@file(admin_list_file('mod'), FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) ?: [] as $line) {
        $e = admin_decode_line($line);
        $mod[] = [
            'key'       => admin_line_key($line),
            'id'        => (string) ($e['id'] ?? ''),
            'bitmap'    => (string) ($e['item'] ?? ''),
            'name'      => (string) ($e['name'] ?? ''),
            'artist'    => (string) ($e['artist'] ?? ''),
            'ts'        => (int) ($e['ts'] ?? 0),
            'status'    => (string) ($e['status'] ?? 'pending'),
            'status_ts' => (int) ($e['status_ts'] ?? 0),
            'notify'    => admin_has_sub_email((string) ($e['id'] ?? '')),
        ];
    }

    $queue = [];
    foreach (@file(admin_list_file('queue'), FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) ?: [] as $line) {
        $e = admin_decode_line($line);
        $queue[] = [
            'key'    => admin_line_key($line),
            'bitmap' => (string) ($e['item'] ?? ''),
            'name'   => (string) ($e['name'] ?? ''),
            'artist' => (string) ($e['artist'] ?? ''),
            'notify' => admin_has_sub_email((string) ($e['sub_id'] ?? '')),
        ];
    }

    $gallery = [];
    $emails = par_emails_dir();
    foreach (glob(admin_root() . '/gallery/*', GLOB_ONLYDIR) ?: [] as $dir) {
        $id = basename($dir);
        if (!ctype_digit($id)) {
            continue;
        }
        $file = is_file("$dir/info.json") ? 'info' : (is_file("$dir/pending.json") ? 'pending' : null);
        $info = $file !== null ? json_decode((string) @file_get_contents("$dir/$file.json"), true) : null;
        if (!is_array($info)) {
            $info = [];
        }
        $image = null;
        foreach (['image.jpg', 'image.png'] as $img) {
            if (is_file("$dir/$img")) {
                $image = "gallery/$id/$img?v=" . filemtime("$dir/$img");
                break;
            }
        }
        $notify = null;
        foreach (['enc' => 'owed', 'sending' => 'sending', 'sent' => 'sent'] as $ext => $label) {
            if (is_file("$emails/gid-$id.$ext")) {
                $notify = $label;
                break;
            }
        }
        $gallery[] = [
            'id'       => (int) $id,
            // `file` is what is on disk; `pending` is what the public site shows
            // (gallery.php treats an entry with a photo as complete regardless).
            // null `file` = a directory with no json, invisible on the site.
            'file'     => $file,
            'pending'  => $file === 'pending' && $image === null,
            'bitmap'   => (string) ($info['bitmap'] ?? ''),
            'name'     => (string) ($info['name'] ?? ''),
            'artist'   => (string) ($info['artist'] ?? ''),
            'video_id' => is_string($info['video_id'] ?? null) ? $info['video_id'] : null,
            'image'    => $image,
            'notify'   => $notify,
        ];
    }
    usort($gallery, static fn (array $a, array $b): int => $b['id'] - $a['id']);

    return ['mod' => $mod, 'queue' => $queue, 'gallery' => $gallery, 'max_queue' => ADMIN_MAX_MAIN_QUEUE];
}

// ------------------------------------------------- mod queue / print queue ---

/**
 * Reorder a list to match $keys. Lines the panel didn't know about (submitted
 * since it loaded) keep their relative order at the end; keys that no longer
 * exist (popped by the device meanwhile) are ignored. Never adds or drops a
 * line.
 */
function admin_reorder(string $list, array $keys): void
{
    [$h, $lines] = admin_lines_open(admin_list_file($list));
    try {
        $used = array_fill(0, count($lines), false);
        $out = [];
        foreach ($keys as $key) {
            foreach ($lines as $i => $line) {
                if (!$used[$i] && admin_line_key($line) === $key) {
                    $used[$i] = true;
                    $out[] = $line;
                    break;
                }
            }
        }
        foreach ($lines as $i => $line) {
            if (!$used[$i]) {
                $out[] = $line;
            }
        }
        admin_lines_save($h, $out);
    } finally {
        admin_lines_close($h);
    }
}

/** Remove a line. Returns the removed entry (for the audit log). */
function admin_delete(string $list, string $key): array
{
    [$h, $lines] = admin_lines_open(admin_list_file($list));
    try {
        $i = admin_find($lines, $key);
        $entry = admin_decode_line($lines[$i]);
        array_splice($lines, $i, 1);
        admin_lines_save($h, $lines);
    } finally {
        admin_lines_close($h);
    }
    // It will never print now, so drop the stored notification address — same
    // as a moderation reject in mod-action.php.
    par_delete_submission_email((string) ($entry[$list === 'mod' ? 'id' : 'sub_id'] ?? ''));
    return $entry;
}

/**
 * Insert a copy of a line directly after it. The copy is a separate piece: a
 * mod copy gets its own id, and neither kind inherits the notification email
 * (the submitter is told once, about the original). Queue copies carry a
 * unique `copy` field purely so the line text — and therefore its key —
 * differs from the original's.
 */
function admin_duplicate(string $list, string $key): array
{
    [$h, $lines] = admin_lines_open(admin_list_file($list));
    try {
        $i = admin_find($lines, $key);
        $entry = admin_decode_line($lines[$i]);
        if ($list === 'queue') {
            if (count($lines) >= ADMIN_MAX_MAIN_QUEUE) {
                throw new AdminError('main_queue_full', 409);
            }
            $copy = [
                'item'   => (string) ($entry['item'] ?? ''),
                'name'   => (string) ($entry['name'] ?? ''),
                'artist' => (string) ($entry['artist'] ?? ''),
                'sub_id' => '',
                'copy'   => uniqid('', true),
            ];
        } else {
            $now = time();
            $status = (string) ($entry['status'] ?? 'pending');
            $copy = [
                'id'        => uniqid('', true),
                'item'      => (string) ($entry['item'] ?? ''),
                'name'      => (string) ($entry['name'] ?? ''),
                'artist'    => (string) ($entry['artist'] ?? ''),
                'ts'        => $now,
                // `processing` is the daemon's claim on the original, not the copy.
                'status'    => $status === 'processing' ? 'pending' : $status,
                'status_ts' => $now,
                'notify'    => false,
            ];
        }
        array_splice($lines, $i + 1, 0, [admin_encode($copy)]);
        admin_lines_save($h, $lines);
    } finally {
        admin_lines_close($h);
    }
    return $entry;
}

function admin_edit(string $list, string $key, $name, $artist): array
{
    [$h, $lines] = admin_lines_open(admin_list_file($list));
    try {
        $i = admin_find($lines, $key);
        $entry = admin_decode_line($lines[$i]);
        $before = ['name' => $entry['name'] ?? '', 'artist' => $entry['artist'] ?? ''];
        $entry['name']   = admin_clean_text($name);
        $entry['artist'] = admin_clean_text($artist);
        $lines[$i] = admin_encode($entry);
        admin_lines_save($h, $lines);
    } finally {
        admin_lines_close($h);
    }
    return $before;
}

/**
 * Set a moderation entry's status:
 *   pending     — the daemon's next poll re-runs the automatic checks
 *   processing  — claimed by the daemon (auto-reverts to pending after 10 min)
 *   email_sent  — parked for a human; the daemon leaves it alone
 */
function admin_mod_status(string $key, string $status): void
{
    if (!in_array($status, ADMIN_MOD_STATUSES, true)) {
        throw new AdminError('bad_status');
    }
    [$h, $lines] = admin_lines_open(admin_list_file('mod'));
    try {
        $i = admin_find($lines, $key);
        $entry = admin_decode_line($lines[$i]);
        if (!isset($entry['id'])) {
            $entry['id'] = uniqid('', true); // legacy line; mod-next.php backfills the same way
        }
        $entry['status']    = $status;
        $entry['status_ts'] = time();
        $lines[$i] = admin_encode($entry);
        admin_lines_save($h, $lines);
    } finally {
        admin_lines_close($h);
    }
}

function admin_insert(array $lines, string $line, string $position): array
{
    if ($position === 'front') {
        array_unshift($lines, $line);
    } else {
        $lines[] = $line;
    }
    return $lines;
}

/**
 * Approve: promote a moderation entry into the print queue. All-or-nothing in
 * the same sense as mod-action.php — the entry leaves mod_queue.txt only after
 * queue.txt has been written.
 */
function admin_approve(string $key, string $position): array
{
    [$mh, $mod] = admin_lines_open(admin_list_file('mod'));
    try {
        $i = admin_find($mod, $key);
        $entry = admin_decode_line($mod[$i]);
        [$qh, $queue] = admin_lines_open(admin_list_file('queue'));
        try {
            if (count($queue) >= ADMIN_MAX_MAIN_QUEUE) {
                throw new AdminError('main_queue_full', 409);
            }
            admin_lines_save($qh, admin_insert($queue, admin_encode([
                'item'   => (string) ($entry['item'] ?? ''),
                'name'   => (string) ($entry['name'] ?? ''),
                'artist' => (string) ($entry['artist'] ?? ''),
                'sub_id' => (string) ($entry['id'] ?? ''),
            ]), $position));
        } finally {
            admin_lines_close($qh);
        }
        array_splice($mod, $i, 1);
        admin_lines_save($mh, $mod);
    } finally {
        admin_lines_close($mh);
    }
    return $entry;
}

/**
 * Send an approved print back to moderation. It lands as `email_sent` (held
 * for a human) — as `pending` the daemon would just auto-approve it straight
 * back into the print queue.
 */
function admin_demote(string $key): array
{
    [$mh, $mod] = admin_lines_open(admin_list_file('mod'));
    try {
        [$qh, $queue] = admin_lines_open(admin_list_file('queue'));
        try {
            $i = admin_find($queue, $key);
            $entry = admin_decode_line($queue[$i]);
            $subId = (string) ($entry['sub_id'] ?? '');
            $id = par_valid_subid($subId) ? $subId : uniqid('', true);
            $now = time();
            $mod[] = admin_encode([
                'id'        => $id,
                'item'      => (string) ($entry['item'] ?? ''),
                'name'      => (string) ($entry['name'] ?? ''),
                'artist'    => (string) ($entry['artist'] ?? ''),
                'ts'        => $now,
                'status'    => 'email_sent',
                'status_ts' => $now,
                'notify'    => admin_has_sub_email($id),
            ]);
            admin_lines_save($mh, $mod);
            array_splice($queue, $i, 1);
            admin_lines_save($qh, $queue);
        } finally {
            admin_lines_close($qh);
        }
    } finally {
        admin_lines_close($mh);
    }
    return $entry;
}

// -------------------------------------------------------------- gallery ---

/** @return array{dir:string, file:?string, path:?string, info:array} */
function admin_gallery_entry(int $id): array
{
    $dir = admin_root() . '/gallery/' . $id;
    if ($id <= 0 || !is_dir($dir)) {
        throw new AdminError('not_found', 404);
    }
    $file = is_file("$dir/info.json") ? 'info' : (is_file("$dir/pending.json") ? 'pending' : null);
    $info = $file !== null ? json_decode((string) @file_get_contents("$dir/$file.json"), true) : null;
    return [
        'dir'  => $dir,
        'file' => $file,
        'path' => $file !== null ? "$dir/$file.json" : null,
        'info' => is_array($info) ? $info : [],
    ];
}

function admin_write_json(string $path, array $data): void
{
    $tmp = $path . '.tmp';
    if (file_put_contents($tmp, admin_encode($data)) === false || !rename($tmp, $path)) {
        throw new AdminError('write_failed', 500);
    }
}

/**
 * Flip an entry between pending.json and info.json. Marking complete does NOT
 * arm the snapshot flag or stop a recording — it only changes what the gallery
 * says. $notify additionally sends the submitter's "your piece is done" email
 * if one is still owed (idempotent, same hook the device path uses).
 */
function admin_gallery_status(int $id, string $status, bool $notify): void
{
    $e = admin_gallery_entry($id);
    if ($e['file'] === null) {
        throw new AdminError('no_entry_json', 409);
    }
    $want = $status === 'complete' ? 'info' : ($status === 'pending' ? 'pending' : null);
    if ($want === null) {
        throw new AdminError('bad_status');
    }
    if ($e['file'] !== $want && !rename($e['path'], $e['dir'] . "/$want.json")) {
        throw new AdminError('rename_failed', 500);
    }
    if ($want === 'info' && $notify) {
        par_notify_gallery_complete($id);
    }
}

function admin_gallery_edit(int $id, $name, $artist, $videoId): array
{
    $e = admin_gallery_entry($id);
    if ($e['file'] === null) {
        throw new AdminError('no_entry_json', 409);
    }
    $videoId = is_string($videoId) ? trim($videoId) : '';
    if ($videoId !== '' && !preg_match('/^[A-Za-z0-9_-]{11}$/', $videoId)) {
        throw new AdminError('bad_video_id');
    }
    $info = $e['info'];
    $before = ['name' => $info['name'] ?? '', 'artist' => $info['artist'] ?? '', 'video_id' => $info['video_id'] ?? null];
    $info['name']   = admin_clean_text($name);
    $info['artist'] = admin_clean_text($artist);
    if ($videoId === '') {
        unset($info['video_id']);
    } else {
        $info['video_id'] = $videoId;
    }
    admin_write_json($e['path'], $info);
    admin_sync_latest_video($id);
    return $before;
}

function admin_trash_dir(): string
{
    $d = par_private_dir() . '/trash';
    if (!is_dir($d)) {
        @mkdir($d, 0700, true);
    }
    return $d;
}

/**
 * Remove a gallery entry by MOVING gallery/<id>/ (json, photo) and its email
 * markers into PRIVATE_DIR/trash/gallery-<id>-<stamp>/. Returns that path.
 * The email markers must go too: next.php hands out max(id)+1, so removing the
 * newest entry frees its id for the next print, and a stale gid-<id>.* marker
 * would then be attributed to the wrong piece.
 */
function admin_gallery_trash(int $id): string
{
    $e = admin_gallery_entry($id);
    $dest = admin_trash_dir() . '/gallery-' . $id . '-' . date('Ymd-His') . '-' . bin2hex(random_bytes(3));
    if (!rename($e['dir'], $dest)) {
        throw new AdminError('trash_failed', 500);
    }
    foreach (['enc', 'sending', 'sent'] as $ext) {
        $marker = par_emails_dir() . "/gid-$id.$ext";
        if (is_file($marker)) {
            @rename($marker, "$dest/email-gid-$id.$ext");
        }
    }
    admin_sync_latest_video($id);
    return $dest;
}

/**
 * Keep latest-video.json (the Latest tab) honest after entry $id changed or
 * went away: if it points at $id, repoint it at that entry's current video, or
 * failing that at the newest remaining entry that has one. With none left the
 * file is moved to the trash so the Latest tab shows its empty state.
 */
function admin_sync_latest_video(int $id): void
{
    $path = admin_root() . '/latest-video.json';
    $latest = json_decode((string) @file_get_contents($path), true);
    if (!is_array($latest) || (int) ($latest['id'] ?? 0) !== $id) {
        return;
    }
    $ids = [];
    foreach (glob(admin_root() . '/gallery/*', GLOB_ONLYDIR) ?: [] as $dir) {
        if (ctype_digit(basename($dir))) {
            $ids[] = (int) basename($dir);
        }
    }
    rsort($ids);
    if (in_array($id, $ids, true)) {
        array_unshift($ids, $id); // prefer the entry itself if it still has a video
    }
    foreach ($ids as $candidate) {
        $info = admin_gallery_entry($candidate)['info'];
        if (is_string($info['video_id'] ?? null) && preg_match('/^[A-Za-z0-9_-]{11}$/', $info['video_id'])) {
            admin_write_json($path, [
                'id'       => $candidate,
                'video_id' => $info['video_id'],
                'name'     => (string) ($info['name'] ?? ''),
                'ts'       => time(),
            ]);
            return;
        }
    }
    @rename($path, admin_trash_dir() . '/latest-video-' . date('Ymd-His') . '.json');
}

/**
 * Put a gallery entry's bitmap back in the print queue. With $remove the
 * gallery entry is trashed (a "move": the usual fix for a print that failed or
 * is stuck pending); without it the entry stays and this is a reprint. Either
 * way the submitter's notification email goes with it, so they are told when
 * the reprint finishes.
 *
 * @return array{entry: array, trashed: ?string}
 */
function admin_gallery_requeue(int $id, string $position, bool $remove): array
{
    $e = admin_gallery_entry($id);
    $bitmap = (string) ($e['info']['bitmap'] ?? '');
    $raw = base64_decode($bitmap, true);
    if ($raw === false || strlen($raw) !== ADMIN_BITMAP_BYTES) {
        throw new AdminError('bad_bitmap', 409);
    }
    $entry = [
        'item'   => $bitmap,
        'name'   => (string) ($e['info']['name'] ?? ''),
        'artist' => (string) ($e['info']['artist'] ?? ''),
        'sub_id' => '',
    ];

    [$qh, $queue] = admin_lines_open(admin_list_file('queue'));
    try {
        if (count($queue) >= ADMIN_MAX_MAIN_QUEUE) {
            throw new AdminError('main_queue_full', 409);
        }
        // The submitter's address follows the piece back into the queue, whatever
        // state it was in: still owed (.enc), mid-send (.sending) or already
        // sent (.sent — the server thought the print finished, but it is being
        // redone, so they are told again when it really is). All three are the
        // same encrypted blob. Only when the entry is kept AND was already sent
        // is the blob copied rather than moved, so the gallery entry keeps its
        // "sent" record.
        $subId = uniqid('', true);
        foreach (['enc', 'sending', 'sent'] as $ext) {
            $marker = par_emails_dir() . "/gid-$id.$ext";
            if (!is_file($marker)) {
                continue;
            }
            $dest = par_emails_dir() . "/sub-$subId.enc";
            $carried = (!$remove && $ext === 'sent') ? @copy($marker, $dest) : @rename($marker, $dest);
            if ($carried) {
                $entry['sub_id'] = $subId;
            }
            break;
        }
        admin_lines_save($qh, admin_insert($queue, admin_encode($entry), $position));
    } finally {
        admin_lines_close($qh);
    }

    return ['entry' => $entry, 'trashed' => $remove ? admin_gallery_trash($id) : null];
}

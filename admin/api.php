<?php
declare(strict_types=1);

// Admin panel API. POST application/json {action, ...}; requires the admin
// session cookie plus the X-CSRF-Token header. Every successful call answers
// {ok:true, state:{mod, queue, gallery}} so the panel re-renders from truth.

require_once __DIR__ . '/../lib/admin_auth.php';
require_once __DIR__ . '/../lib/admin_store.php';

admin_security_headers();

$email = admin_current_email();
if ($email === null) {
    admin_json(401, ['ok' => false, 'error' => 'unauthorized']);
}
$data = admin_json_body();
if (!admin_csrf_ok()) {
    admin_json(403, ['ok' => false, 'error' => 'bad_csrf']);
}
// Release the session lock: nothing below writes to the session, and holding
// it would serialize the panel's background refresh behind a slow action.
session_write_close();

$str = static function (string $k) use ($data): string {
    $v = $data[$k] ?? '';
    if (!is_string($v)) {
        throw new AdminError('bad_' . $k);
    }
    return $v;
};
$gid = static function () use ($data): int {
    $v = $data['id'] ?? null;
    if (!is_int($v) || $v <= 0) {
        throw new AdminError('bad_id');
    }
    return $v;
};
$position = static fn (): string => ($data['position'] ?? 'end') === 'front' ? 'front' : 'end';

$action = $data['action'] ?? '';
try {
    switch ($action) {
        case 'state':
            break;

        case 'reorder':
            $keys = $data['keys'] ?? null;
            if (!is_array($keys) || array_filter($keys, static fn ($k): bool => !is_string($k)) !== []) {
                throw new AdminError('bad_keys');
            }
            admin_reorder($str('list'), $keys);
            admin_audit($email, 'reorder', ['list' => $str('list'), 'count' => count($keys)]);
            break;

        case 'delete':
            $removed = admin_delete($str('list'), $str('key'));
            admin_audit($email, 'delete', ['list' => $str('list'), 'entry' => $removed]);
            break;

        case 'duplicate':
            $entry = admin_duplicate($str('list'), $str('key'));
            admin_audit($email, 'duplicate', ['list' => $str('list'), 'name' => $entry['name'] ?? '']);
            break;

        case 'edit':
            $before = admin_edit($str('list'), $str('key'), $data['name'] ?? '', $data['artist'] ?? '');
            admin_audit($email, 'edit', ['list' => $str('list'), 'before' => $before,
                'name' => $data['name'] ?? '', 'artist' => $data['artist'] ?? '']);
            break;

        case 'mod_status':
            admin_mod_status($str('key'), $str('status'));
            admin_audit($email, 'mod_status', ['key' => $str('key'), 'status' => $str('status')]);
            break;

        case 'approve':
            $entry = admin_approve($str('key'), $position());
            admin_audit($email, 'approve', ['id' => $entry['id'] ?? '', 'name' => $entry['name'] ?? '',
                'position' => $position()]);
            break;

        case 'demote':
            $entry = admin_demote($str('key'));
            admin_audit($email, 'demote', ['name' => $entry['name'] ?? '']);
            break;

        case 'gallery_status':
            admin_gallery_status($gid(), $str('status'), ($data['notify'] ?? false) === true);
            admin_audit($email, 'gallery_status', ['id' => $gid(), 'status' => $str('status'),
                'notify' => ($data['notify'] ?? false) === true]);
            break;

        case 'gallery_edit':
            $before = admin_gallery_edit($gid(), $data['name'] ?? '', $data['artist'] ?? '', $data['video_id'] ?? '');
            admin_audit($email, 'gallery_edit', ['id' => $gid(), 'before' => $before]);
            break;

        case 'gallery_requeue':
            $res = admin_gallery_requeue($gid(), $position(), ($data['remove'] ?? false) === true);
            admin_audit($email, 'gallery_requeue', ['id' => $gid(), 'position' => $position(),
                'trashed' => $res['trashed']]);
            break;

        case 'gallery_delete':
            $dest = admin_gallery_trash($gid());
            admin_audit($email, 'gallery_delete', ['id' => $gid(), 'trashed' => $dest]);
            break;

        default:
            throw new AdminError('unknown_action');
    }
} catch (AdminError $e) {
    admin_json($e->status, ['ok' => false, 'error' => $e->getMessage(), 'state' => admin_state()]);
}

admin_json(200, ['ok' => true, 'state' => admin_state()]);

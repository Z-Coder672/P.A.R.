<?php
declare(strict_types=1);

// Admin panel entry point: the sign-in page when there is no valid session,
// the panel shell when there is. All data comes from api.php.

require_once __DIR__ . '/../lib/admin_auth.php';

admin_security_headers();
header('Content-Type: text/html; charset=UTF-8');

$email = admin_current_email();
$h = static fn (string $s): string => htmlspecialchars($s, ENT_QUOTES, 'UTF-8');
$v = static fn (string $f): string => $f . '?v=' . (string) @filemtime(__DIR__ . '/' . $f);
?>
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <meta name="robots" content="noindex, nofollow">
    <title>P.A.R. Admin</title>
    <link rel="icon" href="../favicon.webp">
    <link rel="stylesheet" href="<?= $h($v('admin.css')) ?>">
</head>
<?php if ($email === null): $cfg = admin_firebase_config(); ?>
<body class="login-body">
    <main class="login-card">
        <h1>P.A.R. Admin</h1>
        <p class="muted">Sign in with an authorized Google account.</p>
<?php if ($cfg === null): ?>
        <p class="status error">Firebase is not configured. Add <code>FIREBASE_API_KEY</code>,
            <code>FIREBASE_PROJECT_ID</code> and <code>ADMIN_EMAILS</code> to <code>.env</code>.</p>
<?php else: ?>
        <button id="googleSignIn" class="btn primary wide" type="button">Sign in with Google</button>
        <p id="loginStatus" class="status" hidden></p>
        <script id="firebaseConfig" type="application/json"><?= json_encode($cfg, JSON_HEX_TAG | JSON_HEX_AMP) ?></script>
        <script type="module" src="<?= $h($v('login.js')) ?>"></script>
<?php endif; ?>
    </main>
</body>
<?php else: admin_csrf_token(); ?>
<body data-csrf="<?= $h($_SESSION['csrf_token']) ?>">
    <header class="topbar">
        <a class="brand" href="../">P.A.R. <span>Admin</span></a>
        <nav class="tabs" role="tablist">
            <button class="tab" data-tab="mod" type="button">Moderation <span class="count" id="count-mod">0</span></button>
            <button class="tab" data-tab="queue" type="button">Print queue <span class="count" id="count-queue">0</span></button>
            <button class="tab" data-tab="gallery" type="button">Gallery <span class="count" id="count-gallery">0</span></button>
        </nav>
        <div class="who">
            <span class="muted"><?= $h($email) ?></span>
            <button id="refresh" class="btn" type="button">Refresh</button>
            <button id="logout" class="btn" type="button">Sign out</button>
        </div>
    </header>
    <main class="panel">
        <p id="hint" class="muted hint"></p>
        <div id="list" class="list"></div>
    </main>
    <div id="toast" class="toast" hidden></div>
    <dialog id="dialog" class="dialog"></dialog>
    <script src="<?= $h($v('admin.js')) ?>"></script>
</body>
<?php endif; ?>
</html>

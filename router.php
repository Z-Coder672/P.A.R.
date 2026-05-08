<?php
if (php_sapi_name() === 'cli-server') {
    $requested = parse_url($_SERVER["REQUEST_URI"], PHP_URL_PATH);
    
    // Serve PHP files directly
    if (file_exists(__DIR__ . $requested) && pathinfo($requested, PATHINFO_EXTENSION) === 'php') {
        require __DIR__ . $requested;
        exit;
    }
    
    // Serve actual static files
    if (file_exists(__DIR__ . $requested) && is_file(__DIR__ . $requested)) {
        return false;
    }
    
    // Everything else → index.html for client-side routing
    require __DIR__ . '/index.html';
}
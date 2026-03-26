<?php
declare(strict_types=1);

/**
 * Example: calling Pando from PHP via flexicorp_pando FFI.
 *
 * Prerequisites:
 *   - PHP 7.4+ with ffi.enable=true (or "preload" in production)
 *   - libflexicorp_pando.so built and accessible
 *
 * Usage:
 *   php example_ffi.php /path/to/project_root '[lemma="book"]'
 */

// ── Configuration ────────────────────────────────────────────────────────

$header  = __DIR__ . '/flexicorp_pando_ffi.h';           // preprocessor-free header for FFI
$lib_so  = __DIR__ . '/build/libflexicorp_pando.so';    // Linux
$lib_dy  = __DIR__ . '/build/libflexicorp_pando.dylib'; // macOS
$lib     = is_file($lib_dy) ? $lib_dy : $lib_so;

$projectRoot = $argv[1] ?? '';
$query       = $argv[2] ?? '[form="de"]';
$offset      = (int)($argv[3] ?? 0);
$limit       = (int)($argv[4] ?? 20);

if ($projectRoot === '') {
    fwrite(STDERR, "Usage: php example_ffi.php <project_root> [query] [offset] [limit]\n");
    exit(2);
}

// ── Load library ─────────────────────────────────────────────────────────

if (!file_exists($header)) {
    fwrite(STDERR, "Header not found: $header\n");
    exit(1);
}
if (!file_exists($lib)) {
    fwrite(STDERR, "Library not found: $lib\n");
    exit(1);
}

$ffi = FFI::cdef(file_get_contents($header), $lib);

// ── Check API version ────────────────────────────────────────────────────

$version = $ffi->flexicorp_pando_api_version();
if ($version !== 1) {
    fwrite(STDERR, "Unexpected API version: $version (expected 1)\n");
    exit(1);
}

// ── Open corpus ──────────────────────────────────────────────────────────

$ctx = $ffi->flexicorp_pando_open($projectRoot, null, 0);
if (FFI::isNull($ctx)) {
    $err = FFI::string($ffi->flexicorp_pando_last_error(null));
    fwrite(STDERR, "Open failed: $err\n");
    exit(1);
}

// ── Run query ────────────────────────────────────────────────────────────

$start = microtime(true);

$result = $ffi->flexicorp_pando_query(
    $ctx,
    $query,
    $offset,
    $limit,
    10000,  // max_total
    5,      // context
    null    // attrs: all
);

$elapsed = (microtime(true) - $start) * 1000;

$json = FFI::string($result);
$ffi->flexicorp_pando_free($result);

// ── Output ───────────────────────────────────────────────────────────────

$decoded = json_decode($json, true);
if (is_array($decoded)) {
    echo json_encode($decoded, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE);
} else {
    echo $json;
}
echo "\n";

fwrite(STDERR, sprintf("php_total_ms: %.1f\n", $elapsed));

// ── Cleanup ──────────────────────────────────────────────────────────────
// In a real FPM worker, you'd keep $ctx open as a static and skip this.

$ffi->flexicorp_pando_close($ctx);

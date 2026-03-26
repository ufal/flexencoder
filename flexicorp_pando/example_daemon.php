<?php
declare(strict_types=1);

/**
 * Example: calling Pando via flexicorp-pando-server over Unix socket.
 *
 * Prerequisites:
 *   - flexicorp-pando-server running:
 *     ./flexicorp-pando-server --socket /tmp/flexicorp-pando.sock --corpus-root /path/to/corpora
 *
 * Usage:
 *   php example_daemon.php /path/to/corpus/index '[lemma="book"]'
 */

// ── Configuration ────────────────────────────────────────────────────────

$socketPath = getenv('FLEXICORP_PANDO_SOCKET') ?: '/tmp/flexicorp-pando.sock';
$corpusPath = $argv[1] ?? '';
$query      = $argv[2] ?? '[form="de"]';
$offset     = (int)($argv[3] ?? 0);
$limit      = (int)($argv[4] ?? 20);

if ($corpusPath === '') {
    fwrite(STDERR, "Usage: php example_daemon.php <corpus_path> [query] [offset] [limit]\n");
    exit(2);
}

// ── Connect to daemon ────────────────────────────────────────────────────

/**
 * Send a request to the flexicorp-pando-server and return the decoded response.
 *
 * @param string $socketPath  Path to the Unix domain socket
 * @param array  $request     Request payload (will be JSON-encoded)
 * @return array              Decoded JSON response
 */
function pando_daemon_request(string $socketPath, array $request): array {
    $sock = @stream_socket_client("unix://$socketPath", $errno, $errstr, 2.0);
    if (!$sock) {
        return ['ok' => false, 'error' => "Cannot connect to daemon: $errstr ($errno)"];
    }

    // Send request as a single JSON line.
    $line = json_encode($request, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE) . "\n";
    fwrite($sock, $line);

    // Read response (one JSON line).
    $response = '';
    while (($chunk = fgets($sock, 65536)) !== false) {
        $response .= $chunk;
        if (str_ends_with(rtrim($chunk), '}')) break;  // end of JSON object
    }
    fclose($sock);

    $decoded = json_decode(trim($response), true);
    if (!is_array($decoded)) {
        return ['ok' => false, 'error' => 'Daemon returned invalid JSON', 'raw' => $response];
    }
    return $decoded;
}

// ── Run query ────────────────────────────────────────────────────────────

$start = microtime(true);

$result = pando_daemon_request($socketPath, [
    'action'    => 'query',
    'corpus'    => $corpusPath,
    'query'     => $query,
    'offset'    => max(0, $offset),
    'limit'     => max(1, $limit),
    'max_total' => 10000,
    'context'   => 5,
    'attrs'     => '',
]);

$elapsed = (microtime(true) - $start) * 1000;

// ── Output ───────────────────────────────────────────────────────────────

echo json_encode($result, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE);
echo "\n";

fwrite(STDERR, sprintf("php_total_ms: %.1f (includes socket connect + query)\n", $elapsed));

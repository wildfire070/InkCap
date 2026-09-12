#pragma once
#include <string>

namespace UrlUtils {

/**
 * Check if URL uses HTTPS protocol
 */
bool isHttpsUrl(const std::string& url);

/**
 * Prepend http:// if no protocol specified (server will redirect to https if needed)
 */
std::string ensureProtocol(const std::string& url);

/**
 * Extract host with protocol from URL (e.g., "http://example.com" from "http://example.com/path")
 */
std::string extractHost(const std::string& url);

/**
 * Percent-encode raw characters that esp_http_client rejects in a URL.
 */
std::string encodeUnsafeUrlChars(const std::string& url);

/**
 * Build full URL from server URL and path.
 * If path starts with /, it's an absolute path from the host root.
 * Otherwise, it's relative to the server URL.
 */
std::string buildUrl(const std::string& serverUrl, const std::string& path);

/**
 * True if `url` and `serverUrl` share the same scheme+host+port (both passed
 * through ensureProtocol() first). buildUrl() returns a feed-supplied path
 * verbatim when it is itself an absolute URL (containing "://") -- an
 * untrusted OPDS feed entry's href can therefore point anywhere, not just at
 * the configured server. Callers that hold credentials for a specific server
 * must check this before attaching them to a request built from feed-supplied
 * content, the same way HttpDownloader already refuses to carry credentials
 * across a cross-origin redirect.
 */
bool sameOrigin(const std::string& serverUrl, const std::string& url);

}  // namespace UrlUtils

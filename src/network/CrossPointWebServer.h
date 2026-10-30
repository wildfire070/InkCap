#pragma once

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <NetworkUdp.h>
#include <WebServer.h>
#include <WebSocketsServer.h>

#include <memory>
#include <string>
#include <vector>

// Structure to hold file information
struct FileInfo {
  String name;
  size_t size;
  bool isEpub;
  bool isDirectory;
};

class CrossPointWebServer {
 public:
  struct WsUploadStatus {
    bool inProgress = false;
    size_t received = 0;
    size_t total = 0;
    std::string filename;
    std::string lastCompleteName;
    std::string lastCompletePath;  // folder + filename, as actually landed (AO3 Receive may dedup it)
    size_t lastCompleteSize = 0;
    unsigned long lastCompleteAt = 0;

    // Running tally for the current Calibre Wireless screen visit. The plugin
    // reconnects the WebSocket for every individual file with varying gaps
    // between files, so there's no reliable single signal for "the send job is
    // over" -- these simply accumulate for as long as the server has been
    // running and are only reset when the screen is re-entered (see begin()).
    std::vector<std::string> batchSucceeded;
    std::vector<std::string> batchFailed;  // "filename: reason"

    // Set once per explicit "the whole send job just finished" ping from a
    // patched Calibre plugin (POST /api/calibre-job-done; see the job-done
    // patch in the calibre-plugins repo) -- jobCompleteAt changes on each ping,
    // and lastJobSucceeded/lastJobFailed are a frozen snapshot of the batch at
    // that exact moment (the live batch above is cleared right after, ready for
    // the next job). An unpatched plugin never calls this, so jobCompleteAt
    // simply stays 0 forever and callers rely on the live batch instead.
    unsigned long jobCompleteAt = 0;
    std::vector<std::string> lastJobSucceeded;
    std::vector<std::string> lastJobFailed;
  };

  // Used by POST upload handler
  struct UploadState {
    HalFile file;
    String fileName;
    String path = "/";
    size_t size = 0;
    bool success = false;
    String error = "";

    // Upload write buffer - batches small writes into larger SD card operations
    // 4KB is a good balance: large enough to reduce syscall overhead, small enough
    // to keep individual write times short and the server responsive
    static constexpr size_t UPLOAD_BUFFER_SIZE = 4096;  // 4KB buffer
    std::vector<uint8_t> buffer;
    size_t bufferPos = 0;

    UploadState() { buffer.resize(UPLOAD_BUFFER_SIZE); }
  } upload;

  CrossPointWebServer();
  ~CrossPointWebServer();

  // Start the web server (call after WiFi is connected)
  void begin();

  // Stop the web server
  void stop();

  // Call this periodically to handle client requests
  void handleClient();

  // Check if server is running
  bool isRunning() const { return running; }

  WsUploadStatus getWsUploadStatus() const;

  // AO3 Receive mode: uploads with no destination folder land in `folder` (created if
  // missing), a name that is already taken gets a numeric suffix instead of failing, and
  // every completed .epub upload is added to the pending-review list. Cleared by stop().
  void enableAo3Receive(const std::string& folder);

  // Borrowed callback/context, owned by the activity that owns this server.
  using UploadCancelCheck = bool (*)(void*);
  void setUploadCancelCheck(UploadCancelCheck check, void* context) {
    uploadCancelCheck = check;
    uploadCancelContext = context;
  }

  // Get the port number
  uint16_t getPort() const { return port; }

 private:
  std::unique_ptr<WebServer> server = nullptr;
  std::unique_ptr<WebSocketsServer> wsServer = nullptr;
  bool running = false;
  bool apMode = false;  // true when running in AP mode, false for STA mode
  uint16_t port = 80;
  uint16_t wsPort = 81;  // WebSocket port
  NetworkUDP udp;
  bool udpActive = false;
  UploadCancelCheck uploadCancelCheck = nullptr;
  void* uploadCancelContext = nullptr;
  bool dropUploadIfCancelled() const;
  void abortUpload(UploadState& state, const char* error = "Upload aborted") const;
  void abortFontUpload();

  // WebSocket upload state
  void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
  static void wsEventCallback(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
  void abortWsUpload(const char* tag, const char* reason);

  // File scanning
  using FileVisitor = void (*)(const FileInfo& info, void* context);
  bool scanFiles(const char* path, FileVisitor visitor, void* context) const;
  String formatFileSize(size_t bytes) const;
  bool isEpubFile(const String& filename) const;

  // Request handlers
  void handleRoot() const;
  void handleJszip() const;
  void handleStyleCss() const;
  void handleLogo() const;
  void handleNotFound() const;
  void handleStatus() const;
  void handleFileList() const;
  void handleFileListData() const;
  void handleDownload() const;
  void handleUpload(UploadState& state) const;
  void handleUploadPost(UploadState& state) const;
  void handleCalibreJobDone();
  void handleCreateFolder() const;
  void handleRename() const;
  void handleMove() const;
  void handleDelete() const;

  // Settings handlers
  void handleSettingsPage() const;
  void handleGetSettings() const;
  void handlePostSettings();
  void handleGetStatusBars() const;
  void handlePostStatusBars();

  // Font management handlers
  void handleFontsPage() const;
  void handleFontList() const;
  void handleFontUpload();
  void handleFontUploadData();
  void handleFontDelete();

  // Font upload state
  struct FontUploadState {
    HalFile file;
    std::string familyName;
    std::string filePath;
    bool valid = false;
    size_t bytesWritten = 0;
    static constexpr size_t BUFFER_SIZE = 4096;
    std::vector<uint8_t> buffer;
    size_t bufferPos = 0;

    FontUploadState() { buffer.resize(BUFFER_SIZE); }
  } fontUpload;

  // OPDS server handlers
  void handleGetOpdsServers() const;
  void handlePostOpdsServer();
  void handleDeleteOpdsServer();

  // Wi-Fi credential handlers
  void handleGetWifiNetworks() const;
  void handlePostWifiNetwork();
  void handleDeleteWifiNetwork();
  // Missing or malformed JSON sends a 400 response and returns false.
  bool readJsonBody(JsonDocument& out) const;
  void sendJson(const JsonDocument& doc) const;
  bool rejectIfPluginsDisabled() const;
  void handlePluginList() const;  // GET  /api/plugins   -> discovered plugins
  void handlePluginFile() const;  // GET  /plugin?name&file -> serve SD file
  void handleRelay();             // POST /api/relay     -> device makes an HTTP(S) call
  void handleCrypto();            // POST /api/crypto    -> generic crypto primitive (base64 I/O)
  void handleFetch();             // POST /api/fetch     -> device downloads a URL to SD
  void handleBookKey();           // POST /api/book-key  -> store a protected book's wrapped content key
  void handlePluginFs();          // POST /api/plugin-fs -> plugin writes a small file to SD
  void handlePluginFsUpload();    // its multipart file part, streamed to <path>.tmp

  // One /api/plugin-fs write in flight: chunks land in `tmp`, which replaces
  // `path` only after a complete, non-empty body.
  struct PluginFsUploadState {
    HalFile file;
    std::string path, tmp;
    size_t bytes = 0;
    bool started = false;
    int errorStatus = 0;  // non-zero: HTTP status to answer with
    const char* error = nullptr;
  } pluginFsUpload;

  // SD-plugin job queue. External systems (a companion app, a script) enqueue
  // {plugin, action, args}; any open page hosting the plugin (File Manager,
  // Settings, or the headless /plugins-run page) claims and executes it, then
  // posts the result. The firmware only stores small JSON blobs — plugin logic
  // never runs on-device. Fixed pool inside this (heap-allocated, web-session
  // lifetime) object: no allocation per job, oldest finished slot recycled.
  struct PluginJob {
    uint32_t id = 0;         // 0 = empty slot
    uint32_t claim = 0;      // current claim; a completion must echo it
    uint32_t updatedAt = 0;  // millis() of last state change
    uint8_t state = 0;
    char plugin[24] = {0};
    char action[24] = {0};
    char args[192] = {0};    // Serialized JSON value
    char result[192] = {0};  // Serialized JSON value from the executor
  };
  static constexpr uint8_t JOB_EMPTY = 0;
  static constexpr uint8_t JOB_PENDING = 1;
  static constexpr uint8_t JOB_RUNNING = 2;
  static constexpr uint8_t JOB_DONE = 3;
  static constexpr uint8_t JOB_ERROR = 4;
  static constexpr size_t MAX_PLUGIN_JOBS = 6;
  static constexpr uint32_t PLUGIN_JOB_LEASE_MS = 10UL * 60 * 1000;
  PluginJob pluginJobs[MAX_PLUGIN_JOBS];
  uint32_t nextPluginJobId = 1;
  uint32_t nextPluginJobClaim = 1;
  PluginJob* allocPluginJob();
  void handlePluginRunnerPage() const;  // GET /plugins-run -> headless executor page
  void handlePluginJobSubmit();         // POST /api/plugin-jobs          -> {id}
  void handlePluginJobClaim();          // GET  /api/plugin-jobs/claim    -> next pending job for a plugin
  void handlePluginJobComplete();       // POST /api/plugin-jobs/complete -> executor posts the outcome
  void handlePluginJobStatus();         // GET  /api/plugin-jobs/status   -> external caller polls

  // An outbound transfer blocks the serving task for its whole duration, so
  // the WebSocket server and discovery UDP cannot answer anyone until it
  // finishes; their buffers are worth more as TLS headroom (4.4KB heap floor
  // measured with them resident during a large transfer).
  void suspendTransferServices();
  void resumeTransferServices();
  void releasePluginRequest();
  void streamFileToClient(HalFile& file) const;
};

diff --git a/chrome/browser/browseros_server/browseros_server_manager.cc b/chrome/browser/browseros_server/browseros_server_manager.cc
new file mode 100644
index 0000000000000..e5ebe48cf4432
--- /dev/null
+++ b/chrome/browser/browseros_server/browseros_server_manager.cc
@@ -0,0 +1,899 @@
+// Copyright 2024 The Chromium Authors
+// Use of this source code is governed by a BSD-style license that can be
+// found in the LICENSE file.
+
+#include "chrome/browser/browseros_server/browseros_server_manager.h"
+
+#include "base/command_line.h"
+#include "base/files/file_path.h"
+#include "base/files/file_util.h"
+#include "base/json/json_writer.h"
+#include "base/logging.h"
+#include "base/path_service.h"
+#include "base/process/kill.h"
+#include "base/process/launch.h"
+#include "base/rand_util.h"
+#include "base/strings/string_number_conversions.h"
+#include "base/system/sys_info.h"
+#include "base/task/thread_pool.h"
+#include "base/threading/thread_restrictions.h"
+#include "build/build_config.h"
+#include "chrome/browser/browser_process.h"
+#include "chrome/browser/browseros_server/browseros_server_prefs.h"
+#include "chrome/browser/net/system_network_context_manager.h"
+#include "chrome/browser/profiles/profile.h"
+#include "chrome/browser/profiles/profile_manager.h"
+#include "chrome/common/chrome_paths.h"
+#include "components/metrics/browseros_metrics/browseros_metrics_service.h"
+#include "components/metrics/browseros_metrics/browseros_metrics_service_factory.h"
+#include "components/prefs/pref_change_registrar.h"
+#include "components/prefs/pref_service.h"
+#include "components/version_info/version_info.h"
+#include "content/public/browser/devtools_agent_host.h"
+#include "content/public/browser/devtools_socket_factory.h"
+#include "content/public/browser/storage_partition.h"
+#include "net/base/net_errors.h"
+#include "net/base/port_util.h"
+#include "net/log/net_log_source.h"
+#include "net/socket/tcp_server_socket.h"
+#include "net/traffic_annotation/network_traffic_annotation.h"
+#include "services/network/public/cpp/resource_request.h"
+#include "services/network/public/cpp/simple_url_loader.h"
+#include "services/network/public/mojom/url_loader_factory.mojom.h"
+#include "url/gurl.h"
+
+namespace {
+
+const int kBackLog = 10;
+
+// Helper function to check for command-line port override.
+// Returns the port value if valid override is found, 0 otherwise.
+int GetPortOverrideFromCommandLine(base::CommandLine* command_line,
+                                    const char* switch_name,
+                                    const char* port_name) {
+  if (!command_line->HasSwitch(switch_name)) {
+    return 0;
+  }
+
+  std::string port_str = command_line->GetSwitchValueASCII(switch_name);
+  int port = 0;
+
+  if (!base::StringToInt(port_str, &port) || !net::IsPortValid(port) ||
+      port <= 0) {
+    LOG(WARNING) << "browseros: Invalid " << port_name
+                 << " specified on command line: " << port_str
+                 << " (must be 1-65535)";
+    return 0;
+  }
+
+  // Warn about problematic ports but respect explicit user intent
+  if (net::IsWellKnownPort(port)) {
+    LOG(WARNING) << "browseros: " << port_name << " " << port
+                 << " is well-known (0-1023) and may require elevated "
+                    "privileges";
+  }
+  if (!net::IsPortAllowedForScheme(port, "http")) {
+    LOG(WARNING) << "browseros: " << port_name << " " << port
+                 << " is restricted by Chromium (may interfere with system "
+                    "services)";
+  }
+
+  LOG(INFO) << "browseros: " << port_name
+            << " overridden via command line: " << port;
+  return port;
+}
+
+// Launches the BrowserOS server process on a background thread.
+// This function performs blocking I/O operations (PathExists, LaunchProcess).
+base::Process LaunchProcessOnBackgroundThread(
+    const base::FilePath& exe_path,
+    const base::FilePath& resources_dir,
+    uint16_t cdp_port,
+    uint16_t mcp_port,
+    uint16_t agent_port,
+    uint16_t extension_port) {
+  // Check if executable exists (blocking I/O)
+  if (!base::PathExists(exe_path)) {
+    LOG(ERROR) << "browseros: BrowserOS server executable not found at: "
+               << exe_path;
+    return base::Process();
+  }
+
+  // Build command line
+  base::CommandLine cmd(exe_path);
+  cmd.AppendSwitchASCII("cdp-port", base::NumberToString(cdp_port));
+  cmd.AppendSwitchASCII("http-mcp-port", base::NumberToString(mcp_port));
+  cmd.AppendSwitchASCII("agent-port", base::NumberToString(agent_port));
+  cmd.AppendSwitchASCII("extension-port", base::NumberToString(extension_port));
+  cmd.AppendSwitchPath("resources-dir", resources_dir);
+
+  // Set up launch options
+  base::LaunchOptions options;
+#if BUILDFLAG(IS_WIN)
+  options.start_hidden = true;
+#endif
+
+  // Launch the process (blocking I/O)
+  return base::LaunchProcess(cmd, options);
+}
+
+// Factory for creating TCP server sockets for CDP
+class CDPServerSocketFactory : public content::DevToolsSocketFactory {
+ public:
+  explicit CDPServerSocketFactory(uint16_t port) : port_(port) {}
+
+  CDPServerSocketFactory(const CDPServerSocketFactory&) = delete;
+  CDPServerSocketFactory& operator=(const CDPServerSocketFactory&) = delete;
+
+ private:
+  std::unique_ptr<net::ServerSocket> CreateLocalHostServerSocket(int port) {
+    std::unique_ptr<net::ServerSocket> socket(
+        new net::TCPServerSocket(nullptr, net::NetLogSource()));
+    if (socket->ListenWithAddressAndPort("127.0.0.1", port, kBackLog) ==
+        net::OK) {
+      return socket;
+    }
+    if (socket->ListenWithAddressAndPort("::1", port, kBackLog) == net::OK) {
+      return socket;
+    }
+    return nullptr;
+  }
+
+  // content::DevToolsSocketFactory implementation
+  std::unique_ptr<net::ServerSocket> CreateForHttpServer() override {
+    return CreateLocalHostServerSocket(port_);
+  }
+
+  std::unique_ptr<net::ServerSocket> CreateForTethering(
+      std::string* name) override {
+    return nullptr;  // Tethering not needed for BrowserOS
+  }
+
+  uint16_t port_;
+};
+
+}  // namespace
+
+// static
+BrowserOSServerManager* BrowserOSServerManager::GetInstance() {
+  static base::NoDestructor<BrowserOSServerManager> instance;
+  return instance.get();
+}
+
+BrowserOSServerManager::BrowserOSServerManager() = default;
+
+BrowserOSServerManager::~BrowserOSServerManager() {
+  Shutdown();
+}
+
+void BrowserOSServerManager::InitializePortsAndPrefs() {
+  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
+  PrefService* prefs = g_browser_process->local_state();
+
+  if (!prefs) {
+    cdp_port_ = browseros_server::kDefaultCDPPort;
+    mcp_port_ = browseros_server::kDefaultMCPPort;
+    agent_port_ = browseros_server::kDefaultAgentPort;
+    extension_port_ = browseros_server::kDefaultExtensionPort;
+    mcp_enabled_ = true;
+  } else {
+    cdp_port_ = prefs->GetInteger(browseros_server::kCDPServerPort);
+    if (cdp_port_ <= 0) {
+      cdp_port_ = browseros_server::kDefaultCDPPort;
+    }
+
+    mcp_port_ = prefs->GetInteger(browseros_server::kMCPServerPort);
+    if (mcp_port_ <= 0) {
+      mcp_port_ = browseros_server::kDefaultMCPPort;
+    }
+
+    agent_port_ = prefs->GetInteger(browseros_server::kAgentServerPort);
+    if (agent_port_ <= 0) {
+      agent_port_ = browseros_server::kDefaultAgentPort;
+    }
+
+    extension_port_ = prefs->GetInteger(browseros_server::kExtensionServerPort);
+    if (extension_port_ <= 0) {
+      extension_port_ = browseros_server::kDefaultExtensionPort;
+    }
+
+    mcp_enabled_ = prefs->GetBoolean(browseros_server::kMCPServerEnabled);
+
+    if (!pref_change_registrar_) {
+      pref_change_registrar_ = std::make_unique<PrefChangeRegistrar>();
+      pref_change_registrar_->Init(prefs);
+      pref_change_registrar_->Add(
+          browseros_server::kMCPServerEnabled,
+          base::BindRepeating(&BrowserOSServerManager::OnMCPEnabledChanged,
+                              base::Unretained(this)));
+    }
+  }
+
+  int mcp_override = GetPortOverrideFromCommandLine(
+      command_line, "browseros-mcp-port", "MCP port");
+  if (mcp_override > 0) {
+    mcp_port_ = mcp_override;
+    mcp_enabled_ = true;
+  }
+
+  int cdp_override = GetPortOverrideFromCommandLine(
+      command_line, "browseros-cdp-port", "CDP port");
+  if (cdp_override > 0) {
+    cdp_port_ = cdp_override;
+  }
+
+  int agent_override = GetPortOverrideFromCommandLine(
+      command_line, "browseros-agent-port", "Agent port");
+  if (agent_override > 0) {
+    agent_port_ = agent_override;
+  }
+
+  int extension_override = GetPortOverrideFromCommandLine(
+      command_line, "browseros-extension-port", "Extension port");
+  if (extension_override > 0) {
+    extension_port_ = extension_override;
+  }
+
+  if (prefs) {
+    prefs->SetInteger(browseros_server::kCDPServerPort, cdp_port_);
+    prefs->SetInteger(browseros_server::kMCPServerPort, mcp_port_);
+    prefs->SetInteger(browseros_server::kAgentServerPort, agent_port_);
+    prefs->SetInteger(browseros_server::kExtensionServerPort, extension_port_);
+    prefs->SetBoolean(browseros_server::kMCPServerEnabled, mcp_enabled_);
+    LOG(INFO) << "browseros: Ports initialized and saved to prefs - CDP: "
+              << cdp_port_ << ", MCP: " << mcp_port_ << ", Agent: "
+              << agent_port_ << ", Extension: " << extension_port_;
+  }
+}
+
+void BrowserOSServerManager::Start() {
+  if (is_running_) {
+    LOG(INFO) << "browseros: BrowserOS server already running";
+    return;
+  }
+
+  InitializePortsAndPrefs();
+
+  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
+  if (command_line->HasSwitch("disable-browseros-server")) {
+    LOG(INFO) << "browseros: BrowserOS server disabled via command line";
+    return;
+  }
+
+  LOG(INFO) << "browseros: Starting BrowserOS server";
+
+  StartCDPServer();
+  LaunchBrowserOSProcess();
+
+  health_check_timer_.Start(FROM_HERE, base::Seconds(60), this,
+                            &BrowserOSServerManager::CheckServerHealth);
+}
+
+void BrowserOSServerManager::Stop() {
+  if (!is_running_) {
+    return;
+  }
+
+  LOG(INFO) << "browseros: Stopping BrowserOS server";
+  health_check_timer_.Stop();
+  process_check_timer_.Stop();
+
+  TerminateBrowserOSProcess();
+  StopCDPServer();
+}
+
+bool BrowserOSServerManager::IsRunning() const {
+  return is_running_ && process_.IsValid();
+}
+
+void BrowserOSServerManager::Shutdown() {
+  Stop();
+}
+
+void BrowserOSServerManager::StartCDPServer() {
+  cdp_port_ = FindAvailablePort(cdp_port_);
+  mcp_port_ = FindAvailablePort(mcp_port_);
+  agent_port_ = FindAvailablePort(agent_port_);
+  extension_port_ = FindAvailablePort(extension_port_);
+
+  LOG(INFO) << "browseros: Starting CDP server on port " << cdp_port_;
+
+  content::DevToolsAgentHost::StartRemoteDebuggingServer(
+      std::make_unique<CDPServerSocketFactory>(cdp_port_),
+      base::FilePath(),
+      base::FilePath());
+
+  LOG(INFO) << "browseros: CDP WebSocket server started at ws://127.0.0.1:"
+            << cdp_port_;
+  LOG(INFO) << "browseros: MCP server port: " << mcp_port_
+            << " (enabled: " << (mcp_enabled_ ? "true" : "false") << ")";
+}
+
+void BrowserOSServerManager::StopCDPServer() {
+  if (cdp_port_ == 0) {
+    return;
+  }
+
+  LOG(INFO) << "browseros: Stopping CDP server";
+  content::DevToolsAgentHost::StopRemoteDebuggingServer();
+  cdp_port_ = 0;
+}
+
+void BrowserOSServerManager::LaunchBrowserOSProcess() {
+  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
+  base::FilePath exe_path = GetBrowserOSServerExecutablePath();
+  base::FilePath resources_dir;
+
+  // Determine resources directory:
+  // 1. Explicit override takes precedence
+  // 2. If binary is overridden but not resources, derive from binary location
+  // 3. Otherwise use default location
+  if (command_line->HasSwitch("browseros-server-resources-dir")) {
+    resources_dir = GetBrowserOSServerResourcesPath();
+  } else if (command_line->HasSwitch("browseros-server-binary")) {
+    // Custom binary: assume resources are two levels up from binary
+    // .../resources/bin/browseros_server -> .../resources/
+    resources_dir = exe_path.DirName().DirName();
+    LOG(INFO) << "browseros: Deriving resources from custom binary location";
+  } else {
+    resources_dir = GetBrowserOSServerResourcesPath();
+  }
+
+  LOG(INFO) << "browseros: Launching server - binary: " << exe_path;
+  LOG(INFO) << "browseros: Launching server - resources: " << resources_dir;
+
+  // Capture values to pass to background thread
+  uint16_t cdp_port = cdp_port_;
+  uint16_t mcp_port = mcp_port_;
+  uint16_t agent_port = agent_port_;
+  uint16_t extension_port = extension_port_;
+
+  // Post blocking work to background thread, get result back on UI thread
+  base::ThreadPool::PostTaskAndReplyWithResult(
+      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
+      base::BindOnce(&LaunchProcessOnBackgroundThread, exe_path, resources_dir,
+                     cdp_port, mcp_port, agent_port, extension_port),
+      base::BindOnce(&BrowserOSServerManager::OnProcessLaunched,
+                     weak_factory_.GetWeakPtr()));
+}
+
+void BrowserOSServerManager::OnProcessLaunched(base::Process process) {
+  if (!process.IsValid()) {
+    LOG(ERROR) << "browseros: Failed to launch BrowserOS server";
+    StopCDPServer();
+    return;
+  }
+
+  process_ = std::move(process);
+  is_running_ = true;
+
+  LOG(INFO) << "browseros: BrowserOS server started";
+  LOG(INFO) << "browseros: CDP port: " << cdp_port_;
+  LOG(INFO) << "browseros: MCP port: " << mcp_port_;
+  LOG(INFO) << "browseros: Agent port: " << agent_port_;
+  LOG(INFO) << "browseros: Extension port: " << extension_port_;
+
+  process_check_timer_.Start(FROM_HERE, base::Seconds(5), this,
+                             &BrowserOSServerManager::CheckProcessStatus);
+
+  // /init will be sent after first successful periodic health check
+
+  // If MCP is disabled, send control request to disable it
+  if (!mcp_enabled_) {
+    SendMCPControlRequest(false);
+  }
+}
+
+void BrowserOSServerManager::TerminateBrowserOSProcess() {
+  if (!process_.IsValid()) {
+    return;
+  }
+
+  LOG(INFO) << "browseros: Terminating BrowserOS server process";
+
+  // Reset init flag so it gets sent again after restart
+  init_request_sent_ = false;
+
+  // Try graceful shutdown first
+  process_.Terminate(0, false);
+
+  // Give it some time to shut down, then force kill if still running
+  base::ThreadPool::PostDelayedTask(
+      FROM_HERE, {base::MayBlock()},
+      base::BindOnce(
+          [](base::Process process) {
+            if (process.IsValid()) {
+              // Force kill if still running
+              process.Terminate(0, false);
+            }
+          },
+          process_.Duplicate()),
+      base::Seconds(2));
+
+  is_running_ = false;
+}
+
+void BrowserOSServerManager::OnProcessExited(int exit_code) {
+  LOG(INFO) << "browseros: BrowserOS server exited with code: " << exit_code;
+  is_running_ = false;
+
+  // Stop CDP server since BrowserOS process is gone
+  StopCDPServer();
+
+  // Restart if it crashed unexpectedly
+  if (exit_code != 0) {
+    LOG(WARNING) << "browseros: BrowserOS server crashed, restarting...";
+    Start();
+  }
+}
+
+void BrowserOSServerManager::CheckServerHealth() {
+  if (!is_running_) {
+    return;
+  }
+
+  // First check if process is still alive
+  if (!process_.IsValid()) {
+    LOG(WARNING) << "browseros: BrowserOS server process is invalid, restarting...";
+    RestartBrowserOSProcess();
+    return;
+  }
+
+  // Build health check URL
+  GURL health_url("http://127.0.0.1:" + base::NumberToString(mcp_port_) + "/health");
+
+  // Create network traffic annotation
+  net::NetworkTrafficAnnotationTag traffic_annotation =
+      net::DefineNetworkTrafficAnnotation("browseros_health_check", R"(
+        semantics {
+          sender: "BrowserOS Server Manager"
+          description:
+            "Checks if the BrowserOS MCP server is healthy by querying its "
+            "/health endpoint."
+          trigger: "Periodic health check every 60 seconds while server is running."
+          data: "No user data sent, just an HTTP GET request."
+          destination: LOCAL
+        }
+        policy {
+          cookies_allowed: NO
+          setting: "This feature cannot be disabled by settings."
+          policy_exception_justification:
+            "Internal health check for BrowserOS server functionality."
+        })");
+
+  // Create resource request
+  auto resource_request = std::make_unique<network::ResourceRequest>();
+  resource_request->url = health_url;
+  resource_request->method = "GET";
+  resource_request->credentials_mode = network::mojom::CredentialsMode::kOmit;
+
+  // Create URL loader with 10 second timeout
+  auto url_loader = network::SimpleURLLoader::Create(
+      std::move(resource_request), traffic_annotation);
+  url_loader->SetTimeoutDuration(base::Seconds(10));
+
+  // Get URL loader factory from default storage partition
+  auto* url_loader_factory =
+      g_browser_process->system_network_context_manager()
+          ->GetURLLoaderFactory();
+
+  // Keep a raw pointer for the callback
+  auto* url_loader_ptr = url_loader.get();
+
+  // Download response
+  url_loader_ptr->DownloadHeadersOnly(
+      url_loader_factory,
+      base::BindOnce(&BrowserOSServerManager::OnHealthCheckComplete,
+                     weak_factory_.GetWeakPtr(), std::move(url_loader)));
+}
+
+void BrowserOSServerManager::CheckProcessStatus() {
+  if (!is_running_ || !process_.IsValid()) {
+    return;
+  }
+
+  // Check if process has exited
+  int exit_code = 0;
+  if (process_.WaitForExitWithTimeout(base::TimeDelta(), &exit_code)) {
+    // Process has exited
+    OnProcessExited(exit_code);
+  }
+}
+
+void BrowserOSServerManager::OnHealthCheckComplete(
+    std::unique_ptr<network::SimpleURLLoader> url_loader,
+    scoped_refptr<net::HttpResponseHeaders> headers) {
+  if (!is_running_) {
+    return;
+  }
+
+  // Check if we got a valid response
+  int response_code = 0;
+  if (headers) {
+    response_code = headers->response_code();
+  }
+
+  if (response_code == 200) {
+    // Health check passed
+    LOG(INFO) << "browseros: Health check passed";
+
+    // Send /init request on first successful health check
+    if (!init_request_sent_) {
+      init_request_sent_ = true;
+      SendInitRequest();
+    }
+    return;
+  }
+
+  // Health check failed
+  int net_error = url_loader->NetError();
+  LOG(WARNING) << "browseros: Health check failed - HTTP " << response_code
+               << ", net error: " << net::ErrorToString(net_error)
+               << ", restarting BrowserOS server process...";
+
+  RestartBrowserOSProcess();
+}
+
+void BrowserOSServerManager::RestartBrowserOSProcess() {
+  LOG(INFO) << "browseros: Restarting BrowserOS server process";
+
+  // Stop the process and monitoring
+  process_check_timer_.Stop();
+  TerminateBrowserOSProcess();
+
+  // Relaunch the process
+  LaunchBrowserOSProcess();
+}
+
+void BrowserOSServerManager::OnMCPEnabledChanged() {
+  if (!is_running_) {
+    return;
+  }
+
+  PrefService* prefs = g_browser_process->local_state();
+  if (!prefs) {
+    return;
+  }
+
+  bool new_value = prefs->GetBoolean(browseros_server::kMCPServerEnabled);
+
+  if (new_value != mcp_enabled_) {
+    LOG(INFO) << "browseros: MCP enabled preference changed from "
+              << (mcp_enabled_ ? "true" : "false") << " to "
+              << (new_value ? "true" : "false");
+
+    mcp_enabled_ = new_value;
+    SendMCPControlRequest(new_value);
+  }
+}
+
+void BrowserOSServerManager::SendMCPControlRequest(bool enabled) {
+  if (!is_running_) {
+    return;
+  }
+
+  GURL control_url("http://127.0.0.1:" + base::NumberToString(mcp_port_) +
+                   "/mcp/control");
+
+  net::NetworkTrafficAnnotationTag traffic_annotation =
+      net::DefineNetworkTrafficAnnotation("browseros_mcp_control", R"(
+        semantics {
+          sender: "BrowserOS Server Manager"
+          description:
+            "Sends control command to BrowserOS MCP server to enable/disable "
+            "the MCP protocol at runtime."
+          trigger: "User changes MCP enabled preference."
+          data: "JSON payload with enabled state: {\"enabled\": true/false}"
+          destination: LOCAL
+        }
+        policy {
+          cookies_allowed: NO
+          setting: "This feature cannot be disabled by settings."
+          policy_exception_justification:
+            "Internal control request for BrowserOS server functionality."
+        })");
+
+  auto resource_request = std::make_unique<network::ResourceRequest>();
+  resource_request->url = control_url;
+  resource_request->method = "POST";
+  resource_request->credentials_mode = network::mojom::CredentialsMode::kOmit;
+  resource_request->headers.SetHeader("Content-Type", "application/json");
+
+  std::string json_body = enabled ? "{\"enabled\":true}" : "{\"enabled\":false}";
+
+  auto url_loader = network::SimpleURLLoader::Create(
+      std::move(resource_request), traffic_annotation);
+  url_loader->AttachStringForUpload(json_body, "application/json");
+  url_loader->SetTimeoutDuration(base::Seconds(10));
+
+  auto* url_loader_factory =
+      g_browser_process->system_network_context_manager()
+          ->GetURLLoaderFactory();
+
+  auto* url_loader_ptr = url_loader.get();
+
+  url_loader_ptr->DownloadHeadersOnly(
+      url_loader_factory,
+      base::BindOnce(&BrowserOSServerManager::OnMCPControlRequestComplete,
+                     weak_factory_.GetWeakPtr(), enabled,
+                     std::move(url_loader)));
+
+  LOG(INFO) << "browseros: Sent MCP control request: {\"enabled\": "
+            << (enabled ? "true" : "false") << "}";
+}
+
+void BrowserOSServerManager::OnMCPControlRequestComplete(
+    bool requested_state,
+    std::unique_ptr<network::SimpleURLLoader> url_loader,
+    scoped_refptr<net::HttpResponseHeaders> headers) {
+  if (!is_running_) {
+    return;
+  }
+
+  int response_code = 0;
+  if (headers) {
+    response_code = headers->response_code();
+  }
+
+  if (response_code == 200) {
+    LOG(INFO) << "browseros: MCP control request succeeded - MCP server is now "
+              << (requested_state ? "enabled" : "disabled");
+    return;
+  }
+
+  int net_error = url_loader->NetError();
+  LOG(ERROR) << "browseros: MCP control request failed - HTTP " << response_code
+             << ", net error: " << net::ErrorToString(net_error);
+}
+
+void BrowserOSServerManager::SendInitRequest() {
+  if (!is_running_) {
+    return;
+  }
+
+  // Get the default profile to access BrowserOSMetricsService
+  ProfileManager* profile_manager = g_browser_process->profile_manager();
+  if (!profile_manager) {
+    LOG(ERROR) << "browseros: Failed to get ProfileManager for /init request";
+    return;
+  }
+
+  Profile* profile = profile_manager->GetLastUsedProfileIfLoaded();
+  if (!profile || profile->IsOffTheRecord()) {
+    LOG(WARNING) << "browseros: No valid profile available for /init request";
+    return;
+  }
+
+  // Get BrowserOSMetricsService to retrieve install_id
+  browseros_metrics::BrowserOSMetricsService* metrics_service =
+      browseros_metrics::BrowserOSMetricsServiceFactory::GetForBrowserContext(
+          profile);
+  if (!metrics_service) {
+    LOG(ERROR) << "browseros: Failed to get BrowserOSMetricsService for /init "
+                  "request";
+    return;
+  }
+
+  // Build the /init payload
+  base::Value::Dict payload;
+  payload.Set("client_id", metrics_service->GetInstallId());
+  payload.Set("version", version_info::GetVersionNumber());
+  payload.Set("os", base::SysInfo::OperatingSystemName());
+  payload.Set("arch", base::SysInfo::OperatingSystemArchitecture());
+
+  std::string json_payload;
+  if (!base::JSONWriter::Write(payload, &json_payload)) {
+    LOG(ERROR) << "browseros: Failed to serialize /init payload";
+    return;
+  }
+
+  GURL init_url("http://127.0.0.1:" + base::NumberToString(mcp_port_) +
+                "/init");
+
+  net::NetworkTrafficAnnotationTag traffic_annotation =
+      net::DefineNetworkTrafficAnnotation("browseros_server_init", R"(
+        semantics {
+          sender: "BrowserOS Server Manager"
+          description:
+            "Sends initialization metadata to BrowserOS MCP server including "
+            "install ID, browser version, OS, and architecture."
+          trigger: "BrowserOS server process successfully launched."
+          data:
+            "JSON payload with install_id, version, os, and arch. No PII."
+          destination: LOCAL
+        }
+        policy {
+          cookies_allowed: NO
+          setting: "This feature cannot be disabled by settings."
+          policy_exception_justification:
+            "Internal initialization for BrowserOS server functionality."
+        })");
+
+  auto resource_request = std::make_unique<network::ResourceRequest>();
+  resource_request->url = init_url;
+  resource_request->method = "POST";
+  resource_request->credentials_mode = network::mojom::CredentialsMode::kOmit;
+  resource_request->headers.SetHeader("Content-Type", "application/json");
+
+  auto url_loader = network::SimpleURLLoader::Create(
+      std::move(resource_request), traffic_annotation);
+  url_loader->AttachStringForUpload(json_payload, "application/json");
+  url_loader->SetTimeoutDuration(base::Seconds(10));
+
+  auto* url_loader_factory =
+      g_browser_process->system_network_context_manager()
+          ->GetURLLoaderFactory();
+
+  auto* url_loader_ptr = url_loader.get();
+
+  url_loader_ptr->DownloadHeadersOnly(
+      url_loader_factory,
+      base::BindOnce(&BrowserOSServerManager::OnInitRequestComplete,
+                     weak_factory_.GetWeakPtr(), std::move(url_loader)));
+
+  LOG(INFO) << "browseros: Sent /init request to MCP server";
+}
+
+void BrowserOSServerManager::OnInitRequestComplete(
+    std::unique_ptr<network::SimpleURLLoader> url_loader,
+    scoped_refptr<net::HttpResponseHeaders> headers) {
+  if (!is_running_) {
+    return;
+  }
+
+  int response_code = 0;
+  if (headers) {
+    response_code = headers->response_code();
+  }
+
+  if (response_code == 200) {
+    LOG(INFO) << "browseros: /init request succeeded";
+    return;
+  }
+
+  int net_error = url_loader->NetError();
+  LOG(WARNING) << "browseros: /init request failed - HTTP " << response_code
+               << ", net error: " << net::ErrorToString(net_error);
+}
+
+int BrowserOSServerManager::FindAvailablePort(int starting_port) {
+  const int kMaxPortAttempts = 100;
+  const int kMaxPort = 65535;
+
+  LOG(INFO) << "browseros: Finding port starting from "
+            << starting_port;
+
+  for (int i = 0; i < kMaxPortAttempts; i++) {
+    int port_to_try = starting_port + i;
+
+    // Don't exceed max valid port number
+    if (port_to_try > kMaxPort) {
+      break;
+    }
+
+    if (IsPortAvailable(port_to_try)) {
+      if (port_to_try != starting_port) {
+        LOG(INFO) << "browseros: Port " << starting_port
+                  << " was in use, using " << port_to_try << " instead";
+      } else {
+        LOG(INFO) << "browseros: Using port " << port_to_try;
+      }
+      return port_to_try;
+    }
+  }
+
+  // Fallback to starting port if we couldn't find anything
+  LOG(WARNING) << "browseros: Could not find available port after "
+               << kMaxPortAttempts
+               << " attempts, using " << starting_port << " anyway";
+  return starting_port;
+}
+
+bool BrowserOSServerManager::IsPortAvailable(int port) {
+  // Check port is in valid range
+  if (!net::IsPortValid(port) || port == 0) {
+    return false;
+  }
+
+  // Avoid well-known ports (0-1023, require elevated privileges)
+  if (net::IsWellKnownPort(port)) {
+    return false;
+  }
+
+  // Avoid restricted ports (could interfere with system services)
+  if (!net::IsPortAllowedForScheme(port, "http")) {
+    return false;
+  }
+
+  // Try to bind to both IPv4 and IPv6 localhost
+  // If EITHER is in use, the port is NOT available
+  std::unique_ptr<net::TCPServerSocket> socket(
+      new net::TCPServerSocket(nullptr, net::NetLogSource()));
+
+  // Try binding to IPv4 localhost
+  int result = socket->ListenWithAddressAndPort("127.0.0.1", port, 1);
+  if (result != net::OK) {
+    return false;  // IPv4 port is in use
+  }
+
+  // Try binding to IPv6 localhost
+  std::unique_ptr<net::TCPServerSocket> socket6(
+      new net::TCPServerSocket(nullptr, net::NetLogSource()));
+  int result6 = socket6->ListenWithAddressAndPort("::1", port, 1);
+  if (result6 != net::OK) {
+    return false;  // IPv6 port is in use
+  }
+
+  return true;
+}
+
+base::FilePath BrowserOSServerManager::GetBrowserOSServerResourcesPath() const {
+  // Check for command-line override first
+  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
+  if (command_line->HasSwitch("browseros-server-resources-dir")) {
+    base::FilePath custom_path =
+        command_line->GetSwitchValuePath("browseros-server-resources-dir");
+    LOG(INFO) << "browseros: Using custom resources dir from command line: "
+              << custom_path;
+    return custom_path;
+  }
+
+  base::FilePath exe_dir;
+
+#if BUILDFLAG(IS_MAC)
+  // On macOS, the binary will be in the app bundle
+  if (!base::PathService::Get(base::DIR_EXE, &exe_dir)) {
+    LOG(ERROR) << "browseros: Failed to get executable directory";
+    return base::FilePath();
+  }
+
+  // Navigate to Resources folder in the app bundle
+  // Chrome.app/Contents/MacOS -> Chrome.app/Contents/Resources
+  exe_dir = exe_dir.DirName().Append("Resources");
+
+#elif BUILDFLAG(IS_WIN)
+  // On Windows, installer places BrowserOS Server under the versioned directory
+  if (!base::PathService::Get(base::DIR_EXE, &exe_dir)) {
+    LOG(ERROR) << "browseros: Failed to get executable directory";
+    return base::FilePath();
+  }
+  // Append version directory (chrome.release places BrowserOSServer under versioned dir)
+  exe_dir = exe_dir.AppendASCII(version_info::GetVersionNumber());
+
+#elif BUILDFLAG(IS_LINUX)
+  // On Linux, binary is in the same directory as chrome
+  if (!base::PathService::Get(base::DIR_EXE, &exe_dir)) {
+    LOG(ERROR) << "browseros: Failed to get executable directory";
+    return base::FilePath();
+  }
+#endif
+
+  // Return path to resources directory
+  return exe_dir.Append(FILE_PATH_LITERAL("BrowserOSServer"))
+      .Append(FILE_PATH_LITERAL("default"))
+      .Append(FILE_PATH_LITERAL("resources"));
+}
+
+base::FilePath BrowserOSServerManager::GetBrowserOSServerExecutablePath() const {
+  // Check for direct binary path override first
+  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
+  if (command_line->HasSwitch("browseros-server-binary")) {
+    base::FilePath custom_path =
+        command_line->GetSwitchValuePath("browseros-server-binary");
+    LOG(INFO) << "browseros: Using custom server binary from command line: "
+              << custom_path;
+    return custom_path;
+  }
+
+  // Derive executable path from resources directory
+  base::FilePath browseros_exe =
+      GetBrowserOSServerResourcesPath()
+          .Append(FILE_PATH_LITERAL("bin"))
+          .Append(FILE_PATH_LITERAL("browseros_server"));
+
+#if BUILDFLAG(IS_WIN)
+  browseros_exe = browseros_exe.AddExtension(FILE_PATH_LITERAL(".exe"));
+#endif
+
+  return browseros_exe;
+}

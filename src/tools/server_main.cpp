// gsplat_server: the PC side of the phone capture workflow (task 3.4).
//
//   gsplat_server [--port 8765] [--scenes DIR] [--steps 30000]
//
// * http://<pc>:8765/ in the phone's browser: pick a video from the gallery, it uploads to
//   <scenes>/<name>/<name>.mp4 and is queued.
// * A worker runs tools/capture.ps1 on each queued video (frames -> COLMAP -> Brush), one at a time.
// * The Android app finds the server with a UDP broadcast (port 8766), lists the scenes
//   (GET /api/list) and downloads a finished one (GET /api/files + /files/<name>/<path>): the PLY
//   plus the COLMAP cameras, so the phone opens it level and at the first training camera.
// LAN only, no authentication: run it on a trusted network.
#include "core/net.h"
#include "core/platform.h"
#include "core/utf8_path.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace gsv;

namespace {

struct Job {
  std::string name;
  std::string state;  // queued | running | done | failed
  std::string message;
  double seconds = 0.0;
};

struct Server {
  fs::path scenes, repo;
  int steps = 30000;
  std::mutex mutex;
  std::condition_variable wake;
  std::vector<Job> jobs;
};

std::string jsonEscape(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\', o += c;
    else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

// Scene names become folder names: keep them boring.
std::string sanitizeName(const std::string& in) {
  std::string out;
  for (char c : in)
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') out += c;
  if (out.size() > 40) out.resize(40);
  return out;
}

fs::path scenePly(const fs::path& sceneDir) { return sceneDir / (pathToUtf8(sceneDir.filename()) + ".ply"); }

// Files the phone needs for a scene: the trained splats and the COLMAP cameras (orientation and
// the first training view). Relative to the scene folder, '/' separators.
std::vector<std::string> sceneFiles(const fs::path& dir) {
  std::vector<std::string> files;
  std::error_code ec;
  if (fs::is_regular_file(scenePly(dir), ec)) files.push_back(pathToUtf8(scenePly(dir).filename()));
  for (const char* f : {"cameras.bin", "images.bin", "cameras.txt", "images.txt"})
    if (fs::is_regular_file(dir / "dataset" / "sparse" / "0" / f, ec)) files.push_back(std::string("dataset/sparse/0/") + f);
  return files;
}

void scanExistingScenes(Server& srv) {
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(srv.scenes, ec)) {
    if (!e.is_directory(ec) || !fs::is_regular_file(scenePly(e.path()), ec)) continue;
    srv.jobs.push_back({pathToUtf8(e.path().filename()), "done", "trained earlier", 0.0});
  }
}

// ---- worker: one capture at a time --------------------------------------------------------------
void worker(Server& srv, const bool& stop) {
  for (;;) {
    std::string name;
    {
      std::unique_lock<std::mutex> lock(srv.mutex);
      srv.wake.wait(lock, [&] {
        if (stop) return true;
        for (const Job& j : srv.jobs)
          if (j.state == "queued") return true;
        return false;
      });
      if (stop) return;
      for (Job& j : srv.jobs)
        if (j.state == "queued") {
          j.state = "running";
          j.message = "frames -> COLMAP -> Brush";
          name = j.name;
          break;
        }
    }
    const fs::path dir = srv.scenes / name;
    fs::path video;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
      if (e.path().stem() == name && e.path().extension() != ".ply") video = e.path();
    fs::create_directories(dir / "logs", ec);
    const std::string cmd = "powershell -NoProfile -ExecutionPolicy Bypass -File " +
                            quoteArgument(pathToUtf8(srv.repo / "tools" / "capture.ps1")) + " " +
                            quoteArgument(pathToUtf8(video)) + " -NoViewer -Steps " + std::to_string(srv.steps) + " > " +
                            quoteArgument(pathToUtf8(dir / "logs" / "server_capture.log")) + " 2>&1";
    std::printf("[job %s] %s\n", name.c_str(), cmd.c_str());
    const auto t0 = std::chrono::steady_clock::now();
    const int code = video.empty() ? -1 : runProcess(cmd);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::lock_guard<std::mutex> lock(srv.mutex);
    for (Job& j : srv.jobs)
      if (j.name == name) {
        j.seconds = s;
        const bool ok = code == 0 && fs::is_regular_file(scenePly(dir), ec);
        j.state = ok ? "done" : "failed";
        j.message = ok ? "ready" : video.empty() ? "no video" : "see " + name + "/logs/server_capture.log";
        std::printf("[job %s] %s in %.0f s\n", name.c_str(), j.state.c_str(), s);
      }
  }
}

// ---- HTTP handlers ------------------------------------------------------------------------------
const char* kPage = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><title>gsplat capture</title>
<style>body{font:16px system-ui,sans-serif;margin:16px;max-width:720px}input,button{font-size:17px;margin:6px 0}
progress{width:100%;height:22px}table{border-collapse:collapse;width:100%}td,th{border-bottom:1px solid #ccc;padding:6px;text-align:left}
.done{color:#1a7f37}.failed{color:#c00}.running{color:#9a6700}</style></head><body>
<h2>gsplat capture</h2>
<p>Film a scene with the phone camera (1080p or 4K, 30 fps, walk around it slowly), then send it here.
Training takes 10&ndash;20 minutes; open the gsplat-viewer app to download the result.</p>
<label>Scene name <input id="name" placeholder="desk" pattern="[A-Za-z0-9_-]+"></label><br>
<input type="file" id="file" accept="video/*"><br>
<button onclick="send()">Upload and train</button>
<progress id="bar" value="0" max="1" hidden></progress><div id="msg"></div>
<h3>Scenes</h3><table><thead><tr><th>Scene</th><th>State</th><th>Time</th><th></th></tr></thead><tbody id="jobs"></tbody></table>
<script>
function send(){const f=document.getElementById('file').files[0];let n=document.getElementById('name').value.replace(/[^A-Za-z0-9_-]/g,'');
 if(!f){msg.textContent='Pick a video first';return} if(!n)n='scene'+Date.now()%100000;
 const ext=(f.name.split('.').pop()||'mp4').toLowerCase();const x=new XMLHttpRequest();bar.hidden=false;
 x.upload.onprogress=e=>{if(e.lengthComputable)bar.value=e.loaded/e.total;msg.textContent=(e.loaded/1048576).toFixed(0)+' / '+(e.total/1048576).toFixed(0)+' MB'};
 x.onload=()=>{msg.textContent=x.responseText;load()};x.onerror=()=>msg.textContent='Upload failed';
 x.open('POST','/upload?name='+encodeURIComponent(n)+'&ext='+encodeURIComponent(ext));x.send(f)}
function load(){fetch('/api/jobs').then(r=>r.json()).then(js=>{jobs.innerHTML=js.map(j=>'<tr><td>'+j.name+'</td><td class="'+j.state+'">'+j.state+
 '</td><td>'+(j.seconds?Math.round(j.seconds/60)+' min':'')+'</td><td>'+j.message+'</td></tr>').join('')})}
load();setInterval(load,3000);
</script></body></html>)HTML";

void handleUpload(Server& srv, net::Socket& s, net::Request& r) {
  std::string name = sanitizeName(net::queryValue(r.query, "name"));
  std::string ext = sanitizeName(net::queryValue(r.query, "ext"));
  if (name.empty()) return void(net::sendResponse(s, 400, "text/plain", "missing scene name\n"));
  if (ext != "mp4" && ext != "mov" && ext != "mkv" && ext != "webm" && ext != "3gp") ext = "mp4";
  if (r.contentLength <= 0) return void(net::sendResponse(s, 400, "text/plain", "missing Content-Length\n"));
  if (r.contentLength > (8ll << 30)) return void(net::sendResponse(s, 413, "text/plain", "video larger than 8 GB\n"));
  {
    std::lock_guard<std::mutex> lock(srv.mutex);
    std::error_code ec;
    const std::string base = name;
    for (int k = 2; fs::exists(srv.scenes / name, ec); ++k) name = base + "_" + std::to_string(k);  // never overwrite
    fs::create_directories(srv.scenes / name, ec);
  }
  const fs::path video = srv.scenes / name / (name + "." + ext);
  std::ofstream f(video, std::ios::binary);
  long long received = static_cast<long long>(r.bodyStart.size());
  f.write(r.bodyStart.data(), static_cast<std::streamsize>(r.bodyStart.size()));
  std::vector<char> buf(1 << 20);
  s.setTimeout(60000);
  while (received < r.contentLength) {
    const long n = s.receive(buf.data(), buf.size());
    if (n <= 0) break;
    f.write(buf.data(), n);
    received += n;
  }
  f.close();
  if (received != r.contentLength) {
    std::error_code ec;
    fs::remove(video, ec);
    return void(net::sendResponse(s, 400, "text/plain", "upload interrupted\n"));
  }
  {
    std::lock_guard<std::mutex> lock(srv.mutex);
    srv.jobs.push_back({name, "queued", "waiting", 0.0});
  }
  srv.wake.notify_all();
  std::printf("[upload] %s: %.1f MB\n", name.c_str(), static_cast<double>(received) / 1048576.0);
  net::sendResponse(s, 200, "text/plain", "Queued '" + name + "' (" + std::to_string(received >> 20) + " MB). Training starts now.\n");
}

void handle(Server& srv, net::Socket s) {
  net::Request r;
  s.setTimeout(15000);
  if (!net::readRequest(s, r)) return;
  if (r.method == "POST" && r.path == "/upload") return handleUpload(srv, s, r);
  if (r.method != "GET") return void(net::sendResponse(s, 405, "text/plain", "method not allowed\n"));
  if (r.path == "/") return void(net::sendResponse(s, 200, "text/html; charset=utf-8", kPage));
  if (r.path == "/api/jobs" || r.path == "/api/list") {
    std::lock_guard<std::mutex> lock(srv.mutex);
    std::string json = "[", text;
    for (const Job& j : srv.jobs) {
      if (json.size() > 1) json += ',';
      json += "{\"name\":\"" + jsonEscape(j.name) + "\",\"state\":\"" + j.state + "\",\"message\":\"" +
              jsonEscape(j.message) + "\",\"seconds\":" + std::to_string(static_cast<int>(j.seconds)) + "}";
      text += j.name + "\t" + j.state + "\t" + j.message + "\n";  // for the app: name, state, message
    }
    json += "]";
    return void(r.path == "/api/jobs" ? net::sendResponse(s, 200, "application/json", json)
                                      : net::sendResponse(s, 200, "text/plain; charset=utf-8", text));
  }
  if (r.path == "/api/files") {  // "relative/path\tsize" per line
    const std::string name = sanitizeName(net::queryValue(r.query, "name"));
    std::string text;
    std::error_code ec;
    for (const std::string& f : sceneFiles(srv.scenes / name))
      text += f + "\t" + std::to_string(fs::file_size(srv.scenes / name / pathFromUtf8(f), ec)) + "\n";
    return void(net::sendResponse(s, text.empty() ? 404 : 200, "text/plain; charset=utf-8", text));
  }
  if (r.path.rfind("/files/", 0) == 0) {  // /files/<name>/<relative path>, only the listed files
    const std::string rest = r.path.substr(7);
    const size_t slash = rest.find('/');
    const std::string name = sanitizeName(rest.substr(0, slash));
    const std::string rel = slash == std::string::npos ? std::string() : rest.substr(slash + 1);
    for (const std::string& f : sceneFiles(srv.scenes / name))
      if (f == rel) return void(net::sendFile(s, pathToUtf8(srv.scenes / name / pathFromUtf8(f)), "application/octet-stream"));
    return void(net::sendResponse(s, 404, "text/plain", "not found\n"));
  }
  net::sendResponse(s, 404, "text/plain", "not found\n");
}

}  // namespace

int main(int argc, char** argv) {
  Utf8Argv args(argc, argv);
  Server srv;
  // The repository is the parent of the build folder that holds this executable.
  srv.repo = pathFromUtf8(executableDirectory()).parent_path();
  srv.scenes = srv.repo / "scenes";
  uint16_t port = net::kHttpPort;
  for (int i = 1; i < args.argc(); ++i) {
    const std::string a = args.strings[static_cast<size_t>(i)];
    auto next = [&]() { return i + 1 < args.argc() ? args.strings[static_cast<size_t>(++i)] : std::string(); };
    if (a == "--port") port = static_cast<uint16_t>(std::atoi(next().c_str()));
    else if (a == "--scenes") srv.scenes = pathFromUtf8(next());
    else if (a == "--repo") srv.repo = pathFromUtf8(next());
    else if (a == "--steps") srv.steps = std::max(1000, std::atoi(next().c_str()));
    else {
      std::fprintf(stderr, "usage: gsplat_server [--port 8765] [--scenes DIR] [--repo DIR] [--steps 30000]\n");
      return 2;
    }
  }
  std::error_code ec;
  fs::create_directories(srv.scenes, ec);
  scanExistingScenes(srv);

  std::string error;
  net::Socket listener = net::listenTcp(port, error);
  if (!listener.valid()) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  bool stop = false;
  std::thread discovery([&] { net::serveDiscovery(port, stop); });
  std::thread work([&] { worker(srv, stop); });
  std::printf("gsplat_server on port %u (scenes: %s, %zu ready)\n"
              "Open http://<this PC's IP>:%u/ in the phone's browser to send a video.\n",
              port, pathToUtf8(srv.scenes).c_str(), srv.jobs.size(), port);
  std::fflush(stdout);
  for (;;) {
    std::string peer;
    net::Socket client = net::acceptClient(listener, &peer);
    if (!client.valid()) continue;
    std::thread([&srv](net::Socket c) { handle(srv, std::move(c)); }, std::move(client)).detach();
  }
}

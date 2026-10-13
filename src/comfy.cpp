#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

#include "comfy.h"

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <cmath>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>

namespace comfy {

static std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

Server Server::fromUrl(const std::string& url) {
    Server s;
    std::string u = url;
    if (u.rfind("https://", 0) == 0) {
        s.https = true;
        s.port = 443;
        u = u.substr(8);
    } else if (u.rfind("http://", 0) == 0) {
        u = u.substr(7);
    }
    size_t slash = u.find('/');
    if (slash != std::string::npos) u = u.substr(0, slash);
    size_t colon = u.rfind(':');
    if (colon != std::string::npos) {
        s.port = std::atoi(u.substr(colon + 1).c_str());
        u = u.substr(0, colon);
    }
    s.host = u.empty() ? "127.0.0.1" : u;
    return s;
}

std::string Server::label() const {
    if (cloud) return "Comfy Cloud";
    return std::string(https ? "https://" : "http://") + host + ":" + std::to_string(port);
}

static Http requestUrl(bool https, const std::string& host, int port, const char* method, const std::string& path,
                       const std::string& body, const std::string& contentType, const std::string& apiKey) {
    Http r;
    HINTERNET ses = WinHttpOpen(L"AlphaGenSAM-OFX/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) {
        r.error = "WinHttpOpen failed";
        return r;
    }
    WinHttpSetTimeouts(ses, 10000, 10000, 120000, 300000);
    HINTERNET con = WinHttpConnect(ses, widen(host).c_str(), (INTERNET_PORT)port, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, widen(method).c_str(), widen(path).c_str(), nullptr,
                                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             https ? WINHTTP_FLAG_SECURE : 0)
                        : nullptr;
    if (req) {
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;  // follow redirects ourselves, without the key
        WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    }
    std::wstring headers;
    if (!body.empty()) headers += L"Content-Type: " + widen(contentType) + L"\r\n";
    if (!apiKey.empty()) headers += L"X-API-Key: " + widen(apiKey) + L"\r\n";
    BOOL ok = req && WinHttpSendRequest(req, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                        (DWORD)-1L, (LPVOID)(body.empty() ? nullptr : body.data()),
                                        (DWORD)body.size(), (DWORD)body.size(), 0) &&
              WinHttpReceiveResponse(req, nullptr);
    if (!ok) {
        r.error = "cannot reach " + std::string(https ? "https://" : "http://") + host + ":" + std::to_string(port) +
                  " (WinHTTP error " + std::to_string(GetLastError()) + ")";
    } else {
        DWORD code = 0, sz = sizeof(code);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &code, &sz, WINHTTP_NO_HEADER_INDEX);
        r.status = (int)code;
        wchar_t loc[4096];
        DWORD locSz = sizeof(loc);
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, loc, &locSz,
                                WINHTTP_NO_HEADER_INDEX)) {
            int n = WideCharToMultiByte(CP_UTF8, 0, loc, -1, nullptr, 0, nullptr, nullptr);
            r.location.resize(n > 0 ? n - 1 : 0);
            if (n > 1) WideCharToMultiByte(CP_UTF8, 0, loc, -1, r.location.data(), n, nullptr, nullptr);
        }
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(req, &avail) && avail) {
            size_t old = r.body.size();
            r.body.resize(old + avail);
            DWORD read = 0;
            WinHttpReadData(req, r.body.data() + old, avail, &read);
            r.body.resize(old + read);
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return r;
}

Http Server::request(const char* method, const std::string& path, const std::string& body,
                     const std::string& contentType) const {
    return requestUrl(https, host, port, method, path, body, contentType, apiKey);
}

static std::string urlEncode(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else {
            char b[4];
            snprintf(b, sizeof(b), "%%%02X", c);
            o += b;
        }
    }
    return o;
}

static std::string errorText(const Http& h) {
    if (!h.error.empty()) return h.error;
    json::Value v;
    if (json::parse(h.body, v)) {
        std::string msg = v["error"]["message"].str(v["message"].str());
        std::string det = v["error"]["details"].str();
        std::string out = "HTTP " + std::to_string(h.status) + ": " + msg + (det.empty() ? "" : " - " + det);
        // node validation errors: {"node_errors": {"12": {"errors": [{"message", "details"}], "class_type"}}}
        for (auto& ne : v["node_errors"].o) {
            out += "\n  node " + ne.first + " (" + ne.second["class_type"].str() + "):";
            for (auto& e : ne.second["errors"].a) out += " " + e["message"].str() + " " + e["details"].str() + ";";
        }
        return out;
    }
    return "HTTP " + std::to_string(h.status) + ": " + h.body.substr(0, 800);
}

std::string upload(const Server& s, const std::wstring& path, const std::string& name, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "cannot read upload file";
        return "";
    }
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string boundary = "----AlphaGenSAM" + std::to_string(GetTickCount64());
    std::string body;
    auto field = [&](const char* n, const std::string& v) {
        body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + n + "\"\r\n\r\n" + v + "\r\n";
    };
    field("type", "input");
    field("overwrite", "true");
    body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"image\"; filename=\"" + name +
            "\"\r\nContent-Type: application/octet-stream\r\n\r\n";
    body += data;
    body += "\r\n--" + boundary + "--\r\n";
    Http h = s.request("POST", "/api/upload/image", body, "multipart/form-data; boundary=" + boundary);
    if (h.status != 200) {
        err = "upload failed: " + errorText(h);
        return "";
    }
    json::Value v;
    if (!json::parse(h.body, v)) {
        err = "upload: bad reply " + h.body.substr(0, 300);
        return "";
    }
    std::string sub = v["subfolder"].str();
    std::string n = v["name"].str(name);
    return sub.empty() ? n : sub + "/" + n;
}

// Cloud: outputs of a finished job, from /api/history_v2/{id} ({id: {outputs}} or {outputs}).
static bool readOutputs(const json::Value& entry, std::vector<std::pair<std::string, std::vector<OutputFile>>>& outputs) {
    const json::Value& outs = entry["outputs"];
    if (outs.type != json::Value::Object) return false;
    outputs.clear();
    for (auto& node : outs.o) {
        std::vector<OutputFile> files;
        for (auto& kind : node.second.o) {
            if (kind.second.type != json::Value::Array) continue;
            for (auto& f : kind.second.a)
                if (f.has("filename")) files.push_back({f["filename"].str(), f["subfolder"].str(), f["type"].str("output")});
        }
        outputs.emplace_back(node.first, std::move(files));
    }
    return true;
}

static std::string executionError(const json::Value& status) {
    std::string msg;
    for (auto& m : status["messages"].a)
        if (m[0].str() == "execution_error") {
            const json::Value& d = m[1];
            msg = d["node_type"].str() + " (node " + d["node_id"].str() + "): " + d["exception_message"].str();
        }
    return msg;
}

bool run(const Server& s, const std::string& nodesJson, const std::string& clientId, const Progress& progress,
         std::vector<std::pair<std::string, std::vector<OutputFile>>>& outputs, std::string& err, bool& cancelled) {
    cancelled = false;
    std::string body = "{\"client_id\":" + json::quote(clientId) + ",\"prompt\":" + nodesJson + "}";
    Http h = s.request("POST", "/api/prompt", body);
    if (h.status != 200) {
        err = std::string(s.cloud ? "Comfy Cloud" : "ComfyUI") + " rejected the job. " + errorText(h);
        if (h.status == 401) err += "\nCheck your Comfy Cloud API key.";
        if (h.status == 403 && h.body.find("free tier") != std::string::npos)
            err += "\nThe Comfy account this API key belongs to is on the free plan, and Comfy Cloud runs API jobs "
                   "only for paid subscriptions. Create the key while logged in to your subscribed account "
                   "(platform.comfy.org) and put it in Documents\\DUGco\\comfy_cloud_api_key.txt.";
        if (h.status == 402) err += "\nYour Comfy Cloud account is out of credits.";
        if (h.status == 429) err += "\nYour Comfy Cloud subscription isn't active (the API needs one).";
        return false;
    }
    json::Value v;
    if (!json::parse(h.body, v) || v["prompt_id"].str().empty()) {
        err = "ComfyUI did not return a prompt_id: " + h.body.substr(0, 300);
        return false;
    }
    std::string id = v["prompt_id"].str();
    auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(s.cloud ? 2000 : 700));
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        // ComfyUI has no per-job percentage over plain HTTP; creep so the bar shows life.
        double frac = 0.95 * (1.0 - std::exp(-secs / 90.0));
        if (progress && !progress(frac, "")) {
            s.request("POST", "/api/queue", "{\"delete\":[" + json::quote(id) + "]}");
            s.request("POST", "/api/interrupt", "{\"prompt_id\":" + json::quote(id) + "}");
            cancelled = true;
            err = "Cancelled.";
            return false;
        }
        if (s.cloud) {
            Http st = s.request("GET", "/api/job/" + id + "/status");
            if (!st.error.empty()) {
                err = st.error;
                return false;
            }
            json::Value sv;
            if (st.status != 200 || !json::parse(st.body, sv)) continue;
            std::string state = sv["status"].str();
            const bool done = state == "success" || state == "completed";
            const bool failed = state == "error" || state == "failed" || state == "non_retryable_error" ||
                                state == "lost" || state == "cancelled";
            if (!done && !failed) continue;
            Http hh = s.request("GET", "/api/history_v2/" + id);
            json::Value hist;
            json::parse(hh.body, hist);
            const json::Value& entry = hist[id].type == json::Value::Object ? hist[id] : hist;
            if (failed) {
                std::string msg = executionError(entry["status"]);
                err = "Comfy Cloud job " + state + ". " + (msg.empty() ? hh.body.substr(0, 800) : msg);
                return false;
            }
            if (!readOutputs(entry, outputs)) {
                err = "Comfy Cloud finished the job but returned no outputs: " + hh.body.substr(0, 400);
                return false;
            }
            return true;
        }
        Http hh = s.request("GET", "/api/history/" + id);
        if (!hh.error.empty()) {
            err = hh.error;
            return false;
        }
        json::Value hist;
        if (!json::parse(hh.body, hist)) continue;
        const json::Value& entry = hist[id];
        if (entry.type != json::Value::Object) continue;
        const json::Value& st = entry["status"];
        std::string status = st["status_str"].str();
        if (status.empty() && !st["completed"].b) continue;
        if (status != "success") {
            std::string msg = executionError(st);
            err = "ComfyUI job failed. " + (msg.empty() ? hh.body.substr(0, 800) : msg);
            return false;
        }
        readOutputs(entry, outputs);
        return true;
    }
}

bool download(const Server& s, const OutputFile& f, std::string& bytes, std::string& err) {
    std::string path = "/api/view?filename=" + urlEncode(f.filename) + "&subfolder=" + urlEncode(f.subfolder) +
                       "&type=" + urlEncode(f.type);
    Http h = s.request("GET", path);
    if (h.status == 301 || h.status == 302 || h.status == 307 || h.status == 308) {
        // signed storage URL: fetch it WITHOUT our API key
        Server t = Server::fromUrl(h.location);
        std::string loc = h.location;
        size_t p = loc.find("://");
        p = loc.find('/', p == std::string::npos ? 0 : p + 3);
        std::string rest = p == std::string::npos ? "/" : loc.substr(p);
        h = requestUrl(t.https, t.host, t.port, "GET", rest, "", "", "");
    }
    if (h.status != 200) {
        err = "download " + f.filename + " failed: " + errorText(h);
        return false;
    }
    bytes = std::move(h.body);
    return true;
}

bool ping(const Server& s, std::string& info, std::string& err) {
    if (s.cloud) {  // an authenticated call: proves the key too
        Http h = s.request("GET", "/api/queue");
        if (h.status != 200) {
            err = "Comfy Cloud: " + errorText(h) + (h.status == 401 ? "\nCheck your Comfy Cloud API key." : "");
            return false;
        }
        info = "Comfy Cloud";
        return true;
    }
    Http h = s.request("GET", "/api/system_stats");
    if (h.status != 200) {
        err = errorText(h);
        return false;
    }
    json::Value v;
    if (json::parse(h.body, v)) {
        const json::Value& dev = v["devices"][0];
        info = v["system"]["comfyui_version"].str() + " on " + dev["name"].str();
    }
    return true;
}

bool modelOptions(const Server& s, const std::string& node, const std::string& input, std::vector<std::string>& out,
                  std::string& err) {
    // Comfy Cloud serves only the full catalogue (/api/object_info): fetch it once per
    // process and keep it. Local ComfyUI answers per node.
    static std::mutex lock;
    static std::map<std::string, std::shared_ptr<json::Value>> catalogues;
    std::shared_ptr<json::Value> v;
    if (s.cloud) {
        std::lock_guard<std::mutex> g(lock);
        auto& c = catalogues[s.label()];
        if (!c) {
            Http h = s.request("GET", "/api/object_info");
            auto parsed = std::make_shared<json::Value>();
            if (h.status != 200 || !json::parse(h.body, *parsed)) {
                err = "could not read the node catalogue from Comfy Cloud: " + errorText(h);
                return false;
            }
            c = parsed;
        }
        v = c;
    } else {
        Http h = s.request("GET", "/api/object_info/" + node);
        v = std::make_shared<json::Value>();
        if (h.status != 200 || !json::parse(h.body, *v)) {
            err = "could not read " + node + " from the server: " + errorText(h);
            return false;
        }
    }
    const json::Value& spec = (*v)[node]["input"]["required"][input];
    // old style: [[options...], {...}]; new style: ["COMBO", {"options": [...]}]
    const json::Value& list = spec[0].type == json::Value::Array ? spec[0] : spec[1]["options"];
    out.clear();
    for (auto& o : list.a) out.push_back(o.str());
    return true;
}

// Key file, first found: Documents\DUGco\comfy_cloud_api_key.txt, then %APPDATA%\DUGco\...
static std::string readKeyFile() {
    std::string line;
    for (const wchar_t* var : {L"USERPROFILE", L"APPDATA"}) {
        wchar_t* dir = nullptr;
        size_t n = 0;
        if (_wdupenv_s(&dir, &n, var) || !dir) continue;
        std::wstring p = std::wstring(dir) + (wcscmp(var, L"USERPROFILE") == 0 ? L"\\Documents" : L"") +
                         L"\\DUGco\\comfy_cloud_api_key.txt";
        free(dir);
        std::ifstream f(p);
        if (f && std::getline(f, line) && !line.empty()) break;
        line.clear();
    }
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\n')) line.pop_back();
    while (!line.empty() && line.front() == ' ') line.erase(line.begin());
    return line;
}

static std::wstring keyFilePath() {
    wchar_t* dir = nullptr;
    size_t n = 0;
    if (_wdupenv_s(&dir, &n, L"USERPROFILE") || !dir) return L"";
    std::wstring p = std::wstring(dir) + L"\\Documents\\DUGco\\comfy_cloud_api_key.txt";
    free(dir);
    return p;
}

bool Server::saveCloudKey(const std::string& keyIn, std::string& err) {
    std::string key = keyIn;
    while (!key.empty() && isspace((unsigned char)key.back())) key.pop_back();
    while (!key.empty() && isspace((unsigned char)key.front())) key.erase(key.begin());
    std::wstring p = keyFilePath();
    if (p.empty()) {
        err = "could not find your Documents folder";
        return false;
    }
    std::wstring dir = p.substr(0, p.find_last_of(L'\\'));
    CreateDirectoryW(dir.substr(0, dir.find_last_of(L'\\')).c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) {
        err = "could not write the key file";
        return false;
    }
    f << key;
    return true;
}

std::string Server::cloudKeyHint() {
    std::string err;
    Server s = cloudServer(err);
    if (s.apiKey.empty()) return "";
    return "..." + s.apiKey.substr(s.apiKey.size() > 4 ? s.apiKey.size() - 4 : 0);
}

Server Server::cloudServer(std::string& err) {
    Server s = fromUrl("https://cloud.comfy.org");
    s.cloud = true;
    char* env = nullptr;
    size_t n = 0;
    if (!_dupenv_s(&env, &n, "COMFY_CLOUD_API_KEY") && env) {
        s.apiKey = env;
        free(env);
    }
    if (s.apiKey.empty()) s.apiKey = readKeyFile();
    if (s.apiKey.empty())
        err = "No Comfy Cloud API key. Create one at platform.comfy.org, then either set the COMFY_CLOUD_API_KEY "
              "environment variable or save it as the only line of Documents\\DUGco\\comfy_cloud_api_key.txt, and "
              "restart Nuke.";
    return s;
}

}  // namespace comfy

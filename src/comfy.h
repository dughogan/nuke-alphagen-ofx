// ComfyUI REST client: upload inputs, queue API-format prompts, wait, download outputs.
// Local ComfyUI and Comfy Cloud (cloud.comfy.org) share the /api routes; Cloud adds an
// X-API-Key header, reports job state at /api/job/{id}/status and outputs at
// /api/history_v2/{id}, and serves files through signed-URL redirects.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "json.h"

namespace comfy {

struct Http {
    int status = 0;
    std::string body;
    std::string location;  // Location header on redirects
    std::string error;     // transport error (no response)
};

struct Server {
    bool https = false;
    std::string host = "127.0.0.1";
    int port = 8000;
    std::string apiKey;  // Comfy Cloud only (never sent to redirect targets)
    bool cloud = false;  // Comfy Cloud job/status routes

    static Server fromUrl(const std::string& url);
    // Comfy Cloud with the user's API key: COMFY_CLOUD_API_KEY, else the first line of
    // %APPDATA%\DUGco\comfy_cloud_api_key.txt. The key never goes into Nuke scripts.
    static Server cloudServer(std::string& err);
    // Save the key to Documents\DUGco\comfy_cloud_api_key.txt (creating the folder).
    static bool saveCloudKey(const std::string& key, std::string& err);
    // "" when no key is set, else a hint like "...a1b2" (never the key itself).
    static std::string cloudKeyHint();
    std::string label() const;

    Http request(const char* method, const std::string& path, const std::string& body = "",
                 const std::string& contentType = "application/json") const;
};

struct OutputFile {
    std::string filename, subfolder, type;
};

// Progress callback: value 0-1, text. Return false to cancel.
using Progress = std::function<bool(double, const std::string&)>;

// Upload a local file into ComfyUI's input folder. Returns the name to reference in
// the prompt (ComfyUI may rename it), or "" and sets err.
std::string upload(const Server& s, const std::wstring& path, const std::string& name, std::string& err);

// Queue a prompt (the JSON object of nodes) and block until it finishes.
// On success fills outputs (node id -> files, in the order ComfyUI saved them).
bool run(const Server& s, const std::string& nodesJson, const std::string& clientId, const Progress& progress,
         std::vector<std::pair<std::string, std::vector<OutputFile>>>& outputs, std::string& err, bool& cancelled);

// Download an output file's bytes.
bool download(const Server& s, const OutputFile& f, std::string& bytes, std::string& err);

// Quick reachability check (GET /api/system_stats; Cloud: an authenticated /api/queue).
bool ping(const Server& s, std::string& info, std::string& err);

// The choices a loader node offers for one of its inputs (GET /api/object_info/<node>).
bool modelOptions(const Server& s, const std::string& node, const std::string& input, std::vector<std::string>& out,
                  std::string& err);

}  // namespace comfy

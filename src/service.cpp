#include <winsock2.h>
#include <ws2tcpip.h>
#include "worker.h"
#include <winsqlite/winsqlite3.h>
#include <wincred.h>
#include <userenv.h>
#include <bcrypt.h>
#include <winhttp.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <thread>

namespace {
using cs2::Json;
using cs2::service::Handle;
using cs2::service::require;
using cs2::service::wide;
namespace fs = std::filesystem;
constexpr std::uint64_t GiB = 1024ULL * 1024 * 1024;
long long now() { return static_cast<long long>(time(nullptr)); }
std::string random_token() {
    std::array<unsigned char, 32> bytes{};
    require(BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0, "Random generation failed");
    const char* hex = "0123456789abcdef";
    std::string result;
    for (auto byte : bytes) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}
bool identifier(const std::string& value, size_t length = 32) {
    return value.size() == length && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}
std::string text_file(const fs::path& path, size_t limit) {
    const auto attributes = GetFileAttributesW(path.c_str());
    require(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        fs::is_regular_file(path) && fs::file_size(path) <= limit, "Artifact unavailable or too large");
    std::ifstream input(path, std::ios::binary);
    std::string result((std::istreambuf_iterator<char>(input)), {});
    require(!input.bad() && result.size() <= limit, "Artifact read failed");
    return result;
}
Json small_json(const fs::path& path, size_t limit = 1024 * 1024) {
    return Json::parse(text_file(path, limit));
}
void atomic_json(const fs::path& path, const Json& value) {
    const auto pending = path.wstring() + L".pending";
    cs2::write_text(pending, value.dump());
    require(MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0,
        "State persistence failed");
}
bool elevated() {
    Handle token;
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value) != 0, "Cannot inspect service token");
    TOKEN_ELEVATION value{}; DWORD size = 0;
    require(GetTokenInformation(token.value, TokenElevation, &value, sizeof(value), &size) != 0,
        "Cannot inspect elevation");
    return value.TokenIsElevated != 0;
}
std::wstring object_name(const fs::path& root, const std::wstring& suffix) {
    return L"Local\\CSDA." + wide(cs2::sha256_text(root.u8string()).substr(0, 24)) + suffix;
}
std::wstring credential_name(const fs::path& root) { return object_name(root, L".BetaAccess"); }
std::string beta_secret(const fs::path& root, bool create) {
    PCREDENTIALW credential = nullptr;
    if (CredReadW(credential_name(root).c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
        std::string result(reinterpret_cast<char*>(credential->CredentialBlob), credential->CredentialBlobSize);
        CredFree(credential);
        require(identifier(result, 64), "Beta credential has invalid format");
        return result;
    }
    require(GetLastError() == ERROR_NOT_FOUND && create, "Beta credential unavailable; run setup as the service user");
    auto result = random_token();
    CREDENTIALW value{};
    auto target = credential_name(root);
    value.Type = CRED_TYPE_GENERIC;
    value.TargetName = target.data();
    value.CredentialBlobSize = static_cast<DWORD>(result.size());
    value.CredentialBlob = reinterpret_cast<LPBYTE>(result.data());
    value.Persist = CRED_PERSIST_LOCAL_MACHINE;
    require(CredWriteW(&value, 0) != 0, "Cannot save beta credential");
    return result;
}
struct Database {
    sqlite3* db = nullptr;
    explicit Database(const fs::path& path) {
        require(sqlite3_open16(path.c_str(), &db) == SQLITE_OK, "Cannot open job ledger");
        execute("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;"
            "CREATE TABLE IF NOT EXISTS jobs(id TEXT PRIMARY KEY,data TEXT NOT NULL);"
            "CREATE TABLE IF NOT EXISTS sessions(id TEXT PRIMARY KEY,data TEXT NOT NULL);");
    }
    ~Database() { if (db) sqlite3_close(db); }
    void execute(const char* sql) {
        char* error = nullptr;
        const auto result = sqlite3_exec(db, sql, nullptr, nullptr, &error);
        if (error) sqlite3_free(error);
        require(result == SQLITE_OK, "Job ledger operation failed");
    }
    void put(const char* table, const std::string& id, const Json& data) {
        sqlite3_stmt* statement = nullptr;
        const auto sql = std::string("INSERT OR REPLACE INTO ") + table + "(id,data) VALUES(?,?)";
        require(sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK, "Ledger preparation failed");
        const auto serialized = data.dump();
        const bool ok = sqlite3_bind_text(statement, 1, id.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
            sqlite3_bind_text(statement, 2, serialized.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
            sqlite3_step(statement) == SQLITE_DONE;
        sqlite3_finalize(statement);
        require(ok, "Ledger commit failed");
    }
    std::map<std::string, Json> load(const char* table) {
        std::map<std::string, Json> result;
        sqlite3_stmt* statement = nullptr;
        const auto sql = std::string("SELECT id,data FROM ") + table;
        require(sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK, "Cannot read ledger");
        int step = 0;
        while ((step = sqlite3_step(statement)) == SQLITE_ROW) {
            const auto* id = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
            const auto* data = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
            require(id && data && sqlite3_column_bytes(statement, 1) <= 1024 * 1024 &&
                result.size() < 1024, "Ledger exceeds bounded startup size");
            result[id] = Json::parse(data);
        }
        sqlite3_finalize(statement);
        require(step == SQLITE_DONE, "Ledger read failed");
        return result;
    }
    void erase(const char* table, const std::string& id) {
        sqlite3_stmt* statement = nullptr;
        const auto sql = std::string("DELETE FROM ") + table + " WHERE id=?";
        require(sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK, "Ledger delete preparation failed");
        const bool ok = sqlite3_bind_text(statement, 1, id.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
            sqlite3_step(statement) == SQLITE_DONE;
        sqlite3_finalize(statement);
        require(ok, "Ledger delete failed");
    }
};
struct ApiError : std::runtime_error {
    int status;
    ApiError(int code, const char* message) : std::runtime_error(message), status(code) {}
};
#ifdef CSDA_SERVICE_TEST
std::function<void(bool)> transport_observer;
#endif
struct Request {
    SOCKET socket = INVALID_SOCKET;
    std::string method, path, pending;
    std::map<std::string, std::string> headers;
    std::uint64_t length = 0, received = 0, started = GetTickCount64();
    bool response_started = false;
    int receive(char* buffer, int count) {
#ifdef CSDA_SERVICE_TEST
        if (transport_observer) transport_observer(false);
#endif
        return recv(socket, buffer, count, 0);
    }
    explicit Request(SOCKET value) : socket(value) {
        DWORD timeout = 5000;
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
        setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
        std::string header;
        char buffer[8192];
        size_t end = std::string::npos;
        while ((end = header.find("\r\n\r\n")) == std::string::npos) {
            const auto count = receive(buffer, sizeof(buffer));
            require(count > 0 && header.size() + static_cast<size_t>(count) <= 16384 &&
                GetTickCount64() - started <= 10000, "Invalid request headers");
            header.append(buffer, static_cast<size_t>(count));
        }
        pending = header.substr(end + 4);
        header.resize(end);
        std::istringstream lines(header);
        std::string line, version, extra;
        std::getline(lines, line);
        std::istringstream first(line);
        require(static_cast<bool>(first >> method >> path >> version) && !(first >> extra) &&
            version == "HTTP/1.1" && path.size() <= 256 && path.find_first_of("?#%\\") == std::string::npos,
            "Invalid request line");
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto colon = line.find(':');
            require(colon != std::string::npos && colon > 0 && line[0] != ' ' && line[0] != '\t',
                "Invalid request header");
            auto name = line.substr(0, colon);
            require(name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") == std::string::npos,
                "Invalid header name");
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
            auto value_text = line.substr(colon + 1);
            const auto begin = value_text.find_first_not_of(" \t");
            value_text = begin == std::string::npos ? "" : value_text.substr(begin);
            require(!headers.count(name) && value_text.find_first_of("\r\n") == std::string::npos,
                "Duplicate/invalid header");
            headers[name] = value_text;
        }
        require(!headers.count("transfer-encoding") && !headers.count("expect"), "Unsupported body framing");
        if (headers.count("content-length")) {
            const auto& text = headers.at("content-length");
            require(!text.empty() && text.size() < 12 && text.find_first_not_of("0123456789") == std::string::npos,
                "Invalid content length");
            length = std::stoull(text);
        }
        require(pending.size() <= length, "Unexpected pipelined bytes");
    }
    ~Request() { if (socket != INVALID_SOCKET) closesocket(socket); }
    std::string header(const std::string& name) const {
        const auto found = headers.find(name);
        return found == headers.end() ? "" : found->second;
    }
    void stream(const std::function<void(const char*, size_t)>& write, std::uint64_t limit,
                const std::atomic<bool>* cancel = nullptr) {
        require(length <= limit, "Upload exceeds byte limit");
        if (!pending.empty()) { write(pending.data(), pending.size()); received += pending.size(); pending.clear(); }
        std::array<char, 65536> buffer{};
        while (received < length) {
            require(GetTickCount64() - started <= 180000 && (!cancel || !cancel->load()), "Upload cancelled or timed out");
            const auto count = receive(buffer.data(), static_cast<int>(
                std::min<std::uint64_t>(buffer.size(), length - received)));
            require(count > 0, "Upload interrupted");
            write(buffer.data(), static_cast<size_t>(count));
            received += static_cast<size_t>(count);
        }
    }
    Json json() {
        if (length > 4096) throw ApiError(413, "Request body exceeds byte limit");
        std::string body;
        stream([&](const char* data, size_t count) { body.append(data, count); }, 4096);
        try { return Json::parse(body); }
        catch (const Json::parse_error&) { throw ApiError(400, "Invalid JSON request"); }
    }
    size_t send(int status, const std::string& body, const std::string& type = "application/json",
              const std::string& extra_headers = "") {
        require(!response_started, "Response already started");
        std::string response = "HTTP/1.1 " + std::to_string(status) + " Response\r\nConnection: close\r\n"
            "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n"
            "Content-Security-Policy: default-src 'none'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
            "connect-src 'self'; img-src 'none'; base-uri 'none'; frame-ancestors 'none'; form-action 'self'\r\n"
            "Content-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + extra_headers + "\r\n";
        if (method != "HEAD") response += body;
        size_t offset = 0;
        while (offset < response.size()) {
#ifdef CSDA_SERVICE_TEST
            if (transport_observer) transport_observer(true);
#endif
            const auto sent = ::send(socket, response.data() + offset,
                static_cast<int>(std::min<size_t>(65536, response.size() - offset)), 0);
            require(sent > 0, "Response interrupted");
            response_started = true;
            offset += static_cast<size_t>(sent);
        }
        return method == "HEAD" ? 0 : body.size();
    }
};
std::string session_cookie(const std::string& cookie) {
    std::optional<std::string> token;
    for (size_t begin = 0; begin < cookie.size();) {
        auto end = cookie.find(';', begin);
        if (end == std::string::npos) end = cookie.size();
        auto field = cookie.substr(begin, end - begin);
        const auto first = field.find_first_not_of(" \t");
        if (first != std::string::npos) {
            field = field.substr(first, field.find_last_not_of(" \t") - first + 1);
            const auto equals = field.find('=');
            if (field.substr(0, equals) == "__Host-csda") {
                if (token || equals == std::string::npos) throw ApiError(401, "Ambiguous session cookie");
                token = field.substr(equals + 1);
            }
        }
        begin = end + 1;
    }
    if (!token) throw ApiError(401, "Beta access required");
    if (!identifier(*token, 64)) throw ApiError(401, "Invalid session");
    return *token;
}
struct Job {
    Json data;
    std::atomic<bool> cancel{false};
};
struct Server {
    fs::path root, local;
    Json config;
    Database db;
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<Job>> jobs;
    std::map<std::string, Json> sessions;
    std::map<std::string, std::vector<long long>> logins;
    std::atomic<bool> stopping{false};
    std::string secret;
    std::function<long long()> clock;
    bool ready = false, cleanup_safe = true;
    explicit Server(const fs::path& directory) : Server(directory,
        small_json(directory / "local" / "service.json", 16384), beta_secret(directory, false), now) {}
    Server(const fs::path& directory, Json settings, std::string invite,
           std::function<long long()> time_source) : root(directory), local(root / "local"),
        config(std::move(settings)), db(local / "jobs.sqlite"), secret(std::move(invite)),
        clock(std::move(time_source)) {
        require(config.at("origin").is_string() && config.at("port") == 8091 &&
            config.at("upload_max_bytes").get<std::uint64_t>() <= GiB, "Invalid service configuration");
        sessions = db.load("sessions");
        for (auto& pair : db.load("jobs")) {
            require(identifier(pair.first), "Invalid persisted job identifier");
            auto job = std::make_shared<Job>(); job->data = pair.second;
            jobs[pair.first] = job;
            if (job->data.at("state") != "complete") {
                job->data["state"] = "cleanup_pending";
                job->data["error"] = "Interrupted by service restart";
                job->data["expires"] = time_now();
                db.put("jobs", pair.first, job->data);
            }
        }
    }
    long long time_now() const { return clock(); }
    void admit_login(const std::string& ip) {
        const auto instant = time_now();
        for (auto iterator = logins.begin(); iterator != logins.end();) {
            auto& attempts = iterator->second;
            attempts.erase(std::remove_if(attempts.begin(), attempts.end(),
                [instant](long long time) { return instant - time > 60; }), attempts.end());
            if (attempts.empty()) iterator = logins.erase(iterator);
            else ++iterator;
        }
        auto found = logins.find(ip);
        if (found == logins.end()) {
            if (logins.size() >= 512) throw ApiError(429, "Login admission busy");
            found = logins.emplace(ip, std::vector<long long>{}).first;
        }
        if (found->second.size() >= 6) throw ApiError(429, "Wait a minute before trying access again");
        found->second.push_back(instant);
    }
    fs::path workspace(const std::string& id) {
        require(identifier(id), "Invalid job path");
        return local / "jobs" / id;
    }
    void save(const std::shared_ptr<Job>& job) { db.put("jobs", job->data.at("id"), job->data); }
    void log(const char* component, const std::exception& error) {
        std::ofstream output(local / "errors.log", std::ios::app);
        output << time_now() << " " << component << ": " << std::string(error.what()).substr(0, 240) << "\n";
        require(static_cast<bool>(output), "Cannot persist service error");
    }
    std::string owner(Request& request, bool mutate) {
        const auto token = session_cookie(request.header("cookie"));
        const auto id = cs2::sha256_text(token);
        if (!sessions.count(id) || sessions.at(id).at("expires").get<long long>() <= time_now())
            throw ApiError(401, "Session expired");
        if (mutate && request.header("x-csda-csrf") != sessions.at(id).at("csrf"))
            throw ApiError(403, "Invalid request token");
        return id;
    }
    std::shared_ptr<Job> owned(const std::string& id, const std::string& user) {
        const auto found = jobs.find(id);
        if (!identifier(id) || found == jobs.end() || found->second->data.at("owner") != user)
            throw ApiError(404, "Job not found");
        return found->second;
    }
    Json public_job(const std::shared_ptr<Job>& job) {
        auto result = job->data;
        result.erase("owner"); result.erase("selected_name");
        result["server_time"] = time_now();
        return result;
    }
    void handle(Request& request) {
        const bool read = request.method == "GET" || request.method == "HEAD";
        if (read && request.path == "/healthz") {
            request.send(ready ? 200 : 503, Json{{"ready", ready}, {"role", "csda-service-v1"},
                {"pid", GetCurrentProcessId()}}.dump()); return;
        }
        if (!ready) throw ApiError(503, "Service starting");
        if (read && (request.path == "/" || request.path == "/app.js")) {
            request.send(200, text_file(root / "web" / (request.path == "/" ? "index.html" : "app.js"), 128 * 1024),
                request.path == "/" ? "text/html; charset=utf-8" : "text/javascript; charset=utf-8"); return;
        }
        if (request.path.rfind("/api/", 0) != 0) throw ApiError(404, "Not found");
        if (!read && request.header("origin") != config.at("origin")) throw ApiError(403, "Origin denied");
        if (request.path == "/api/session" && request.method == "POST") {
            auto ip = request.header("x-csda-client-ip");
            if (ip.empty() || ip.size() > 64) ip = "loopback";
            {
                std::lock_guard<std::mutex> lock(mutex);
                admit_login(ip);
            }
            auto body = request.json();
            if (!body.is_object() || body.size() != 1 || !body.contains("invite") || !body["invite"].is_string() ||
                cs2::sha256_text(body["invite"].get<std::string>()) != cs2::sha256_text(secret))
                throw ApiError(403, "Invalid beta access");
            std::unique_lock<std::mutex> lock(mutex);
            if (sessions.size() >= 128) throw ApiError(503, "Session capacity reached");
            const auto token = random_token(), id = cs2::sha256_text(token);
            sessions[id] = {{"csrf", random_token()}, {"expires", time_now() + 7200}};
            db.put("sessions", id, sessions[id]);
            const auto response = Json{{"csrf", sessions[id]["csrf"]}, {"upload_max_bytes", config["upload_max_bytes"]}}.dump();
            lock.unlock();
            request.send(200, response,
                "application/json", "Set-Cookie: __Host-csda=" + token +
                "; Secure; HttpOnly; SameSite=Strict; Path=/; Max-Age=7200\r\n"); return;
        }
        std::unique_lock<std::mutex> lock(mutex);
        const auto user = owner(request, !read);
        if (request.path == "/api/session" && read) {
            const auto response = Json{{"csrf", sessions.at(user).at("csrf")},
                {"upload_max_bytes", config["upload_max_bytes"]}}.dump();
            lock.unlock();
            request.send(200, response); return;
        }
        if (request.path == "/api/jobs" && request.method == "POST") {
            size_t pending = 0;
            for (const auto& pair : jobs) {
                const auto state = pair.second->data.at("state").get<std::string>();
                if (state != "complete" && state != "failed" && state != "cancelled") {
                    if (pair.second->data.at("owner") == user) throw ApiError(409, "Finish or cancel your existing job");
                    ++pending;
                }
            }
            if (!cleanup_safe || pending >= 4 || jobs.size() >= 32) throw ApiError(503, "Queue/storage admission full");
            if (cs2::service::available_memory() < 8 * GiB || fs::space(local).available < 8 * GiB)
                throw ApiError(503, "Host memory or disk reserve unavailable");
            auto job = std::make_shared<Job>();
            const auto id = random_token().substr(0, 32);
            require(fs::create_directory(workspace(id)), "Cannot create exclusive job");
            job->data = {{"id", id}, {"owner", user}, {"state", "reserved"}, {"created", time_now()},
                {"expires", time_now() + 180}, {"upload_bytes", 0}, {"phases", Json::array()}};
            jobs[id] = job; save(job);
            const auto response = public_job(job).dump();
            lock.unlock();
            request.send(201, response); return;
        }
        const auto prefix = std::string("/api/jobs/");
        if (request.path.rfind(prefix, 0) != 0) throw ApiError(404, "Not found");
        const auto rest = request.path.substr(prefix.size());
        const auto slash = rest.find('/');
        const auto id = rest.substr(0, slash);
        const auto action = slash == std::string::npos ? "" : rest.substr(slash + 1);
        const auto job = owned(id, user);
        if (read && action.empty()) {
            const auto response = public_job(job).dump();
            lock.unlock();
            request.send(200, response); return;
        }
        if (request.method == "DELETE" && action.empty()) {
            job->cancel = true;
            job->data["expires"] = time_now();
            save(job);
            lock.unlock();
            request.send(202, Json{{"state", "cancelling"}}.dump()); return;
        }
        if (action == "demo" && request.method == "PUT") {
            if (job->cancel || job->data.at("state") != "reserved" ||
                job->data.at("expires").get<long long>() <= time_now())
                throw ApiError(409, "Upload reservation is not active");
            if (request.length < 16 || request.length > config.at("upload_max_bytes").get<std::uint64_t>())
                throw ApiError(413, "Replay exceeds the upload limit or is empty");
            job->data["state"] = "uploading"; job->data["expires"] = time_now() + 180; save(job);
            lock.unlock();
            try {
                const auto path = workspace(id) / "upload.pending";
                std::ofstream output(path, std::ios::binary);
                require(static_cast<bool>(output), "Cannot open upload");
                request.stream([&](const char* data, size_t count) {
                    output.write(data, static_cast<std::streamsize>(count));
                    require(static_cast<bool>(output), "Upload write failed");
                }, config.at("upload_max_bytes").get<std::uint64_t>(), &job->cancel);
                output.close();
                std::ifstream input(path, std::ios::binary);
                char signature[8]{}; input.read(signature, sizeof(signature));
                require(input.gcount() == 8 && std::string(signature, 8) == std::string("PBDEMS2\0", 8),
                    "Not a supported CS2 replay header");
                input.close();
                fs::rename(path, workspace(id) / "upload.dem");
                const auto hash = cs2::sha256_file(workspace(id) / "upload.dem");
                lock.lock();
                job->data["state"] = job->cancel ? "cleanup_pending" : "queued";
                job->data["upload_bytes"] = request.received;
                job->data["upload_ms"] = GetTickCount64() - request.started;
                job->data["source_sha256"] = hash;
                job->data["expires"] = job->cancel ? time_now() : time_now() + 900;
                save(job);
            } catch (const std::exception& error) {
                if (!lock.owns_lock()) lock.lock();
                log("upload", error);
                job->data["state"] = "cleanup_pending"; job->data["expires"] = time_now();
                job->data["error"] = "Upload failed: interrupted, unsupported replay, or resource limit";
                save(job);
                throw ApiError(422, "Upload failed; no analysis was published");
            }
            const auto response = public_job(job).dump();
            lock.unlock();
            request.send(200, response);
            return;
        }
        if (action == "analysis" && request.method == "POST") {
            lock.unlock();
            auto body = request.json();
            lock.lock();
            if (owner(request, true) != user || owned(id, user) != job)
                throw ApiError(404, "Job not found");
            if (job->cancel || job->data.at("expires").get<long long>() <= time_now())
                throw ApiError(409, "Player selection expired or cancelled");
            if (job->data.at("state") != "select") throw ApiError(409, "Job is not awaiting player selection");
            if (!body.is_object() || body.size() != 1 || !body.contains("steam_id") || !body["steam_id"].is_string())
                throw ApiError(400, "Select a player from this job");
            const auto id_text = body["steam_id"].get<std::string>();
            bool found = false;
            for (const auto& player : job->data.at("players")) if (player.at("steam_id") == id_text) {
                found = true; job->data["selected_name"] = player.at("name"); break;
            }
            if (!found) throw ApiError(400, "Player does not belong to this replay");
            job->data["steam_id"] = id_text; job->data["state"] = "analysis_queued";
            job->data["expires"] = time_now() + 3600;
            save(job);
            const auto response = public_job(job).dump();
            lock.unlock();
            request.send(202, response); return;
        }
        if (read && (action == "result" || action == "report" || action == "receipt")) {
            if (job->cancel || job->data.at("state") != "complete" ||
                job->data.at("expires").get<long long>() <= time_now())
                throw ApiError(409, "Completed result unavailable");
            const auto snapshot = public_job(job);
            lock.unlock();
            if (action == "receipt") {
                request.send(200, snapshot.dump(2), "application/json",
                    "Content-Disposition: attachment; filename=\"CSDA-receipt.json\"\r\n");
            } else {
                const auto filename = action == "result" ? "result.json" : "report.html";
                const auto path = local / "results" / id / filename;
                require(cs2::sha256_file(path) == snapshot.at("artifacts").at(filename).at("sha256"),
                    "Published artifact hash mismatch");
                const auto content = text_file(path, action == "result" ? 1024 * 1024 : 16 * 1024 * 1024);
                const auto sent = request.send(200, content, action == "result" ? "application/json" : "application/octet-stream",
                    action == "report" ? "Content-Disposition: attachment; filename=\"CSDA-report.html\"\r\n" : "");
                if (sent) {
                    lock.lock();
                    const auto found = jobs.find(id);
                    if (found != jobs.end() && found->second == job && !job->cancel) {
                        job->data["served_payload_bytes"] = job->data.value("served_payload_bytes", 0ULL) + sent;
                        save(job);
                    }
                }
            }
            return;
        }
        throw ApiError(404, "Not found");
    }
    void cleanup(const std::string& id, const std::shared_ptr<Job>& job) {
        auto work = workspace(id);
        if (fs::exists(work)) cs2::service::validate_tree(work);
        if (fs::exists(work / "worker-profile.txt")) {
            const auto name = text_file(work / "worker-profile.txt", 128);
            require(name.rfind("CSDA.Job.", 0) == 0 && name.size() == 25, "Invalid retained worker profile");
            const auto error = DeleteAppContainerProfile(wide(name).c_str());
            require(SUCCEEDED(error) || error == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),
                "Retained profile cleanup failed");
        }
        if (fs::exists(work)) {
            require(!(GetFileAttributesW(work.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT), "Job root changed");
            fs::remove_all(work);
        }
        (void)job;
    }
    void phase(const std::shared_ptr<Job>& job, const std::string& id, const fs::path& executable,
               const std::vector<std::wstring>& arguments, const std::string& name) {
        const auto available = cs2::service::available_memory();
        require(available > 5 * GiB, "Host memory reserve unavailable");
        const auto budget = std::min<std::uint64_t>(16 * GiB, available - 4 * GiB);
        require(budget >= GiB, "Host memory reserve unavailable");
        auto metric = cs2::service::run_isolated(executable, arguments, workspace(id),
            job->cancel, budget, 300, name != "decode");
        metric["stage"] = name;
        {
            std::lock_guard<std::mutex> lock(mutex);
            job->data["phases"].push_back(metric); save(job);
        }
        cs2::service::validate_tree(workspace(id));
        if (metric.at("exit_code") != 0 || metric.contains("error")) {
            const auto diagnostics = text_file(workspace(id) / "worker.log", 32768);
            throw std::runtime_error(name + " failed: " + diagnostics.substr(0, 180));
        }
    }
    void execute(const std::shared_ptr<Job>& job, bool decode) {
        Json snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex);
            snapshot = job->data;
        }
        const std::string id = snapshot.at("id");
        const auto work = workspace(id);
        if (decode) {
            const auto decoder = root / "tools" / "csda" / "csda.exe";
            require(cs2::sha256_file(decoder) == small_json(decoder.parent_path() / "installed.json").at("exe_sha256"),
                "Installed decoder hash changed");
            cs2::write_text(work / "match.json", "");
            phase(job, id, decoder, {L"-demo-path=" + (work / "upload.dem").wstring(),
                L"-output=" + (work / "match.json").wstring(), L"-format=json", L"-minify"}, "decode");
            require(cs2::sha256_file(work / "upload.dem") == snapshot.at("source_sha256"),
                "Sealed replay changed");
            const auto decoded_hash = cs2::sha256_file(work / "match.json");
            phase(job, id, cs2::service::image_path(), {L"--roster", work.wstring()}, "roster");
            const auto roster = small_json(work / "roster.json", 32768);
            require(roster.is_array() && !roster.empty() && roster.size() <= 64, "Decoded roster invalid");
            std::lock_guard<std::mutex> lock(mutex);
            require(!job->cancel && job->data.at("expires").get<long long>() > time_now(),
                "Job cancelled or expired before selection");
            job->data["players"] = roster; job->data["state"] = "select";
            job->data["decoded_sha256"] = decoded_hash;
            job->data["decoder_version"] = "v1.11.0";
            job->data["expires"] = time_now() + 900; save(job);
        } else {
            const auto steam = snapshot.at("steam_id").get<std::string>();
            require(cs2::sha256_file(work / "match.json") == snapshot.at("decoded_sha256"),
                "Sealed decoder output changed");
            phase(job, id, cs2::service::image_path(), {L"--analyze", work.wstring(), wide(steam)}, "analysis");
            const auto publication = small_json(work / "publish" / "publication.json", 4096);
            require(publication.is_object() && publication.size() == 2 &&
                publication.contains("result.json") && publication.contains("report.html"), "Publication manifest invalid");
            const auto destination = local / "results" / id;
            cs2::service::private_directory(destination);
            for (const auto* filename : {"result.json", "report.html"}) {
                const auto file = work / "publish" / filename;
                require(fs::file_size(file) == publication.at(filename).at("bytes") &&
                    cs2::sha256_file(file) == publication.at(filename).at("sha256"), "Incomplete publication");
                text_file(file, std::string(filename) == "result.json" ? 1024 * 1024 : 16 * 1024 * 1024);
                fs::copy_file(file, destination / filename, fs::copy_options::none);
            }
            // Remove raw data before exposing the completed result.
            cleanup(id, job);
            std::lock_guard<std::mutex> lock(mutex);
            require(!job->cancel && job->data.at("expires").get<long long>() > time_now(),
                "Job cancelled or expired before publication");
            job->data["artifacts"] = publication; job->data["state"] = "complete";
            job->data["expires"] = time_now() + 3600; save(job);
        }
    }
    std::pair<std::shared_ptr<Job>, bool> take_work() {
        std::shared_ptr<Job> selected; bool decode = false;
        std::lock_guard<std::mutex> lock(mutex);
        for (auto iterator = sessions.begin(); iterator != sessions.end();) {
            if (iterator->second.at("expires").get<long long>() <= time_now()) {
                db.erase("sessions", iterator->first); iterator = sessions.erase(iterator);
            } else ++iterator;
        }
        for (auto iterator = jobs.begin(); iterator != jobs.end();) {
            const auto id = iterator->first;
            auto job = iterator->second;
            const auto state = job->data.at("state").get<std::string>();
            if (state != "uploading" && (job->cancel || job->data.at("expires").get<long long>() <= time_now() ||
                state == "cleanup_pending")) {
                try {
                    cleanup(id, job);
                    const auto result = local / "results" / id;
                    if (fs::exists(result)) fs::remove_all(result);
                    if (state == "cleanup_pending" && job->data.contains("error") && !job->cancel) {
                        job->data["state"] = "failed"; job->data["expires"] = time_now() + 120; save(job);
                        ++iterator; continue;
                    }
                    db.erase("jobs", id); iterator = jobs.erase(iterator); continue;
                } catch (const std::exception& error) {
                    log("cleanup", error); cleanup_safe = false;
                    job->data["state"] = "cleanup_pending"; save(job);
                    ++iterator; continue;
                }
            }
            if (!selected && !job->cancel && job->data.at("expires").get<long long>() > time_now() &&
                (state == "queued" || state == "analysis_queued")) {
                selected = job; decode = state == "queued";
                job->data["state"] = decode ? "decoding" : "analyzing"; save(job);
            }
            ++iterator;
        }
        return {selected, decode};
    }
    void worker_loop() {
        while (!stopping) {
            const auto next = take_work();
            const auto& selected = next.first;
            if (selected) {
                try { execute(selected, next.second); }
                catch (const std::exception& error) {
                    std::lock_guard<std::mutex> lock(mutex);
                    log("worker", error);
                    selected->data["state"] = "cleanup_pending";
                    selected->data["error"] = "Processing failed: unsupported replay, worker failure, or resource limit";
                    selected->data["expires"] = time_now(); save(selected);
                }
            } else Sleep(200);
        }
    }
};
int process_roster(const fs::path& work) {
    const auto match = cs2::read_json(work / "match.json");
    cs2::canonical_core(match);
    require(match.at("players").is_object() && match.at("players").size() <= 64, "Roster size invalid");
    Json players = Json::array();
    for (auto iterator = match.at("players").begin(); iterator != match.at("players").end(); ++iterator) {
        if (iterator.key() == "0") continue;
        cs2::parse_steam_id(iterator.key());
        const auto name = iterator.value().at("name").get<std::string>();
        require(name.size() <= 256, "Player name exceeds bounds");
        players.push_back({{"steam_id", iterator.key()}, {"name", name}});
    }
    cs2::write_text(work / "roster.json", players.dump());
    return 0;
}
int process_analysis(const fs::path& work, const std::string& steam) {
    const auto match = cs2::read_json(work / "match.json");
    require(match.at("players").contains(steam), "Selected player absent");
    cs2::Report report;
    report.steam_id = steam;
    report.name = match.at("players").at(steam).at("name").get<std::string>();
    report.platform = match.at("source").get<std::string>();
    auto stats = cs2::analyze_match(match, steam, "m1", report);
    require(!stats.is_null(), "Selected player has no analyzable match data");
    report.matches.push_back(stats);
    const auto output = work / "report";
    fs::create_directory(output);
    cs2::write_report(report, output, "Replay");
    const auto manifest = small_json(output / "manifest.json", 1024 * 1024);
    require(manifest.at("status") == "complete", "Report completion missing");
    const auto publish = work / "publish";
    fs::create_directory(publish);
    Json publication;
    for (const auto& pair : std::vector<std::pair<std::string, std::string>>{
        {"analysis.json", "result.json"}, {"Replay-report.html", "report.html"}}) {
        bool confirmed = false;
        for (const auto& file : manifest.at("outputs")) if (file.at("path") == pair.first)
            confirmed = file.at("sha256") == cs2::sha256_file(output / pair.first) &&
                file.at("bytes") == fs::file_size(output / pair.first);
        require(confirmed, "Report artifact is not manifest-confirmed");
        fs::copy_file(output / pair.first, publish / pair.second);
        publication[pair.second] = {{"bytes", fs::file_size(publish / pair.second)},
            {"sha256", cs2::sha256_file(publish / pair.second)}};
    }
    cs2::write_text(publish / "publication.json", publication.dump());
    return 0;
}
int serve(const fs::path& root) {
    require(!elevated(), "The public service must run as a normal user");
    Handle mutex(CreateMutexW(nullptr, TRUE, object_name(root, L".Mutex").c_str()));
    require(mutex.value && GetLastError() != ERROR_ALREADY_EXISTS, "CSDA service already running");
    Handle stop(CreateEventW(nullptr, TRUE, FALSE, object_name(root, L".Stop").c_str()));
    require(stop.value != nullptr, "Cannot create stop event");
    Server server(root);
    FILETIME created{}, exited{}, kernel{}, user{};
    require(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != 0,
        "Cannot identify service creation time");
    const auto creation = (static_cast<unsigned long long>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    WSADATA sockets{};
    require(WSAStartup(MAKEWORD(2, 2), &sockets) == 0, "Cannot initialize network");
    const auto listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(listener != INVALID_SOCKET, "Cannot create listener");
    BOOL exclusive = TRUE;
    require(setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char*>(&exclusive), sizeof(exclusive)) == 0,
        "Cannot reserve exact listener");
    sockaddr_in address{}; address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(8091);
    require(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
        listen(listener, 16) == 0, "Cannot bind loopback service");
    const auto control = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(control != INVALID_SOCKET &&
        connect(control, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
        "Uncontained listener positive control failed");
    const auto accepted = accept(listener, nullptr, nullptr);
    require(accepted != INVALID_SOCKET, "Listener positive control was not accepted");
    closesocket(accepted); closesocket(control);
    const auto probe = server.local / "startup-probe";
    const auto canary = server.local / "private-canary.txt";
    cs2::write_text(canary, "synthetic service canary");
    std::atomic<bool> cancel{false};
    auto proof = cs2::service::run_isolated(cs2::service::image_path(),
        {L"--probe", probe.wstring(), canary.wstring()}, probe, cancel, GiB, 20);
    require(proof.at("exit_code") == 0, "Startup containment check failed; uploads remain closed");
    require(small_json(probe / "isolation.json").at("network_denied") == true, "Containment evidence missing");
    atomic_json(server.local / "containment.json", proof);
    fs::remove_all(probe); fs::remove(canary);
    server.ready = true;
    std::thread worker([&] { server.worker_loop(); });
    std::vector<std::thread> pool;
    for (unsigned index = 0; index < 4; ++index) pool.emplace_back([&] {
        while (!server.stopping) {
            const auto client = accept(listener, nullptr, nullptr);
            if (client == INVALID_SOCKET) break;
            bool constructed = false;
            try {
                Request request(client);
                constructed = true;
                try { server.handle(request); }
                catch (const ApiError& error) { request.send(error.status, Json{{"error", error.what()}}.dump()); }
                catch (const std::exception& error) {
                    { std::lock_guard<std::mutex> lock(server.mutex); server.log("request", error); }
                    if (!request.response_started)
                        request.send(500, Json{{"error", "Request failed; no result was published"}}.dump());
                }
            } catch (const std::exception& error) {
                if (!constructed) closesocket(client);
                std::lock_guard<std::mutex> lock(server.mutex); server.log("transport", error);
            }
        }
    });
    while (WaitForSingleObject(stop.value, 1000) == WAIT_TIMEOUT) {
        atomic_json(server.local / "service-status.json", Json{{"pid", GetCurrentProcessId()},
            {"created", creation}, {"ready", true}, {"heartbeat", now()}, {"role", "csda-service-v1"}});
    }
    server.stopping = true;
    { std::lock_guard<std::mutex> lock(server.mutex); for (auto& pair : server.jobs) pair.second->cancel = true; }
    closesocket(listener);
    for (auto& thread : pool) thread.join();
    worker.join();
    WSACleanup();
    atomic_json(server.local / "service-status.json", Json{{"ready", false}, {"heartbeat", now()}});
    return 0;
}
struct Internet {
    HINTERNET value;
    explicit Internet(HINTERNET handle) : value(handle) { require(value != nullptr, "HTTP client initialization failed"); }
    ~Internet() { WinHttpCloseHandle(value); }
};
struct Reply { DWORD status = 0; std::string body, cookie; };
Reply local_request(const std::wstring& method, const std::string& path, const std::string& cookie = "",
                    const std::string& csrf = "", const std::string& body = "", bool valid_origin = true) {
    Internet session(WinHttpOpen(L"CSDA qualification/1", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0));
    require(WinHttpSetTimeouts(session.value, 5000, 5000, 180000, 180000) != 0, "HTTP timeouts failed");
    Internet connection(WinHttpConnect(session.value, L"127.0.0.1", 8091, 0));
    Internet request(WinHttpOpenRequest(connection.value, method.c_str(), wide(path).c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS;
    require(WinHttpSetOption(request.value, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)) != 0,
        "Cannot disable HTTP redirects");
    const auto headers = wide("Origin: " + std::string(valid_origin ? "https://csda.autom8ed.me" : "https://invalid.example") +
        "\r\nContent-Type: application/octet-stream\r\n" + (cookie.empty() ? "" : "Cookie: " + cookie + "\r\n") +
        (csrf.empty() ? "" : "X-CSDA-CSRF: " + csrf + "\r\n"));
    require(body.size() <= MAXDWORD && WinHttpSendRequest(request.value, headers.c_str(),
        static_cast<DWORD>(headers.size()), body.empty() ? nullptr : const_cast<char*>(body.data()),
        static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) != 0 &&
        WinHttpReceiveResponse(request.value, nullptr) != 0, "HTTP qualification request failed");
    Reply result; DWORD size = sizeof(result.status);
    require(WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        nullptr, &result.status, &size, nullptr) != 0, "HTTP status unavailable");
    wchar_t cookie_text[2048]{}; size = sizeof(cookie_text);
    if (WinHttpQueryHeaders(request.value, WINHTTP_QUERY_SET_COOKIE, nullptr, cookie_text, &size, nullptr))
        result.cookie = cs2::utf8(cookie_text);
    char buffer[65536]; DWORD count = 0;
    do {
        require(WinHttpReadData(request.value, buffer, sizeof(buffer), &count) != 0 &&
            result.body.size() + count <= 16 * 1024 * 1024, "Qualification response failed or exceeded bound");
        result.body.append(buffer, count);
    } while (count);
    return result;
}
int qualify(const fs::path& root, const fs::path& demo) {
    require(!elevated(), "Qualification must use the normal service user");
    require(local_request(L"GET", "/").status == 200, "Real page is unavailable");
    require(local_request(L"POST", "/api/jobs").status == 401, "Anonymous job creation was allowed");
    const auto secret = beta_secret(root, false);
    const auto payload = Json{{"invite", secret}}.dump();
    require(local_request(L"POST", "/api/session", "", "", payload, false).status == 403,
        "Foreign-origin session creation was allowed");
    auto login = local_request(L"POST", "/api/session", "", "", payload);
    require(login.status == 200 && !login.cookie.empty(), "Beta login failed");
    const auto cookie = login.cookie.substr(0, login.cookie.find(';'));
    const auto csrf = Json::parse(login.body).at("csrf").get<std::string>();
    require(local_request(L"POST", "/api/jobs", cookie, "wrong", "{}").status == 403, "CSRF gate failed");
    auto created = local_request(L"POST", "/api/jobs", cookie, csrf, "{}");
    require(created.status == 201, "Job admission failed");
    const auto id = Json::parse(created.body).at("id").get<std::string>();
    const auto path = "/api/jobs/" + id;
    auto second = local_request(L"POST", "/api/session", "", "", payload);
    require(second.status == 200, "Second owner login failed");
    require(local_request(L"GET", path, second.cookie.substr(0, second.cookie.find(';'))).status == 404,
        "A different session could read another owner's job");
    require(local_request(L"POST", "/api/jobs", cookie, csrf, "{}").status == 409, "Per-owner admission failed");
    require(local_request(L"PUT", path + "/demo", cookie, csrf, "not a replay file").status == 422,
        "Invalid replay signature was accepted");
    require(local_request(L"DELETE", path, cookie, csrf).status == 202, "Job cancellation failed");
    Sleep(1000);
    if (demo.empty()) {
        std::cout << "{\"api_gates\":\"pass\",\"real_demo\":\"not_requested\"}\n"; return 0;
    }
    created = local_request(L"POST", "/api/jobs", cookie, csrf, "{}");
    require(created.status == 201, "Real-demo job admission failed");
    const auto live_id = Json::parse(created.body).at("id").get<std::string>();
    const auto live_path = "/api/jobs/" + live_id;
    require(local_request(L"PUT", live_path + "/demo", cookie, csrf,
        text_file(demo, 512 * 1024 * 1024)).status == 200, "Real demo upload failed");
    auto wait_state = [&](const std::string& expected) {
        for (unsigned attempt = 0; attempt < 300; ++attempt) {
            const auto response = local_request(L"GET", live_path, cookie);
            require(response.status == 200, "Live job became unavailable");
            const auto job = Json::parse(response.body);
            if (job.at("state") == expected) return job;
            require(job.at("state") != "failed" && job.at("state") != "cleanup_pending", "Live worker failed; inspect private errors.log");
            Sleep(1000);
        }
        throw std::runtime_error("Live job timed out");
    };
    const auto roster = wait_state("select");
    require(roster.at("players").size() >= 10, "Public fixture roster incomplete");
    const std::string steam = "76561198057057588";
    require(local_request(L"POST", live_path + "/analysis", cookie, csrf,
        Json{{"steam_id", steam}}.dump()).status == 202, "Real player selection failed");
    const auto complete = wait_state("complete");
    const auto result = local_request(L"GET", live_path + "/result", cookie);
    require(result.status == 200, "Real result unavailable");
    const auto analysis = Json::parse(result.body);
    require(analysis.at("metadata").at("steam_id") == steam && analysis.at("summary").at("rounds").get<int>() > 0 &&
        analysis.at("warnings").empty(), "Live report failed reconciliation");
    require(!fs::exists(root / "local" / "jobs" / live_id), "Raw replay/export survived publication");
    const auto report = local_request(L"GET", live_path + "/report", cookie);
    require(report.status == 200 && cs2::sha256_text(report.body) == complete.at("artifacts").at("report.html").at("sha256"),
        "Report download hash mismatch");
    atomic_json(root / "local" / "last-qualification.json", Json{{"status", "pass"}, {"at", now()},
        {"api_gates", "pass"}, {"public_fixture", true}, {"job", complete},
        {"native_service_sha256", cs2::sha256_file(cs2::service::image_path())}});
    std::cout << Json{{"status", "pass"}, {"job_id", live_id}, {"players", roster.at("players").size()},
        {"summary", analysis.at("summary")}, {"resource_phases", complete.at("phases")},
        {"upload_bytes", complete.at("upload_bytes")}, {"upload_ms", complete.at("upload_ms")},
        {"report_bytes", report.body.size()}, {"raw_deleted", true}}.dump() << "\n";
    return 0;
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        require(argc >= 2, "Usage: csda-service <setup|serve|stop|invite> [--root DIRECTORY]");
        const std::wstring action = argv[1];
        if (action == L"qualify" && (argc == 4 || argc == 6) && std::wstring(argv[2]) == L"--root") {
            require(argc == 4 || std::wstring(argv[4]) == L"--demo", "Unknown qualification arguments");
            return qualify(fs::weakly_canonical(argv[3]), argc == 6 ? fs::path(argv[5]) : fs::path());
        }
        if (action == L"--probe" && argc == 4) return cs2::service::worker_probe(argv[2], argv[3]);
        if (action == L"--roster" && argc == 3) return process_roster(argv[2]);
        if (action == L"--analyze" && argc == 4) return process_analysis(argv[2], cs2::utf8(argv[3]));
        const auto root = fs::weakly_canonical(argc == 4 && std::wstring(argv[2]) == L"--root" ?
            fs::path(argv[3]) : cs2::service::image_path().parent_path().parent_path().parent_path());
        require(argc == 2 || (argc == 4 && std::wstring(argv[2]) == L"--root"), "Unknown service arguments");
        if (action == L"setup") {
            require(!elevated(), "Setup must run under the normal service user");
            const auto local = root / "local";
            cs2::service::private_directory(local);
            cs2::service::private_directory(local / "jobs");
            cs2::service::private_directory(local / "results");
            if (!fs::exists(local / "service.json")) atomic_json(local / "service.json",
                Json{{"schema_version", 1}, {"origin", "https://csda.autom8ed.me"}, {"port", 8091},
                    {"upload_max_bytes", 512ULL * 1024 * 1024}});
            beta_secret(root, true);
            std::cout << "{\"configured\":true,\"credentials\":\"Windows Credential Manager; not printed\"}\n";
        } else if (action == L"serve") {
            FILE* log = nullptr;
            require(_wfreopen_s(&log, (root / "local" / "service-native.log").c_str(), L"a", stderr) == 0,
                "Cannot open native service diagnostics");
            return serve(root);
        }
        else if (action == L"stop") {
            Handle event(OpenEventW(EVENT_MODIFY_STATE, FALSE, object_name(root, L".Stop").c_str()));
            require(event.value && SetEvent(event.value), "Service stop signal unavailable");
        } else if (action == L"invite") {
            auto secret = beta_secret(root, false);
            if (MessageBoxW(nullptr, L"Copy the private beta access code to your clipboard?\r\n"
                L"Then paste it into https://csda.autom8ed.me.\r\nKeep the code private.",
                L"CSDA beta access", MB_YESNO | MB_ICONINFORMATION) == IDYES) {
                const auto text = wide(secret);
                require(OpenClipboard(nullptr) != 0, "Clipboard unavailable");
                const auto memory = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
                require(memory != nullptr, "Clipboard allocation failed");
                auto* destination = GlobalLock(memory);
                require(destination != nullptr, "Clipboard lock failed");
                memcpy(destination, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
                GlobalUnlock(memory);
                require(EmptyClipboard() != 0 && SetClipboardData(CF_UNICODETEXT, memory) != nullptr,
                    "Clipboard write failed");
                CloseClipboard();
            }
            SecureZeroMemory(secret.data(), secret.size());
        } else throw std::runtime_error("Unknown service action");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CSDA: " << error.what() << "\n";
        return 1;
    }
}

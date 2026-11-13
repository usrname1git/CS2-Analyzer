#define CSDA_SERVICE_TEST
#define wmain csda_production_entry
#include "..\src\service.cpp"
#undef wmain
#include <future>

namespace {
void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Function>
void status_error(Function action, int status) {
    try { action(); }
    catch (const ApiError& error) {
        expect(error.status == status, "Unexpected typed API status"); return;
    }
    throw std::runtime_error("Expected typed API rejection");
}
struct SocketPair {
    SOCKET client = INVALID_SOCKET, server = INVALID_SOCKET;
    SocketPair() {
        const auto listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        require(listener != INVALID_SOCKET, "Fixture listener failed");
        sockaddr_in address{};
        address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
            listen(listener, 1) == 0, "Fixture bind failed");
        int size = sizeof(address);
        require(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size) == 0, "Fixture port unavailable");
        client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        require(client != INVALID_SOCKET && connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
            "Fixture connection failed");
        server = accept(listener, nullptr, nullptr); closesocket(listener);
        require(server != INVALID_SOCKET, "Fixture accept failed");
        DWORD timeout = 2000;
        require(setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout)) == 0,
            "Fixture receive timeout failed");
    }
    ~SocketPair() {
        if (client != INVALID_SOCKET) closesocket(client);
        if (server != INVALID_SOCKET) closesocket(server);
    }
    SOCKET take_server() { const auto value = server; server = INVALID_SOCKET; return value; }
    void write(const std::string& text) {
        size_t offset = 0;
        while (offset < text.size()) {
            const auto count = ::send(client, text.data() + offset, static_cast<int>(text.size() - offset), 0);
            require(count > 0, "Fixture send failed"); offset += static_cast<size_t>(count);
        }
    }
    std::string read() {
        std::string text; char buffer[65536];
        for (;;) {
            const auto count = recv(client, buffer, sizeof(buffer), 0);
            require(count >= 0, "Fixture response timed out or failed");
            if (!count) return text;
            text.append(buffer, static_cast<size_t>(count));
        }
    }
};
std::string serve_once(Server& server, SOCKET socket_value) {
    Request request(socket_value);
    try { server.handle(request); }
    catch (const ApiError& error) { request.send(error.status, Json{{"error", error.what()}}.dump()); }
    catch (const std::exception& error) {
        { std::lock_guard<std::mutex> lock(server.mutex); server.log("fixture-request", error); }
        if (!request.response_started) request.send(500, Json{{"error", "Request failed"}}.dump());
        return error.what();
    }
    return "";
}
struct FixtureReply {
    int status;
    std::string headers, body;
    explicit FixtureReply(const std::string& response) {
        const auto end = response.find("\r\n\r\n");
        expect(end != std::string::npos, "Missing HTTP response framing");
        headers = response.substr(0, end); body = response.substr(end + 4);
        status = std::stoi(headers.substr(9, 3));
    }
};
const std::string token(64, 'a'), invite(64, 'b'), csrf = "fixture-csrf";
const std::string cookie = "__Host-csda=" + token;
std::string wire(const std::string& method, const std::string& path, const std::string& body = "",
                 const std::string& cookies = cookie, const std::string& extra = "") {
    return method + " " + path + " HTTP/1.1\r\nHost: fixture.invalid\r\nOrigin: https://csda.invalid\r\n"
        "X-CSDA-CSRF: " + csrf + "\r\n" + (cookies.empty() ? "" : "Cookie: " + cookies + "\r\n") +
        extra + "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}
FixtureReply call(Server& server, const std::string& method, const std::string& path, const std::string& body = "",
           const std::string& cookies = cookie, const std::string& extra = "") {
    SocketPair sockets;
    sockets.write(wire(method, path, body, cookies, extra));
    auto handling = std::async(std::launch::async, [&server, value = sockets.take_server()] { return serve_once(server, value); });
    const auto response = sockets.read();
    expect(handling.get().empty(), "Native request failed unexpectedly");
    return FixtureReply(response);
}
bool cleanup_failed = false;
struct Fixture {
    fs::path root = fs::current_path() / ("service-fixture-" + cs2::random_suffix());
    std::atomic<long long> instant{1000};
    std::unique_ptr<Server> server;
    Fixture() {
        for (const auto* directory : {"local\\jobs", "local\\results", "web"}) fs::create_directories(root / directory);
        cs2::write_text(root / "web" / "index.html", "synthetic fixture page");
        cs2::write_text(root / "web" / "app.js", "synthetic fixture script");
        server = std::make_unique<Server>(root, Json{{"origin", "https://csda.invalid"}, {"port", 8091},
            {"upload_max_bytes", 512ULL * 1024 * 1024}}, invite, [this] { return instant.load(); });
        server->ready = true;
        server->sessions[cs2::sha256_text(token)] = {{"csrf", csrf}, {"expires", 10000}};
        server->db.put("sessions", cs2::sha256_text(token), server->sessions.at(cs2::sha256_text(token)));
    }
    ~Fixture() {
        transport_observer = {};
        server.reset();
        std::error_code error;
        fs::remove_all(root, error);
        if (error) { cleanup_failed = true; std::cerr << "Fixture cleanup failed: " << error.message() << "\n"; }
    }
    std::shared_ptr<Job> job(char key, const std::string& state, long long expires = 1900) {
        auto value = std::make_shared<Job>();
        const std::string id(32, key);
        value->data = {{"id", id}, {"owner", cs2::sha256_text(token)}, {"state", state},
            {"expires", expires}, {"phases", Json::array()},
            {"players", Json::array({Json{{"steam_id", "76561198000000001"}, {"name", "Fixture"}}})}};
        fs::create_directories(server->workspace(id));
        server->jobs[id] = value; server->save(value); return value;
    }
    std::shared_ptr<Job> artifact(size_t size = 4096) {
        auto value = job('1', "complete");
        const auto destination = root / "local" / "results" / std::string(32, '1');
        fs::create_directories(destination);
        Json artifacts;
        for (const auto* filename : {"report.html", "result.json"}) {
            const auto text = std::string(size, 'x');
            cs2::write_text(destination / filename, text);
            artifacts[filename] = {{"bytes", text.size()}, {"sha256", cs2::sha256_text(text)}};
        }
        value->data["artifacts"] = artifacts; server->save(value); return value;
    }
};
std::string path(char key = '1') { return "/api/jobs/" + std::string(32, key); }
const std::string selection = R"({"steam_id":"76561198000000001"})";
void cookies_and_transport() {
    Fixture fixture; auto& server = *fixture.server;
    unsigned io_calls = 0;
    transport_observer = [&](bool) {
        const auto available = std::async(std::launch::async, [&] {
            if (!server.mutex.try_lock()) return false;
            server.mutex.unlock(); return true;
        }).get();
        expect(available, "Socket I/O ran while the state mutex was held"); ++io_calls;
    };
    expect(call(server, "GET", "/api/session", "", "other=value; " + cookie + "; last=x").status == 200, "Exact cookie rejected");
    for (const auto& invalid : std::vector<std::string>{"x" + cookie, "other=" + cookie, cookie + "; " + cookie,
        cookie + "; __Host-csda=" + std::string(64, 'c'), "__Host-csda=invalid"}) {
        expect(call(server, "GET", "/api/session", "", invalid).status == 401, "Ambiguous or embedded cookie authorized");
    }
    expect(call(server, "POST", "/api/jobs", "", cookie, "X-Other: fixture\r\n").status == 201, "Reservation failed");
    const auto reserved = server.jobs.begin()->first;
    auto rejected_csrf = wire("DELETE", "/api/jobs/" + reserved);
    rejected_csrf.replace(rejected_csrf.find(csrf), csrf.size(), "wrong");
    {
        SocketPair sockets; sockets.write(rejected_csrf);
        auto handling = std::async(std::launch::async, [&server, value = sockets.take_server()] { return serve_once(server, value); });
        expect(FixtureReply(sockets.read()).status == 403, "Wrong CSRF authorized"); expect(handling.get().empty(), "CSRF transport failed");
    }
    expect(call(server, "GET", "/api/jobs/" + reserved).status == 200, "Status failed");
    const std::string other_token(64, 'c');
    server.sessions[cs2::sha256_text(other_token)] = {{"csrf", csrf}, {"expires", 10000}};
    expect(call(server, "GET", "/api/jobs/" + reserved, "", "__Host-csda=" + other_token).status == 404,
        "Another owner read the job");
    expect(call(server, "DELETE", "/api/jobs/" + reserved, "", "__Host-csda=" + other_token).status == 404,
        "Another owner cancelled the job");
    expect(call(server, "DELETE", "/api/jobs/" + reserved).status == 202, "Delete failed");
    server.take_work();
    expect(call(server, "POST", "/api/session", Json{{"invite", invite}}.dump(), "").status == 200, "Real login path failed");
    expect(call(server, "POST", "/api/session", "{", "").status == 400, "Malformed JSON lost typed status");
    expect(call(server, "POST", "/api/session", std::string(4097, 'x'), "").status == 413, "Oversize JSON lost byte limit");
    auto job = fixture.job('1', "reserved");
    const auto replay = std::string("PBDEMS2\0", 8) + std::string(16, 'x');
    expect(call(server, "PUT", path() + "/demo", replay).status == 200, "Upload transport failed");
    expect(job->data.at("source_sha256") == cs2::sha256_text(replay) &&
        cs2::sha256_file(server.workspace(std::string(32, '1')) / "upload.dem") == cs2::sha256_text(replay),
        "Sealed upload hash changed");
    job->data["state"] = "select"; server.save(job);
    expect(call(server, "POST", path() + "/analysis", selection).status == 202, "Analysis transport failed");
    fixture.artifact();
    for (const auto* action : {"result", "report", "receipt"})
        expect(call(server, "GET", path() + "/" + action).status == 200, "Artifact transport failed");
    expect(call(server, "HEAD", path() + "/report").body.empty(), "HEAD sent a body");
    server.sessions[cs2::sha256_text(token)]["expires"] = 1000;
    expect(call(server, "GET", "/api/session").status == 401, "Expired session authorized");
    expect(io_calls > 30, "Transport routes were not exercised");
    std::cout << "PASS cookie/session/CSRF gates and actual socket I/O lock observations\n";
}
void login_limits() {
    Fixture fixture; auto& server = *fixture.server;
    auto attempt = [&](const std::string& ip) { std::lock_guard<std::mutex> lock(server.mutex); server.admit_login(ip); };
    for (unsigned index = 0; index < 512; ++index) attempt(std::to_string(index));
    status_error([&] { attempt("new"); }, 429);
    for (unsigned index = 0; index < 5; ++index) attempt("0");
    status_error([&] { attempt("0"); }, 429);
    fixture.instant = 1060;
    status_error([&] { attempt("0"); }, 429);
    fixture.instant = 1061; attempt("new");
    expect(server.logins.size() == 1, "Expired IP buckets were not erased before admission");
    server.logins.clear();
    std::atomic<unsigned> accepted{0};
    std::vector<std::future<void>> attempts;
    for (unsigned index = 0; index < 20; ++index) attempts.emplace_back(std::async(std::launch::async, [&] {
        try { attempt("concurrent"); ++accepted; }
        catch (const ApiError& error) { expect(error.status == 429, "Concurrent admission status changed"); }
    }));
    for (auto& result : attempts) result.get();
    expect(accepted == 6 && server.logins.at("concurrent").size() == 6, "Login attempts were not atomic");
    std::cout << "PASS bounded login capacity, expiry, existing identities and concurrent rate limits\n";
}
void selection_deadlines() {
    Fixture fixture; auto& server = *fixture.server;
    auto job = fixture.job('1', "select", 1001);
    expect(call(server, "POST", path() + "/analysis", selection).status == 202, "Near-boundary analysis rejected");
    expect(job->data.at("expires") == 4600, "Accepted analysis retained selection expiry");
    fixture.instant = 1002;
    expect(server.take_work().first == job && job->data.at("state") == "analyzing", "Accepted work vanished at old expiry");
    job = fixture.job('1', "select", 1002);
    expect(call(server, "POST", path() + "/analysis", selection).status == 409, "Expired selection accepted");
    job = fixture.job('1', "analysis_queued", 1003);
    fixture.instant = 1003;
    expect(!server.take_work().first && !server.jobs.count(std::string(32, '1')), "Queued deadline did not expire");
    job = fixture.job('1', "analysis_queued", 2000); job->cancel = true;
    expect(!server.take_work().first && !server.jobs.count(std::string(32, '1')), "Cancelled work dispatched");
    job = fixture.job('1', "select", 2000);
    auto first = std::async(std::launch::async, [&] { return call(server, "POST", path() + "/analysis", selection).status; });
    auto second = std::async(std::launch::async, [&] { return call(server, "POST", path() + "/analysis", selection).status; });
    const auto a = first.get(), b = second.get();
    expect((a == 202 && b == 409) || (a == 409 && b == 202), "Concurrent selections both committed");
    std::cout << "PASS fake-clock queued retention, expiry, cancellation and atomic selection\n";
}
void slow_body(bool analysis) {
    Fixture fixture; auto& server = *fixture.server;
    if (analysis) fixture.job('1', "select");
    const auto body = analysis ? selection : Json{{"invite", "wrong"}}.dump();
    auto request = wire("POST", analysis ? path() + "/analysis" : "/api/session", body);
    const auto boundary = request.find("\r\n\r\n") + 4;
    SocketPair sockets; sockets.write(request.substr(0, boundary + 1));
    std::promise<void> waiting; auto observed = waiting.get_future();
    std::atomic<unsigned> receives{0};
    transport_observer = [&](bool send) { if (!send && receives++ == 1) waiting.set_value(); };
    auto handling = std::async(std::launch::async, [&server, value = sockets.take_server()] { return serve_once(server, value); });
    expect(observed.wait_for(std::chrono::seconds(2)) == std::future_status::ready, "Body did not reach actual recv");
    expect(call(server, "GET", "/api/session").status == 200, "Slow body blocked an unrelated session");
    if (analysis) expect(call(server, "DELETE", path()).status == 202, "Slow selection blocked cancellation");
    sockets.write(request.substr(boundary + 1));
    expect(FixtureReply(sockets.read()).status == (analysis ? 409 : 403), "Body completion did not revalidate state");
    expect(handling.get().empty(), "Slow-body handling failed");
    std::cout << "PASS real stalled " << (analysis ? "analysis" : "unauthenticated login") << " body concurrency\n";
}
void artifact_accounting() {
    Fixture fixture; auto& server = *fixture.server;
    auto job = fixture.artifact();
    const auto head = call(server, "HEAD", path() + "/report");
    expect(head.status == 200 && head.body.empty() && head.headers.find("Content-Length: 4096") != std::string::npos,
        "HEAD framing changed");
    expect(job->data.value("served_payload_bytes", 0ULL) == 0, "HEAD inflated served payload");
    expect(call(server, "GET", path() + "/report").body.size() == 4096, "GET body changed");
    expect(Json::parse(call(server, "GET", path() + "/receipt").body).at("served_payload_bytes") == 4096, "GET receipt accounting failed");
    job = fixture.artifact(2 * 1024 * 1024);
    job->data["served_payload_bytes"] = 0; server.save(job);
    SocketPair sockets;
    sockets.write(wire("GET", path() + "/report"));
    std::promise<void> pause, release; auto paused = pause.get_future(); auto released = release.get_future().share();
    std::atomic<unsigned> sends{0};
    transport_observer = [&](bool send) { if (send && sends++ == 1) { pause.set_value(); released.wait(); } };
    auto handling = std::async(std::launch::async, [&server, value = sockets.take_server()] { return serve_once(server, value); });
    char buffer[8192];
    const auto count = recv(sockets.client, buffer, sizeof(buffer), 0);
    expect(count > 1024, "Partial-transfer fixture sent no body");
    expect(paused.wait_for(std::chrono::seconds(2)) == std::future_status::ready, "Partial transfer did not pause");
    linger reset{1, 0};
    require(setsockopt(sockets.client, SOL_SOCKET, SO_LINGER, reinterpret_cast<char*>(&reset), sizeof(reset)) == 0,
        "Fixture connection reset setup failed");
    closesocket(sockets.client); sockets.client = INVALID_SOCKET;
    Sleep(50); release.set_value();
    expect(handling.get() == "Response interrupted", "Partial transfer failure was silently swallowed");
    expect(job->data.at("served_payload_bytes") == 0, "Failed partial response counted as completed payload");
    std::cout << "PASS HEAD, GET receipt and real interrupted partial-transfer accounting\n";
}
void stalled_response_and_delete() {
    Fixture fixture; auto& server = *fixture.server; fixture.artifact(2 * 1024 * 1024);
    SocketPair sockets;
    int buffer_size = 8192;
    require(setsockopt(sockets.server, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char*>(&buffer_size), sizeof(buffer_size)) == 0,
        "Fixture send buffer limit failed");
    buffer_size = 65536;
    require(setsockopt(sockets.client, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&buffer_size), sizeof(buffer_size)) == 0,
        "Fixture receive buffer limit failed");
    sockets.write(wire("GET", path() + "/report"));
    std::promise<void> sending; auto observed = sending.get_future();
    std::atomic<unsigned> sends{0};
    transport_observer = [&](bool send) { if (send && sends++ == 0) sending.set_value(); };
    auto handling = std::async(std::launch::async, [&server, value = sockets.take_server()] { return serve_once(server, value); });
    expect(observed.wait_for(std::chrono::seconds(2)) == std::future_status::ready, "Response did not reach send");
    expect(handling.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout,
        "Unread socket did not backpressure the production response");
    expect(call(server, "GET", "/api/session").status == 200, "Stalled response blocked session reads");
    expect(call(server, "DELETE", path()).status == 202, "Stalled response blocked cancellation");
    expect(!server.take_work().first && server.jobs.empty(), "Cancellation cleanup did not erase the job");
    const auto remaining = sockets.read();
    expect(handling.get().empty() && FixtureReply(remaining).body.size() == 2 * 1024 * 1024,
        "Authorized snapshot did not finish after delete");
    expect(server.jobs.empty() && server.db.load("jobs").empty(), "Post-send accounting resurrected deleted job");
    std::cout << "PASS stalled native response concurrency and no post-delete ledger resurrection\n";
}
}
int main(int argc, char** argv) {
    try {
        WSADATA data{}; require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "Fixture Winsock startup failed");
        const std::string selected = argc == 2 ? argv[1] : "all";
        expect(argc <= 2 && (selected == "all" || selected == "cookies" || selected == "rate" ||
            selected == "expiry" || selected == "bodies" || selected == "accounting" || selected == "response"),
            "Unknown fixture selector");
        if (selected == "all" || selected == "cookies") cookies_and_transport();
        if (selected == "all" || selected == "rate") login_limits();
        if (selected == "all" || selected == "expiry") selection_deadlines();
        if (selected == "all" || selected == "bodies") { slow_body(false); slow_body(true); }
        if (selected == "all" || selected == "accounting") artifact_accounting();
        if (selected == "all" || selected == "response") stalled_response_and_delete();
        WSACleanup(); expect(!cleanup_failed, "Fixture artifacts could not be cleaned");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Service fixture failure: " << error.what() << "\n"; return 1;
    }
}

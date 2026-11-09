#include <winsock2.h>
#include <ws2tcpip.h>
#include "worker.h"
#include <userenv.h>
#include <aclapi.h>
#include <sddl.h>
#include <psapi.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <thread>

namespace cs2::service {
namespace fs = std::filesystem;
void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
std::wstring wide(const std::string& text) {
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    require(size > 0 || text.empty(), "Invalid UTF-8");
    std::wstring result(static_cast<size_t>(size), L'\0');
    if (size) require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), size) == size, "UTF-8 conversion failed");
    return result;
}
fs::path image_path() {
    std::wstring path(32768, L'\0');
    const auto count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    require(count > 0 && count < path.size(), "Cannot identify current image");
    path.resize(count);
    return path;
}
static std::wstring user_sid() {
    Handle token;
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value) != 0, "Cannot open caller token");
    DWORD size = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    require(GetTokenInformation(token.value, TokenUser, buffer.data(), size, &size) != 0, "Cannot identify caller");
    LPWSTR text = nullptr;
    require(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text) != 0,
        "Cannot encode caller SID");
    std::wstring result(text);
    LocalFree(text);
    return result;
}
static void owner_acl(const fs::path& path) {
    const auto sddl = L"D:P(A;OICI;FA;;;" + user_sid() + L")(A;OICI;FA;;;SY)";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
        &descriptor, nullptr) != 0, "Cannot construct private directory ACL");
    const bool ok = SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION |
        PROTECTED_DACL_SECURITY_INFORMATION, descriptor) != 0;
    LocalFree(descriptor);
    require(ok, "Cannot restrict private directory");
}
void private_directory(const fs::path& path) {
    fs::create_directories(path);
    require(!(GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT), "Directory must not be a reparse point");
    owner_acl(path);
}
void validate_tree(const fs::path& path) {
    const auto root_attributes = GetFileAttributesW(path.c_str());
    require(root_attributes != INVALID_FILE_ATTRIBUTES && !(root_attributes & FILE_ATTRIBUTE_REPARSE_POINT),
        "Job root is unavailable or redirected");
    size_t entries = 0;
    for (const auto& entry : fs::recursive_directory_iterator(path)) {
        const auto attributes = GetFileAttributesW(entry.path().c_str());
        require(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
            ++entries <= 4096, "Job tree redirected or exceeded its file budget");
    }
}
void grant_package(const fs::path& path, PSID sid, DWORD access, bool directory) {
    PACL old_acl = nullptr, new_acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    require(GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, &old_acl, nullptr, &descriptor) == ERROR_SUCCESS, "Cannot read job ACL");
    EXPLICIT_ACCESSW entry{};
    entry.grfAccessPermissions = access;
    entry.grfAccessMode = GRANT_ACCESS;
    entry.grfInheritance = directory ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
    entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entry.Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
    entry.Trustee.ptstrName = static_cast<LPWSTR>(sid);
    auto error = SetEntriesInAclW(1, &entry, old_acl, &new_acl);
    if (error == ERROR_SUCCESS) error = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()),
        SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, new_acl, nullptr);
    if (new_acl) LocalFree(new_acl);
    LocalFree(descriptor);
    require(error == ERROR_SUCCESS, "Cannot grant package job access");
    if (access & FILE_WRITE_DATA) {
        PSECURITY_DESCRIPTOR low = nullptr;
        require(ConvertStringSecurityDescriptorToSecurityDescriptorW(directory ? L"S:(ML;OICI;NW;;;LW)" : L"S:(ML;;NW;;;LW)",
            SDDL_REVISION_1, &low, nullptr) != 0, "Cannot construct job integrity label");
        const bool ok = SetFileSecurityW(path.c_str(), LABEL_SECURITY_INFORMATION, low) != 0;
        LocalFree(low);
        require(ok, "Cannot label job directory");
    }
}
std::uint64_t available_memory() {
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    require(GlobalMemoryStatusEx(&memory) != 0, "Cannot read host memory");
    return memory.ullAvailPhys;
}
static std::wstring quote(const std::wstring& text) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (const auto c : text) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        if (c == L'"') result += L'\\';
        result += c; slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L"\"";
}
struct Profile {
    std::wstring name;
    PSID sid = nullptr;
    explicit Profile(const std::wstring& value) : name(value) {
        const auto hr = CreateAppContainerProfile(name.c_str(), L"CSDA isolated worker",
            L"Temporary replay parser; no network capabilities", nullptr, 0, &sid);
        require(SUCCEEDED(hr), "Cannot create isolated worker profile (HRESULT " + std::to_string(hr) + ")");
    }
    ~Profile() { if (sid) FreeSid(sid); }
    void remove() {
        const auto hr = DeleteAppContainerProfile(name.c_str());
        require(SUCCEEDED(hr), "Worker profile cleanup failed (HRESULT " + std::to_string(hr) + ")");
    }
};
struct AttributeList {
    std::vector<BYTE> buffer;
    LPPROC_THREAD_ATTRIBUTE_LIST value;
    AttributeList(SECURITY_CAPABILITIES& capabilities, HANDLE* handles, size_t count) : value(nullptr) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 2, 0, &size);
        buffer.resize(size);
        value = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
        require(InitializeProcThreadAttributeList(value, 2, 0, &size) != 0, "Cannot initialize worker attributes");
        require(UpdateProcThreadAttribute(value, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
            &capabilities, sizeof(capabilities), nullptr, nullptr) != 0, "Cannot set worker containment");
        require(UpdateProcThreadAttribute(value, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            handles, count * sizeof(HANDLE), nullptr, nullptr) != 0, "Cannot restrict inherited worker handles");
    }
    ~AttributeList() { if (value) DeleteProcThreadAttributeList(value); }
};
struct ChildReaper {
    HANDLE process;
    ~ChildReaper() {
        if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
            if (!TerminateProcess(process, 1) || WaitForSingleObject(process, 10000) != WAIT_OBJECT_0)
                std::fputs("CSDA: created worker termination/reap failed\n", stderr);
        }
    }
};
Json run_isolated(const fs::path& executable, const std::vector<std::wstring>& arguments,
                  const fs::path& workspace, std::atomic<bool>& cancel,
                  std::uint64_t memory_bytes, unsigned timeout_seconds, bool protect_match) {
    if (fs::exists(workspace)) validate_tree(workspace);
    private_directory(workspace);
    Profile profile(L"CSDA.Job." + wide(random_suffix()));
    const auto marker = workspace / "worker-profile.txt";
    write_text(marker, utf8(profile.name));
    const auto bin = workspace / "bin";
    fs::create_directories(bin);
    const auto copy = bin / executable.filename();
    fs::copy_file(executable, copy, fs::copy_options::overwrite_existing);
    const DWORD modify = FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE;
    grant_package(workspace, profile.sid, modify, true);
    owner_acl(bin);
    owner_acl(copy);
    grant_package(bin, profile.sid, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, true);
    grant_package(copy, profile.sid, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, false);
    // Existing files need explicit package grants as their ACLs predate the job SID.
    for (const auto& entry : fs::recursive_directory_iterator(workspace)) {
        require(!entry.is_symlink() && !(GetFileAttributesW(entry.path().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT),
            "Job tree contains a reparse point");
        if (entry.path() == bin || entry.path().parent_path() == bin) continue;
        const auto name = entry.path().filename().wstring();
        if (!entry.is_directory() && (name == L"upload.dem" || (name == L"match.json" && protect_match) ||
            name == L"worker-profile.txt")) {
            owner_acl(entry.path());
            grant_package(entry.path(), profile.sid, FILE_GENERIC_READ, false);
        } else grant_package(entry.path(), profile.sid, modify, entry.is_directory());
    }
    fs::create_directories(workspace / "temp");
    SECURITY_CAPABILITIES capabilities{};
    capabilities.AppContainerSid = profile.sid;
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle output(CreateFileW((workspace / "worker.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &inherit, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    require(output.value != INVALID_HANDLE_VALUE && input.value != INVALID_HANDLE_VALUE, "Cannot capture worker diagnostics");
    HANDLE inherited[] = {output.value, input.value};
    AttributeList attributes(capabilities, inherited, 2);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes.value;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = output.value;
    startup.StartupInfo.hStdError = output.value;
    startup.StartupInfo.hStdInput = input.value;
    std::wstring command = quote(copy.wstring());
    for (const auto& argument : arguments) command += L" " + quote(argument);
    wchar_t system[32768]{};
    require(GetEnvironmentVariableW(L"SystemRoot", system, 32768) > 0, "SystemRoot unavailable");
    std::map<std::wstring, std::wstring> safe_environment;
    for (const auto* key : {L"APPDATA", L"LOCALAPPDATA", L"USERPROFILE", L"SYSTEMDRIVE"}) {
        wchar_t value[32768]{};
        const auto count = GetEnvironmentVariableW(key, value, 32768);
        require(count > 0 && count < 32768, "Required Windows profile environment unavailable");
        safe_environment[key] = value;
    }
    safe_environment[L"SYSTEMROOT"] = system;
    safe_environment[L"WINDIR"] = system;
    safe_environment[L"PATH"] = std::wstring(system) + L"\\System32";
    safe_environment[L"TEMP"] = (workspace / "temp").wstring();
    safe_environment[L"TMP"] = (workspace / "temp").wstring();
    std::wstring environment;
    for (const auto& entry : safe_environment)
        environment += entry.first + L"=" + entry.second + L'\0';
    environment += L'\0';
    Handle job(CreateJobObjectW(nullptr, nullptr));
    require(job.value != nullptr, "Cannot create worker Job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
        JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_JOB_MEMORY;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    limits.JobMemoryLimit = static_cast<SIZE_T>(memory_bytes);
    require(SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) != 0,
        "Cannot enforce worker resource limits");
    PROCESS_INFORMATION process{};
    const bool created = CreateProcessW(copy.c_str(), command.data(), nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW |
        CREATE_UNICODE_ENVIRONMENT | BELOW_NORMAL_PRIORITY_CLASS, environment.data(),
        workspace.c_str(), &startup.StartupInfo, &process) != 0;
    const auto create_error = GetLastError();
    require(created, "Cannot create contained worker (Win32 " + std::to_string(create_error) + ")");
    Handle child(process.hProcess), thread(process.hThread);
    ChildReaper reaper{child.value};
    if (!AssignProcessToJobObject(job.value, child.value)) {
        TerminateProcess(child.value, 1);
        WaitForSingleObject(child.value, 10000);
        throw std::runtime_error("Worker Job assignment failed; created child terminated");
    }
    const auto started = GetTickCount64();
    require(ResumeThread(thread.value) != static_cast<DWORD>(-1), "Cannot resume worker");
    std::string failure;
    while (WaitForSingleObject(child.value, 100) == WAIT_TIMEOUT) {
        JOBOBJECT_BASIC_AND_IO_ACCOUNTING_INFORMATION accounting{};
        require(QueryInformationJobObject(job.value, JobObjectBasicAndIoAccountingInformation,
            &accounting, sizeof(accounting), nullptr) != 0, "Cannot monitor worker");
        if (cancel.load()) failure = "Cancelled";
        else if (GetTickCount64() - started > timeout_seconds * 1000ULL) failure = "Worker timed out";
        else if (accounting.IoInfo.WriteTransferCount > 3ULL * 1024 * 1024 * 1024)
            failure = "Worker exceeded temporary-write budget";
        else if (available_memory() < 4ULL * 1024 * 1024 * 1024) failure = "Host memory reserve reached";
        if (!failure.empty()) {
            require(TerminateJobObject(job.value, 1) != 0, "Cannot terminate owned worker Job");
            require(WaitForSingleObject(child.value, 10000) == WAIT_OBJECT_0, "Worker exit not confirmed");
            break;
        }
    }
    DWORD exit_code = 1;
    require(WaitForSingleObject(child.value, 0) == WAIT_OBJECT_0, "Worker exit was not confirmed");
    require(GetExitCodeProcess(child.value, &exit_code) != 0, "Cannot read worker exit");
    JOBOBJECT_BASIC_AND_IO_ACCOUNTING_INFORMATION accounting{};
    require(QueryInformationJobObject(job.value, JobObjectBasicAndIoAccountingInformation,
        &accounting, sizeof(accounting), nullptr) != 0, "Cannot read worker accounting");
    require(QueryInformationJobObject(job.value, JobObjectExtendedLimitInformation,
        &limits, sizeof(limits), nullptr) != 0, "Cannot read worker peak memory");
    PROCESS_MEMORY_COUNTERS memory{};
    memory.cb = sizeof(memory);
    require(GetProcessMemoryInfo(child.value, &memory, sizeof(memory)) != 0, "Cannot read peak working set");
    Json result{{"exit_code", exit_code}, {"elapsed_ms", GetTickCount64() - started},
        {"cpu_seconds", (accounting.BasicInfo.TotalKernelTime.QuadPart +
            accounting.BasicInfo.TotalUserTime.QuadPart) / 10000000.0},
        {"peak_memory_bytes", limits.PeakJobMemoryUsed},
        {"peak_working_set_bytes", memory.PeakWorkingSetSize},
        {"read_bytes", accounting.IoInfo.ReadTransferCount},
        {"write_bytes", accounting.IoInfo.WriteTransferCount}, {"memory_limit_bytes", memory_bytes},
        {"isolation", "appcontainer-no-capabilities-v1"}};
    profile.remove();
    fs::remove(marker);
    if (!failure.empty()) result["error"] = failure;
    return result;
}
int worker_probe(const fs::path& workspace, const fs::path& private_canary, unsigned short probe_port) {
    Handle token;
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value) != 0, "Probe token unavailable");
    DWORD container = 0, size = 0;
    require(GetTokenInformation(token.value, TokenIsAppContainer, &container, sizeof(container), &size) != 0 &&
        container == 1, "Probe is not contained");
    TOKEN_ELEVATION elevation{};
    require(GetTokenInformation(token.value, TokenElevation, &elevation, sizeof(elevation), &size) != 0 &&
        !elevation.TokenIsElevated, "Probe is elevated");
    GetTokenInformation(token.value, TokenCapabilities, nullptr, 0, &size);
    std::vector<BYTE> capability_data(size);
    require(GetTokenInformation(token.value, TokenCapabilities, capability_data.data(), size, &size) != 0 &&
        reinterpret_cast<TOKEN_GROUPS*>(capability_data.data())->GroupCount == 0,
        "Probe has unexpected capabilities");
    Handle canary(CreateFileW(private_canary.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, 0, nullptr));
    const DWORD denied = GetLastError();
    require(canary.value == INVALID_HANDLE_VALUE && denied == ERROR_ACCESS_DENIED, "Private-path access was not denied");
    Handle bin_write(CreateFileW((image_path().parent_path() / "unexpected-write.txt").c_str(),
        GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr));
    require(bin_write.value == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED,
        "Worker binary directory is writable");
    WSADATA data{};
    require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "Probe Winsock failed");
    const auto socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(socket != INVALID_SOCKET, "Probe socket failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(probe_port);
    const int connected = connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    const int error = WSAGetLastError();
    closesocket(socket);
    WSACleanup();
    require(connected == SOCKET_ERROR && (error == WSAEACCES || error == WSAETIMEDOUT),
        "Network denial was not proven (connect " + std::to_string(connected) +
        ", Winsock " + std::to_string(error) + ")");
    require(GetEnvironmentVariableW(L"GODBRAIN_API_TOKEN", nullptr, 0) == 0 &&
        GetEnvironmentVariableW(L"MONGODB_URI", nullptr, 0) == 0, "Worker inherited credentials");
    write_text(workspace / "isolation.json", Json{{"appcontainer", true}, {"elevated", false},
        {"private_read_denied", true}, {"network_denied", true}, {"credentials_absent", true}}.dump());
    return 0;
}
}

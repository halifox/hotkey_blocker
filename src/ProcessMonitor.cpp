#include "ProcessMonitor.h"

#include "PathUtils.h"

#include <windows.h>
#include <wbemidl.h>

#include <tlhelp32.h>

#include <atomic>
#include <new>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// WMI process-trace subscriptions are the primary path. Some user contexts
// do not have permission to subscribe, so keep a low-frequency compatibility
// fallback instead of making the normal path pay the polling cost.
constexpr DWORD kFallbackScanIntervalMs = 1000;
constexpr DWORD kProcessResolveRetries = 3;
constexpr DWORD kProcessResolveRetryDelayMs = 50;

bool IsStopRequested(HANDLE stopEvent) {
    return stopEvent != nullptr && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0;
}

}  // namespace

class ProcessMonitor::WmiSink final : public IWbemObjectSink {
public:
    using Handler = std::function<void(void*)>;

    explicit WmiSink(Handler handler) : m_handler(std::move(handler)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interfaceId,
                                              void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        if (interfaceId == IID_IUnknown || interfaceId == IID_IWbemObjectSink) {
            *object = static_cast<IWbemObjectSink*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_references.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG references = m_references.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (references == 0) {
            delete this;
        }
        return references;
    }

    HRESULT STDMETHODCALLTYPE Indicate(LONG count, IWbemClassObject** objects) override {
        if (objects == nullptr || count <= 0 || !m_handler) {
            return WBEM_S_NO_ERROR;
        }
        for (LONG index = 0; index < count; ++index) {
            if (objects[index] != nullptr) {
                m_handler(objects[index]);
            }
        }
        return WBEM_S_NO_ERROR;
    }

    HRESULT STDMETHODCALLTYPE SetStatus(LONG, HRESULT, BSTR, IWbemClassObject*) override {
        return WBEM_S_NO_ERROR;
    }

private:
    std::atomic<ULONG> m_references{1};
    Handler m_handler;
};

ProcessMonitor::ProcessMonitor() = default;

ProcessMonitor::~ProcessMonitor() {
    Stop();
}

bool ProcessMonitor::Start(Callback callback) {
    Stop();

    HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE pendingEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stopEvent == nullptr || pendingEvent == nullptr || readyEvent == nullptr) {
        if (stopEvent != nullptr) {
            CloseHandle(stopEvent);
        }
        if (pendingEvent != nullptr) {
            CloseHandle(pendingEvent);
        }
        if (readyEvent != nullptr) {
            CloseHandle(readyEvent);
        }
        return false;
    }

    {
        std::lock_guard lock(m_mutex);
        m_callback = std::move(callback);
        m_currentProcesses.clear();
        m_pendingEvents.clear();
        m_stopEvent = stopEvent;
        m_pendingEvent = pendingEvent;
        m_readyEvent = readyEvent;
        m_stopRequested = false;
        m_running = true;
    }

    try {
        m_thread = std::thread(&ProcessMonitor::Run, this);
    } catch (...) {
        std::lock_guard lock(m_mutex);
        m_callback = {};
        m_currentProcesses.clear();
        m_pendingEvents.clear();
        m_running = false;
        m_stopRequested = false;
        m_stopEvent = nullptr;
        m_pendingEvent = nullptr;
        m_readyEvent = nullptr;
        CloseHandle(stopEvent);
        CloseHandle(pendingEvent);
        CloseHandle(readyEvent);
        return false;
    }
    return true;
}

void ProcessMonitor::Stop() {
    HANDLE stopEvent = nullptr;
    HANDLE pendingEvent = nullptr;
    HANDLE readyEvent = nullptr;
    {
        std::lock_guard lock(m_mutex);
        if (!m_running && !m_thread.joinable()) {
            return;
        }
        m_stopRequested = true;
        stopEvent = m_stopEvent;
        pendingEvent = m_pendingEvent;
        readyEvent = m_readyEvent;
    }

    if (stopEvent != nullptr) {
        SetEvent(stopEvent);
    }
    if (pendingEvent != nullptr) {
        SetEvent(pendingEvent);
    }
    if (readyEvent != nullptr) {
        SetEvent(readyEvent);
    }

    if (m_thread.joinable()) {
        m_thread.join();
    }

    {
        std::lock_guard lock(m_mutex);
        m_callback = {};
        m_currentProcesses.clear();
        m_pendingEvents.clear();
        m_running = false;
        m_stopRequested = false;
        m_stopEvent = nullptr;
        m_pendingEvent = nullptr;
        m_readyEvent = nullptr;
    }

    if (stopEvent != nullptr) {
        CloseHandle(stopEvent);
    }
    if (pendingEvent != nullptr) {
        CloseHandle(pendingEvent);
    }
    if (readyEvent != nullptr) {
        CloseHandle(readyEvent);
    }
}

std::vector<ProcessInfo> ProcessMonitor::Snapshot() const {
    std::lock_guard lock(m_mutex);
    std::vector<ProcessInfo> result;
    result.reserve(m_currentProcesses.size());
    for (const auto& [pid, process] : m_currentProcesses) {
        (void)pid;
        result.push_back(process);
    }
    return result;
}

bool ProcessMonitor::WaitUntilReady(DWORD timeoutMs) const {
    HANDLE readyEvent = nullptr;
    {
        std::lock_guard lock(m_mutex);
        readyEvent = m_readyEvent;
    }
    return readyEvent != nullptr && WaitForSingleObject(readyEvent, timeoutMs) == WAIT_OBJECT_0;
}

bool ProcessMonitor::IsProcessAlive(const ProcessInfo& process) {
    if (process.pid == 0) {
        return false;
    }

    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
                                process.pid);
    if (handle == nullptr) {
        return false;
    }

    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    bool sameProcess = true;
    if (process.creationTime != 0) {
        if (!GetProcessTimes(handle, &creation, &exit, &kernel, &user)) {
            CloseHandle(handle);
            return false;
        }
        ULARGE_INTEGER value{};
        value.LowPart = creation.dwLowDateTime;
        value.HighPart = creation.dwHighDateTime;
        sameProcess = value.QuadPart == process.creationTime;
    }

    const DWORD waitResult = WaitForSingleObject(handle, 0);
    CloseHandle(handle);
    return sameProcess && waitResult == WAIT_TIMEOUT;
}

bool ProcessMonitor::QueryProcessInfo(DWORD pid, ProcessInfo& process) {
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (handle == nullptr) {
        return false;
    }

    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(handle, &creation, &exit, &kernel, &user)) {
        CloseHandle(handle);
        return false;
    }

    std::vector<wchar_t> buffer(32768);
    DWORD length = static_cast<DWORD>(buffer.size());
    const BOOL success = QueryFullProcessImageNameW(handle, 0, buffer.data(), &length);
    CloseHandle(handle);
    if (!success || length == 0) {
        return false;
    }

    std::wstring imagePath(buffer.data(), length);
    imagePath = PathUtils::NormalizePath(imagePath);
    if (imagePath.empty()) {
        return false;
    }

    ULARGE_INTEGER creationValue{};
    creationValue.LowPart = creation.dwLowDateTime;
    creationValue.HighPart = creation.dwHighDateTime;
    process = {pid, std::move(imagePath), creationValue.QuadPart};
    return true;
}

bool ProcessMonitor::SameProcess(const ProcessInfo& left, const ProcessInfo& right) {
    if (left.pid != right.pid) {
        return false;
    }
    return left.creationTime == 0 || right.creationTime == 0 ||
           left.creationTime == right.creationTime;
}

ProcessMonitor::ProcessMap ProcessMonitor::Enumerate(const ProcessMap& previous) {
    ProcessMap current;
    const DWORD ownPid = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return previous;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == ownPid || entry.th32ProcessID == 0) {
                continue;
            }

            ProcessInfo process;
            if (!QueryProcessInfo(entry.th32ProcessID, process)) {
                const auto oldProcess = previous.find(entry.th32ProcessID);
                if (oldProcess != previous.end()) {
                    current.emplace(oldProcess->first, oldProcess->second);
                }
                continue;
            }
            current.emplace(process.pid, std::move(process));
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return current;
}

void ProcessMonitor::QueueRawEvent(ProcessEventType type, DWORD pid) {
    if (pid == 0) {
        return;
    }

    HANDLE pendingEvent = nullptr;
    {
        std::lock_guard lock(m_mutex);
        if (m_stopRequested) {
            return;
        }
        m_pendingEvents.push_back({type, pid});
        pendingEvent = m_pendingEvent;
    }
    if (pendingEvent != nullptr) {
        SetEvent(pendingEvent);
    }
}

void ProcessMonitor::HandleWmiObject(ProcessEventType type, void* object) {
    auto* processObject = static_cast<IWbemClassObject*>(object);
    if (processObject == nullptr) {
        return;
    }

    VARIANT value{};
    VariantInit(&value);
    BSTR propertyName = SysAllocString(L"ProcessID");
    const HRESULT result = propertyName == nullptr
                               ? E_OUTOFMEMORY
                               : processObject->Get(propertyName, 0, &value, nullptr, nullptr);
    if (propertyName != nullptr) {
        SysFreeString(propertyName);
    }
    if (SUCCEEDED(result)) {
        DWORD pid = 0;
        if (value.vt == VT_UI4) {
            pid = value.ulVal;
        } else if (value.vt == VT_I4 && value.lVal > 0) {
            pid = static_cast<DWORD>(value.lVal);
        }
        QueueRawEvent(type, pid);
    }
    VariantClear(&value);
}

void ProcessMonitor::HandleRawEvent(const RawProcessEvent& event) {
    if (event.type == ProcessEventType::Started) {
        ProcessInfo process;
        bool resolved = false;
        for (DWORD attempt = 0; attempt < kProcessResolveRetries; ++attempt) {
            if (QueryProcessInfo(event.pid, process)) {
                resolved = true;
                break;
            }
            if (attempt + 1 < kProcessResolveRetries &&
                WaitForSingleObject(m_stopEvent, kProcessResolveRetryDelayMs) == WAIT_OBJECT_0) {
                return;
            }
        }
        if (!resolved) {
            return;
        }

        ProcessInfo previous;
        bool hadPrevious = false;
        {
            std::lock_guard lock(m_mutex);
            if (m_stopRequested) {
                return;
            }
            const auto iterator = m_currentProcesses.find(process.pid);
            if (iterator != m_currentProcesses.end()) {
                previous = iterator->second;
                hadPrevious = true;
                if (SameProcess(previous, process)) {
                    return;
                }
            }
            m_currentProcesses[process.pid] = process;
        }

        if (hadPrevious) {
            Publish({ProcessEventType::Exited, previous, false});
        }
        Publish({ProcessEventType::Started, process, false});
        return;
    }

    ProcessInfo process;
    {
        std::lock_guard lock(m_mutex);
        const auto iterator = m_currentProcesses.find(event.pid);
        if (iterator == m_currentProcesses.end()) {
            return;
        }
        process = iterator->second;
    }

    // A stop notification can arrive just before the process handle becomes
    // signaled. Wait on the process handle itself, with the monitor stop event
    // as the cancellation path; this keeps the exit path event-driven too.
    HANDLE processHandle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                                       FALSE, process.pid);
    if (processHandle != nullptr) {
        HANDLE handles[] = {m_stopEvent, processHandle};
        const DWORD waitResult = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        CloseHandle(processHandle);
        if (waitResult == WAIT_OBJECT_0 || waitResult != WAIT_OBJECT_0 + 1) {
            return;
        }
    } else {
        // The stop trace can be delivered after the process handle is already
        // gone. Query once more to distinguish that case from a reused PID.
        ProcessInfo replacement;
        if (QueryProcessInfo(event.pid, replacement) && SameProcess(process, replacement)) {
            return;
        }
    }

    bool removed = false;
    {
        std::lock_guard lock(m_mutex);
        const auto iterator = m_currentProcesses.find(event.pid);
        if (iterator != m_currentProcesses.end() && SameProcess(iterator->second, process)) {
            m_currentProcesses.erase(iterator);
            removed = true;
        }
    }
    if (removed) {
        Publish({ProcessEventType::Exited, process, false});
    }
}

void ProcessMonitor::DrainPendingEvents() {
    for (;;) {
        if (m_pendingEvent != nullptr) {
            ResetEvent(m_pendingEvent);
        }

        std::deque<RawProcessEvent> pending;
        {
            std::lock_guard lock(m_mutex);
            pending.swap(m_pendingEvents);
        }
        if (pending.empty()) {
            return;
        }

        for (const RawProcessEvent& event : pending) {
            HandleRawEvent(event);
        }
    }
}

void ProcessMonitor::Publish(const ProcessEvent& event) {
    Callback callback;
    {
        std::lock_guard lock(m_mutex);
        if (m_stopRequested) {
            return;
        }
        callback = m_callback;
    }
    if (callback) {
        callback(event);
    }
}

bool ProcessMonitor::RunWmi() {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
        return false;
    }
    if (initialized == RPC_E_CHANGED_MODE) {
        return false;
    }

    const HRESULT security = CoInitializeSecurity(
        nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT,
        RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
    if (FAILED(security) && security != RPC_E_TOO_LATE) {
        CoUninitialize();
        return false;
    }

    IWbemLocator* locator = nullptr;
    IWbemServices* services = nullptr;
    WmiSink* startSink = nullptr;
    WmiSink* stopSink = nullptr;
    bool subscribedToStart = false;
    bool subscribedToStop = false;

    HRESULT result = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_IWbemLocator,
                                      reinterpret_cast<void**>(&locator));
    if (SUCCEEDED(result)) {
        BSTR namespaceName = SysAllocString(L"ROOT\\CIMV2");
        result = namespaceName == nullptr
                     ? E_OUTOFMEMORY
                     : locator->ConnectServer(namespaceName, nullptr, nullptr, nullptr, 0,
                                               nullptr, nullptr, &services);
        if (namespaceName != nullptr) {
            SysFreeString(namespaceName);
        }
    }
    if (SUCCEEDED(result)) {
        result = CoSetProxyBlanket(
            services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
            RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    }

    auto subscribe = [&](const wchar_t* query, WmiSink*& sink, bool& subscribed) {
        if (FAILED(result)) {
            return;
        }
        sink = new (std::nothrow) WmiSink([this, query](void* object) {
            const ProcessEventType type =
                std::wstring_view(query).find(L"StartTrace") != std::wstring_view::npos
                    ? ProcessEventType::Started
                    : ProcessEventType::Exited;
            HandleWmiObject(type, object);
        });
        if (sink == nullptr) {
            result = E_OUTOFMEMORY;
            return;
        }

        BSTR language = SysAllocString(L"WQL");
        BSTR queryText = SysAllocString(query);
        if (language == nullptr || queryText == nullptr) {
            if (language != nullptr) {
                SysFreeString(language);
            }
            if (queryText != nullptr) {
                SysFreeString(queryText);
            }
            result = E_OUTOFMEMORY;
            return;
        }
        result = services->ExecNotificationQueryAsync(
            language, queryText, WBEM_FLAG_SEND_STATUS, nullptr, sink);
        SysFreeString(language);
        SysFreeString(queryText);
        subscribed = SUCCEEDED(result);
    };

    subscribe(L"SELECT * FROM Win32_ProcessStartTrace", startSink, subscribedToStart);
    subscribe(L"SELECT * FROM Win32_ProcessStopTrace", stopSink, subscribedToStop);

    if (SUCCEEDED(result)) {
        const ProcessMap initial = Enumerate({});
        {
            std::lock_guard lock(m_mutex);
            if (!m_stopRequested) {
                m_currentProcesses = initial;
            }
        }
        for (const auto& [pid, process] : initial) {
            (void)pid;
            Publish({ProcessEventType::Started, process, true});
        }

        // Events received while the baseline was being enumerated are handled
        // after the baseline, so a process created during startup is not
        // incorrectly classified as an already-running process.
        DrainPendingEvents();
        SignalReady();

        HANDLE handles[] = {m_stopEvent, m_pendingEvent};
        while (!IsStopRequested(m_stopEvent)) {
            const DWORD waitResult = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            if (waitResult == WAIT_OBJECT_0) {
                break;
            }
            if (waitResult == WAIT_OBJECT_0 + 1) {
                DrainPendingEvents();
                continue;
            }
            result = E_FAIL;
            break;
        }
    }

    if (subscribedToStart && startSink != nullptr) {
        services->CancelAsyncCall(startSink);
    }
    if (subscribedToStop && stopSink != nullptr) {
        services->CancelAsyncCall(stopSink);
    }
    if (startSink != nullptr) {
        startSink->Release();
    }
    if (stopSink != nullptr) {
        stopSink->Release();
    }
    if (services != nullptr) {
        services->Release();
    }
    if (locator != nullptr) {
        locator->Release();
    }
    CoUninitialize();

    return SUCCEEDED(result);
}

void ProcessMonitor::RunPollingFallback() {
    ProcessMap previous;
    bool firstScan = true;

    while (!IsStopRequested(m_stopEvent)) {
        ProcessMap current = Enumerate(previous);
        std::vector<ProcessEvent> events;
        events.reserve(current.size() + previous.size());

        for (const auto& [pid, process] : current) {
            const auto oldProcess = previous.find(pid);
            if (oldProcess == previous.end()) {
                events.push_back({ProcessEventType::Started, process, firstScan});
            } else if (!SameProcess(oldProcess->second, process) ||
                       !PathUtils::SamePath(oldProcess->second.imagePath,
                                            process.imagePath)) {
                events.push_back({ProcessEventType::Exited, oldProcess->second, false});
                events.push_back({ProcessEventType::Started, process, false});
            }
        }
        for (const auto& [pid, process] : previous) {
            if (current.find(pid) == current.end()) {
                events.push_back({ProcessEventType::Exited, process, false});
            }
        }

        {
            std::lock_guard lock(m_mutex);
            if (m_stopRequested) {
                break;
            }
            m_currentProcesses = current;
        }
        for (const ProcessEvent& event : events) {
            Publish(event);
        }

        if (firstScan) {
            SignalReady();
        }

        previous = std::move(current);
        firstScan = false;
        if (WaitForSingleObject(m_stopEvent, kFallbackScanIntervalMs) == WAIT_OBJECT_0) {
            break;
        }
    }
}

void ProcessMonitor::SignalReady() {
    HANDLE readyEvent = nullptr;
    {
        std::lock_guard lock(m_mutex);
        readyEvent = m_readyEvent;
    }
    if (readyEvent != nullptr) {
        SetEvent(readyEvent);
    }
}

void ProcessMonitor::Run() {
    const bool wmiStarted = RunWmi();
    if (wmiStarted) {
        return;
    }

    if (!IsStopRequested(m_stopEvent)) {
        RunPollingFallback();
    }
}

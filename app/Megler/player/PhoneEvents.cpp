#include "pch.h"

#include "player/PhoneEvents.h"

#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include "megler.h"

#include <winrt/Windows.ApplicationModel.Calls.h>
#include <winrt/Windows.ApplicationModel.Chat.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.System.Threading.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace megler::app::phoneEvents {
namespace {

namespace chat = winrt::Windows::ApplicationModel::Chat;
namespace calls = winrt::Windows::ApplicationModel::Calls;
using winrt::Windows::Foundation::DateTime;

std::mutex gMutex;
bool gStarted = false;
int64_t gSince = 0;              // ms since 1970: what is older was the phone's before the app started
int64_t gNewestCall = 0;         // the start of the newest call told
std::set<std::wstring> gToldMessages;
chat::ChatMessageStore gChat{nullptr};
calls::PhoneCallHistoryStore gCalls{nullptr};
std::atomic<bool> gReadingCalls{false};

int64_t msOf(DateTime t) {
    const int64_t ticks = int64_t(winrt::clock::to_file_time(t).value); // 100 ns since 1601
    return (ticks - 116444736000000000LL) / 10000;
}

int64_t nowMs() { return msOf(winrt::clock::now()); }

// A message of the store, told once if it came in after the app started. Nothing of it goes in the log but that it came.
void tellMessage(chat::ChatMessage const& m, std::wstring const& id) {
    if (!m || !m.IsIncoming() || m.MessageKind() != chat::ChatMessageKind::Standard) return;
    int64_t at = msOf(m.NetworkTimestamp());
    if (at <= 0) at = msOf(m.LocalTimestamp());
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (msOf(m.LocalTimestamp()) < gSince || !gToldMessages.insert(id).second) return;
    }
    megler_sms_received(narrow(std::wstring(m.From())).c_str(), narrow(std::wstring(m.Body())).c_str(), at);
    shellLog::write("phone: a text message came in: told to the app");
}

winrt::fire_and_forget messageMade(chat::ChatMessageStore store, winrt::hstring id) {
    // The event's sender can be null (Windows 10 Mobile's ChatApis): the store watched is asked then. Asked of the null
    // sender, Megler ended with an access violation as a code came in for Gosuslugi's sign-up (package 558).
    if (!store) {
        std::lock_guard<std::mutex> lock(gMutex);
        store = gChat;
    }
    if (!store) {
        shellLog::write("phone: a text message came in, with no store to read it from: nothing told");
        co_return;
    }
    try {
        chat::ChatMessage m = co_await store.GetMessageAsync(id);
        tellMessage(m, std::wstring(id));
    } catch (const winrt::hresult_error& e) {
        shellLog::write(format("phone: a text message came in that Windows does not give (0x%08X)", unsigned(e.code())));
    }
}

winrt::fire_and_forget openMessages() {
    try {
        chat::ChatMessageStore store = co_await chat::ChatMessageManager::RequestStoreAsync();
        if (!store) {
            shellLog::write("phone: Windows gives no text message store: an app hears of no message");
            co_return;
        }
        {
            std::lock_guard<std::mutex> lock(gMutex);
            gChat = store;
        }
        // (Never taken off: wanted until the process ends. It comes on a thread of Windows' own.)
        store.StoreChanged([](chat::ChatMessageStore const& s, chat::ChatMessageStoreChangedEventArgs const& e) {
            if (e.Kind() == chat::ChatStoreChangedEventKind::MessageCreated) messageMade(s, e.Id());
        });
        shellLog::write("phone: text messages watched: one that comes in is told to the app");
    } catch (const winrt::hresult_error& e) {
        shellLog::write(format("phone: Windows does not let Megler read text messages (0x%08X): an app hears of no message", unsigned(e.code())));
    }
}

// The messages of the last while, for what came while Megler was suspended and the store's word was not heard.
winrt::fire_and_forget readMessages() {
    chat::ChatMessageStore store{nullptr};
    int64_t since;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        store = gChat;
        since = gSince;
    }
    if (!store) co_return;
    try {
        const auto recent = std::chrono::milliseconds(std::max<int64_t>(nowMs() - since, 1000));
        chat::ChatMessageReader reader = store.GetMessageReader(std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(recent));
        auto batch = co_await reader.ReadBatchAsync();
        std::vector<chat::ChatMessage> fresh;
        for (auto const& m : batch) fresh.push_back(m);
        std::sort(fresh.begin(), fresh.end(), [](auto const& a, auto const& b) { return msOf(a.LocalTimestamp()) < msOf(b.LocalTimestamp()); });
        for (auto const& m : fresh) tellMessage(m, std::wstring(m.Id()));
    } catch (const winrt::hresult_error& e) {
        shellLog::write(format("phone: the recent text messages cannot be read (0x%08X)", unsigned(e.code())));
    }
}

// The call history's newest calls that began after the app started and were not told, oldest first.
winrt::fire_and_forget readCalls() {
    calls::PhoneCallHistoryStore store{nullptr};
    {
        std::lock_guard<std::mutex> lock(gMutex);
        store = gCalls;
    }
    if (!store || gReadingCalls.exchange(true)) co_return;
    try {
        auto batch = co_await store.GetEntryReader().ReadBatchAsync();
        struct call {
            int64_t at;
            int type, seconds;
            std::string number;
        };
        std::vector<call> fresh;
        {
            std::lock_guard<std::mutex> lock(gMutex);
            for (auto const& e : batch) {
                const int64_t at = msOf(e.StartTime());
                if (at < gSince || at <= gNewestCall) continue;
                int seconds = 0;
                if (auto d = e.Duration()) seconds = int(std::chrono::duration_cast<std::chrono::seconds>(d.Value()).count());
                // Android's call log types: 1 incoming, 2 outgoing, 3 missed
                const int type = e.IsIncoming() ? (e.IsMissed() ? 3 : 1) : 2;
                const auto address = e.Address();
                fresh.push_back({at, type, seconds, address ? narrow(std::wstring(address.RawAddress())) : std::string()});
            }
            for (auto const& c : fresh) gNewestCall = std::max(gNewestCall, c.at);
        }
        std::sort(fresh.begin(), fresh.end(), [](call const& a, call const& b) { return a.at < b.at; });
        for (auto const& c : fresh) megler_call_logged(c.number.c_str(), c.at, c.type, c.seconds);
        if (!fresh.empty()) shellLog::write(format("phone: %d call(s) of the phone's history since the app started: told to the app", int(fresh.size())));
    } catch (const winrt::hresult_error& e) {
        shellLog::write(format("phone: the call history cannot be read (0x%08X)", unsigned(e.code())));
    }
    gReadingCalls = false;
}

// A call has ended: the history has it a moment later, and is read then and once more.
void callsSoon() {
    using winrt::Windows::System::Threading::ThreadPoolTimer;
    for (int ms : {1500, 6000}) ThreadPoolTimer::CreateTimer([](auto&&) { readCalls(); }, std::chrono::milliseconds(ms));
}

winrt::fire_and_forget openCalls() {
    try {
        calls::PhoneCallHistoryStore store{nullptr};
        try {
            store = co_await calls::PhoneCallHistoryManager::RequestStoreAsync(calls::PhoneCallHistoryStoreAccessType::AllEntriesLimitedReadWrite);
        } catch (const winrt::hresult_error&) {
        }
        if (!store) store = co_await calls::PhoneCallHistoryManager::RequestStoreAsync(calls::PhoneCallHistoryStoreAccessType::AllEntriesReadWrite);
        if (!store) {
            shellLog::write("phone: Windows gives no call history: an app hears of no call");
            co_return;
        }
        {
            std::lock_guard<std::mutex> lock(gMutex);
            gCalls = store;
        }
        bool told = false;
        if (winrt::Windows::Foundation::Metadata::ApiInformation::IsTypePresent(L"Windows.ApplicationModel.Calls.PhoneCallManager")) {
            calls::PhoneCallManager::CallStateChanged([](auto&&, auto&&) {
                if (!calls::PhoneCallManager::IsCallActive() && !calls::PhoneCallManager::IsCallIncoming()) callsSoon();
            });
            told = true;
        }
        shellLog::write(told ? "phone: calls watched: once one is over, the calls of the history since the app started are told to it"
                             : "phone: Windows says nothing of calls as they come: the call history is read when Megler is resumed");
    } catch (const winrt::hresult_error& e) {
        shellLog::write(format("phone: Windows does not let Megler read the call history (0x%08X): an app hears of no call", unsigned(e.code())));
    }
}

} // namespace

void start(bool messages, bool calls) {
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (gStarted) return;
        gStarted = true;
        gSince = nowMs();
    }
    if (messages) openMessages();
    if (calls) openCalls();
    if (!messages || !calls)
        shellLog::write(std::string("phone: ") + (!messages && !calls ? "neither text messages nor calls" : !messages ? "no text messages" : "no calls") +
                        " watched: the app's permissions ask for none");
}

void check() {
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (!gStarted) return;
    }
    readMessages();
    readCalls();
}

} // namespace megler::app::phoneEvents

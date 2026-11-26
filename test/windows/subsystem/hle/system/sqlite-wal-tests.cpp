#include "../../support/test-suite.h"
#include "hle/system/stub-libsqlite.h"
#include <windows.h>
#include <sqlite3.h>
#include <fstream>
#include <iostream>
#include <vector>

using namespace stone::windows;
using namespace stone::windows::test;

namespace {
template <class T> T Sqlite(const char* name, const char* signature)
{
    const auto target = SqliteExport(name, signature);
    STONE_CHECK(target);
    return reinterpret_cast<T>(target);
}

struct Api {
    decltype(&sqlite3_open_v2) open = Sqlite<decltype(open)>("sqlite3_open_v2", "(LLIL)I");
    decltype(&sqlite3_close) close = Sqlite<decltype(close)>("sqlite3_close", "(L)I");
    decltype(&sqlite3_exec) exec = Sqlite<decltype(exec)>("sqlite3_exec", "(LLLLL)I");
    decltype(&sqlite3_free) free = Sqlite<decltype(free)>("sqlite3_free", "(L)V");
    decltype(&sqlite3_prepare_v2) prepare = Sqlite<decltype(prepare)>("sqlite3_prepare_v2", "(LLILL)I");
    decltype(&sqlite3_step) step = Sqlite<decltype(step)>("sqlite3_step", "(L)I");
    decltype(&sqlite3_column_text) text = Sqlite<decltype(text)>("sqlite3_column_text", "(LI)L");
    decltype(&sqlite3_finalize) finalize = Sqlite<decltype(finalize)>("sqlite3_finalize", "(L)I");
    decltype(&sqlite3_wal_checkpoint_v2) checkpoint =
        Sqlite<decltype(checkpoint)>("sqlite3_wal_checkpoint_v2", "(LLILL)I");
    decltype(&sqlite3_file_control) control = Sqlite<decltype(control)>("sqlite3_file_control", "(LLIL)I");
};

Api* api;

struct Database {
    sqlite3* handle = nullptr;

    Database(const std::filesystem::path& path, int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)
    {
        const auto name = path.generic_u8string();
        const int result = api->open(reinterpret_cast<const char*>(name.c_str()), &handle, flags, nullptr);
        if (result != SQLITE_OK) {
            if (handle)
                api->close(handle);
            handle = nullptr;
        }
        STONE_CHECK(result == SQLITE_OK && handle);
    }

    ~Database()
    {
        if (handle)
            api->close(handle);
    }

    void Close()
    {
        STONE_CHECK(api->close(handle) == SQLITE_OK);
        handle = nullptr;
    }

    void Exec(const char* sql, int expected = SQLITE_OK)
    {
        char* error = nullptr;
        const int result = api->exec(handle, sql, nullptr, nullptr, &error);
        const std::string message = error ? error : "";
        if (error)
            api->free(error);
        if (result != expected)
            throw std::runtime_error(std::string(sql) + ": " + message + " code=" + std::to_string(result));
    }

    std::string Scalar(const char* sql)
    {
        sqlite3_stmt* statement = nullptr;

        struct Finish {
            sqlite3_stmt*& value;

            ~Finish()
            {
                if (value)
                    api->finalize(value);
            }
        } finish{statement};

        STONE_CHECK(api->prepare(handle, sql, -1, &statement, nullptr) == SQLITE_OK);
        STONE_CHECK(api->step(statement) == SQLITE_ROW);
        const auto* value = api->text(statement, 0);
        STONE_CHECK(value);
        return reinterpret_cast<const char*>(value);
    }
};

struct Handle {
    HANDLE value = nullptr;

    ~Handle()
    {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};

std::wstring Quoted(const std::wstring& value)
{
    STONE_CHECK(value.find(L'"') == value.npos && !value.ends_with(L'\\'));
    return L'"' + value + L'"';
}

struct Actor {
    Handle ready, release, process;
    bool done = false;

    Actor(const std::filesystem::path& library, const std::filesystem::path& database, const wchar_t* mode)
    {
        static unsigned sequence = 0;
        const auto prefix = L"Local\\StoneSqliteWal-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++sequence);
        const auto first = prefix + L"-ready", second = prefix + L"-release";
        ready.value = CreateEventW(nullptr, TRUE, FALSE, first.c_str());
        release.value = CreateEventW(nullptr, TRUE, FALSE, second.c_str());
        STONE_CHECK(ready.value && release.value);
        wchar_t executable[32768]{};
        STONE_CHECK(GetModuleFileNameW(nullptr, executable, 32768));
        auto command = Quoted(executable) + L" --worker " + Quoted(library.wstring()) + L" " +
                       Quoted(database.wstring()) + L" " + Quoted(first) + L" " + Quoted(second) + L" " + mode;
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION child{};
        STONE_CHECK(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                   nullptr, &startup, &child));
        process.value = child.hProcess;
        CloseHandle(child.hThread);
        HANDLE waits[]{ready.value, process.value};
        const auto result = WaitForMultipleObjects(2, waits, FALSE, 60000);
        if (result != WAIT_OBJECT_0) {
            TerminateProcess(process.value, 9);
            WaitForSingleObject(process.value, 5000);
        }
        STONE_CHECK(result == WAIT_OBJECT_0);
    }

    ~Actor()
    {
        if (!done && process.value) {
            TerminateProcess(process.value, 9);
            WaitForSingleObject(process.value, 5000);
        }
    }

    void Finish()
    {
        STONE_CHECK(SetEvent(release.value));
        STONE_CHECK(WaitForSingleObject(process.value, 60000) == WAIT_OBJECT_0);
        DWORD result = 9;
        STONE_CHECK(GetExitCodeProcess(process.value, &result) && result == 0);
        done = true;
    }
};

void Worker(const std::filesystem::path& path, const wchar_t* readyName, const wchar_t* releaseName,
            std::wstring_view mode)
{
    Handle ready{OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName)},
        release{OpenEventW(SYNCHRONIZE, FALSE, releaseName)};
    STONE_CHECK(ready.value && release.value);
    Database database(path);
    database.Exec("PRAGMA wal_autocheckpoint=0; PRAGMA synchronous=FULL;");
    STONE_CHECK(database.Scalar("PRAGMA journal_mode") == "wal");
    if (mode == L"reader") {
        database.Exec("BEGIN");
        STONE_CHECK(database.Scalar("SELECT v FROM counter") == "2");
    }
    else if (mode == L"writer") {
        database.Exec("BEGIN IMMEDIATE; UPDATE counter SET v=99");
    }
    else if (mode == L"crash") {
        database.Exec(
            "UPDATE counter SET v=7; BEGIN IMMEDIATE; UPDATE counter SET v=8; UPDATE payload SET b=zeroblob(8192) WHERE id<=2000;");
    }
    else
        throw std::runtime_error("Unknown SQLite WAL worker mode");
    STONE_CHECK(SetEvent(ready.value));
    STONE_CHECK(WaitForSingleObject(release.value, 60000) == WAIT_OBJECT_0);
    if (mode == L"reader") {
        STONE_CHECK(database.Scalar("SELECT v FROM counter") == "2");
        database.Exec("COMMIT");
        STONE_CHECK(database.Scalar("SELECT v FROM counter") == "3");
    }
    else if (mode == L"writer")
        database.Exec("ROLLBACK");
    else {
        TerminateProcess(GetCurrentProcess(), 0);
        throw std::runtime_error("Could not terminate crash worker");
    }
}

void Run(const std::filesystem::path& library, const std::filesystem::path& output)
{
    std::filesystem::create_directories(output);
    const auto path =
        output / (L"wal-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L".db");

    struct Files {
        std::filesystem::path path;

        ~Files()
        {
            for (const auto* suffix : {L"", L"-wal", L"-shm", L"-journal"}) {
                std::error_code error;
                std::filesystem::remove(path.wstring() + suffix, error);
            }
        }
    } files{path};

    Database first(path);
    STONE_CHECK(first.Scalar("PRAGMA journal_mode=DELETE") == "delete");
    first.Exec(
        "CREATE TABLE counter(v); INSERT INTO counter VALUES(1); BEGIN IMMEDIATE; UPDATE counter SET v=99; ROLLBACK;");
    STONE_CHECK(first.Scalar("SELECT v FROM counter") == "1");
    std::cout << "PASS rollback journal remains available before WAL conversion\n";
    STONE_CHECK(first.Scalar("PRAGMA journal_mode=WAL") == "wal");
    first.Exec("PRAGMA wal_autocheckpoint=0; CREATE TABLE payload(id INTEGER PRIMARY KEY,b BLOB);");
    Database second(path);
    STONE_CHECK(second.Scalar("PRAGMA journal_mode") == "wal");
    second.Exec("BEGIN");
    STONE_CHECK(second.Scalar("SELECT v FROM counter") == "1");
    first.Exec("BEGIN IMMEDIATE; UPDATE counter SET v=2; COMMIT");
    STONE_CHECK(second.Scalar("SELECT v FROM counter") == "1");
    second.Exec("COMMIT");
    STONE_CHECK(second.Scalar("SELECT v FROM counter") == "2");
    first.Exec("BEGIN IMMEDIATE");
    second.Exec("BEGIN IMMEDIATE", SQLITE_BUSY);
    first.Exec("ROLLBACK");
    first.Exec("BEGIN IMMEDIATE");
    int checkpointed = -1, backfilled = -1;
    STONE_CHECK(api->checkpoint(second.handle, nullptr, SQLITE_CHECKPOINT_PASSIVE, &checkpointed, &backfilled) ==
                SQLITE_BUSY);
    first.Exec("ROLLBACK");
    STONE_CHECK(api->checkpoint(second.handle, nullptr, SQLITE_CHECKPOINT_PASSIVE, &checkpointed, &backfilled) ==
                SQLITE_OK);
    sqlite3_file* file = nullptr;
    STONE_CHECK(api->control(first.handle, nullptr, SQLITE_FCNTL_FILE_POINTER, &file) == SQLITE_OK && file &&
                file->pMethods->iVersion >= 2);
    const auto lock =
        Guest<decltype(sqlite3_io_methods::xShmLock)>(reinterpret_cast<uintptr_t>(file->pMethods->xShmLock), "(LIII)I");
    STONE_CHECK(lock(file, 1, 1, SQLITE_SHM_LOCK | SQLITE_SHM_EXCLUSIVE) == SQLITE_OK);
    second.Exec("BEGIN IMMEDIATE", SQLITE_BUSY);
    STONE_CHECK(lock(file, 0, 1, SQLITE_SHM_LOCK | SQLITE_SHM_EXCLUSIVE) == SQLITE_OK);
    STONE_CHECK(lock(file, 0, 1, SQLITE_SHM_UNLOCK | SQLITE_SHM_EXCLUSIVE) == SQLITE_OK);
    second.Exec("BEGIN IMMEDIATE", SQLITE_BUSY);
    STONE_CHECK(lock(file, 1, 1, SQLITE_SHM_UNLOCK | SQLITE_SHM_EXCLUSIVE) == SQLITE_OK);
    second.Exec("BEGIN IMMEDIATE; ROLLBACK");
    std::cout << "PASS writer/checkpoint exclusion and nested lock lifetime\n";
    std::cout << "PASS WAL read snapshots, concurrent commit and competing writer exclusion\n";
    first.Exec(
        "WITH RECURSIVE n(v) AS (SELECT 1 UNION ALL SELECT v+1 FROM n WHERE v<5000) INSERT INTO payload SELECT v,zeroblob(4000) FROM n");
    STONE_CHECK(std::filesystem::file_size(path.wstring() + L"-shm") >= 65536);
    STONE_CHECK(second.Scalar("SELECT COUNT(*) FROM payload") == "5000");
    std::cout << "PASS WAL index growth across Windows allocation-granularity boundaries\n";
    {
        Actor reader(library, path, L"reader");
        first.Exec("UPDATE counter SET v=3");
        reader.Finish();
        Actor writer(library, path, L"writer");
        first.Exec("BEGIN IMMEDIATE", SQLITE_BUSY);
        STONE_CHECK(api->checkpoint(first.handle, nullptr, SQLITE_CHECKPOINT_PASSIVE, &checkpointed, &backfilled) ==
                    SQLITE_BUSY);
        writer.Finish();
        STONE_CHECK(first.Scalar("SELECT v FROM counter") == "3");
    }
    std::cout << "PASS cross-process WAL snapshots and writer lock ownership\n";
    {
        Database readOnly(path, SQLITE_OPEN_READONLY);
        STONE_CHECK(readOnly.Scalar("SELECT v FROM counter") == "3");
        readOnly.Exec("UPDATE counter SET v=4", SQLITE_READONLY);
    }
    int logged = -1, applied = -1;
    STONE_CHECK(api->checkpoint(first.handle, nullptr, SQLITE_CHECKPOINT_TRUNCATE, &logged, &applied) == SQLITE_OK);
    STONE_CHECK(std::filesystem::file_size(path.wstring() + L"-wal") == 0);
    second.Close();
    first.Close();
    {
        Actor crash(library, path, L"crash");
        crash.Finish();
    }
    Database recovered(path);
    STONE_CHECK(recovered.Scalar("SELECT v FROM counter") == "7");
    STONE_CHECK(recovered.Scalar("SELECT COUNT(*) FROM payload WHERE length(b)=4000") == "5000");
    STONE_CHECK(recovered.Scalar("PRAGMA quick_check") == "ok");
    STONE_CHECK(api->checkpoint(recovered.handle, nullptr, SQLITE_CHECKPOINT_TRUNCATE, &logged, &applied) == SQLITE_OK);
    std::cout << "PASS read-only connection, checkpoint and crash recovery of committed/uncommitted WAL frames\n";
}
}

int wmain(int argc, wchar_t** argv)
{
    try {
        STONE_CHECK(argc == 3 || (argc == 7 && std::wstring_view(argv[1]) == L"--worker"));
        const auto library = std::filesystem::canonical(argv[argc == 3 ? 1 : 2]);
        std::ifstream input(library, std::ios::binary);
        STONE_CHECK(input);
        std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(input), {});
        LoadImageSqlite(bytes);
        const auto registerVfs = Sqlite<decltype(&sqlite3_vfs_register)>("sqlite3_vfs_register", "(LI)I");
        STONE_CHECK(registerVfs(WindowsSqliteVfs(), 1) == SQLITE_OK);
        Api functions;
        api = &functions;
        if (argc == 7)
            Worker(argv[3], argv[4], argv[5], argv[6]);
        else {
            const auto version = Sqlite<decltype(&sqlite3_libversion)>("sqlite3_libversion", "()L");
            const auto source = Sqlite<decltype(&sqlite3_sourceid)>("sqlite3_sourceid", "()L");
            std::cout << "Selected image SQLite " << version() << " source=" << source() << '\n';
            Run(library, std::filesystem::absolute(argv[2]));
        }
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

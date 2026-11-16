// column_sort_probe.cpp — GUI probe: does every column of the column view list its folder
// in that folder's own remembered sort? Runs only in an explicitly isolated --shot test
// instance (PULSE_TEST_COLUMN_SORT_FLOW=<log file>); the fixture is built under
// PULSE_TEST_DATA_DIR\sortjump and its sorts are set in the instance's own preferences.
//
// Fixture: R\{a_old, b_cur, c_new} (folders, oldest to newest), R\b_cur = P holding
// child\ and aa.txt (2019) / zz.txt (2024), P\child = C holding a.txt (2019) / b.txt (2024).
// R and P are sorted by name, C by modified time, newest first.
#include "app_input.h"
#include "app_column_view.h"
#include "app_navigation.h"
#include <cstdio>
#include <string>
#include <vector>

void Render(pulse::AppState& state);

namespace {
using namespace pulse;

std::string Narrow(const std::wstring& text) {
    std::string out;
    for (const wchar_t ch : text) out.push_back(static_cast<char>(ch)); // fixture names are ASCII
    return out;
}

bool MakeFile(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

void SetYear(const std::wstring& path, int year, bool directory) {
    HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st{};
    st.wYear = static_cast<WORD>(year);
    st.wMonth = 6;
    st.wDay = 15;
    FILETIME ft{};
    SystemTimeToFileTime(&st, &ft);
    SetFileTime(h, &ft, &ft, &ft);
    CloseHandle(h);
}

class Probe {
public:
    Probe(AppState& state, FILE* out) : s(state), log(out) {}
    int Run();

private:
    AppState& s;
    FILE* log;
    int failures = 0;

    void Check(bool ok, const char* label) {
        std::fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", label);
        std::fflush(log);
        failures += !ok;
    }
    app::Tab* Tab() { return ActiveTab(s); }
    void Pump(int ms) {
        const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(ms);
        do {
            s.teachTip = -1;
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            Render(s);
            Sleep(16);
        } while (GetTickCount64() < end);
    }
    static std::wstring Join(const std::vector<std::wstring>& names) {
        std::wstring out;
        for (const auto& n : names) out += (out.empty() ? L"" : L",") + n;
        return out;
    }
    static std::vector<std::wstring> Names(const app::ColumnStripColumn* col) {
        std::vector<std::wstring> out;
        if (!col || !col->rows || !col->rows_snapshot) return out;
        for (const int source : *col->rows) out.push_back((*col->rows_snapshot)[static_cast<size_t>(source)].name);
        return out;
    }
    const app::ColumnStripColumn* Ancestor(const std::wstring& leaf) {
        for (const auto& col : Tab()->column_strip.ancestors) {
            const std::wstring tail = L"\\" + leaf;
            if (col.path.size() >= tail.size() &&
                col.path.compare(col.path.size() - tail.size(), tail.size(), tail) == 0 && col.rows) return &col;
        }
        return nullptr;
    }
    bool WaitForAncestor(const std::wstring& leaf) {
        for (int i = 0; i < 200; ++i) {
            Pump(50);
            if (!Names(Ancestor(leaf)).empty()) return true;
        }
        return false;
    }
    void Note(const char* what, const std::vector<std::wstring>& names) {
        std::fprintf(log, "[INFO] %s: %s\n", what, Narrow(Join(names)).c_str());
        std::fflush(log);
    }
};

int Probe::Run() {
    wchar_t data[MAX_PATH]{};
    if (!s.isolatedTest || !GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", data, MAX_PATH)) {
        std::fprintf(log, "[ERROR] needs a --test-instance with PULSE_TEST_DATA_DIR\n");
        return 2;
    }
    const std::wstring root = std::wstring(data) + L"\\sortjump";
    const std::wstring r = root + L"\\R", p = r + L"\\b_cur", c = p + L"\\child";
    for (const auto* dir : {&root, &r, &p, &c}) CreateDirectoryW(dir->c_str(), nullptr);
    CreateDirectoryW((r + L"\\a_old").c_str(), nullptr);
    CreateDirectoryW((r + L"\\c_new").c_str(), nullptr);
    MakeFile(p + L"\\aa.txt");
    MakeFile(p + L"\\zz.txt");
    MakeFile(c + L"\\a.txt");
    MakeFile(c + L"\\b.txt");
    SetYear(p + L"\\aa.txt", 2019, false);
    SetYear(p + L"\\zz.txt", 2024, false);
    SetYear(c + L"\\a.txt", 2019, false);
    SetYear(c + L"\\b.txt", 2024, false);
    SetYear(r + L"\\a_old", 2020, true);
    SetYear(p, 2022, true);
    SetYear(r + L"\\c_new", 2024, true);

    s.appPrefs.folder_sorts.Set(r, {ui::SortColumn::Name, ui::SortDirection::Asc});
    s.appPrefs.folder_sorts.Set(p, {ui::SortColumn::Name, ui::SortDirection::Asc});
    s.appPrefs.folder_sorts.Set(c, {ui::SortColumn::Mtime, ui::SortDirection::Desc});

    // Navigation restores a folder's remembered sort and view only outside --shot runs.
    s.shot.active = false;
    NavigateTo(s, p);
    s.shot.active = true;
    ToggleColumnLayout(s, 0);
    for (int i = 0; i < 100 && !(Tab() && Tab()->column_layout && Tab()->EntryCount() >= 3); ++i) Pump(50);
    app::Tab* tab = Tab();
    Check(tab && tab->column_layout && tab->EntryCount() >= 3, "the column view opens on the fixture folder");
    if (!tab || tab->EntryCount() < 3) return 2;
    for (size_t i = 0; i < tab->EntryCount(); ++i)
        if (tab->EntryAt(i).name == L"child") tab->SelectOnly(static_cast<int>(i));
    Check(WaitForAncestor(L"R"), "the parent column is listed");
    Pump(800);
    const auto r_first = Names(Ancestor(L"R"));
    Note("R column beside P (R sorts by name)", r_first);
    Check(r_first == std::vector<std::wstring>({L"a_old", L"b_cur", L"c_new"}), "R is listed by name while P is open");

    // Press a file in the child column (what starting a drag does): the list moves into C.
    std::vector<std::wstring> child_before;
    {
        const auto child_names = Names(&Tab()->column_strip.child);
        child_before = child_names;
        Note("child column C (C sorts by modified, newest first)", child_names);
        Check(child_names == std::vector<std::wstring>({L"b.txt", L"a.txt"}),
              "the child column lists C in C's own sort (modified, newest first)");
        int row = -1;
        for (size_t i = 0; i < child_names.size(); ++i) if (child_names[i] == L"a.txt") row = static_cast<int>(i);
        Check(row >= 0, "the child column lists a.txt");
        if (row < 0) return 2;
        ui::HitTestResult hit;
        hit.region = ui::HitTestResult::ColumnStripRow;
        hit.pane_index = 0;
        hit.sub_index = ui::kColumnStripChildId;
        hit.index = row;
        ui::WindowViewModel vm = BuildVm(s, false);
        s.shot.active = false;
        HandleColumnStripMouseDown(s, vm, hit, 0);
        s.shot.active = true;
        Pump(1500);
    }
    Check(Tab()->current_path.size() >= 5 && Tab()->current_path.compare(Tab()->current_path.size() - 5, 5, L"child") == 0,
          "the list moved into C");
    std::fprintf(log, "[INFO] tab sort after entering C: column %d direction %d (Mtime=%d, Desc=1)\n",
                 static_cast<int>(Tab()->sort_column), static_cast<int>(Tab()->sort_direction),
                 static_cast<int>(ui::SortColumn::Mtime));
    {
        std::vector<std::wstring> list;
        for (size_t i = 0; i < Tab()->EntryCount(); ++i) list.push_back(Tab()->EntryAt(i).name);
        Note("C's own list (the current folder)", list);
        Check(list == child_before, "entering C leaves its rows in the order the child column showed");
    }
    const auto r_in_c = Names(Ancestor(L"R"));
    const auto p_in_c = Names(Ancestor(L"b_cur"));
    Note("R column after entering C", r_in_c);
    Note("P column after entering C", p_in_c);
    const void* p_snapshot_before = Ancestor(L"b_cur") ? Ancestor(L"b_cur")->snapshot.get() : nullptr;
    Check(r_in_c == r_first, "R keeps its own order (name) after the list moved into C");
    Check(p_in_c == std::vector<std::wstring>({L"child", L"aa.txt", L"zz.txt"}), "P is listed by its own sort (name)");

    // A move into or out of P marks it dirty; P is listed again.
    s.store.MarkDirty(p);
    s.store.MarkDirty(r);
    Pump(1500);
    const auto r_after = Names(Ancestor(L"R"));
    const auto p_after = Names(Ancestor(L"b_cur"));
    Note("R column after a re-list", r_after);
    Note("P column after a re-list", p_after);
    std::fprintf(log, "[INFO] P re-listed (new snapshot): %d\n",
                 Ancestor(L"b_cur") && Ancestor(L"b_cur")->snapshot.get() != p_snapshot_before ? 1 : 0);
    Check(r_after == r_first, "R keeps its order after it is listed again");
    Check(p_after == p_in_c, "P keeps its order after it is listed again");
    return failures ? 1 : 0;
}
} // namespace

// Explicit isolated --shot runs only.
int RunColumnSortProbe(pulse::AppState& state, const wchar_t* output) {
    FILE* log = nullptr;
    if (_wfopen_s(&log, output, L"w") || !log) return 2;
    Probe probe(state, log);
    const int result = probe.Run();
    std::fclose(log);
    return result;
}

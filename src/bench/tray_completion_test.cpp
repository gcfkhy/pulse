// Move task completion: confirmed move roots across success, partial
// cancellation, skips, merges, shutdown and the elevated helper report.
// (Written for the staging tray, which was removed; the ops checks remain.)
#include <windows.h>
#include "../ops/ops_manager.h"
#include "../ops/confirmed_move_roots.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>

int RunTrayCompletionTest() {
    namespace fs = std::filesystem;
    using namespace pulse;
    std::error_code log_error;
    fs::create_directories(L"bench_data", log_error);
    std::ofstream log(fs::path(L"bench_data/m12002_tray_completion_test.log"), std::ios::trunc);
    if (log_error || !log.is_open()) {
        std::cerr << "[FAIL] could not open bench_data/m12002_tray_completion_test.log" << std::endl;
        return 1;
    }
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
        log << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
        if (!ok) ++failures;
    };
    wchar_t temporary[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temporary)) {
        check(false, "could not obtain temporary fixture parent");
        return 2;
    }
    const auto root = fs::path(temporary) / (L"pulse_tray_completion_" +
        std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
    std::error_code ec;
    if (!fs::create_directory(root, ec) || ec) {
        check(false, "could not create exclusive temporary fixture");
        return 2;
    }
    auto write = [&](const fs::path& path) {
        fs::create_directories(path.parent_path());
        std::ofstream file(path); file << "test";
    };
    ops::OpsManager manager;
    manager.Start([] {}, false); // Private transfer worker, no shared Shell service.
    auto run = [&](ops::OpRequest request, ops::ConflictChoice choice) {
        const auto task = manager.Submit(std::move(request));
        std::vector<ops::FinishedTask> finished;
        const auto start = GetTickCount64();
        while (finished.empty() && GetTickCount64() - start < 10000) {
            if (const auto conflict = manager.PendingConflict())
                manager.ResolveConflict(conflict->token, choice, true);
            finished = manager.DrainFinishedTasks();
            if (finished.empty()) Sleep(5);
        }
        const auto completed = manager.DrainCompletions();
        check(task && finished.size() == 1 && finished[0].task_id == task,
              "real worker publishes exactly one matching terminal after success records");
        check(std::all_of(completed.begin(), completed.end(), [&](const auto& value) { return value.task_id == task; }),
              "real success mappings carry submitted task id");
        check(manager.DrainFinishedTasks().empty(), "terminal drain does not replay task");
        return finished.empty() ? ops::FinishedTask{} : std::move(finished.front());
    };
    const auto destination = root / L"destination";
    fs::create_directory(destination);
    auto move = [&](std::vector<std::wstring> sources) {
        ops::OpRequest request; request.type = ops::OpType::Move;
        request.sources = std::move(sources); request.dest_dir = destination.wstring(); return request;
    };
    const auto fast = root / L"fast.txt"; write(fast);
    auto finished = run(move({fast.wstring()}), ops::ConflictChoice::Cancel);
    check(finished.moved_sources == std::vector<std::wstring>{fast.wstring()} &&
          !fs::exists(fast) && fs::exists(destination / fast.filename()), "real atomic move confirms entire root");
    const auto first = root / L"first.txt", conflict = root / L"conflict.txt";
    write(first); write(conflict); write(destination / conflict.filename());
    finished = run(move({first.wstring(), conflict.wstring()}), ops::ConflictChoice::Cancel);
    check(finished.moved_sources == std::vector<std::wstring>{first.wstring()} && fs::exists(conflict),
          "real partial cancellation confirms moved root and retains conflicting source");
    finished = run(move({conflict.wstring()}), ops::ConflictChoice::Skip);
    check(finished.moved_sources.empty() && fs::exists(conflict), "real conflict skip confirms no root");
    const auto folder = root / L"merged";
    write(folder / L"keep.txt"); write(folder / L"move.txt"); write(destination / L"merged/keep.txt");
    finished = run(move({folder.wstring()}), ops::ConflictChoice::Skip);
    check(finished.moved_sources.empty() && fs::exists(folder / L"keep.txt") &&
          !fs::exists(folder / L"move.txt"), "partial directory merge never consumes root from child success");
    finished = run(move({folder.wstring()}), ops::ConflictChoice::Replace);
    check(finished.moved_sources == std::vector<std::wstring>{folder.wstring()} && !fs::exists(folder),
          "complete merge confirms root only after successful source directory removal");
    finished = run(move({(root / L"absent.txt").wstring()}), ops::ConflictChoice::Cancel);
    check(finished.moved_sources.empty(), "missing source failure still terminates without confirmed roots");
    manager.Stop();
    check(manager.DrainFinishedTasks().empty(), "stop does not duplicate finished tasks");

    manager.Start([] {}, false);
    const auto blocked = manager.Submit(move({conflict.wstring()}));
    const auto conflict_start = GetTickCount64();
    while (!manager.PendingConflict() && GetTickCount64() - conflict_start < 5000) Sleep(5);
    check(manager.PendingConflict().has_value(), "private conflict holds worker for queued shutdown boundary");
    const auto queued = manager.Submit(move({(root / L"never-started.txt").wstring()}));
    manager.Stop();
    const auto stopped = manager.DrainFinishedTasks();
    check(stopped.size() == 2 && stopped[0].task_id == blocked && stopped[1].task_id == queued &&
          stopped[0].moved_sources.empty() && stopped[1].moved_sources.empty(),
          "shutdown terminates active and unstarted tasks once without successful roots");
    manager.Start([] {}, false);
    Sleep(20);
    manager.Stop();
    check(manager.DrainFinishedTasks().empty(), "restarting worker cannot replay already terminated queued request");

    const auto confirmed = ops::ConfirmReportedMoveRoots({folder.wstring()}, {folder.wstring()},
                                                       {(destination / L"merged").wstring()});
    check(confirmed == std::vector<std::wstring>{folder.wstring()}, "authorized helper accepts mapped vanished root with existing target");
    check(ops::ConfirmReportedMoveRoots({folder.wstring()}, {(folder / L"keep.txt").wstring()},
              {(destination / L"merged/keep.txt").wstring()}).empty(),
          "authorized descendant-only report retains even a vanished root");
    check(ops::ConfirmReportedMoveRoots({conflict.wstring()}, {conflict.wstring()},
              {(destination / conflict.filename()).wstring()}).empty(),
          "authorized root report with remaining source is not consumed");
    check(ops::ConfirmReportedMoveRoots({folder.wstring()}, {}, {}).empty() &&
          ops::ConfirmReportedMoveRoots({folder.wstring()}, {folder.wstring()}, {(root / L"absent").wstring()}).empty(),
          "absence alone or absent destination cannot confirm authorized success");
    const auto canonical_parent = fs::weakly_canonical(root.parent_path(), ec);
    const bool safe = !ec && fs::equivalent(canonical_parent, fs::path(temporary), ec) && !ec &&
        root.filename().wstring().starts_with(L"pulse_tray_completion_");
    if (safe) fs::remove_all(root, ec);
    check(safe && !ec && !fs::exists(root), "exclusive temporary fixture cleaned up");
    if (!safe || ec) std::wcerr << L"fixture=" << root.wstring() << L" error=" << ec.value() << L'\n';
    if (!log.good()) return 1;
    return failures ? 1 : 0;
}

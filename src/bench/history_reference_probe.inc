#include "../app/app_ops_ui.h"
#include <fstream>
#include "../app/keymap_input.h"

int RunHistoryReferenceProbe(pulse::AppState& s) {
    using namespace pulse;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
        failures += !ok;
    };
    const auto output = std::filesystem::absolute(L"../bench_data/history-reference-ui").lexically_normal();
    std::filesystem::create_directories(output);
    const auto root = output / (L"fixture-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(root / L"from");
    std::filesystem::create_directories(root / L"to");
    std::ofstream(root / L"from" / L"first.txt") << "history first";
    std::ofstream(root / L"from" / L"second.txt") << "history second";
    s.appPrefs.status_hints_learned = false;
    s.appPrefs.show_hints = true;
    auto* intro_tab = ActiveTab(s);
    intro_tab->current_path = (root / L"from").wstring();
    intro_tab->loading = false;
    intro_tab->SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>());
    s.hoverRegion = ui::HitTestResult::None;
    const auto initial = BuildVm(s, false);
    check(initial.status.hint_text == l10n::Get(l10n::StringId::StatusIntroHint), "new user sees HTML onboarding hint");
    s.compositor.Resize(1100, 720); s.renderer.SetScale(1); s.compositor.RecreateTextFormats(1);
    bool initial_drawn = true;
    for (int frame = 0; frame < 32; ++frame) {
        s.compositor.Dc()->BeginDraw();
        s.renderer.Render(initial, D2D1::RectF(0,0,1100,720), ui::MakeTheme(false,ui::HexColor(0x6474C7)));
        initial_drawn = SUCCEEDED(s.compositor.Dc()->EndDraw()) && initial_drawn;
        Sleep(16);
    }
    check(initial_drawn && s.compositor.SaveSnapshot((output/L"status-new-user.png").c_str()),
        "new-user onboarding status screenshot saved");
    s.appPrefs.status_hints_learned = true;
    const auto experienced = BuildVm(s, false);
    check(experienced.status.hint_text.empty() && experienced.status.hint_action_text.empty(),
        "experienced user has no onboarding hint even without in-memory history");
    s.appPrefs.status_hints_learned = false;
    s.ops.Start([] {});
    auto wait = [&](const auto& predicate) {
        const auto deadline = GetTickCount64() + 10000;
        while (!predicate() && GetTickCount64() < deadline) Sleep(5);
        return predicate();
    };
    auto copy = [&](const wchar_t* name) {
        ops::OpRequest request; request.type = ops::OpType::Copy;
        request.sources = {(root / L"from" / name).wstring()}; request.dest_dir = (root / L"to").wstring();
        const auto id = s.ops.Submit(std::move(request));
        check(wait([&] { return s.ops.FinalStatus(id).has_value(); }), "isolated copy completes through actual operations worker");
        return id;
    };
    copy(L"first.txt"); copy(L"second.txt");
    auto* tab = ActiveTab(s); tab->current_path = (root / L"to").wstring();
    auto snapshot = std::make_shared<std::vector<fs::DirEntry>>();
    for (const auto* name : {L"first.txt", L"second.txt"}) {
        fs::DirEntry entry; entry.name = name;
        entry.full_path = (root / L"to" / name).wstring(); entry.attrs = FILE_ATTRIBUTE_NORMAL;
        snapshot->push_back(std::move(entry));
    }
    tab->SetSnapshot(snapshot); tab->loading = false; tab->view_mode = ui::ViewMode::Details;
    auto actual = BuildVm(s, false);
    const auto click_bounds = D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height()));
    const auto chip = ui::MakeStatusBarMetrics(actual, click_bounds, 1.0f, 28.0f,
        s.compositor.DwriteFactory(), s.compositor.SmallFormat());
    check(chip.task.right <= chip.task.left || chip.task.left >= chip.selection_label.right ||
        actual.status.selection_text.empty(),
        "successful copies do not leave a finished-operation pill");
    check(actual.status.hint_text.empty() && actual.status.hint_action_text.empty(),
        "completed operations retire right-side onboarding hints");
    check(actual.status.selection_text.empty(), "no selection omits status selection text");
    s.appPrefs.persist = true;
    UpdateOperationWindow(s, false);
    check(s.appPrefs.status_hints_learned, "operation update marks status onboarding learned");
    app::AppPrefs reloaded;
    check(reloaded.Load() && reloaded.status_hints_learned, "retired status onboarding persists in isolated profile");
    s.appPrefs.persist = false;
    s.compositor.Resize(1100, 720); s.renderer.SetScale(1.0f); s.compositor.RecreateTextFormats(1.0f);
    const auto actual_bounds = D2D1::RectF(0, 0, 1100, 720);
    const auto actual_theme = ui::MakeTheme(false, ui::HexColor(0x6474C7));
    for (bool selected : {false, true}) {
        if (selected) tab->SelectOnly(0); else tab->ClearSelection();
        auto state = BuildVm(s, false);
        check(selected ? state.status.selection_text == L"已选中 1 项" : state.status.selection_text.empty(),
            "HTML selection count appears only for selected items");
        state.hover_region = ui::HitTestResult::None;
        bool drawn = true;
        for (int frame = 0; frame < 32; ++frame) {
            s.compositor.Dc()->BeginDraw(); s.renderer.Render(state, actual_bounds, actual_theme);
            drawn = SUCCEEDED(s.compositor.Dc()->EndDraw()) && drawn;
            Sleep(16);
        }
        check(drawn && s.compositor.SaveSnapshot((output / (selected ? L"status-selected.png" : L"status-unselected.png")).c_str()),
            "actual status bar selected and unselected screenshots saved");
    }
    tab->ClearSelection();
    check(app::RouteKey(app::Keymap{}, app::KeyChord{app::kKeyCtrl | app::kKeyShift, 'Z'}, 0).kind == app::KeyRoute::Pass,
        "default keymap preserves Ctrl+Shift+Z instead of mapping it to Undo");
    s.ops.Stop();
    std::error_code cleanup_error; std::filesystem::remove_all(root, cleanup_error);
    check(!cleanup_error, "isolated real-operation fixture cleaned");
    for (int width : {360, 640, 1100}) for (bool query : {false, true}) {
        ui::WindowViewModel vm;
        vm.status.query_active = query; vm.status.task_is_update = !query;
        vm.status.query_text = vm.status.task_text = L"正在处理文件并更新结果";
        const auto bounds = D2D1::RectF(0, 0, static_cast<float>(width), 640);
        const auto metrics = ui::MakeStatusBarMetrics(vm, bounds, 1, 28,
            s.compositor.DwriteFactory(), s.compositor.SmallFormat());
        check(metrics.task.right <= width && metrics.task.left >= 0,
            "search/update progress stays inside narrow window");
    }
    for (float scale : {1.0f, 1.5f}) for (int width : {360, 640, 1100}) for (bool selected : {false, true}) {
        s.compositor.RecreateTextFormats(scale);
        ui::WindowViewModel vm; vm.status.ai_state = ui::StatusAiState::Idle;
        vm.status.status_text = L"24 个项目"; vm.status.selection_text = selected ? L"已选中 2 项" : L"";
        vm.status.hint_text = L"空格 快速预览";
        const auto bounds = D2D1::RectF(0, 0, width * scale, 720 * scale);
        const auto m = ui::MakeStatusBarMetrics(vm, bounds, scale, 28 * scale,
            s.compositor.DwriteFactory(), s.compositor.SmallFormat());
        check(m.status_label.left >= m.ai_chip.right &&
            (!selected || m.selection_label.left >= m.status_label.right),
            "HTML status order is AI, count, optional selection");
        check(m.status_label.right-m.status_label.left >= ui::MeasureTextWidth(s.compositor.DwriteFactory(),s.compositor.SmallFormat(),vm.status.status_text)-0.1f,
            "item count is not truncated by a history capsule");
        vm.status.task_text = L"正在复制文件"; vm.status.task_active = true;
        const auto task = ui::MakeStatusBarMetrics(vm,bounds,scale,28*scale,s.compositor.DwriteFactory(),s.compositor.SmallFormat());
        const float after_counts = selected && task.selection_label.right > task.selection_label.left
            ? task.selection_label.right : task.status_label.right;
        check(task.task.left + 0.1f >= after_counts,
            "task pill follows counts without a history capsule gap");
    }
    return failures ? 1 : 0;
}

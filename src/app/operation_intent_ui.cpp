#include "operation_intent_ui.h"
#include "app_navigation.h"
#include "create_rename_intent.h"
#include <algorithm>
#include <shlwapi.h>

namespace pulse {
void ApplyOperationIntentCompletion(AppState& state, const ops::CompletedOperation& completed) {
    // 合并到文件夹 arms its rename on the move into the new folder, so the
    // editor opens once the grouped items have left the listing.
    const bool grouped_move = completed.type == ops::OpType::Move && !completed.destinations.empty();
    if (completed.refresh_only || !completed.task_id ||
        (completed.type != ops::OpType::CreateFolder && completed.type != ops::OpType::CreateTextFile &&
         !grouped_move)) return;
    std::vector<std::wstring> created = completed.destinations;
    if (grouped_move) {
        const std::wstring& first = completed.destinations.front();
        const size_t slash = first.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return;
        created = {first.substr(0, slash)};
    }
    std::vector<std::wstring> parents;
    ForEachPane(state, [&](app::Pane& pane) {
        auto* tab = pane.ActiveTab();
        if (!tab) return;
        for (const auto& destination : created) {
            if (app::CompleteCreateRenameIntent(tab->create_rename_intents, completed.task_id,
                    tab->current_path, tab->view_generation, destination) &&
                std::find(parents.begin(), parents.end(), tab->current_path) == parents.end())
                parents.push_back(tab->current_path);
        }
    });
    // A watcher may already have delivered the new item while its owner was
    // unfocused. Request a fresh identity-checked snapshot for that owner too.
    for (const auto& parent : parents) RefreshPath(state, parent, RefreshReason::OperationCompleted);
}

void FinishOperationIntents(AppState& state, const ops::FinishedTask& finished) {
    // 解散文件夹: the chain renames, moves and recycles under one listing; its
    // last step lists the parent afresh rather than trusting the patched snapshot.
    for (auto it = state.dissolveSteps.begin(); it != state.dissolveSteps.end();) {
        if (it->task_id != finished.task_id) {
            ++it;
            continue;
        }
        RefreshPath(state, it->parent, RefreshReason::OperationCompleted);
        it = state.dissolveSteps.erase(it);
    }
    ForEachPane(state, [&](app::Pane& pane) {
        if (auto* tab = pane.ActiveTab())
            app::FinishCreateRenameIntent(tab->create_rename_intents, finished.task_id);
    });
}
}

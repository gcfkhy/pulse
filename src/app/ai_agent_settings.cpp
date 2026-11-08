// ai_agent_settings.cpp - Pulse 助手's change_settings: Pulse's settings, old -> new, with undo.
// Split from ai_assistant_agent.cpp; the shared parts are in ai_agent_internal.h.
#include "ai_agent_internal.h"

namespace pulse {
namespace ai_agent {
using app::actions::SetChoice;
using app::actions::SettingsPageTitle;

// The value as the model sees it.
json::Value SettingJson(const SetDef& d, int v) {
    if (IsSwitch(d)) return json::Value(v != 0);
    for (const SetChoice& c : d.choices)
        if (c.key && c.value == v) return json::Value(std::string(c.key));
    return json::Value(static_cast<double>(v));   // a value no choice has (an old row height)
}


struct SetChange {
    const SetDef* def = nullptr;
    int from = 0, to = 0;          // prefs values
    int theme_raw = 0;             // theme: prefs.theme_mode before (-1: the legacy session theme)
};

void ToolChangeSettings(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args) {
    AiAgent& a = Agent(s);
    AiChat& c = Chat(s);
    json::Value report = json::Value::Array();
    std::vector<SetChange> done;
    bool deny_delete = false, deny_own = false;
    const json::Value* list = args.find("changes");
    if (!list || !list->is_array() || list->size() == 0) {
        AddToolResult(a, call.id, ToolOutcome::Fail("invalid_args", "changes is empty"));
        ++a.call_index;
        NextTool(s);
        return;
    }
    HideRenameOverlay(s, true);
    FoldSteps(s, a);
    for (size_t i = 0; i < list->size() && i < 24; ++i) {
        const json::Value& ch = list->at(i);
        if (!ch.is_object()) continue;
        const json::Value* idv = ch.find("id");
        const json::Value* val = ch.find("value");
        const std::string id = idv && idv->is_string() ? idv->as_string() : std::string();
        json::Value rep = json::Value::Object();
        rep.set("id", id);
        auto fail = [&](const char* why) {
            rep.set("ok", false);
            rep.set("error", why);
            report.push(std::move(rep));
        };
        if (id == "assistant_permissions" || id == "always_allow" || id == "connections" || id == "api_key") {
            deny_own = true;
            fail("locked: only the user can change this in Pulse's settings; Pulse showed them why");
            continue;
        }
        const SetDef* d = FindSetting(id);
        if (!d) { fail("unknown setting"); continue; }
        // The value: a switch takes true / false, a choice its key (numbers
        // such as 7 may come as numbers).
        int index = -1;
        if (IsSwitch(*d)) {
            if (val && val->is_bool()) index = val->as_bool() ? 1 : 0;
            else if (val && val->is_string()) {
                const std::string& t = val->as_string();
                index = t == "true" || t == "on" ? 1 : t == "false" || t == "off" ? 0 : -1;
            }
        } else if (val) {
            std::string key = val->is_string() ? val->as_string()
                              : val->is_number() ? std::to_string(static_cast<int>(val->as_number())) : std::string();
            if (key == "mica-alt") key = "mica_alt";
            for (size_t k = 0; k < d->choices.size(); ++k)
                if (d->choices[k].key && key == d->choices[k].key) index = static_cast<int>(k);
        }
        if (index < 0) { fail("invalid value"); continue; }
        if (id == "confirm_delete" && index == 0) {
            deny_delete = true;
            fail("locked: the delete confirmation can never be turned off by the assistant; Pulse showed the user why");
            continue;
        }
        SetChange sc;
        sc.def = d;
        sc.from = d->get(s);
        sc.theme_raw = s.appPrefs.theme_mode;
        const int want = IsSwitch(*d) ? index : d->choices[static_cast<size_t>(index)].value;
        if (sc.from == want) {
            rep.set("ok", true);
            rep.set("already", true);
            rep.set("value", SettingJson(*d, want));
            report.push(std::move(rep));
            continue;
        }
        ApplySetting(s, *d, index);
        sc.to = d->get(s);
        if (sc.to != want) {
            // The setter refused (a hidden prerequisite, a failed registry write).
            if (sc.to != sc.from) ApplySetting(s, *d, ChoiceIndex(*d, sc.from));
            fail("Pulse could not change it");
            continue;
        }
        rep.set("ok", true);
        rep.set("from", SettingJson(*d, sc.from));
        rep.set("to", SettingJson(*d, sc.to));
        if (d->flags & kSetSys) rep.set("affects_windows", true);
        report.push(std::move(rep));
        done.push_back(sc);
    }
    ToolOutcome out;
    out.data = json::Value::Object();
    out.data.set("ok", !done.empty());
    out.data.set("changes", std::move(report));
    if (deny_delete || deny_own) {
        AiBlockView& e = c.Add(AiBlockKind::Error, Now());
        e.tone = ui::AiErrTone::Warn;
        e.icon = ui::AiIcon::Lock;
        e.title = Text(deny_delete ? I::AiSetDenyT : I::AiSetLockT);
        e.sub = Text(deny_delete ? I::AiSetDenyS : I::AiSetLockS);
        e.hint = Text(I::AiSetLockHint);
        e.hint_icon = ui::AiIcon::Shield;
        e.pills = {Text(I::AiSetPillPerm), Text(I::AiSetPillAllow), Text(I::AiSetPillConn)};
    }
    if (done.empty()) {
        AddToolResult(a, call.id, out);
        ++a.call_index;
        Redraw(s);
        NextTool(s);
        return;
    }
    // The pages, in the order of the changes.
    std::vector<int> pages;
    for (const SetChange& sc : done)
        if (std::find(pages.begin(), pages.end(), sc.def->page) == pages.end()) pages.push_back(sc.def->page);
    std::wstring page_names;
    for (int p : pages) page_names += (page_names.empty() ? L"" : Text(I::AiTkListSep)) + SettingsPageTitle(p);
    std::vector<std::wstring> marks;
    for (const SetChange& sc : done) marks.push_back(Text(sc.def->label));
    s.settings.SetAssistantMarks(marks);
    AddStep(s, a, ui::AiIcon::Gear, Text(I::AiSetStep),
            Fill(Fill(Text(I::AiSetStepArg), L"{page}", page_names), L"{n}", Num(done.size())), {});
    AiChatSetChip(c, ui::StatusAiState::Busy, Text(I::AiSetStep), 0.5f);
    Redraw(s);
    const uint64_t gen = a.generation;
    const std::string call_id = call.id;
    auto changes = std::make_shared<std::vector<SetChange>>(std::move(done));
    AiChatPush(s, 350, [gen, call_id, changes, pages, page_names, out](AppState& s2) mutable {
        AiAgent& ag = Agent(s2);
        if (ag.generation != gen) return -1;
        EndStep(s2, ag, true, Text(I::AiSetApplied));
        const std::wstring title =
            changes->size() == 1 ? Text(I::AiSetResTitle1) : Fill(Text(I::AiSetResTitle), L"{n}", Num(changes->size()));
        // The folded steps say what the turn did, like the card (改了 2 项设置).
        const auto steps_id = ag.steps_id;
        FoldSteps(s2, ag);
        if (AiBlockView* sb = Block(s2, steps_id)) sb->label = title;
        AiChat& c2 = Chat(s2);
        AiBlockView& r = c2.Add(AiBlockKind::Result, Now());
        r.title = title;
        r.sub = Fill(Text(I::AiSetResSub), L"{page}", page_names);
        bool fm_on = false;
        for (const SetChange& sc : *changes) {
            ui::AiChangeView v;
            v.name = Text(sc.def->label);
            v.from = SettingText(*sc.def, sc.from);
            v.to = SettingText(*sc.def, sc.to);
            if (sc.def->flags & kSetSys) v.tag = Text(I::AiSetSys);
            r.changes.push_back(std::move(v));
            fm_on |= std::string_view(sc.def->id) == "open_folders_with_pulse" && sc.to != 0;
        }
        if (fm_on) r.warn = Text(I::AiSetWarnFm);
        r.deny = Text(I::AiAsstUndo);
        r.accept = Text(I::AiSetShow);
        r.accept_icon = ui::AiIcon::Gear;
        c2.result_id = r.id;
        const int first_page = pages.front();
        c2.on_show = [first_page](AppState& st) { OpenSettingsTab(st, first_page); };
        c2.on_undo = [changes](AppState& st) {
            bool fm_back = false;
            for (auto it = changes->rbegin(); it != changes->rend(); ++it) {
                const SetDef& d = *it->def;
                const int now = d.get(st);
                if (now == it->from) continue;
                const int idx = ChoiceIndex(d, it->from);
                if (idx >= 0) ApplySetting(st, d, idx);
                else {
                    // A size off the choices (a custom row height): put the number back.
                    const std::string_view id = d.id;
                    if (id == "row_height") st.appPrefs.row_height = it->from;
                    else if (id == "font_size") st.appPrefs.ui_font_scale = it->from;
                    st.appPrefs.Save();
                    ApplySettingsEffects(st, app::SettingsEffect::RowHeight);
                }
                if (std::string_view(d.id) == "theme" && it->theme_raw < 0) {
                    st.appPrefs.theme_mode = it->theme_raw;   // the legacy session theme stays legacy
                    st.appPrefs.Save();
                }
                fm_back |= std::string_view(d.id) == "open_folders_with_pulse" && it->to != 0;
            }
            st.settings.SetAssistantMarks({});
            AiChat& c3 = Chat(st);
            c3.peek_undo = false;
            AiChatHidePeek(st);
            if (AiBlockView* rr = c3.Find(c3.result_id)) {
                rr->done = true;
                rr->title = Text(I::AiAsstUndone);
                std::wstring t = changes->size() == 1 ? Text(I::AiSetUndone1)
                                 : changes->size() == 2 ? Text(I::AiSetUndone2)
                                                        : Fill(Text(I::AiSetUndoneN), L"{n}", Num(changes->size()));
                if (fm_back) t += Text(I::AiSetUndoneFm);
                rr->sub = t;
            }
            AiBlockView& note = c3.Add(AiBlockKind::Note, Now());
            note.text = Text(I::AiSetNote);
            AiChatSetChip(c3, ui::StatusAiState::Done, Text(I::AiAsstUndone));
            if (c3.agent)
                c3.agent->pending_note =
                    L"(The user clicked Undo in Pulse: every setting of the last run_actions call is back as it was.)";
            Redraw(st);
        };
        AddToolResult(ag, call_id, out);
        ++ag.call_index;
        Redraw(s2);
        NextTool(s2);
        return -1;
    });
}
} // namespace ai_agent

} // namespace pulse

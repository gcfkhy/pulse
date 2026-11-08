#!/usr/bin/env python3
"""Checks that every action is registered everywhere the assistant needs it.

- src/app/actions/action_inventory.h links each MenuCmd (src/app/context_menu.h)
  and each kCmdGit* (src/app/git_ui.h) to an action, or gives the reason it
  has none. KeyAction coverage is checked at compile time (action_inventory.cpp).
- Every action in the inventory has a registry row (action_registry.cpp
  kDefs), or a reason in kNoRow.
- Every action that takes arguments (kRunners, kExtRunners, the file steps)
  has an argument spec (action_params.cpp kSpecs), and every spec names a
  known action.
- External actions have their own confirm words (kAskIds); only-you actions
  have a lock text (kLockIds); moves and deletes are file steps with a plan
  card (action_files.cpp FileOpOf) or have their own flow (FLOW_MOVES).
- The common actions the run_actions prompt lists exist, and the arguments
  it names are in their specs.

Reports gaps and exits 0; --strict exits 1 when anything is missing.
"""
import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
APP = ROOT / "src" / "app"


def read(path):
    return path.read_text(encoding="utf-8-sig")


def menu_commands():
    text = read(APP / "context_menu.h")
    body = text.split("enum MenuCmd", 1)[1].split("};", 1)[0]
    names = re.findall(r"^\s*(Cmd\w+)", body, re.M)
    names += re.findall(r"constexpr int (kCmdGit\w+)", read(APP / "git_ui.h"))
    return names


def inventory():
    text = read(APP / "actions" / "action_inventory.h")
    actions = re.findall(r'^\s*\{"([a-z_.]+)", ActionRisk::', text, re.M)
    body = text.split("kMenuCmdLinks[]", 1)[1].split("};", 1)[0]
    links = re.findall(r'\{(\w+), \{"([a-z_.]*)"(?:, "([^"]*)")?\}\}', body)
    return actions, links


# Moves that are not run_actions file steps: the flow tools have their cards.
FLOW_MOVES = {}


def ids_in(text, table):
    """The quoted ids of a constexpr table: {"id", ...} pairs or plain strings."""
    body = text.split(table, 1)[1].split("};", 1)[0]
    return re.findall(r'"([a-z_]+\.[a-z_]+)"', body)


def registry():
    text = read(APP / "actions" / "action_registry.cpp")
    body = text.split("kDefs[] = {", 1)[1].split("\n};", 1)[0]
    rows = re.findall(r'^\s*\{"([a-z_.]+)", I::', body, re.M)
    no_row = dict(re.findall(r'\{"([a-z_.]+)", "([^"]+)"\}', text.split("kNoRow[] = {", 1)[1].split("};", 1)[0])) \
        if "kNoRow[] = {" in text else {}
    return rows, no_row


def specs():
    text = read(APP / "actions" / "action_params.cpp")
    body = text.split("kSpecs[] = {", 1)[1].split("\n};", 1)[0]
    out = {}
    for action_id, spec in re.findall(r'\{"([a-z_.]+)", "([^"]*)"\}', body):
        out[action_id] = {part.split(":", 1)[0] for part in spec.split()}
    return out


def file_steps():
    text = read(APP / "actions" / "action_files.cpp")
    body = text.split("FileOp FileOpOf(", 1)[1].split("\n}", 1)[0]
    return set(re.findall(r'id == "([a-z_.]+)"', body))


def prompt_actions():
    """(id, arg names) the run_actions description lists with {args}."""
    text = read(APP / "ai_assistant_agent.cpp")
    start = text.index("Does things in Pulse itself")
    end = text.index("\\nSettings (settings.set", start)
    desc = text[start:end]
    out = []
    for chain, args in re.findall(r'\b((?:[a-z]+\.[a-z_]+ / )*[a-z]+\.[a-z_]+) \{([^{}]*)\}', desc):
        args = re.sub(r"\[[^\]]*\]", "", args)
        names = set()
        for part in args.split(","):
            part = part.strip()
            if not part:
                continue
            key = part.split(":", 1)[0]
            names.update(w for w in re.split(r"\s+or\s+|\s+", key) if re.fullmatch(r"[a-z_]+", w))
        out.extend((action_id, names) for action_id in chain.split(" / "))
    return out


def prompt_mentions():
    """Every action id the run_actions description names, with or without args."""
    text = read(APP / "ai_assistant_agent.cpp")
    start = text.index("Does things in Pulse itself")
    end = text.index("\\nSettings (settings.set", start)
    return set(re.findall(r'(?<![\\\w.])([a-z]+\.[a-z_]+)(?![\w.])', text[start:end]))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--strict", action="store_true", help="exit 1 on any gap")
    args = parser.parse_args()

    commands = menu_commands()
    actions, links = inventory()
    known = set(actions)
    linked = {}
    problems = []
    for command, action_id, why in links:
        if command in linked:
            problems.append(f"{command}: linked twice")
        linked[command] = action_id
        if action_id and action_id not in known:
            problems.append(f"{command}: unknown action '{action_id}'")
        if not action_id and not why:
            problems.append(f"{command}: no action and no reason")
    for command in commands:
        if command not in linked:
            problems.append(f"{command}: not linked to an action (add it to kMenuCmdLinks)")
    for command in linked:
        if command not in commands:
            problems.append(f"{command}: linked but no longer a menu command")

    # Registry rows (settings.set has one per setting, from action_settings).
    rows, no_row = registry()
    for action_id in actions:
        if action_id not in rows and action_id not in no_row:
            problems.append(f"{action_id}: no registry row (add it to kDefs, or to kNoRow with a reason)")
    for action_id in set(rows) | set(no_row):
        if action_id not in known:
            problems.append(f"{action_id}: registry row for an unknown action")
    for action_id in no_row:
        if action_id in rows:
            problems.append(f"{action_id}: in kNoRow but has a registry row")

    # Argument specs.
    spec = specs()
    args_text = read(APP / "actions" / "action_args.cpp")
    ext_text = read(APP / "actions" / "action_args_ext.cpp")
    takes_args = set(ids_in(args_text, "kRunners[] = {")) | set(ids_in(ext_text, "kExtRunners[] = {"))
    steps = file_steps()
    for action_id in sorted(takes_args | (steps - {"file.paste", "recycle.restore", "recycle.empty"})):
        if action_id not in spec:
            problems.append(f"{action_id}: has an argument runner but no spec in kSpecs (\"\" when it takes none)")
    for action_id in spec:
        if action_id not in known:
            problems.append(f"{action_id}: spec for an unknown action")

    # Confirm words, lock texts, plan cards.
    risks = dict(re.findall(r'^\s*\{"([a-z_.]+)", ActionRisk::(\w+)', read(APP / "actions" / "action_inventory.h"), re.M))
    ask = set(ids_in(ext_text, "kAskIds[] = {"))
    lock = set(re.findall(r'"([a-z_.]+)"', ext_text.split("kLockIds[] = {", 1)[1].split("};", 1)[0]))
    for action_id, risk in risks.items():
        if risk == "External" and action_id not in ask:
            problems.append(f"{action_id}: risk external but no confirm words (add them to kAskIds / AskFor)")
        if risk == "UserOnly" and action_id not in lock:
            problems.append(f"{action_id}: only the user can do it but has no lock text (kLockIds / LockFor)")
        if risk in ("Move", "Delete", "PermanentDelete") and action_id not in steps and action_id not in FLOW_MOVES:
            problems.append(f"{action_id}: risk {risk.lower()} but no plan card (a file step in FileOpOf, or FLOW_MOVES)")
    for action_id in ask - known:
        problems.append(f"{action_id}: confirm words for an unknown action")

    # The prompt's common actions.
    listed = prompt_actions()
    for action_id in sorted(prompt_mentions() - known):
        problems.append(f"prompt: {action_id} is not an action")
    for action_id, names in listed:
        if action_id not in known:
            continue
        elif action_id == "select.items" or action_id == "settings.set":
            continue
        elif action_id not in spec:
            problems.append(f"prompt: {action_id} lists arguments but has no spec")
        else:
            for name in sorted(names - spec[action_id]):
                problems.append(f"prompt: {action_id} {{{name}}} is not in its spec")

    with_action = sum(1 for c in commands if linked.get(c))
    print(f"actions {len(actions)} · registry rows {len(set(rows))} · menu commands {len(commands)} "
          f"(linked {with_action}, excluded {len(commands) - with_action - sum(c not in linked for c in commands)}) · "
          f"specs {len(spec)} · prompt actions {len(listed)}")
    for p in problems:
        print("FAIL " + p)
    if problems:
        print(f"{len(problems)} problem{'s' if len(problems) != 1 else ''}")
    else:
        print("every action is registered, described and gated")
    return 1 if problems and args.strict else 0


if __name__ == "__main__":
    sys.exit(main())

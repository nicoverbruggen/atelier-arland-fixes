#!/usr/bin/env python3
"""Keep Batch 4's hook and byte-patch rollback invariants visible in CI."""

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
HOOK_H = (ROOT / "src" / "core" / "hook_util.h").read_text()
HOOK_CPP = (ROOT / "src" / "core" / "hook_util.cpp").read_text()
GATES = (ROOT / "src" / "engines" / "phyre" / "fast_save_menu.cpp").read_text()
PAGE_PATCH = (ROOT / "src" / "core" / "page_patch.h").read_text()
MAIN = (ROOT / "src" / "core" / "main.cpp").read_text()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def main():
    try:
        for token in (
            "class HookTransaction", "TargetCollision", "DisableRollback",
            "RemoveRollback", "kMaxHooks", "kMaxPublications",
        ):
            require(token in HOOK_H, f"hook transaction header is missing {token}")
        for token in (
            "MH_RemoveHook", "clearPublications", "transaction.rollback()",
            "transaction.commit()",
        ):
            require(token in HOOK_CPP, f"hook transaction implementation is missing {token}")
        helper = re.search(
            r"bool installMinHookDetour\(.*?\n\}", HOOK_CPP, re.DOTALL
        )
        require(helper, "installMinHookDetour not found")
        helper = helper.group(0)
        require("transaction.create" in helper and "transaction.enableAll" in helper,
                "single-hook helper bypasses the transaction")
        require(helper.index("transaction.rollback") < helper.index("return false"),
                "single-hook failure returns before rollback")

        for token in ("PagePatchTransaction", "transaction.rollback()",
                      "transaction.commit()", "rollback_incomplete"):
            require(token in GATES, f"save-menu patch transaction is missing {token}")
        apply = re.search(r"bool applyGates\(.*?\n\}", GATES, re.DOTALL)
        require(apply and "transaction.applyNops" in apply.group(0),
                "save-menu apply bypasses the page-patch transaction")
        for token in ("ProtectProc", "FlushProc", "ApplyRestoreProtection",
                      "RollbackRestoreProtection", "std::memcmp",
                      "rollback()"):
            require(token in PAGE_PATCH,
                    f"page-patch transaction is missing {token}")

        # The proxy's own swap-chain and factory installers. They are outside
        # both the single-hook helper and the game-side installers, so nothing
        # above covers them.
        for token in ("MH_CreateHook", "MH_EnableHook"):
            require(token not in MAIN,
                    f"proxy installer calls {token} outside a transaction")
        require(MAIN.count("HookTransaction transaction") == 2 and
                MAIN.count("transaction.enableAll()") == 2 and
                MAIN.count("transaction.commit()") == 2,
                "the Present and factory installers are not both transactional")
        require("declineHookTransaction" in MAIN and
                "transaction.rollback()" in MAIN and
                "ROLLBACK INCOMPLETE" in MAIN,
                "proxy installer failure does not roll back and report")
        for token in ("presentHookInstalled", "createSwapChainHookInstalled",
                      "presentHookPoisoned", "createSwapChainHookPoisoned"):
            require(token in MAIN,
                    f"proxy installer state is missing {token}")
        require("if (originalPresent)\n    return;" not in MAIN and
                "if (!originalCreateSwapChain)" not in MAIN,
                "proxy installer still uses a published original as its state")
    except (OSError, ValueError) as exc:
        print(f"transaction contract check failed: {exc}", file=sys.stderr)
        return 1
    print("transaction contract ok: hook and save-menu rollback invariants hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())

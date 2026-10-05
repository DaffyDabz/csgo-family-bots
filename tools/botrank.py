"""CS:GO bot rank picker: the launcher's CS:GO page picks how good the bots are (the
owner's pick, 2026-09-25: the 18 CS:GO ranks, Silver I .. Global Elite; no pick = the bots
follow the players, as before; only the bots change, nobody's own rank).

The pick is one line in the server's test-hook file, csgo/addons/family_test.txt:
"bot_elo <elo>". family_party.cpp re-reads it on every director check (no restart) and
the bots average that elo, each with its own jitter. Other PCs cannot reach the WSL file,
so the launcher's hub service on the host PC writes it for them: GET/POST /csgo/botrank.

Elo <-> rank is the plugin's own FamilyRank(): rank 1 = Silver I at 300 and below ...
18 = Global Elite from 2400, 17 equal steps between. A picked rank sets the middle of
its step (Gold Nova I = 1103), so the jitter stays inside the rank.
"""
import os

TEST_FILE = os.environ.get(
    "FAMILY_HUB_CSGO_TEST",
    r"\\wsl.localhost\Ubuntu\opt\csgo\server\csgo\addons\family_test.txt")

RANKS = ("Silver I", "Silver II", "Silver III", "Silver IV", "Silver Elite", "Silver Elite Master",
         "Gold Nova I", "Gold Nova II", "Gold Nova III", "Gold Nova Master",
         "Master Guardian I", "Master Guardian II", "Master Guardian Elite",
         "Distinguished Master Guardian", "Legendary Eagle", "Legendary Eagle Master",
         "Supreme Master First Class", "Global Elite")
STEP = 2100.0 / 17.0


def rank_of(elo):
    """family_party.cpp FamilyRank(): 1..18."""
    return max(1, min(18, 1 + int((float(elo) - 300.0) // STEP)))


def elo_of(rank):
    """The middle of a rank's step (Global Elite: half a step above 2400)."""
    rank = int(rank)
    if not 1 <= rank <= 18:
        raise ValueError("rank 1-18")
    return int(round(300.0 + (rank - 0.5) * STEP))


def _lines(path):
    try:
        with open(path, "r", encoding="utf-8") as f:
            return f.read().splitlines()
    except FileNotFoundError:
        return []


def read(path=None):
    """{"rank": 1-18 or 0 (follow the players), "name", "elo"}."""
    elo = 0.0
    for ln in _lines(path or TEST_FILE):
        k = ln.split()
        if len(k) >= 2 and k[0] == "bot_elo":
            try:
                elo = float(k[1])          # last one wins, as in the plugin's TestValue()
            except ValueError:
                pass
    if elo <= 0:
        return {"rank": 0, "name": "", "elo": 0}
    r = rank_of(elo)
    return {"rank": r, "name": RANKS[r - 1], "elo": int(elo)}


def write(rank, path=None):
    """rank 1-18 sets "bot_elo"; 0 removes it (the bots follow the players again).
    Every other line of the file (target, rate_bots, brain_* switches) is kept."""
    path = path or TEST_FILE
    rank = int(rank)
    keep = [ln for ln in _lines(path) if not (ln.split()[:1] == ["bot_elo"])]
    if rank:
        keep.append("bot_elo %d" % elo_of(rank))
    if not keep and not os.path.exists(path):
        return read(path)
    tmp = path + ".new"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write("".join(ln + "\n" for ln in keep))
    os.replace(tmp, path)
    return read(path)

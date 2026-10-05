#!/usr/bin/env python3
"""Build the family botprofile.db + the per-bot dials from roster.txt (CS:GO step 8, step 10 2026-09-24).

  python make_botprofile.py            -> ../overlay/csgo/botprofile.db
                                          ../overlay/csgo/addons/family_roster.txt  (name elo, read by family_party.so)
                                          ../overlay/csgo/addons/family_dials.txt   (name dial offset ..., read by the brain)
  python make_botprofile.py --print    -> also prints the table of generated values
  python make_botprofile.py --dump     -> every bot's dial values (0..1) and archetype
  options: --elo <family_elo.json | none>   live elo (default: the family server's file if reachable, else roster elo)
           --out <dir>                      write <dir>/csgo/botprofile.db + <dir>/csgo/addons/family_dials.txt instead
           --override <json>                test profiles: {"all": {"aim": 0.5, ...}, "bots": {"Calvin": {"footwork": 0}}}
                                            (absolute dial values; family_dials.txt then carries "=value")

STEP 10: every bot has its own dials, not one elo curve. Ten dials, 0..1 each:
  aim footwork angles sense aware utility aggression patience teamwork economy
value = clamp(s + offset), s = smoothstep((live elo - 300) / 2100). The offset of each dial is fixed per bot forever:
sigma x N(0,1) from sha256("family-v1:<name>:<dial>") (bump "v1" to re-seed everyone). About 1 in 4 bots is a specialist
(roster column 4, else from its seed): Aimer, Brain, Rat, Entry, Nade-nerd, Anchor. Fairness: the bot's mean offset is
subtracted, so its overall strength stays its elo. Kid floor (live elo < 700): aim / footwork / awareness offsets are
capped at +0.03 and a specialist never adds aim.
Profile fields: aim -> ReactionTime, AttackDelay, AimFocus*, LookAngle* (incl. Damping); Skill <- footwork, INVERTED
(measured: on this build the stock Skill field makes a bot strafe and jump while it shoots and lose fights, see SKILL_FW);
Aggression = aggression dial minus half the patience offset; Teamwork = teamwork dial. (awareness: plugin, later batch) The plugin dials (footwork's quiet walk / stop-to-shoot; later: angles,
sense, utility, economy) read their offsets from family_dials.txt and follow the live elo themselves.
The .db is read by the server at start: a change needs a restart (csgo-ctl.sh assemble). Undo: step10 INSTALLED-step10.txt.
STEP 11 (2026-09-26): family_dials.txt also carries "react" and "spray" per bot (seeded, kid-capped, not in the fairness
mean); the plugin writes the shooting fields (ReactionTime, AttackDelay, Skill, AimFocus*, LookAngle*) into each bot's live
profile from them - botprofile.db itself is unchanged by step 11. Undo: step11 INSTALLED-step11.txt.
"""
import hashlib
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROSTER = os.path.join(HERE, "roster.txt")
OVERLAY = os.path.join(HERE, "..", "overlay")
OUT_ROSTER = os.path.join(OVERLAY, "csgo", "addons", "family_roster.txt")
LIVE_ELO = ["/opt/csgo/server/csgo/addons/family_elo.json",
            r"\\wsl.localhost\Ubuntu\opt\csgo\server\csgo\addons\family_elo.json"]

ELO_LO, ELO_HI = 300.0, 2400.0
SEED = "family-v1"
DIALS = ["aim", "footwork", "angles", "sense", "aware", "utility", "aggression", "patience", "teamwork", "economy"]
SIGMA = {"aim": 0.10, "footwork": 0.12, "angles": 0.12, "sense": 0.15, "aware": 0.12, "utility": 0.15,
         "aggression": 0.20, "patience": 0.15, "teamwork": 0.15, "economy": 0.15}
ARCHETYPES = {
    "Aimer": {"aim": 0.25, "sense": -0.15},
    "Brain": {"sense": 0.25, "aim": -0.15},
    "Rat": {"footwork": 0.30, "patience": 0.30, "aggression": -0.20},
    "Entry": {"aggression": 0.30, "footwork": -0.20},
    "Nade-nerd": {"utility": 0.30},
    "Anchor": {"angles": 0.25},
}
ARCH_ORDER = ["Aimer", "Brain", "Rat", "Entry", "Nade-nerd", "Anchor"]
KID_ELO, KID_CAP = 700.0, 0.03
# STEP 11 (2026-09-26): two shooting dials the PLUGIN applies (it writes the bot's live BotProfile; botprofile.db is
# unchanged): react = time to first shot, spray = trigger discipline + recoil control. Seeded per bot like the ten
# (own seed keys, so the ten existing offsets do not move), zero mean by construction (no fairness shift), kid floor
# (live elo < 700) capped at +0.03 as for aim.
SHOOT_DIALS = ["react", "spray"]
SHOOT_SIGMA = {"react": 0.10, "spray": 0.10}
# the aim dial reaches the aim fields through t = aim ** AIM_GAMMA (< 1 spreads the bottom: the kid-floor gate)
AIM_GAMMA = float(os.environ.get("FAMILY_AIM_GAMMA", "0.75"))

# aim fields: (field, value at aim 0, value at aim 1, log-lerp?, format)
AIM_CURVE = [
    ("ReactionTime", 0.90, 0.08, False, "%.2f"),
    ("AttackDelay", 1.00, 0.00, False, "%.2f"),
    ("AimFocusInitial", 25.0, 0.5, True, "%.2f"),
    ("AimFocusDecay", 0.8, 0.1, True, "%.2f"),
    ("AimFocusOffsetScale", 0.7, 0.05, True, "%.3f"),
    ("AimfocusInterval", 0.9, 0.05, True, "%.2f"),
    ("LookAngleMaxAccelNormal", 1200.0, 4000.0, True, "%.0f"),
    ("LookAngleStiffnessNormal", 80.0, 150.0, False, "%.0f"),
    ("LookAngleMaxAccelAttacking", 1500.0, 20000.0, True, "%.0f"),
    ("LookAngleStiffnessAttacking", 80.0, 300.0, False, "%.0f"),
]
# look damping = the spring's critical damping (2 sqrt(stiffness)) x this ratio: a low-aim bot overshoots and sways
# (under-damped), a top one settles at once (critical). Valve's default is ~1.2 x critical for every bot.
DAMP_NORMAL = (0.9, 1.2)
# STEP 10, measured (step10 logs, bot-only 30-round A/B, aim fixed 0.5): the stock Skill field makes a bot WORSE in a
# fight on this build - Skill 10 beat 50 and 50 beat 90 (it strafes and jumps while shooting, switches to the pistol).
# So Skill carries the BAD side of footwork: bottom footwork = Skill 60 (dodges while shooting, jumps on contact),
# top footwork = Skill 0 (stands still to shoot). The good side (quiet walk, stop-to-shoot) is the plugin's.
SKILL_FW = (60.0, 0.0)
SKILL_MODE = os.environ.get("FAMILY_SKILL_MODE", "footwork")
DAMP_ATTACK = (0.6, 1.0)

WEAPONS = {   # Valve's weapon templates (stock botprofile.db); Rifle/RifleT/Punch/PunchT unchanged
    "Rifle": ["m4a1", "ak47", "famas", "galilar", "mp7"],
    "RifleT": ["ak47", "m4a1", "galilar", "famas", "mp7"],
    "Punch": ["aug", "sg556", "famas", "galilar", "mp7"],
    "PunchT": ["aug", "sg556", "famas", "galilar", "mp7"],
    # STEP 10c (2026-09-25, his call): RIFLE FIRST. Measured in 10b at equal dials: shotgun/MG/SMG-template bots won 27% of
    # rounds vs rifle bots, sniper-template bots 18%; aim +0.1/+0.2 did not pay it back. The bot buys the first weapon on
    # its list it can afford, so each flavour gun now sits right after m4a1/ak47: bought only when a rifle is not
    # affordable (force buys), before the cheap famas/galil. The MG (m249 $5200) can never be the cheaper buy -> negev.
    # Sniper: AWP stays first, but the brain (family_brain.cpp KidBuy, "rifle first") holds a sniper bot's buy money
    # under the AWP price unless it is its team's one AWP this round (the richest sniper bot with >= $5750); then the
    # rifle, then famas/galil, and the scout (ssg08) only below those. The autosnipers are dropped ($5000, never cheaper).
    # Measured (step10 logs\summary-10c.txt, sniper team vs rifle bots, equal dials, 180 rounds each): 10b 18%; scout
    # right after the rifles 41%; scout after famas/galil (this) 47%; no AWP at all 53%.
    "Sniper": ["awp", "m4a1", "ak47", "famas", "galilar", "ssg08", "mp7"],
    "Power": ["m4a1", "ak47", "negev", "xm1014", "nova", "famas", "galilar", "mp7"],
    "Shotgun": ["m4a1", "ak47", "xm1014", "nova", "famas", "galilar", "mp7"],
    "Spray": ["m4a1", "ak47", "p90", "mp9", "mac10", "mp7"],
}


def clamp(v, lo=0.0, hi=1.0):
    return min(hi, max(lo, v))


def curve_t(elo):
    x = clamp((elo - ELO_LO) / (ELO_HI - ELO_LO))
    return x * x * (3 - 2 * x)


def lerp(a, b, t, log=False):
    return math.exp(math.log(a) + (math.log(b) - math.log(a)) * t) if log else a + (b - a) * t


def unit(key):
    """Two uniforms in (0, 1) from sha256(key) - the same on every machine and Python version."""
    h = hashlib.sha256(key.encode("utf-8")).digest()
    a = (int.from_bytes(h[0:8], "big") + 0.5) / 2.0 ** 64
    b = (int.from_bytes(h[8:16], "big") + 0.5) / 2.0 ** 64
    return a, b


def gauss(key):
    u1, u2 = unit(key)
    return math.sqrt(-2.0 * math.log(u1)) * math.cos(2 * math.pi * u2)


def archetype_of(name, given):
    if given:
        return None if given in ("-", "none") else given
    u, v = unit("%s:%s:arch" % (SEED, name))
    return ARCH_ORDER[int(v * len(ARCH_ORDER)) % len(ARCH_ORDER)] if u < 0.25 else None


def offsets(name, elo, arch):
    """The bot's fixed dial offsets: seeded draw + archetype, fairness (mean 0), kid floor."""
    o = {d: SIGMA[d] * gauss("%s:%s:%s" % (SEED, name, d)) for d in DIALS}
    kid = elo < KID_ELO
    for d, add in (ARCHETYPES.get(arch) or {}).items():
        if kid and d == "aim" and add > 0:
            continue   # a specialist never adds aim to a kid-level bot
        o[d] += add
    mean = sum(o.values()) / len(o)
    for d in DIALS:
        o[d] -= mean
    if kid:
        for d in ("aim", "footwork", "aware"):
            o[d] = min(o[d], KID_CAP)
    for d in SHOOT_DIALS:   # STEP 11: after the fairness mean (the ten stay exactly as they were)
        o[d] = SHOOT_SIGMA[d] * gauss("%s:%s:%s" % (SEED, name, d))
        if kid:
            o[d] = min(o[d], KID_CAP)
    return o


def dial_values(elo, off, absolute=None):
    s = curve_t(elo)
    v = {d: clamp(s + off[d]) for d in DIALS}
    if absolute:
        v.update({d: clamp(float(x)) for d, x in absolute.items()})
    return v


def profile(elo, v, off):
    out = {}
    a = v["aim"] ** AIM_GAMMA
    for name, lo, hi, log, fmt in AIM_CURVE:
        out[name] = fmt % lerp(lo, hi, a, log)
    for kind, (r0, r1) in (("Normal", DAMP_NORMAL), ("Attacking", DAMP_ATTACK)):
        k = float(out["LookAngleStiffness" + kind])
        out["LookAngleDamping" + kind] = "%.1f" % (2.0 * math.sqrt(k) * lerp(r0, r1, a))
    if SKILL_MODE == "elo":   # step-8 curve (test comparison only)
        out["Skill"] = "%d" % round(100 * curve_t(elo))
    else:
        out["Skill"] = "%d" % round(lerp(SKILL_FW[0], SKILL_FW[1], v["footwork"]))
    aggr = clamp(v["aggression"] - 0.5 * off.get("patience", 0.0))
    out["Aggression"] = "%d" % round(lerp(15, 80, aggr))
    out["Teamwork"] = "%d" % round(lerp(50, 100, v["teamwork"]))
    if elo < 800:
        out["Difficulty"], out["Cost"] = "EASY", "1"
    elif elo < 1200:
        out["Difficulty"], out["Cost"] = "NORMAL", "2"
    elif elo < 1700:
        out["Difficulty"], out["Cost"] = "HARD", "3"
    else:
        out["Difficulty"], out["Cost"] = "EXPERT", "4"
    # no "Rank" key: this build's parser rejects it ("unknown attribute 'Rank'"); the elo lives in family_roster.txt
    out["VoicePitch"] = "%d" % round(110 - 25 * curve_t(elo))
    return out


def read_roster(path=ROSTER):
    rows = []
    for line in open(path, encoding="utf-8"):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        f = line.split()
        name, tmpl, elo = f[0], f[1], int(f[2])
        arch = f[3] if len(f) > 3 else ""
        if tmpl not in WEAPONS:
            raise SystemExit("roster: unknown weapon template %r for %s" % (tmpl, name))
        if arch and arch not in ARCHETYPES and arch not in ("-", "none"):
            raise SystemExit("roster: unknown archetype %r for %s" % (arch, name))
        rows.append((name, tmpl, elo, arch))
    names = [r[0] for r in rows]
    if len(set(n.lower() for n in names)) != len(names):
        raise SystemExit("roster: duplicate bot name")
    return rows


def live_elo(arg):
    paths = [] if arg == "none" else ([arg] if arg else LIVE_ELO)
    for p in paths:
        try:
            with open(p, encoding="utf-8") as f:
                bots = json.load(f).get("bots", {})
            return {k: float(v.get("elo", 0)) for k, v in bots.items()}, p
        except (OSError, ValueError):
            continue
    return {}, "roster start elo"


def bots_table(rows, elos, override=None):
    out = []
    for name, tmpl, relo, rarch in rows:
        elo = elos.get(name, float(relo))
        arch = archetype_of(name, rarch)
        off = offsets(name, elo, arch)
        absolute = None
        if override:
            absolute = dict(override.get("all", {}))
            absolute.update(override.get("bots", {}).get(name, {}))
        fields = {}
        if absolute:   # test only: "_Skill": 50 etc. set a profile field directly
            fields = {k[1:]: v for k, v in absolute.items() if k.startswith("_")}
            absolute = {k: v for k, v in absolute.items() if not k.startswith("_")}
        v = dial_values(elo, off, {k: x for k, x in (absolute or {}).items() if k in DIALS} or None)
        for d in SHOOT_DIALS:
            v[d] = clamp(float(absolute[d])) if absolute and d in absolute else clamp(curve_t(elo) + off[d])
        prof = profile(elo, v, {} if absolute else off)
        prof.update({k: str(x) for k, x in fields.items()})
        out.append({"name": name, "tmpl": tmpl, "elo": elo, "arch": arch, "off": off, "abs": absolute or {}, "v": v, "prof": prof})
    return out


def build_db(table, source):
    L = ["// Family CS:GO botprofile.db - GENERATED by servers/csgo/bots/make_botprofile.py from roster.txt (step 10 dials).",
         "// Do not edit by hand. %d bots, live elo %.0f-%.0f from %s. Valve's original: csgo/botprofile.db in the game install."
         % (len(table), min(b["elo"] for b in table), max(b["elo"] for b in table), source),
         "",
         "Default",
         "\tSkill = 50", "\tAggression = 50", "\tReactionTime = 0.3", "\tAttackDelay = 0", "\tTeamwork = 75",
         "\tAimFocusInitial = 20", "\tAimFocusDecay = 0.7", "\tAimFocusOffsetScale = 0.30", "\tAimfocusInterval = 0.8",
         "\tWeaponPreference = none", "\tCost = 0", "\tDifficulty = NORMAL", "\tVoicePitch = 100", "\tSkin = 0",
         "\tLookAngleMaxAccelNormal = 2000.0", "\tLookAngleStiffnessNormal = 100.0", "\tLookAngleDampingNormal = 25.0",
         "\tLookAngleMaxAccelAttacking = 3000.0", "\tLookAngleStiffnessAttacking = 150.0",
         "\tLookAngleDampingAttacking = 30.0",
         "End", ""]
    for tmpl, weapons in WEAPONS.items():
        L.append("Template %s" % tmpl)
        L += ["\tWeaponPreference = %s" % w for w in weapons]
        L += ["End", ""]
    for b in table:
        # one template per bot holds its skill values, so the bot line can inherit skill + weapons
        L.append("Template E_%s" % b["name"])
        for k, v in b["prof"].items():
            L.append("\t%s = %s" % (k, v))
        L += ["End", "", "E_%s+%s %s" % (b["name"], b["tmpl"], b["name"]), "End", ""]
    return "\r\n".join(L) + "\r\n"


def build_dials(table, source):
    L = ["// family_dials.txt - GENERATED by servers/csgo/bots/make_botprofile.py (step 10). Per bot: <dial> <offset> ...",
         "// value = clamp(smoothstep((live elo - 300) / 2100) + offset); \"=v\" = an absolute test value. Seed %s, elo from %s."
         % (SEED, source),
         "// dials: " + " ".join(DIALS)]
    for b in table:
        parts = []
        for d in DIALS + SHOOT_DIALS:   # STEP 11: + react spray (an older plugin skips unknown names)
            parts.append("%s =%.3f" % (d, b["abs"][d]) if d in b["abs"] else "%s %+.3f" % (d, b["off"][d]))
        # STEP 10c: "tmpl <weapon template>" - the brain's rifle-first buy reads it (an older plugin skips the pair)
        L.append("%s %s tmpl %s   # %s elo %.0f" % (b["name"], " ".join(parts), b["tmpl"], b["arch"] or "-", b["elo"]))
    return "\n".join(L) + "\n"


def main():
    args = sys.argv[1:]

    def opt(k):
        return args[args.index(k) + 1] if k in args and args.index(k) + 1 < len(args) else None
    rows = read_roster()
    elos, source = live_elo(opt("--elo"))
    override = None
    if opt("--override"):
        with open(opt("--override"), encoding="utf-8") as f:
            override = json.load(f)
    table = bots_table(rows, elos, override)
    out = opt("--out")
    out_db = os.path.join(out or OVERLAY, "csgo", "botprofile.db")
    out_dials = os.path.join(out or OVERLAY, "csgo", "addons", "family_dials.txt")
    os.makedirs(os.path.dirname(out_dials), exist_ok=True)
    db = build_db(table, source)
    with open(out_db, "w", encoding="ascii", newline="") as f:
        f.write(db)
    with open(out_dials, "w", encoding="ascii", newline="\n") as f:
        f.write(build_dials(table, source))
    if not out:
        with open(OUT_ROSTER, "w", encoding="ascii", newline="\n") as f:
            f.write("// name elo (starting elo; generated from roster.txt by make_botprofile.py)\n")
            for name, _, elo, _a in rows:
                f.write("%s %d\n" % (name, elo))
    print("wrote %s (%d bytes, %d bots, elo from %s) and %s" % (os.path.normpath(out_db), len(db), len(rows), source,
                                                              os.path.normpath(out_dials)))
    if "--print" in args:
        keys = ["ReactionTime", "AttackDelay", "AimFocusInitial", "AimFocusOffsetScale", "LookAngleDampingAttacking", "Skill",
                "Aggression", "Teamwork", "Difficulty"]
        print("%-8s %5s %-9s " % ("bot", "elo", "type") + " ".join("%10s" % k[-10:] for k in keys))
        for b in table:
            print("%-8s %5.0f %-9s " % (b["name"], b["elo"], b["arch"] or "-") + " ".join("%10s" % b["prof"][k] for k in keys))
    if "--dump" in args:
        print("%-8s %5s %-9s " % ("bot", "elo", "type") + " ".join("%6s" % d[:6] for d in DIALS))
        for b in table:
            print("%-8s %5.0f %-9s " % (b["name"], b["elo"], b["arch"] or "-") + " ".join("%6.3f" % b["v"][d] for d in DIALS))


if __name__ == "__main__":
    main()

// family_brain: STEP 9 (2026-09-24) bot team play for the family_party plugin (family_party.cpp owns the plugin object).
//
// The stock CS:GO bots (CCSBot) pick their own goals one by one: Ts wander ("hunt") while the bomber walks alone,
// CTs guard a random site or roam, nobody rotates on information, buys are per bot. This layer gives each round a
// team plan and steers the bots' own state machine towards it, like a caller would:
//   CT  split over the sites (2-1-2 by team size), hold hiding spots that look at the site entry, rotate when a site
//       is hit, gather and retake after a plant.
//   T   gather at a stack point and hit one site together (sometimes a fake first), sometimes one lurker at another
//       site's choke, post-plant holds around the bomb.
//   all trade a teammate's death, fall back / regroup when outnumbered, buy as a team (eco / force / full).
//   hostage maps: CTs push in (the stock AI collects and rescues) and escort the carrier, T guard the hostages and
//       hold there; no trade runs or fall-backs there (STEP 9c).
// Elo scales it: smarts s = (elo - 300) / 2100. A bot plays its basic part (hold, group, post-plant) with chance
// 0.6 + 0.4 s and an advanced one (retake gather) with 0.3 + 0.8 s, holds the best spot
// only when s is high, rotates after 6 s .. 1 s, trades with chance 0.15 + 0.75 s; lurks, fakes, waits for the
// group and buys as a team only with a smart team. An elo-300 bot is left to the stock AI most of the time.
//
// The map points come from csgo/addons/family_maps/<map>.txt (bots/make_mapinfo.py, derived from each map's .nav and
// entities - no hand-made tables). No map file = stock bots.
//
// Levers (measured on the 1.38.8.1 Linux server.so, no symbols; the bot states are found by their RTTI names):
//   CCSBot embeds its states (IdleState, MoveToState, HideState, ...) and keeps m_state + m_stateTimestamp. MoveTo =
//   write MoveToState's goal/route and switch state (OnExit old, OnEnter new: the bot computes its own path);
//   hold = HideState with the bot's current nav area, spot, duration, hold flag. The layout is re-verified by RTTI on
//   every level; any mismatch switches steering off for that level (stock bots, logged). Team buy = the bot's money
//   (m_iAccount, offset found each level by matching the server log's money lines) is lowered during the freeze of an
//   eco round and given back after the freeze. Nothing is steered while a bot fights, plants, defuses, fetches the
//   bomb, buys, opens a door or escapes; humans are never touched.
// STEP 9b (2026-09-24): rotations walk make_mapinfo's CT-side routes ("rot" waypoints) and start on an earlier call
// (a T on the site with another close behind), the anchor follows late instead of staying, rotators hold the back of
// the hit site and regroup there when outnumbered;
// the retake gather waits for the group (bounded by the bomb clock) and is no longer cut short by the stock
// find-the-bomb task; the T group closes up at the site entrance before going in.
// Metrics: one "RND ..." line per round in csgo/addons/family_brain.log (also with steering off, for before/after).
// Test hooks (addons/family_test.txt): brain_steer 0 = metrics only; brain_diag 1 = per-bot dump every 3 s.
#include <fcntl.h>
#include <sys/stat.h>
#include <strings.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "family_logic.h"   // STEP 12: the pure decision rules (unit-tested in test_pick.cpp)

struct edict_t;

static const char *kBrainLog = "csgo/addons/family_brain.log";
static const char *kMapDir = "csgo/addons/family_maps";
static void BLog(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (FILE *f = fopen(kBrainLog, "a"))
    {
        time_t t = time(nullptr);
        char ts[32];
        strftime(ts, sizeof(ts), "%m-%d %H:%M:%S", localtime(&t));
        fprintf(f, "%s %s\n", ts, buf);
        // a server that never changes level (mp_match_end_restart) never reaches the 4 MB turn-over in BrainLevelInit:
        // every 500 lines, over 16 MB it becomes family_brain.log.old here too (a test match never gets near that)
        static unsigned lines;
        long n = ++lines % 500 == 0 ? ftell(f) : 0;
        fclose(f);
        if (n > 16L * 1024 * 1024) rename(kBrainLog, (std::string(kBrainLog) + ".old").c_str());
    }
}

template <typename Fn> static Fn VF(void *obj, int index) { return reinterpret_cast<Fn>((*reinterpret_cast<void ***>(obj))[index]); }

// ---- safe memory reads: write() from the address into a pipe fails with EFAULT instead of crashing ----
static int g_pipe[2] = {-1, -1};
static bool SafeRead(const void *addr, void *out, size_t n)
{
    if ((uintptr_t)addr < 0x10000)
        return false;
    if (g_pipe[0] < 0)
    {
        if (pipe(g_pipe) != 0)
            return false;
        fcntl(g_pipe[0], F_SETFL, O_NONBLOCK);
        fcntl(g_pipe[1], F_SETFL, O_NONBLOCK);
    }
    ssize_t w = write(g_pipe[1], addr, n);
    if (w != (ssize_t)n)
    {
        if (w > 0)
        {
            char tmp[256];
            while (read(g_pipe[0], tmp, sizeof(tmp)) > 0) {}
        }
        return false;
    }
    return read(g_pipe[0], out, n) == (ssize_t)n;
}
static bool SafeWord(uintptr_t addr, uintptr_t &out) { return SafeRead((void *)addr, &out, 4); }
static std::string SafeStr(uintptr_t addr, size_t max = 48)
{
    std::string s;
    char c;
    for (size_t i = 0; i < max && SafeRead((void *)(addr + i), &c, 1) && c; i++)
        s += c;
    return s;
}
// RTTI class name of a vtable pointer (vtable[-1] = typeinfo; typeinfo+4 = mangled name)
static std::string VtName(uintptr_t vt)
{
    uintptr_t ti, nm;
    if (!SafeWord(vt - 4, ti) || !SafeWord(ti + 4, nm))
        return "";
    return SafeStr(nm);
}

// ---- small maths ---- (STEP 12: the vector and its distance live in family_logic.h, shared with the unit tests)
typedef fl::V3 Vec;
using fl::Dist;
static float Rnd() { return (float)rand() / (float)RAND_MAX; }
static float Lerp(float a, float b, float t) { return a + (b - a) * t; }

// ---- engine plumbing handed over by family_party.cpp ----
static void *(*g_pi)(edict_t *);
static double (*g_eloOf)(const char *);
static void *g_globals;   // CGlobalVars: +16 curtime
static float CurTime() { return g_globals ? *(float *)((char *)g_globals + 16) : 0.f; }
static const char *PI_Name(void *pi) { return VF<const char *(*)(void *)>(pi, 0)(pi); }
static int PI_Team(void *pi) { return VF<int (*)(void *)>(pi, 3)(pi); }
static bool PI_Dead(void *pi) { return VF<bool (*)(void *)>(pi, 12)(pi); }
static bool PI_Observer(void *pi) { return VF<bool (*)(void *)>(pi, 14)(pi); }
static Vec PI_Origin(void *pi) { return VF<Vec (*)(void *)>(pi, 15)(pi); }

// ---- measured CCSBot layout (1.38.8.1 Linux server.so, STEP 9 diag 2026-09-24), re-verified by RTTI per level ----
enum
{
    kOffIdle = 0x3cc4, kOffMoveTo = 0x3d4c, kOffHide = 0x3d78,
    kOffState = 0x3e98, kOffStateTime = 0x3e9c, kOffTask = 0x3ea4, kOffArea = 0x3ec4,
    kStateLo = 0x3cc4, kStateHi = 0x3e98,   // the embedded states live in [lo, hi)
};
static int g_layout;   // 0 unknown, 1 verified, -1 mismatch (steering off)

static void *BotEntity(edict_t *e)
{
    uintptr_t unk, vt;
    if (!SafeWord((uintptr_t)e + 12, unk) || !unk || !SafeWord(unk, vt))
        return nullptr;
    static uintptr_t botVt;   // the CCSBot vtable, once seen (class names are not re-read every tick)
    if (botVt && vt == botVt)
        return (void *)unk;
    if (VtName(vt) == "6CCSBot")
    {
        botVt = vt;
        return (void *)unk;
    }
    return nullptr;
}
static bool VerifyLayout(uintptr_t b)
{
    uintptr_t v1, v2, v3, st;
    bool ok = SafeWord(b + kOffIdle, v1) && SafeWord(b + kOffMoveTo, v2) && SafeWord(b + kOffHide, v3) && SafeWord(b + kOffState, st) &&
              VtName(v1) == "9IdleState" && VtName(v2) == "11MoveToState" && VtName(v3) == "9HideState" &&
              (st == 0 || (st >= b + kStateLo && st < b + kStateHi)) &&
              *(uint8_t *)(b + kOffStateTime + 4) <= 1 && *(uint8_t *)(b + kOffStateTime + 5) <= 1;   // STEP 10: m_isAttacking, m_isOpeningDoor
    BLog("layout check on %p: %s", (void *)b, ok ? "OK (steering on)" : "MISMATCH - steering off for this level");
    return ok;
}
static std::string StateName(uintptr_t b)
{
    uintptr_t st = *(uintptr_t *)(b + kOffState);
    if (st < b + kStateLo || st >= b + kStateHi)
        return "";
    static std::map<uintptr_t, std::string> cache;   // state vtable -> name
    uintptr_t vt = *(uintptr_t *)st;
    auto it = cache.find(vt);
    if (it != cache.end())
        return it->second;
    return cache[vt] = VtName(vt);
}
// CCSBot::m_task (measured: CTs 14 COLLECT_HOSTAGES / 15 RESCUE_HOSTAGES, Ts 16 GUARD_HOSTAGES, 8, 17, 18 as in the
// CS:S enum). The bot's own objective work is never overridden: 1 plant (once the T hit is on), 2 find the ticking bomb,
// 3 defuse, 5 guard the defuser, 9 escape the bomb, 14 collect hostages, 15 rescue hostages.
static int BotTask(uintptr_t b)
{
    int t = *(int *)(b + kOffTask);
    return t >= 0 && t < 40 ? t : -1;
}
// STEP 10: CCSBot::m_isAttacking (the byte after m_stateTimestamp, measured: 1 on every shot a bot fires). A fight is
// not a state change here: the bot keeps its state (MoveTo, Hide, ...) and runs AttackState's update beside it.
static bool BotAttacking(uintptr_t b) { return *(uint8_t *)(b + kOffStateTime + 4) == 1; }
static bool ObjectiveTask(int t, bool plantToo) { return t == 2 || t == 3 || t == 5 || t == 9 || t == 14 || t == 15 || (plantToo && t == 1); }
// BotState vtable: 0 OnEnter(bot), 1 OnUpdate(bot), 2 OnExit(bot), 3 GetName()  (same order as CCSBot::SetState)
static void SetBotState(uintptr_t b, uintptr_t ns)
{
    uintptr_t old = *(uintptr_t *)(b + kOffState);
    if (old >= b + kStateLo && old < b + kStateHi)
        VF<void (*)(void *, void *)>((void *)old, 2)((void *)old, (void *)b);
    VF<void (*)(void *, void *)>((void *)ns, 0)((void *)ns, (void *)b);
    *(uintptr_t *)(b + kOffState) = ns;
    *(float *)(b + kOffStateTime) = CurTime();
}
static void BotMoveTo(uintptr_t b, const Vec &goal, int route)   // route: 1 fastest, 2 safest
{
    uintptr_t st = b + kOffMoveTo;
    memcpy((void *)(st + 4), &goal, 12);
    *(int *)(st + 16) = route;
    SetBotState(b, st);
}
static bool BotHideHere(uintptr_t b, const Vec &spot, float duration)
{
    uintptr_t area = *(uintptr_t *)(b + kOffArea);
    if (!area)
        return false;
    uintptr_t st = b + kOffHide;
    *(uintptr_t *)(st + 4) = area;        // m_searchFromArea
    *(float *)(st + 8) = 300.f;           // m_range
    memcpy((void *)(st + 12), &spot, 12); // m_hidingSpot
    *(float *)(st + 28) = duration;       // m_duration
    *(uint8_t *)(st + 44) = 1;            // m_isHoldingPosition
    SetBotState(b, st);
    return true;
}

// ---- map points ----
// STEP 12J: a T approach to a site (make_mapinfo appr / apwp / apstk / apchk / apent lines): the way in by name, its
// waypoints from the T spawn to its stack, and its own stack / choke / entry; flags 1 = through a *mid* place
struct Approach
{
    std::string name;
    int flags = 0;
    float len = 0;
    std::vector<Vec> wp;
    Vec stack{}, choke{}, entry{};
    bool ok = false;
};
struct Site
{
    std::string label;
    Vec c{}, entry{}, choke{}, stack{}, retake{}, lurk{};
    float radius = 450;
    std::vector<Vec> hold, post;
    std::vector<Approach> ap;   // STEP 12J (an old map file: none - approach 0 is made from stack / choke / entry)
};
struct MidArea   // STEP 12J: a nav area of a *mid* place
{
    std::string place;
    float x0, y0, x1, y1, z;
};
struct MapInfo
{
    bool ok = false;
    bool hostage = false;
    Vec tspawn{}, ctspawn{}, mid{}, rescue{};
    bool hasMid = false;
    std::vector<Site> sites;
    std::map<std::string, std::vector<Vec>> rot;   // STEP 9b: "from>to" -> CT rotation waypoints (from = site or mid)
    // STEP 10: earliest-occupy grid (make_mapinfo occ_grid): [0] T, [1] CT, 1/10 s, 1200 = never
    float ox = 0, oy = 0, ocell = 0;
    int onx = 0, ony = 0;
    std::vector<uint16_t> occ[2];
    std::vector<std::pair<Vec, int>> spots;   // STEP 10b: nav hiding spots + flags (1 IN_COVER, 8 EXPOSED)
    std::vector<MidArea> midAreas;            // STEP 12J
};
static MapInfo g_mi;
static std::string g_mapName;
static Site *FindSite(const std::string &l)
{
    for (auto &s : g_mi.sites)
        if (s.label == l)
            return &s;
    return nullptr;
}
static void LoadMapInfo(const std::string &map)
{
    g_mi = MapInfo();
    std::string p = std::string(kMapDir) + "/" + map + ".txt";
    FILE *f = fopen(p.c_str(), "r");
    if (!f)
    {
        BLog("no map points for %s (%s): stock bots on this map", map.c_str(), p.c_str());
        return;
    }
    static char line[8192];   // STEP 10: occ rows are long
    while (fgets(line, sizeof(line), f))
    {
        char k[32] = "", l[32] = "";
        Vec v{};
        if (line[0] == '/' || sscanf(line, "%31s", k) != 1)
            continue;
        if (!strcmp(k, "occgrid"))
        {
            if (sscanf(line, "%*s %f %f %f %d %d", &g_mi.ox, &g_mi.oy, &g_mi.ocell, &g_mi.onx, &g_mi.ony) == 5 && g_mi.ocell > 0 &&
                g_mi.onx > 0 && g_mi.ony > 0 && g_mi.onx * g_mi.ony < 400000)
                for (int t = 0; t < 2; t++)
                    g_mi.occ[t].assign(g_mi.onx * g_mi.ony, 1200);
            else
                g_mi.onx = g_mi.ony = 0;
            continue;
        }
        if (!strcmp(k, "occ"))
        {
            int row, n = 0, t;
            char *p = line + 3;
            if (!g_mi.onx || sscanf(p, "%31s %d%n", l, &row, &n) != 2 || row < 0 || row >= g_mi.ony)
                continue;
            t = !strcmp(l, "CT") ? 1 : 0;
            p += n;
            for (int x = 0; x < g_mi.onx; x++)
            {
                char *e;
                long val = strtol(p, &e, 10);
                if (e == p) break;
                g_mi.occ[t][row * g_mi.onx + x] = (uint16_t)std::max(0L, std::min(1200L, val));
                p = e;
            }
            continue;
        }
        if (!strcmp(k, "spot"))   // STEP 10b
        {
            int fl;
            if (sscanf(line, "%*s %f %f %f %d", &v.x, &v.y, &v.z, &fl) == 4 && g_mi.spots.size() < 20000)
                g_mi.spots.push_back({v, fl});
            continue;
        }
        if (!strcmp(k, "kind"))
        {
            g_mi.hostage = strstr(line, "hostage") != nullptr;
            continue;
        }
        // STEP 12J: T approaches + mid areas (an older plugin skips these lines)
        if (!strcmp(k, "appr") || !strcmp(k, "apwp") || !strcmp(k, "apstk") || !strcmp(k, "apchk") || !strcmp(k, "apent"))
        {
            int ai = -1, fl = 0;
            float len = 0;
            char nm[64] = "";
            Site *s = nullptr;
            if (!strcmp(k, "appr"))
            {
                if (sscanf(line, "%*s %31s %d %63s %d %f", l, &ai, nm, &fl, &len) != 5 || !(s = FindSite(l)) || ai < 0 || ai > 7) continue;
            }
            else if (sscanf(line, "%*s %31s %d %f %f %f", l, &ai, &v.x, &v.y, &v.z) != 5 || !(s = FindSite(l)) || ai < 0 || ai > 7)
                continue;
            if ((int)s->ap.size() <= ai) s->ap.resize(ai + 1);
            Approach &a = s->ap[ai];
            if (!strcmp(k, "appr")) { a.name = nm; a.flags = fl; a.len = len; }
            else if (!strcmp(k, "apwp")) { if (a.wp.size() < 64) a.wp.push_back(v); }
            else if (!strcmp(k, "apstk")) { a.stack = v; a.ok = true; }
            else if (!strcmp(k, "apchk")) a.choke = v;
            else a.entry = v;
            continue;
        }
        if (!strcmp(k, "midarea"))
        {
            MidArea m;
            char nm[64] = "";
            if (sscanf(line, "%*s %63s %f %f %f %f %f", nm, &m.x0, &m.y0, &m.x1, &m.y1, &m.z) == 6 && g_mi.midAreas.size() < 1000)
            {
                m.place = nm;
                g_mi.midAreas.push_back(m);
            }
            continue;
        }
        if (!strcmp(k, "tspawn") || !strcmp(k, "ctspawn") || !strcmp(k, "mid") || !strcmp(k, "rescue"))
        {
            if (sscanf(line, "%*s %f %f %f", &v.x, &v.y, &v.z) != 3)
                continue;
            if (!strcmp(k, "tspawn")) g_mi.tspawn = v;
            else if (!strcmp(k, "ctspawn")) g_mi.ctspawn = v;
            else if (!strcmp(k, "rescue")) g_mi.rescue = v;
            else { g_mi.mid = v; g_mi.hasMid = true; }
            continue;
        }
        if (!strcmp(k, "radius"))
        {
            float r;
            if (sscanf(line, "%*s %31s %f", l, &r) == 2)
                if (Site *s = FindSite(l))
                    s->radius = r;
            continue;
        }
        if (!strcmp(k, "rot"))
        {
            char to[32] = "";
            if (sscanf(line, "%*s %31s %31s %f %f %f", l, to, &v.x, &v.y, &v.z) == 5)
                g_mi.rot[std::string(l) + ">" + to].push_back(v);
            continue;
        }
        if (sscanf(line, "%*s %31s %f %f %f", l, &v.x, &v.y, &v.z) != 4)
            continue;
        if (!strcmp(k, "site"))
        {
            Site s;
            s.label = l;
            s.c = s.entry = s.choke = s.stack = s.retake = s.lurk = v;
            g_mi.sites.push_back(s);
            continue;
        }
        Site *s = FindSite(l);
        if (!s) continue;
        if (!strcmp(k, "entry")) s->entry = v;
        else if (!strcmp(k, "choke")) s->choke = v;
        else if (!strcmp(k, "stack")) s->stack = v;
        else if (!strcmp(k, "retake")) s->retake = v;
        else if (!strcmp(k, "lurk")) s->lurk = v;
        else if (!strcmp(k, "hold")) s->hold.push_back(v);
        else if (!strcmp(k, "post")) s->post.push_back(v);
    }
    fclose(f);
    // STEP 12J: every site has at least its approach 0 (an old map file: step 9's stack / choke / entry, no waypoints,
    // "mid" when its stack is within 600 u of the map's mid point); an approach without its stack line is dropped
    int naps = 0;
    for (auto &s : g_mi.sites)
    {
        std::vector<Approach> keep;
        for (auto &a : s.ap)
            if (a.ok) keep.push_back(a);
        s.ap = keep;
        if (s.ap.empty())
        {
            Approach a;
            a.name = "main";
            a.stack = s.stack;
            a.choke = s.choke;
            a.entry = s.entry;
            a.flags = g_mi.hasMid && Dist(s.stack, g_mi.mid) < 600.f ? 1 : 0;
            a.ok = true;
            s.ap.push_back(a);
        }
        naps += (int)s.ap.size();
    }
    g_mi.ok = !g_mi.sites.empty();
    BLog("map points %s: %s, %d site(s)%s, %d rotation route(s), occupy grid %dx%d, %d hiding spots, %d T approaches, %d mid areas", map.c_str(),
         g_mi.hostage ? "hostage" : "bomb", (int)g_mi.sites.size(), g_mi.hasMid ? ", mid" : "", (int)g_mi.rot.size(), g_mi.onx, g_mi.ony,
         (int)g_mi.spots.size(), naps, (int)g_mi.midAreas.size());
    for (auto &s : g_mi.sites)
    {
        std::string a;
        for (size_t i = 0; i < s.ap.size(); i++)
            a += (i ? ", " : "") + s.ap[i].name + (s.ap[i].flags & 1 ? " (mid)" : "") + " " + std::to_string(s.ap[i].wp.size()) + "wp";
        BLog("map points %s: site %s approaches: %s", map.c_str(), s.label.c_str(), a.c_str());
    }
}

// ---- players ----
enum TaskKind { T_NONE, T_MOVE, T_HOLD };
struct Task
{
    TaskKind kind = T_NONE;
    Vec goal{};
    int route = 2;
    float arrive = 150;      // T_MOVE: done inside this distance; T_HOLD: hide once inside
    float holdFor = 40;      // T_HOLD hide duration (renewed while the task lasts)
    float startAt = 0;       // not before this time (rotation delays)
    std::string why;
    std::vector<Vec> via;    // STEP 9b: waypoints walked first (CT rotations over the CT side of the map)
    size_t wp = 0;
};
struct Player
{
    std::string name;
    int team = 0;
    bool alive = false, bot = false;
    Vec pos{};
    uintptr_t ent = 0;
    float s = 0;             // smarts 0..1 from elo
    edict_t *e = nullptr;
    Task task;
    float issuedAt = -100, progressAt = 0, bestDist = 1e9f;
    Vec issuedGoal{};
    Vec snapGoal{};          // JOB 5c Q / T: the MoveTo state's own goal right after our issue (the nav's snap of it)
    int issues = 0;
    std::string role;
    // STEP 12C: defenders - the anchor of its site this round; last round's site / spot (kept across rounds: a player
    // plays "his" site and varies the angle); a noise episode's chase-or-hold roll
    struct Ct
    {
        bool anchor = false;
        std::string lastSite;
        Vec lastSpot{};
        float invAt = -1;
        bool invChase = false;
        Vec setSpot{};           // JOB 5c Q: this round's setup spot and role (the "at freeze end + 20 s" proof)
        std::string setRole;
        bool hand = false;       // JOB 5d: this round's spot is one he placed (face his aim, not a worked-out one)
        float handP = 0, handY = 0;
    } ct;
    // JOB 5c U: the way in a holder faces at its spot (worked out once per spot), this round's counts
    struct Fc
    {
        Vec tgt{}, goal{0, 0, -1e9f};
        bool have = false;
        float holdT = 0, okT = 0, next = 0;
        float moveT = 0, walkT = 0, spotMoveT = 0, spotWalkT = 0;
    } fc;
    // JOB 5b O: utility - what it carries, the throw in progress, this round's counts
    struct Ut
    {
        int phase = 0;                  // 0 idle, 1 equip ("use"), 2 aim, 3 pin pulled (the game throws it)
        int kind = -1, tries = 0, steady = 0;
        std::string job, item;
        Vec target{}, land{}, waitAt{}, solvedFrom{};
        float waitR = 0, pitch = 0, yaw = 0, miss = 0, errP = 0, errY = 0;
        float at = 0, nextTry = 0, equipAt = -1, pinAt = -1, relAt = -1, deadline = 0, nextInv = 0, nextRoll = 0;
        bool forced = false, haveOk = false, bought = false;
        float cool = 0, awayUntil = 0;   // no new job before `cool` (after an abort); turned away from our flash until `awayUntil`
        float backAt = -1;               // "lastinv" sent (the gun back after an abort): checked 0.6 s on
        Vec awayFrom{};
        int have[fl::UK_N] = {};
        int startHave = -1, threw = 0, aborted = 0, buys = 0;
    } ut;
    // JOB 5b P: movement in fights - the cycle, the fight in progress (metrics on and off alike), this round's counts
    struct Mv
    {
        fl::Cyc cyc;
        bool mover = false, inFight = false;
        float lastT = -100, rollAt = -100, fightAt = -1, lastAtk = -100, shotSeen = -100, nextSeen = 0, fallAt = -100, repoAt = -100, seenEp = -100;
        float fMove = 0, fStop = 0;
        int shots0 = 0, hits0 = 0, kills0 = 0, seenBy = 0;
        int fights = 0, moverFights = 0, cycles = 0, repos = 0, falls = 0, seen2 = 0, still = 0;
        float moveT = 0, fightT = 0;
    } mv;
    // JOB 5b Q: crouching - what we want this frame, what we wrote, the hold's roll, this round's counts (on and off)
    struct Cr
    {
        int want = 0;            // +1 crouch, -1 stand, 0 the stock bot decides
        int wrote = -1;          // the last value we wrote to its crouch flag (-1 none)
        bool spray = false;      // this fight's bursts crouched
        bool holdCrouch = false;
        Vec holdGoal{0, 0, -1e9f};
        float fightDuck = 0, holdT = 0, holdDuckT = 0;
        int sprays = 0, crouchHolds = 0, standHolds = 0;
    } cr;
    // JOB 5b N: the rotation / retake in progress (metrics on and off alike: ROT line)
    struct Rt
    {
        bool on = false, bomb = false, inFight = false, sneaky = false;
        std::string why, to;
        float at = 0, startAt = 0, stall = 0;
        int fights = 0;
    } rt;
    // STEP 12E: player habits - pre-aim while moving, jiggle / shoulder peeks, counter-strafe at first sight
    struct Pk
    {
        int phase = 0;              // jiggle: 1 out, 2 exposed, 3 back, 4 in cover
        Vec P{}, Q{}, tgt{};
        int left = 0, peeksDone = 0;
        float until = 0, reissue = 0, out = 0.2f, back = 0.6f, cool = -100, nextLook = 0, csUntil = -100, lastPosT = -100, startAt = 0, phaseAt = 0;
        Vec lastPos{};
        float moveYaw = 0;
        bool moving = false, contact = false, entranceDone = false;
        std::string why, task;
        std::map<int, float> checked;   // nav spot -> when it was pre-aimed
        int preaims = 0, jiggles = 0, peeks = 0, contacts = 0, cs = 0;
    } pk;
    std::string weapon;         // STEP 12E/F: the weapon in its hands (item_equip), humans too
    float nadeNext = 0, nadeVetoAt = -100, nadeVetoUntil = -100;   // STEP 12F9: the grenade check
    float nadeYaw = 0, nadePitch = 1000;                           // its view at the last check (steady = aimed)
    int nadeBad = 0, nadeVetoes = 0;
    std::string seenName;       // STEP 12K: the enemy it saw last (in its view, clear line)
    float seenNameAt = -100;
    int unholds = 0;
    // STEP 12D: this round's T plan group (1 main, 2 the split's second way) and approach
    struct T12
    {
        int group = 0, ap = 0;
    } t12;
    // STEP 10: footwork (updated every frame) and per-round counters
    int uid = -1;
    float fw = 0, aim = 0, elo = 0;             // this round's dial values
    bool walker = false;                        // quiet-walks this round (rolled from the footwork dial)
    Vec lastPos{};
    float lastT = -100, speed = 0;
    bool inAttack = false, stopper = false, modOn = false, walking = false;
    float attackAt = 0, stopUntil = 0, wrote = 1, tagVal = 1, tagAt = -100, lastFire = -100, fsAt = -100;
    bool fsOpen = false, fsMoving = false, reactOpen = false;
    // STEP 10b: safe router this round (rolled from footwork), jump + stuck + route tracking
    bool safe = false, jumpOpen = false, stuckOn = false;
    std::string lastSt;
    float jumpAt = -100, jumpZ = 0, jumpSp = 0, slowSince = -1, safeAt = -100;
    // STEP 11: shooting (profile writes, sight / engagement metrics, panic, lucky first shot)
    struct Sh
    {
        uintptr_t prof = 0;                   // the bot's BotProfile (CBot::m_profile)
        int state = 0;                        // 0 unchecked, 1 verified (ours to write), -1 layout mismatch (never touched)
        float react = 0, spray = 0, aimd = 0; // this round's dials
        float sightAt = -100, lastSeen = -100, closeAt = -100, nextScan = 0, closeDist = 0;
        Vec closePos{};
        bool lucky = false, panic = false, eng = false, engPanic = false, engLucky = false, fsDone = false, fsHitOpen = false;
        float engStart = 0, engEnd = 0, engSight = -1, firstShot = -1, fsDist = 0;
        int engShots = 0, engBurst = 0, engHits = 0, engHeads = 0, fsHit = 0, fsGroup = 0, engFull = 0;
        std::string fsWeapon;
        float lastShot = -100;
        int run = 0, turns = 0;   // turns: frames the panic turn moved the view (this engagement)
        float unholdAt = -100;
        int unholds = 0;
        float firedAt = -100;     // last shot this player fired (humans too)
        std::string closeName;    // the nearest enemy in line of sight within shoot_r
    } sh;
    // STEP 12 (Job 5): every player, humans too - its entity (bots: = ent; humans: the CCSPlayer, found by RTTI), where
    // it is this frame and how fast it moves (a noise's source)
    uintptr_t pent = 0;
    Vec fpos{};
    bool falive = false;
    float hsp = 0, hspT = -100;
    Vec hspPos{};
    // STEP 12: look controller - one owner of a bot's view outside fights (hearing, reacting to fire, pre-aim, clearing)
    struct Lk
    {
        int pri = 0;              // 0 idle; higher wins (LK_* below)
        Vec tgt{};
        float speed = 0, until = -100, start = -100, onAt = -1;
        const char *why = "";
    } lk;
    // STEP 12B: hearing - a noise heard -> after the bot's reaction delay it turns to the likely angle and holds it
    struct Hr
    {
        float h = 0;              // this round's hearing dial (the "aware" dial, profile-style: no ramp, every rank reacts)
        bool pend = false;        // a reaction waits for the bot's reaction delay
        std::string src;
        int kind = -1, prio = 0;
        Vec npos{};
        float due = 0, at = -100, dist = 0;
        std::string lookSrc;      // the source of the reaction in progress (its next steps move the aim along)
        bool corner = false;      // ... unless it pre-aims the corner he will come round and he is still near it
        Vec cornerAt{};
        float stopUntil = -100;   // stops to listen until then
        std::map<std::string, float> lastBy;
        // metric (on and off alike): an enemy noise from outside the bot's view -> did it face him within 2.5 s
        bool chk = false;
        std::string chkSrc;
        int chkKind = 0;
        float chkT0 = 0, chkD = 0;
        int reacts = 0, stops = 0, calls = 0, cN = 0, cK = 0;
        float cT = 0;
    } hr;
    // STEP 12G: reacting to fire - hit or shot at -> turn / dodge / break line of sight and re-peek
    struct Fr
    {
        float g = 0;                          // this round's fire dial (footwork + react, profile-style)
        std::string att;                      // who shoots at it
        std::vector<std::pair<float, float>> hitsT;   // recent hits (time, damage)
        float shotAt = -100, actAt = -100, due = -1;
        int pendAct = 0, act = 0;
        bool pendHit = false;
        float dodgeUntil = -100, skill = 0;
        int phase = 0;                        // cover: 1 dash, 2 behind cover, 3 re-peek
        Vec spot{}, from{}, attPos{};
        float phaseAt = 0, hold = 0, turnSpeed = 600, reissue = 0;
        bool outOfSight = false;
        // the FIRE line is written 1 s after the reaction, with what the bot did
        bool logPend = false;
        float logAt = 0, spdSum = 0, dist = 0, hp = 0;
        int spdN = 0, lhits = 0;
        Vec actPos{};
        // stood still after a hit (1.5 s of speed samples)
        bool stillOpen = false;
        float stillAt = 0, nextSample = 0;
        std::vector<float> stillV;
        int nHit = 0, nStill = 0, nTurn = 0, nDodge = 0, nCover = 0, nCoverOk = 0, nPeek = 0, nShotAt = 0;
    } fr;
    struct M
    {
        int steps = 0, near = 0, jumps = 0, jumpsF = 0, shots = 0, hits = 0, fs = 0, fsHit = 0, fsMov = 0, fsMovHit = 0;
        int kills = 0, deaths = 0, reactN = 0, stops = 0, fights = 0, shotsAtk = 0;
        int jUp = 0, jFlat = 0, jDown = 0, jSlow = 0, stuck = 0, mvStock[4] = {}, mvOurs = 0, mvSafe = 0, hideExp = 0, hideCov = 0;   // STEP 10b
        float moveT = 0, walkT = 0, quietT = 0, react = 0, stuckT = 0;
    } m;
};
static std::map<std::string, Player> g_pl;   // by name, refreshed every tick

static float Smart(const std::string &name)
{
    double e = g_eloOf ? g_eloOf(name.c_str()) : 1000.0;
    return std::max(0.f, std::min(1.f, (float)((e - 300.0) / 2100.0)));
}

// ---- round state ----
static bool g_steer = true;       // test hook brain_steer 0 = metrics only
static bool g_modeOk = true;      // classic modes only (casual, competitive, Wingman); never deathmatch / arms race / etc.
static bool g_matchLive;          // after Match_Start (warmup rounds are not counted)
static int g_diag;
enum Phase { PH_NONE, PH_FREEZE, PH_LIVE, PH_OVER };
static Phase g_phase;
static float g_liveAt;            // freeze end
struct Plan
{
    std::string tSite, fakeSite, lurker;
    bool gather = false, executed = false, fake = false, bomberFollowed = false;
    float gatherUntil = 0, firstAtStack = 0;
    bool entering = false;               // STEP 9b: the group is closing up at the site's entrance before going in
    float enterAt = 0, firstAtDoor = 0;
    std::string hitSite;
    int hitLevel = 0;
    float hitAt = 0;
    bool planted = false;
    std::string plantSite;
    Vec bomb{};
    float plantAt = 0;
    bool retakeGo = false;
    bool recalled = false;     // STEP 12C: the defenders' first call was a fake - one re-call made
    float retakeUntil = 0;
    float gatherFirstAt = 0;   // STEP 9b: when the first retake CT reached the gather point
    float gatherSecondAt = 0;  // STEP 12F8: ... and the second
};
static Plan g_plan;
static std::string g_bomber;      // who carries the bomb (log), kept across the round reset

// metrics of the current round
struct RoundM
{
    bool sampled20 = false;
    std::map<std::string, int> ct20;   // site -> CTs near it at freeze end + 20 s
    int ctAlive20 = 0;
    std::string hit;
    float hitAt = 0;
    int hitMax = 0, tAliveAtHit = 0, lurkers = 0;
    std::set<std::string> away, rotated;
    bool plant = false, defuse = false, exploded = false, postSampled = false;
    std::string plantSite;
    int postNear = 0, postAlive = 0;
    std::string winner;
    int trades = 0, fallbacks = 0, steered = 0, rescued = 0;
    std::map<std::string, int> money;      // name -> money at freeze start
    std::map<std::string, int> bought;     // name -> 1 if a primary was bought this freeze
    std::string buyT = "-", buyCT = "-";   // team buy call
    std::string primT = "-", primCT = "-"; // bots holding a primary at freeze end / bots
};
static RoundM g_rm;
static std::map<std::string, int> g_money;   // live money by name (server log money lines)
static std::map<std::string, int> g_teamOf;  // by name from log lines
static std::set<std::string> g_hasPrimary; // bought a primary and has not died since (log)
static int g_roundNo;

// eco masks: money set aside during the freeze. STEP 10: every dollar comes back - the old rule skipped the hand-back
// when the bot's money had grown meanwhile (a kill in the round-end time, a late payout), which lost the savings.
// Now only a team switch (half time: the game resets everyone's money) or a match restart drops a mask; the money
// the bot earns / spends while masked is counted from the server log's money lines and checked at the hand-back.
struct Mask
{
    int before = 0, keep = 0, take = 0, team = 0, round = 0;
    int income = 0, spend = 0;   // logged while masked
};
static std::map<std::string, Mask> g_masked;
static const int kMaxMoney = 16000;   // mp_maxmoney (no mode cfg overrides it)
// ---- money offset (m_iAccount) found by matching the log's money lines, per level ----
static int g_moneyOff = 0;
static int g_moneyTries;
static void FindMoneyOffset()
{
    if (g_moneyOff && g_moneyOff != 0x2dbc && g_phase == PH_LIVE && g_masked.empty())
    {
        // re-check now and then: two bots off = the offset was a coincidence, find it again
        static float nextCheck;
        if (CurTime() < nextCheck && CurTime() > nextCheck - 60)
            return;
        nextCheck = CurTime() + 10.f;
        int bad = 0;
        for (auto &kv : g_pl)
        {
            auto m = g_money.find(kv.first);
            if (kv.second.bot && kv.second.ent && m != g_money.end() && *(int *)(kv.second.ent + g_moneyOff) != m->second)
                bad++;
        }
        if (bad >= 3)
        {
            BLog("money offset 0x%x no longer matches (%d bots) - searching again", g_moneyOff, bad);
            g_moneyOff = 0;
        }
        return;
    }
    if (g_moneyOff || g_phase != PH_LIVE)
        return;
    std::vector<std::pair<uintptr_t, int>> known;
    for (auto &kv : g_pl)
    {
        auto m = g_money.find(kv.first);
        if (kv.second.bot && kv.second.ent && m != g_money.end())
            known.push_back({kv.second.ent, m->second});
    }
    if (known.size() < 4)
        return;
    std::set<int> distinct;
    for (auto &k : known)
        distinct.insert(k.second);
    // 0x2dbc = m_iAccount measured on 1.38.8.1 (10 bots, 7 distinct values): accepted when 4+ bots match it; a full
    // search (only with 2+ distinct values, 20 tries per level) is the fallback for another build
    bool full = distinct.size() > 1 && g_moneyTries <= 20;
    if (full)
        g_moneyTries++;
    for (int i = -1; i < (full ? 0x6000 / 4 : 0); i++)
    {
        int off = i < 0 ? 0x2dbc : i * 4;
        if (off < 0x100) continue;
        bool all = true;
        for (auto &k : known)
        {
            int v;
            if (!SafeRead((void *)(k.first + off), &v, 4) || v != k.second)
            {
                all = false;
                break;
            }
        }
        if (all)
        {
            g_moneyOff = off;
            BLog("money offset 0x%x (matched %d bots, %d distinct values)", off, (int)known.size(), (int)distinct.size());
            return;
        }
    }
}
static void MarkChanged(edict_t *e)
{
    if (e)
        *(int *)e |= (1 << 0) | (1 << 8);   // FL_EDICT_CHANGED | FL_FULL_EDICT_CHANGED
}
static void UnmaskAll(bool drop = false)
{
    int n = 0, given = 0, taken = 0, bad = 0;
    for (auto &m : g_masked)
    {
        const Mask &k = m.second;
        taken += k.take;
        auto it = g_pl.find(m.first);
        if (it == g_pl.end() || !it->second.ent || !g_moneyOff)
        {
            BLog("MONEY back r=%d %s before=%d take=%d gone (left the server) give=0", k.round, m.first.c_str(), k.before, k.take);
            continue;
        }
        int *acc = (int *)(it->second.ent + g_moneyOff);
        int a = *acc;
        // the game reset everyone's money meanwhile (half time = the teams switched, or a match restart): the savings
        // are gone for players too, so nothing is handed back
        if (drop || it->second.team != k.team)
        {
            BLog("MONEY back r=%d %s before=%d take=%d acc=%d dropped (%s) give=0", k.round, m.first.c_str(), k.before, k.take, a,
                 drop ? "match restart" : "teams switched - money reset");
            continue;
        }
        int give = std::max(0, std::min(k.take, kMaxMoney - a));
        *acc = a + give;
        MarkChanged(it->second.e);
        // what the bot would have without the mask: before + income (capped like the game) - spend
        int expect = std::min(kMaxMoney, k.before + k.income) - k.spend;
        int gameView = k.keep + k.income - k.spend;   // the account while masked, as the log explains it
        bool ok = *acc == expect;
        if (!ok) bad++;
        BLog("MONEY back r=%d %s before=%d keep=%d take=%d income=%d spend=%d acc=%d logged=%d give=%d result=%d expect=%d %s", k.round,
             m.first.c_str(), k.before, k.keep, k.take, k.income, k.spend, a, gameView, give, *acc, expect, ok ? "OK" : "MISMATCH");
        given += give;
        n++;
    }
    if (!g_masked.empty())
        BLog("buy: gave eco money back to %d of %d bots ($%d of $%d set aside)%s", n, (int)g_masked.size(), given, taken,
             bad ? " - MONEY MISMATCH" : "");
    g_masked.clear();
}
// a money line of the server log for a masked bot: count what it earned / spent while masked
static void MaskedMoneyLine(const std::string &name, const char *m)
{
    auto it = g_masked.find(name);
    if (it == g_masked.end())
        return;
    int a, b, c;
    char op;
    if (sscanf(m, " money change %d%c%d = $%d", &a, &op, &b, &c) != 4)
        return;
    if (op == '+') it->second.income += b;
    else if (op == '-') it->second.spend += b;
}

// ---- helpers ----
static std::vector<Player *> Team(int team, bool botsOnly, bool aliveOnly = true)
{
    std::vector<Player *> v;
    for (auto &kv : g_pl)
        if (kv.second.team == team && (!aliveOnly || kv.second.alive) && (!botsOnly || kv.second.bot))
            v.push_back(&kv.second);
    return v;
}
static int AliveCount(int team) { return (int)Team(team, false).size(); }
static float MeanSmart(const std::vector<Player *> &v)
{
    if (v.empty()) return 0;
    float s = 0;
    for (auto *p : v) s += p->s;
    return s / v.size();
}
static bool Takes(Player *p) { return Rnd() < 0.3f + 0.8f * p->s; }       // follows an advanced call (retake gather)
static bool TakesBasic(Player *p) { return Rnd() < 0.6f + 0.4f * p->s; }  // plays its basic part (hold a site, stay with
                                                                          // the group, post-plant)
static void Assign(Player *p, TaskKind k, const Vec &g, int route, float arrive, const char *why, float holdFor = 40, float delay = 0)
{
    if (!p->bot)
        return;
    p->task.kind = k;
    p->task.goal = g;
    p->task.route = route;
    p->task.arrive = arrive;
    p->task.holdFor = holdFor;
    p->task.startAt = CurTime() + delay;
    p->task.why = why;
    p->task.via.clear();
    p->task.wp = 0;
    p->bestDist = 1e9f;
    p->progressAt = CurTime() + delay;
    p->issues = 0;
}
static void Release(Player *p) { p->task = Task(); }
static const Site *NearestSite(const Vec &v, float *d = nullptr)
{
    const Site *best = nullptr;
    float bd = 1e9f;
    for (auto &s : g_mi.sites)
    {
        float x = Dist(v, s.c);
        if (x < bd) { bd = x; best = &s; }
    }
    if (d) *d = bd;
    return best;
}
// STEP 9b: a CT's way to site `to` over the CT side (make_mapinfo's rot waypoints from the nearest site or mid), cut to
// the part still ahead of the bot and, for a gather point short of the site, to the part before that point
static void SetRoute(Player *p, const Site &to, const Vec *stopAt = nullptr)
{
    p->task.via.clear();
    p->task.wp = 0;
    std::string from;
    float bd = 1e9f;
    for (auto &s : g_mi.sites)
        if (s.label != to.label && Dist(p->pos, s.c) < bd) { bd = Dist(p->pos, s.c); from = s.label; }
    if (g_mi.hasMid && Dist(p->pos, g_mi.mid) < bd) from = "mid";
    auto it = g_mi.rot.find(from + ">" + to.label);
    if (it == g_mi.rot.end() || it->second.empty())
        return;
    const std::vector<Vec> &w = it->second;
    size_t k = 0;
    for (size_t i = 1; i < w.size(); i++)
        if (Dist(p->pos, w[i]) < Dist(p->pos, w[k])) k = i;
    if (k + 1 < w.size() && Dist(p->pos, w[k + 1]) < Dist(w[k], w[k + 1])) k++;   // already past the nearest one
    if (Dist(p->pos, to.c) < Dist(w[k], to.c)) return;                            // already nearer than the route
    float stopD = stopAt ? Dist(*stopAt, to.c) : 0.f;
    for (size_t i = k; i < w.size(); i++)
    {
        if (stopAt && Dist(w[i], to.c) < stopD) break;
        p->task.via.push_back(w[i]);
    }
}

// ---- round plans ----
static void PlanCT()
{
    auto bots = Team(3, true);
    std::sort(bots.begin(), bots.end(), [](Player *a, Player *b) { return a->s > b->s; });
    int n = (int)bots.size();
    if (!n) return;
    if (g_mi.hostage)
    {
        // STEP 9c: push straight in. Step 9's gather at the CT stack (6..22 s) got the first hostage picked up ~15 s
        // later than stock bots and gave the guards time to set up (escort: TickPlan); the stock "collect hostages" task (never steered)
        // does the pick-up and the rescue, the push only brings the CTs that were not sent for the hostages along
        const Site &s = g_mi.sites[rand() % g_mi.sites.size()];
        g_plan.tSite = s.label;
        g_plan.executed = true;
        for (auto *p : bots)
            if (TakesBasic(p))
            {
                Assign(p, T_MOVE, s.c, 1, 300, "CT push hostages");
                p->role = "push";
            }
        return;
    }
    // bomb map: 2-1-2 for five (site, site, mid, site, site), fewer = drop the mid first
    std::vector<std::string> L;
    for (auto &s : g_mi.sites) L.push_back(s.label);
    int flip = rand() % 2;
    std::vector<std::string> roles;
    if (L.size() == 1)
        roles.assign(n, L[0]);
    else
        for (int i = 0; i < n; i++)
        {
            if (i == 2 && n >= 3 && (n % 2 == 1) && g_mi.hasMid)
                roles.push_back("mid");
            else
                roles.push_back(L[(i + flip) % L.size()]);
        }
    std::map<std::string, int> used;
    for (int i = 0; i < n; i++)
    {
        Player *p = bots[i];
        p->role = roles[i];
        if (!TakesBasic(p))
        {
            p->role = "free";
            continue;
        }
        if (roles[i] == "mid")
        {
            Assign(p, T_HOLD, g_mi.mid, 2, 150, "CT hold mid", 45);
            continue;
        }
        const Site *s = FindSite(roles[i]);
        if (!s) continue;
        int k = used[roles[i]]++;
        Vec spot;
        if (!s->hold.empty() && p->s >= 0.35f)
            spot = s->hold[k % s->hold.size()];                       // the best spots watching the entry
        else if (!s->post.empty())
            spot = s->post[rand() % s->post.size()];                  // somewhere on the site
        else
            spot = s->c;
        Assign(p, T_HOLD, spot, 2, 120, ("CT hold " + roles[i]).c_str(), 45);
    }
}
static void PlanT()
{
    auto bots = Team(2, true);
    int n = (int)bots.size();
    if (!n) return;
    float st = MeanSmart(bots);
    if (g_mi.hostage)
    {
        // guard the hostages: most hold around them, a smart one holds the CT approach
        const Site &s = g_mi.sites[rand() % g_mi.sites.size()];
        int k = 0;
        for (auto *p : bots)
        {
            if (!TakesBasic(p)) { p->role = "free"; continue; }
            if (k == 1 && n >= 3 && p->s >= 0.4f)
            {
                Assign(p, T_HOLD, s.retake, 2, 150, "T hold hostage approach", 45);
                p->role = "forward";
            }
            else
            {
                Vec spot = s.post.empty() ? s.c : s.post[k % s.post.size()];
                Assign(p, T_HOLD, spot, 2, 150, "T guard hostages", 45);
                p->role = "guard";
            }
            k++;
        }
        return;
    }
    const Site &target = g_mi.sites[rand() % g_mi.sites.size()];
    g_plan.tSite = target.label;
    // a fake first (smart teams, two sites): show up at the other site's stack, then hit the target
    if (g_mi.sites.size() >= 2 && n >= 3 && Rnd() < 0.3f * st)
    {
        for (auto &s : g_mi.sites)
            if (s.label != target.label) { g_plan.fakeSite = s.label; break; }
        g_plan.fake = true;
    }
    // one lurker (smart teams, 3+ Ts, two sites): the smartest non-bomber holds another site's choke
    if (g_mi.sites.size() >= 2 && n >= 3 && Rnd() < 0.1f + 0.6f * st)
    {
        Player *best = nullptr;
        for (auto *p : bots)
            if (p->name != g_bomber && p->s >= 0.25f && (!best || p->s > best->s)) best = p;
        if (best)
        {
            g_plan.lurker = best->name;
            best->role = "lurk";
            Assign(best, T_HOLD, target.lurk, 2, 150, "T lurk", 60);
        }
    }
    g_plan.gather = st >= 0.2f;      // a sloppy team trickles in one by one
    const Site *first = g_plan.fake ? FindSite(g_plan.fakeSite) : &target;
    for (auto *p : bots)
    {
        if (p->name == g_plan.lurker) continue;
        bool bomber = p->name == g_bomber;
        if (!bomber && !TakesBasic(p))
        {
            // did not take the call: often still heads for the target, alone and straight in (a trickle); else stock
            if (Rnd() < 0.5f + 0.5f * p->s)
            {
                p->role = "trickle";
                Assign(p, T_MOVE, target.c, 2, target.radius * 0.6f, "T go site alone");
            }
            else
                p->role = "free";
            continue;
        }
        p->role = "group";
        if (g_plan.gather)
            Assign(p, T_HOLD, first->stack, 2, 250, g_plan.fake ? "T gather (fake)" : "T gather", 25);
        else
            Assign(p, T_MOVE, target.c, 2, target.radius * 0.6f, "T go site");
    }
}
// STEP 9b: from the stack the group first closes up at the site's entrance (the choke, just outside the site) and goes
// in from there together (TickPlan); step 9 ran straight in from the stack ~900 units back, and the first T usually
// reached the site alone while the rest were strung out behind (inferno's grouped hits)
static void TExecute(const char *why)
{
    const Site *s = FindSite(g_plan.tSite);
    if (!s) return;
    g_plan.entering = true;
    g_plan.enterAt = CurTime();
    g_plan.firstAtDoor = 0;
    int k = 0;
    for (auto *p : Team(2, true))
        if (p->role == "group")
        {
            Assign(p, T_HOLD, s->choke, 1, 200, "T to the entrance", 10);
            k++;
        }
    BLog("plan: T group to %s's entrance with %d (%s)", s->label.c_str(), k, why);
}
static void ClearStart(const Site &s);   // STEP 12F5 (job-5 code)
static void TGoIn(const char *why, int there, int group)
{
    const Site *s = FindSite(g_plan.tSite);
    if (!s) return;
    g_plan.executed = true;
    for (auto *p : Team(2, true))
        if (p->role == "group")
            Assign(p, T_MOVE, s->c, 1, s->radius * 0.6f, "T hit");
    BLog("plan: T hit %s with %d, %d at the entrance (%s)", s->label.c_str(), group, there, why);
    ClearStart(*s);   // STEP 12F5: clear the site's corners on the way in
}
// STEP 9b: a rotating CT joins at the back of the hit site (the holds farthest from the T entrance, spread over the
// two farthest), not at a random hold: step 9's rotators often ran into the Ts at the entrance
static Vec RotateSpot(const Site &s, int k)
{
    if (s.hold.empty())
        return s.c;
    std::vector<Vec> h = s.hold;
    std::sort(h.begin(), h.end(), [&](const Vec &a, const Vec &b) { return Dist(a, s.entry) > Dist(b, s.entry); });
    return h[k % std::min<size_t>(2, h.size())];
}
static bool CtAnchorStays(const Player *p, const std::string &hitSite);   // STEP 12C (job-5 code)
static bool RotOn();                                                      // JOB 5b N (defined after STEP 12)
static void RotStart(Player *p, const char *to);
static Vec VaryPost(const Site *s, const Vec &bomb, int k, const std::vector<Vec> &sorted);   // JOB 5b P
// level 1 = a T was seen on the site: one CT (the mid, else the nearest) goes to help;
// level 2 = the site is hit (2+ Ts or the bomber): everyone rotates but a smart team's anchor on the other site
static void OnSiteHit(const Site &s, int level)
{
    if (g_plan.planted || g_plan.hitLevel >= 2 || (g_plan.hitSite == s.label && g_plan.hitLevel >= level) || (level == 1 && g_plan.hitLevel >= 1))
        return;
    g_plan.hitSite = s.label;
    g_plan.hitLevel = level;
    g_plan.hitAt = CurTime();
    auto cts = Team(3, true);
    if (level == 1)
    {
        Player *best = nullptr;
        for (auto *p : cts)
        {
            if (Dist(p->pos, s.c) < s.radius + 400 || p->role == "rotate") continue;
            if (CtAnchorStays(p, s.label)) continue;   // STEP 12C: another site's anchor never leaves it
            // the rotator goes: the lurker (JOB 5b M) first, then the mid, else the nearest
            auto rk = [](const Player *q) { return q->role == "lurk" ? 0 : q->role == "mid" ? 1 : 2; };
            if (!best || rk(p) < rk(best) || (rk(p) == rk(best) && Dist(p->pos, s.c) < Dist(best->pos, s.c)))
                best = p;
        }
        if (best && Rnd() < 0.5f + 0.5f * best->s)
        {
            Vec spot = s.hold.empty() ? s.c : s.hold[rand() % s.hold.size()];
            Assign(best, T_HOLD, spot, 1, 150, "CT help", 30, RotOn() ? fl::RotateDelay(best->s, Rnd()) : Lerp(4.f, 0.5f, best->s));
            SetRoute(best, s);
            best->role = "rotate";
            RotStart(best, s.label.c_str());   // JOB 5b N
        }
        BLog("plan: T seen on %s - %s goes to help", s.label.c_str(), best && best->role == "rotate" ? best->name.c_str() : "nobody");
        return;
    }
    int away = 0;
    for (auto *p : cts)
        if (Dist(p->pos, s.c) >= s.radius + 400 && p->role != "rotate") away++;
    // STEP 9b: a smart team's anchor on the other site no longer stays for good (step 9 kept one CT back, which alone
    // cost a third of the rotations): with 3+ away it holds a little longer, then follows
    bool keepAnchor = AliveCount(3) >= 4 && MeanSmart(cts) >= 0.45f && away >= 3;
    if (CtAnchorStays(nullptr, s.label)) keepAnchor = false;   // STEP 12C: the anchors stay instead (below)
    int rot = 0, stay = 0;
    for (auto *p : cts)
    {
        float d = Dist(p->pos, s.c);
        if (d < s.radius + 400 || p->role == "rotate") continue;
        if (CtAnchorStays(p, s.label))
        {
            stay++;   // STEP 12C: "keep at least one planted on site" - the other sites' anchors hold
            continue;
        }
        float delay = Lerp(3.f, 0.5f, p->s) + Rnd() * 1.f;   // STEP 9b: was 6..1 s + 0..1.5 s
        if (RotOn()) delay = fl::RotateDelay(p->s, Rnd());   // JOB 5b N: on the call, at once
        if (keepAnchor && p->role != "mid" && p->role != "free")
        {
            keepAnchor = false;
            delay += 7.f;
        }
        else if (!RotOn() && Rnd() > 0.85f + 0.15f * p->s)
            continue;   // a sloppy CT sometimes stays where it is (step 9: 0.7 + 0.3 s; JOB 5b N: never)
        Vec spot = RotateSpot(s, rot);
        Assign(p, T_HOLD, spot, 1, 150, "CT rotate", 30, delay);
        SetRoute(p, s);   // over the CT side, not the bots' own shortest path through T ground
        p->role = "rotate";
        RotStart(p, s.label.c_str());   // JOB 5b N
        rot++;
    }
    // the lurker comes in from behind once the hit is on
    if (!g_plan.lurker.empty())
    {
        auto it = g_pl.find(g_plan.lurker);
        if (it != g_pl.end() && it->second.alive)
            Assign(&it->second, T_MOVE, s.c, 1, s.radius * 0.6f, "lurker joins");
    }
    BLog("plan: site %s hit - %d CT(s) rotate%s", s.label.c_str(), rot, stay ? (", " + std::to_string(stay) + " anchor(s) hold their site").c_str() : "");
}
static bool PostOn();                                        // STEP 12F (job-5 code)
static int g_rtkJoined, g_rtkAlive;                           // JOB 5b N: CTs given a retake job / alive at the plant
static bool RetakeOn();
static void RetakeAwp(Player *p, const Vec &bomb, const Site *s);
static void OnPlant(const Vec &bomb, const std::string &site)
{
    const Site *s = FindSite(site);
    if (!s)
        s = NearestSite(bomb);
    // T: post-plant holds around the bomb, spread over the post spots nearest to it
    std::vector<Vec> spots = s ? s->post : std::vector<Vec>();
    std::sort(spots.begin(), spots.end(), [&](const Vec &a, const Vec &b) { return Dist(a, bomb) < Dist(b, bomb); });
    int k = 0;
    for (auto *p : Team(2, true))
    {
        if (!TakesBasic(p))
        {
            // STEP 12F7: no T leaves the bomb (step 9 released a bot that failed its roll to the stock AI - his "3 Ts ran
            // back to spawn"): it defends from a sloppier spot (a random post spot), not the best one
            if (PostOn())
            {
                Assign(p, T_HOLD, spots.empty() ? bomb : spots[rand() % spots.size()], 1, 150, "T post-plant (sloppy)", 40);
                p->role = "post";
            }
            else
                Release(p);
            continue;
        }
        Vec spot = spots.empty() ? bomb : spots[k % spots.size()];
        spot = VaryPost(s, bomb, k++, spots);   // JOB 5b P: another spot than last round's when there is one
        Assign(p, T_HOLD, spot, 1, 120, "T post-plant", 45);
        p->role = "post";
    }
    // CT: a smart team gathers before the retake, a sloppy one runs in (the stock AI then finds and defuses the bomb)
    auto cts = Team(3, true);
    bool gather = cts.size() >= 2 && MeanSmart(cts) >= 0.35f && s;
    g_plan.retakeGo = !gather;
    // STEP 9b: step 9 went in 5 s after the plant whatever happened (logged "0/2 gathered" most rounds), so the CTs
    // still ran in one by one. Now the gather waits for the group, bounded by the bomb clock: the last moment to go
    // leaves the walk from the gather point to the bomb plus a kitless defuse (10 s) and 3 s spare of the 40 s fuse.
    float walk = s ? Dist(s->retake, bomb) / 200.f : 0.f;
    g_plan.retakeUntil = CurTime() + std::max(4.f, 40.f - 10.f - 3.f - walk);
    if (RetakeOn()) g_plan.retakeUntil = fl::RetakeDeadline(CurTime(), walk, 10.f);   // STEP 12F8: hurry (<= 15 s gather)
    g_plan.gatherFirstAt = 0;
    g_rtkJoined = 0;
    g_rtkAlive = (int)cts.size();
    for (auto *p : cts)
    {
        // STEP 9b: a smart team's CTs join the retake as a basic part (0.6 + 0.4 s; step 9: 0.3 + 0.8 s for everyone)
        // JOB 5b N: every CT joins at once (a released one was "the bot on B not doing anything" - the stock AI then
        // searched for the bomb); one that fails its roll is only a moment late
        float late = 0.f;
        if (!(gather ? TakesBasic(p) : Takes(p)))
        {
            if (!RotOn()) { Release(p); continue; }
            late = 1.0f + 0.5f * Rnd();
        }
        if (gather && Dist(p->pos, bomb) > 900)
        {
            Assign(p, T_HOLD, s->retake, 1, 250, "CT gather for retake", 30, late);
            SetRoute(p, *s, &s->retake);
        }
        else
            Assign(p, T_MOVE, bomb, 1, 350, "CT retake", 40, late);
        p->role = "retake";
        RotStart(p, "bomb");   // JOB 5b N
        g_rtkJoined++;
        RetakeAwp(p, bomb, s);   // STEP 12F8: an AWP holds a long angle over the bomb and fights from there
    }
    BLog("plan: bomb planted at %s - T post-plant, CT %s", site.c_str(), gather ? "gather then retake" : "retake");
}

static void DeathInfo(const char *msg, const std::string &vname, int vt, const Vec &vpos);   // JOB 5c S (below)
// ---- log lines (called from the plugin's log reader, main thread) ----
static bool ParseActor(const char *msg, std::string &name, std::string &team)
{
    // "Name<uid><STEAM_x or BOT><TEAM>" ...
    if (msg[0] != '"') return false;
    const char *lt = strchr(msg + 1, '<');
    if (!lt) return false;
    name.assign(msg + 1, lt - msg - 1);
    const char *t = lt;
    for (int i = 0; i < 2 && t; i++) t = strchr(t + 1, '<');
    if (!t) return false;
    const char *gt = strchr(t, '>');
    if (!gt) return false;
    team.assign(t + 1, gt - t - 1);
    return true;
}
static bool IsPrimary(const char *w)
{
    static const char *prim[] = {"ak47", "m4a1", "m4a1_silencer", "aug", "sg556", "famas", "galilar", "awp", "ssg08", "scar20", "g3sg1",
                                 "mp9", "mac10", "mp7", "mp5sd", "ump45", "p90", "bizon", "nova", "xm1014", "mag7", "sawedoff",
                                 "negev", "m249"};
    for (auto *p : prim)
        if (!strcmp(w, p)) return true;
    return false;
}
static void FlushRound(const char *why);
static void StartRound();
static void RoundLive();
static bool CtReangle(Player *p, const Vec &at);   // STEP 12C (job-5 code)
static bool CtKeeps(const Player *p);
static bool CtOn();
static void PlanCT12();
static void CtTick();
static bool PlanT12();          // STEP 12D (job-5 code)
static bool T12();
static void TickT12();
static void J5RoundLive();
static void FwRoundStart();   // STEP 10
static void FwRoundLog();
// JOB 5b (addendum 2, defined after STEP 12)
static void RefreshJ5b();
static void J5bLevelInit();
static void J5bMatchStart();
static bool PushOn();                          // L
static bool CtSetOn();                         // M
static void PlanCT5b();
static bool PlanCT5d();   // JOB 5d (end of file)
static bool CtFocusSkip(const Player &b, const Vec &noise);
static bool NearMid(const Vec &p);
static void UtilPurchased(const std::string &name, const char *item);   // O
static void UtilFreezeTick(float now);
static void UtilTick(float now);
static bool UtilBusy(const Player &p);
static bool UtilStill(const Player &p);
static void UtilDetonate(Player *p, const std::string &kind, const Vec &at, float now);
static void UtilFrame(Player &b, float now);
static void UtilExecute();
static bool UtilFlashFirst(float now);
static bool MoveCycling(const Player &p);      // P
static void J5bWant(Player &p, float *w);
static void MoveFrame(Player &b, float now);
static void CrouchFrame(Player &b, float now);   // Q
static void CrouchTick(float now);
static bool CrouchOn(const Player &p);
static void CrouchRestoreAll();
static Vec VaryPost(const Site *s, const Vec &bomb, int k, const std::vector<Vec> &sorted);
static Vec VaryLurk(const Vec &lurk);
static bool UtilHoldGo(float now);
static bool RotOn();                           // N
static void RotStart(Player *p, const char *to);
static bool RotUrgent(const Player &p);
static int RotRoute(Player &p, const Vec &goal, int route);
static void RotFrame(float now);
static void RotLog(Player &p, const char *end, float now);
static void J5bRoundStart();
static void J5bRoundEnd();
static void PushPlanRound();
static void CtPushTick(float now);
static bool PushPlanned(const std::string &name);
static bool J5bTradeBack(Player &p);
static unsigned g_matchSeed;
static unsigned Fnv(const std::string &s);

void BrainLogLine(const char *msg)
{
    std::string name, team;
    if (strstr(msg, "World triggered \"Match_Start\"") == msg)
    {
        g_matchLive = true;
        g_roundNo = 0;
        g_hasPrimary.clear();
        g_money.clear();   // the restart resets everyone's money without a money line
        UnmaskAll(true);   // STEP 10: ... so a mask from before it is dropped, not handed back on top
        g_matchSeed = Fnv(g_mapName + ":" + std::to_string((long)time(nullptr)));   // STEP 10: per-match form
        J5bMatchStart();   // JOB 5b: the round count starts again - so does "rounds since the last push"
        BLog("match start on %s (form seed %u)", g_mapName.c_str(), g_matchSeed);
        return;
    }
    if (!strncmp(msg, "Starting Freeze period", 22))
    {
        StartRound();
        return;
    }
    if (strstr(msg, "World triggered \"Round_Start\"") == msg)
    {
        RoundLive();
        return;
    }
    if (strstr(msg, "World triggered \"Round_End\"") == msg)
    {
        FlushRound("round end");
        return;
    }
    if (!strncmp(msg, "Team \"", 6) && strstr(msg, "triggered \""))
    {
        if (strstr(msg, "Team \"CT\"") == msg) g_rm.winner = "CT";
        else if (strstr(msg, "Team \"TERRORIST\"") == msg) g_rm.winner = "T";
        if (strstr(msg, "Target_Bombed")) g_rm.exploded = true;
        return;
    }
    if (!ParseActor(msg, name, team))
        return;
    if (team == "CT" || team == "TERRORIST")
        g_teamOf[name] = team == "CT" ? 3 : 2;
    if (const char *m = strstr(msg, "\" money change "))
    {
        if (const char *eq = strstr(m, "= $"))
            g_money[name] = atoi(eq + 3);
        MaskedMoneyLine(name, m + 1);   // STEP 10: the money check
        return;
    }
    if (const char *pu = strstr(msg, "\" purchased \""))
    {
        char w[48] = "";
        sscanf(pu + 13, "%47[^\"]", w);
        if (IsPrimary(w)) { g_rm.bought[name] = 1; g_hasPrimary.insert(name); }
        UtilPurchased(name, w);   // JOB 5b O: our util buys show up here
        return;
    }
    if (strstr(msg, "triggered \"Got_The_Bomb\""))
    {
        g_bomber = name;
        return;
    }
    if (strstr(msg, "triggered \"Dropped_The_Bomb\""))
    {
        if (g_bomber == name) g_bomber.clear();
        return;
    }
    if (strstr(msg, "triggered \"Planted_The_Bomb\""))
    {
        const char *at = strstr(msg, "at bombsite ");
        std::string site = at ? std::string(at + 12, 1) : "";
        auto it = g_pl.find(name);
        Vec bomb = it != g_pl.end() ? it->second.pos : Vec{};
        if (site.empty() || !FindSite(site))
        {
            const Site *s = NearestSite(bomb);
            site = s ? s->label : "";
        }
        g_rm.plant = true;
        g_rm.plantSite = site;
        g_plan.planted = true;
        g_plan.plantAt = CurTime();
        g_plan.bomb = bomb;
        g_plan.plantSite = site;
        g_bomber.clear();
        if (g_phase == PH_LIVE && g_mi.ok && !g_mi.hostage && g_steer && g_layout == 1)
            OnPlant(bomb, site);
        return;
    }
    if (strstr(msg, "triggered \"Rescued_A_Hostage\""))
    {
        g_rm.rescued++;
        return;
    }
    if (strstr(msg, "triggered \"Defused_The_Bomb\""))
    {
        g_rm.defuse = true;
        return;
    }
    const char *kl = strstr(msg, "] killed \"");
    if (kl && g_phase == PH_LIVE)
    {
        std::string vname, vteam;
        if (!ParseActor(kl + 9, vname, vteam)) return;
        float vx, vy, vz;
        const char *vb = strchr(kl + 9, '[');
        if (!vb || sscanf(vb, "[%f %f %f]", &vx, &vy, &vz) != 3) return;
        Vec vpos{vx, vy, vz};
        int vt = vteam == "CT" ? 3 : vteam == "TERRORIST" ? 2 : 0;
        auto vit = g_pl.find(vname);
        if (vit != g_pl.end()) vit->second.alive = false;
        g_hasPrimary.erase(vname);
        if (!vt || !g_steer || g_layout != 1 || !g_mi.ok) return;
        DeathInfo(msg, vname, vt, vpos);   // JOB 5c S: the team learns where the killer is
        // trade: the nearest teammate bot follows up the death
        Player *best = nullptr;
        float bd = 1300;
        for (auto *p : Team(vt, true))
        {
            if (p->name == g_bomber || p->name == g_plan.lurker || ObjectiveTask(BotTask(p->ent), true)) continue;
            if (p->role == "escort") continue;   // STEP 12H: an escort stays with the bomb carrier
            // STEP 9b: a rotating CT trades only on the site it is rotating to; a CT gathering for the retake waits for
            // the others (a lone trade run into the post-plant crossfire was step 9's most common retake death)
            if (vt == 3 && p->role == "rotate")
            {
                const Site *hs = FindSite(g_plan.hitSite);
                if (!hs || Dist(vpos, hs->c) > hs->radius + 600) continue;
            }
            if (vt == 3 && p->role == "retake" && !g_plan.retakeGo) continue;
            if (vt == 3 && p->role == "retake-awp") continue;   // STEP 12F8: the retake AWP keeps its long angle
            // STEP 9c: on hostage maps a CT does not trade (a lone run into the guards) - it escorts the carrier instead
            if (vt == 3 && g_mi.hostage) continue;
            // STEP 9c: a hostage guard trades only a death at the hostages (it stays a guard); a trade run elsewhere
            // pulled guards out to chase the carriers
            if (vt == 2 && g_mi.hostage)
            {
                const Site *hs = NearestSite(vpos);
                if (!hs || Dist(vpos, hs->c) > hs->radius + 300 || Dist(p->pos, hs->c) > hs->radius + 450) continue;
            }
            // STEP 12C: a holding CT never runs to trade - it turns its angle to where the killer is ("catch you slipping")
            if (vt == 3 && CtReangle(p, vpos)) continue;
            // a CT holding a site (or mid) keeps its angle unless the death was on its own site
            if (vt == 3 && !g_plan.planted && p->task.kind == T_HOLD && p->role != "rotate" && p->role != "fallback")
            {
                const Site *own = FindSite(p->role);
                if (!own || Dist(vpos, own->c) > own->radius + 600) continue;
            }
            float d = Dist(p->pos, vpos);
            if (d < bd) { bd = d; best = p; }
        }
        if (best && Rnd() < 0.15f + 0.75f * best->s)
        {
            Assign(best, T_MOVE, vpos, 1, 180, "trade");
            best->role = "trade";
            g_rm.trades++;
        }
        // fall back when outnumbered by two or more (not after a plant): hold together
        int mine = AliveCount(vt), theirs = AliveCount(vt == 2 ? 3 : 2);
        // STEP 9c: not on hostage maps - a CT "fall back" was a 20 s hide on the spot (hostage sites have no holds), which
        // stopped the rescue; a T one left its guard spot for a teammate's
        if (!g_mi.hostage && !g_plan.planted && mine >= 1 && theirs - mine >= 2)
        {
            auto team2 = Team(vt, true);
            for (auto *p : team2)
            {
                if (p->s < 0.3f || p->role == "fallback" || p == best || p->name == g_bomber || ObjectiveTask(BotTask(p->ent), true)) continue;
                if (vt == 3 && CtKeeps(p)) continue;   // STEP 12C: a holding CT already holds its site (and stays its anchor)
                if (vt == 3 && p->role == "rotate")
                {
                    // STEP 9b: a rotating CT regroups where the team is going - the back of the hit site - instead of
                    // step 9's first hold of the site it is nearest to (which sent it back where it came from)
                    if (const Site *hs = FindSite(g_plan.hitSite))
                    {
                        Assign(p, T_HOLD, RotateSpot(*hs, 0), 1, 200, "fall back", 20);
                        SetRoute(p, *hs);
                        p->role = "fallback";
                        g_rm.fallbacks++;
                    }
                    continue;
                }
                Vec spot = p->pos;
                if (vt == 3)
                {
                    const Site *s = NearestSite(p->pos);
                    if (s && !s->hold.empty()) spot = s->hold[0];
                }
                else
                    for (auto *q : team2)
                        if (q != p) { spot = q->pos; break; }
                Assign(p, T_HOLD, spot, 2, 200, "fall back", 20);
                p->role = "fallback";
                g_rm.fallbacks++;
            }
        }
    }
}

// ---- rounds ----
static std::string g_callT = "-", g_callCT = "-";   // team buy calls made at the previous round end
static bool g_buyDone = true;
static float g_buyAt;
static void StartRound()
{
    g_phase = PH_FREEZE;
    g_plan = Plan();
    for (auto &kv : g_pl)
    {
        Release(&kv.second);
        kv.second.role.clear();
    }
    g_rm = RoundM();
    g_rm.money = g_money;
    g_rm.buyT = g_callT;
    g_rm.buyCT = g_callCT;
    g_callT = g_callCT = "-";
    FwRoundStart();
}
// team buy: the bots buy the moment the freeze starts, so the call is made 1.5 s after the round end (the round's
// money is paid by then) and an eco team's money is set aside until the freeze is over (smart teams only)
// STEP 10b (2026-09-25) kid floor, economy: a kid-level bot buys like a beginner. During the buy it holds at most
// KidCap(live elo) dollars - $2500 up to elo 350 (an SMG and armour, or a cheap rifle; never rifle + armour + grenades),
// rising to no cap at elo 550 - and gets every dollar back at the same hand-back as the eco masks (checked the same way).
// Measured: an elo-321 team lost 77% of rounds to an elo-693 team (390 rounds, gate 80%; the aim curve was already used
// twice); with the kid buy at $2500 83%. ($1700: the bots bought nothing at all - pistols only, 92%.) Switches:
// brain_kidbuy 0, kidbuy_cap <dollars> (family_test.txt).
static double TestHook(const char *k, double d);   // STEP 10b (defined with the step 10 test hooks)
static const int kKidCap = 2500;
static int KidCap(double elo)
{
    const double lo = 350, hi = 550;
    const int floor = (int)TestHook("kidbuy_cap", kKidCap);
    if (elo >= hi) return kMaxMoney;
    if (elo <= lo) return floor;
    return (int)(floor + (kMaxMoney - floor) * (elo - lo) / (hi - lo));
}
// STEP 10c (2026-09-25, his call) RIFLE FIRST for sniper-template bots. Measured in 10b at equal dials: a sniper-template
// team won 18% of rounds vs rifle bots (AWP every rich round, autosnipers). The Sniper template keeps the AWP first, and
// here a sniper bot holds at most kNoAwpCap ($1 under the AWP) during the buy - so it buys the rifle (famas/galil, then
// the scout, when a rifle is not affordable) - unless it is its team's ONE AWP this round: the richest sniper-template bot holding at least
// kAwpRich. Same Mask records and checked hand-back as the kid buy: income never changes, only what is bought.
// (Shotgun/MG/SMG templates need no brain help: rifle first is their template order, make_botprofile.py.)
// Measured (step10 logs/summary-10c.txt, equal dials, 180 rounds): sniper team vs rifle bots 18% -> 47%.
// Switches: brain_riflefirst 0, awp_rich <dollars> (family_test.txt).
static const int kNoAwpCap = 4749, kAwpRich = 5750;
static const char *BotTmpl(const std::string &name);   // defined with the dials
static void KidBuy()
{
    const bool kid = (int)TestHook("brain_kidbuy", 1), rifle = (int)TestHook("brain_riflefirst", 1);
    if (!kid && !rifle)
        return;
    const int rich = (int)TestHook("awp_rich", kAwpRich);
    for (int team = 2; team <= 3; team++)
    {
        auto bots = Team(team, true, false);
        std::string awper;
        int best = -1;
        for (auto *p : bots)
        {
            if (!p->ent || g_masked.count(p->name) || strcmp(BotTmpl(p->name), "Sniper")) continue;
            int m = *(int *)(p->ent + g_moneyOff);
            if (m >= rich && m > best) { best = m; awper = p->name; }
        }
        for (auto *p : bots)
        {
            if (!p->ent || g_masked.count(p->name)) continue;
            int cap = kid ? KidCap(g_eloOf ? g_eloOf(p->name.c_str()) : 1000.0) : kMaxMoney;
            bool noAwp = rifle && !strcmp(BotTmpl(p->name), "Sniper") && p->name != awper;
            if (noAwp) cap = std::min(cap, kNoAwpCap);
            int *acc = (int *)(p->ent + g_moneyOff);
            int take = *acc - std::min(*acc, cap);
            if (cap >= kMaxMoney || take <= 0 || take > kMaxMoney) continue;
            Mask mk;
            mk.before = *acc;
            mk.keep = *acc - take;
            mk.take = take;
            mk.team = p->team;
            mk.round = g_roundNo;
            *acc = mk.keep;
            MarkChanged(p->e);
            g_masked[p->name] = mk;
            BLog("MONEY mask r=%d %s before=%d keep=%d take=%d (%s)", g_roundNo, p->name.c_str(), mk.before, mk.keep, take,
                 cap == kNoAwpCap ? "rifle first" : "kid buy");
        }
        if (!awper.empty())
            BLog("buy: %s AWP this round = %s ($%d)", team == 2 ? "T" : "CT", awper.c_str(), best);
    }
}
static void TeamBuy()
{
    if (g_buyDone || g_phase != PH_OVER || CurTime() < g_buyAt || !g_steer || g_layout != 1 || !g_moneyOff || !g_matchLive)
        return;
    g_buyDone = true;
    for (int team = 2; team <= 3; team++)
    {
        auto bots = Team(team, true, false);
        if (bots.size() < 2) continue;
        long sum = 0;
        int n = 0;
        for (auto *p : bots)
            if (p->ent) { sum += *(int *)(p->ent + g_moneyOff); n++; }
        if (!n) continue;
        int avg = (int)(sum / n);
        const char *call = avg >= 4000 ? "full" : avg >= 2400 ? "force" : "eco";
        if (MeanSmart(bots) < 0.3f)
            call = "own";   // a sloppy team buys bot by bot
        (team == 2 ? g_callT : g_callCT) = call;
        if (strcmp(call, "eco") || avg <= 1000)   // a pistol-round team just buys what it can
            continue;
        int k = 0;
        for (auto *p : bots)
        {
            if (!p->ent) continue;
            int *acc = (int *)(p->ent + g_moneyOff);
            int keep = std::min(*acc, 400);   // an eco keeps pocket money for a pistol at most
            int take = *acc - keep;
            if (take <= 0 || take > kMaxMoney || g_masked.count(p->name)) continue;
            Mask mk;
            mk.before = *acc;
            mk.keep = keep;
            mk.take = take;
            mk.team = p->team;
            mk.round = g_roundNo;
            *acc = keep;
            MarkChanged(p->e);
            g_masked[p->name] = mk;
            BLog("MONEY mask r=%d %s before=%d keep=%d take=%d", g_roundNo, p->name.c_str(), mk.before, keep, take);
            k++;
        }
        BLog("buy: %s team avg $%d -> eco (%d bots saving)", team == 2 ? "T" : "CT", avg, k);
    }
    KidBuy();   // STEP 10b
}
static void RoundLive()
{
    g_phase = PH_LIVE;
    g_liveAt = CurTime();
    if (g_matchLive) g_roundNo++;
    for (int team = 2; team <= 3; team++)
    {
        int n = 0, k = 0;
        for (auto &kv : g_pl)
            if (kv.second.bot && kv.second.team == team)
            {
                n++;
                if (g_hasPrimary.count(kv.first)) k++;
            }
        (team == 2 ? g_rm.primT : g_rm.primCT) = std::to_string(k) + "/" + std::to_string(n);
    }
    UnmaskAll();   // the freeze (the bots' buy time) is over: eco money back
    J5RoundLive();   // STEP 12I: who carries the bomb this round (logged, steering on or off)
    if (!g_steer || g_layout != 1 || !g_mi.ok)
        return;
    if (PlanCT5d()) {}           // JOB 5d: his rule - 2 on A, 2 on B, 1 on mid, from his hand-placed spots
    else if (CtSetOn()) PlanCT5b();   // JOB 5b M: anchors on the sites, one mid, one lurker, spots vary round to round
    else if (CtOn()) PlanCT12();   // STEP 12C: 2 / 2 / 1 with an anchor on every site, every CT holds (else step 9's)
    else PlanCT();
    PushPlanRound();          // JOB 5b L: a planned push this round (rare), never aimed at the enemy
    if (!PlanT12()) PlanT();   // STEP 12D: the called plan (else step 9's)
    int n = 0;
    std::string roles;
    for (auto &kv : g_pl)
    {
        if (kv.second.task.kind != T_NONE) n++;
        if (kv.second.bot && kv.second.alive)
            roles += kv.first + "(" + (kv.second.team == 2 ? "T" : "CT") + std::to_string((int)(kv.second.s * 100)) + ":" +
                     (kv.second.role.empty() ? "-" : kv.second.role) + ") ";
    }
    g_rm.steered = n;
    BLog("plan r%d: T site %s%s%s%s | %s", g_roundNo, g_plan.tSite.c_str(), g_plan.fake ? (" fake " + g_plan.fakeSite).c_str() : "",
         g_plan.lurker.empty() ? "" : (" lurker " + g_plan.lurker).c_str(), g_plan.gather ? " gather" : " trickle", roles.c_str());
}

// metrics sampling (steering on or off)
static void SampleMetrics()
{
    if (g_phase != PH_LIVE || !g_mi.ok)
        return;
    float t = CurTime() - g_liveAt;
    // attackers / defenders: T / CT on bomb maps, CT / T on hostage maps (the "ct" fields then count the T guards)
    bool hm = g_mi.hostage;
    auto ts = Team(hm ? 3 : 2, false), cts = Team(hm ? 2 : 3, false);
    const Vec &aspawn = hm ? g_mi.ctspawn : g_mi.tspawn;
    if (!g_rm.sampled20 && t >= 20.f)
    {
        g_rm.sampled20 = true;
        g_rm.ctAlive20 = (int)cts.size();
        for (auto &s : g_mi.sites)
        {
            int k = 0;
            for (auto *p : cts)
                if (Dist(p->pos, s.c) < s.radius + 450) k++;
            g_rm.ct20[s.label] = k;
        }
    }
    // hit = the first time a T is on a site before a plant
    if (g_rm.hit.empty() && !g_plan.planted)
        for (auto &s : g_mi.sites)
        {
            for (auto *p : ts)
                if (Dist(p->pos, s.c) < s.radius + 100)
                {
                    g_rm.hit = s.label;
                    g_rm.hitAt = CurTime();
                    g_rm.tAliveAtHit = (int)ts.size();
                    for (auto *q : ts)
                        if (Dist(q->pos, s.c) > 1800 && Dist(q->pos, aspawn) > 1000) g_rm.lurkers++;
                    for (auto *q : cts)
                        if (Dist(q->pos, s.c) > 1500) g_rm.away.insert(q->name);
                    break;
                }
            if (!g_rm.hit.empty()) break;
        }
    if (!g_rm.hit.empty())
    {
        const Site *s = FindSite(g_rm.hit);
        if (s && CurTime() - g_rm.hitAt <= 6.f)
        {
            int k = 0;
            for (auto *p : ts)
                if (Dist(p->pos, s->c) < s->radius + 500) k++;
            g_rm.hitMax = std::max(g_rm.hitMax, k);
        }
        if (s && CurTime() - g_rm.hitAt <= 35.f)
            for (auto *p : cts)
                if (g_rm.away.count(p->name) && Dist(p->pos, s->c) < s->radius + 400) g_rm.rotated.insert(p->name);
    }
    if (g_plan.planted && !g_rm.postSampled && CurTime() - g_plan.plantAt >= 15.f)
    {
        g_rm.postSampled = true;
        g_rm.postAlive = (int)ts.size();
        for (auto *p : ts)
            if (Dist(p->pos, g_plan.bomb) < 1100) g_rm.postNear++;
    }
}
static void FlushRound(const char *why)
{
    if (g_phase != PH_LIVE)
    {
        g_phase = PH_OVER;
        return;
    }
    g_phase = PH_OVER;
    g_buyDone = false;
    g_buyAt = CurTime() + 1.5f;
    for (auto &kv : g_pl)
        Release(&kv.second);
    if (!g_matchLive || !g_mi.ok)
        return;
    FwRoundLog();   // STEP 10: one BOTR line per bot
    // team buys: coherent = all of a team's bots bought a primary, or none did
    auto buyStr = [&](int team) {
        int n = 0, b = 0, nm = 0;
        long money = 0;
        for (auto &kv : g_pl)
        {
            if (kv.second.team != team || !kv.second.bot) continue;
            n++;
            if (g_rm.bought.count(kv.first)) b++;
            auto m = g_rm.money.find(kv.first);
            if (m != g_rm.money.end()) { money += m->second; nm++; }
        }
        char o[64];
        snprintf(o, sizeof(o), "%d/%d/$%d", b, n, nm ? (int)(money / nm) : -1);
        return std::string(o);
    };
    // STEP 10: each side's mean elo / footwork / aim this round (the elo and dial A/B tests)
    auto teamDials = [](std::map<std::string, Player> &pl) {
        double e[4] = {}, f[4] = {}, a[4] = {};
        int n[4] = {};
        for (auto &kv : pl)
            if (kv.second.bot && (kv.second.team == 2 || kv.second.team == 3))
            {
                int t = kv.second.team;
                e[t] += kv.second.elo; f[t] += kv.second.fw; a[t] += kv.second.aim; n[t]++;
            }
        char o[160];
        snprintf(o, sizeof(o), "eloT=%.0f eloCT=%.0f fwT=%.2f fwCT=%.2f aimT=%.2f aimCT=%.2f", n[2] ? e[2] / n[2] : 0, n[3] ? e[3] / n[3] : 0,
                 n[2] ? f[2] / n[2] : 0, n[3] ? f[3] / n[3] : 0, n[2] ? a[2] / n[2] : 0, n[3] ? a[3] / n[3] : 0);
        return std::string(o);
    };
    std::string ct20;
    for (auto &kv : g_rm.ct20)
        ct20 += kv.first + std::to_string(kv.second);
    BLog("RND map=%s r=%d steer=%d ct20=%s alive20=%d hit=%s hitT=%d/%d lurk=%d away=%d rot=%d plant=%s post=%d/%d defuse=%d "
         "exploded=%d win=%s buyT=%s callT=%s primT=%s buyCT=%s callCT=%s primCT=%s trades=%d fallbacks=%d steered=%d rescued=%d %s",
         g_mapName.c_str(), g_roundNo, g_steer && g_layout == 1 ? 1 : 0, ct20.empty() ? "-" : ct20.c_str(), g_rm.ctAlive20,
         g_rm.hit.empty() ? "-" : g_rm.hit.c_str(), g_rm.hitMax, g_rm.tAliveAtHit, g_rm.lurkers, (int)g_rm.away.size(),
         (int)g_rm.rotated.size(), g_rm.plant ? g_rm.plantSite.c_str() : "-", g_rm.postNear, g_rm.postAlive, g_rm.defuse,
         g_rm.exploded, g_rm.winner.empty() ? "-" : g_rm.winner.c_str(), buyStr(2).c_str(), g_rm.buyT.c_str(), g_rm.primT.c_str(), buyStr(3).c_str(),
         g_rm.buyCT.c_str(), g_rm.primCT.c_str(), g_rm.trades, g_rm.fallbacks, g_rm.steered, g_rm.rescued, teamDials(g_pl).c_str());
    (void)why;
}

// ---- the per-tick plan logic ----
static void TickPlan()
{
    if (g_phase != PH_LIVE || !g_steer || g_layout != 1 || !g_mi.ok)
        return;
    float now = CurTime();
    auto ts = Team(2, false);
    TickT12();   // STEP 12D/H: the called plan's groups (it replaces the three step-9 T blocks below this round)
    // T group: go when gathered (or the wait ran out)
    if (!T12() && !g_mi.hostage && g_plan.gather && !g_plan.executed && !g_plan.entering && !g_plan.planted)
    {
        const Site *first = g_plan.fake ? FindSite(g_plan.fakeSite) : FindSite(g_plan.tSite);
        int group = 0, there = 0;
        for (auto *p : Team(2, true))
            if (p->role == "group")
            {
                group++;
                if (first && Dist(p->pos, first->stack) < 450) there++;
            }
        float st = MeanSmart(Team(2, true));
        if (there > 0 && g_plan.firstAtStack == 0)
            g_plan.firstAtStack = now;
        float wait = Lerp(4.f, 16.f, st);
        if (group == 0 || there == group || (g_plan.firstAtStack > 0 && now - g_plan.firstAtStack > wait) || now - g_liveAt > 50.f)
        {
            if (g_plan.fake && g_plan.gatherUntil == 0)
            {
                g_plan.gatherUntil = now + 3.f;   // show up at the fake for a moment, then go
                BLog("plan: fake at %s, then %s", g_plan.fakeSite.c_str(), g_plan.tSite.c_str());
            }
            if (!g_plan.fake || now >= g_plan.gatherUntil)
                TExecute(there == group ? "group together" : "wait over");
        }
    }
    // STEP 9b: at the entrance - in together once all are there, a short wait after the first, or the clock says go
    if (!T12() && !g_mi.hostage && g_plan.entering && !g_plan.executed && !g_plan.planted)
    {
        const Site *s = FindSite(g_plan.tSite);
        int group = 0, there = 0;
        for (auto *p : Team(2, true))
            if (p->role == "group")
            {
                group++;
                if (s && (Dist(p->pos, s->choke) < 400 || Dist(p->pos, s->c) < s->radius + 100)) there++;
            }
        if (there > 0 && g_plan.firstAtDoor == 0)
            g_plan.firstAtDoor = now;
        float wait = Lerp(1.5f, 4.f, MeanSmart(Team(2, true)));
        if (group == 0 || there >= group)
            TGoIn("all at the entrance", there, group);
        else if (g_plan.firstAtDoor > 0 && now - g_plan.firstAtDoor > wait)
            TGoIn("wait over", there, group);
        else if (now - g_plan.enterAt > 10.f || now - g_liveAt > 75.f)
            TGoIn("clock", there, group);
    }
    // hostage guard: CTs at the hostages are logged only. STEP 9c: step 9 collapsed the guards onto the site centre
    // (a MoveTo, then released): released guards turned into roaming hunters and ran down the hostage carriers on the
    // way out. Now the guards keep their holds around the hostages and fight from there.
    if (g_mi.hostage && g_plan.hitSite.empty())
        for (auto &s : g_mi.sites)
        {
            int k = 0;
            for (auto *p : Team(3, false))
                if (Dist(p->pos, s.c) < s.radius + 300) k++;
            if (k >= 1)
            {
                g_plan.hitSite = s.label;
                BLog("plan: CTs at hostages %s - guards hold", s.label.c_str());
                break;
            }
        }
    // STEP 9c: escort - once a CT carries a hostage (stock task 15), the other free CT bots go with him; a carrier
    // walking out alone was the most common lost rescue. The carrier himself is never steered (ObjectiveTask).
    if (g_mi.hostage)
    {
        std::vector<Player *> carriers;
        for (auto *p : Team(3, false))
            if (p->bot && p->ent && BotTask(p->ent) == 15) carriers.push_back(p);
        if (!carriers.empty())
            for (auto *p : Team(3, true))
            {
                if (ObjectiveTask(BotTask(p->ent), true)) continue;
                Player *c = nullptr;
                float bd = 1e9f;
                for (auto *q : carriers)
                    if (Dist(p->pos, q->pos) < bd) { bd = Dist(p->pos, q->pos); c = q; }
                if (!c || bd > 2500.f) continue;
                if (p->task.why == "CT escort")
                {
                    if (Dist(p->task.goal, c->pos) > 200.f) p->task.goal = c->pos;   // keep up with him
                }
                else if (bd > 400.f)
                {
                    Assign(p, T_MOVE, c->pos, 1, 250, "CT escort");
                    if (p->role != "escort") BLog("plan: %s escorts the hostage carrier %s", p->name.c_str(), c->name.c_str());
                    p->role = "escort";
                }
            }
    }
    // CT: a site is hit (called) when a T is on it, 2+ Ts are at it, or the bomber is at it
    if (!g_mi.hostage && g_plan.hitLevel < 2 && !g_plan.planted)
        for (auto &s : g_mi.sites)
        {
            int k = 0, on = 0, nearby = 0;
            bool bomber = false;
            for (auto *p : ts)
            {
                if (Dist(p->pos, s.c) < s.radius + 600) nearby++;
                if (Dist(p->pos, s.c) < s.radius + 250)
                {
                    k++;
                    if (Dist(p->pos, s.c) < s.radius + 100) on++;
                    if (p->name == g_bomber) bomber = true;
                }
            }
            // STEP 9b: also a T on the site with another one close behind (step 9 waited for two at the site, so the
            // rotation often started after the first T was already in)
            if (k >= 2 || bomber || (on >= 1 && nearby >= 2))
            {
                OnSiteHit(s, 2);
                break;
            }
            if (on >= 1)
                OnSiteHit(s, 1);
        }
    CtTick();   // STEP 12C: calls from what the CTs heard / saw, a re-call after a fake
    CtPushTick(now);   // JOB 5b L: the planned push (out, hold, back)
    RotFrame(now);     // JOB 5b N: rotations / retakes under way (ROT lines)
    UtilTick(now);     // JOB 5b O: who throws what, where
    // retake: go in together once gathered (or after the wait)
    if (g_plan.planted && !g_plan.retakeGo)
    {
        const Site *s = FindSite(g_plan.plantSite);
        int group = 0, there = 0;
        for (auto *p : Team(3, true))
            if (p->role == "retake")
            {
                group++;
                if (s && (Dist(p->pos, s->retake) < 500 || Dist(p->pos, g_plan.bomb) < 900)) there++;
            }
        if (there > 0 && g_plan.gatherFirstAt == 0)
            g_plan.gatherFirstAt = now;
        // STEP 9b: go when the whole group is in, or 2+ are in and the rest are 8 s late, or a lone CT is left, or the
        // bomb clock says now (step 9: 2 in or 5 s after the plant)
        if (there >= 2 && g_plan.gatherSecondAt == 0) g_plan.gatherSecondAt = now;
        float late = RetakeOn() ? 4.f : 8.f;   // STEP 12F8: hurry - 4 s after the second one is in (step 9b: 8 s after the first)
        if (group <= 1 || there >= group || (there >= 2 && now - (RetakeOn() ? g_plan.gatherSecondAt : g_plan.gatherFirstAt) > late) ||
            now > g_plan.retakeUntil)
        {
            g_plan.retakeGo = true;
            for (auto *p : Team(3, true))
                if (p->role == "retake")
                {
                    Assign(p, T_MOVE, g_plan.bomb, 1, 350, "CT retake");
                    if (s) SetRoute(p, *s);   // STEP 9b: a late one comes over the CT side too
                    RotStart(p, "bomb");      // JOB 5b N
                }
            BLog("plan: CT retake together (%d/%d gathered)", there, group);
        }
    }
    // the bomber picks his own site (task 1, MoveTo goal): before the hit, the group follows his choice
    if (!T12() && !g_mi.hostage && !g_plan.executed && !g_plan.entering && !g_plan.planted && !g_plan.fake && !g_plan.bomberFollowed)
    {
        auto b = g_pl.find(g_bomber);
        if (b != g_pl.end() && b->second.ent && b->second.alive && BotTask(b->second.ent) == 1 && StateName(b->second.ent) == "11MoveToState" &&
            Dist(b->second.issuedGoal, *(Vec *)(b->second.ent + kOffMoveTo + 4)) > 200.f)
        {
            g_plan.bomberFollowed = true;
            const Site *want = NearestSite(*(Vec *)(b->second.ent + kOffMoveTo + 4));
            if (want && want->label != g_plan.tSite)
            {
                g_plan.tSite = want->label;
                for (auto *p : Team(2, true))
                    if (p->role == "group" && g_plan.gather) Assign(p, T_HOLD, want->stack, 2, 250, "T gather (bomber's site)", 25);
                    else if (p->role == "trickle") Assign(p, T_MOVE, want->c, 2, want->radius * 0.6f, "T go site alone");
                    else if (p->role == "lurk") Assign(p, T_HOLD, want->lurk, 2, 150, "T lurk", 60);
                BLog("plan: the bomber heads for %s - the group follows", want->label.c_str());
            }
        }
    }
    // T bomber: once on a site during the hit, leave him to the stock AI (it plants)
    auto bit = g_pl.find(g_bomber);
    if (bit != g_pl.end() && bit->second.task.kind != T_NONE)
        for (auto &s : g_mi.sites)
            if (Dist(bit->second.pos, s.c) < s.radius * 0.7f && (s.label == g_plan.tSite || g_plan.executed))
                Release(&bit->second);
}

// ---- steering one bot towards its task ----
static int SafeRoute(Player &p, int route);   // STEP 10b
static bool J5Busy(const Player &p);          // STEP 12 (job-5 code)
static int CtChase(Player &p, const std::string &st);   // -1 not a holding CT, 0 holds, 1 chases
static bool StockRetarget(Player &p, const Vec &goal);  // JOB 5c Q / T (below)
static void Steer(Player &p)
{
    Task &t = p.task;
    if (t.kind == T_NONE || !p.alive || !p.ent)
        return;
    float now = CurTime();
    if (now < t.startAt)
        return;
    if (p.sh.panic || now - p.sh.unholdAt < 2.f)
        return;   // STEP 11: an enemy is on top of it - it fights (panic), no task until that is over
    if (J5Busy(p))
        return;   // STEP 12G: it is breaking line of sight / re-peeking under fire
    if (UtilBusy(p))
        return;   // JOB 5b O: it is throwing a grenade
    std::string st = StateName(p.ent);
    // the bot's own business comes first: fighting, the bomb, buying, doors, escaping, hostages
    if (st != "9IdleState" && st != "9HuntState" && st != "11MoveToState" && st != "9HideState" && st != "11FollowState" &&
        st != "21InvestigateNoiseState")
        return;
    int chase = CtChase(p, st);   // STEP 12C: a holding CT chases a noise only by its roll (hearing turns it instead)
    if (chase == 1)
        return;
    if (chase < 0 && st == "21InvestigateNoiseState" && p.s < 0.6f && !RotUrgent(p))
        return;   // only a smart holder ignores a noise to keep its angle (JOB 5b N: a rotation / retake never stops for one)
    // STEP 9b: the retake gather too - the stock "find / defuse the bomb" task sent each CT in alone, so step 9's gather
    // mostly never happened (the defuse itself is DefuseBombState, which is never steered)
    // STEP 12F8: the retake AWP's angle and the lead CT's short wait are retake steps too (else the stock defuse task
    // walked them straight onto the bomb)
    if (ObjectiveTask(BotTask(p.ent), g_plan.executed || g_plan.planted || g_mi.hostage) &&
        !(t.kind == T_MOVE && p.task.why == "CT retake") && p.task.why != "CT gather for retake" && p.task.why != "CT retake AWP angle" &&
        p.task.why != "CT retake: wait for the next one")
        return;   // on its way to plant / defuse / the hostages: its own job
    // STEP 9b: waypoints first (a rotation over the CT side); one is passed within 250 or once the bot is nearer the
    // next point than the waypoint itself is
    Vec goal = t.goal;
    while (t.wp < t.via.size())
    {
        const Vec &w = t.via[t.wp];
        const Vec &nx = t.wp + 1 < t.via.size() ? t.via[t.wp + 1] : t.goal;
        if (Dist(p.pos, w) < 250.f || Dist(p.pos, nx) < Dist(w, nx))
        {
            t.wp++;
            p.bestDist = 1e9f;
            p.progressAt = now;
            continue;
        }
        goal = w;
        break;
    }
    bool onWay = t.wp < t.via.size();
    float d = Dist(p.pos, goal);
    if (!onWay && d <= t.arrive)
    {
        if (t.kind == T_MOVE)
        {
            if (J5bTradeBack(p)) return;   // JOB 5b L: a CT back from a trade run holds again (not the stock hunt)
            Release(&p);
            return;
        }
        // hold: hide here and keep holding (HideState looks at the approaches and waits)
        uintptr_t hideSt = p.ent + kOffHide;
        bool hiding = st == "9HideState" && Dist(*(Vec *)(hideSt + 12), p.pos) < t.arrive + 100;
        if (!hiding && now - p.issuedAt > 1.0f && BotHideHere(p.ent, p.pos, t.holdFor))
        {
            p.issuedAt = now;
            p.issues++;
        }
        return;
    }
    // on the way: (re)issue the move when the bot is doing something else or is stuck
    // (the bot may move its MoveTo goal onto the nav mesh, so "ours" = the last goal we issued, not the state's field)
    bool movingThere = st == "11MoveToState" && Dist(p.issuedGoal, goal) < 1.f && now - p.issuedAt < 30.f;
    // JOB 5c Q / T: the stock AI re-targets its own MoveTo (a noise, a hunt, the bomber's own site) while "our" goal stays
    // recorded - then the bot is NOT on its way, whatever issuedGoal says
    bool retarget = movingThere && StockRetarget(p, goal);
    if (retarget) movingThere = false;
    if (d < p.bestDist - 40)
    {
        p.bestDist = d;
        p.progressAt = now;
    }
    bool stuck = now - p.progressAt > 8.f;
    bool newGoal = Dist(p.issuedGoal, goal) >= 1.f;   // the next waypoint: no need to wait out the 1 s
    if ((!movingThere || stuck) && now - p.issuedAt > (newGoal || RotUrgent(p) ? 0.25f : 1.0f))   // JOB 5b N: back on the way at once
    {
        BotMoveTo(p.ent, goal, RotRoute(p, goal, SafeRoute(p, t.route)));   // STEP 10b: B5; JOB 5b N: sneaky when needed
        p.issuedAt = now;
        p.issuedGoal = goal;
        p.snapGoal = *(Vec *)(p.ent + kOffMoveTo + 4);   // JOB 5c: OnEnter ran inside BotMoveTo
        if (!retarget || stuck) p.issues++;   // JOB 5c: setting a re-targeted MoveTo back is not "cannot reach it"
        if (stuck)
        {
            p.progressAt = now;
            p.bestDist = d;
        }
        if (p.issues > 40)
            Release(&p);   // give up on a goal it cannot reach
    }
}

// ==== STEP 10 (2026-09-24): per-bot dials + footwork ================================================================
// Dials (0..1 each, per bot): value = clamp(s_live + the bot's own offset + a small per-match form), s_live =
// smoothstep((live elo - 300) / 2100). Offsets come from csgo/addons/family_dials.txt (bots/make_botprofile.py, seeded
// per bot name, fair: they average out to the bot's elo). The profile dials (aim, Skill = footwork + awareness,
// aggression, teamwork) are written into botprofile.db by the same script; the PLUGIN dials here are ramped to 0 at
// elo 300 (x (elo - 300) / 400 up to elo 700), so a bottom bot plays like a stock bot or worse (kid floor).
// Footwork (plugin part): quiet walk - a bot that rolled "walker" this round (chance = its footwork) walks at shift
// speed where the enemy can already be (nav earliest-occupy grid vs the round clock) and a living enemy is in hearing
// range, never while fighting (AttackState) or on an urgent job (rotation, retake, trade, the hit, defuse, escape);
// stop-to-shoot - when a fight starts the bot (chance = footwork) stops dead for its first shot and after each shot.
// Lever: CCSPlayer::m_flVelocityModifier (the game's own "slowed when hit" speed scale, found by name in the server's
// send tables every load; the bot's real speed drops, so steps go quiet and it looks like walking, not slow motion).
// Metrics: player_footstep / weapon_fire / player_hurt / player_jump / player_death game events -> one "BOTR" line per
// bot per round. Kill switches (addons/family_test.txt): brain_walk 0, brain_stopshot 0, brain_dials 0 (offsets off).
typedef void *(*BrainIfaceFn)(const char *, int *);
static double (*g_tv)(const char *, double);
static double TestHook(const char *k, double d) { return g_tv ? g_tv(k, d) : d; }
// STEP 10b (2026-09-25): B5 safe routes + cover defaults (test hooks brain_safe / brain_cover, A/B: safe_team 2|3).
// B7 jump discipline was tried and REMOVED: CBot::m_jumpTimestamp (0x3bd4 on 1.38.8.1, it held the clock at 1251 of 1251
// bot jumps) kept at "0.35 s ago" blocks the optional jumps, but the bots' jumps are path jumps - they jumped when they
// slowed instead (jumps -6% / +4%, slow jumps x5, stuck episodes +20-27%, fewer rounds won). Proof: step10 logs b3_j*.
static const int kSafeDefault = 1, kCoverDefault = 0;
static struct Tv
{
    int walk = 1, stop = 1, dials = 1, lever = 0, force = 0, forceTeam = 0, trace = 0, ctMove = 0;
    int safe = 0, safeTeam = 0;                    // STEP 10b: B5 safe routes; A/B tests: only this team (2 T, 3 CT)
    int cover = 0;                                 // STEP 10b: B5 cover (an exposed hiding spot -> the nearest IN_COVER one)
    float mult = 0.52f, tagRate = 0.4f, fwT = -1, fwCT = -1, stopFirst = 0.35f, stopShot = 0.22f, bandLo = 700, bandHi = 1600;
    time_t at = 0;
} g_t;
static void RefreshTest()
{
    time_t now = time(nullptr);
    if (!g_tv || now == g_t.at)
        return;
    g_t.at = now;
    g_t.walk = (int)g_tv("brain_walk", 1);
    g_t.stop = (int)g_tv("brain_stopshot", 1);
    g_t.dials = (int)g_tv("brain_dials", 1);
    g_t.lever = (int)g_tv("fw_lever", 0);          // 0 m_flVelocityModifier, 1 m_flLaggedMovementValue (measurement only)
    g_t.force = (int)g_tv("fw_force", 0);          // measurement: walk everywhere outside fights
    g_t.forceTeam = (int)g_tv("fw_force_team", 0); // ... only this team (2 T, 3 CT)
    g_t.trace = (int)g_tv("fw_trace", 0);
    g_t.mult = (float)g_tv("fw_mult", 0.52);
    g_t.tagRate = (float)g_tv("fw_tagrate", 0.4);
    g_t.fwT = (float)g_tv("fwT", -1);              // A/B tests: this team's footwork, absolute
    g_t.fwCT = (float)g_tv("fwCT", -1);
    g_t.stopFirst = (float)g_tv("fw_stop_first", 0.35);   // stop this long when a fight starts
    g_t.stopShot = (float)g_tv("fw_stop_shot", 0.22);     // ... and after each shot
    g_t.bandLo = (float)g_tv("fw_band_lo", 700);          // walk only with the nearest enemy this far (closer = a fight: run) ...
    g_t.bandHi = (float)g_tv("fw_band_hi", 1600);         // ... up to this far (footstep hearing range)
    g_t.ctMove = (int)g_tv("fw_ct_moveto", 0);            // 0: a CT on its way somewhere (MoveTo) runs - the site needs him
    g_t.safe = (int)g_tv("brain_safe", kSafeDefault);     // STEP 10b: B5 safe routes (footwork dial)
    g_t.cover = (int)g_tv("brain_cover", kCoverDefault);
    g_t.safeTeam = (int)g_tv("safe_team", 0);
    char k[400];
    snprintf(k, sizeof(k), "walk %d stop %d dials %d lever %d force %d/%d trace %d mult %.2f tagrate %.2f fwT %.2f fwCT %.2f stop %.2f/%.2f band %.0f-%.0f"
             " safe %d/%d cover %d",
             g_t.walk, g_t.stop, g_t.dials, g_t.lever, g_t.force, g_t.forceTeam, g_t.trace, g_t.mult, g_t.tagRate, g_t.fwT, g_t.fwCT,
             g_t.stopFirst, g_t.stopShot, g_t.bandLo, g_t.bandHi, g_t.safe, g_t.safeTeam, g_t.cover);
    static std::string last;
    if (last != k)
        BLog("test hooks: %s", k);
    last = k;
}

// ---- send-table offsets (by name, every load) ----
static struct NetProps
{
    int lag = -1, velmod = -1, walking = -1, account = -1, stamina = -1, flags = -1;
    int eyeP = -1, eyeY = -1, punch = -1, punchVel = -1, shotsFired = -1, health = -1;   // STEP 11 (health: circle test only)
    int activeWeapon = -1, myWeapons = -1;                // JOB 5b O: the weapon in hand, the carried ones (EHANDLEs)
    int pin = -1, throwTime = -1, strength = -1;          // JOB 5b O: CBaseCSGrenade (every grenade class must agree)
    int redraw = -1;                                      // JOB 5b O: m_bRedraw (the same; -2 = not checked)
    bool ok = false;
} g_np;
static int PropOffset(uintptr_t table, const char *name, int base, int depth)
{
    uintptr_t props, n;
    if (depth > 8 || !SafeWord(table, props) || !SafeWord(table + 4, n) || n > 4000)
        return -1;
    for (uintptr_t j = 0; j < n; j++)
    {
        uintptr_t p = props + j * 0x54, type, nm, dt, off;   // SendProp: +8 m_Type, +0x30 m_pVarName, +0x48 m_pDataTable, +0x4c m_Offset
        if (!SafeWord(p + 8, type) || !SafeWord(p + 0x30, nm) || !SafeWord(p + 0x48, dt) || !SafeWord(p + 0x4c, off))
            return -1;
        if (type == 6 && dt)
        {
            // STEP 11: some tables carry flag bits above the offset (0x100000 m_Local, 0x300000 the eye angles on 1.38.8.1;
            // used raw, a prop found there read 3 MB past the player and killed a test server): keep the low 20 bits
            if (off >= 0x100000)
                off &= 0xfffff;
            int r = PropOffset(dt, name, base + (int)off, depth + 1);
            if (r >= 0) return r;
            continue;
        }
        if (nm && SafeStr(nm, 64) == name)
            return base + (int)(off & 0xfffff);   // STEP 11: flag bits above the offset (see above)
    }
    return -1;
}
// JOB 5b O: the offset of a DATA TABLE prop by name (an array like m_hMyWeapons is a table of "000".."063")
static int PropTableOffset(uintptr_t table, const char *name, int base, int depth)
{
    uintptr_t props, n;
    if (depth > 8 || !SafeWord(table, props) || !SafeWord(table + 4, n) || n > 4000)
        return -1;
    for (uintptr_t j = 0; j < n; j++)
    {
        uintptr_t p = props + j * 0x54, type, nm, dt, off;
        if (!SafeWord(p + 8, type) || !SafeWord(p + 0x30, nm) || !SafeWord(p + 0x48, dt) || !SafeWord(p + 0x4c, off))
            return -1;
        if (type != 6 || !dt) continue;
        off &= 0xfffff;
        if (nm && SafeStr(nm, 64) == name) return base + (int)off;
        int r = PropTableOffset(dt, name, base + (int)off, depth + 1);
        if (r >= 0) return r;
    }
    return -1;
}
static void FindNetProps(BrainIfaceFn serverFactory)
{
    g_np = NetProps();
    void *gd = serverFactory ? serverFactory("ServerGameDLL005", nullptr) : nullptr;
    uintptr_t vt, fn, gp, sc;
    uint8_t code[10];
    // IServerGameDLL vtable[10] GetAllServerClasses is "push ebp; mov eax,[g_pServerClassHead]; mov ebp,esp; pop ebp; ret"
    // (measured on 1.38.8.1): the head is read from the global, the function is never called
    if (!gd || !SafeWord((uintptr_t)gd, vt) || !SafeWord(vt + 40, fn) || !SafeRead((void *)fn, code, 10) || code[0] != 0x55 ||
        code[1] != 0xa1 || code[6] != 0x89 || code[7] != 0xe5 || code[8] != 0x5d || code[9] != 0xc3)
    {
        BLog("footwork: server class list not found - footwork off (stock movement)");
        return;
    }
    memcpy(&gp, code + 2, 4);
    if (!SafeWord(gp, sc))
        return;
    for (int i = 0; sc && i < 2000; i++)
    {
        uintptr_t nm, tb, nx;
        if (!SafeWord(sc, nm) || !SafeWord(sc + 4, tb) || !SafeWord(sc + 8, nx))
            break;
        if (SafeStr(nm) == "CCSPlayer")
        {
            g_np.lag = PropOffset(tb, "m_flLaggedMovementValue", 0, 0);
            g_np.velmod = PropOffset(tb, "m_flVelocityModifier", 0, 0);
            g_np.walking = PropOffset(tb, "m_bIsWalking", 0, 0);
            g_np.account = PropOffset(tb, "m_iAccount", 0, 0);
            g_np.stamina = PropOffset(tb, "m_flStamina", 0, 0);
            g_np.flags = PropOffset(tb, "m_fFlags", 0, 0);
            // STEP 11: eye angles (sight / turn metrics), aim punch (recoil control), shots in the current spray
            g_np.eyeP = PropOffset(tb, "m_angEyeAngles[0]", 0, 0);
            g_np.eyeY = PropOffset(tb, "m_angEyeAngles[1]", 0, 0);
            g_np.punch = PropOffset(tb, "m_aimPunchAngle", 0, 0);
            g_np.punchVel = PropOffset(tb, "m_aimPunchAngleVel", 0, 0);
            g_np.shotsFired = PropOffset(tb, "m_iShotsFired", 0, 0);
            g_np.health = PropOffset(tb, "m_iHealth", 0, 0);
            for (int *o : {&g_np.eyeP, &g_np.eyeY, &g_np.punch, &g_np.punchVel, &g_np.shotsFired, &g_np.health})
                if (*o <= 0 || *o >= 0x6000) *o = -1;   // must be a member of the player object
            BLog("shoot netprops: eye 0x%x/0x%x punch 0x%x punchVel 0x%x shotsFired 0x%x", g_np.eyeP, g_np.eyeY, g_np.punch, g_np.punchVel,
                 g_np.shotsFired);
            // JOB 5b O: the weapon in hand and the carried weapons (arrays of EHANDLEs)
            g_np.activeWeapon = PropOffset(tb, "m_hActiveWeapon", 0, 0);
            g_np.myWeapons = PropTableOffset(tb, "m_hMyWeapons", 0, 0);
            for (int *o : {&g_np.activeWeapon, &g_np.myWeapons})
                if (*o <= 0 || *o >= 0x6000) *o = -1;
        }
        else
        {
            // JOB 5b O: the grenades' own fields (CBaseCSGrenade), found in every grenade class - they must all agree
            std::string cn = SafeStr(nm);
            if (cn == "CSmokeGrenade" || cn == "CFlashbang" || cn == "CHEGrenade" || cn == "CMolotovGrenade" || cn == "CIncendiaryGrenade" ||
                cn == "CDecoyGrenade")
            {
                int pin = PropOffset(tb, "m_bPinPulled", 0, 0), tt = PropOffset(tb, "m_fThrowTime", 0, 0), st = PropOffset(tb, "m_flThrowStrength", 0, 0);
                bool okk = pin > 0 && pin < 0x4000 && tt > 0 && tt < 0x4000 && st > 0 && st < 0x4000;
                if (!okk || (g_np.pin > 0 && (g_np.pin != pin || g_np.throwTime != tt || g_np.strength != st)))
                    g_np.pin = g_np.throwTime = g_np.strength = -2;   // missing or not the same in every class: the lever stays off
                else if (g_np.pin != -2)
                {
                    g_np.pin = pin;
                    g_np.throwTime = tt;
                    g_np.strength = st;
                }
                int rd = PropOffset(tb, "m_bRedraw", 0, 0);   // just thrown, the next one not out yet: never pull a pin then
                if (rd <= 0 || rd >= 0x4000 || (g_np.redraw > 0 && g_np.redraw != rd)) g_np.redraw = -2;
                else if (g_np.redraw != -2) g_np.redraw = rd;
            }
        }
        sc = nx;
    }
    BLog("util netprops: active weapon 0x%x, weapons 0x%x, grenade pin 0x%x throw time 0x%x strength 0x%x redraw 0x%x%s", g_np.activeWeapon,
         g_np.myWeapons, g_np.pin, g_np.throwTime, g_np.strength, g_np.redraw, g_np.redraw > 0 ? "" : " (redraw not checked)");
    // layout check: m_iAccount from the tables must be the measured 0x2dbc the money code uses
    g_np.ok = g_np.velmod > 0 && g_np.lag > 0 && g_np.account == 0x2dbc;
    BLog("footwork netprops: velmod 0x%x lag 0x%x walking 0x%x account 0x%x stamina 0x%x flags 0x%x: %s", g_np.velmod, g_np.lag,
         g_np.walking, g_np.account, g_np.stamina, g_np.flags, g_np.ok ? "OK" : "MISMATCH - footwork off (stock movement)");
}

// ---- dials ----
// STEP 11: + react (time to first shot) and spray (trigger discipline / recoil control), profile-style dials (no ramp)
enum { D_AIM, D_FOOT, D_ANGLES, D_SENSE, D_AWARE, D_UTIL, D_AGGR, D_PATIENCE, D_TEAM, D_ECON, D_REACT, D_SPRAY, D_N };
static const char *kDialName[D_N] = {"aim", "footwork", "angles", "sense", "aware", "utility", "aggression", "patience", "teamwork", "economy", "react", "spray"};
static const char *kDialsPath = "csgo/addons/family_dials.txt";
struct DialRow
{
    float v[D_N] = {};
    bool abs[D_N] = {};
    std::string tmpl;   // STEP 10c: the roster weapon template ("tmpl <name>" in the dials file)
};
static std::map<std::string, DialRow> g_dials;
static unsigned Fnv(const std::string &s)
{
    unsigned h = 2166136261u;
    for (unsigned char c : s) h = (h ^ c) * 16777619u;
    return h;
}
static void LoadDials()
{
    g_dials.clear();
    FILE *f = fopen(kDialsPath, "r");
    if (!f)
    {
        BLog("dials: %s missing - every bot on its elo curve", kDialsPath);
        return;
    }
    char line[1024];
    while (fgets(line, sizeof(line), f))
    {
        if (line[0] == '/' || line[0] == '#')
            continue;
        char name[64], dn[32], val[32];
        int n = 0;
        char *p = line;
        if (sscanf(p, "%63s%n", name, &n) != 1)
            continue;
        p += n;
        DialRow r;
        while (sscanf(p, "%31s %31s%n", dn, val, &n) == 2)
        {
            p += n;
            if (!strcmp(dn, "tmpl"))
                r.tmpl = val;
            for (int d = 0; d < D_N; d++)
                if (!strcmp(dn, kDialName[d]))
                {
                    r.abs[d] = val[0] == '=';
                    r.v[d] = (float)atof(val + (val[0] == '=' ? 1 : 0));
                }
        }
        g_dials[name] = r;
    }
    fclose(f);
    BLog("dials: %d bots in %s", (int)g_dials.size(), kDialsPath);
}
static const char *BotTmpl(const std::string &name)   // STEP 10c
{
    auto it = g_dials.find(name);
    return it == g_dials.end() ? "" : it->second.tmpl.c_str();
}
static float SLive(double elo)
{
    float x = std::max(0.f, std::min(1.f, (float)((elo - 300.0) / 2100.0)));
    return x * x * (3 - 2 * x);
}
// plugin = a plugin dial (ramped to 0 at elo 300 + form jitter); a profile dial is only logged here
static float DialOf(const Player &p, int d, bool plugin)
{
    double elo = g_eloOf ? g_eloOf(p.name.c_str()) : 1000.0;
    if (d == D_FOOT && plugin)
    {
        float o = p.team == 2 ? g_t.fwT : p.team == 3 ? g_t.fwCT : -1.f;
        if (o >= 0) return std::min(1.f, o);   // A/B test: absolute for the whole team
    }
    float v = SLive(elo);
    auto it = g_dials.find(p.name);
    if (it != g_dials.end() && g_t.dials)
    {
        if (it->second.abs[d]) return std::max(0.f, std::min(1.f, it->second.v[d]));
        v += it->second.v[d];
    }
    if (!plugin)
        return std::max(0.f, std::min(1.f, v));
    v += ((Fnv(p.name + ":" + kDialName[d] + ":" + std::to_string(g_matchSeed)) % 10001) / 10000.f - 0.5f) * 0.06f;   // form +-0.03
    float ramp = std::max(0.f, std::min(1.f, (float)((elo - 300.0) / 400.0)));
    return std::max(0.f, std::min(1.f, v)) * ramp;
}

// ---- game events (IGameEventManager2, engine GAMEEVENTSMANAGER002; Linux vtables carry two destructor slots) ----
//   manager: 4 AddListener(listener, name, serverSide), 6 RemoveListener(listener)
//   event:   2 GetName, 7 GetInt(key, default), 10 GetString(key, default)      (measured: SetInt = 14, AddListener = +0x10)
struct EvListener
{
    void **vt;
};
static void *g_evMgr;
static void EvDtor(EvListener *) {}
static int EvDebugId(EvListener *) { return 42; }   // EVENT_DEBUG_ID_INIT
static void EvFire(EvListener *, void *ev);
static void *g_evVt[4] = {(void *)EvDtor, (void *)EvDtor, (void *)EvFire, (void *)EvDebugId};
static EvListener g_evL = {g_evVt};
static const char *kEvents[] = {"player_footstep", "weapon_fire", "player_hurt", "player_jump", "player_death",
                                "weapon_reload", "bomb_beginplant", "bomb_begindefuse", "weapon_zoom",   // STEP 12B: + the other noises
                                "item_equip",                                                            // STEP 12E: the weapon in hand
                                "molotov_detonate", "hegrenade_detonate", "flashbang_detonate", "smokegrenade_detonate",
                                "player_blind",                                                          // STEP 12F9: grenades
                                "grenade_thrown"};                                                       // JOB 5b O: a throw left the hand
static std::map<int, std::string> g_byUid;
static Player *ByUid(int uid)
{
    auto it = g_byUid.find(uid);
    if (it == g_byUid.end()) return nullptr;
    auto p = g_pl.find(it->second);
    return p == g_pl.end() ? nullptr : &p->second;
}
static bool NearEnemy(const Player &p, float r)
{
    for (auto &kv : g_pl)
        if (kv.second.alive && (kv.second.team == 2 || kv.second.team == 3) && kv.second.team != p.team && Dist(kv.second.pos, p.pos) < r)
            return true;
    return false;
}
static bool IsGun(const char *w)
{
    static const char *no[] = {"knife", "bayonet", "hegrenade", "flashbang", "smokegrenade", "molotov", "incgrenade", "decoy", "c4", "taser"};
    for (auto *n : no)
        if (strstr(w, n)) return false;
    return true;
}
struct VmTrace
{
    std::string name;
    float at;
    int k;
};
static std::vector<VmTrace> g_vmTrace;
// STEP 11 (defined below)
static void ShootOnFire(Player &p, const char *w, float now);
static void ShootOnHurt(Player *a, Player *v, int group, int dmg, float now);
static void ShootOnDeath(Player *v, Player *a, float now);
static void HearEvent(const char *n, Player *p, void *ev, float now);   // STEP 12B (defined with the job-5 code)
static void FireOnShot(Player &shooter, float now);                      // STEP 12G
static void NadeEvent(const char *n, Player *p, void *ev, float now);    // STEP 12F9
static void UtilEvent(const char *n, Player *p, void *ev, float now);    // JOB 5b O
static void FireOnHurt(Player *a, Player *v, int dmg, float now);
static void WalkupHurt(Player *a, Player *v, int group, int dmg, float now);
static void EvFire(EvListener *, void *ev)
{
    if (!ev)
        return;
    const char *n = VF<const char *(*)(void *)>(ev, 2)(ev);
    if (!n)
        return;
    if (!strcmp(n, "item_equip"))   // STEP 12E/F: the weapon in its hands (the freeze's buys too)
    {
        Player *q = ByUid(VF<int (*)(void *, const char *, int)>(ev, 7)(ev, "userid", 0));
        const char *it = VF<const char *(*)(void *, const char *, const char *)>(ev, 10)(ev, "item", "");
        if (q && it) q->weapon = it;
        return;
    }
    if (g_phase != PH_LIVE)
        return;
    auto GetInt = [&](const char *k) { return VF<int (*)(void *, const char *, int)>(ev, 7)(ev, k, 0); };
    float now = CurTime();
    Player *p = ByUid(GetInt("userid"));
    HearEvent(n, p, ev, now);   // STEP 12B: every noise, humans' too
    NadeEvent(n, p, ev, now);   // STEP 12F9: grenade throws / detonations / team flashes
    UtilEvent(n, p, ev, now);   // JOB 5b O: our throws leaving the hand, a defuse to burn
    if (!strcmp(n, "player_footstep"))
    {
        if (p && p->bot)
        {
            p->m.steps++;
            if (NearEnemy(*p, 1500.f)) p->m.near++;
        }
    }
    else if (!strcmp(n, "player_jump"))
    {
        if (p && p->bot)
        {
            p->m.jumps++;
            if (p->inAttack) p->m.jumpsF++;
            // STEP 10b: classified 0.8 s later by the height it landed at (FootFrame): up = onto something, flat, down
            p->jumpOpen = true;
            p->jumpAt = now;
            p->jumpZ = p->lastPos.z;
            p->jumpSp = p->speed;
        }
    }
    else if (!strcmp(n, "weapon_fire"))
    {
        const char *w = VF<const char *(*)(void *, const char *, const char *)>(ev, 10)(ev, "weapon", "");
        if (p && w && IsGun(w)) p->sh.firedAt = now;   // STEP 11 (humans too)
        if (p && w && IsGun(w)) FireOnShot(*p, now);    // STEP 12G: whom is he aiming at
        if (!p || !p->bot || !w || !IsGun(w))
            return;
        ShootOnFire(*p, w, now);   // STEP 11
        p->m.shots++;
        if (BotAttacking(p->ent)) p->m.shotsAtk++;
        if (now - p->lastFire > 1.0f)
        {
            // the first shot of an exchange: moving or stopped (hit = a player_hurt by this bot within 0.15 s)
            p->m.fs++;
            p->fsOpen = true;
            p->fsAt = now;
            p->fsMoving = p->speed > 80.f;
            if (p->fsMoving) p->m.fsMov++;
        }
        if (p->reactOpen && p->inAttack && now - p->attackAt < 3.f)
        {
            p->m.react += now - p->attackAt;
            p->m.reactN++;
        }
        p->reactOpen = false;
        p->lastFire = now;
        if (p->stopper && p->inAttack)
            p->stopUntil = std::max(p->stopUntil, now + g_t.stopShot);
    }
    else if (!strcmp(n, "player_hurt"))
    {
        Player *a = ByUid(GetInt("attacker"));
        ShootOnHurt(a, p, GetInt("hitgroup"), GetInt("dmg_health"), now);   // STEP 11
        FireOnHurt(a, p, GetInt("dmg_health"), now);                        // STEP 12G
        WalkupHurt(a, p, GetInt("hitgroup"), GetInt("dmg_health"), now);    // STEP 12K (test)
        if (a && a->bot && a != p && p && p->team != a->team)
        {
            a->m.hits++;
            if (a->fsOpen && now - a->fsAt <= 0.15f)
            {
                a->m.fsHit++;
                if (a->fsMoving) a->m.fsMovHit++;
                a->fsOpen = false;
            }
        }
        if (g_t.trace && p && p->bot && !p->modOn)
        {
            g_vmTrace.push_back({p->name, now, 0});
            static int tn;
            if (tn++ < 3) BLog("trace push %s", p->name.c_str());
        }
    }
    else if (!strcmp(n, "player_death"))
    {
        Player *a = ByUid(GetInt("attacker"));
        ShootOnDeath(p, a, now);   // STEP 11
        if (p && p->bot) p->m.deaths++;
        if (a && a->bot && a != p && p && a->team != p->team) a->m.kills++;
        // STEP 10b: where bots die (B5: deaths on open ground, classified offline from the nav areas - the navs carry no
        // visibility lists)
        if (p && p->bot && (p->team == 2 || p->team == 3))
            BLog("DEATH map=%s r=%d %s team=%s fw=%.2f safe=%d t=%.1f x=%.0f y=%.0f z=%.0f", g_mapName.c_str(), g_roundNo, p->name.c_str(),
                 p->team == 2 ? "T" : "CT", p->fw, (int)p->safe, now - g_liveAt, p->lastPos.x, p->lastPos.y, p->lastPos.z);
    }
}
static void HookEvents(BrainIfaceFn engineFactory)
{
    g_evMgr = engineFactory ? engineFactory("GAMEEVENTSMANAGER002", nullptr) : nullptr;
    if (!g_evMgr)
    {
        BLog("events: no GAMEEVENTSMANAGER002 - footwork metrics off");
        return;
    }
    int ok = 0;
    for (auto *e : kEvents)
        ok += VF<bool (*)(void *, void *, const char *, bool)>(g_evMgr, 4)(g_evMgr, &g_evL, e, true) ? 1 : 0;
    BLog("events: listening to %d of %d game events", ok, (int)(sizeof(kEvents) / sizeof(kEvents[0])));
}

// ---- quiet walk / stop-to-shoot (every server frame) ----
static uint16_t OccAt(int team, const Vec &v)   // earliest time (1/10 s) team 2/3 can be within 800 units of v
{
    if (!g_mi.onx || (team != 2 && team != 3)) return 1200;
    int ix = (int)((v.x - g_mi.ox) / g_mi.ocell), iy = (int)((v.y - g_mi.oy) / g_mi.ocell);
    if (ix < 0 || iy < 0 || ix >= g_mi.onx || iy >= g_mi.ony) return 1200;
    return g_mi.occ[team == 2 ? 0 : 1][iy * g_mi.onx + ix];
}
static bool Urgent(const Player &p, int task)
{
    if (p.role == "rotate" || p.role == "retake" || p.role == "trade" || p.role == "escort" || p.role == "fallback") return true;
    if (p.task.why == "T hit" || p.task.why == "lurker joins" || p.task.why == "CT retake" || p.task.why == "trade") return true;
    if (task == 2 || task == 3 || task == 5 || task == 9 || task == 15) return true;   // find / defuse the bomb, escape, rescue
    if (g_plan.planted && p.team == 3) return true;                                    // a retake runs
    if (!g_mi.hostage && p.team == 2 && CurTime() - g_liveAt > 75.f && !g_plan.planted) return true;   // Ts need the time
    return p.name == g_bomber && g_plan.executed;
}
static bool WalkHere(Player &p, const std::string &st)
{
    if (!g_t.walk || g_phase != PH_LIVE || !g_mi.ok || !(p.walker || g_t.force))
        return false;
    if (st != "9IdleState" && st != "9HuntState" && st != "11MoveToState" && st != "9HideState" && st != "11FollowState" &&
        st != "21InvestigateNoiseState")
        return false;
    if (g_t.force)
        return !g_t.forceTeam || g_t.forceTeam == p.team;
    if (Urgent(p, BotTask(p.ent)))
        return false;
    if (p.team == 3 && st == "11MoveToState" && !g_t.ctMove)
        return false;
    // a CT walks only while nothing is known yet: once a site is called or the bomb is down it hurries (the step 9b
    // rotation), and on hostage maps the CTs are the attackers against the clock (the rescue) - they never walk
    if (p.team == 3 && (g_mi.hostage || g_plan.hitLevel >= 1 || g_plan.planted))
        return false;
    float t = CurTime() - g_liveAt;
    return OccAt(p.team == 2 ? 3 : 2, p.pos) <= (int)((t + 2.f) * 10) && NearEnemy(p, g_t.bandHi) && !(g_t.bandLo > 0 && NearEnemy(p, g_t.bandLo));
}
static void ApplyMove(Player &p, float target, float now)
{
    int off = g_t.lever == 1 ? g_np.lag : g_np.velmod;
    float *f = (float *)(p.ent + off);
    float cur = *f;
    if (!(cur >= 0.f && cur <= 1.5f))
        return;   // not a speed scale: never write
    if (p.modOn && g_t.lever == 0 && cur < p.wrote - 0.002f)
    {
        p.tagVal = cur;   // the game lowered it (the bot was hit): keep the game's slow-down
        p.tagAt = now;
    }
    float natural = 1.f;
    if (g_t.lever == 0 && p.tagAt > 0)
        natural = std::min(1.f, p.tagVal + g_t.tagRate * (now - p.tagAt));
    if (target < 1.f)
    {
        float w = std::min(natural, target);
        *f = w;
        p.wrote = w;
        p.modOn = true;
    }
    else if (p.modOn)
    {
        *f = natural;   // restore (a fight, a state change, death, round end, a kill switch)
        p.modOn = false;
        p.tagAt = -100;
    }
}
// ---- STEP 10b: B5 safe routes + cover, jump / stuck / route metrics (every frame, live bots) ----
// B5: a "safe router" (rolled per round, chance = its footwork dial, so never at footwork 0 / elo 300) takes the game's
// own SAFEST route (path cost adds the nav danger where its team died - it stops walking into the same angle) instead
// of the fastest one, for its own moves and ours, except on an urgent job (rotation, retake, trade, bomb, hostages) and
// never for a CT on a hostage map (the attacker against the clock, as for the quiet walk). Cover (brain_cover): the same
// bot, when it picks an EXPOSED nav hiding spot itself, hides at the nearest IN_COVER one within 350 units instead.
// Measured (b3_s*, 180 rounds, lever side vs the same side without): deaths on open ground 48% -> 40%, rotations and
// defuses not down, plants the same. Hostage maps (the Ts' routes): cs_office rescued 72% (65/90) vs 39% (127/329)
// without, cs_italy 51% (46/90) vs 60% (108/180) - rescues pooled 62% vs 46%.
static bool SafeOn(const Player &p) { return g_t.safeTeam ? g_t.safeTeam == p.team : p.safe; }   // A/B: the whole team
static bool SafeRoutes(const Player &p) { return g_t.safe && SafeOn(p) && !(g_mi.hostage && p.team == 3); }
static void FrameB(Player &p, const std::string &st, bool attack, const Vec &pos, float now, float dt, bool on)
{
    if (p.jumpOpen && now - p.jumpAt >= 0.8f)
    {
        float dz = pos.z - p.jumpZ;
        if (dz > 20.f) p.m.jUp++;
        else if (dz < -20.f) p.m.jDown++;
        else p.m.jFlat++;
        if (p.jumpSp < 60.f) p.m.jSlow++;
        p.jumpOpen = false;
    }
    bool moving = st == "11MoveToState" || st == "9HuntState" || st == "11FollowState";
    if (moving && !attack && p.speed < 30.f)
    {
        if (p.slowSince < 0) p.slowSince = now;
        if (!p.stuckOn && now - p.slowSince > 1.5f)
        {
            p.stuckOn = true;
            p.m.stuck++;
        }
        if (p.stuckOn) p.m.stuckT += dt;
    }
    else
    {
        p.slowSince = -1;
        p.stuckOn = false;
    }
    if (st == "11MoveToState" && p.lastSt != st)
    {
        int route = *(int *)(p.ent + kOffMoveTo + 16);
        if (now - p.issuedAt < 0.1f || now - p.safeAt < 0.1f)
            p.m.mvOurs++;
        else
        {
            if (route >= 0 && route < 4) p.m.mvStock[route]++;
            int task = BotTask(p.ent);
            if (on && SafeRoutes(p) && route != 2 && !attack && !Urgent(p, task) && !ObjectiveTask(task, true))
            {
                Vec goal;
                memcpy(&goal, (void *)(p.ent + kOffMoveTo + 4), 12);
                BotMoveTo(p.ent, goal, 2);
                p.safeAt = now;
                p.m.mvSafe++;
            }
        }
    }
    if (st == "9HideState" && p.lastSt != st && now - p.issuedAt >= 0.1f && now - p.safeAt >= 0.1f)
    {
        // the bot's own hiding spot (ours are the step-9 cover holds): logged (classified offline by the nav flags too).
        // B5 cover: an EXPOSED spot -> the nearest IN_COVER, not exposed spot within 350 units on the same level
        Vec spot;
        memcpy(&spot, (void *)(p.ent + kOffHide + 12), 12);
        int fl = -1;
        float bd = 40.f * 40.f;
        for (auto &s : g_mi.spots)
        {
            float dd = (s.first.x - spot.x) * (s.first.x - spot.x) + (s.first.y - spot.y) * (s.first.y - spot.y);
            if (dd < bd && fabsf(s.first.z - spot.z) < 80.f) { bd = dd; fl = s.second; }
        }
        int moved = 0;
        if (fl >= 0 && (fl & 8)) p.m.hideExp++;
        int task = BotTask(p.ent);
        if (on && g_t.cover && SafeOn(p) && fl >= 0 && (fl & 8) && !attack && !Urgent(p, task) && !ObjectiveTask(task, true))
        {
            const Vec *best = nullptr;
            float cd = 350.f * 350.f;
            for (auto &s : g_mi.spots)
            {
                if (!(s.second & 1) || (s.second & 8) || fabsf(s.first.z - spot.z) >= 80.f) continue;
                float dd = (s.first.x - spot.x) * (s.first.x - spot.x) + (s.first.y - spot.y) * (s.first.y - spot.y);
                if (dd < cd && dd > 40.f * 40.f) { cd = dd; best = &s.first; }
            }
            if (best)
            {
                memcpy((void *)(p.ent + kOffHide + 12), best, 12);
                SetBotState(p.ent, p.ent + kOffHide);   // re-enter: the bot walks to the covered spot instead
                p.safeAt = now;
                p.m.hideCov++;
                moved = 1;
            }
        }
        BLog("HIDE map=%s r=%d %s team=%s fw=%.2f safe=%d t=%.1f x=%.0f y=%.0f z=%.0f fl=%d moved=%d", g_mapName.c_str(), g_roundNo,
             p.name.c_str(), p.team == 2 ? "T" : "CT", p.fw, (int)SafeOn(p), now - g_liveAt, spot.x, spot.y, spot.z, fl, moved);
    }
}
static bool CtQFast(const Player &p);   // JOB 5c Q
static bool TCallFast(const Player &p);  // JOB 5c T
static int SafeRoute(Player &p, int route)
{
    if (CtQFast(p)) return route;   // JOB 5c Q: a CT on its way to its setup spot takes the fastest way (the safest went round the T side)
    if (TCallFast(p)) return 1;     // JOB 5c T: a T on the called plan walks the called way (the safest re-planned it through mid)
    return SafeRoutes(p) && route != 2 && !Urgent(p, BotTask(p.ent)) ? 2 : route;
}
static float g_fwLast;
static bool J5Pause(const Player &p, float now);   // STEP 12: stops to listen / hold for a moment (job-5 code)
static float J5StopUntil(const Player &p, float now);   // STEP 12G: a dodging bot stops only a moment per shot
static bool J5Dash(const Player &p);                    // STEP 12G: running for cover
static bool J5CounterStrafe(const Player &p, float now);  // STEP 12E
static bool J5ClearWalk(const Player &p, float now);      // STEP 12F5
static void FootFrame()
{
    float now = CurTime();
    float dt = now - g_fwLast;
    g_fwLast = now;
    if (dt < 0 || dt > 1.f)
        dt = 0;
    RefreshTest();
    bool on = g_np.ok && g_layout == 1 && g_steer && g_modeOk;
    for (auto &kv : g_pl)
    {
        Player &p = kv.second;
        if (!p.bot || !p.ent || !p.e)
            continue;
        void *pi = g_pi ? g_pi(p.e) : nullptr;
        if (!pi)
            continue;
        p.uid = VF<int (*)(void *)>(pi, 1)(pi);
        g_byUid[p.uid] = p.name;
        bool alive = (p.team == 2 || p.team == 3) && !PI_Dead(pi) && !PI_Observer(pi);
        Vec pos = PI_Origin(pi);
        if (now - p.lastT >= 0.05f || now < p.lastT)
        {
            float d = sqrtf((pos.x - p.lastPos.x) * (pos.x - p.lastPos.x) + (pos.y - p.lastPos.y) * (pos.y - p.lastPos.y));
            float sp = now > p.lastT ? d / (now - p.lastT) : 0.f;
            p.speed = sp < 600.f ? sp : 0.f;   // a respawn jump is not speed
            p.lastPos = pos;
            p.lastT = now;
        }
        if (g_phase == PH_LIVE && alive && dt > 0)
        {
            if (p.speed > 40.f)
            {
                p.m.moveT += dt;
                if (p.speed <= 145.f) p.m.walkT += dt;
                if (p.walking) p.m.quietT += dt;
            }
        }
        if (!g_np.ok || g_layout != 1)
            continue;
        std::string st = alive ? StateName(p.ent) : "";
        bool attack = alive && BotAttacking(p.ent);
        if (alive && g_phase == PH_LIVE)
            FrameB(p, st, attack, pos, now, dt, on);   // STEP 10b
        p.lastSt = st;
        if (attack && !p.inAttack)
        {
            p.inAttack = true;
            p.attackAt = now;
            p.reactOpen = true;
            p.m.fights++;
            p.stopper = on && g_t.stop && Rnd() < p.fw;
            if (p.stopper)
            {
                p.stopUntil = g_t.stopFirst > 0 ? now + g_t.stopFirst : 0.f;
                p.m.stops++;
            }
        }
        else if (!attack)
        {
            p.inAttack = false;
            p.stopper = false;
        }
        float target = 1.f;
        p.walking = false;
        if (on && alive && g_phase == PH_LIVE)
        {
            if (attack && MoveCycling(p))
                target = p.mv.cyc.phase == 1 ? 0.05f : 1.f;   // JOB 5b P: stop to shoot, move between bursts
            else if (attack && p.stopper && now < J5StopUntil(p, now))
                target = 0.05f;
            else if (J5CounterStrafe(p, now))
                target = 0.05f;   // STEP 12E: it saw an enemy while moving - stops before it shoots
            else if (!attack && UtilStill(p))
                target = 0.05f;   // JOB 5b O: it stands to throw (the throw adds its own speed)
            else if (!attack && J5Pause(p, now))
                target = 0.05f;   // STEP 12B: it stopped to listen (hearing)
            else if (!attack && J5ClearWalk(p, now))
            {
                target = g_t.mult;   // STEP 12F5: a slow site take - it walks in, checking its corners
                p.walking = true;
            }
            else if (!attack && !J5Dash(p) && WalkHere(p, st))   // STEP 12G: no walking while it runs for cover
            {
                target = g_t.mult;
                p.walking = true;
            }
        }
        if (!alive)
            target = 1.f;
        ApplyMove(p, target, now);
    }
    // measurement: the game's own recovery of the speed scale after a hit (fw_trace 1)
    for (auto it = g_vmTrace.begin(); it != g_vmTrace.end();)
    {
        static const float at[] = {0.f, 0.1f, 0.25f, 0.5f, 1.f, 1.5f, 2.f, 3.f};
        auto p = g_pl.find(it->name);
        if (p == g_pl.end() || it->k >= 8 || now < it->at - 1.f) { it = g_vmTrace.erase(it); continue; }
        if (now - it->at >= at[it->k])
        {
            BLog("VMTRACE %s +%.2f velmod %.3f lag %.3f", it->name.c_str(), now - it->at, *(float *)(p->second.ent + g_np.velmod),
                 *(float *)(p->second.ent + g_np.lag));
            it->k++;
        }
        ++it;
    }
}
static void FootRestoreAll()
{
    for (auto &kv : g_pl)
        if (kv.second.bot && kv.second.ent && kv.second.modOn && g_np.ok)
            ApplyMove(kv.second, 1.f, CurTime());
}
static void ShootRoundStart();   // STEP 11
static void ShootRoundEnd();
static void J5RoundStart();      // STEP 12 (job 5)
static void J5RoundEnd();
static void FwRoundStart()
{
    ShootRoundStart();
    J5RoundStart();
    for (auto &kv : g_pl)
    {
        Player &p = kv.second;
        p.m = Player::M();
        p.fsOpen = p.reactOpen = false;
        p.lastFire = -100;
        if (!p.bot) continue;
        p.fw = DialOf(p, D_FOOT, true);
        p.aim = DialOf(p, D_AIM, false);
        p.elo = g_eloOf ? (float)g_eloOf(p.name.c_str()) : 0.f;
        p.walker = Rnd() < p.fw;
        p.safe = Rnd() < p.fw;    // STEP 10b
        p.jumpOpen = p.stuckOn = false;
        p.slowSince = -1;
    }
}
static void FwRoundLog()
{
    ShootRoundEnd();
    J5RoundEnd();
    for (auto &kv : g_pl)
    {
        Player &p = kv.second;
        if (!p.bot || (p.team != 2 && p.team != 3)) continue;
        const Player::M &m = p.m;
        BLog("BOTR map=%s r=%d %s team=%s elo=%.0f aim=%.2f fw=%.2f walker=%d steps=%d near=%d moveT=%.1f walkT=%.1f quietT=%.1f "
             "jumps=%d jumpsF=%d shots=%d shotsAtk=%d hits=%d fs=%d fsHit=%d fsMov=%d fsMovHit=%d kills=%d deaths=%d fights=%d react=%.2f/%d stops=%d"
             " safe=%d jUp=%d jFlat=%d jDown=%d jSlow=%d stuck=%d stuckT=%.1f mv0=%d mv1=%d mv2=%d mv3=%d mvOurs=%d mvSafe=%d hideExp=%d hideCov=%d",
             g_mapName.c_str(), g_roundNo, p.name.c_str(), p.team == 2 ? "T" : "CT", p.elo, p.aim, p.fw, p.walker, m.steps, m.near, m.moveT,
             m.walkT, m.quietT, m.jumps, m.jumpsF, m.shots, m.shotsAtk, m.hits, m.fs, m.fsHit, m.fsMov, m.fsMovHit, m.kills, m.deaths, m.fights,
             m.react, m.reactN, m.stops, (int)SafeOn(p), m.jUp, m.jFlat, m.jDown, m.jSlow, m.stuck, m.stuckT, m.mvStock[0],
             m.mvStock[1], m.mvStock[2], m.mvStock[3], m.mvOurs, m.mvSafe, m.hideExp, m.hideCov);
    }
}
// ==== end STEP 10 ===================================================================================================

// ==== STEP 11 (2026-09-26): shooting - time to first shot, spray, first shot + lucky hits, close-range panic ==========
// His playtests 09-25/26: bots see you and "dance" ~2 s before shooting; low ranks strafe and one-tap; they miss the first
// shot nearly always; a Silver I bot can be run up to and circled. Measured causes (step11 logs): the step-10 profile gave
// the bottom bot ReactionTime 0.90 + AttackDelay 1.00 (= the 2 s), Skill 0.60 at bottom footwork (the stock bot taps
// single shots beyond 400 units and dodges sideways when Skill >= 0.5 - Valve's own rule), AimFocusInitial 25 degrees
// (misses everything); and every bot turned at Valve's Default look speed: the per-bot LookAngle* keys are never copied
// by this build's template inheritance (read back from the live BotProfile objects).
// Lever: the bot's own BotProfile object (CBot::m_profile at +0x3b4c, measured on 1.38.8.1; the stock bot reads every
// field live on each use). Fields by offset (read back against botprofile.db, verified per bot per level: the name and
// four fields we never write must match the .db, else that bot is never touched): +4 Aggression, +8 Skill, +0xc Teamwork,
// +0x10 AimFocusInitial, +0x14 AimFocusDecay, +0x18 AimFocusOffsetScale, +0x1c AimfocusInterval, +0x54 ReactionTime,
// +0x58 AttackDelay, +0x68..+0x7c LookAngle MaxAccel/Stiffness/Damping Normal then Attacking.
// So botprofile.db stays as it is (the kill switch puts every field back to its .db value, live, no restart) and the
// plugin writes, per bot, from its dials (make_botprofile.py seeds react + spray per bot, kid cap as for aim):
//   react  -> r = react dial ^ 0.5 (stretches the bottom: an elo-700 bot is clearly quicker than an elo-300 one - the
//             kid floor measured 80.4% with a straight dial, 84.4% with this);
//             ReactionTime 0.40 .. 0.06 s (time to notice; 0.40 not 0.35 at the bottom: kid floor 79.9% -> 81.7%); AttackDelay 0.05 .. 0.12 s (the trigger waits for the view
//             to settle: the stock bot fires the first frame its view is within ~2 body widths of its aim point, so a
//             low rank fires early and whiffs, a high rank lands the first shot). No hesitation at any rank.
//   spray  -> Skill 0.10 .. 0.45: always below Valve's 0.5 line, so the stock bot holds the trigger at every range and
//             never side-steps between taps (the strafe-one-tap he saw came from Skill 0.60 at the bottom); the rank
//             shows in recoil control instead: the aim punch (m_aimPunchAngle + Vel, the recoil the game adds to every
//             bullet) decays faster, 0 (ugly full sprays) .. shoot_rck 40/s x spray^1.5 (controlled). Skill 0.8 at the
//             top (taps beyond 400 units) was measured and dropped: first shots hit 22% vs 40%.
//   aim    -> AimFocusInitial 10 .. 0.5 deg, AimFocusOffsetScale 0.45 .. 0.05 (step 10: 25 / 0.7 at the bottom), and the
//             look speed per bot (Valve Default at the bottom, faster up to 15000 deg/s2 attacking at the top)
//   lucky  -> every engagement, at every rank, the bot has a shoot_lucky_p chance (0.15) that its first shot is aimed
//             true (focus error ~0 until that shot); it lands on the chest or head wherever the bot aims
//   panic  -> an enemy in line of sight within shoot_r (400 units = Valve's own close-range spray line in the stock fire
//             code, ~1.6 s of running) makes ANY bot, the lowest too: react 0.20 .. 0.08 s, no attack delay, hold the
//             trigger (Skill 0), fast look (9000 deg/s2, critically damped); and if he is outside its view it turns
//             towards him at 600 .. 1200 deg/s (the player's view angle copy +0x0c80, see the panic turn below) - he
//             cannot circle it. Measured without the turn (build s15): a runner circling a Silver I at 250 units
//             survived 10 of 14 times - the turn is what closes it.
// Metrics: sight = a world-only trace (engine EngineTraceServer004) from the bot's eye to the enemy's head or chest and
// the enemy inside 50 deg of the bot's view; one "ENG" line per engagement (sight -> attack, sight -> first shot, shots,
// longest trigger hold (m_iShotsFired), hits, first-shot hit + hitgroup, panic, lucky). Test harness circle_test <R>:
// 1v1, the T bot never shoots and runs up to then circles the CT bot at radius R; one "CIRC" line per round.
// Kill switches (addons/family_test.txt, live): brain_shoot 0 (all of it, .db values back), shoot_panic 0, shoot_lucky 0,
// shoot_rc 0, shoot_turn 0; A/B test: shoot_team 2|3 (only that team).
static const char *kDbPath = "csgo/botprofile.db";
static const int kOffProfile = 0x3b4c;
static const int kOffViewYaw = 0x0c80;   // the player's view angle copy the bot turns from (pitch at -4), see the panic turn
enum { PF_AGGR, PF_SKILL, PF_TEAM, PF_FINIT, PF_FDECAY, PF_FOFF, PF_FINT, PF_REACT, PF_ADELAY, PF_LNA, PF_LNS, PF_LND, PF_LAA, PF_LAS, PF_LAD, PF_N };
static const int kPfOff[PF_N] = {0x04, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c, 0x54, 0x58, 0x68, 0x6c, 0x70, 0x74, 0x78, 0x7c};
static const char *kPfKey[PF_N] = {"Aggression", "Skill", "Teamwork", "AimFocusInitial", "AimFocusDecay", "AimFocusOffsetScale", "AimfocusInterval",
                                   "ReactionTime", "AttackDelay", "LookAngleMaxAccelNormal", "LookAngleStiffnessNormal", "LookAngleDampingNormal",
                                   "LookAngleMaxAccelAttacking", "LookAngleStiffnessAttacking", "LookAngleDampingAttacking"};
static const int kPfWrite[] = {PF_SKILL, PF_FINIT, PF_FOFF, PF_REACT, PF_ADELAY, PF_LNA, PF_LNS, PF_LND, PF_LAA, PF_LAS, PF_LAD};
struct DbProf
{
    float v[PF_N];
};
static std::map<std::string, DbProf> g_db;   // bot name -> the values the game loaded (look fields = Default: not inherited)
static struct ShT
{
    int shoot = 1, panic = 1, lucky = 1, rc = 1, team = 0, circle = 0, turn = 1;
    float radius = 400, luckyP = 0.15f, rcK = 40, rtLo = 0.40f, rtHi = 0.06f, adLo = 0.05f, adHi = 0.12f, fiLo = 10.f, foLo = 0.45f, luckyAd = 0.12f;
    float laaHi = 15000.f, skHi = 0.45f, rGam = 0.5f;
} g_s;
static void RefreshShoot()
{
    static time_t at;
    time_t t = time(nullptr);
    if (!g_tv || t == at) return;   // the test file is read at most once a second
    at = t;
    const ShT d;   // the shipped values (calibrated in step11 logs\summary-11a.txt)
    g_s.shoot = (int)g_tv("brain_shoot", d.shoot);
    g_s.panic = (int)g_tv("shoot_panic", d.panic);
    g_s.lucky = (int)g_tv("shoot_lucky", d.lucky);
    g_s.rc = (int)g_tv("shoot_rc", d.rc);
    g_s.turn = (int)g_tv("shoot_turn", d.turn);
    g_s.team = (int)g_tv("shoot_team", d.team);
    g_s.circle = (int)g_tv("circle_test", d.circle);
    g_s.radius = (float)g_tv("shoot_r", d.radius);
    g_s.luckyP = (float)g_tv("shoot_lucky_p", d.luckyP);
    g_s.rcK = (float)g_tv("shoot_rck", d.rcK);
    g_s.rtLo = (float)g_tv("shoot_rt_lo", d.rtLo);   // calibration hooks (test only)
    g_s.rtHi = (float)g_tv("shoot_rt_hi", d.rtHi);
    g_s.adLo = (float)g_tv("shoot_ad_lo", d.adLo);
    g_s.adHi = (float)g_tv("shoot_ad_hi", d.adHi);
    g_s.luckyAd = (float)g_tv("shoot_lucky_ad", d.luckyAd);
    g_s.laaHi = (float)g_tv("shoot_laa_hi", d.laaHi);
    g_s.skHi = (float)g_tv("shoot_sk_hi", d.skHi);
    g_s.fiLo = (float)g_tv("shoot_fi_lo", d.fiLo);
    g_s.foLo = (float)g_tv("shoot_fo_lo", d.foLo);
    g_s.rGam = (float)g_tv("shoot_rgam", d.rGam);
    char k[300];
    snprintf(k, sizeof(k), "shoot %d panic %d lucky %d rc %d turn %d team %d circle %d r %.0f luckyP %.2f luckyAd %.2f rcK %.1f rt %.2f-%.2f ad %.2f-%.2f fi %.1f fo %.2f laa %.0f sk %.2f rgam %.2f",
             g_s.shoot, g_s.panic, g_s.lucky, g_s.rc, g_s.turn, g_s.team, g_s.circle, g_s.radius, g_s.luckyP, g_s.luckyAd, g_s.rcK, g_s.rtLo, g_s.rtHi,
             g_s.adLo, g_s.adHi, g_s.fiLo, g_s.foLo, g_s.laaHi, g_s.skHi, g_s.rGam);
    static std::string last;
    if (last != k)
        BLog("shoot hooks: %s", k);
    last = k;
}
static void LoadDb()
{
    g_db.clear();
    FILE *f = fopen(kDbPath, "r");
    if (!f)
    {
        BLog("shoot: %s missing - shooting stays stock", kDbPath);
        return;
    }
    DbProf def{}, cur{};
    const float stock[PF_N] = {0.5f, 0.5f, 0.75f, 20, 0.7f, 0.3f, 0.8f, 0.3f, 0, 2000, 100, 25, 3000, 150, 30};
    memcpy(def.v, stock, sizeof(stock));
    std::string block;   // "Default", "E_<name>" or "" (other templates / bot lines)
    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        char a[128], b[128];
        float val;
        if (sscanf(line, " %127s", a) != 1 || a[0] == '/')
            continue;
        if (!strcmp(a, "Default"))
        {
            block = "Default";
            cur = def;
            continue;
        }
        if (!strcmp(a, "Template") && sscanf(line, " %127s %127s", a, b) == 2)
        {
            block = strncmp(b, "E_", 2) ? "" : b;
            cur = def;
            continue;
        }
        if (!strcmp(a, "End"))
        {
            if (block == "Default")
                def = cur;
            else if (!block.empty())
            {
                DbProf d = cur;
                for (int i = PF_LNA; i <= PF_LAD; i++)
                    d.v[i] = def.v[i];   // measured: the per-bot LookAngle keys are not inherited - the bot keeps Default's
                g_db[block.substr(2)] = d;
            }
            block.clear();
            continue;
        }
        if (block.empty() || sscanf(line, " %127s = %f", a, &val) != 2)
            continue;
        for (int i = 0; i < PF_N; i++)
            if (!strcasecmp(a, kPfKey[i]))
                cur.v[i] = (i == PF_AGGR || i == PF_SKILL || i == PF_TEAM) ? val / 100.f : val;
    }
    fclose(f);
    BLog("shoot: %d bot profiles read from %s", (int)g_db.size(), kDbPath);
}
static float PfGet(uintptr_t prof, int i) { return *(float *)(prof + kPfOff[i]); }
static void PfSet(uintptr_t prof, int i, float v)
{
    float *f = (float *)(prof + kPfOff[i]);
    if (fabsf(*f - v) > 1e-5f) *f = v;
}
static int g_profBad;
static void ProfCheck(Player &p)
{
    auto it = g_db.find(p.name);
    uintptr_t prof = 0, nm = 0;
    bool ok = it != g_db.end() && SafeWord(p.ent + kOffProfile, prof) && prof && SafeWord(prof, nm) && SafeStr(nm, 64) == p.name;
    if (ok)
    {
        unsigned char buf[0x80];
        ok = SafeRead((void *)prof, buf, sizeof(buf));
        for (int i : {PF_AGGR, PF_TEAM, PF_FDECAY, PF_FINT})
            ok = ok && fabsf(PfGet(prof, i) - it->second.v[i]) < 0.006f;
        for (int i : {PF_REACT, PF_ADELAY, PF_LAA, PF_LAS})
            ok = ok && std::isfinite(PfGet(prof, i)) && PfGet(prof, i) >= 0.f && PfGet(prof, i) < 1e6f;
    }
    p.sh.prof = ok ? prof : 0;
    p.sh.state = ok ? 1 : -1;
    if (!ok && g_profBad++ < 6)
        BLog("shoot: profile of %s does not match botprofile.db (prof %p) - this bot keeps stock shooting", p.name.c_str(), (void *)prof);
}
static void ProfRestore(Player &p)
{
    auto it = g_db.find(p.name);
    if (p.sh.state != 1 || it == g_db.end()) return;
    for (int i : kPfWrite)
        PfSet(p.sh.prof, i, it->second.v[i]);
}
static float LogLerp(float a, float b, float t) { return expf(logf(a) + (logf(b) - logf(a)) * t); }
static bool ShootOn(const Player &p) { return g_s.shoot && (!g_s.team || g_s.team == p.team); }
static struct Circ
{
    std::string circ, tgt;
    float tIn = -1, tShot = -1, tTurn = -1, tDeath = -1, inT = 0, losT = 0, fovT = 0, angSum = 0, radSum = 0, lastAng = 0, nextMove = 0, tgtElo = 0;
    float tVdeath = -100;   // when the target's damage on the circler reached 100 (a real player dead), game time
    float nextHold = 0, slowSince = -1, dir = 1;
    int shotsIn = 0, hits = 0, heads = 0, shotsFar = 0, logged = 0, circling = 0, held = 0, panicAt = 0, hitsFar = 0, dmg = 0, released = 0;
    bool haveAng = false;
} g_circ;
static bool TestMute(const Player &p);   // STEP 12B: the circler or the sneak test's runner (job-5 code)
static void J5Seen(Player &p, Player &e, const Vec &at, float now);   // STEP 12
static void J5Want(Player &p, float *w);  // STEP 12G
static void J5Gold(Player &p, float *w);  // STEP 12K
// the values this bot should have now (writable fields only)
static void Want(Player &p, bool panic, float *w)
{
    float r = powf(std::max(0.f, p.sh.react), g_s.rGam), c = p.sh.spray, a = powf(std::max(0.f, p.sh.aimd), 0.75f);
    w[PF_REACT] = Lerp(g_s.rtLo, g_s.rtHi, r);
    w[PF_ADELAY] = Lerp(g_s.adLo, g_s.adHi, r);
    w[PF_SKILL] = Lerp(0.10f, g_s.skHi, c);
    w[PF_FINIT] = LogLerp(g_s.fiLo, 0.5f, a);
    w[PF_FOFF] = LogLerp(g_s.foLo, 0.05f, a);
    w[PF_LNA] = LogLerp(2000, 4000, a);
    w[PF_LNS] = Lerp(100, 150, a);
    w[PF_LND] = 2.f * sqrtf(w[PF_LNS]) * Lerp(1.25f, 1.0f, a);
    w[PF_LAA] = LogLerp(3000, g_s.laaHi, a);
    w[PF_LAS] = Lerp(150, 300, a);
    w[PF_LAD] = 2.f * sqrtf(w[PF_LAS]) * Lerp(1.22f, 1.0f, a);
    J5Gold(p, w);   // STEP 12K: the middle ranks (Gold Nova .. Master Guardian) react and aim a little sharper
    if (g_s.lucky && p.sh.lucky)
    {
        // the first shot of this engagement goes where the bot aims: no focus error, a snap turn, and the trigger waits
        // until the view has settled (the stock bot fires the first frame its view is within ~2 body widths of the aim
        // point, i.e. on the way in - that is why first shots miss at every rank)
        w[PF_FINIT] = 0.3f;
        w[PF_FOFF] = 0.02f;
        w[PF_LAA] = std::max(w[PF_LAA], 20000.f);
        w[PF_LAS] = std::max(w[PF_LAS], 400.f);
        w[PF_LAD] = 2.f * sqrtf(w[PF_LAS]);
        w[PF_ADELAY] = std::max(w[PF_ADELAY], g_s.luckyAd);
    }
    if (panic && g_s.panic)
    {
        w[PF_REACT] = std::min(w[PF_REACT], Lerp(0.20f, 0.08f, r));
        w[PF_ADELAY] = 0.f;
        w[PF_SKILL] = 0.f;                          // hold the trigger, no side-step
        // at arm's length even a panicking beginner points the gun at you: the focus error is what made a Silver I miss a
        // runner 300 units away for seconds (circle test: 2 hits in 26 shots with 6 deg); the spray's recoil stays ugly
        // (a flat 1.5 deg measured the kid floor at 76.7%, 3 deg at the bottom let a circling runner live 9 of 9 times:
        // 1.5 deg at the bottom, sharper up the ranks so rank still shows at arm's length)
        float sa = sqrtf(a);
        w[PF_FINIT] = std::min(w[PF_FINIT], Lerp(1.5f, 0.5f, sa));
        w[PF_FOFF] = std::min(w[PF_FOFF], Lerp(0.10f, 0.03f, sa));
        w[PF_LAA] = std::max(w[PF_LAA], 9000.f);
        w[PF_LAS] = std::max(w[PF_LAS], 250.f);
        w[PF_LAD] = 2.f * sqrtf(w[PF_LAS]);
    }
    J5Want(p, w);   // STEP 12G: under fire - the side-step band while dodging, no re-engage while dashing to cover
    if (!panic) J5bWant(p, w);   // JOB 5b P: a mover's fight side-steps between its bursts
    if (TestMute(p))
    {
        w[PF_REACT] = 60.f;   // the test circler (STEP 12B: or sneak runner) never notices anyone, so never shoots
        w[PF_ADELAY] = 60.f;
    }
}

// ---- world-only line of sight (engine trace) ----
static void *g_trace;
static int g_traceOk;   // 0 unknown, 1 checked, -1 off
struct alignas(16) VecA
{
    float x, y, z, w;
};
struct alignas(16) RayT
{
    VecA start, delta, startOff, ext;
    const void *axis;
    bool isRay, isSwept;
};
struct TrFilter
{
    void **vt;
};
static bool TrShould(TrFilter *, void *, int) { return false; }
static int TrType(TrFilter *) { return 1; }   // TRACE_WORLD_ONLY
static void *g_trVt[2] = {(void *)TrShould, (void *)TrType};
static TrFilter g_trF = {g_trVt};
static float TraceFracM(const Vec &a, const Vec &b, unsigned mask);
static float TraceFrac(const Vec &a, const Vec &b) { return TraceFracM(a, b, 0x6081 /* MASK_VISIBLE */); }
// JOB 5b O: the mask is a parameter (a grenade's flight: 0x400b = solid | window | grate | moveable)
static float TraceFracM(const Vec &a, const Vec &b, unsigned mask)
{
    RayT r;
    memset(&r, 0, sizeof(r));
    r.start = {a.x, a.y, a.z, 0};
    r.delta = {b.x - a.x, b.y - a.y, b.z - a.z, 0};
    r.isRay = true;
    r.isSwept = true;
    alignas(16) unsigned char tr[256];
    memset(tr, 0, sizeof(tr));
    VF<void (*)(void *, const RayT &, unsigned, void *, void *)>(g_trace, 5)(g_trace, r, mask, &g_trF, tr);
    float fr;
    memcpy(&fr, tr + 0x2c, 4);
    return fr;
}
static bool Los(const Vec &a, const Vec &b)
{
    if (g_traceOk < 0 || !g_trace) return false;
    float fr = TraceFrac(a, b);
    if (!(fr >= 0.f && fr <= 1.0001f))
    {
        g_traceOk = -1;
        BLog("shoot: trace returned fraction %f - sight metrics off", fr);
        return false;
    }
    return fr > 0.97f;
}
static float Wrap180(float a)
{
    // fmod, never a subtract loop (a garbage 1e30 - 360 == 1e30 hung a test server while this was built)
    if (!std::isfinite(a) || fabsf(a) > 1e6f) return 1e6f;
    a = fmodf(a + 180.f, 360.f);
    if (a < 0) a += 360.f;
    return a - 180.f;
}
using fl::YawTo;   // STEP 12: shared with family_logic.h
static Vec EyeOf(uintptr_t ent, const Vec &o)
{
    bool duck = g_np.flags > 0 && (*(int *)(ent + g_np.flags) & 2);
    return {o.x, o.y, o.z + (duck ? 46.f : 64.f)};
}

// ---- engagements ----
static void EngOpen(Player &p, float now)
{
    Player::Sh &s = p.sh;
    s.eng = true;
    s.engStart = s.engEnd = now;
    s.engSight = (s.sightAt > 0 && now - s.lastSeen < 0.5f && now - s.sightAt < 4.f) ? s.sightAt : -1.f;
    s.firstShot = -1;
    s.engShots = s.engBurst = s.engHits = s.engHeads = s.fsHit = s.fsGroup = s.engFull = 0;
    s.fsDone = s.fsHitOpen = false;
    s.engPanic = s.panic;
    s.engLucky = s.lucky && g_s.lucky && ShootOn(p);
    s.fsWeapon.clear();
}
static void EngClose(Player &p)
{
    Player::Sh &s = p.sh;
    if (!s.eng) return;
    s.eng = false;
    BLog("ENG map=%s r=%d %s team=%s elo=%.0f react=%.2f spray=%.2f aim=%.2f on=%d s2a=%.2f s2f=%.2f a2f=%.2f shots=%d burst=%d full=%d hits=%d heads=%d fsHit=%d fsGroup=%d fsDist=%.0f panic=%d lucky=%d turn=%d dur=%.2f w=%s",
         g_mapName.c_str(), g_roundNo, p.name.c_str(), p.team == 2 ? "T" : "CT", p.elo, s.react, s.spray, s.aimd, (int)ShootOn(p),
         s.engSight >= 0 ? s.engStart - s.engSight : -1.f, s.engSight >= 0 && s.firstShot >= 0 ? s.firstShot - s.engSight : -1.f,
         s.firstShot >= 0 ? s.firstShot - s.engStart : -1.f, s.engShots, s.engBurst, s.engFull, s.engHits, s.engHeads, s.fsHit, s.fsGroup, s.fsDist,
         (int)s.engPanic, (int)s.engLucky, s.turns > 0 ? 1 : 0, s.engEnd - s.engStart, s.fsWeapon.empty() ? "-" : s.fsWeapon.c_str());
    s.turns = 0;
    if (g_s.lucky) s.lucky = Rnd() < g_s.luckyP;   // the next engagement
}
static void ShootOnFire(Player &p, const char *w, float now)
{
    Player::Sh &s = p.sh;
    if (!s.eng) EngOpen(p, now);
    s.engEnd = now;
    s.engShots++;
    // longest trigger hold: m_iShotsFired (shots in this hold, autos) or back-to-back shots <= 0.13 s apart (a tap-firing
    // bot fires every 0.15-0.7 s, Valve's fire code)
    int sf = g_np.shotsFired > 0 ? *(int *)(p.ent + g_np.shotsFired) : 0;
    s.run = now - s.lastShot <= 0.13f ? s.run + 1 : 1;
    s.lastShot = now;
    if (sf > 0 && sf < 200) s.engBurst = std::max(s.engBurst, sf);
    s.engBurst = std::max(s.engBurst, s.run);
    if (s.engBurst >= 10) s.engFull = 1;
    if (!s.fsDone)
    {
        s.fsDone = true;
        s.firstShot = now;
        s.fsWeapon = w;
        s.fsHitOpen = true;
        s.fsDist = 0;
        float bd = 1e9f;
        for (auto &kv : g_pl)
            if (kv.second.alive && (kv.second.team == 2 || kv.second.team == 3) && kv.second.team != p.team)
                bd = std::min(bd, Dist(kv.second.lastPos.x || kv.second.lastPos.y ? kv.second.lastPos : kv.second.pos, p.lastPos));
        s.fsDist = bd < 1e8f ? bd : -1;
        s.lucky = false;   // used (the focus goes back to the bot's own next frame)
    }
    if (g_s.circle && p.name == g_circ.tgt)
    {
        if (g_circ.tIn >= 0)
        {
            if (now - g_circ.tIn < 12.f) g_circ.shotsIn++;
            if (g_circ.tShot < 0) g_circ.tShot = now;
        }
        else g_circ.shotsFar++;
    }
}
static void ShootOnHurt(Player *a, Player *v, int group, int dmg, float now)
{
    if (!a || !v || a == v || a->team == v->team) return;
    if (a->bot)
    {
        Player::Sh &s = a->sh;
        if (s.eng)
        {
            s.engHits++;
            if (group == 1) s.engHeads++;
            if (s.fsHitOpen && now - s.firstShot <= 0.15f)
            {
                s.fsHit = 1;
                s.fsGroup = group;
                s.fsHitOpen = false;
            }
        }
    }
    if (g_s.circle && a->name == g_circ.tgt && v->name == g_circ.circ)
    {
        if (g_circ.tIn >= 0) g_circ.hits++;
        else g_circ.hitsFar++;
        if (group == 1) g_circ.heads++;
        g_circ.dmg += dmg;
        if (g_circ.dmg >= 100 && g_circ.tVdeath < -99.f) g_circ.tVdeath = now;   // a 100-hp player would be dead now
    }
}
static void CircLog(int survived)
{
    if (g_circ.logged || g_circ.circ.empty()) return;
    g_circ.logged = 1;
    float t0 = g_circ.tIn;
    auto rel = [&](float t) { return t0 >= 0 && t >= 0 ? t - t0 : -1.f; };
    // vdeath: seconds after the circler came within shoot_r until a 100-hp player would have died (negative = shot dead on
    // the way in; -100 = never, i.e. he survived the whole circling window)
    BLog("CIRC map=%s r=%d target=%s elo=%.0f R=%d on=%d tIn=%d shot=%.2f turn=%.2f vdeath=%.2f dmg=%d shotsIn=%d shotsFar=%d hits=%d hitsFar=%d heads=%d inT=%.1f angv=%.0f rad=%.0f los=%.2f fov=%.2f panic=%d survived=%d",
         g_mapName.c_str(), g_roundNo, g_circ.tgt.c_str(), g_circ.tgtElo, g_s.circle, (int)g_s.shoot, t0 >= 0, rel(g_circ.tShot), rel(g_circ.tTurn),
         g_circ.tVdeath > -99.f ? (t0 >= 0 ? g_circ.tVdeath - t0 : -50.f) : -100.f, g_circ.dmg, g_circ.shotsIn, g_circ.shotsFar, g_circ.hits,
         g_circ.hitsFar, g_circ.heads, g_circ.inT, g_circ.inT > 0 ? g_circ.angSum / g_circ.inT : 0.f,
         g_circ.inT > 0 ? g_circ.radSum / g_circ.inT : 0.f, g_circ.inT > 0 ? g_circ.losT / g_circ.inT : 0.f, g_circ.inT > 0 ? g_circ.fovT / g_circ.inT : 0.f,
         g_circ.panicAt, survived);
}
static void ShootOnDeath(Player *v, Player *a, float now)
{
    if (v && v->bot) EngClose(*v);
    if (g_s.circle && v && v->name == g_circ.circ && g_circ.tDeath < 0)
    {
        g_circ.tDeath = now;
        CircLog(0);
    }
    (void)a;
}

// ---- circle test (test hook circle_test <radius>): 1v1, the T runs up to the CT bot and circles it, never shooting ----
static void CircleFrame(float now, float dt, std::map<std::string, Vec> &pos)
{
    Player *c = nullptr, *t = nullptr;
    for (auto &kv : g_pl)
        if (kv.second.bot && kv.second.alive)
        {
            if (kv.second.team == 2 && !c) c = &kv.second;
            if (kv.second.team == 3 && !t) t = &kv.second;
        }
    if (!c || !t || g_layout != 1) return;
    if (g_circ.circ.empty())
    {
        g_circ.circ = c->name;
        g_circ.tgt = t->name;
        g_circ.tgtElo = t->elo;
    }
    if (c->name != g_circ.circ || t->name != g_circ.tgt) return;
    Vec cp = pos[c->name], tp = pos[t->name];
    // the arena: bomb site A's centre (open ground on the bomb maps; the spawns have walls where the circle points fail)
    Vec arena = g_mi.ok && !g_mi.sites.empty() ? g_mi.sites[0].c : tp;
    if (!g_circ.held && now - g_liveAt > 0.5f && now >= g_circ.nextHold)
    {
        g_circ.nextHold = now + 0.5f;
        float da = sqrtf((tp.x - arena.x) * (tp.x - arena.x) + (tp.y - arena.y) * (tp.y - arena.y));
        if (da > 60.f && now - g_liveAt < 25.f)
            BotMoveTo(t->ent, arena, 1);
        else
            g_circ.held = BotHideHere(t->ent, tp, 300.f) ? 1 : 0;   // then it stands its ground there
    }
    float d = Dist(cp, tp), R = (float)g_s.circle;
    if (g_circ.tIn < 0 && d < g_s.radius) g_circ.tIn = now;
    // the circler soaks the shots (health 5000) for the whole run-up and 12 s of circling, so every round measures the
    // full circle; the damage a real 100-hp player would have taken is counted (vdeath). Then it is released (1 hp).
    if (g_np.health > 0)
    {
        int *hp = (int *)(c->ent + g_np.health);
        bool window = g_circ.tIn < 0 || now - g_circ.tIn < 12.f;
        if (*hp > 0 && *hp <= 10000)
        {
            if (window) *hp = 5000;
            else if (!g_circ.released)
            {
                *hp = 1;
                g_circ.released = 1;
            }
        }
    }
    if (g_circ.tIn >= 0 && g_circ.tDeath < 0 && now - g_circ.tIn < 12.f)
    {
        float yaw = g_np.eyeY > 0 ? *(float *)(t->ent + g_np.eyeY) : 0.f;
        float off = fabsf(Wrap180(YawTo(tp, cp) - yaw));
        if (g_circ.tTurn < 0 && off < 15.f) g_circ.tTurn = now;
        if (d < R + 150.f)
        {
            float ang = YawTo(tp, cp);
            if (g_circ.haveAng) g_circ.angSum += fabsf(Wrap180(ang - g_circ.lastAng));
            g_circ.lastAng = ang;
            g_circ.haveAng = true;
            g_circ.inT += dt;
            g_circ.radSum += d * dt;
            if (off < 50.f) g_circ.fovT += dt;
            if (now - t->sh.closeAt < 0.1f) g_circ.losT += dt;
        }
        if (t->sh.panic) g_circ.panicAt = 1;
    }
    // the circler never fights back (it would stop and side-step instead of running): its attack flag is cleared; and it
    // keeps full speed when hit (a human the bot misses keeps running; the damage still counts in vdeath)
    *(uint8_t *)(c->ent + kOffStateTime + 4) = 0;
    if (g_np.velmod > 0)
    {
        float *vm = (float *)(c->ent + g_np.velmod);
        if (*vm >= 0.f && *vm < 1.f) *vm = 1.f;
    }
    if (now < g_circ.nextMove) return;
    g_circ.nextMove = now + 0.25f;
    if (!g_circ.circling && d > R + 80.f)
    {
        BotMoveTo(c->ent, tp, 1);
        return;
    }
    g_circ.circling = 1;
    // stuck against something (under 60 u/s for 0.6 s): run the other way round
    if (c->speed < 60.f)
    {
        if (g_circ.slowSince < 0) g_circ.slowSince = now;
        if (now - g_circ.slowSince > 0.6f)
        {
            g_circ.dir = -g_circ.dir;
            g_circ.slowSince = -1;
        }
    }
    else
        g_circ.slowSince = -1;
    float a = atan2f(cp.y - tp.y, cp.x - tp.x) + g_circ.dir * 60.f / 57.29578f;
    Vec goal = {tp.x + R * cosf(a), tp.y + R * sinf(a), tp.z};
    BotMoveTo(c->ent, goal, 1);
    static int dl;
    if (g_s.circle && dl++ % 8 == 0 && dl < 400)
        BLog("CIRCDIAG r=%d t=%.1f d=%.0f speed=%.0f state %s | target %s attack=%d off=%.0f panic=%d close=%.2f seen=%.2f task=%d", g_roundNo,
             now - g_liveAt, d, c->speed, StateName(c->ent).c_str(), StateName(t->ent).c_str(), (int)BotAttacking(t->ent),
             g_np.eyeY > 0 ? Wrap180(YawTo(tp, cp) - *(float *)(t->ent + g_np.eyeY)) : 0.f, (int)t->sh.panic, now - t->sh.closeAt,
             now - t->sh.lastSeen, BotTask(t->ent));
}

// ---- every server frame ----
static float g_shLast;
static int g_npCheck;
static void ShootFrame()
{
    float now = CurTime();
    float dt = now - g_shLast;
    g_shLast = now;
    if (dt < 0 || dt > 1.f) dt = 0;
    RefreshShoot();
    if (g_traceOk == 0 && g_trace) g_traceOk = 1;
    // fresh positions this frame (humans too: they are enemies)
    std::map<std::string, Vec> pos;
    std::map<std::string, bool> alive;
    for (auto &kv : g_pl)
    {
        Player &p = kv.second;
        void *pi = p.e && g_pi ? g_pi(p.e) : nullptr;
        if (!pi) continue;
        bool al = (p.team == 2 || p.team == 3) && !PI_Dead(pi) && !PI_Observer(pi);
        alive[p.name] = al;
        if (al) pos[p.name] = PI_Origin(pi);
    }
    bool live = g_phase == PH_LIVE;
    for (auto &kv : g_pl)
    {
        Player &p = kv.second;
        if (!p.bot || !p.ent || !p.e) continue;
        Player::Sh &s = p.sh;
        if (s.state == 0 && !g_db.empty()) ProfCheck(p);
        bool al = alive[p.name];
        bool on = ShootOn(p) && s.state == 1 && g_modeOk;
        if (al && live && g_npCheck < 20)
        {
            // the eye-angle / punch offsets must hold angles on live bots (else those levers stay off)
            float ep = g_np.eyeP > 0 ? *(float *)(p.ent + g_np.eyeP) : 0.f, ey = g_np.eyeY > 0 ? *(float *)(p.ent + g_np.eyeY) : 0.f;
            float *pu = g_np.punch > 0 ? (float *)(p.ent + g_np.punch) : nullptr;
            bool eyeOk = std::isfinite(ep) && std::isfinite(ey) && fabsf(ep) <= 90.f && fabsf(ey) <= 400.f;
            bool puOk = !pu || (std::isfinite(pu[0]) && std::isfinite(pu[1]) && fabsf(pu[0]) < 90.f && fabsf(pu[1]) < 90.f && pu[2] == 0.f);
            if (!eyeOk && g_np.eyeY > 0) g_np.eyeY = g_np.eyeP = -1;
            if (!puOk && g_np.punch > 0) g_np.punch = g_np.punchVel = -1;
            if (++g_npCheck == 20 || !eyeOk || !puOk)
                BLog("shoot netprop check (%s): eye %.1f %.1f punch %.2f %.2f -> eye %s punch %s", p.name.c_str(), ep, ey, pu ? pu[0] : 0.f, pu ? pu[1] : 0.f,
                     g_np.eyeY > 0 ? "ok" : "OFF", g_np.punch > 0 ? "ok" : "OFF");
        }
        // sight / close-range scan (every 0.05 s of game time)
        if (al && live && now >= s.nextScan && g_np.eyeY > 0)
        {
            s.nextScan = now + 0.05f;
            Vec me = pos[p.name], eye = EyeOf(p.ent, me);
            float yaw = *(float *)(p.ent + g_np.eyeY);
            bool seen = false;
            float bestClose = 1e9f;
            for (auto &q : g_pl)
            {
                Player &e = q.second;
                if (!alive[e.name] || e.team == p.team || (e.team != 2 && e.team != 3)) continue;
                Vec ep = pos[e.name];
                float d = Dist(me, ep);
                if (d > 5000.f) continue;
                bool inFov = fabsf(Wrap180(YawTo(eye, ep) - yaw)) < 50.f;
                if (!inFov && d >= g_s.radius) continue;
                bool los = Los(eye, {ep.x, ep.y, ep.z + 62.f}) || Los(eye, {ep.x, ep.y, ep.z + 42.f});
                if (!los) continue;
                if (inFov) seen = true;
                if (inFov) J5Seen(p, e, ep, now);   // STEP 12: what the team knows (sight)
                if (d < g_s.radius && d < bestClose)
                {
                    bestClose = d;
                    s.closePos = ep;
                    s.closeName = e.name;
                }
            }
            if (seen)
            {
                if (now - s.lastSeen > 1.0f) s.sightAt = now;
                s.lastSeen = now;
            }
            if (bestClose < 1e8f)
            {
                s.closeAt = now;
                s.closeDist = bestClose;
            }
        }
        s.panic = on && g_s.panic && al && live && now - s.closeAt < 0.3f && !TestMute(p);
        bool attack = al && g_layout == 1 && BotAttacking(p.ent);
        if (attack && !s.eng && live) EngOpen(p, now);
        if (s.eng && attack)
        {
            s.engEnd = now;
            if (s.panic) s.engPanic = true;
        }
        if (s.eng && !attack && now - s.engEnd > 0.75f) EngClose(p);
        if (s.fsHitOpen && now - s.firstShot > 0.15f) s.fsHitOpen = false;
        // profile values
        if (s.state == 1)
        {
            if (on || TestMute(p))
            {
                float w[PF_N];
                Want(p, s.panic, w);
                if (!on) ProfRestore(p);
                for (int i : kPfWrite)
                    if (on || i == PF_REACT || i == PF_ADELAY) PfSet(s.prof, i, w[i]);
            }
            else
                ProfRestore(p);
        }
        // recoil control: the aim punch decays faster (only while the bot fights)
        if (on && g_s.rc && al && attack && dt > 0 && g_np.punch > 0 && g_np.punchVel > 0)
        {
            float k = g_s.rcK * powf(s.spray, 1.5f);
            if (k > 0)
            {
                float f = expf(-k * dt);
                float *pu = (float *)(p.ent + g_np.punch), *pv = (float *)(p.ent + g_np.punchVel);
                if (std::isfinite(pu[0]) && std::isfinite(pu[1]) && fabsf(pu[0]) < 90 && fabsf(pu[1]) < 90)
                {
                    pu[0] *= f;
                    pu[1] *= f;
                    pv[0] *= f;
                    pv[1] *= f;
                }
            }
        }
        // panic while holding: a bot in HideState that holds its position does not open fire on an enemy who is not
        // shooting at it (measured in the circle test: circler 250 units in front, in plain view, no attack for
        // seconds; the stock "hold" disposition waits to be fired on). Panic drops the hold (-> IdleState, the bot's own
        // re-decide state: it engages what it sees); the step-9 plan re-issues the hold after the fight. Not on hostage
        // maps: there the step-9c guards hold the hostages (dropping it cost rescues 44% -> 27% in 2 office matches).
        // Only when the hold is really what blocks it: the runner is NOT shooting (a shooting enemy gets fought from the
        // hold anyway), has been in its view for 0.3 s and it still has not opened fire (dropping every hold at first
        // contact cost the holders their angle: the kid floor fell to 77%).
        auto ce = g_pl.find(s.closeName);
        bool quietRunner = ce == g_pl.end() || now - ce->second.sh.firedAt > 1.5f;
        if (on && s.panic && !attack && al && g_layout == 1 && !g_mi.hostage && quietRunner && now - s.lastSeen < 0.1f &&
            now - s.sightAt > 0.3f && now - s.unholdAt > 1.f && StateName(p.ent) == "9HideState")
        {
            SetBotState(p.ent, p.ent + kOffIdle);
            s.unholdAt = now;
            s.unholds++;
        }
        // panic turn: the close enemy is outside the bot's view (more than 50 deg off) -> the bot turns towards him until
        // he is in view (then its own reaction takes over). Lever (measured with a write probe, logs\probe2.txt): the bot's current view angle is the
        // player copy at +0x0c7c pitch / +0x0c80 yaw (the bot integrates its look spring from it every frame; writes to
        // m_angEyeAngles or the bot's own +0x3b68 copy alone are overwritten). Written together with m_angEyeAngles, only
        // while that copy agrees with the eye angles (layout check), turning at 600 .. 1200 deg/s by the react dial.
        if (on && s.panic && g_s.turn && al && dt > 0 && g_np.eyeY > 0)
        {
            float *vy = (float *)(p.ent + kOffViewYaw), *vp = (float *)(p.ent + kOffViewYaw - 4);
            float *ey = (float *)(p.ent + g_np.eyeY), *ep = (float *)(p.ent + g_np.eyeP);
            Vec eye = EyeOf(p.ent, pos[p.name]);
            float want = YawTo(eye, s.closePos), diff = Wrap180(want - *vy);
            bool layoutOk = std::isfinite(*vy) && fabsf(Wrap180(*vy - *ey)) < 2.f && std::isfinite(*vp) && fabsf(*vp - *ep) < 2.f && fabsf(*vp) <= 90.f;
            if (layoutOk && fabsf(diff) > 50.f)   // only an enemy OUTSIDE its view (in view it reacts itself)
            {
                float step = Lerp(600.f, 1200.f, s.react) * dt;
                float ny = *vy + std::max(-step, std::min(step, diff));
                float dz = s.closePos.z + 50.f - eye.z, dxy = Dist({eye.x, eye.y, 0}, {s.closePos.x, s.closePos.y, 0});
                float wp = atan2f(-dz, std::max(1.f, dxy)) * 57.29578f;
                float np = *vp + std::max(-step, std::min(step, wp - *vp));
                *vy = ny;
                *ey = ny;
                *vp = np;
                *ep = np;
                s.turns++;
            }
        }
    }
    if (g_s.circle && live) CircleFrame(now, dt, pos);
}
static void ShootRoundStart()
{
    g_circ = Circ();
    for (auto &kv : g_pl)
    {
        Player &p = kv.second;
        p.sh.eng = false;
        p.sh.sightAt = p.sh.lastSeen = p.sh.closeAt = -100;
        p.sh.panic = false;
        if (!p.bot) continue;
        p.sh.react = DialOf(p, D_REACT, false);
        p.sh.spray = DialOf(p, D_SPRAY, false);
        p.sh.aimd = DialOf(p, D_AIM, false);
        p.sh.lucky = g_s.lucky && Rnd() < g_s.luckyP;
    }
}
static void ShootRoundEnd()
{
    for (auto &kv : g_pl)
        if (kv.second.bot) EngClose(kv.second);
    if (g_s.circle) CircLog(1);
}
static void ShootRestoreAll()
{
    for (auto &kv : g_pl)
        if (kv.second.bot && kv.second.sh.state == 1) ProfRestore(kv.second);
}
static void ShootInit(BrainIfaceFn engineFactory)
{
    g_trace = engineFactory ? engineFactory("EngineTraceServer004", nullptr) : nullptr;
    g_traceOk = g_trace ? 0 : -1;
    BLog("shoot: engine trace %s", g_trace ? "EngineTraceServer004 found" : "missing - sight metrics off");
}
static void ShootLevelInit()
{
    LoadDb();
    g_profBad = 0;
    g_circ = Circ();
    g_shLast = 0;
    g_npCheck = 0;
}
// ==== end STEP 11 ===================================================================================================

// ==== STEP 12 (Job 5, 2026-09-26): batch 2 parts B-K - bots that play like people ===================================
// His verdict after part A (Gold Nova I, de_dust2): better up close, but "not rotating right, not aiming right, not
// reacting right" - the bar is bots that play like players. Each part below is one loop a player runs, behind its own
// kill switch (addons/family_test.txt, re-read every second; switch off = the step-11 bots exactly):
//   B hearing        brain_hear 0      hear -> turn to the likely angle (every rank; higher = sooner, sharper, holds)
// Shared here: every player's entity (humans too, for their walk / duck / eye angles), per-frame positions and speeds,
// the user-id map for humans (step 10/11 mapped bots only: a human's shots never set sh.firedAt - fixed here), and ONE
// look controller per bot (LK_*): the only writer of a bot's view outside fights besides step 11's panic turn, which
// always wins. The view lever is step 11's (+0x0c80 yaw / +0x0c7c pitch with m_angEyeAngles, written only while that
// copy agrees with the eye angles); nothing is written while the bot fights (m_isAttacking) - the stock aim owns fights.
enum { LK_NONE = 0, LK_PREAIM = 1, LK_CLEAR = 2, LK_PEEK = 3, LK_HEAR = 4, LK_FIRE = 5, LK_UTIL = 6, LK_VETO = 7 };   // JOB 5b O: LK_UTIL
static struct J5T
{
    int hear = 1, hearTurn = 1, hearStop = 1, hearCall = 1, hearTeam = 0, hearLog = 60, chkLog = 150, sneak = 0, sneakWalk = 0;
    float hearDial = -1;   // test: every bot's hearing dial, absolute (A/B)
    int fire = 1, fireTurn = 1, fireDodge = 1, fireCover = 1, fireTeam = 0, fireLog = 80;   // STEP 12G
    float fireDial = -1, fireSkill = -1;
    int ct = 1, ctAnchor = 1, ctStack = 1, ctHold = 1, ctCalls = 1, ctStackForce = -1;          // STEP 12C
    int plan = 1, planChat = 1, planSay = 1, planHuman = 1, planMid = 1, follow = 1, planForce = -1;   // STEP 12D/H/J
    int followTest = 0, overruleTest = 0;   // tests: a bot bomber stands in for the human carrier; a pretend human types a site
    int peek = 1, peekPre = 1, peekJig = 1, peekCs = 1, peekTeam = 0, peekLog = 60;   // STEP 12E
    float peekDial = -1;
    int gold = 1, goldTeam = 0, walkup = 0, walkupWalk = 0, unhold = 1, unholdLog = 30;  // STEP 12K
    int clear = 1, post = 1, retake = 1, nade = 1;                                       // STEP 12F
    float goldRt = 0.30f, goldFi = 0.40f, goldFo = 0.30f, goldLa = 0.35f;
    fl::HearRanges hr = fl::DefaultRanges();
    time_t at = 0;
} g_j;
static void RefreshJ5()
{
    time_t t = time(nullptr);
    if (!g_tv || t == g_j.at) return;   // the test file is read at most once a second
    g_j.at = t;
    const J5T d;
    g_j.hear = (int)g_tv("brain_hear", d.hear);
    g_j.hearTurn = (int)g_tv("hear_turn", d.hearTurn);
    g_j.hearStop = (int)g_tv("hear_stop", d.hearStop);
    g_j.hearCall = (int)g_tv("hear_call", d.hearCall);
    g_j.hearTeam = (int)g_tv("hear_team", d.hearTeam);   // A/B: only this team hears the new way (2 T, 3 CT)
    g_j.hearLog = (int)g_tv("hear_log", d.hearLog);      // HEAR lines per round (the rest are counted)
    g_j.chkLog = (int)g_tv("hear_chklog", d.chkLog);     // HEARCHK lines per round (the rest are counted in HEARR)
    g_j.hearDial = (float)g_tv("hear_dial", d.hearDial);
    g_j.sneak = (int)g_tv("hear_test", d.sneak);         // sneak test: approach distance (units), 0 = off
    g_j.sneakWalk = (int)g_tv("hear_test_walk", d.sneakWalk);
    g_j.fire = (int)g_tv("brain_fire", d.fire);           // STEP 12G
    g_j.fireTurn = (int)g_tv("fire_turn", d.fireTurn);
    g_j.fireDodge = (int)g_tv("fire_dodge", d.fireDodge);
    g_j.fireCover = (int)g_tv("fire_cover", d.fireCover);
    g_j.fireTeam = (int)g_tv("fire_team", d.fireTeam);    // A/B: only this team reacts the new way
    g_j.fireLog = (int)g_tv("fire_log", d.fireLog);
    g_j.fireDial = (float)g_tv("fire_dial", d.fireDial);  // test: every bot's fire dial, absolute
    g_j.fireSkill = (float)g_tv("fire_skill", d.fireSkill);   // test: the dodge Skill, absolute
    g_j.ct = (int)g_tv("brain_ct", d.ct);                 // STEP 12C
    g_j.ctAnchor = (int)g_tv("ct_anchor", d.ctAnchor);
    g_j.ctStack = (int)g_tv("ct_stack", d.ctStack);
    g_j.ctHold = (int)g_tv("ct_hold", d.ctHold);
    g_j.ctCalls = (int)g_tv("ct_calls", d.ctCalls);
    g_j.ctStackForce = (int)g_tv("ct_stack_site", d.ctStackForce);   // test: stack this site index every round
    g_j.plan = (int)g_tv("brain_plan", d.plan);           // STEP 12D/J
    g_j.planChat = (int)g_tv("plan_chat", d.planChat);     // a bot says the plan in team chat
    g_j.planSay = (int)g_tv("plan_say", d.planSay);        // no chat lever: console "say" when no human is on CT
    g_j.planHuman = (int)g_tv("plan_human", d.planHuman);  // a human T's "a" / "b" overrules
    g_j.planMid = (int)g_tv("plan_midcap", d.planMid);     // J: never two mid-heavy plans in three rounds
    g_j.follow = (int)g_tv("brain_follow", d.follow);      // STEP 12H: follow + escort a human bomb carrier
    g_j.planForce = (int)g_tv("plan_site", d.planForce);   // test: every plan at this site index
    g_j.followTest = (int)g_tv("follow_test", d.followTest);   // test: the bot bomber plays the "human" carrier (stock AI)
    g_j.overruleTest = (int)g_tv("plan_test_overrule", d.overruleTest);   // test: 3 s into the freeze "<site k-1>" is typed
    g_j.peek = (int)g_tv("brain_peek", d.peek);            // STEP 12E
    g_j.peekPre = (int)g_tv("peek_preaim", d.peekPre);
    g_j.peekJig = (int)g_tv("peek_jiggle", d.peekJig);
    g_j.peekCs = (int)g_tv("peek_cs", d.peekCs);
    g_j.peekTeam = (int)g_tv("peek_team", d.peekTeam);     // A/B: only this team
    g_j.peekLog = (int)g_tv("peek_log", d.peekLog);
    g_j.peekDial = (float)g_tv("peek_dial", d.peekDial);   // test: every bot's angles dial for E, absolute
    g_j.gold = (int)g_tv("brain_gold", d.gold);            // STEP 12K
    g_j.goldTeam = (int)g_tv("gold_team", d.goldTeam);     // A/B: only this team
    g_j.goldRt = (float)g_tv("gold_rt", d.goldRt);         // calibration: the full bump's cuts / lift
    g_j.goldFi = (float)g_tv("gold_fi", d.goldFi);
    g_j.goldFo = (float)g_tv("gold_fo", d.goldFo);
    g_j.goldLa = (float)g_tv("gold_laa", d.goldLa);
    g_j.walkup = (int)g_tv("walkup_test", d.walkup);       // walk-up test: the distance in front of the target (units)
    g_j.walkupWalk = (int)g_tv("walkup_walk", d.walkupWalk);
    g_j.unhold = (int)g_tv("gold_unhold", d.unhold);        // a holder engages a quiet enemy in plain view (any range)
    g_j.unholdLog = (int)g_tv("gold_unhold_log", d.unholdLog);
    g_j.clear = (int)g_tv("brain_clear", d.clear);         // STEP 12F5
    g_j.post = (int)g_tv("brain_post", d.post);            // STEP 12F7
    g_j.retake = (int)g_tv("brain_retake", d.retake);      // STEP 12F8
    g_j.nade = (int)g_tv("brain_nade", d.nade);            // STEP 12F9 (the veto; NADE lines always)
    for (int k = 0; k < fl::NZ_N; k++)
    {
        char key[32];
        snprintf(key, sizeof(key), "hear_r_%s", fl::NoiseName(k));
        g_j.hr.r[k] = (float)g_tv(key, d.hr.r[k]);
    }
    char k[600];
    snprintf(k, sizeof(k), "hear %d turn %d stop %d call %d team %d dial %.2f test %d/%d ranges step %.0f jump %.0f land %.0f shot %.0f sil %.0f reload %.0f plant %.0f defuse %.0f zoom %.0f"
             " | fire %d turn %d dodge %d cover %d team %d dial %.2f skill %.2f | ct %d anchor %d stack %d/%d hold %d calls %d"
             " | plan %d chat %d say %d human %d midcap %d follow %d site %d | peek %d pre %d jig %d cs %d team %d dial %.2f"
             " | gold %d team %d rt %.2f fi %.2f fo %.2f laa %.2f walkup %d/%d unhold %d | clear %d post %d retake %d nade %d",
             g_j.hear, g_j.hearTurn, g_j.hearStop, g_j.hearCall, g_j.hearTeam, g_j.hearDial, g_j.sneak, g_j.sneakWalk, g_j.hr.r[0], g_j.hr.r[1],
             g_j.hr.r[2], g_j.hr.r[3], g_j.hr.r[4], g_j.hr.r[5], g_j.hr.r[6], g_j.hr.r[7], g_j.hr.r[8], g_j.fire, g_j.fireTurn, g_j.fireDodge,
             g_j.fireCover, g_j.fireTeam, g_j.fireDial, g_j.fireSkill, g_j.ct, g_j.ctAnchor, g_j.ctStack, g_j.ctStackForce, g_j.ctHold, g_j.ctCalls,
             g_j.plan, g_j.planChat, g_j.planSay, g_j.planHuman, g_j.planMid, g_j.follow, g_j.planForce, g_j.peek, g_j.peekPre, g_j.peekJig, g_j.peekCs,
             g_j.peekTeam, g_j.peekDial, g_j.gold, g_j.goldTeam, g_j.goldRt, g_j.goldFi, g_j.goldFo, g_j.goldLa, g_j.walkup, g_j.walkupWalk, g_j.unhold, g_j.clear, g_j.post, g_j.retake, g_j.nade);
    static std::string last;
    if (last != k) BLog("job5 hooks: %s", k);
    last = k;
}
// the entity of any player: a bot (6CCSBot) or a human (9CCSPlayer), by RTTI like BotEntity
static uintptr_t PlayerEnt(edict_t *e)
{
    uintptr_t unk, vt;
    if (!SafeWord((uintptr_t)e + 12, unk) || !unk || !SafeWord(unk, vt)) return 0;
    static uintptr_t known[2];
    if (vt == known[0] || vt == known[1]) return unk;
    std::string n = VtName(vt);
    if (n == "6CCSBot") known[0] = vt;
    else if (n == "9CCSPlayer") known[1] = vt;
    else return 0;
    return unk;
}
static bool IsWalking(const Player &p) { return p.pent && g_np.walking > 0 && *(uint8_t *)(p.pent + g_np.walking) == 1; }
static bool IsDucking(const Player &p) { return p.pent && g_np.flags > 0 && (*(int *)(p.pent + g_np.flags) & 2); }
static bool EyeYaw(const Player &p, float *yaw)
{
    if (!p.pent || g_np.eyeY <= 0) return false;
    float y = *(float *)(p.pent + g_np.eyeY);
    if (!std::isfinite(y) || fabsf(y) > 400.f) return false;
    *yaw = y;
    return true;
}
// ---- the look controller ----
static bool LookWant(Player &p, int pri, const Vec &tgt, float speed, float holdFor, const char *why)
{
    float now = CurTime();
    if (p.lk.pri > pri && now < p.lk.until) return false;
    p.lk.pri = pri;
    p.lk.tgt = tgt;
    p.lk.speed = speed;
    p.lk.until = now + holdFor;
    p.lk.start = now;
    p.lk.onAt = -1;
    p.lk.why = why;
    return true;
}
static void LookFrame(Player &p, float now, float dt)
{
    if (!p.lk.pri) return;
    bool attack = BotAttacking(p.ent);
    // it sees an enemy: the stock reaction owns the view (only part G's turn to the shooter carries on, and only while
    // the shooter is the one it sees)
    bool turnToHim = p.lk.pri == LK_FIRE && !strcmp(p.lk.why, "fire") && p.seenName == p.fr.att && now - p.seenNameAt < 0.25f;
    bool sees = now - p.sh.lastSeen < 0.25f && now >= p.sh.lastSeen && !turnToHim;
    if (!p.falive || now > p.lk.until || attack || p.sh.panic || sees || g_np.eyeY <= 0 || g_np.eyeP <= 0 || g_layout != 1 || dt <= 0)
    {
        if (!p.falive || now > p.lk.until || attack || p.sh.panic || sees) p.lk.pri = 0;
        return;
    }
    float *vy = (float *)(p.ent + kOffViewYaw), *vp = (float *)(p.ent + kOffViewYaw - 4);
    float *ey = (float *)(p.ent + g_np.eyeY), *ep = (float *)(p.ent + g_np.eyeP);
    // the same agreement check as step 11's panic turn: the copy must hold the eye angles, else never write
    if (!(std::isfinite(*vy) && fabsf(Wrap180(*vy - *ey)) < 2.f && std::isfinite(*vp) && fabsf(*vp - *ep) < 2.f && fabsf(*vp) <= 90.f))
        return;
    Vec eye = EyeOf(p.ent, p.fpos);
    float wy = YawTo(eye, p.lk.tgt), wp = fl::PitchTo(eye, p.lk.tgt), step = p.lk.speed * dt;
    float d = Wrap180(wy - *vy);
    float ny = *vy + std::max(-step, std::min(step, d));
    float np = std::max(-89.f, std::min(89.f, *vp + std::max(-step, std::min(step, wp - *vp))));
    *vy = ny;
    *ey = ny;
    *vp = np;
    *ep = np;
    if (p.lk.onAt < 0 && fabsf(Wrap180(wy - ny)) < 3.f) p.lk.onAt = now;
}

// ---- STEP 12B: hearing ----------------------------------------------------------------------------------------------
// His words: bots barely react to sound; "running behind a bot, he can't turn and hear me"; "walking is so important, not
// being able to hear is crazy". Part A only turned a bot to an enemy inside 400 units WITH line of sight.
// Sources (game events, humans' too): player_footstep (the game fires it only for a step that makes a sound - a running
// player), player_jump (+ its landing 0.5 s later), weapon_fire (weapon_fire.silenced -> the short range), weapon_reload,
// bomb_beginplant, bomb_begindefuse, weapon_zoom. Ranges: fl::DefaultRanges (hooks hear_r_<kind>). Walking and
// crouching stay silent: a step from a player whose m_bIsWalking is set, who is ducking (m_fFlags FL_DUCKING) or who
// moves under 140 u/s is dropped even if the game fired it.
// Listeners: every living enemy bot in range that does not already SEE the source (in its 50 deg view with line of sight
// - then the stock bot reacts itself). After its reaction delay (fl::HearFor by the bot's "aware" dial - the map-awareness
// dial of step 10 that the research put sound reading under; profile-style, so an elo-300 bot still reacts, slowly) the
// bot turns (look controller) to where the enemy will appear: a higher rank pre-aims the corner he will come round (the
// nav hiding spot it can see nearest to the noise, fl::PickCorner), a lower rank the bare direction with a wider error;
// it holds that angle (0.9 .. 2.6 s), and the next steps of the same runner move the aim along (it tracks him through
// the wall). A higher rank also stops to listen (0.4 .. 0.9 s, off on urgent jobs) and calls it to the team (intel for
// the defenders' rotations, part C). Nothing is done while the bot fights, panics (step 11) or on an objective.
// Proof: "HEAR" line per reaction (first hear_log a round), "HEARCHK" per noise from outside a bot's view - did it face
// the source (within 30 deg) inside 2.5 s - written with the switch on AND off (before/after), "HEARR" per bot per round.
// Test harness hear_test <R>: 1v1, the T bot (muted: never notices, never shoots) runs round behind the CT bot and up to R
// units (hear_test_walk 1: it walks - must stay unheard); one "SNEAK" line per round.
// Switches: brain_hear 0 (all), hear_turn 0, hear_stop 0, hear_call 0; A/B: hear_team 2|3; hear_dial <v> (test).
struct Intel
{
    Vec pos;
    float t;
    int how;   // 1 heard, 2 seen
};
static std::map<std::string, Intel> g_intel[4];   // [team that knows] enemy name -> last known (STEP 12B/C)
static void IntelPut(int team, const std::string &enemy, const Vec &pos, float t, int how)
{
    if (team != 2 && team != 3) return;
    Intel &i = g_intel[team][enemy];
    if (t >= i.t - 0.01f || how >= i.how) i = Intel{pos, t, how};
}
static bool HearOn(const Player &p) { return g_j.hear && (!g_j.hearTeam || g_j.hearTeam == p.team) && g_modeOk; }
static float HearDial(const Player &p) { return g_j.hearDial >= 0 ? std::min(1.f, g_j.hearDial) : p.hr.h; }
static bool J5Pause(const Player &p, float now)
{
    if (p.pk.phase == 2 || p.pk.phase == 4) return true;   // STEP 12E: a jiggle's stops (exposed / in cover)
    return g_j.hear && g_j.hearStop && now < p.hr.stopUntil && HearOn(p);
}
static int g_hearLogged, g_chkLogged;
struct PendNoise
{
    std::string src;
    int kind;
    Vec pos;
    float at;
};
static std::vector<PendNoise> g_landings;   // a jump's landing, heard 0.5 s after the jump
static Player *FindPl(const std::string &n)
{
    auto it = g_pl.find(n);
    return it == g_pl.end() ? nullptr : &it->second;
}
// a noise made by `src` at `pos`: every enemy bot in range may hear it
static void HearNoise(Player &src, int kind, const Vec &pos, float now)
{
    if ((src.team != 2 && src.team != 3) || g_layout != 1) return;
    bool walking = IsWalking(src), ducking = IsDucking(src);
    for (auto &kv : g_pl)
    {
        Player &b = kv.second;
        if (!b.bot || !b.ent || !b.falive || b.team == src.team || (b.team != 2 && b.team != 3)) continue;
        float d = Dist(b.fpos, pos);
        if (!fl::Audible(g_j.hr, kind, d, src.hsp, walking, ducking)) continue;
        float yaw;
        if (!EyeYaw(b, &yaw)) continue;
        Vec eye = EyeOf(b.ent, b.fpos);
        float off = fabsf(Wrap180(YawTo(eye, pos) - yaw));
        bool los = Los(eye, {pos.x, pos.y, pos.z + 62.f}) || Los(eye, {pos.x, pos.y, pos.z + 42.f});
        // metric: a noise from outside the bot's view - will it face him within 2.5 s? (on and off alike)
        if (!b.hr.chk && off > 50.f && src.falive)
        {
            b.hr.chk = true;
            b.hr.chkSrc = src.name;
            b.hr.chkKind = kind;
            b.hr.chkT0 = now;
            b.hr.chkD = d;
        }
        if (!HearOn(b) || (off < 50.f && los)) continue;   // it sees him: the stock bot reacts itself
        if (BotAttacking(b.ent) || b.sh.panic || now - b.sh.lastSeen < 0.5f) continue;   // busy with an enemy it sees
        // a reaction in progress on this source follows him (his next steps move the aim along)
        if (b.lk.pri == LK_HEAR && b.hr.lookSrc == src.name && now < b.lk.until)
        {
            if (!b.hr.corner || Dist(pos, b.hr.cornerAt) > 600.f)
            {
                b.lk.tgt = {pos.x, pos.y, pos.z + 60.f};
                b.hr.corner = false;
            }
            b.lk.until = std::max(b.lk.until, now + 0.6f);
            continue;
        }
        if (kind == fl::NZ_STEP)
        {
            auto lb = b.hr.lastBy.find(src.name);
            if (lb != b.hr.lastBy.end() && now - lb->second < 1.5f && now >= lb->second) continue;   // a runner's steps: once in 1.5 s
        }
        int prio = fl::NoisePrio(kind);
        if (b.hr.pend && !fl::HearBetter(prio, d, b.hr.prio, b.hr.dist, now - b.hr.at)) continue;
        fl::HearReact r = fl::HearFor(HearDial(b), Rnd());
        b.hr.pend = true;
        b.hr.src = src.name;
        b.hr.kind = kind;
        b.hr.prio = prio;
        b.hr.npos = pos;
        b.hr.at = now;
        b.hr.dist = d;
        b.hr.due = now + r.delay;
        b.hr.lastBy[src.name] = now;
    }
}
static void SneakNoise(Player &src, int kind, const Vec &pos, float now);   // the sneak test (below)
static void HearEvent(const char *n, Player *p, void *ev, float now)
{
    if (!p || !p->pent || !g_pi || !p->e) return;
    int kind = -1;
    if (!strcmp(n, "player_footstep")) kind = fl::NZ_STEP;
    else if (!strcmp(n, "player_jump")) kind = fl::NZ_JUMP;
    else if (!strcmp(n, "weapon_reload")) kind = fl::NZ_RELOAD;
    else if (!strcmp(n, "bomb_beginplant")) kind = fl::NZ_PLANT;
    else if (!strcmp(n, "bomb_begindefuse")) kind = fl::NZ_DEFUSE;
    else if (!strcmp(n, "weapon_zoom")) kind = fl::NZ_ZOOM;
    else if (!strcmp(n, "weapon_fire"))
    {
        const char *w = VF<const char *(*)(void *, const char *, const char *)>(ev, 10)(ev, "weapon", "");
        if (!w || !IsGun(w)) return;
        kind = VF<int (*)(void *, const char *, int)>(ev, 7)(ev, "silenced", 0) ? fl::NZ_SHOT_SIL : fl::NZ_SHOT;
    }
    if (kind < 0) return;
    void *pi = g_pi(p->e);
    if (!pi) return;
    Vec pos = PI_Origin(pi);
    if (kind == fl::NZ_JUMP) g_landings.push_back({p->name, fl::NZ_LAND, pos, now + 0.5f});
    SneakNoise(*p, kind, pos, now);
    HearNoise(*p, kind, pos, now);
}
// a pending reaction is due: turn to the likely angle, maybe stop to listen and call it
static void HearAct(Player &b, float now)
{
    b.hr.pend = false;
    Player *src = FindPl(b.hr.src);
    if (!src || !b.falive || !b.ent || BotAttacking(b.ent) || b.sh.panic || !HearOn(b) || now - b.sh.lastSeen < 0.5f) return;
    int task = BotTask(b.ent);
    if (task == 1 && (g_plan.executed || g_plan.planted)) return;   // planting: its own job
    if (task == 3) return;                                            // defusing
    float h = HearDial(b);
    fl::HearReact r = fl::HearFor(h, 0.5f);
    Vec eye = EyeOf(b.ent, b.fpos);
    // where the noise is now: the source's position this frame if it is still near the noise (a runner moves on)
    Vec np = b.hr.npos;
    if (src->falive && Dist(src->fpos, np) < 400.f) np = src->fpos;
    Vec tgt{np.x, np.y, np.z + 60.f};
    const char *how = "direction";
    bool los = Los(eye, tgt);
    if (los)
        how = "sight line";
    else if (r.corner && !g_mi.spots.empty())
    {
        // the corner he will come round: nav hiding spots near the noise that the bot can see (<= 24 traces)
        std::vector<std::pair<float, Vec>> near;
        for (auto &s : g_mi.spots)
        {
            float ds = Dist(s.first, np);
            if (ds < 600.f && fabsf(s.first.z - np.z) < 200.f) near.push_back({ds, s.first});
        }
        std::sort(near.begin(), near.end(), [](const std::pair<float, Vec> &a, const std::pair<float, Vec> &c) { return a.first < c.first; });
        if (near.size() > 24) near.resize(24);
        std::vector<Vec> spots;
        std::vector<char> vis;
        for (auto &s : near)
        {
            spots.push_back(s.second);
            vis.push_back(Los(eye, {s.second.x, s.second.y, s.second.z + 56.f}) ? 1 : 0);
        }
        int k = fl::PickCorner(b.fpos, np, spots, vis);
        if (k >= 0)
        {
            tgt = {spots[k].x, spots[k].y, spots[k].z + 56.f};
            how = "corner";
        }
    }
    // the aim error of its rank (a sideways miss around the bot, and a third of it up/down)
    float e = (Rnd() * 2.f - 1.f) * r.errDeg, dxy = std::max(1.f, fl::Dist2(eye, tgt));
    float yaw = (YawTo(eye, tgt) + e) / 57.29578f;
    Vec aim{eye.x + dxy * cosf(yaw), eye.y + dxy * sinf(yaw), tgt.z + tanf((Rnd() * 2.f - 1.f) * r.errDeg / 3.f / 57.29578f) * dxy};
    bool turned = false;
    if (g_j.hearTurn && !CtFocusSkip(b, np) && LookWant(b, LK_HEAR, aim, r.turnSpeed, r.holdFor, "hear"))
    {
        b.hr.lookSrc = src->name;
        b.hr.corner = !strcmp(how, "corner");
        b.hr.cornerAt = tgt;
        turned = true;
    }
    bool stop = false;
    if (g_j.hearStop && Rnd() < r.stopChance && b.hr.dist < 900.f && !Urgent(b, task) && !ObjectiveTask(task, true))
    {
        b.hr.stopUntil = now + Lerp(0.4f, 0.9f, h);
        b.hr.stops++;
        stop = true;
    }
    bool call = false;
    if (g_j.hearCall && Rnd() < r.callChance)
    {
        IntelPut(b.team, src->name, np, now, 1);
        b.hr.calls++;
        call = true;
    }
    b.hr.reacts++;
    if (g_hearLogged++ < g_j.hearLog)
        BLog("HEAR map=%s r=%d bot=%s team=%s elo=%.0f hear=%.2f src=%s kind=%s d=%.0f delay=%.2f how=%s err=%.0f turn=%d stop=%d call=%d | heard %s at %.0fu -> %s %s (%.2f s)",
             g_mapName.c_str(), g_roundNo, b.name.c_str(), b.team == 2 ? "T" : "CT", b.elo, h, src->name.c_str(), fl::NoiseName(b.hr.kind), b.hr.dist,
             now - b.hr.at, !strcmp(how, "sight line") ? "sightline" : how, fabsf(e), (int)turned, (int)stop, (int)call, src->name.c_str(), b.hr.dist,
             b.name.c_str(), turned ? "turns" : "notes it", now - b.hr.at);
}
// ---- STEP 12G: reacting to fire -------------------------------------------------------------------------------------
// His words (09-26 playtest): "They don't move if they're getting shot at." Step 11 put every bot's Skill under Valve's
// 0.5 line (so they spray instead of strafe-tapping) - and the stock fight only side-steps above that line - and step
// 10's stop-to-shoot plants a footwork bot's feet for each shot: nothing moved a bot that was being shot.
// Under fire = hit (player_hurt) or shot at: an enemy fired (weapon_fire) with his aim within max(3 deg, a body width)
// of the bot and a clear line to it (humans' eye angles by send-table name, their entity by RTTI - part B).
// The reaction (fl::FireDecide by the bot's fire dial = mean of its footwork and react dials, profile-style: every rank)
// comes after the rank's delay (0.40 .. 0.10 s):
//   TURN  the shooter is outside its view -> look controller at 500 .. 1200 deg/s towards him (released the moment it
//         fights - then the stock aim has him)
//   DODGE it fights him -> for 1..2 s its profile Skill is lifted into the side-step band (0.55 .. 0.75; the stock
//         fight's own dodge), and a stop-to-shoot bot stops only 0.08 s after a shot instead of 0.22 (counter-strafe:
//         move, stop, shoot, move). A low rank dodges only up close, where the stock bot sprays at any Skill.
//   COVER it is losing the trade (<= 55 hp, 2 hits in 1.5 s or one heavy hit) and a nav hiding spot hidden from the
//         shooter (two world traces from his eye) is 80..450 u away, not towards him (fl::PickCover): it runs there -
//         its attack flag is cleared every frame of the dash and its ReactionTime held at 1 s, exactly how step 11's
//         circle test keeps its circler running (a fighting bot's own state never moves it) - then holds behind the
//         cover (HideState) pre-aiming where he was, and re-peeks from where it left (2.0 .. 0.8 s later by rank).
//         Never inside 300 u (a point-blank fight is fought: step 11's panic), never while planting / defusing.
// Proof: "FIRE" per reaction, written 1 s later with what the bot did (metres moved, mean speed, out of his sight);
// "FIRER" per bot per round: hits taken, how many it STOOD STILL through (speed < 40 u/s for 70% of the 1.5 s after the
// hit - written with the switch on and off: the before/after number), reactions, covers that broke the line, re-peeks.
// Switches: brain_fire 0 (all of G), fire_turn 0, fire_dodge 0, fire_cover 0; A/B fire_team 2|3; tests fire_dial,
// fire_skill, fire_log (FIRE lines per round, 80).
static bool IsAwp(const Player &p);   // STEP 12F8 (below)
static bool FireOn(const Player &p) { return g_j.fire && (!g_j.fireTeam || g_j.fireTeam == p.team) && g_modeOk && g_layout == 1; }
static float FireDial(const Player &p) { return g_j.fireDial >= 0 ? std::min(1.f, g_j.fireDial) : p.fr.g; }
static int g_fireLogged;
static bool J5Dash(const Player &p) { return p.fr.phase == 1 || p.pk.phase == 1 || p.pk.phase == 3; }   // runs (no quiet walk)
static bool J5Busy(const Player &p) { return p.fr.phase != 0 || p.pk.phase != 0; }
// may a J5 move (cover dash, re-peek, jiggle) take the bot over? Never on the bomb / hostage job (planting, defusing,
// its way there) and only from the states step 9's Steer takes over, plus a fight (the dash breaks one off on purpose)
static bool J5State(const Player &p)
{
    if (!p.ent) return false;
    std::string st = StateName(p.ent);
    return st == "9IdleState" || st == "9HuntState" || st == "11MoveToState" || st == "9HideState" || st == "11FollowState" ||
           st == "21InvestigateNoiseState" || st == "11AttackState";
}
static bool J5MayMove(const Player &p) { return p.ent && !ObjectiveTask(BotTask(p.ent), true) && J5State(p); }
static float J5StopUntil(const Player &p, float now)
{
    if (now < p.fr.dodgeUntil && FireOn(p)) return std::min(p.stopUntil, p.lastFire + 0.08f);
    return p.stopUntil;
}
static void J5Want(Player &p, float *w)
{
    float now = CurTime();
    if (!FireOn(p)) return;
    if (now < p.fr.dodgeUntil && g_j.fireDodge)
        w[PF_SKILL] = std::max(w[PF_SKILL], g_j.fireSkill >= 0 ? g_j.fireSkill : p.fr.skill);   // the stock fight side-steps
    if (RetakeOn() && g_plan.planted && p.team == 3 && (p.role == "retake" || p.role == "retake-awp") && IsAwp(p))
    {
        w[PF_ADELAY] = 0.f;                          // STEP 12F8: an AWP on the retake shoots what it sees
        w[PF_REACT] = std::min(w[PF_REACT], 0.15f);
    }
    if (p.fr.phase == 1)
    {
        w[PF_REACT] = std::max(w[PF_REACT], 1.0f);   // running for cover: it does not turn back to fight mid-dash
        w[PF_ADELAY] = std::max(w[PF_ADELAY], 0.5f);
    }
}
static int BotHealth(const Player &p)
{
    if (!p.pent || g_np.health <= 0) return -1;
    int h = *(int *)(p.pent + g_np.health);
    return h >= 0 && h <= 500 ? h : -1;
}
// the bot sees him: inside its 50-deg view with a clear line to his head or chest
static bool Sees(const Player &b, const Player &e)
{
    float yaw;
    if (!EyeYaw(b, &yaw)) return false;
    Vec eye = EyeOf(b.ent, b.fpos);
    if (fabsf(Wrap180(YawTo(eye, e.fpos) - yaw)) > 50.f) return false;
    return Los(eye, {e.fpos.x, e.fpos.y, e.fpos.z + 62.f}) || Los(eye, {e.fpos.x, e.fpos.y, e.fpos.z + 42.f});
}
static void FireTrigger(Player &b, Player &att, bool hit, float now)
{
    if (!b.bot || !b.ent || !b.falive || !FireOn(b)) return;
    Player::Fr &f = b.fr;
    if (f.phase != 0) return;                                   // already breaking the line / re-peeking
    bool losingNow = false;
    if (hit)
    {
        float dmg = 0;
        for (auto &h : f.hitsT) dmg += h.second;
        fl::FireSit q{FireDial(b), true, true, (float)BotHealth(b), dmg, (int)f.hitsT.size(), Dist(b.fpos, att.fpos), true, false};
        losingNow = fl::FireLosing(q);
    }
    // one reaction a second, unless it now starts losing (then it may still break the line)
    if (f.due > 0 || (now - f.actAt < 1.2f && !(losingNow && f.act != fl::FA_COVER))) return;
    f.att = att.name;
    f.pendHit = hit;
    f.due = now + fl::FireDecide(fl::FireSit{FireDial(b), true, hit, 100, 0, 0, 0, false, false}, 1, 1).delay;
}
static void FireOnHurt(Player *a, Player *v, int dmg, float now)
{
    if (!a || !v || a == v || a->team == v->team || !v->bot || !v->ent) return;
    Player::Fr &f = v->fr;
    while (!f.hitsT.empty() && (now - f.hitsT.front().first > 1.5f || now < f.hitsT.front().first)) f.hitsT.erase(f.hitsT.begin());
    f.hitsT.push_back({now, (float)dmg});
    f.nHit++;
    if (!f.stillOpen)
    {
        f.stillOpen = true;   // stood still through it? (sampled in FireFrame, on and off alike)
        f.stillAt = now;
        f.stillV.clear();
    }
    FireTrigger(*v, *a, true, now);
}
static void WalkupShot(Player &sh, float now);   // STEP 12K (below)
static void RetakeShot(Player &sh);             // STEP 12F8 (below)
static void WalkupHurt(Player *a, Player *v, int group, int dmg, float now);
static void FireOnShot(Player &sh, float now)
{
    WalkupShot(sh, now);
    RetakeShot(sh);
    if ((sh.team != 2 && sh.team != 3) || !sh.pent || g_np.eyeY <= 0 || g_np.eyeP <= 0 || g_layout != 1) return;
    float yaw, pitch = *(float *)(sh.pent + g_np.eyeP);
    if (!EyeYaw(sh, &yaw) || !std::isfinite(pitch) || fabsf(pitch) > 90.f) return;
    Vec eye = EyeOf(sh.pent, sh.fpos);
    for (auto &kv : g_pl)
    {
        Player &b = kv.second;
        if (!b.bot || !b.ent || !b.falive || b.team == sh.team || (b.team != 2 && b.team != 3)) continue;
        Vec chest{b.fpos.x, b.fpos.y, b.fpos.z + 44.f};
        float d = Dist(eye, chest);
        if (d > 3000.f || d < 1.f) continue;
        float cone = std::max(3.f, atan2f(32.f, d) * 57.29578f);
        if (fabsf(Wrap180(YawTo(eye, chest) - yaw)) > cone || fabsf(fl::PitchTo(eye, chest) - pitch) > cone) continue;
        if (!Los(eye, chest) && !Los(eye, {b.fpos.x, b.fpos.y, b.fpos.z + 62.f})) continue;
        b.fr.shotAt = now;
        b.fr.nShotAt++;
        FireTrigger(b, sh, false, now);
    }
}
static void FireLog(Player &b, float now)
{
    Player::Fr &f = b.fr;
    f.logPend = false;
    if (g_fireLogged++ >= g_j.fireLog) return;
    float moved = fl::Dist2(b.fpos, f.actPos), sp = f.spdN ? f.spdSum / f.spdN : 0.f;
    const char *what = f.act == fl::FA_TURN ? "turns to him" : f.act == fl::FA_DODGE ? "dodges while it fights" : f.act == fl::FA_COVER ? "breaks the line to cover" : "keeps fighting";
    BLog("FIRE map=%s r=%d bot=%s team=%s elo=%.0f dial=%.2f src=%s hit=%d hp=%.0f hits=%d d=%.0f act=%s moved=%.0f speed=%.0f outOfSight=%d alive=%d | %s %s by %s at %.0fu -> %s (moved %.0fu in %.1f s)",
         g_mapName.c_str(), g_roundNo, b.name.c_str(), b.team == 2 ? "T" : "CT", b.elo, FireDial(b), f.att.c_str(), (int)f.pendHit, f.hp, f.lhits, f.dist,
         fl::FireActName(f.act), moved, sp, (int)f.outOfSight, (int)b.falive, b.name.c_str(), f.pendHit ? "hit" : "shot at", f.att.c_str(), f.dist, what,
         moved, now - f.actAt);
}
// the reaction is due
static void FireAct(Player &b, float now)
{
    Player::Fr &f = b.fr;
    f.due = -1;
    Player *att = FindPl(f.att);
    if (!att || !att->falive || !b.falive || !FireOn(b) || b.sh.panic) return;
    // no reaction at all while planting / defusing (or in a state step 9 never steers); turn / dodge on the way to the
    // bomb, but never the cover dash (J5MayMove)
    int task = BotTask(b.ent);
    bool mayMove = J5MayMove(b);
    bool objective = !J5State(b) || task == 3 || task == 5 || (task == 1 && (g_plan.executed || g_plan.planted));
    while (!f.hitsT.empty() && now - f.hitsT.front().first > 1.5f) f.hitsT.erase(f.hitsT.begin());
    float dmg = 0;
    for (auto &h : f.hitsT) dmg += h.second;
    fl::FireSit q{FireDial(b), Sees(b, *att), f.pendHit, (float)BotHealth(b), dmg, (int)f.hitsT.size(), Dist(b.fpos, att->fpos), false, objective};
    if (q.health < 0) q.health = 100;
    // a cover spot only when it is losing (<= 12 nav spots near it, two traces each from his eye)
    Vec spot{};
    Vec aeye = EyeOf(att->pent ? att->pent : b.ent, att->fpos);
    if (g_j.fireCover && mayMove && fl::FireLosing(q) && q.dist >= 300.f && !g_mi.spots.empty())
    {
        std::vector<std::pair<float, int>> near;
        for (size_t i = 0; i < g_mi.spots.size(); i++)
        {
            float d = fl::Dist2(g_mi.spots[i].first, b.fpos);
            if (d >= 80.f && d <= 450.f && fabsf(g_mi.spots[i].first.z - b.fpos.z) < 80.f) near.push_back({d, (int)i});
        }
        std::sort(near.begin(), near.end());
        if (near.size() > 12) near.resize(12);
        std::vector<Vec> sp;
        std::vector<char> hid, cov;
        for (auto &n : near)
        {
            const Vec &v = g_mi.spots[n.second].first;
            sp.push_back(v);
            hid.push_back(!Los(aeye, {v.x, v.y, v.z + 64.f}) && !Los(aeye, {v.x, v.y, v.z + 40.f}) ? 1 : 0);
            cov.push_back((g_mi.spots[n.second].second & 1) ? 1 : 0);
        }
        int k = fl::PickCover(b.fpos, att->fpos, sp, hid, cov);
        if (k >= 0)
        {
            spot = sp[k];
            q.cover = true;
        }
    }
    fl::FireChoice c = fl::FireDecide(q, Rnd(), Rnd());
    if (c.act == fl::FA_TURN && !g_j.fireTurn) c.act = fl::FA_NONE;
    if (c.act == fl::FA_DODGE && !g_j.fireDodge) c.act = fl::FA_NONE;
    f.act = c.act;
    f.actAt = now;
    f.actPos = b.fpos;
    f.dist = q.dist;
    f.hp = q.health;
    f.lhits = q.hits;
    f.outOfSight = false;
    f.spdSum = 0;
    f.spdN = 0;
    f.turnSpeed = c.turnSpeed;
    if (c.act == fl::FA_TURN)
    {
        if (LookWant(b, LK_FIRE, {att->fpos.x, att->fpos.y, att->fpos.z + 50.f}, c.turnSpeed, 1.0f, "fire")) f.nTurn++;
    }
    else if (c.act == fl::FA_DODGE)
    {
        f.dodgeUntil = now + Lerp(1.0f, 2.0f, q.dial);
        f.skill = c.skill;
        f.nDodge++;
    }
    else if (c.act == fl::FA_COVER)
    {
        f.phase = 1;
        f.phaseAt = now;
        f.spot = spot;
        f.from = b.fpos;
        f.attPos = att->fpos;
        f.hold = c.hold;
        f.reissue = 0;
        f.nCover++;
    }
    f.logPend = c.act != fl::FA_NONE;
    f.logAt = now + 1.0f;
}
static void FireFrame(Player &b, float now)
{
    Player::Fr &f = b.fr;
    if (f.due > 0 && now >= f.due) FireAct(b, now);
    if (f.due > 0 && now < f.due - 5.f) f.due = -1;   // the clock went back (new level)
    if (f.logPend)
    {
        f.spdSum += b.hsp;
        f.spdN++;
        if (now >= f.logAt || !b.falive) FireLog(b, now);
    }
    // stood still through a hit? (on and off alike; 10 samples a second for 1.5 s)
    if (f.stillOpen && now >= f.nextSample)
    {
        f.nextSample = now + 0.1f;
        if (b.falive) f.stillV.push_back(b.hsp);
        if (now - f.stillAt >= 1.5f || !b.falive)
        {
            f.stillOpen = false;
            if (b.falive && fl::StoodStill(f.stillV)) f.nStill++;
        }
    }
    if (!f.phase) return;
    Player *att = FindPl(f.att);
    bool attack = BotAttacking(b.ent);
    if (!b.falive || !FireOn(b) || !att || !att->falive || !J5MayMove(b))
    {
        f.phase = 0;   // over (dead, switch off, he is gone, or the bomb / its own business came up): hands off
        return;
    }
    if (f.phase == 1)
    {
        // the dash: no fight until it is behind the cover (step 11's circle-test lever), fastest route
        if (Dist(b.fpos, att->fpos) < 250.f)
        {
            f.phase = 0;   // he pushed into it: fight
            return;
        }
        *(uint8_t *)(b.ent + kOffStateTime + 4) = 0;
        if (now >= f.reissue)
        {
            BotMoveTo(b.ent, f.spot, 1);
            f.reissue = now + 0.4f;
        }
        if (fl::Dist2(b.fpos, f.spot) < 70.f || now - f.phaseAt > 1.3f)
        {
            Vec aeye = EyeOf(att->pent ? att->pent : b.ent, att->fpos), eye = EyeOf(b.ent, b.fpos);
            f.outOfSight = !Los(aeye, eye) && !Los(aeye, {b.fpos.x, b.fpos.y, b.fpos.z + 40.f});
            if (f.outOfSight) f.nCoverOk++;
            BotHideHere(b.ent, b.fpos, f.hold + 2.f);
            LookWant(b, LK_HEAR, {f.attPos.x, f.attPos.y, f.attPos.z + 60.f}, f.turnSpeed, f.hold + 1.5f, "fire: pre-aim the re-peek");
            f.phase = 2;
            f.phaseAt = now;
        }
        return;
    }
    if (attack)
    {
        f.phase = 0;   // he came round the corner (or it saw him on the re-peek): the fight is on
        return;
    }
    if (f.phase == 2 && now - f.phaseAt >= f.hold)
    {
        BotMoveTo(b.ent, f.from, 1);   // re-peek from where it was
        f.phase = 3;
        f.phaseAt = now;
        f.nPeek++;
        if (g_fireLogged < g_j.fireLog)
            BLog("FIRE map=%s r=%d bot=%s act=repeek hold=%.1f | %s re-peeks %s after %.1f s behind cover", g_mapName.c_str(), g_roundNo, b.name.c_str(),
                 f.hold, b.name.c_str(), f.att.c_str(), f.hold);
    }
    else if (f.phase == 3 && (fl::Dist2(b.fpos, f.from) < 80.f || now - f.phaseAt > 1.5f))
        f.phase = 0;
}
// ---- STEP 12C: defenders --------------------------------------------------------------------------------------------
// His words: "they do a lot of pushing as defenders... they should hold corners, wait, catch you slipping; right now
// there's always somebody pushing somewhere"; default two on each site + one lurker/rotator ("stacking is fine"
// sometimes, not the default); "they should really try to keep at least one planted on site"; 09-26: "still not
// rotating right". Step 9's CT plan left a bot that failed its TakesBasic roll "free" (the stock AI: it roams and hunts
// = the pushing), sent a holder on a trade run into the Ts when its site-mate died, let a holder below 0.6 smarts chase
// every noise (InvestigateNoiseState), and on a site hit rotated everyone but a late anchor.
// Now (bomb maps; hostage maps keep step 9c):
//   setup  fl::CtRoles - 2 / 2 / 1 (mid = the rotator); an anchor on EVERY site; a smart team (mean smarts >= 0.35,
//          4+ bots) stacks 12% of rounds, 32% towards a site the Ts hit in 2 of their last 3 (fl::CtStackPick) - the
//          other sites keep their anchor. Anchors = the best angles dial (the site's anchor last round first), the
//          rotator = the best game sense, the rest keep last round's site. No free CT: a bot that fails its roll still
//          holds its site, at a sloppier spot (a post spot, 25 s holds). A smart bot varies its angle round to round
//          (off-angles, by its angles dial: another of the site's holds 20% .. 70% of rounds).
//   hold   a holding CT never runs to trade - it turns its angle to where the killer is (look controller, 3 s); it
//          chases a noise only by a roll per noise (35% at elo 300 .. 0% at the top; hearing, part B, turns it to the
//          noise instead); after a fight step 9's Steer walks it back to its hold as before.
//   rotate a site hit keeps every other site's anchor (step 9b's late-following anchor is gone); calls also come from
//          what the CTs HEARD or SAW (part B intel, last 4 s): 3+ Ts near a site's entrance = hit (level 2), one seen on
//          it = help (level 1); and one re-call if the first call was a fake (2+ Ts or the bomber at another site while
//          1 or none remain at the called one): the rotators and the mid go there over the CT side.
// Proof: "CTSET" per round (who holds what, anchors *, stack), "plan: ..." lines as in step 9 (+ anchors that hold,
// intel calls, re-calls), "CTR" per round (on and off alike): CTs that stood on T ground (fl::TGround - the Ts get there
// 3+ s before the CTs can) before the first T reached a site = pushers + push seconds; anchor coverage = share of the
// pre-plant time each site had a CT on it; noise chases / holds kept; re-angles; intel calls; re-calls.
// Switches: brain_ct 0 (step 9's CT plan), ct_anchor 0, ct_stack 0, ct_hold 0, ct_calls 0; test ct_stack_site <k>.
static std::vector<int> g_tHits;   // the site the Ts hit each round on this map (-1 none), newest last
static struct CtM
{
    float pushT = 0, preT = 0;
    std::set<std::string> pushers;
    std::map<std::string, float> cover;
    int chases = 0, keeps = 0, reangles = 0, calls = 0, recalls = 0, anchorsHeld = 0;
    float fwdT = 0, plannedT = 0, towardT = 0;   // JOB 5b L: CTs off their own ground (planned push or not), near a T there
    float spawnT = 0;                            // JOB 5b M: CT-seconds by the CT spawn (after the first 10 s)
    int midMax = 0;                              // JOB 5b M: most CTs near mid at once
    std::string stack = "-", setup;
    float nextTick = 0;
} g_ctm;
static bool CtOn() { return g_j.ct && g_steer && g_layout == 1 && g_mi.ok && !g_mi.hostage && g_modeOk; }
static bool CtAnchorStays(const Player *p, const std::string &hitSite)
{
    if (!CtOn() || !g_j.ctAnchor || g_plan.planted) return false;
    if (!p) return true;   // asked for the team: anchors are on
    return p->team == 3 && p->ct.anchor && !p->role.empty() && p->role != hitSite && p->role != "mid" && FindSite(p->role) != nullptr;
}
static int SiteIndex(const std::string &l)
{
    for (size_t i = 0; i < g_mi.sites.size(); i++)
        if (g_mi.sites[i].label == l) return (int)i;
    return -1;
}
// the spot a CT holds on its site: a smart one takes one of the site's holds (the k-th CT another one), and varies it
// from last round by its angles dial; a sloppy one (low smarts or a failed roll) a random post spot
static Vec CtSpot(Player *p, const Site &s, std::set<int> &used, bool sloppy)
{
    if (sloppy || p->s < 0.35f || s.hold.empty())
        return !s.post.empty() ? s.post[rand() % s.post.size()] : s.c;
    int m = (int)s.hold.size(), i = 0;
    while (i < m && used.count(i)) i++;
    if (i >= m) i = rand() % m;
    bool vary = Rnd() < Lerp(0.2f, 0.7f, DialOf(*p, D_ANGLES, false));
    if (vary || (p->ct.lastSite == s.label && Dist(s.hold[i], p->ct.lastSpot) < 50.f))
        for (int k = 1; k < m; k++)
        {
            int j = (i + k + rand() % m) % m;
            if (!used.count(j) && Dist(s.hold[j], p->ct.lastSpot) >= 50.f) { i = j; break; }
        }
    used.insert(i);
    return s.hold[i];
}
static void PlanCT12()
{
    auto bots = Team(3, true);
    int n = (int)bots.size(), ns = (int)g_mi.sites.size();
    if (!n || !ns) return;
    bool smart = MeanSmart(bots) >= 0.35f && n >= 4;
    int stack = g_j.ctStack ? fl::CtStackPick(g_tHits, ns, smart, Rnd(), Rnd()) : -1;
    if (g_j.ctStackForce >= 0 && g_j.ctStackForce < ns) stack = g_j.ctStackForce;
    std::vector<int> roles = fl::CtRoles(n, ns, g_mi.hasMid, stack);
    std::vector<Player *> left(bots.begin(), bots.end()), who(roles.size(), nullptr);
    auto take = [&](size_t slot, const std::function<float(Player *)> &score) {
        int best = -1;
        float bs = -1e9f;
        for (size_t i = 0; i < left.size(); i++)
        {
            float v = score(left[i]);
            if (v > bs) { bs = v; best = (int)i; }
        }
        if (best < 0) return;
        who[slot] = left[best];
        left.erase(left.begin() + best);
    };
    // anchors first (the best angle holders; last round's anchor of that site keeps it), then the rotator (sense),
    // then the rest (last round's site)
    for (size_t i = 0; i < roles.size(); i++)
        if ((int)i < ns)
        {
            const std::string &l = g_mi.sites[roles[i]].label;
            take(i, [&](Player *p) { return DialOf(*p, D_ANGLES, false) + (p->ct.lastSite == l && p->ct.anchor ? 0.3f : 0.f) + 0.15f * Rnd(); });
        }
    for (size_t i = 0; i < roles.size(); i++)
        if (roles[i] < 0) take(i, [&](Player *p) { return DialOf(*p, D_SENSE, false) + 0.15f * Rnd(); });
    for (size_t i = 0; i < roles.size(); i++)
        if (!who[i])
        {
            const std::string &l = g_mi.sites[roles[i]].label;
            take(i, [&](Player *p) { return (p->ct.lastSite == l ? 0.5f : 0.f) + 0.3f * Rnd(); });
        }
    std::map<int, std::set<int>> used;
    std::map<std::string, std::string> line;
    for (size_t i = 0; i < roles.size(); i++)
    {
        Player *p = who[i];
        if (!p) continue;
        p->ct.anchor = (int)i < ns;
        if (roles[i] < 0)
        {
            Assign(p, T_HOLD, g_mi.mid, 2, 150, "CT hold mid (rotator)", 45);
            p->role = "mid";
            p->ct.lastSite = "mid";
            if (PushOn()) p->ct.lastSpot = g_mi.mid;   // JOB 5b L: where it goes back to after a trade run (off: job 5's spots)
            line["mid"] += p->name + " ";
            continue;
        }
        const Site &st = g_mi.sites[roles[i]];
        bool sloppy = !TakesBasic(p);
        Vec spot = CtSpot(p, st, used[roles[i]], sloppy);
        Assign(p, T_HOLD, spot, 2, 120, p->ct.anchor ? "CT anchor" : sloppy ? "CT hold (sloppy)" : "CT hold", sloppy ? 25 : 45);
        p->role = st.label;
        p->ct.lastSite = st.label;
        p->ct.lastSpot = spot;
        line[st.label] += p->name + (p->ct.anchor ? "* " : sloppy ? "~ " : " ");
    }
    std::string out;
    for (auto &kv : line) out += kv.first + "=" + kv.second;
    g_ctm.stack = stack >= 0 ? g_mi.sites[stack].label : "-";
    g_ctm.setup = out;
    std::string reads;
    for (int h : g_tHits) reads += h >= 0 && h < ns ? g_mi.sites[h].label : "-";
    BLog("CTSET map=%s r=%d n=%d stack=%s reads=%s | %s(* anchor, ~ sloppy)", g_mapName.c_str(), g_roundNo, n, g_ctm.stack.c_str(),
         reads.empty() ? "-" : reads.substr(reads.size() > 6 ? reads.size() - 6 : 0).c_str(), out.c_str());
}
static bool CtHolder(const Player *p)
{
    return p->bot && p->team == 3 && p->task.kind == T_HOLD && !g_plan.planted &&
           (p->role == "mid" || p->role == "lurk" || FindSite(p->role) != nullptr);   // JOB 5b M: + the lurker
}
static bool CtKeeps(const Player *p) { return CtOn() && CtHolder(p); }
static bool CtReangle(Player *p, const Vec &at)
{
    if (!CtOn() || !g_j.ctHold || !CtHolder(p)) return false;
    if (Dist(p->pos, at) < 1800.f && p->ent && !BotAttacking(p->ent) && CurTime() - p->sh.lastSeen > 0.5f &&
        LookWant(*p, LK_HEAR, {at.x, at.y, at.z + 50.f}, 700.f, 3.f, "re-angle"))
        g_ctm.reangles++;
    return true;   // a holder never runs to trade
}
static int CtChase(Player &p, const std::string &st)
{
    if (!CtOn() || !CtHolder(&p)) return -1;
    if (st != "21InvestigateNoiseState")
    {
        p.ct.invAt = -1;
        return 0;
    }
    if (p.ct.invAt < 0)
    {
        p.ct.invAt = CurTime();
        // JOB 5b L: never with the push switch on - the noise chases were the "two my way every time"
        p.ct.invChase = PushOn() ? false : g_j.ctHold ? Rnd() < Lerp(0.35f, 0.f, p.s) : p.s < 0.6f;
        (p.ct.invChase ? g_ctm.chases : g_ctm.keeps)++;
    }
    return p.ct.invChase ? 1 : 0;
}
static void PeekSeen(Player &p, float now);   // STEP 12E (below)
static void ReactSeen(Player &p, Player &e, const Vec &at, float now);   // JOB 5c R (below)
static void J5Seen(Player &p, Player &e, const Vec &at, float now)
{
    IntelPut(p.team, e.name, at, now, 2);
    ReactSeen(p, e, at, now);
    if (now - p.sh.lastSeen > 1.0f) PeekSeen(p, now);   // first sight: counter-strafe (part E)
    p.seenName = e.name;
    p.seenNameAt = now;
}
// every TickPlan: calls from intel, the re-call after a fake
static void CtTick()
{
    if (!CtOn() || g_plan.planted || g_phase != PH_LIVE) return;
    float now = CurTime();
    if (g_j.ctCalls && g_plan.hitLevel < 2)
        for (auto &s : g_mi.sites)
        {
            int near = 0, onSeen = 0;
            for (auto &kv : g_intel[3])
            {
                const Intel &i = kv.second;
                Player *t = FindPl(kv.first);
                if (now - i.t > 4.f || now < i.t || !t || !t->alive) continue;
                if (Dist(i.pos, s.entry) < 700.f || Dist(i.pos, s.c) < s.radius + 250.f) near++;
                if (i.how == 2 && Dist(i.pos, s.c) < s.radius + 150.f) onSeen++;
            }
            int before = g_plan.hitLevel;
            if (near >= 3)
            {
                OnSiteHit(s, 2);
                if (g_plan.hitLevel != before)
                {
                    g_ctm.calls++;
                    BLog("plan: CT call from what they heard/saw: %d Ts at %s", near, s.label.c_str());
                }
                break;
            }
            if (onSeen >= 1 && g_plan.hitLevel < 1)
            {
                OnSiteHit(s, 1);
                if (g_plan.hitLevel != before)
                {
                    g_ctm.calls++;
                    BLog("plan: CT call from what they saw: a T on %s", s.label.c_str());
                }
            }
        }
    if (g_plan.hitLevel >= 2 && !g_plan.recalled)
    {
        const Site *hs = FindSite(g_plan.hitSite);
        auto ts = Team(2, false);
        int old = 0;
        if (hs)
            for (auto *p : ts)
                if (Dist(p->pos, hs->c) < hs->radius + 600.f) old++;
        for (auto &s : g_mi.sites)
        {
            if (s.label == g_plan.hitSite) continue;
            int k = 0;
            bool bomber = false;
            for (auto *p : ts)
                if (Dist(p->pos, s.c) < s.radius + 250.f)
                {
                    k++;
                    if (p->name == g_bomber) bomber = true;
                }
            if ((k >= 2 || bomber) && old <= 1)
            {
                g_plan.recalled = true;
                g_plan.hitSite = s.label;
                g_plan.hitAt = now;
                int rot = 0;
                for (auto *p : Team(3, true))
                    if (p->role == "rotate" || p->role == "fallback" || p->role == "mid")
                    {
                        Assign(p, T_HOLD, RotateSpot(s, rot++), 1, 150, "CT re-rotate", 30, Lerp(1.5f, 0.3f, p->s));
                        SetRoute(p, s);
                        p->role = "rotate";
                        RotStart(p, s.label.c_str());   // JOB 5b N
                    }
                g_ctm.recalls++;
                BLog("plan: CT re-call - %s was a fake (%d T left there), %s is hit (%d%s) - %d CT(s) rotate back", hs ? hs->label.c_str() : "?", old,
                     s.label.c_str(), k, bomber ? " + the bomber" : "", rot);
                break;
            }
        }
    }
}
// 4 times a second (on and off alike): pushing and anchor coverage before the plant
static void CtMetrics(float now)
{
    if (g_phase != PH_LIVE || !g_mi.ok || g_mi.hostage || now < g_ctm.nextTick) return;
    if (g_ctm.nextTick > now + 5.f) g_ctm.nextTick = 0;
    g_ctm.nextTick = now + 0.25f;
    if (g_plan.planted) return;
    auto cts = Team(3, false);
    if ((int)cts.size() < (int)g_mi.sites.size()) return;
    g_ctm.preT += 0.25f;
    for (auto &s : g_mi.sites)
        for (auto *p : cts)
            if (Dist(p->pos, s.c) < s.radius + 150.f)
            {
                g_ctm.cover[s.label] += 0.25f;
                break;
            }
    if (!g_rm.hit.empty()) return;   // pushing = on T ground before the first T is on a site
    // JOB 5b M: parked by the CT spawn, and how many watch mid at once
    int midN = 0;
    for (auto *p : cts)
    {
        if (!p->bot) continue;
        if (now - g_liveAt > 10.f && Dist2(p->pos, g_mi.ctspawn) < 650.f) g_ctm.spawnT += 0.25f;
        midN += NearMid(p->pos);
    }
    g_ctm.midMax = std::max(g_ctm.midMax, midN);
    auto ts = Team(2, false);
    for (auto *p : cts)
    {
        if (!p->bot) continue;
        int oT = OccAt(2, p->pos), oC = OccAt(3, p->pos);
        if (fl::TGround(oT, oC))
        {
            g_ctm.pushT += 0.25f;
            g_ctm.pushers.insert(p->name);
        }
        // JOB 5b L: off its own ground (the CTs do not get there 3 s before the Ts): planned or not, and near a T there
        if (oC < 1200 && oT < 1200 && oC + 30 >= oT)
        {
            g_ctm.fwdT += 0.25f;
            if (PushPlanned(p->name)) g_ctm.plannedT += 0.25f;
            for (auto *t : ts)
                if (Dist(t->pos, p->pos) < 1200.f)
                {
                    g_ctm.towardT += 0.25f;
                    break;
                }
        }
    }
}
static void CtRoundEnd()
{
    if (!g_mi.ok || g_mi.hostage) return;
    g_tHits.push_back(SiteIndex(g_rm.hit));
    if (g_tHits.size() > 8) g_tHits.erase(g_tHits.begin());
    std::string cov;
    for (auto &s : g_mi.sites)
    {
        char b[48];
        snprintf(b, sizeof(b), " cover%s=%.0f", s.label.c_str(), g_ctm.preT > 0 ? 100.f * g_ctm.cover[s.label] / g_ctm.preT : -1.f);
        cov += b;
    }
    BLog("CTR map=%s r=%d on=%d stack=%s pushers=%d pushT=%.1f preT=%.1f%s chases=%d keeps=%d reangles=%d calls=%d recalls=%d hit=%s"
         " pushOn=%d fwdT=%.1f plannedT=%.1f towardT=%.1f setOn=%d spawnT=%.1f midMax=%d",
         g_mapName.c_str(), g_roundNo, (int)CtOn(), g_ctm.stack.c_str(), (int)g_ctm.pushers.size(), g_ctm.pushT, g_ctm.preT, cov.c_str(), g_ctm.chases,
         g_ctm.keeps, g_ctm.reangles, g_ctm.calls, g_ctm.recalls, g_rm.hit.empty() ? "-" : g_rm.hit.c_str(), (int)PushOn(), g_ctm.fwdT, g_ctm.plannedT,
         g_ctm.towardT, (int)CtSetOn(), g_ctm.spawnT, g_ctm.midMax);
}
// ---- STEP 12D: a bot speaks in team chat ---------------------------------------------------------------------------
// There is no server command that makes a bot talk. The game's own "say_team" is a console command of server.so
// (CON_COMMAND: a ConCommand object whose callback runs Host_Say for the "command client" - the player whose command
// the engine is running). We run it the way the engine runs a client's command: set the command client to the bot, call
// the callback with a CCommand ("say_team \"B split\""; the layout family_party.cpp already reads in ClientCommand),
// put the command client back. Every piece is found and checked at load, never guessed:
//   - server.so's mappings from /proc/self/maps (the one holding the server factory); the "say_team" name string in
//     its read-only part; the ConCommand = a word in its data / .bss that points at that string 12 bytes into an object
//     whose vtable's RTTI name is ConCommand (m_pszName at +12 after vtable, m_pNext, m_bRegistered); exactly one;
//     its callback (+24) inside server.so's code and its flags (+32) saying "plain CCommand callback" (not the old
//     no-argument kind, not an interface);
//   - the command client global: IServerGameClients::SetCommandClient (interface ServerGameClients004) found by its
//     code - the only vtable slot whose bytes are exactly "store arg1 to one global" (fl::SetterPattern) - never
//     called; the global must hold a player index (-1..64);
//   - the bot's index: edict + 6 (CBaseEdict::m_EdictIndex), trusted only when every player's index is 1..64, unique
//     and the edicts sit one array apart (fl::EdictStride), checked once a level.
// Proof: the game logs every chat line ("Chad<12><BOT><TERRORIST>" say_team "B split"); family_party.cpp hands say_team
// lines to BrainChatLine: a line seen within 4 s = "delivered". Two sends never seen (and none ever seen) = the lever
// is switched off for good this run. No lever: the closest thing is the console's "say" over the local RCON
// ("Console: [T] Chad: B split", everyone sees it - so only when no human is on CT; plan_say 0 = log only).
struct MapRange
{
    uintptr_t lo, hi;
    std::string perm, path;
};
static std::vector<MapRange> ReadMaps()
{
    std::vector<MapRange> v;
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return v;
    char line[600];
    while (fgets(line, sizeof(line), f))
    {
        unsigned long lo, hi;
        char perm[8] = "", path[512] = "";
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %511[^\n]", &lo, &hi, perm, path) < 3) continue;
        v.push_back({(uintptr_t)lo, (uintptr_t)hi, perm, path});
    }
    fclose(f);
    return v;
}
static struct Chat
{
    int state = 0;   // 0 not looked for, 1 ready, -1 off
    std::string why;
    uintptr_t cb = 0, cmdClient = 0;
    int stride = 0, strideBad = 0;
    int sent = 0, ok = 0, fail = 0, fallback = 0;
    struct Pend
    {
        std::string bot, text;
        float at;
    };
    std::vector<Pend> pend;
} g_chat;
static void (*g_serverSay)(const char *);   // family_party: "say <text>" over the local RCON (its worker thread)
struct CCmd   // tier1 CCommand, CS:GO (the layout family_party.cpp reads in ClientCommand)
{
    int argc, argv0Size;
    char argS[512], argvBuf[512];
    const char *argv[64];
};
static void ChatOff(const char *why)
{
    g_chat.state = -1;
    g_chat.why = why;
    BLog("team chat: %s - bots cannot speak in team chat; plan calls go to %s", why,
         "the console \"say\" when no human is on CT (plan_say), else the log only");
}
static void ChatInit(BrainIfaceFn serverFactory)
{
    g_chat = Chat();
    if (!serverFactory) return ChatOff("no server factory");
    std::vector<MapRange> maps = ReadMaps();
    std::string path;
    for (auto &m : maps)
        if ((uintptr_t)serverFactory >= m.lo && (uintptr_t)serverFactory < m.hi) path = m.path;
    if (path.empty()) return ChatOff("server.so not found in /proc/self/maps");
    std::vector<MapRange> mine, data;
    bool chain = false;
    uintptr_t lastHi = 0, total = 0;
    for (auto &m : maps)
    {
        bool rw = m.perm.size() >= 2 && m.perm[0] == 'r' && m.perm[1] == 'w';
        if (m.path == path)
        {
            mine.push_back(m);
            chain = rw;
            if (rw && total < (64u << 20)) { data.push_back(m); total += m.hi - m.lo; }
            lastHi = m.hi;
        }
        else if (chain && m.path.empty() && rw && m.lo == lastHi && total < (64u << 20))
        {
            data.push_back(m);   // server.so's .bss: only the first anonymous region right after its data
            total += m.hi - m.lo;
            chain = false;
        }
        else
            chain = false;
    }
    auto inCode = [&](uintptr_t a) {
        for (auto &m : mine)
            if (a >= m.lo && a < m.hi && m.perm.size() >= 3 && m.perm[2] == 'x') return true;
        return false;
    };
    std::vector<uintptr_t> strs;
    static const char pat[] = "\0say_team";   // + its terminating 0 (sizeof = 10)
    for (auto &m : mine)
    {
        if (m.perm.size() < 2 || m.perm[0] != 'r' || m.perm[1] == 'w') continue;
        const char *lo = (const char *)m.lo, *hi = (const char *)m.hi, *q = lo;
        while (q && q < hi && strs.size() < 16)
        {
            q = (const char *)memmem(q, hi - q, pat, sizeof(pat));
            if (!q) break;
            strs.push_back((uintptr_t)q + 1);
            q++;
        }
    }
    if (strs.empty()) return ChatOff("the say_team command name is not in server.so");
    std::vector<uintptr_t> objs;
    for (auto &m : data)   // read a page at a time through SafeRead: a region can vanish while we scan
        for (uintptr_t pg = m.lo & ~(uintptr_t)4095; pg < m.hi; pg += 4096)
        {
            uintptr_t words[1024];
            if (!SafeRead((void *)pg, words, sizeof(words))) continue;
            for (int i = 0; i < 1024; i++)
            {
                uintptr_t a = pg + 4 * i;
                if (a < m.lo + 12 || a + 4 > m.hi) continue;
                for (uintptr_t sa : strs)
                    if (words[i] == sa)
                    {
                        uintptr_t obj = a - 12, vt;
                        if (SafeWord(obj, vt) && VtName(vt) == "10ConCommand") objs.push_back(obj);
                    }
            }
        }
    if (objs.size() != 1) return ChatOff(objs.empty() ? "no ConCommand object named say_team" : "more than one say_team ConCommand");
    uintptr_t obj = objs[0], cb = 0;
    uint8_t bits = 0;
    if (!SafeWord(obj + 24, cb) || !SafeRead((void *)(obj + 32), &bits, 1) || !inCode(cb) || (bits & 6) != 2)
        return ChatOff("say_team's callback is not a plain CCommand callback");
    void *sgc = serverFactory("ServerGameClients004", nullptr);
    if (!sgc) sgc = serverFactory("ServerGameClients003", nullptr);
    uintptr_t vt = 0;
    if (!sgc || !SafeWord((uintptr_t)sgc, vt)) return ChatOff("no ServerGameClients interface");
    int found = 0, slot = -1;
    unsigned g = 0;
    for (int i = 0; i < 48; i++)
    {
        uintptr_t fn;
        if (!SafeWord(vt + 4 * i, fn) || !inCode(fn)) break;   // past the end of the vtable
        unsigned char code[24];
        unsigned a = 0;
        if (SafeRead((void *)fn, code, sizeof(code)) && fl::SetterPattern(code, sizeof(code), &a))
        {
            found++;
            g = a;
            slot = i;
        }
    }
    if (found != 1) return ChatOff(found ? "more than one one-line setter in ServerGameClients" : "SetCommandClient not found in ServerGameClients");
    bool gOk = false;
    for (auto &m : data) gOk = gOk || (g >= m.lo && g + 4 <= m.hi);
    int cur = -99;
    if (!gOk || !SafeRead((void *)(uintptr_t)g, &cur, 4) || cur < -1 || cur > 64) return ChatOff("the command-client global does not hold a player index");
    g_chat.cb = cb;
    g_chat.cmdClient = g;
    g_chat.state = 1;
    BLog("team chat: say_team ConCommand %p callback %p, SetCommandClient = ServerGameClients slot %d -> global %p (now %d): bots can speak "
         "(each line must show up in the server log)", (void *)obj, (void *)cb, slot, (void *)(uintptr_t)g, cur);
}
static int EdictIndexOf(edict_t *e)
{
    if (!e) return -1;
    if (!g_chat.stride)
    {
        std::vector<std::pair<unsigned long, int>> v;
        for (auto &kv : g_pl)
            if (kv.second.e) v.push_back({(unsigned long)kv.second.e, (int)*(int16_t *)((char *)kv.second.e + 6)});
        g_chat.stride = fl::EdictStride(v);
        if (!g_chat.stride)
        {
            if (g_chat.strideBad++ < 3) BLog("team chat: the edict indices of %d players do not check out - no chat this time", (int)v.size());
            return -1;
        }
        BLog("team chat: edict indices check out (%d players, one array, stride %d)", (int)v.size(), g_chat.stride);
    }
    int i = *(int16_t *)((char *)e + 6);
    return i >= 1 && i <= 64 ? i : -1;
}
static bool BotSay(Player &b, const std::string &text)
{
    if (!g_j.planChat || g_chat.state != 1 || !b.bot || !b.e) return false;
    int idx = EdictIndexOf(b.e);
    if (idx < 1) return false;
    std::string t;
    for (char c : text)
        if (c != '"' && c != ';' && (unsigned char)c >= 32 && t.size() < 120) t += c;
    static CCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    snprintf(cmd.argS, sizeof(cmd.argS), "say_team \"%s\"", t.c_str());
    cmd.argv0Size = 9;   // ArgS() = the text with its quotes (tier1's Tokenize puts it there; Host_Say strips them)
    memcpy(cmd.argvBuf, "say_team", 9);
    memcpy(cmd.argvBuf + 9, t.c_str(), t.size() + 1);
    cmd.argc = 2;
    cmd.argv[0] = cmd.argvBuf;
    cmd.argv[1] = cmd.argvBuf + 9;
    int *cc = (int *)g_chat.cmdClient;
    int old = *cc;
    *cc = idx - 1;   // the engine's command client = the bot's client slot
    ((void (*)(const CCmd &))g_chat.cb)(cmd);
    *cc = old;
    g_chat.sent++;
    g_chat.pend.push_back({b.name, t, CurTime()});
    return true;
}
static bool HumanOn(int team)
{
    for (auto &kv : g_pl)
        if (!kv.second.bot && kv.second.team == team) return true;
    return false;
}
// a T bot tells its team: team chat, else the console say (no human CT to overhear), else the log only
static std::string TeamSay(Player *b, const std::string &text)
{
    if (b && BotSay(*b, text)) return "chat";
    if (g_j.planSay && g_serverSay && !HumanOn(3))
    {
        g_serverSay(("[T] " + (b ? b->name + ": " : std::string()) + text).c_str());
        g_chat.fallback++;
        return "console";
    }
    return "log";
}
static void ChatFrame(float now)
{
    for (auto it = g_chat.pend.begin(); it != g_chat.pend.end();)
    {
        if (now - it->at < 4.f && now >= it->at) { ++it; continue; }
        g_chat.fail++;
        BLog("CHAT not delivered: %s's \"%s\" never showed up in the server log (%d sent, %d delivered)", it->bot.c_str(), it->text.c_str(), g_chat.sent,
             g_chat.ok);
        it = g_chat.pend.erase(it);
        if (!g_chat.ok && g_chat.fail >= 2 && g_chat.state == 1) ChatOff("two lines said, none reached the server log");
    }
}

// ---- STEP 12D/H/I/J: round plans ------------------------------------------------------------------------------------
// D (his approved list): at freeze time one T bot - the team's best (smarts) - calls the plan in team chat, 1.5 s into the
//   freeze ("A push", "A split", "B push", "B split", "B to A", "A to B", "mid to B"); the T bots then play it together:
//   no "free" T any more (a bot that fails its roll trickles to the called site by the called way, alone). Any human T
//   can overrule by typing a / b (also "go b", "b split", ...: fl::ParseCall) in team chat during the freeze (or the
//   first 4 s): the team switches to that site and a bot acknowledges ("ok B"). Human chat reaches the plugin through
//   the server log (family_party.cpp hands say_team lines over).
// J: plans vary by map and round (fl::PickPlan: every push / split / fake the map's approaches allow, weighted against
//   the last rounds on this map) and a mid-heavy plan (60%+ of the Ts through a *mid* approach) never follows one in
//   the last 2 rounds - so never 2 in any 3 ("not straight down mid doors two out of three rounds"). Every T walks its
//   group's approach waypoints (make_mapinfo's appr / apwp lines) to its stack, so no bot drifts down mid on its own
//   path; a split's two groups gather at their own stacks and go in together from their own chokes.
// H: a HUMAN bomb carrier's direction is a stronger overrule than any call: once he is clearly on one site's approach
//   (fl::CarrierHeading, held 1.5 s) the team re-plans to that site the way he went ("following bomb B" in chat), and
//   1-2 bots escort him all round; the team waits at the entrance for the bomb (up to 60 s), then goes in with him.
// I: the bomb goes to a fair random T each round: family_party.cpp sets bot_defer_to_human_items 0 (Valve's C4 handout
//   then picks among all Ts, bots included; switch brain_c4fair 0 = the level's own value); a bot carrier plays the
//   main group, never lurks, and goes in 0.8 s behind the first ones (the stock AI plants once it is on the site).
// Proof: "PLAN" per call / overrule / follow / acknowledgement, "CHAT delivered" per bot line seen in the server log,
// "C4" per round (who got the bomb: bot or human), "TPR" per round (the plan played, its source, how many Ts went
// through mid and which mid places, hit / plant / win).
// Switches: brain_plan 0 (step 9's T plan, no calls), plan_chat 0, plan_say 0, plan_human 0, plan_midcap 0,
// brain_follow 0, brain_c4fair 0 (family_party); test plan_site <site index>.
static struct TPS
{
    bool active = false, assigned = false, called = false;
    fl::TPlan plan;
    std::string text, caller, source = "bot", chat = "-", ackBy, ackText;
    float callAt = -1, ackAt = -1, headingAt = -1, nextMid = 0;
    int heading = -1, headingRoute = -1, follows = 0, overrules = 0, escortsLogged = 0, bomberKept = 0;
    std::map<std::string, std::set<std::string>> midPlaces;   // mid place -> Ts seen there in the first 45 s
    std::set<std::string> midTs;
} g_tp;
static std::vector<fl::TPlan> g_tpHist;          // this map's plans, newest last
static std::map<std::string, int> g_tpCount;     // this map: plan text -> rounds
static std::vector<fl::Route> g_routes;          // every T approach as a line from the T spawn to its site's entry
static std::vector<int> g_routeAp;               // ... and its approach index
static bool TPlanOn() { return g_j.plan && g_steer && g_layout == 1 && g_mi.ok && !g_mi.hostage && g_modeOk && g_mi.sites.size() >= 1; }
static bool T12() { return g_tp.assigned && TPlanOn(); }
static std::vector<fl::PlanSite> PlanSites()
{
    std::vector<fl::PlanSite> v;
    for (auto &s : g_mi.sites)
    {
        fl::PlanSite p;
        p.label = s.label;
        for (auto &a : s.ap) p.apMid.push_back(a.flags & 1 ? 1 : 0);
        v.push_back(p);
    }
    return v;
}
static const Approach &ApOf(const Site &s, int k) { return s.ap[k >= 0 && k < (int)s.ap.size() ? k : 0]; }
static int MainApOf(const Site &s)
{
    for (size_t i = 0; i < s.ap.size(); i++)
        if (!(s.ap[i].flags & 1)) return (int)i;
    return 0;
}
static void BuildRoutes()
{
    g_routes.clear();
    g_routeAp.clear();
    for (size_t i = 0; i < g_mi.sites.size(); i++)
        for (size_t k = 0; k < g_mi.sites[i].ap.size(); k++)
        {
            const Approach &a = g_mi.sites[i].ap[k];
            fl::Route r;
            r.site = (int)i;
            r.pts.push_back(g_mi.tspawn);
            for (auto &w : a.wp) r.pts.push_back(w);
            r.pts.push_back(a.stack);
            r.pts.push_back(a.choke);
            r.pts.push_back(a.entry);
            g_routes.push_back(r);
            g_routeAp.push_back((int)k);
        }
}
static Player *HumanCarrier()
{
    auto it = g_pl.find(g_bomber);
    if (it == g_pl.end() || (it->second.bot && !g_j.followTest) || it->second.team != 2 || !it->second.alive) return nullptr;
    return &it->second;   // follow_test 1: a bot bomber stands in for the human (the stock AI picks its site)
}
// the part of an approach's waypoints still ahead of the bot (like step 9b's SetRoute for the CT side)
static void SetVia(Player *p, const std::vector<Vec> &wp, const Vec &goal)
{
    p->task.via.clear();
    p->task.wp = 0;
    if (wp.empty()) return;
    size_t k = 0;
    for (size_t i = 1; i < wp.size(); i++)
        if (Dist(p->pos, wp[i]) < Dist(p->pos, wp[k])) k = i;
    if (k + 1 < wp.size() && Dist(p->pos, wp[k + 1]) < Dist(wp[k], wp[k + 1])) k++;
    if (Dist(p->pos, goal) < Dist(wp[k], goal)) return;
    for (size_t i = k; i < wp.size(); i++) p->task.via.push_back(wp[i]);
}
static Vec GroupStack(int g)
{
    const fl::TPlan &P = g_tp.plan;
    if (P.type == fl::PL_FAKE) return ApOf(g_mi.sites[P.from], P.ap1).stack;
    return ApOf(g_mi.sites[P.site], g == 2 ? P.ap2 : P.ap1).stack;
}
static Vec GroupChoke(int g)
{
    const fl::TPlan &P = g_tp.plan;
    const Site &s = g_mi.sites[P.site];
    if (P.type == fl::PL_FAKE) return ApOf(s, MainApOf(s)).choke;   // from the fake: the target's main way in
    return ApOf(s, g == 2 ? P.ap2 : P.ap1).choke;
}
static std::string ApName(int site, int k)
{
    if (site < 0 || site >= (int)g_mi.sites.size() || k < 0) return "-";
    return ApOf(g_mi.sites[site], k).name;
}
static void PlanPick(int forceSite, int forceType)
{
    auto bots = Team(2, true);
    std::vector<fl::PlanSite> ps = PlanSites();
    if (g_j.planForce >= 0 && g_j.planForce < (int)ps.size() && forceSite < 0) forceSite = g_j.planForce;
    g_tp.plan = fl::PickPlan(ps, std::max(1, (int)Team(2, false).size()), MeanSmart(bots), g_tpHist, g_j.planMid != 0, Rnd(), forceSite, forceType);
    g_tp.text = fl::PlanText(g_tp.plan, ps);
}
// freeze start: the plan and who calls it
static void TPlanFreeze()
{
    g_tp = TPS();
    if (!TPlanOn()) return;
    auto bots = Team(2, true);
    if (bots.empty()) return;
    PlanPick(-1, -1);
    Player *c = nullptr;
    for (auto *p : bots)
        if (!c || p->s > c->s + 0.01f || (fabsf(p->s - c->s) <= 0.01f && Rnd() < 0.5f)) c = p;
    g_tp.caller = c->name;
    g_tp.callAt = CurTime() + 1.5f;
    g_tp.active = true;
}
static void LogPlan(const char *what, const char *who)
{
    const fl::TPlan &P = g_tp.plan;
    static const char *type[] = {"push", "split", "fake"};
    BLog("PLAN map=%s r=%d what=%s plan=\"%s\" type=%s site=%s from=%s ap=%s%s%s mid=%.2f by=%s chat=%s", g_mapName.c_str(), g_roundNo + (g_phase == PH_LIVE ? 0 : 1),
         what, g_tp.text.c_str(), type[P.type % 3], g_mi.sites[P.site].label.c_str(), P.from >= 0 ? g_mi.sites[P.from].label.c_str() : "-",
         ApName(P.type == fl::PL_FAKE ? P.from : P.site, P.ap1).c_str(), P.ap2 >= 0 ? "+" : "", P.ap2 >= 0 ? ApName(P.site, P.ap2).c_str() : "", P.midShare,
         who, g_tp.chat.c_str());
}
static void AckSay(const std::string &text)
{
    Player *a = FindPl(g_tp.caller);
    if (!a || !a->bot || !a->alive || a->team != 2)
        for (auto *p : Team(2, true)) { a = p; break; }
    if (!a) return;
    g_tp.ackBy = a->name;
    g_tp.ackText = text;
    g_tp.ackAt = CurTime() + 0.6f + 0.8f * Rnd();
}
static void AssignT12(bool again);
// a human T typed a site in team chat
static void HumanCall(const std::string &name, const std::string &text)
{
    if (!TPlanOn() || !g_j.planHuman || !g_tp.active) return;
    float now = CurTime();
    bool freeze = g_phase == PH_FREEZE, grace = g_phase == PH_LIVE && now - g_liveAt < 4.f && !g_plan.executed && !g_plan.entering;
    std::vector<std::string> labels;
    for (auto &s : g_mi.sites) labels.push_back(s.label);
    int ty = -1, site = fl::ParseCall(text, labels, &ty);
    if (site < 0) return;
    if (!freeze && !grace)
    {
        BLog("PLAN map=%s r=%d what=late human=%s typed \"%s\" after the freeze - the plan stays \"%s\"", g_mapName.c_str(), g_roundNo, name.c_str(), text.c_str(),
             g_tp.text.c_str());
        return;
    }
    std::string was = g_tp.text;
    PlanPick(site, ty == fl::PL_SPLIT || ty == fl::PL_PUSH ? ty : -1);
    g_tp.source = "human " + name;
    g_tp.overrules++;
    static const char *ack[] = {"ok %s", "%s it is", "rgr, %s", "going %s"};
    char b[64];
    snprintf(b, sizeof(b), ack[rand() % 4], g_tp.text.c_str());
    AckSay(b);
    LogPlan("overrule", name.c_str());
    BLog("plan: %s overrules \"%s\" with \"%s\" (typed \"%s\")", name.c_str(), was.c_str(), g_tp.text.c_str(), text.c_str());
    if (grace && g_tp.assigned) AssignT12(true);
}
// the game's chat lines (family_party.cpp: every say / say_team line of the server log)
void BrainChatLine(const char *msg)
{
    const char *st = strstr(msg, "\" say_team \"");
    bool team = st != nullptr;
    if (!st) st = strstr(msg, "\" say \"");
    if (!st || msg[0] != '"') return;
    const char *tx = st + (team ? 12 : 7);
    const char *end = strrchr(tx, '"');
    std::string text = end && end >= tx ? std::string(tx, end - tx) : std::string(tx);
    // "Name<uid><STEAM_x or BOT><TEAM>": the last three <...> groups before the quote
    std::string actor(msg + 1, st - msg - 1);
    std::string fld[3];
    size_t e = actor.size();
    for (int i = 2; i >= 0; i--)
    {
        if (e == 0 || actor[e - 1] != '>') return;
        size_t b = actor.rfind('<', e - 1);
        if (b == std::string::npos) return;
        fld[i] = actor.substr(b + 1, e - b - 2);
        e = b;
    }
    std::string name = actor.substr(0, e);
    if (fld[1] == "BOT")
    {
        for (auto it = g_chat.pend.begin(); it != g_chat.pend.end(); ++it)
            if (it->bot == name && it->text == text)
            {
                g_chat.ok++;
                BLog("CHAT delivered: %s said \"%s\" in %s chat (server log line, %d of %d so far)", name.c_str(), text.c_str(), team ? "team" : "all",
                     g_chat.ok, g_chat.sent);
                g_chat.pend.erase(it);
                break;
            }
        return;
    }
    // a bot's line under someone else's name: the lever spoke as the wrong client - off for the level, and the line is
    // not read as that human's call
    for (auto it = g_chat.pend.begin(); it != g_chat.pend.end(); ++it)
        if (it->text == text)
        {
            BLog("CHAT misfire: %s's line \"%s\" came out as %s", it->bot.c_str(), text.c_str(), name.c_str());
            g_chat.pend.erase(it);
            if (g_chat.state == 1) ChatOff("a bot's line came out under another player's name (wrong command client)");
            return;
        }
    if (team && fld[2] == "TERRORIST") HumanCall(name, text);
}
void BrainSetServerSay(void (*fn)(const char *)) { g_serverSay = fn; }
// the round goes live: the plan's tasks
static void AssignT12(bool again)
{
    auto bots = Team(2, true);
    int n = (int)bots.size();
    if (!n) return;
    float st = MeanSmart(bots);
    const fl::TPlan &P = g_tp.plan;
    const Site &target = g_mi.sites[P.site];
    g_plan.tSite = target.label;
    g_plan.fake = P.type == fl::PL_FAKE;
    g_plan.fakeSite = g_plan.fake ? g_mi.sites[P.from].label : "";
    g_plan.gather = st >= 0.2f;   // a sloppy team trickles in one by one (step 9)
    g_plan.entering = false;
    g_plan.firstAtStack = 0;
    g_plan.gatherUntil = 0;
    g_plan.lurker.clear();
    Player *hc = g_j.follow ? HumanCarrier() : nullptr;
    std::vector<Player *> pool;
    std::set<Player *> esc;
    if (hc && n >= 2)
    {
        std::vector<Player *> byD;
        for (auto *p : bots)
            if (p != hc) byD.push_back(p);
        std::sort(byD.begin(), byD.end(), [&](Player *a, Player *b) { return Dist(a->pos, hc->pos) < Dist(b->pos, hc->pos); });
        int want = std::min(n >= 4 ? 2 : 1, (int)byD.size() - 1);
        for (int i = 0; i < want; i++) esc.insert(byD[i]);
    }
    for (auto *p : bots)
        if (!esc.count(p) && p != hc) pool.push_back(p);
    if (hc && hc->bot) Release(hc);   // follow_test: the stand-in carrier is left to the stock AI
    // one lurker (a push, a smart team, 3+ Ts, two sites): the smartest non-bomber at another site's choke (step 9)
    if (P.type == fl::PL_PUSH && g_mi.sites.size() >= 2 && n >= 3 && Rnd() < 0.1f + 0.6f * st)
    {
        Player *best = nullptr;
        for (auto *p : pool)
            if (p->name != g_bomber && p->s >= 0.25f && (!best || p->s > best->s)) best = p;
        if (best)
        {
            g_plan.lurker = best->name;
            best->role = "lurk";
            best->t12.group = 0;
            Assign(best, T_HOLD, VaryLurk(target.lurk), 2, 150, "T lurk", 60);   // JOB 5b P: the lurk spot varies
            pool.erase(std::find(pool.begin(), pool.end(), best));
        }
    }
    // groups: a split sends the smaller part the second way; the bot bomber is always in the main group
    std::vector<Player *> order;
    for (auto *p : pool)
        if (p->name == g_bomber) order.push_back(p);
    std::vector<Player *> others;
    for (auto *p : pool)
        if (p->name != g_bomber) others.push_back(p);
    for (size_t i = others.size(); i > 1; i--) std::swap(others[i - 1], others[rand() % i]);   // shuffle
    for (auto *p : others) order.push_back(p);
    int m = (int)order.size(), g2 = P.type == fl::PL_SPLIT && m >= 2 && P.ap2 >= 0 ? m - fl::SplitMain(m) : 0;
    for (int i = 0; i < m; i++)
    {
        Player *p = order[i];
        int grp = i >= m - g2 ? 2 : 1;
        p->t12.group = grp;
        p->t12.ap = grp == 2 ? P.ap2 : P.ap1;
        const Site &gs = g_plan.fake ? g_mi.sites[P.from] : target;
        const Approach &A = ApOf(gs, p->t12.ap);
        bool bomber = p->name == g_bomber;
        if (!bomber && !TakesBasic(p))
        {
            // did not take the call: it still goes to the called site the called way, alone (a trickle; no "free" T)
            const Approach &T = ApOf(target, g_plan.fake ? MainApOf(target) : p->t12.ap);
            p->role = "trickle";
            Assign(p, T_MOVE, target.c, 2, target.radius * 0.6f, "T go site alone");
            std::vector<Vec> w = T.wp;
            w.push_back(T.stack);
            w.push_back(T.choke);
            SetVia(p, w, target.c);
            continue;
        }
        p->role = grp == 2 ? "group2" : "group";
        bool past = Dist(p->pos, target.c) < Dist(A.stack, target.c) - 100.f && !g_plan.fake;
        if (g_plan.gather && !past)
        {
            Assign(p, T_HOLD, A.stack, 2, 250, g_plan.fake ? "T gather (fake)" : grp == 2 ? "T gather (split)" : "T gather", 25);
            SetVia(p, A.wp, A.stack);
        }
        else if (g_plan.gather)
            Assign(p, T_HOLD, GroupChoke(grp), 1, 200, "T to the entrance", 10);
        else
        {
            Assign(p, T_MOVE, target.c, 2, target.radius * 0.6f, "T go site");
            std::vector<Vec> w = A.wp;
            w.push_back(A.stack);
            w.push_back(A.choke);
            SetVia(p, w, target.c);
        }
    }
    for (auto *p : esc)
    {
        p->role = "escort";
        p->t12.group = 1;
        p->t12.ap = P.ap1;
        Assign(p, T_MOVE, hc->pos, 1, 220, "T escort bomb");
    }
    if (!esc.empty() && g_tp.escortsLogged++ < 3)
    {
        std::string who;
        for (auto *p : esc) who += (who.empty() ? "" : " and ") + p->name;
        BLog("plan: %s escort%s the bomb carrier %s", who.c_str(), esc.size() > 1 ? "" : "s", hc->name.c_str());
    }
    g_tp.assigned = true;
    (void)again;
}
static bool PlanT12()
{
    if (!TPlanOn()) return false;
    if (!g_tp.active)
    {
        // the freeze was missed (plugin loaded mid-round, a restart): pick now, no call
        auto bots = Team(2, true);
        if (bots.empty()) return false;
        PlanPick(-1, -1);
        g_tp.active = true;
        g_tp.called = true;
        g_tp.chat = "late";
        g_tp.caller = bots[0]->name;
    }
    AssignT12(false);
    return true;
}
static void TExecute12(const char *why)
{
    const Site &s = g_mi.sites[g_tp.plan.site];
    g_plan.entering = true;
    g_plan.enterAt = CurTime();
    g_plan.firstAtDoor = 0;
    int k = 0;
    for (auto *p : Team(2, true))
        if (p->role == "group" || p->role == "group2")
        {
            Assign(p, T_HOLD, GroupChoke(p->t12.group), 1, 200, "T to the entrance", 10);
            k++;
        }
    BLog("plan: T group to %s's entrance with %d (\"%s\", %s)", s.label.c_str(), k, g_tp.text.c_str(), why);
    UtilExecute();   // JOB 5b O: smokes and a molotov from the entrance
}
static void TGoIn12(const char *why, int there, int group)
{
    const Site &s = g_mi.sites[g_tp.plan.site];
    g_plan.executed = true;
    for (auto *p : Team(2, true))
        if (p->role == "group" || p->role == "group2" || p->role == "escort")
            Assign(p, T_MOVE, s.c, 1, s.radius * 0.6f, "T hit", 40, p->name == g_bomber ? 0.8f : 0.f);   // I: the bomb goes in second
    BLog("plan: T hit %s with %d, %d at the entrance (\"%s\", %s)", s.label.c_str(), group, there, g_tp.text.c_str(), why);
    ClearStart(s);   // STEP 12F5
}
// H: the human carrier's heading
static void FollowTick(float now)
{
    Player *c = HumanCarrier();
    if (!c || !g_j.follow || g_plan.executed || g_plan.entering || g_routes.empty()) return;
    std::vector<Vec> sc;
    std::vector<float> sr;
    for (auto &s : g_mi.sites)
    {
        sc.push_back(s.c);
        sr.push_back(s.radius);
    }
    int rk = -1, h = fl::CarrierHeading(g_routes, sc, sr, c->pos, &rk);
    if (h != g_tp.heading)
    {
        g_tp.heading = h;
        g_tp.headingAt = now;
        g_tp.headingRoute = rk;
    }
    if (h < 0 || h == g_tp.plan.site || now - g_tp.headingAt < 1.5f || g_tp.follows >= 2) return;
    std::string was = g_tp.text;
    fl::TPlan np;
    np.type = fl::PL_PUSH;
    np.site = h;
    np.ap1 = rk >= 0 && rk < (int)g_routeAp.size() ? g_routeAp[rk] : MainApOf(g_mi.sites[h]);
    np.midShare = ApOf(g_mi.sites[h], np.ap1).flags & 1 ? 1.f : 0.f;
    g_tp.plan = np;
    g_tp.text = fl::PlanText(np, PlanSites());
    g_tp.source = "carrier " + c->name;
    g_tp.follows++;
    AssignT12(true);
    AckSay("following bomb " + g_mi.sites[h].label);
    LogPlan("follow", c->name.c_str());
    BLog("plan: the bomb carrier %s heads %s (was \"%s\") - the team follows \"%s\"", c->name.c_str(), g_mi.sites[h].label.c_str(), was.c_str(),
         g_tp.text.c_str());
}
static void EscortTick()
{
    Player *hc = HumanCarrier();
    for (auto *p : Team(2, true))
    {
        if (p->role != "escort") continue;
        if (!hc)
        {
            // the carrier is gone (dropped the bomb, died): back to the main group
            p->role = "group";
            if (g_plan.executed) Assign(p, T_MOVE, g_mi.sites[g_tp.plan.site].c, 1, 300, "T hit");
            else Assign(p, T_HOLD, g_plan.entering ? GroupChoke(1) : GroupStack(1), 1, 250, "T gather", 25);
            continue;
        }
        if (g_plan.executed) continue;
        if (Dist(p->pos, hc->pos) > 350.f && (p->task.kind == T_NONE || Dist(p->task.goal, hc->pos) > 200.f))
            Assign(p, T_MOVE, hc->pos, 1, 220, "T escort bomb");
    }
}
static void TickT12()
{
    if (!T12() || g_mi.hostage || g_plan.planted || g_phase != PH_LIVE) return;
    float now = CurTime();
    FollowTick(now);
    EscortTick();
    const Site &target = g_mi.sites[g_tp.plan.site];
    float st = MeanSmart(Team(2, true));
    Player *hc = g_j.follow ? HumanCarrier() : nullptr;
    bool carrierThere = hc && (Dist(hc->pos, GroupChoke(1)) < 700.f || Dist(hc->pos, GroupStack(1)) < 500.f || Dist(hc->pos, target.c) < target.radius + 200.f);
    bool waitCarrier = hc && !carrierThere && now - g_liveAt < 60.f;   // H: the team waits for the human with the bomb
    if (g_plan.gather && !g_plan.executed && !g_plan.entering)
    {
        int group = 0, there = 0;
        for (auto *p : Team(2, true))
            if (p->role == "group" || p->role == "group2")
            {
                group++;
                if (Dist(p->pos, GroupStack(p->t12.group)) < 450.f) there++;
            }
        if (there > 0 && g_plan.firstAtStack == 0) g_plan.firstAtStack = now;
        float wait = Lerp(4.f, 16.f, st);
        bool go = group == 0 || there == group || (g_plan.firstAtStack > 0 && now - g_plan.firstAtStack > wait) || now - g_liveAt > 50.f;
        if (waitCarrier && now - g_liveAt <= 50.f) go = false;
        if (carrierThere && there > 0) go = true;
        if (go)
        {
            if (g_plan.fake && g_plan.gatherUntil == 0)
            {
                g_plan.gatherUntil = now + 3.f;   // show up at the fake for a moment, then go
                BLog("plan: fake at %s, then %s (\"%s\")", g_plan.fakeSite.c_str(), target.label.c_str(), g_tp.text.c_str());
            }
            if (!g_plan.fake || now >= g_plan.gatherUntil)
                TExecute12(there == group ? "group together" : carrierThere ? "with the bomb" : "wait over");
        }
    }
    if (g_plan.entering && !g_plan.executed)
    {
        int group = 0, there = 0;
        for (auto *p : Team(2, true))
            if (p->role == "group" || p->role == "group2")
            {
                group++;
                if (Dist(p->pos, GroupChoke(p->t12.group)) < 400.f || Dist(p->pos, target.c) < target.radius + 100.f) there++;
            }
        if (there > 0 && g_plan.firstAtDoor == 0) g_plan.firstAtDoor = now;
        float wait = Lerp(1.5f, 4.f, st);
        // JOB 5b O: the execute's smokes land first, and the flashes pop before the group steps in
        bool smokes = UtilHoldGo(now) && now - g_liveAt < 75.f;
        auto go = [&](const char *why) {
            if (now - g_liveAt < 75.f && UtilFlashFirst(now)) return;
            TGoIn12(why, there, group);
        };
        if (hc && Dist(hc->pos, target.c) < target.radius + 100.f) TGoIn12("the bomb is going in", there, group);
        else if (smokes) { /* waiting for the smokes (up to 7 s after the group reached the entrance) */ }
        else if (group == 0 || (there >= group && !waitCarrier)) go("all at the entrance");
        else if (g_plan.firstAtDoor > 0 && now - g_plan.firstAtDoor > wait && !waitCarrier) go("wait over");
        else if ((now - g_plan.enterAt > 10.f && !waitCarrier) || now - g_liveAt > 75.f) go("clock");
    }
    // I: a bot bomber keeps to the called plan (the stock bomber re-targets its own MoveTo to the site it likes)
    auto b = g_pl.find(g_bomber);
    if (b != g_pl.end() && b->second.bot && !g_j.followTest && b->second.ent && b->second.alive && b->second.task.kind != T_NONE && !g_plan.executed &&
        !J5Busy(b->second) && StateName(b->second.ent) == "11MoveToState" && Dist(b->second.issuedGoal, *(Vec *)(b->second.ent + kOffMoveTo + 4)) > 200.f &&
        now - b->second.issuedAt > 1.0f)
    {
        BotMoveTo(b->second.ent, b->second.issuedGoal, 2);
        b->second.issuedAt = now;
        g_tp.bomberKept++;
    }
}
// J: how many Ts went through mid (first 45 s of the round), 4 times a second, steering on or off
static void MidTick(float now)
{
    if (g_phase != PH_LIVE || !g_mi.ok || g_mi.hostage || now - g_liveAt > 45.f || now < g_tp.nextMid) return;
    if (g_tp.nextMid > now + 5.f) g_tp.nextMid = 0;
    g_tp.nextMid = now + 0.25f;
    for (auto *p : Team(2, false))
    {
        bool in = false;
        for (auto &m : g_mi.midAreas)
            if (p->pos.x >= m.x0 - 16.f && p->pos.x <= m.x1 + 16.f && p->pos.y >= m.y0 - 16.f && p->pos.y <= m.y1 + 16.f && fabsf(p->pos.z - m.z) < 150.f)
            {
                g_tp.midPlaces[m.place].insert(p->name);
                in = true;
                break;
            }
        if (!in && g_mi.midAreas.empty() && g_mi.hasMid && Dist(p->pos, g_mi.mid) < 500.f)
        {
            g_tp.midPlaces["mid"].insert(p->name);
            in = true;
        }
        if (in) g_tp.midTs.insert(p->name);
    }
}
static void PlanFrame(float now)
{
    ChatFrame(now);
    MidTick(now);
    if (!g_tp.active) return;
    if (!g_tp.called && now >= g_tp.callAt && (g_phase == PH_FREEZE || (g_phase == PH_LIVE && now - g_liveAt < 3.f)))
    {
        g_tp.called = true;
        Player *c = FindPl(g_tp.caller);
        if (!c || !c->bot || c->team != 2)
            for (auto *p : Team(2, true)) { c = p; break; }
        if (c) g_tp.caller = c->name;
        g_tp.chat = TeamSay(c, g_tp.text);
        LogPlan("call", g_tp.caller.c_str());
        BLog("plan: %s calls \"%s\" (%s)", g_tp.caller.c_str(), g_tp.text.c_str(), g_tp.chat.c_str());
    }
    if (g_j.overruleTest > 0 && g_phase == PH_FREEZE && g_tp.called && !g_tp.overrules && now >= g_tp.callAt + 1.5f &&
        g_j.overruleTest <= (int)g_mi.sites.size())
        HumanCall("testhuman", g_mi.sites[g_j.overruleTest - 1].label);   // plan_test_overrule: as if a human typed it
    if (g_tp.ackAt > 0 && now >= g_tp.ackAt)
    {
        g_tp.ackAt = -1;
        Player *a = FindPl(g_tp.ackBy);
        std::string how = TeamSay(a, g_tp.ackText);
        BLog("PLAN map=%s r=%d what=ack by=%s text=\"%s\" chat=%s", g_mapName.c_str(), g_roundNo + (g_phase == PH_LIVE ? 0 : 1), g_tp.ackBy.c_str(),
             g_tp.ackText.c_str(), how.c_str());
    }
}
static void J5RoundLive()
{
    // I: who carries the bomb this round (Valve's handout at the round start; bot_defer_to_human_items decides)
    if (!g_mi.ok || g_mi.hostage || !g_matchLive) return;
    int ts = 0, humans = 0;
    for (auto *p : Team(2, false))
    {
        ts++;
        if (!p->bot) humans++;
    }
    auto b = g_pl.find(g_bomber);
    BLog("C4 map=%s r=%d carrier=%s bot=%d ts=%d humansT=%d fair=%d", g_mapName.c_str(), g_roundNo, g_bomber.empty() ? "-" : g_bomber.c_str(),
         b != g_pl.end() ? (int)b->second.bot : -1, ts, humans, (int)TestHook("brain_c4fair", 1));
}
static void PlanRoundEnd()
{
    if (!g_mi.ok || g_mi.hostage) return;
    std::string places;
    for (auto &kv : g_tp.midPlaces) places += (places.empty() ? "" : ",") + kv.first + ":" + std::to_string(kv.second.size());
    int tsN = (int)Team(2, false, false).size();
    BLog("TPR map=%s r=%d on=%d plan=\"%s\" src=%s caller=%s chat=%s overrules=%d follows=%d midShare=%.2f midTs=%d/%d midPlaces=%s hit=%s plant=%s win=%s bomberKept=%d",
         g_mapName.c_str(), g_roundNo, (int)g_tp.active, g_tp.active ? g_tp.text.c_str() : "-", g_tp.source.c_str(), g_tp.caller.empty() ? "-" : g_tp.caller.c_str(),
         g_tp.chat.c_str(), g_tp.overrules, g_tp.follows, g_tp.active ? g_tp.plan.midShare : -1.f, (int)g_tp.midTs.size(), tsN,
         places.empty() ? "-" : places.c_str(), g_rm.hit.empty() ? "-" : g_rm.hit.c_str(), g_rm.plant ? g_rm.plantSite.c_str() : "-",
         g_rm.winner.empty() ? "-" : g_rm.winner.c_str(), g_tp.bomberKept);
    if (g_tp.active)
    {
        g_tpHist.push_back(g_tp.plan);
        if (g_tpHist.size() > 8) g_tpHist.erase(g_tpHist.begin());
        g_tpCount[g_tp.text]++;
        if (g_roundNo % 10 == 0)
        {
            std::string c;
            for (auto &kv : g_tpCount) c += (c.empty() ? "" : ", ") + kv.first + " " + std::to_string(kv.second);
            BLog("plan counts on %s after %d rounds: %s", g_mapName.c_str(), g_roundNo, c.c_str());
        }
    }
}
// ---- STEP 12E: player habits - pre-aim, jiggle / shoulder peeks, counter-strafe -------------------------------------
// His words: "they don't really try to jiggle peek, or do any player behaviors". Part A made them shoot; this makes them
// move and look like players around corners. By the bot's angles dial (profile-style; the bottom rarely does any of it -
// "walks into sight"), fl::PeekFor:
//   pre-aim   while moving on ground the enemy can already reach (nav earliest-occupy grid vs the round clock), every
//             0.6 .. 0.3 s it may (20% .. 95%) put its crosshair on the next angle ahead: the nearest nav hiding spot
//             within 75 deg of where it goes, 250-1300 u away, in its sight and not looked at in the last 4 s
//             (fl::PickPreaim; <= 6 world traces) - the stock bot glances at encounter spots by its Skill, which step 11
//             keeps low for the spray, so it rarely did.
//   jiggle    instead of walking into a suspected corner it steps out sideways until the corner is in sight (the
//             smallest of 64 / 96 / 128 u that sees it, fl::PickPeek - both world traces), stops there 0.35 .. 0.12 s
//             crosshair on the corner, steps back, waits 0.8 .. 0.4 s, 2-3 times; a suspected AWP (his weapon from
//             item_equip) gets a 0.1 s shoulder peek to bait the shot. Who: a T waiting at the site entrance before the hit
//             (the suspected corner = the site's CT holds: the common angles), and any holding bot that heard or saw an
//             enemy it cannot see now (half as often for a CT: they hold). If it sees him on a peek, the fight is on
//             (the stock aim; part G if he shoots it).
//   counter-strafe  the first time it sees an enemy while moving, a footwork bot (step 10's dial: 0 at elo 300) stops
//             dead for 0.30 .. 0.15 s before it shoots - step 10's stop-to-shoot only started once the fight had begun.
// Levers: step 9's MoveTo for the steps (BotMoveTo; Steer holds off while it jiggles), step 10's speed scale for the
// stops, the look controller for the crosshair. Proof: "PEEK" per jiggle (why, peeks, time exposed, contact), "PEEKR"
// per bot per round (pre-aims, jiggles, peeks, contacts, counter-strafes; on and off).
// Switches: brain_peek 0, peek_preaim 0, peek_jiggle 0, peek_cs 0; A/B peek_team 2|3; test peek_dial.
static int g_peekLogged;
static bool PeekOn(const Player &p) { return g_j.peek && (!g_j.peekTeam || g_j.peekTeam == p.team) && g_modeOk && g_layout == 1; }
static float PeekDial(const Player &p) { return g_j.peekDial >= 0 ? std::min(1.f, g_j.peekDial) : DialOf(p, D_ANGLES, false); }
static bool J5CounterStrafe(const Player &p, float now) { return g_j.peekCs && now < p.pk.csUntil && PeekOn(p); }
static void PeekSeen(Player &p, float now)
{
    if (!PeekOn(p) || !g_j.peekCs || !p.bot || now < p.pk.csUntil || p.hsp < 100.f || BotAttacking(p.ent)) return;
    if (Rnd() >= p.fw) return;   // step 10's footwork dial (0 at elo 300: a beginner runs and guns)
    p.pk.csUntil = now + Lerp(0.30f, 0.15f, p.fw);
    p.pk.cs++;
}
static bool StartJiggle(Player &p, const Vec &tgt, bool awp, const char *why, float now)
{
    fl::PeekParams pp = fl::PeekFor(PeekDial(p));
    Vec eye = EyeOf(p.ent, p.fpos);
    if (Los(eye, tgt)) return false;   // it already sees the corner
    float side = (YawTo(p.fpos, tgt) + 90.f) / 57.29578f;
    std::vector<float> off = {64.f, -64.f, 96.f, -96.f, 128.f, -128.f};
    std::vector<char> vis;
    std::vector<Vec> qs;
    for (float o : off)
    {
        Vec q{p.fpos.x + o * cosf(side), p.fpos.y + o * sinf(side), p.fpos.z};
        bool free = Los(eye, {q.x, q.y, eye.z}) && Los({p.fpos.x, p.fpos.y, p.fpos.z + 20.f}, {q.x, q.y, q.z + 20.f});
        vis.push_back(free && Los({q.x, q.y, eye.z}, tgt) ? 1 : 0);
        qs.push_back(q);
    }
    int k = fl::PickPeek(off, vis);
    if (k < 0) return false;
    Player::Pk &j = p.pk;
    j.phase = 1;
    j.P = p.fpos;
    j.Q = qs[k];
    j.tgt = tgt;
    j.left = pp.peeks;
    j.peeksDone = 0;
    j.out = awp ? std::min(pp.out, 0.10f) : pp.out;
    j.back = pp.back;
    j.reissue = 0;
    j.startAt = j.phaseAt = now;
    j.why = why;
    j.task = p.task.why;
    j.contact = false;
    j.jiggles++;
    j.cool = now + 6.f;
    LookWant(p, LK_PEEK, tgt, 900.f, 1.0f, "peek");
    return true;
}
static void EndJiggle(Player &p, float now, const char *how)
{
    Player::Pk &j = p.pk;
    j.phase = 0;
    if (g_peekLogged++ < g_j.peekLog)
        BLog("PEEK map=%s r=%d bot=%s team=%s elo=%.0f dial=%.2f why=%s peeks=%d out=%.2f contact=%d end=%s dur=%.1f | %s jiggles %s: %d peek(s), %.2f s out%s",
             g_mapName.c_str(), g_roundNo, p.name.c_str(), p.team == 2 ? "T" : "CT", p.elo, PeekDial(p), j.why.c_str(), j.peeksDone, j.out, (int)j.contact, how,
             now - j.startAt, p.name.c_str(), j.why.c_str(), j.peeksDone, j.out, j.contact ? " - saw him" : "");
}
static bool EnemyAwp(const Player &p, const Vec &near)
{
    for (auto &kv : g_pl)
        if (kv.second.team != p.team && (kv.second.team == 2 || kv.second.team == 3) && kv.second.alive && Dist(kv.second.pos, near) < 800.f &&
            strstr(kv.second.weapon.c_str(), "awp"))
            return true;
    return false;
}
static void PeekFrame(Player &p, float now)
{
    Player::Pk &j = p.pk;
    if (!p.falive || !p.ent)
    {
        if (j.phase) EndJiggle(p, now, "dead");
        return;
    }
    // where it goes (over the last ~0.25 s)
    if (now - j.lastPosT >= 0.25f || now < j.lastPosT)
    {
        float d = fl::Dist2(p.fpos, j.lastPos);
        j.moving = d > 15.f && now > j.lastPosT && now - j.lastPosT < 1.f;
        if (j.moving) j.moveYaw = YawTo(j.lastPos, p.fpos);
        j.lastPos = p.fpos;
        j.lastPosT = now;
    }
    bool attack = BotAttacking(p.ent);
    if (j.phase)
    {
        if (!PeekOn(p) || attack || p.sh.panic || p.fr.phase || p.task.why != j.task)
            return EndJiggle(p, now, attack ? "fight" : "interrupted");
        if (!J5MayMove(p)) return EndJiggle(p, now, "objective");
        if (now - p.sh.lastSeen < 0.15f)
        {
            j.contact = true;
            p.pk.contacts++;
            return EndJiggle(p, now, "contact");
        }
        if (j.phase == 1 || j.phase == 3)
        {
            const Vec &to = j.phase == 1 ? j.Q : j.P;
            if (now >= j.reissue)
            {
                BotMoveTo(p.ent, to, 1);
                j.reissue = now + 0.35f;
            }
            if (fl::Dist2(p.fpos, to) < 28.f || now - j.phaseAt > 0.7f)
            {
                if (j.phase == 1)
                {
                    j.peeksDone++;
                    p.pk.peeks++;
                }
                j.phase++;
                j.phaseAt = now;
                j.until = now + (j.phase == 2 ? j.out : j.back);
                LookWant(p, LK_PEEK, j.tgt, 900.f, 1.0f, "peek");
            }
        }
        else if (now >= j.until)
        {
            if (j.phase == 4 && --j.left <= 0) return EndJiggle(p, now, "done");
            j.phase = j.phase == 2 ? 3 : 1;
            j.phaseAt = now;
            j.reissue = 0;
        }
        return;
    }
    if (!PeekOn(p) || attack || p.sh.panic || J5Busy(p) || now - p.sh.lastSeen < 0.5f || !J5MayMove(p)) return;
    fl::PeekParams pp = fl::PeekFor(PeekDial(p));
    std::string st = StateName(p.ent);
    // jiggle a suspected corner: a T at the site entrance before the hit (the site's CT holds = the common angles) ...
    if (g_j.peekJig && now >= j.cool && p.team == 2 && !j.entranceDone && p.task.why == "T to the entrance" && Dist(p.fpos, p.task.goal) < 230.f)
    {
        j.entranceDone = true;
        const Site *s = FindSite(g_plan.tSite);
        if (s && Rnd() < pp.jiggle)
        {
            std::vector<Vec> h = s->hold;
            std::sort(h.begin(), h.end(), [&](const Vec &a, const Vec &b) { return Dist(a, p.fpos) < Dist(b, p.fpos); });
            for (auto &c : h)
                if (Dist(c, p.fpos) < 1600.f && StartJiggle(p, {c.x, c.y, c.z + 56.f}, EnemyAwp(p, c), "entrance", now)) return;
        }
    }
    // ... or a holding bot that heard / saw an enemy it cannot see now
    if (g_j.peekJig && now >= j.cool && st == "9HideState")
    {
        const Intel *best = nullptr;
        float bd = 1500.f;
        for (auto &kv : g_intel[p.team])
        {
            Player *e = FindPl(kv.first);
            if (!e || !e->alive || now - kv.second.t > 5.f || now < kv.second.t) continue;
            float d = Dist(p.fpos, kv.second.pos);
            if (d < bd && d > 250.f) { bd = d; best = &kv.second; }
        }
        if (best)
        {
            j.cool = now + 3.f;   // one roll per 3 s
            if (Rnd() < pp.jiggle * (p.team == 3 ? 0.5f : 1.f) &&
                StartJiggle(p, {best->pos.x, best->pos.y, best->pos.z + 60.f}, EnemyAwp(p, best->pos), best->how == 2 ? "seen" : "heard", now))
                return;
        }
    }
    // pre-aim the next angle while moving on ground the enemy can already reach
    if (!g_j.peekPre || now < j.nextLook || !j.moving || p.hsp < 60.f) return;
    if (st != "11MoveToState" && st != "9HuntState" && st != "11FollowState") return;
    j.nextLook = now + Lerp(0.6f, 0.3f, PeekDial(p));
    if (p.lk.pri > LK_PREAIM && now < p.lk.until) return;
    int enemy = p.team == 2 ? 3 : 2;
    float t = now - g_liveAt;
    if (OccAt(enemy, p.fpos) > (int)((t + 3.f) * 10) || Rnd() >= pp.preaim) return;
    std::vector<std::pair<float, int>> near;
    for (size_t i = 0; i < g_mi.spots.size(); i++)
    {
        const Vec &v = g_mi.spots[i].first;
        float d = fl::Dist2(v, p.fpos);
        if (d < 250.f || d > 1300.f || fabsf(v.z - p.fpos.z) > 250.f || fabsf(Wrap180(YawTo(p.fpos, v) - j.moveYaw)) > 75.f) continue;
        auto c = j.checked.find((int)i);
        if (c != j.checked.end() && now - c->second < 4.f && now >= c->second) continue;
        if (OccAt(enemy, v) > (int)((t + 3.f) * 10)) continue;
        near.push_back({d, (int)i});
    }
    std::sort(near.begin(), near.end());
    if (near.size() > 6) near.resize(6);
    std::vector<Vec> sp;
    std::vector<char> vis, reach, fresh;
    Vec eye = EyeOf(p.ent, p.fpos);
    for (auto &n : near)
    {
        const Vec &v = g_mi.spots[n.second].first;
        sp.push_back(v);
        vis.push_back(Los(eye, {v.x, v.y, v.z + 56.f}) ? 1 : 0);
        reach.push_back(1);
        fresh.push_back(1);
    }
    int k = fl::PickPreaim(p.fpos, j.moveYaw, sp, vis, reach, fresh);
    if (k < 0) return;
    const Vec &v = sp[k];
    if (LookWant(p, LK_PREAIM, {v.x, v.y, v.z + 56.f}, pp.turn, Lerp(0.5f, 1.0f, PeekDial(p)), "pre-aim"))
    {
        j.checked[near[k].second] = now;
        j.preaims++;
    }
}
// ---- STEP 12K: aim at Gold ------------------------------------------------------------------------------------------
// His words (09-26): Gold Nova I "still not quite feeling like Gold", "not aiming right"; Gold should win real duels a
// fair share, not "walk up, headshot". fl::GoldBump over the bot's aim dial lifts only the middle ranks (0 up to elo
// ~700, full from ~1000 to ~1300, 0 again from ~1700): at the full bump ReactionTime x0.70, AimFocusInitial x0.60,
// AimFocusOffsetScale x0.70, look acceleration (attacking) x1.35 - written into the same profile fields step 11 writes,
// after step 11's values and before its lucky / panic overrides (which keep their own limits). For a typical Gold Nova I
// (aim / react dial 0.33): ReactionTime 0.205 -> 0.144 s, first-shot aim error 2.7 -> 1.6 deg, offset scale 0.17 ->
// 0.12, flick 6000 -> 8200 deg/s2. Parts B (hearing), E (pre-aim, counter-strafe) and G (reacting to fire) answer
// "walk up" too: a Gold bot now hears a runner and pre-aims the angle he comes from.
// The check against his words: walkup_test <D> - 1v1, the T bot (muted, soaks damage like step 11's circler) walks
// (walkup_walk 1) or runs into the CT bot's view and stops D units in front of it; "WALKUP" per round: how long from the
// moment the target could see him to its first shot, its first hit, and to 100 damage (a real player dead).
// Switches: brain_gold 0 (step 11's curve), A/B gold_team 2|3; calibration gold_rt / gold_fi / gold_fo / gold_laa.
static bool GoldOn(const Player &p) { return g_j.gold && (!g_j.goldTeam || g_j.goldTeam == p.team); }
static float GoldOf(const Player &p) { return GoldOn(p) ? fl::GoldBump(p.sh.aimd) : 0.f; }
static void J5Gold(Player &p, float *w)
{
    float b = GoldOf(p);
    if (b <= 0.f) return;
    fl::GoldMul m = fl::GoldFor(b, g_j.goldRt, g_j.goldFi, g_j.goldFo, g_j.goldLa);
    w[PF_REACT] *= m.rt;
    w[PF_FINIT] *= m.fi;
    w[PF_FOFF] *= m.fo;
    w[PF_LAA] *= m.laa;
}
// K (2): the hold that waits to be shot. Step 11 measured it: a bot HOLDING an angle (HideState with the hold flag - every
// step 9/12C CT hold) does not open fire on an enemy in plain view who is not shooting at it (the stock hold waits to
// be fired on); step 11 dropped the hold only inside 400 u (its panic). A player walking up to a holding bot at any
// range gets the first shot - "walk up, headshot". Now any holder (bomb maps; step 11 found dropping holds on hostage
// maps cost rescues) that has had a QUIET enemy (no shot for 1.5 s) in its view with a clear line for longer than its
// reaction time + 0.25 s without opening fire drops the hold the step-11 way (-> IdleState: the stock bot engages what it
// sees; step 9's Steer puts the hold back 2 s later). An enemy who shoots it is fought from the hold as before.
// Proof: "UNHOLD" per drop (first gold_unhold_log a round), "HOLDR" per round. Switch: gold_unhold 0.
static int g_unholdLogged;
static bool ReactOn(const Player &b);   // JOB 5c R (below)
static void ReactDrop();
static void UnholdFrame(Player &b, float now)
{
    if (!g_j.unhold || !GoldOn(b) || g_mi.hostage || !b.falive || !b.ent || b.sh.state != 1 || !ShootOn(b)) return;
    if (now - b.seenNameAt > 0.1f || now - b.sh.unholdAt < 1.5f || b.sh.panic || BotAttacking(b.ent)) return;
    Player *e = FindPl(b.seenName);
    if (!e || !e->falive) return;
    // he shoots it: the hold fights back itself. JOB 5c R: only when his shots are at THIS holder - 5b skipped every enemy
    // that had fired lately, so a team pushing and shooting was watched, never fought ("watched all five of us push")
    bool react = ReactOn(b);
    if (!fl::HoldDrop(now - e->sh.firedAt < 1.5f, now - b.fr.shotAt < 1.5f && now >= b.fr.shotAt, react)) return;
    bool byR = react && now - e->sh.firedAt < 1.5f;
    float rt = *(float *)(b.sh.prof + kPfOff[PF_REACT]);
    if (!(rt >= 0.f && rt < 5.f) || now - b.sh.sightAt < rt + 0.25f) return;
    if (StateName(b.ent) != "9HideState") return;   // JOB 5c R: at its spot only (dropping a walking holder sent it chasing down mid)
    SetBotState(b.ent, b.ent + kOffIdle);
    b.sh.unholdAt = now;
    b.unholds++;
    if (byR) ReactDrop();
    if (g_unholdLogged++ < g_j.unholdLog)
        BLog("UNHOLD map=%s r=%d bot=%s team=%s elo=%.0f enemy=%s d=%.0f waited=%.2f firing=%d | %s had %s in plain view %.2f s without shooting - drops the hold to fight",
             g_mapName.c_str(), g_roundNo, b.name.c_str(), b.team == 2 ? "T" : "CT", b.elo, e->name.c_str(), Dist(b.fpos, e->fpos), now - b.sh.sightAt, (int)byR,
             b.name.c_str(), e->name.c_str(), now - b.sh.sightAt);
}
static struct Walkup
{
    std::string run, tgt;
    float seenAt = -1, shotAt = -1, hitAt = -1, deadAt = -1, heldAt = -1, tgtYaw = 0, nextMove = 0, nextHold = 0;
    int held = 0, phase = 0, dmg = 0, dmg1 = 0, heads = 0, shots = 0, logged = 0;
    float tgtElo = 0, tgtAim = 0;
} g_wu;
static void WalkupLog()
{
    if (g_wu.logged || g_wu.tgt.empty()) return;
    g_wu.logged = 1;
    auto rel = [](float t) { return t >= 0 && g_wu.seenAt >= 0 ? t - g_wu.seenAt : -1.f; };
    Player *t = FindPl(g_wu.tgt);
    BLog("WALKUP map=%s r=%d target=%s elo=%.0f aim=%.2f gold=%.2f on=%d D=%d walk=%d seen=%d shot=%.2f hit=%.2f dead=%.2f dmg1s=%d dmg=%d heads=%d shots=%d",
         g_mapName.c_str(), g_roundNo, g_wu.tgt.c_str(), g_wu.tgtElo, g_wu.tgtAim, t ? GoldOf(*t) : -1.f, g_j.gold, g_j.walkup, g_j.walkupWalk,
         g_wu.seenAt >= 0, rel(g_wu.shotAt), rel(g_wu.hitAt), rel(g_wu.deadAt), g_wu.dmg1, g_wu.dmg, g_wu.heads, g_wu.shots);
}
static void WalkupFrame(float now)
{
    Player *run = nullptr, *tgt = nullptr;
    for (auto &kv : g_pl)
        if (kv.second.bot && kv.second.falive)
        {
            if (kv.second.team == 2 && !run) run = &kv.second;
            if (kv.second.team == 3 && !tgt) tgt = &kv.second;
        }
    if (!run || !tgt || g_layout != 1) return;
    if (g_wu.run.empty())
    {
        g_wu.run = run->name;
        g_wu.tgt = tgt->name;
        g_wu.tgtElo = tgt->elo;
        g_wu.tgtAim = tgt->sh.aimd;
    }
    if (run->name != g_wu.run || tgt->name != g_wu.tgt) return;
    *(uint8_t *)(run->ent + kOffStateTime + 4) = 0;   // the runner never fights
    if (g_np.health > 0)
    {
        int *hp = (int *)(run->ent + g_np.health);
        if (*hp > 0 && *hp <= 10000) *hp = 5000;   // it soaks the shots; the damage is counted (a 100-hp player dead at 100)
    }
    if (g_np.velmod > 0)
    {
        float *vm = (float *)(run->ent + g_np.velmod);
        if (*vm >= 0.f && *vm <= 1.5f) *vm = g_j.walkupWalk ? 0.52f : 1.f;
    }
    Vec arena = g_mi.ok && !g_mi.sites.empty() ? g_mi.sites[0].c : tgt->fpos;
    if (!g_wu.held && now - g_liveAt > 0.5f && now >= g_wu.nextHold)
    {
        g_wu.nextHold = now + 0.5f;
        if (fl::Dist2(tgt->fpos, arena) > 60.f && now - g_liveAt < 25.f)
            BotMoveTo(tgt->ent, arena, 1);
        else if (BotHideHere(tgt->ent, tgt->fpos, 300.f))
        {
            g_wu.held = 1;
            g_wu.heldAt = now;
        }
    }
    if (g_wu.seenAt < 0 && g_wu.held && now - tgt->sh.lastSeen < 0.06f) g_wu.seenAt = now;
    if (!g_wu.held || now - g_wu.heldAt < 2.f || now < g_wu.nextMove) return;
    g_wu.nextMove = now + 0.25f;
    float yaw;
    if (g_wu.phase == 0)
    {
        if (!EyeYaw(*tgt, &yaw)) return;
        g_wu.tgtYaw = yaw;
        g_wu.phase = 1;
    }
    float y = g_wu.tgtYaw / 57.29578f;
    Vec front{tgt->fpos.x + (float)g_j.walkup * cosf(y), tgt->fpos.y + (float)g_j.walkup * sinf(y), tgt->fpos.z};
    if (g_wu.phase == 1)
    {
        if (fl::Dist2(run->fpos, front) > 60.f && now - g_liveAt < 45.f)
            BotMoveTo(run->ent, front, 1);
        else if (BotHideHere(run->ent, run->fpos, 300.f))
            g_wu.phase = 2;   // it stands in front of the target (a player lining up his headshot)
    }
}
static void WalkupShot(Player &sh, float now)
{
    if (!g_j.walkup || sh.name != g_wu.tgt || g_wu.seenAt < 0) return;
    g_wu.shots++;
    if (g_wu.shotAt < 0) g_wu.shotAt = now;
}
static void WalkupHurt(Player *a, Player *v, int group, int dmg, float now)
{
    if (!g_j.walkup || !a || !v || a->name != g_wu.tgt || v->name != g_wu.run) return;
    if (g_wu.hitAt < 0) g_wu.hitAt = now;
    g_wu.dmg += dmg;
    if (g_wu.seenAt >= 0 && now - g_wu.seenAt <= 1.f) g_wu.dmg1 += dmg;
    if (group == 1) g_wu.heads++;
    if (g_wu.dmg >= 100 && g_wu.deadAt < 0) g_wu.deadAt = now;
}
// ---- STEP 12F: the rest of his 09-25 list ---------------------------------------------------------------------------
// 5 "they just run in" - clear every corner on site entry, fast sometimes, slow other times, scaled by rank
// 6 Mirage: Ts must not all run down mid - parts D / J (plan variety, the mid cap, approach waypoints)
// 7 "after he planted, 3 Ts ran back to spawn" - every T defends the bomb (OnPlant); aggressive while taking the site
// 8 retakes: hurry (clock) but together, not one by one; "the CT AWP stood on site not shooting moving Ts"
// 9 "a teammate threw a molly the dumbest way possible" - triple-check a grenade's landing before it is thrown
// Switches: brain_clear 0 (5), brain_post 0 (7), brain_retake 0 (8), brain_nade 0 (9: the veto; the NADE lines stay).
static bool ClearOn() { return g_j.clear && g_steer && g_layout == 1 && g_mi.ok && !g_mi.hostage && g_modeOk; }
static bool PostOn() { return g_j.post != 0; }
static bool RetakeOn() { return g_j.retake != 0; }
// -- F5: clearing the site ------------------------------------------------------------------------------------------
// At the hit (step 9 / part D's TGoIn) the site's corners - its CT holds (the common angles) + nav IN_COVER hiding spots
// on it, 150 u apart, at most 8, in sweep order from the site entry - are shared out over the Ts going in
// (fl::ClearShare); each T checks each of its corners with a chance by its aware dial (fl::ClearCheck: 40% .. 100%),
// its crosshair on the corner (look controller, above pre-aim) until it has seen it (in its view within 12 deg with a
// clear line) or 1.5 s pass. The team takes the site SLOW (walks in, 0.5-0.8 s a corner) or FAST (runs, 0.2-0.3 s) by a
// roll per round (fl::ClearSlow: a smart team slow ~half the time, a sloppy one mostly fast). "Aggressive while taking
// a site": fast takes run (no quiet walk), and the Ts fight what they see (the clearing gives way to any fight).
// Proof: "CLEAR" per site take: corners, checked (by the team), tempo, seconds to the plant.
static struct Clr
{
    bool on = false, slow = false, logged = false, tried = false;
    int site = -1;
    float startAt = 0, plantAfter = -1;
    std::vector<Vec> corners;
    std::vector<char> seen;
    std::map<std::string, std::vector<int>> queue;
    std::map<std::string, float> since;
} g_clr;
static void ClearLog(const char *why)
{
    if (!g_clr.on || g_clr.logged) return;
    g_clr.logged = true;
    int k = 0;
    for (char c : g_clr.seen) k += c;
    const std::string &L = g_mi.sites[g_clr.site].label;
    BLog("CLEAR map=%s r=%d site=%s tempo=%s corners=%d checked=%d bots=%d plant=%.1f end=%s | %s taken %s: %d of %d corners checked", g_mapName.c_str(),
         g_roundNo, L.c_str(), g_clr.slow ? "slow" : "fast", (int)g_clr.corners.size(), k, (int)g_clr.queue.size(), g_clr.plantAfter, why, L.c_str(),
         g_clr.slow ? "slow" : "fast", k, (int)g_clr.corners.size());
}
static void ClearStart(const Site &s)
{
    if (!ClearOn() || g_clr.on || g_clr.tried) return;
    g_clr = Clr();
    g_clr.tried = true;   // once a round
    int si = SiteIndex(s.label);
    if (si < 0) return;
    Vec from = s.ap.empty() ? s.entry : s.ap[0].entry;
    std::vector<Vec> c;
    auto add = [&](const Vec &v) {
        for (auto &o : c)
            if (Dist(o, v) < 150.f) return;
        if (c.size() < 8) c.push_back(v);
    };
    for (auto &h : s.hold) add(h);
    for (auto &sp : g_mi.spots)
        if ((sp.second & 1) && Dist(sp.first, s.c) < s.radius + 150.f && Dist(sp.first, from) > 150.f) add(sp.first);
    if (c.empty()) return;
    std::sort(c.begin(), c.end(), [&](const Vec &a, const Vec &b) { return Wrap180(YawTo(from, a) - YawTo(from, s.c)) < Wrap180(YawTo(from, b) - YawTo(from, s.c)); });
    std::vector<Player *> ts;
    for (auto *p : Team(2, true))
        if (p->role == "group" || p->role == "group2" || p->role == "escort" || p->role == "trickle" || Dist(p->pos, s.c) < s.radius + 900.f)
            ts.push_back(p);
    if (ts.empty()) return;
    g_clr.on = true;
    g_clr.site = si;
    g_clr.corners = c;
    g_clr.seen.assign(c.size(), 0);
    g_clr.startAt = CurTime();
    g_clr.slow = fl::ClearSlow(MeanSmart(Team(2, true)), Rnd());
    for (size_t i = 0; i < ts.size(); i++)
    {
        std::vector<int> mine, all = fl::ClearShare((int)c.size(), (int)ts.size(), (int)i);
        float chance = fl::ClearCheck(DialOf(*ts[i], D_AWARE, false));
        for (int k : all)
            if (Rnd() < chance) mine.push_back(k);
        g_clr.queue[ts[i]->name] = mine;
    }
}
static void ClearAuto()
{
    // a team without a called hit (sloppy: it trickles in) starts its clearing when the first T reaches the site
    if (g_clr.on || g_clr.tried || !ClearOn() || g_plan.planted || g_plan.executed || g_phase != PH_LIVE) return;
    const Site *s = FindSite(g_plan.tSite);
    if (!s) return;
    for (auto *p : Team(2, true))
        if (Dist(p->pos, s->c) < s->radius + 200.f)
        {
            ClearStart(*s);
            return;
        }
}
static bool J5ClearWalk(const Player &p, float now)
{
    if (!g_clr.on || !g_clr.slow || g_plan.planted || now - g_clr.startAt > 20.f) return false;
    auto it = g_clr.queue.find(p.name);
    return it != g_clr.queue.end() && !it->second.empty() && p.name != g_bomber;
}
static void ClearFrame(Player &p, float now)
{
    if (!g_clr.on) return;
    if (g_plan.planted || now - g_clr.startAt > 20.f)
    {
        if (g_plan.planted && g_clr.plantAfter < 0) g_clr.plantAfter = g_plan.plantAt - g_clr.startAt;
        ClearLog(g_plan.planted ? "planted" : "timeout");
        return;
    }
    auto it = g_clr.queue.find(p.name);
    if (it == g_clr.queue.end() || it->second.empty() || !p.falive || BotAttacking(p.ent) || p.sh.panic) return;
    std::vector<int> &q = it->second;
    while (!q.empty() && g_clr.seen[q.front()]) q.erase(q.begin());
    if (q.empty()) return;
    const Vec &c = g_clr.corners[q.front()];
    Vec tgt{c.x, c.y, c.z + 56.f};
    float &since = g_clr.since[p.name];
    if (since <= 0 || since > now) since = now;
    float d = PeekDial(p);
    LookWant(p, LK_CLEAR, tgt, Lerp(300.f, 800.f, d), fl::ClearHold(g_clr.slow, DialOf(p, D_AWARE, false)) + 0.3f, "clear");
    float yaw;
    Vec eye = EyeOf(p.ent, p.fpos);
    if (EyeYaw(p, &yaw) && fabsf(Wrap180(YawTo(eye, tgt) - yaw)) < 12.f && now - since >= fl::ClearHold(g_clr.slow, DialOf(p, D_AWARE, false)) * 0.5f &&
        Los(eye, tgt))
    {
        g_clr.seen[q.front()] = 1;
        q.erase(q.begin());
        since = now;
    }
    else if (now - since > 1.5f)
    {
        q.erase(q.begin());   // could not get it in sight in time: next one
        since = now;
    }
}
// -- F7: the post-plant -------------------------------------------------------------------------------------------
// Every T bot defends the bomb (OnPlant above: a bot that fails its roll gets a sloppier post spot instead of step 9's
// release). Proof: "POSTR" 10 s after the plant: Ts alive, near the bomb (< 1500 u), far (> 2500 u = "ran off").
static bool g_postLogged;
static void PostTick(float now)
{
    if (!g_plan.planted || g_postLogged || now - g_plan.plantAt < 10.f || now < g_plan.plantAt) return;
    g_postLogged = true;
    int alive = 0, nearB = 0, far = 0;
    for (auto *p : Team(2, false))
    {
        alive++;
        float d = Dist(p->pos, g_plan.bomb);
        if (d < 1500.f) nearB++;
        if (d > 2500.f) far++;
    }
    BLog("POSTR map=%s r=%d on=%d alive=%d near=%d far=%d site=%s", g_mapName.c_str(), g_roundNo, (int)PostOn(), alive, nearB, far, g_plan.plantSite.c_str());
}
// -- F8: retakes ----------------------------------------------------------------------------------------------------
// Hurry: the gather ends 4 s after the second CT is in, 15 s after the plant at most, or when the clock says so
// (fl::RetakeDeadline; step 9b waited up to 27 s). Together: the first CT inside 900 u of the bomb waits (a hold on the
// spot, 1.5 s) for the next one when he is 600+ u behind, once per retake, never on a tight clock (fl::RetakeWait).
// The AWP: a retaking CT with an AWP (its weapon from item_equip) takes the site hold that is farthest from the bomb but
// within 1400 u and has a clear line to it - a long angle over the bomb - instead of walking onto the site, and while
// retaking it fires without the attack delay (the stock sniper waited; part K's hold fix covers "not shooting moving Ts").
// Proof: "RETAKE" per planted round: retakers, gathered, why they went, the spread of their arrivals (first to last
// inside 900 u of the bomb: "one by one" = a long spread), waits, AWP retakers + their shots, defused.
static struct Rtk
{
    bool waited = false, logged = false;
    std::string waitWho, waitFor;
    float waitUntil = -1;
    std::string goWhy = "-";
    int gathered = 0, total = 0, awps = 0, awpShots = 0;
    std::map<std::string, float> arrived;
} g_rtk;
static bool IsAwp(const Player &p) { return strstr(p.weapon.c_str(), "awp") != nullptr; }
static void RetakeAwp(Player *p, const Vec &bomb, const Site *s)
{
    if (!RetakeOn() || !IsAwp(*p) || !s) return;
    Vec best{};
    float bd = -1;
    for (auto &h : s->hold)
    {
        float d = Dist(h, bomb);
        if (d > bd && d < 1400.f && Los({h.x, h.y, h.z + 64.f}, {bomb.x, bomb.y, bomb.z + 20.f}))
        {
            bd = d;
            best = h;
        }
    }
    if (bd < 0) return;
    Assign(p, T_HOLD, best, 1, 120, "CT retake AWP angle", 40);
    SetRoute(p, *s, &best);
    p->role = "retake-awp";   // not in the gather count, not sent in with the group, no trade runs
    RotLog(*p, "awp angle", CurTime());   // JOB 5b N: its ROT line ends here (it holds an angle, not the bomb)
    g_rtk.awps++;
    BLog("plan: %s (AWP) holds a long angle over the bomb (%.0f u) for the retake", p->name.c_str(), bd);
}
static void RetakeShot(Player &sh)
{
    if (g_plan.planted && sh.team == 3 && (sh.role == "retake" || sh.role == "retake-awp") && IsAwp(sh)) g_rtk.awpShots++;
}
static void RetakeTick(float now)
{
    if (!g_plan.planted || !RetakeOn() || !g_steer || g_layout != 1) return;
    auto cts = Team(3, true);
    std::vector<std::pair<float, Player *>> r;
    for (auto *p : cts)
        if (p->role == "retake")
        {
            float d = Dist(p->pos, g_plan.bomb);
            r.push_back({d, p});
            if (d < 900.f && !g_rtk.arrived.count(p->name)) g_rtk.arrived[p->name] = now;
        }
    // the AWP on its angle is the last CT on the retake (or the clock runs out): it goes for the bomb itself
    float left0 = 40.f - (now - g_plan.plantAt);
    for (auto *p : cts)
        if (p->role == "retake-awp" && (r.empty() || left0 < 14.f))
        {
            p->role = "retake";
            Assign(p, T_MOVE, g_plan.bomb, 1, 350, "CT retake");
            BLog("plan: %s (AWP) leaves its angle for the bomb (%s)", p->name.c_str(), r.empty() ? "last CT on the retake" : "the clock");
        }
    int awpOnAngle = 0;
    for (auto *p : cts) awpOnAngle += p->role == "retake-awp";
    g_rtk.total = std::max(g_rtk.total, (int)r.size() + awpOnAngle);
    if (g_rtk.waitUntil > 0)
    {
        // the waiting CT goes again once the next one has caught up (within 300 u of it) or 1.5 s are over
        Player *w = FindPl(g_rtk.waitWho), *m = FindPl(g_rtk.waitFor);
        bool caught = w && m && m->alive && Dist(m->pos, w->pos) < 300.f;
        if (!w || !w->alive || caught || now >= g_rtk.waitUntil || !m || !m->alive)
        {
            if (w && w->alive && w->role == "retake") Assign(w, T_MOVE, g_plan.bomb, 1, 350, "CT retake");
            g_rtk.waitUntil = -1;
        }
        return;
    }
    if (!g_plan.retakeGo || g_rtk.waited || r.size() < 2) return;
    std::sort(r.begin(), r.end(), [](const std::pair<float, Player *> &a, const std::pair<float, Player *> &b) { return a.first < b.first; });
    float left = 40.f - (now - g_plan.plantAt);
    if (fl::RetakeWait(r[0].first, r[1].first, left, 10.f) && r[0].second->task.kind == T_MOVE &&
        !(RotOn() && now - r[0].second->sh.lastSeen < 2.f))   // JOB 5b N: in contact it keeps going
    {
        g_rtk.waited = true;
        Assign(r[0].second, T_HOLD, r[0].second->pos, 1, 150, "CT retake: wait for the next one", 2);
        Player *first = r[0].second;
        g_rtk.waitWho = first->name;
        g_rtk.waitFor = r[1].second->name;
        g_rtk.waitUntil = now + 1.5f;
        BLog("plan: %s waits for %s before going onto the bomb (%.0f u ahead, %.0f s left)", first->name.c_str(), r[1].second->name.c_str(),
             r[1].first - r[0].first, left);
    }
}
static void RetakeLog()
{
    if (!g_plan.planted || g_rtk.logged || g_mi.hostage) return;
    g_rtk.logged = true;
    float lo = 1e9f, hi = -1e9f;
    for (auto &kv : g_rtk.arrived)
    {
        lo = std::min(lo, kv.second);
        hi = std::max(hi, kv.second);
    }
    BLog("RETAKE map=%s r=%d on=%d site=%s retakers=%d arrived=%d spread=%.1f waited=%d awps=%d awpShots=%d defused=%d joined=%d aliveAtPlant=%d rotOn=%d",
         g_mapName.c_str(), g_roundNo, (int)RetakeOn(), g_plan.plantSite.c_str(), g_rtk.total, (int)g_rtk.arrived.size(),
         g_rtk.arrived.size() >= 2 ? hi - lo : -1.f, (int)g_rtk.waited, g_rtk.awps, g_rtk.awpShots, (int)g_rm.defuse, g_rtkJoined, g_rtkAlive, (int)RotOn());
}
// -- F9: grenades -------------------------------------------------------------------------------------------------
// Every grenade thrown (weapon_fire with a grenade) is matched to its detonation (molotov / HE / flash / smoke
// _detonate events) and judged where it went off (fl::NadeDanger): on a teammate, on the thrower, a fire with no enemy
// known within 900 u (and not on the bomb); team damage from fire / HE and team flashes (player_blind by a teammate,
// > 1 s) are counted. The veto ("triple-check"): while a bot holds a molotov / incendiary / HE (item_equip) and is not
// fighting, its throw is predicted 20 times a second from its view (fl::ThrowArc + world traces: where it lands), judged
// only once its view is steady (< 2 deg between checks = lined up, not still turning); a landing ON A TEAMMATE OR ITS OWN FEET
// twice in a row -> its view is held 30 deg above for 2.2 s (look controller, top priority; "no enemy known there" is
// counted in the NADE lines but never vetoed - the bots' intel is too thin to stop a throw on it). The stock toss pulls
// the pin once its view is on its target (the lineage's "lined up" check; else "THROW FAILED" after ~2 s) and the
// grenade leaves with the view at the release: a veto before the pin cancels the throw, one after it sends the grenade
// 30 deg higher (off the teammate / its feet). At most 2 vetoes per grenade. UNPROVEN on 1.38: the NADE lines say
// whether a vetoed grenade was thrown anyway and whether it still went off dumb (vetoDumb).
// Proof: "NADE" per detonation (thrower, kind, distance thrown, dumb flags, team / self damage so far), "NADE veto" per
// veto (and whether it was thrown within 3 s after all), "NADER" per round.
struct Throw
{
    std::string who, kind;
    Vec from{};
    float at = 0;
    int team = 0;
};
static std::vector<Throw> g_throws;
static struct NadeM
{
    int thrown = 0, dumb = 0, teamDmg = 0, selfDmg = 0, teamFlash = 0, vetoes = 0, vetoThrown = 0, vetoDumb = 0, logged = 0;
} g_nm;
// IGameEvent::GetFloat: slot 9 on this build (step 10 measured GetInt = 7 and GetString = 10; GetUint64 sits at 8, as in
// the SDK's order GetBool, GetInt, GetUint64, GetFloat, GetString) - a coordinate outside the map is dropped by the caller
static float EvFloat(void *ev, const char *k)
{
    float v = VF<float (*)(void *, const char *, float)>(ev, 9)(ev, k, 0.f);
    return std::isfinite(v) ? v : 0.f;
}
static std::string NadeKind(const char *w)
{
    for (const char *k : {"molotov", "incgrenade", "hegrenade", "flashbang", "smokegrenade", "decoy"})
        if (strstr(w, k)) return k;
    return "";
}
static void NadeEvent(const char *n, Player *p, void *ev, float now)
{
    auto GetInt = [&](const char *k) { return VF<int (*)(void *, const char *, int)>(ev, 7)(ev, k, 0); };
    auto GetStr = [&](const char *k) { return VF<const char *(*)(void *, const char *, const char *)>(ev, 10)(ev, k, ""); };
    if (!strcmp(n, "weapon_fire"))
    {
        const char *w = GetStr("weapon");
        std::string k = w ? NadeKind(w) : "";
        if (!p || k.empty()) return;
        g_throws.push_back({p->name, k, p->fpos, now, p->team});
        g_nm.thrown++;
        if (now > p->nadeVetoAt && now < p->nadeVetoUntil + 3.f)
        {
            g_nm.vetoThrown++;   // vetoed, but thrown within 3 s after all: the veto did not hold
            BLog("NADE veto map=%s r=%d bot=%s result=thrown-anyway kind=%s", g_mapName.c_str(), g_roundNo, p->name.c_str(), k.c_str());
        }
        return;
    }
    if (!strcmp(n, "player_hurt"))
    {
        Player *a = ByUid(GetInt("attacker"));
        const char *w = GetStr("weapon");
        if (!a || !p || !w || (!strstr(w, "inferno") && !strstr(w, "hegrenade") && !strstr(w, "molotov"))) return;
        if (a == p) g_nm.selfDmg += GetInt("dmg_health");
        else if (a->team == p->team) g_nm.teamDmg += GetInt("dmg_health");
        return;
    }
    if (!strcmp(n, "player_blind"))
    {
        Player *a = ByUid(GetInt("attacker"));
        float dur = EvFloat(ev, "blind_duration");
        if (a && p && a != p && a->team == p->team && dur > 1.f && dur < 20.f) g_nm.teamFlash++;
        return;
    }
    std::string kind = !strcmp(n, "molotov_detonate") ? "molotov" : !strcmp(n, "hegrenade_detonate") ? "hegrenade" : !strcmp(n, "flashbang_detonate") ? "flashbang"
                     : !strcmp(n, "smokegrenade_detonate") ? "smokegrenade" : "";
    if (kind.empty() || !p) return;
    Vec at{EvFloat(ev, "x"), EvFloat(ev, "y"), EvFloat(ev, "z")};
    if (!(fabsf(at.x) < 20000.f && fabsf(at.y) < 20000.f && fabsf(at.z) < 20000.f)) return;   // not a map position: never trusted
    Throw t{p->name, kind, p->fpos, now, p->team};
    for (auto it = g_throws.begin(); it != g_throws.end(); ++it)
        if (it->who == p->name && (it->kind == kind || (kind == "molotov" && it->kind == "incgrenade")) && now - it->at < 8.f && now >= it->at)
        {
            t = *it;
            g_throws.erase(it);
            break;
        }
    std::vector<Vec> mates, foes;
    for (auto &kv : g_pl)
        if (kv.second.alive && kv.second.team == p->team && kv.second.name != p->name) mates.push_back(kv.second.pos);
    for (auto &kv : g_intel[p->team])
        if (now - kv.second.t < 8.f && now >= kv.second.t) foes.push_back(kv.second.pos);
    UtilDetonate(p, kind, at, now);   // JOB 5b O: where our throws went off
    bool fire = kind == "molotov" || kind == "hegrenade";
    int f = fl::NadeDanger(at, t.from, mates, foes, kind == "molotov", g_plan.planted && Dist(at, g_plan.bomb) < 700.f);
    if (!fire) f &= ~fl::ND_NOTARGET;
    if (fire && (f & (fl::ND_TEAM | fl::ND_SELF))) g_nm.dumb++;
    if (fire && (f & (fl::ND_TEAM | fl::ND_SELF)) && now - p->nadeVetoAt < 6.f) g_nm.vetoDumb++;   // vetoed and still dumb
    if (g_nm.logged++ < 40)
        BLog("NADE map=%s r=%d bot=%d who=%s team=%s kind=%s thrown=%.0f dumb=%d team=%d self=%d notarget=%d vetoed=%d | %s's %s went off %.0fu from where it was thrown%s",
             g_mapName.c_str(), g_roundNo, (int)p->bot, p->name.c_str(), p->team == 2 ? "T" : "CT", kind.c_str(), Dist(at, t.from),
             fire && (f & (fl::ND_TEAM | fl::ND_SELF)) ? 1 : 0, (f & fl::ND_TEAM) ? 1 : 0, (f & fl::ND_SELF) ? 1 : 0, (f & fl::ND_NOTARGET) ? 1 : 0,
             now - p->nadeVetoAt < 6.f ? 1 : 0, p->name.c_str(), kind.c_str(), Dist(at, t.from),
             (f & fl::ND_TEAM) ? " - on a teammate" : (f & fl::ND_SELF) ? " - at its own feet" : (f & fl::ND_NOTARGET) ? " - no enemy known there" : "");
}
static bool NadeOn(const Player &p) { return g_j.nade && g_modeOk && g_layout == 1 && p.bot; }
static void NadeFrame(Player &p, float now)
{
    if (!NadeOn(p) || !p.falive || now < p.nadeNext || UtilBusy(p)) return;   // JOB 5b O: its own throw is checked when it is aimed
    std::string k = NadeKind(p.weapon.c_str());
    if (k != "molotov" && k != "incgrenade" && k != "hegrenade")
    {
        p.nadeBad = 0;
        p.nadeVetoes = 0;
        return;
    }
    p.nadeNext = now + 0.05f;   // 20 a second: the stock toss throws soon after its view is lined up
    if (BotAttacking(p.ent) || now < p.nadeVetoUntil || p.nadeVetoes >= 2 || g_np.eyeP <= 0 || !g_trace || g_traceOk < 0) return;
    float yaw, pitch = *(float *)(p.ent + g_np.eyeP);
    if (!EyeYaw(p, &yaw) || !std::isfinite(pitch)) return;
    // judge only a throw it has lined up: the view steady (< 2 deg) since the last check - not while it still turns
    bool steady = fabsf(Wrap180(yaw - p.nadeYaw)) < 2.f && fabsf(pitch - p.nadePitch) < 2.f;
    p.nadeYaw = yaw;
    p.nadePitch = pitch;
    if (!steady)
    {
        p.nadeBad = 0;
        return;
    }
    Vec eye = EyeOf(p.ent, p.fpos);
    std::vector<Vec> arc = fl::ThrowArc(eye, pitch, yaw, 0.1f, 2.0f);
    Vec land = arc.back();
    for (size_t i = 0; i + 1 < arc.size(); i++)
    {
        float fr = TraceFrac(arc[i], arc[i + 1]);
        if (fr >= 0.f && fr < 0.999f)
        {
            land = {arc[i].x + (arc[i + 1].x - arc[i].x) * fr, arc[i].y + (arc[i + 1].y - arc[i].y) * fr, arc[i].z + (arc[i + 1].z - arc[i].z) * fr};
            break;
        }
    }
    std::vector<Vec> mates, foes;
    for (auto &kv : g_pl)
        if (kv.second.alive && kv.second.team == p.team && kv.second.name != p.name) mates.push_back(kv.second.pos);
    for (auto &kv : g_intel[p.team])
        if (now - kv.second.t < 8.f && now >= kv.second.t) foes.push_back(kv.second.pos);
    int f = fl::NadeDanger(land, p.fpos, mates, foes, k != "hegrenade", g_plan.planted && Dist(land, g_plan.bomb) < 700.f);
    // the veto is for a landing on a teammate or its own feet; "no enemy known there" is only counted (NADE lines) -
    // the bots' intel is too thin to stop a throw on it
    if (!(f & (fl::ND_TEAM | fl::ND_SELF)))
    {
        p.nadeBad = 0;
        return;
    }
    if (++p.nadeBad < 2) return;
    // twice in a row a dumb landing: hold its view 30 deg above its throw for 2.2 s (the toss waits to be lined up)
    float up = std::max(-89.f, pitch - 30.f) / 57.29578f, yr = yaw / 57.29578f;
    Vec away{eye.x + 400.f * cosf(up) * cosf(yr), eye.y + 400.f * cosf(up) * sinf(yr), eye.z - 400.f * sinf(up)};
    if (LookWant(p, LK_VETO, away, 900.f, 2.2f, "nade veto"))
    {
        p.nadePitch = 1000;   // its view moves now: judge again only once it is steady
        p.nadeVetoAt = now;
        p.nadeVetoUntil = now + 2.2f;
        p.nadeVetoes++;
        p.nadeBad = 0;
        g_nm.vetoes++;
        BLog("NADE veto map=%s r=%d bot=%s kind=%s landing=%.0fu team=%d self=%d notarget=%d | %s's %s would land %s - held", g_mapName.c_str(), g_roundNo,
             p.name.c_str(), k.c_str(), Dist(land, p.fpos), (f & fl::ND_TEAM) ? 1 : 0, (f & fl::ND_SELF) ? 1 : 0, (f & fl::ND_NOTARGET) ? 1 : 0, p.name.c_str(),
             k.c_str(), (f & fl::ND_TEAM) ? "on a teammate" : (f & fl::ND_SELF) ? "at its own feet" : "where no enemy is known");
    }
}
static void FRoundEnd()
{
    ClearLog("round end");
    RetakeLog();
    BLog("NADER map=%s r=%d on=%d thrown=%d dumb=%d teamDmg=%d selfDmg=%d teamFlash=%d vetoes=%d vetoThrown=%d vetoDumb=%d", g_mapName.c_str(), g_roundNo,
         g_j.nade, g_nm.thrown, g_nm.dumb, g_nm.teamDmg, g_nm.selfDmg, g_nm.teamFlash, g_nm.vetoes, g_nm.vetoThrown, g_nm.vetoDumb);
}
static void FRoundStart()
{
    g_clr = Clr();
    g_postLogged = false;
    g_rtk = Rtk();
    g_throws.clear();
    g_nm = NadeM();
}
// ---- sneak test (hear_test <R>): 1v1, the T bot runs (or walks) round behind the CT bot and up to R units ----
static struct Sneak
{
    std::string run, tgt;
    float t0 = -1, heardAt = -1, turnAt = -1, seenAt = -1, nextMove = 0, nextHold = 0, heardD = -1, heldAt = -1, tgtElo = 0, tgtYaw = 0;
    int held = 0, logged = 0, phase = 0, steps = 0, outside = 0;
} g_sn;
static bool TestMute(const Player &p)
{
    return (g_s.circle && p.name == g_circ.circ) || (g_j.sneak && p.name == g_sn.run) || (g_j.walkup && p.name == g_wu.run);
}
static void SneakLog()
{
    if (g_sn.logged || g_sn.run.empty()) return;
    g_sn.logged = 1;
    auto rel = [](float t) { return t >= 0 && g_sn.heardAt >= 0 ? t - g_sn.heardAt : -1.f; };
    BLog("SNEAK map=%s r=%d target=%s elo=%.0f R=%d walk=%d on=%d heard=%d heardD=%.0f steps=%d outside=%d turned=%d turn=%.2f seenFirst=%d",
         g_mapName.c_str(), g_roundNo, g_sn.tgt.c_str(), g_sn.tgtElo, g_j.sneak, g_j.sneakWalk, g_j.hear, g_sn.heardAt >= 0, g_sn.heardD, g_sn.steps,
         g_sn.outside, g_sn.turnAt >= 0, rel(g_sn.turnAt), g_sn.seenAt >= 0 && (g_sn.heardAt < 0 || g_sn.seenAt < g_sn.heardAt));
}
static void SneakFrame(float now)
{
    Player *run = nullptr, *tgt = nullptr;
    for (auto &kv : g_pl)
        if (kv.second.bot && kv.second.falive)
        {
            if (kv.second.team == 2 && !run) run = &kv.second;
            if (kv.second.team == 3 && !tgt) tgt = &kv.second;
        }
    if (!run || !tgt || g_layout != 1) return;
    if (g_sn.run.empty())
    {
        g_sn.run = run->name;
        g_sn.tgt = tgt->name;
        g_sn.tgtElo = tgt->elo;
    }
    if (run->name != g_sn.run || tgt->name != g_sn.tgt) return;
    *(uint8_t *)(run->ent + kOffStateTime + 4) = 0;   // the runner never fights
    if (g_sn.run.size() && g_j.sneakWalk && g_np.velmod > 0)
    {
        float *vm = (float *)(run->ent + g_np.velmod);
        if (*vm >= 0.f && *vm <= 1.5f) *vm = 0.52f;   // shift-walk speed (step 10's quiet walk: steps go silent)
    }
    Vec arena = g_mi.ok && !g_mi.sites.empty() ? g_mi.sites[0].c : tgt->fpos;
    // the target walks to the arena and holds there
    if (!g_sn.held && now - g_liveAt > 0.5f && now >= g_sn.nextHold)
    {
        g_sn.nextHold = now + 0.5f;
        if (fl::Dist2(tgt->fpos, arena) > 60.f && now - g_liveAt < 25.f)
            BotMoveTo(tgt->ent, arena, 1);
        else if (BotHideHere(tgt->ent, tgt->fpos, 300.f))
        {
            g_sn.held = 1;
            g_sn.heldAt = now;
        }
    }
    // the target's hearing: first audible step of the runner, did it face him
    float yaw;
    bool eyeOk = EyeYaw(*tgt, &yaw);
    Vec teye = EyeOf(tgt->ent, tgt->fpos);
    float off = eyeOk ? fabsf(Wrap180(YawTo(teye, run->fpos) - yaw)) : 0.f;
    bool los = Los(teye, {run->fpos.x, run->fpos.y, run->fpos.z + 62.f});
    if (g_sn.seenAt < 0 && off < 50.f && los) g_sn.seenAt = now;
    if (g_sn.heardAt >= 0 && g_sn.turnAt < 0 && off < 30.f) g_sn.turnAt = now;
    if (!g_sn.held || now - g_sn.heldAt < 2.f || now < g_sn.nextMove) return;
    g_sn.nextMove = now + 0.25f;
    // round behind the target (the point 700 units behind where it looks), then in to R units, then stand
    if (!eyeOk) return;
    float by = (g_sn.tgtYaw != 0.f ? g_sn.tgtYaw : yaw) / 57.29578f;
    if (g_sn.phase == 0)
    {
        g_sn.tgtYaw = yaw;
        g_sn.phase = 1;
    }
    Vec behind{tgt->fpos.x - 700.f * cosf(by), tgt->fpos.y - 700.f * sinf(by), tgt->fpos.z};
    if (g_sn.phase == 1)
    {
        if (fl::Dist2(run->fpos, behind) > 150.f && now - g_liveAt < 40.f)
        {
            BotMoveTo(run->ent, behind, 1);
            return;
        }
        g_sn.phase = 2;
    }
    if (g_sn.phase == 2)
    {
        if (fl::Dist2(run->fpos, tgt->fpos) > (float)g_j.sneak)
            BotMoveTo(run->ent, tgt->fpos, 1);
        else if (BotHideHere(run->ent, run->fpos, 300.f))
            g_sn.phase = 3;
    }
}
// the runner's noises (called for every noise heard by the sneak target)
static void SneakNoise(Player &src, int kind, const Vec &pos, float now)
{
    if (!g_j.sneak || src.name != g_sn.run || g_sn.tgt.empty()) return;
    Player *t = FindPl(g_sn.tgt);
    if (!t || !t->falive) return;
    float d = Dist(t->fpos, pos);
    if (!fl::Audible(g_j.hr, kind, d, src.hsp, IsWalking(src), IsDucking(src))) return;
    if (kind == fl::NZ_STEP) g_sn.steps++;
    float yaw;
    if (EyeYaw(*t, &yaw) && fabsf(Wrap180(YawTo(EyeOf(t->ent, t->fpos), pos) - yaw)) > 50.f) g_sn.outside++;
    if (g_sn.heardAt < 0)
    {
        g_sn.heardAt = now;
        g_sn.heardD = d;
    }
}
// ---- per frame ----
static float g_j5Last;
static void HearFrame(float now)
{
    for (auto it = g_landings.begin(); it != g_landings.end();)
    {
        if (now < it->at && now > it->at - 5.f) { ++it; continue; }
        Player *p = FindPl(it->src);
        if (p && p->falive && now <= it->at + 1.f)
        {
            SneakNoise(*p, it->kind, p->fpos, now);
            HearNoise(*p, it->kind, p->fpos, now);
        }
        it = g_landings.erase(it);
    }
    for (auto &kv : g_pl)
    {
        Player &b = kv.second;
        if (!b.bot || !b.ent) continue;
        if (b.hr.pend && now >= b.hr.due) HearAct(b, now);
        FireFrame(b, now);   // STEP 12G
        PeekFrame(b, now);   // STEP 12E
        UnholdFrame(b, now); // STEP 12K
        ClearFrame(b, now);  // STEP 12F5
        NadeFrame(b, now);   // STEP 12F9
        UtilFrame(b, now);   // JOB 5b O: a throw in progress (equip, aim, pin)
        MoveFrame(b, now);   // JOB 5b P: the fight's movement, after-fight repositions, falling back when seen by 2+
        CrouchFrame(b, now); // JOB 5b Q: crouch-spray, crouched / standing holds
        if (b.hr.pend && now < b.hr.at - 5.f) b.hr.pend = false;   // the clock went back (new level)
        // the metric: faced the source within 2.5 s?
        if (b.hr.chk)
        {
            Player *src = FindPl(b.hr.chkSrc);
            float yaw;
            bool done = false, turned = false;
            if (!b.falive || !src || !src->falive || now < b.hr.chkT0)
                done = true;
            else if (EyeYaw(b, &yaw) && fabsf(Wrap180(YawTo(EyeOf(b.ent, b.fpos), src->fpos) - yaw)) < 30.f)
                done = turned = true;
            else if (now - b.hr.chkT0 > 2.5f)
                done = true;
            if (done)
            {
                b.hr.chk = false;
                if (b.falive && src && src->falive)
                {
                    b.hr.cN++;
                    if (turned)
                    {
                        b.hr.cK++;
                        b.hr.cT += now - b.hr.chkT0;
                    }
                    if (g_chkLogged++ < g_j.chkLog)
                    BLog("HEARCHK map=%s r=%d %s team=%s elo=%.0f hear=%.2f on=%d src=%s kind=%s d=%.0f turned=%d t=%.2f", g_mapName.c_str(), g_roundNo,
                         b.name.c_str(), b.team == 2 ? "T" : "CT", b.elo, HearDial(b), (int)HearOn(b), b.hr.chkSrc.c_str(), fl::NoiseName(b.hr.chkKind),
                         b.hr.chkD, (int)turned, turned ? now - b.hr.chkT0 : -1.f);
                }
            }
        }
    }
}
static void J5cFrame(float now);   // JOB 5c (below)
static void J5cRoundStart();
static void RefreshJ5c();
static void J5cRoundEnd();
static void Job5Frame()
{
    float now = CurTime();
    float dt = now - g_j5Last;
    g_j5Last = now;
    if (dt < 0 || dt > 1.f) dt = 0;
    RefreshJ5();
    RefreshJ5b();   // JOB 5b
    RefreshJ5c();   // JOB 5c
    // every player: this frame's position, speed, user id (humans too - step 10/11 mapped bots only)
    for (auto &kv : g_pl)
    {
        Player &p = kv.second;
        void *pi = p.e && g_pi ? g_pi(p.e) : nullptr;
        if (!pi)
        {
            p.falive = false;
            continue;
        }
        p.falive = (p.team == 2 || p.team == 3) && !PI_Dead(pi) && !PI_Observer(pi);
        p.fpos = PI_Origin(pi);
        if (!p.bot)
        {
            int uid = VF<int (*)(void *)>(pi, 1)(pi);
            if (uid > 0) g_byUid[uid] = p.name;
        }
        if (now - p.hspT >= 0.05f || now < p.hspT)
        {
            float sp = now > p.hspT ? fl::Dist2(p.fpos, p.hspPos) / (now - p.hspT) : 0.f;
            p.hsp = sp < 600.f ? sp : 0.f;
            p.hspPos = p.fpos;
            p.hspT = now;
        }
    }
    CtMetrics(now);   // STEP 12C (steering on or off)
    J5cFrame(now);    // JOB 5c (proof lines on and off alike; the steering parts check their own switches)
    PlanFrame(now);   // STEP 12D: the call in the freeze, acknowledgements, chat delivery, mid counts
    PostTick(now);    // STEP 12F7
    ClearAuto();      // STEP 12F5
    if (g_phase == PH_LIVE) RetakeTick(now);   // STEP 12F8
    CrouchTick(now);   // JOB 5b Q: find / prove the bots' crouch flag (the freeze too)
    if (g_phase != PH_LIVE || g_layout != 1 || !g_np.ok) return;
    HearFrame(now);
    if (g_j.sneak) SneakFrame(now);
    if (g_j.walkup) WalkupFrame(now);   // STEP 12K test
    for (auto &kv : g_pl)
        if (kv.second.bot && kv.second.ent) LookFrame(kv.second, now, dt);
}
static void J5RoundStart()
{
    J5bRoundStart();   // JOB 5b
    J5cRoundStart();   // JOB 5c
    g_hearLogged = g_chkLogged = g_fireLogged = g_peekLogged = g_unholdLogged = 0;
    g_ctm = CtM();
    TPlanFreeze();   // STEP 12D: this round's plan and its caller (called 1.5 s into the freeze)
    FRoundStart();   // STEP 12F
    g_landings.clear();
    g_intel[2].clear();
    g_intel[3].clear();
    g_sn = Sneak();
    g_wu = Walkup();
    for (auto &kv : g_pl)
    {
        Player &p = kv.second;
        p.lk = Player::Lk();
        p.hr = Player::Hr();
        p.fr = Player::Fr();
        p.pk = Player::Pk();
        p.unholds = 0;
        p.nadeBad = p.nadeVetoes = 0;
        p.nadeVetoAt = p.nadeVetoUntil = -100;
        p.nadePitch = 1000;
        if (p.bot) p.hr.h = DialOf(p, D_AWARE, false);
        if (p.bot) p.fr.g = 0.5f * (DialOf(p, D_FOOT, false) + DialOf(p, D_REACT, false));   // STEP 12G
    }
}
static void J5RoundEnd()
{
    J5bRoundEnd();   // JOB 5b
    J5cRoundEnd();   // JOB 5c
    if (g_j.sneak) SneakLog();
    if (g_j.walkup) WalkupLog();
    CtRoundEnd();
    {
        int u[4] = {0, 0, 0, 0};
        for (auto &kv : g_pl)
            if (kv.second.bot && (kv.second.team == 2 || kv.second.team == 3)) u[kv.second.team] += kv.second.unholds;
        BLog("HOLDR map=%s r=%d on=%d unholdsT=%d unholdsCT=%d", g_mapName.c_str(), g_roundNo, g_j.unhold && g_j.gold, u[2], u[3]);
    }
    PlanRoundEnd();   // STEP 12D/J
    FRoundEnd();      // STEP 12F
    for (auto &kv : g_pl)
    {
        Player &p = kv.second;
        if (!p.bot || (p.team != 2 && p.team != 3)) continue;
        BLog("HEARR map=%s r=%d %s team=%s elo=%.0f hear=%.2f on=%d reacts=%d stops=%d calls=%d chk=%d turned=%d meanT=%.2f", g_mapName.c_str(),
             g_roundNo, p.name.c_str(), p.team == 2 ? "T" : "CT", p.elo, HearDial(p), (int)HearOn(p), p.hr.reacts, p.hr.stops, p.hr.calls, p.hr.cN,
             p.hr.cK, p.hr.cK ? p.hr.cT / p.hr.cK : -1.f);
        const Player::Fr &f = p.fr;
        BLog("FIRER map=%s r=%d %s team=%s elo=%.0f dial=%.2f on=%d hits=%d still=%d shotAt=%d turn=%d dodge=%d cover=%d coverOk=%d repeek=%d",
             g_mapName.c_str(), g_roundNo, p.name.c_str(), p.team == 2 ? "T" : "CT", p.elo, FireDial(p), (int)FireOn(p), f.nHit, f.nStill, f.nShotAt,
             f.nTurn, f.nDodge, f.nCover, f.nCoverOk, f.nPeek);
        const Player::Pk &k = p.pk;
        BLog("PEEKR map=%s r=%d %s team=%s elo=%.0f dial=%.2f fw=%.2f on=%d preaims=%d jiggles=%d peeks=%d contacts=%d cs=%d", g_mapName.c_str(), g_roundNo,
             p.name.c_str(), p.team == 2 ? "T" : "CT", p.elo, PeekDial(p), p.fw, (int)PeekOn(p), k.preaims, k.jiggles, k.peeks, k.contacts, k.cs);
    }
}
static void J5LevelInit()
{
    g_j5Last = 0;
    g_hearLogged = g_chkLogged = g_fireLogged = 0;
    g_tHits.clear();
    g_ctm = CtM();
    g_tp = TPS();
    g_tpHist.clear();
    g_tpCount.clear();
    BuildRoutes();
    g_chat.stride = g_chat.strideBad = 0;
    g_chat.pend.clear();
    g_landings.clear();
    g_intel[2].clear();
    g_intel[3].clear();
    g_sn = Sneak();
    J5bLevelInit();   // JOB 5b
}
// ==== end STEP 12 ===================================================================================================

// ==== JOB 5c (ADDENDUM 3 - his playtest of job 5b, 09-29): the switches of points Q-U (the code is after JOB 5b) =======
// Each point is behind its own kill switch (addons/family_test.txt, re-read every second; switched off = the job-5b bots):
//   Q brain_ctq 0        the CT setup: 2 on A, 2 on B, 1 lingerer (mid); holders stay on their ground and walk the CT
//                        side to their spot (5b: fell back to mid doors on the way out, and went round by long)
//   R brain_react 0      a site holder that sees / hears a push fights it and calls it
//   S brain_deathinfo 0  a teammate's death tells the team where the killer is; they react by rank; a bot says it
//   T brain_callroute 0  the called plan, the bomb carrier and every T's route agree from freeze end (mismatches logged)
//   U brain_ctface 0     holders face the way in they hold; rotations / retakes take the direct way (not the safest detour)
static struct J5cT
{
    int q = 1, qRoles = 1, qFall = 1, qRoute = 1;   // Q (+ its parts: ctq_roles, ctq_fall, ctq_route)
    int r = 1;                                      // R
    int s = 1, sChat = 1;                           // S (+ death_chat: a bot says it)
    int t = 1;                                      // T
    int u = 1, uFace = 1, uRun = 1;                 // U (+ ctface_face, ctface_way: rotations the direct way)
    time_t at = 0;
} g_c;
static void RefreshJ5c()
{
    time_t t = time(nullptr);
    if (!g_tv || t == g_c.at) return;   // the test file is read at most once a second
    g_c.at = t;
    const J5cT d;
    g_c.q = (int)g_tv("brain_ctq", d.q);
    g_c.qRoles = (int)g_tv("ctq_roles", d.qRoles);
    g_c.qFall = (int)g_tv("ctq_fall", d.qFall);
    g_c.qRoute = (int)g_tv("ctq_route", d.qRoute);
    g_c.r = (int)g_tv("brain_react", d.r);
    g_c.s = (int)g_tv("brain_deathinfo", d.s);
    g_c.sChat = (int)g_tv("death_chat", d.sChat);
    g_c.t = (int)g_tv("brain_callroute", d.t);
    g_c.u = (int)g_tv("brain_ctface", d.u);
    g_c.uFace = (int)g_tv("ctface_face", d.uFace);
    g_c.uRun = (int)g_tv("ctface_way", d.uRun);
    char k[200];
    snprintf(k, sizeof(k), "ctq %d roles %d fall %d route %d | react %d | deathinfo %d chat %d | callroute %d | ctface %d face %d way %d", g_c.q, g_c.qRoles,
             g_c.qFall, g_c.qRoute, g_c.r, g_c.s, g_c.sChat, g_c.t, g_c.u, g_c.uFace, g_c.uRun);
    static std::string last;
    if (last != k) BLog("job5c hooks: %s", k);
    last = k;
}
static bool CtOn();
static bool CtQOn() { return g_c.q && CtOn(); }
static bool CtHolder(const Player *p);
// Q: a CT holder before the plant (its site / mid / lurk spot), under the 5c rules
static bool CtQHolder(const Player &b) { return CtQOn() && b.team == 3 && CtHolder(&b); }
static bool CtQFast(const Player &p) { return g_c.qRoute && CtQHolder(p) && p.task.route == 1; }
static bool TPlanRole(const Player &p);
static bool TCallFast(const Player &p) { return g_c.t && TPlanRole(p); }
// Q / T: the bot's MoveTo state heads somewhere else than the goal we gave it (by more than the nav's own snap of a goal)
static int g_retargetCT, g_retargetT;
static bool StockRetarget(Player &p, const Vec &goal)
{
    bool ct = g_c.qRoute && CtQHolder(p), t = g_c.t && TPlanRole(p);
    if (!ct && !t) return false;
    const Vec &sg = *(Vec *)(p.ent + kOffMoveTo + 4);
    if (Dist(sg, goal) <= 300.f || Dist(sg, p.snapGoal) <= 300.f) return false;   // ours (or the nav's snap of ours)
    if (CurTime() - p.issuedAt > 1.0f) (ct ? g_retargetCT : g_retargetT)++;   // counted when Steer re-issues (once a second at most)
    return true;
}

// ==== JOB 5b (ADDENDUM 2 - his playtest of job 5, live 09-26 15:22): points L-P ======================================
// "They're not quite playing right... A lot to fix." Same bar as job 5: bots that play like people. Each point is behind
// its own kill switch (addons/family_test.txt, re-read every second; switched off = the job-5 bots exactly):
//   L brain_ctpush 0   CT pushes rare, planned, never aimed at the enemy
//   M brain_ctset 0    the CT setup: anchors ON the sites, one mid, one lurker, spots vary, never parked in CT spawn
//   N brain_rotate 0   rotations and retakes at once, sneaky when needed, no stopping at the first sighting
//   O brain_util 0     utility bought like a player and thrown every round (smokes, flashes, molotovs, HE)
//   P brain_move 0     fights: move, stop, shoot, move; another angle after a fight; fall back when 2+ see it; spots vary
//   Q brain_crouch 0   crouching chosen, not left to the stock AI: crouch-spray by rank, holds crouched or standing
static struct J5bT
{
    int push = 1, pushForce = -1;   // L
    int set = 1, setFocus = 1;      // M
    int rot = 1, rotSneaky = 1;     // N
    int util = 1, utilBuy = 1, utilThrow = 1, utilLog = 80, utilTest = 0, utilTeam = 0;   // O
    int move = 1, moveCyc = 1, moveRepo = 1, moveFall = 1, moveVary = 1, moveTeam = 0, moveLog = 80;   // P
    int crouch = 1, crouchSpray = 1, crouchHold = 1;                                                   // Q
    float moveDial = -1;
    time_t at = 0;
} g_k;
static void RefreshJ5b()
{
    time_t t = time(nullptr);
    if (!g_tv || t == g_k.at) return;   // the test file is read at most once a second
    g_k.at = t;
    const J5bT d;
    g_k.push = (int)g_tv("brain_ctpush", d.push);
    g_k.pushForce = (int)g_tv("ctpush_spot", d.pushForce);   // test: a push every round to this spot index
    g_k.set = (int)g_tv("brain_ctset", d.set);
    g_k.setFocus = (int)g_tv("ctset_focus", d.setFocus);   // an anchor keeps its angle on a far noise
    g_k.rot = (int)g_tv("brain_rotate", d.rot);
    g_k.rotSneaky = (int)g_tv("rotate_sneaky", d.rotSneaky);
    g_k.util = (int)g_tv("brain_util", d.util);
    g_k.utilBuy = (int)g_tv("util_buy", d.utilBuy);
    g_k.utilThrow = (int)g_tv("util_throw", d.utilThrow);
    g_k.utilLog = (int)g_tv("util_log", d.utilLog);
    g_k.utilTest = (int)g_tv("util_test", d.utilTest);   // test: every bot with util throws it 700 u ahead 3 s into the round
    g_k.utilTeam = (int)g_tv("util_team", d.utilTeam);   // A/B: only this team buys and throws the new way (2 T, 3 CT)
    g_k.move = (int)g_tv("brain_move", d.move);
    g_k.moveCyc = (int)g_tv("move_cycle", d.moveCyc);
    g_k.moveRepo = (int)g_tv("move_repo", d.moveRepo);
    g_k.moveFall = (int)g_tv("move_fall", d.moveFall);
    g_k.moveVary = (int)g_tv("move_vary", d.moveVary);
    g_k.moveTeam = (int)g_tv("move_team", d.moveTeam);   // A/B: only this team moves the new way
    g_k.moveLog = (int)g_tv("move_log", d.moveLog);
    g_k.moveDial = (float)g_tv("move_dial", d.moveDial);   // test: every bot's footwork for P, absolute
    g_k.crouch = (int)g_tv("brain_crouch", d.crouch);
    g_k.crouchSpray = (int)g_tv("crouch_spray", d.crouchSpray);
    g_k.crouchHold = (int)g_tv("crouch_hold", d.crouchHold);
    char k[400];
    snprintf(k, sizeof(k),
             "ctpush %d spot %d | ctset %d focus %d | rotate %d sneaky %d | util %d buy %d throw %d log %d test %d team %d | move %d cycle %d repo %d"
             " fall %d vary %d team %d dial %.2f | crouch %d spray %d hold %d",
             g_k.push, g_k.pushForce, g_k.set, g_k.setFocus, g_k.rot, g_k.rotSneaky, g_k.util, g_k.utilBuy, g_k.utilThrow, g_k.utilLog, g_k.utilTest,
             g_k.utilTeam, g_k.move, g_k.moveCyc, g_k.moveRepo, g_k.moveFall, g_k.moveVary, g_k.moveTeam, g_k.moveDial, g_k.crouch, g_k.crouchSpray,
             g_k.crouchHold);
    static std::string last;
    if (last != k) BLog("job5b hooks: %s", k);
    last = k;
}
// ---- L: CT pushes rare, planned, never aimed at the enemy -----------------------------------------------------------
// His words: "a rush happens, but not every round... two people rushing cat one round, the next two rushing mid, then
// the next two rushing long... if they know where I'm going and they're sending two my way every time, that's super
// predictable." What sent them his way: a holding CT chased every noise it heard by a roll per noise (part C: 35% at elo
// 300 .. 0% at the top - ~22% for a Gold Nova bot, and several bots hear the same runner): InvestigateNoiseState walks it
// straight at the noise; and a CT that finished a trade run was released to the stock AI, which hunts (pushes on). Now:
//   - a holder never chases a noise (part B's hearing turns it to the noise instead; a call still rotates the team);
//   - a CT back from a trade run before the plant holds again (on the called site, else its own spot);
//   - pushes happen only as PLANNED at the round start (fl::PlanPush, from the round number and the team alone - no
//     enemy position, heard or seen, goes in): 10-22% of rounds by team smarts, never two rounds in a row. One CT (two
//     in 30% of pushes) that is not an anchor - the lurker first (part M), else the nearest - walks to a contested spot
//     on a T approach (both teams reach it within 3 s of each other: fl::PushSpotOk on the nav occupy grid; never one
//     of the last two spots), holds it 2.5-5 s and goes back to its spot; a site call, the plant or a fight there (2 s
//     after the last sight) ends it early.
// Proof: "PUSH" per planned push (spot, who, when) and its end (held / call / contact / dead / timeout); part C's "CTR"
// round line gains fwdT (seconds CTs stood off their own ground before the first T reached a site), plannedT (of it, on a
// planned push) and towardT (of it, within 1200 u of a living T - "aimed at him"); "push spots" once a level.
// Switches: brain_ctpush 0 (part C's noise rolls, no planned pushes); test ctpush_spot <k> (a push every round to spot k).
static bool PushOn() { return g_k.push && CtOn(); }
struct PushSpot
{
    Vec p;
    std::string name;
};
static std::vector<PushSpot> g_pushSpots;
static bool g_pushBuilt;
static std::vector<int> g_pushHist;   // this map's pushed spots, newest last
static int g_pushLast = -100;         // the round of the last push
static struct CtPush
{
    int spot = -1;
    std::vector<std::string> who;
    std::map<std::string, Vec> back;   // each pusher's own spot (it goes back there)
    float at = 0, hold = 0, arriveAt = -1, lastSight = -1;
    bool started = false, done = false, contact = false;
} g_push;
static void BuildPushSpots()
{
    g_pushSpots.clear();
    auto add = [](const Vec &v, const std::string &name) {
        for (auto &q : g_pushSpots)
            if (Dist(q.p, v) < 600.f) return;
        if (g_pushSpots.size() < 8) g_pushSpots.push_back({v, name});
    };
    for (auto &s : g_mi.sites)
        for (auto &a : s.ap)
        {
            std::vector<Vec> c = a.wp;
            c.push_back(a.stack);
            c.push_back(a.choke);
            int best = -1, bs = 1 << 30;
            for (size_t i = 0; i < c.size(); i++)
            {
                int oT = OccAt(2, c[i]), oC = OccAt(3, c[i]);
                float d = Dist(c[i], s.c);
                if (!fl::PushSpotOk(oT, oC) || d < s.radius + 250.f || d > 2800.f) continue;
                int sc = fl::PushSpotScore(oT, oC);
                if (sc < bs)
                {
                    bs = sc;
                    best = (int)i;
                }
            }
            if (best >= 0) add(c[best], s.label + "-" + a.name);
        }
    // mid control: the mid area both teams reach closest in time
    int bm = -1, bs = 1 << 30;
    for (size_t i = 0; i < g_mi.midAreas.size(); i++)
    {
        const MidArea &m = g_mi.midAreas[i];
        Vec v{0.5f * (m.x0 + m.x1), 0.5f * (m.y0 + m.y1), m.z};
        int oT = OccAt(2, v), oC = OccAt(3, v);
        if (!fl::PushSpotOk(oT, oC)) continue;
        if (fl::PushSpotScore(oT, oC) < bs)
        {
            bs = fl::PushSpotScore(oT, oC);
            bm = (int)i;
        }
    }
    if (bm >= 0)
    {
        const MidArea &m = g_mi.midAreas[bm];
        add({0.5f * (m.x0 + m.x1), 0.5f * (m.y0 + m.y1), m.z}, "mid-" + m.place);
    }
    std::string list;
    for (auto &q : g_pushSpots) list += (list.empty() ? "" : ", ") + q.name;
    BLog("push spots %s: %d (%s)%s", g_mapName.c_str(), (int)g_pushSpots.size(), list.c_str(),
         g_pushSpots.empty() ? " - no contested ground known (no occupy grid?): no planned pushes on this map" : "");
}
static bool PushPlanned(const std::string &name)
{
    if (g_push.spot < 0 || !g_push.started || g_push.done) return false;
    for (auto &n : g_push.who)
        if (n == name) return true;
    return false;
}
static void PushPlanRound()
{
    g_push = CtPush();
    if (!PushOn() || g_phase != PH_LIVE || !g_matchLive) return;   // no planned pushes in the warmup
    if (!g_pushBuilt)
    {
        BuildPushSpots();
        g_pushBuilt = true;
    }
    auto cts = Team(3, true);
    int n = (int)cts.size();
    fl::PushPlan pp = fl::PlanPush(MeanSmart(cts), g_roundNo - g_pushLast, (int)g_pushSpots.size(), g_pushHist, n, Rnd(), Rnd(), Rnd(), Rnd());
    if (g_k.pushForce >= 0 && g_k.pushForce < (int)g_pushSpots.size())
    {
        pp.target = g_k.pushForce;
        pp.n = 1;
        pp.at = 3.f;
        pp.hold = 4.f;
    }
    if (pp.target < 0) return;
    const Vec v = g_pushSpots[pp.target].p;
    std::vector<Player *> c;
    for (auto *p : cts)
        if (!p->ct.anchor && p->task.kind == T_HOLD) c.push_back(p);
    std::sort(c.begin(), c.end(), [&](Player *a, Player *b) {
        int ra = a->role == "lurk" ? 0 : 1, rb = b->role == "lurk" ? 0 : 1;
        return ra != rb ? ra < rb : Dist(a->pos, v) < Dist(b->pos, v);
    });
    for (int i = 0; i < pp.n && i < (int)c.size(); i++)
    {
        g_push.who.push_back(c[i]->name);
        g_push.back[c[i]->name] = c[i]->task.goal;
    }
    if (g_push.who.empty()) return;
    g_push.spot = pp.target;
    g_push.at = g_liveAt + pp.at;
    g_push.hold = pp.hold;
    g_pushHist.push_back(pp.target);
    if (g_pushHist.size() > 8) g_pushHist.erase(g_pushHist.begin());
    g_pushLast = g_roundNo;
    std::string who;
    for (auto &w : g_push.who) who += (who.empty() ? "" : "+") + w;
    BLog("PUSH map=%s r=%d what=plan spot=%s who=%s in=%.1f hold=%.1f | %s push%s %s %.0f s in (planned at the round start, no enemy position used)",
         g_mapName.c_str(), g_roundNo, g_pushSpots[pp.target].name.c_str(), who.c_str(), pp.at, pp.hold, who.c_str(), g_push.who.size() > 1 ? "" : "es",
         g_pushSpots[pp.target].name.c_str(), pp.at);
}
static void PushEnd(const char *why)
{
    float now = CurTime();
    for (auto &n : g_push.who)
    {
        Player *p = FindPl(n);
        if (!p || !p->alive || !p->bot || p->task.why != "CT push") continue;
        // the plant gives every CT its task; a call moves only the CTs it picks - a pusher it did not pick goes back
        if (!g_plan.planted) Assign(p, T_HOLD, g_push.back[n], 2, 120, "CT back from push", 45);
    }
    g_push.done = true;
    BLog("PUSH map=%s r=%d what=end spot=%s end=%s held=%.1f contact=%d", g_mapName.c_str(), g_roundNo, g_pushSpots[g_push.spot].name.c_str(), why,
         g_push.arriveAt > 0 ? now - g_push.arriveAt : -1.f, (int)g_push.contact);
}
static void CtPushTick(float now)
{
    if (g_push.spot < 0 || g_push.done || g_push.spot >= (int)g_pushSpots.size()) return;
    if (!PushOn()) return PushEnd("switched off");
    if (g_plan.planted || g_plan.hitLevel >= 1) return PushEnd("call");
    if (now < g_push.at) return;
    const Vec &v = g_pushSpots[g_push.spot].p;
    int alive = 0;
    for (auto &n : g_push.who)
    {
        Player *p = FindPl(n);
        if (!p || !p->alive || !p->bot) continue;
        alive++;
        if (!g_push.started) Assign(p, T_HOLD, v, 1, 180, "CT push", 30);
        if (g_push.arriveAt < 0 && Dist(p->pos, v) < 250.f) g_push.arriveAt = now;
        if (now - p->sh.lastSeen < 0.3f)
        {
            g_push.contact = true;
            g_push.lastSight = now;
        }
    }
    g_push.started = true;
    if (!alive) return PushEnd("dead");
    if (g_push.contact && now - g_push.lastSight > 2.f) return PushEnd("contact");
    if (g_push.arriveAt > 0 && now - g_push.arriveAt > g_push.hold && !g_push.contact) return PushEnd("held");
    if (now - g_push.at > 20.f) return PushEnd("timeout");
}
// a CT that finished a trade run before the plant: on the called site (the rotation's back spot), else back to its spot
static bool J5bTradeBack(Player &p)
{
    if (!PushOn() || !p.bot || p.team != 3 || g_plan.planted || p.task.why != "trade") return false;
    const Site *hs = g_plan.hitLevel >= 1 ? FindSite(g_plan.hitSite) : nullptr;
    if (hs)
    {
        Assign(&p, T_HOLD, RotateSpot(*hs, 0), 1, 150, "CT rotate", 30);
        SetRoute(&p, *hs);   // over the CT side, as every rotation
        p.role = "rotate";
        RotStart(&p, hs->label.c_str());   // N: a rotation (logged, never stops for a noise)
    }
    else if (p.ct.lastSpot.x != 0.f || p.ct.lastSpot.y != 0.f)
    {
        Assign(&p, T_HOLD, p.ct.lastSpot, 2, 120, "CT back from trade", 45);
        p.role = p.ct.lastSite.empty() ? "mid" : p.ct.lastSite;
    }
    else
        return false;
    return true;
}
// ---- M: the CT setup ------------------------------------------------------------------------------------------------
// His words: "defenders need to defend on site better. Maybe have one lurker and always have somebody watching mid";
// "there were four people watching mid"; "they really like to hold CT... one bot sat in CT for the last four rounds".
// Why: part C's rotator held the map file's "mid" point - make_mapinfo's halfway point of the walk from site A to site B,
// which on de_dust2 is the CT spawn / mid-doors ground (the "sat in CT"); holds come from the site region (radius + 150)
// so some sat off the site; a bot with a low angles dial kept its spot 80% of rounds; and every CT turned to every noise
// it heard - with him in mid, all of them looked at mid. Now (fl::CtRoles5b):
//   roles  one ANCHOR on each site (always first), ONE mid, ONE lurker (4+ CTs, no stack), the extra man on a site that
//          changes round to round (or the stacked site - part C's stack rolls stay). Anchors = the best angle holders
//          (a bonus for its own site last round, a wide random share), mid = game sense, lurker = aggression.
//   spots  built once a level from the map file + nav hiding spots:
//          site  its holds, post spots and covered hiding spots INSIDE the site (its radius), never in the doorway, never
//                in a mid area, never by the CT spawn (650 u) unless in the site's inner half (fl::SiteSpotOk);
//          mid   a hiding spot on CT ground (the CTs get there 2 s before the Ts) within 1500 u of a mid area that it
//                can SEE (world traces), 650 u+ from the CT spawn, off the sites (fl::MidSpotOk);
//          lurk  a hiding spot on CT ground off the sites and out of mid, 700 u+ from the CT spawn, 500-1600 u from a
//                site's approach choke that it can see (fl::LurkSpotOk) - it watches a way in; it is the first to rotate.
//          Each CT's spot: fl::PickSpot - quality by its dial + a random share, never a spot taken this round, never one
//          used last round while another is free. So the setup changes every round.
//   focus  an anchor turns to a noise only near its own site (its radius + 1300 u) or near itself (1200 u); farther ones
//          it notes (the call still goes to the team) and keeps its angle - no more "four people watching mid".
// Proof: "CTSET" (part C's line, same format) + one "CTSPOT" per CT per round (role, spot, on the site, by the CT spawn,
// in / near mid, the same spot as last round); "ctset spots" once a level; part C's "CTR" gains spawnT (CT-seconds by the
// CT spawn before the first T reached a site, after the first 10 s) and midMax (most CTs near mid at once).
// Switches: brain_ctset 0 (part C's setup), ctset_focus 0 (every CT turns to every noise, part B).
static bool CtSetOn() { return g_k.set && CtOn(); }
static struct CtCands
{
    bool built = false;
    std::vector<std::vector<fl::SpotCand>> site;
    std::vector<fl::SpotCand> mid, lurk;
} g_cc;
static std::vector<Vec> g_spotLast;   // last round's CT spots (all roles)
static int g_extraLast = -1;
static float MidDist(const Vec &p)
{
    float d = 1e9f;
    for (auto &m : g_mi.midAreas) d = std::min(d, fl::RectDist(p, {m.x0, m.y0, m.x1, m.y1, m.z}));
    return d;
}
static bool NearMid(const Vec &p)
{
    if (!g_mi.midAreas.empty()) return MidDist(p) < 300.f;
    return g_mi.hasMid && Dist2(p, g_mi.mid) < 500.f;
}
static void AddCand(std::vector<fl::SpotCand> &v, const Vec &p, float q)
{
    for (auto &c : v)
        if (Dist(c.p, p) < 100.f)
        {
            c.q = std::max(c.q, q);
            return;
        }
    v.push_back({p, q});
}
static void TrimCands(std::vector<fl::SpotCand> &v, size_t n)
{
    std::sort(v.begin(), v.end(), [](const fl::SpotCand &a, const fl::SpotCand &b) { return a.q > b.q; });
    if (v.size() > n) v.resize(n);
}
static void BuildCtCands()
{
    g_cc = CtCands();
    g_cc.built = true;
    const Vec &ct = g_mi.ctspawn;
    for (auto &s : g_mi.sites)
    {
        std::vector<fl::SpotCand> v;
        auto ok = [&](const Vec &p) { return fl::SiteSpotOk(p, s.c, s.radius, s.entry, ct, MidDist(p) < 100.f); };
        for (auto &h : s.hold)
            if (ok(h)) AddCand(v, h, 1.f);
        for (auto &h : s.post)
            if (ok(h)) AddCand(v, h, 0.6f);
        for (auto &sp : g_mi.spots)
            if (!(sp.second & 8) && ok(sp.first)) AddCand(v, sp.first, (sp.second & 1) ? 0.75f : 0.4f);
        TrimCands(v, 16);
        if (v.empty()) v.push_back({s.c, 0.5f});
        g_cc.site.push_back(v);
    }
    auto siteDist = [](const Vec &p, float *rad) {
        float d = 1e9f;
        for (auto &s : g_mi.sites)
            if (Dist2(p, s.c) < d)
            {
                d = Dist2(p, s.c);
                *rad = s.radius;
            }
        return d;
    };
    // mid: CT ground near a mid area it can see (<= 80 candidates, <= 3 traces each)
    std::vector<std::pair<float, Vec>> near;
    for (auto &sp : g_mi.spots)
    {
        if (sp.second & 8) continue;
        float rad = 0, ds = siteDist(sp.first, &rad), dm = g_mi.midAreas.empty() ? (g_mi.hasMid ? Dist2(sp.first, g_mi.mid) : 1e9f) : MidDist(sp.first);
        if (!fl::MidSpotOk(OccAt(2, sp.first), OccAt(3, sp.first), dm, Dist2(sp.first, ct), ds, rad) || (g_mi.midAreas.empty() && dm > 900.f)) continue;
        near.push_back({dm, sp.first});
    }
    std::sort(near.begin(), near.end(), [](const std::pair<float, Vec> &a, const std::pair<float, Vec> &b) { return a.first < b.first; });
    if (near.size() > 80) near.resize(80);
    for (auto &n : near)
    {
        const Vec &p = n.second;
        Vec eye{p.x, p.y, p.z + 64.f};
        std::vector<std::pair<float, Vec>> tg;
        for (auto &m : g_mi.midAreas)
        {
            Vec c{0.5f * (m.x0 + m.x1), 0.5f * (m.y0 + m.y1), m.z + 48.f};
            if (Dist(c, p) < 2500.f && OccAt(2, c) <= OccAt(3, c) + 30) tg.push_back({Dist(c, p), c});   // where the Ts come through
        }
        if (g_mi.midAreas.empty() && g_mi.hasMid) tg.push_back({Dist(g_mi.mid, p), {g_mi.mid.x, g_mi.mid.y, g_mi.mid.z + 48.f}});
        std::sort(tg.begin(), tg.end(), [](const std::pair<float, Vec> &a, const std::pair<float, Vec> &b) { return a.first < b.first; });
        int seen = 0;
        for (size_t i = 0; i < tg.size() && i < 3; i++) seen += Los(eye, tg[i].second) ? 1 : 0;
        if (!seen) continue;
        bool cover = false;
        for (auto &sp : g_mi.spots)
            if (Dist(sp.first, p) < 1.f) cover = (sp.second & 1) != 0;
        AddCand(g_cc.mid, p, 0.4f + 0.1f * seen + (cover ? 0.2f : 0.f));
    }
    TrimCands(g_cc.mid, 10);
    // lurker: CT ground watching an approach choke it can see (<= 80 candidates, 1 trace each)
    std::vector<std::pair<float, std::pair<Vec, Vec>>> lk;
    for (auto &sp : g_mi.spots)
    {
        if (sp.second & 8) continue;
        float rad = 0, ds = siteDist(sp.first, &rad), dc = 1e9f;
        Vec ch{};
        for (auto &s : g_mi.sites)
            for (auto &a : s.ap)
                if (Dist2(sp.first, a.choke) < dc)
                {
                    dc = Dist2(sp.first, a.choke);
                    ch = a.choke;
                }
        if (!fl::LurkSpotOk(OccAt(2, sp.first), OccAt(3, sp.first), ds, rad, MidDist(sp.first) < 150.f, Dist2(sp.first, ct), dc)) continue;
        lk.push_back({dc + ((sp.second & 1) ? 0.f : 400.f), {sp.first, ch}});
    }
    std::sort(lk.begin(), lk.end(), [](const std::pair<float, std::pair<Vec, Vec>> &a, const std::pair<float, std::pair<Vec, Vec>> &b) {
        return a.first < b.first;
    });
    if (lk.size() > 80) lk.resize(80);
    for (auto &l : lk)
    {
        const Vec &p = l.second.first, &ch = l.second.second;
        if (!Los({p.x, p.y, p.z + 64.f}, {ch.x, ch.y, ch.z + 48.f})) continue;
        AddCand(g_cc.lurk, p, 0.6f + (l.first < 1e3f ? 0.2f : 0.f));
    }
    TrimCands(g_cc.lurk, 10);
    std::string sites;
    for (size_t i = 0; i < g_cc.site.size(); i++) sites += (i ? " " : "") + g_mi.sites[i].label + ":" + std::to_string(g_cc.site[i].size());
    BLog("ctset spots %s: sites %s, mid %d, lurker %d%s", g_mapName.c_str(), sites.c_str(), (int)g_cc.mid.size(), (int)g_cc.lurk.size(),
         g_cc.mid.empty() ? " (no mid spot: the extra CTs go to the sites)" : "");
}
static void PlanCT5b()
{
    auto bots = Team(3, true);
    int n = (int)bots.size(), ns = (int)g_mi.sites.size();
    if (!n || !ns) return;
    if (!g_cc.built) BuildCtCands();
    bool smart = MeanSmart(bots) >= 0.35f && n >= 4;
    int stack = g_j.ctStack ? fl::CtStackPick(g_tHits, ns, smart, Rnd(), Rnd()) : -1;
    if (g_j.ctStackForce >= 0 && g_j.ctStackForce < ns) stack = g_j.ctStackForce;
    int extra = rand() % ns;
    if (ns > 1 && extra == g_extraLast && Rnd() < 0.7f) extra = (extra + 1) % ns;   // the extra man mostly changes site
    g_extraLast = extra;
    // JOB 5c Q: 2 - 2 - 1 (the lingerer on mid, the lurk spot 20% of rounds); 5b: A, B, mid, lurker + one extra
    std::vector<int> roles = CtQOn() && g_c.qRoles ? fl::CtRoles5c(n, ns, !g_cc.mid.empty(), !g_cc.lurk.empty(), stack, Rnd() < 0.2f, extra)
                                                   : fl::CtRoles5b(n, ns, !g_cc.mid.empty(), !g_cc.lurk.empty(), stack, extra, rand() % ns);
    std::vector<Player *> left(bots.begin(), bots.end()), who(roles.size(), nullptr);
    auto take = [&](size_t slot, const std::function<float(Player *)> &score) {
        int best = -1;
        float bs = -1e9f;
        for (size_t i = 0; i < left.size(); i++)
        {
            float v = score(left[i]);
            if (v > bs)
            {
                bs = v;
                best = (int)i;
            }
        }
        if (best < 0) return;
        who[slot] = left[best];
        left.erase(left.begin() + best);
    };
    for (size_t i = 0; i < roles.size() && (int)i < ns; i++)   // anchors
    {
        const std::string &l = g_mi.sites[roles[i]].label;
        take(i, [&](Player *p) { return DialOf(*p, D_ANGLES, false) + (p->ct.lastSite == l && p->ct.anchor ? 0.2f : 0.f) + 0.35f * Rnd(); });
    }
    for (size_t i = 0; i < roles.size(); i++)
        if (roles[i] == fl::CR_MID)
            take(i, [&](Player *p) { return DialOf(*p, D_SENSE, false) + 0.35f * Rnd() - (p->ct.lastSite == "mid" ? 0.15f : 0.f); });
    for (size_t i = 0; i < roles.size(); i++)
        if (roles[i] == fl::CR_LURK)
            take(i, [&](Player *p) { return DialOf(*p, D_AGGR, false) + 0.35f * Rnd() - (p->ct.lastSite == "lurk" ? 0.15f : 0.f); });
    for (size_t i = 0; i < roles.size(); i++)
        if (!who[i])
        {
            const std::string &l = g_mi.sites[roles[i]].label;
            take(i, [&](Player *p) { return (p->ct.lastSite == l ? 0.3f : 0.f) + 0.5f * Rnd(); });
        }
    std::vector<Vec> taken;
    std::map<std::string, std::string> line;
    int repeats = 0;
    for (size_t i = 0; i < roles.size(); i++)
    {
        Player *p = who[i];
        if (!p) continue;
        int r = roles[i];
        bool anchor = (int)i < ns, sloppy = !anchor && !TakesBasic(p);
        const std::vector<fl::SpotCand> &c = r >= 0 ? g_cc.site[r] : r == fl::CR_MID ? g_cc.mid : g_cc.lurk;
        std::vector<float> rnd;
        for (size_t k = 0; k < c.size(); k++) rnd.push_back(Rnd());
        int k = fl::PickSpot(c, taken, g_spotLast, sloppy ? 0.f : DialOf(*p, r == fl::CR_MID ? D_SENSE : D_ANGLES, false), rnd);
        Vec spot = k >= 0 ? c[k].p : r >= 0 ? g_mi.sites[r].c : !c.empty() ? c[0].p : g_mi.sites[0].c;
        float q = k >= 0 ? c[k].q : 0.f;
        bool again = false;
        for (auto &h : g_spotLast) again = again || Dist(h, spot) < 120.f;
        repeats += again;
        taken.push_back(spot);
        std::string role = r >= 0 ? g_mi.sites[r].label : r == fl::CR_MID ? "mid" : "lurk";
        const char *why = anchor ? "CT anchor" : r == fl::CR_MID ? "CT mid" : r == fl::CR_LURK ? "CT lurk" : sloppy ? "CT hold (sloppy)" : "CT hold";
        // JOB 5c Q: the fastest way there (5b: the game's safest route - round through A, long and the T side to B)
        Assign(p, T_HOLD, spot, CtQOn() && g_c.qRoute ? 1 : 2, 120, why, sloppy ? 25 : 45);
        // JOB 5c Q: a site holder walks the CT side's waypoints (step 9b's rotation route: CT spawn -> CT mid -> B doors ->
        // B) - with the goal alone the game's planner still took B holders round by A, long and the T side (measured)
        if (CtQOn() && g_c.qRoute && r >= 0) SetRoute(p, g_mi.sites[r]);
        p->role = role;
        p->ct.anchor = anchor;
        p->ct.lastSite = role;
        p->ct.lastSpot = spot;
        p->ct.setSpot = spot;
        p->ct.setRole = role;
        line[role] += p->name + (anchor ? "* " : sloppy ? "~ " : " ");
        const Site *os = r >= 0 ? &g_mi.sites[r] : nullptr;
        BLog("CTSPOT map=%s r=%d bot=%s role=%s anchor=%d q=%.2f onSite=%d nearCT=%d mid=%d again=%d x=%.0f y=%.0f z=%.0f", g_mapName.c_str(), g_roundNo,
             p->name.c_str(), role.c_str(), (int)anchor, q, os ? (int)(Dist2(spot, os->c) <= os->radius) : -1, (int)(Dist2(spot, g_mi.ctspawn) < 650.f),
             (int)(MidDist(spot) < 300.f || r == fl::CR_MID), (int)again, spot.x, spot.y, spot.z);
    }
    g_spotLast = taken;
    std::string out;
    for (auto &kv : line) out += kv.first + "=" + kv.second;
    g_ctm.stack = stack >= 0 ? g_mi.sites[stack].label : "-";
    g_ctm.setup = out;
    std::string reads;
    for (int h : g_tHits) reads += h >= 0 && h < ns ? g_mi.sites[h].label : "-";
    BLog("CTSET map=%s r=%d n=%d stack=%s reads=%s | %s(* anchor, ~ sloppy) repeats=%d", g_mapName.c_str(), g_roundNo, n, g_ctm.stack.c_str(),
         reads.empty() ? "-" : reads.substr(reads.size() > 6 ? reads.size() - 6 : 0).c_str(), out.c_str(), repeats);
}
// an anchor keeps its angle on a noise far from its site and from itself (it still calls it)
static bool CtFocusSkip(const Player &b, const Vec &noise)
{
    if (!CtSetOn() || !g_k.setFocus || b.team != 3 || !b.ct.anchor || g_plan.planted) return false;
    const Site *s = FindSite(b.role);
    if (!s) return false;
    return Dist(noise, s->c) > s->radius + 1300.f && Dist(noise, b.fpos) > 1200.f;
}
// ---- N: rotations -----------------------------------------------------------------------------------------------------
// His words: "bomb planted on A, the bot on B not doing anything... finally starts rotating after the bomb's gonna blow
// up. It almost looks like he's only looking for exit frags... wanted to rotate through CT but then stopped because it saw
// me." Why: after the plant a CT that failed its roll (Takes: 0.3 + 0.8 x smarts - 40% of Gold Nova CTs) was released
// to the stock AI, whose "find the bomb" task searches the sites one by one (its own site first); on a call a sloppy CT
// sometimes stayed put and the rest left 0.5-4 s late; a rotating CT below 0.6 smarts that heard a noise was left to
// chase it (InvestigateNoiseState), and after a fight Steer took up to 1 s to send it on. Now:
//   - on a call every CT but the other sites' anchors (part C) rotates, leaving 1.0 .. 0.2 s after the call (fl::RotateDelay);
//   - after the plant EVERY CT gets its retake job (a failed roll = up to 1.5 s late, never released);
//   - a rotating / retaking CT is never left to chase a noise and is back on its way 0.25 s after a fight;
//   - sneaky when needed: with an enemy the team knows about (seen / heard, last 8 s) within 900 u of the way still
//     ahead (fl::NeedSneaky), its move takes the game's safest route (it avoids where the team died) instead of the
//     fastest - on top of step 9b's CT-side waypoints;
//   - the retake's "first one waits for the next" (part F8) only when it is not in contact (2 s since it saw an enemy).
// Proof (on and off alike): one "ROT" line per rotation / retake move - why, where to, the delay it was given, when it
// arrived, seconds it stood still on the way without fighting (stall), fights on the way, sneaky, how it ended; part F's
// "RETAKE" line gains joined (CTs given a retake job) and aliveAtPlant.
// Switches: brain_rotate 0 (step 9b / part C timing and rolls), rotate_sneaky 0.
static bool RotOn() { return g_k.rot && g_steer && g_layout == 1 && g_mi.ok && !g_mi.hostage && g_modeOk; }
static bool RotWhy(const std::string &w)
{
    return w == "CT help" || w == "CT rotate" || w == "CT re-rotate" || w == "CT retake" || w == "CT gather for retake";
}
static bool RotUrgent(const Player &p) { return RotOn() && p.bot && p.team == 3 && RotWhy(p.task.why); }
static void RotLog(Player &p, const char *end, float now)
{
    if (!p.rt.on) return;
    p.rt.on = false;
    std::string why = p.rt.why, e = end;
    std::replace(why.begin(), why.end(), ' ', '_');   // one word per value (the analyzers split on spaces)
    std::replace(e.begin(), e.end(), ' ', '_');
    BLog("ROT map=%s r=%d bot=%s elo=%.0f on=%d why=%s to=%s delay=%.1f arrive=%.1f stall=%.1f fights=%d sneaky=%d end=%s", g_mapName.c_str(), g_roundNo,
         p.name.c_str(), p.elo, (int)RotOn(), why.c_str(), p.rt.to.c_str(), p.rt.startAt - p.rt.at, !strcmp(end, "arrived") ? now - p.rt.at : -1.f,
         p.rt.stall, p.rt.fights, (int)p.rt.sneaky, e.c_str());
}
static void RotStart(Player *p, const char *to)
{
    if (!p || !p->bot) return;
    float now = CurTime();
    if (p->rt.on) RotLog(*p, "replaced", now);
    p->rt = Player::Rt();
    p->rt.on = true;
    p->rt.bomb = !strcmp(to, "bomb");
    p->rt.why = p->task.why;
    p->rt.to = to;
    p->rt.at = now;
    p->rt.startAt = p->task.startAt;
}
static void RotFrame(float now)
{
    for (auto &kv : g_pl)
    {
        Player &p = kv.second;
        if (!p.rt.on) continue;
        if (!p.alive || !p.ent)
        {
            RotLog(p, "dead", now);
            continue;
        }
        const Site *s = p.rt.bomb ? nullptr : FindSite(p.rt.to);
        bool arrived = p.rt.bomb ? g_plan.planted && Dist(p.pos, g_plan.bomb) < 900.f : s && Dist(p.pos, s->c) < s->radius + 150.f;
        if (arrived)
        {
            RotLog(p, "arrived", now);
            continue;
        }
        if (now < p.rt.startAt) continue;
        bool att = BotAttacking(p.ent);
        if (att && !p.rt.inFight) p.rt.fights++;
        p.rt.inFight = att;
        // stood still on the way, not fighting (a gather hold at its point is waiting by design, not a stall)
        bool atGoal = p.task.kind == T_HOLD && Dist(p.pos, p.task.goal) < p.task.arrive + 60.f;
        if (!att && !atGoal && p.speed < 50.f) p.rt.stall += 0.25f;
    }
}
// the move's route: the game's safest when an enemy the team knows about is near the way still ahead
static bool URunOn();   // JOB 5c U (below)
static int RotRoute(Player &p, const Vec &goal, int route)
{
    if (!RotUrgent(p) || !g_k.rotSneaky || route == 2) return route;
    if (URunOn()) return route;   // JOB 5c U: the direct way - the safest route was the long way round (median 16-17 s vs 2-14 s)
    float now = CurTime();
    std::vector<Vec> known;
    for (auto &kv : g_intel[p.team])
    {
        Player *e = FindPl(kv.first);
        if (e && e->alive && now - kv.second.t < 8.f && now >= kv.second.t) known.push_back(kv.second.pos);
    }
    if (known.empty()) return route;
    std::vector<Vec> ahead = {p.pos};
    for (size_t i = p.task.wp; i < p.task.via.size(); i++) ahead.push_back(p.task.via[i]);
    ahead.push_back(p.task.goal);
    (void)goal;
    if (!fl::NeedSneaky(ahead, known)) return route;
    if (!p.rt.sneaky && p.rt.on) BLog("ROT map=%s r=%d bot=%s sneaky | an enemy is known near its way to %s - the safest route", g_mapName.c_str(), g_roundNo,
                                      p.name.c_str(), p.rt.to.c_str());
    p.rt.sneaky = true;
    return 2;
}
// ---- O: utility, every round -------------------------------------------------------------------------------------------
// His words: "No correct mollies... no correct smokes... We need flashes, smokes, mollies, grenades being used just about
// every single round by at least four out of five bots. If they can afford util they should be buying it like a normal
// player. If they have bought util they should be using it... not all at the same time."
// Why: the stock bot buys one grenade only now and then from leftover money (sv_bot_buy_grenade_chance) and throws it only
// at an enemy it has just seen or heard; nothing ever threw a smoke for a site take, a molotov on a corner or a flash
// before an entry. Now:
//   BUY   a moment into the freeze (after the bots' own buys) each bot buys what a player buys with the money left after
//         its gun and armour (fl::UtilBuyList: smoke, flash, fire, 2nd flash, HE; how many by its utility dial - 1 for a
//         Silver, 2 for a Gold Nova, 4 at the top). The lever: the game's own "buy <item>" client command run for the bot
//         (IServerGameClients::ClientCommand - what the engine calls when a player types a command; SourceMod's
//         FakeClientCommand does the same for bots), so the game's buy zone / buy time / money / limits all apply, it
//         takes the money itself and logs "purchased" + "money change" lines (step 10's money check sees a normal buy;
//         income never changes). Proof: the "purchased" line - two buys never seen (and none ever seen) = buying off.
//   THROW each throw is a job: switch to the grenade (the game's "use weapon_x" console command run for the bot, as the
//         chat lever runs say_team), aim (the look controller, top priority but the veto; the bot stands still), pull
//         the pin (the grenade's own m_bPinPulled + m_flThrowStrength, by send-table name, the same offsets in every
//         grenade class) - the game's grenade code then throws it with the bot's view the next frame the bot is not
//         pressing attack, exactly as a player's release. Stuck for 0.6 s: its throw time is set (the second branch
//         of the same code). The arc is solved from where the bot stands: the flight model (fl::ThrowArc, part F9) +
//         world traces along it (fl::ThrowPitches: a flat throw or a lob landing on the target; fl::AirPitch: a flash /
//         HE popping near it at 1.5 s); no arc within the kind's tolerance = no throw. A lower rank aims worse (up to 3
//         deg off at 0 .. 0.3 at 1, by its utility dial).
//   WHEN  T execute (part D's called hit): as the group goes to the entrance, 1-2 smokes on the CT side of the site (the
//         CT rotation's last waypoint into it, the deepest hold) and a molotov on the first corner (the hold nearest the
//         way in), thrown one after another from the entrance; the go-in waits for the smokes (up to 7 s); then 1-2
//         flashes pop deep over the site and the group steps in once they popped (fl::ExecuteJobs / ExecuteFlashes).
//         Post-plant: a smoke on the retake's way in; a molotov / HE on a CT who starts to defuse.
//         CT: when its site is under threat (a call, or enemies the team knows about near it): Ts still outside -> smoke
//         their way in, then a molotov on it, then HE; at the entry -> a flash on them, then HE (fl::DefendKind). A
//         retaker on the way in: a flash over the bomb, a smoke on a post spot, a molotov on another (never the bomb).
//         Both: 2+ enemies known together -> HE / molotov on them; a holder that knows of an enemy it cannot see ->
//         sometimes a flash before it peeks; late in the round what is left goes at known enemies.
//         Never all at once: a team's throws are spread out (fl::UtilGap), one job per bot, never while fighting, never
//         on a teammate or at its own feet (part F9's check), never while planting / defusing.
// Proof: "util levers" (load), "UTILBUY" per bot per round, "UTIL" per throw (kind, job, target, distance, the arc's
// model miss, result thrown / aborted + why), "UTILHIT" per detonation of our throw (how far from the target), "UTILR" per
// team per round (bots, with util at the start, threw, throws by kind) - the local test's "util used per bot per round".
// Switches: brain_util 0 (all of O: the stock bots' own buys and throws only), util_buy 0, util_throw 0, util_log <n>
// (UTIL lines a round); test util_test 1 (every bot with util throws it 700 u ahead 3 s into the round - the lever check).
static bool UtilOn() { return g_k.util && g_steer && g_layout == 1 && g_mi.ok && g_modeOk && g_np.ok; }   // g_np.ok: UtilFrame runs
static bool UtilTeamOk(int team) { return !g_k.utilTeam || g_k.utilTeam == team; }
static struct Cmds
{
    int state = 0;             // 0 not looked for, 1 ready, -1 off
    std::string why;
    uintptr_t cmdClient = 0;   // the engine's command-client global (as the chat lever finds it)
    uintptr_t useCb = 0;       // the "use" ConCommand's callback: select a weapon by name
    uintptr_t buyCb = 0;       // a "buy" ConCommand, if this build has one (else ClientCommand)
    uintptr_t backCb = 0;      // the "lastinv" ConCommand: back to the weapon it held before (the gun, after an aborted throw)
    uintptr_t clientCmd = 0;   // IServerGameClients::ClientCommand
    void *sgc = nullptr;
    int buySent = 0, buyOk = 0, buyFail = 0, useSent = 0, useOk = 0, pinOk = 0, pinTries = 0, forced = 0;
    int useFail = 0, backSent = 0, backOk = 0, backFail = 0;
    int buyRound = -1, buyRounds = 0, pinRound = -1, pinRounds = 0, useRound = -1, useRounds = 0, backRound = -1, backRounds = 0;   // rounds with a failure (an off needs 2+)
    bool buyOff = false, pinOff = false, useOff = false, backOff = false;
    std::string useHow = "none", buyHow = "none";
} g_cmd;
// a failure of a lever counted once per round: a lever goes off only after failures in 2+ rounds
static void UtilFailRound(int &round, int &rounds)
{
    if (round != g_roundNo)
    {
        round = g_roundNo;
        rounds++;
    }
}
struct PendBuy
{
    std::string bot, item;
    float at;
};
static std::vector<PendBuy> g_pendBuy;
// the ConCommand named `name` in server.so's data: exactly one object whose vtable is ConCommand's and whose name pointer
// points at that string; its callback in server.so's code with the plain-CCommand flag (as ChatInit checks say_team)
static uintptr_t FindConCmd(const char *name, const std::vector<MapRange> &mine, const std::vector<MapRange> &data,
                            const std::function<bool(uintptr_t)> &inCode, bool voidOk = false)
{
    std::string pat = std::string(1, '\0') + name + std::string(1, '\0');
    std::vector<uintptr_t> strs;
    for (auto &m : mine)
    {
        if (m.perm.size() < 2 || m.perm[0] != 'r' || m.perm[1] == 'w') continue;
        const char *lo = (const char *)m.lo, *hi = (const char *)m.hi, *q = lo;
        while (q && q < hi && strs.size() < 256)
        {
            q = (const char *)memmem(q, hi - q, pat.data(), pat.size());
            if (!q) break;
            strs.push_back((uintptr_t)q + 1);
            q++;
        }
    }
    if (strs.empty()) return 0;
    std::sort(strs.begin(), strs.end());
    std::vector<uintptr_t> objs;
    for (auto &m : data)
        for (uintptr_t pg = m.lo & ~(uintptr_t)4095; pg < m.hi; pg += 4096)
        {
            uintptr_t words[1024];
            if (!SafeRead((void *)pg, words, sizeof(words))) continue;
            for (int i = 0; i < 1024; i++)
            {
                uintptr_t a = pg + 4 * i;
                if (a < m.lo + 12 || a + 4 > m.hi || !std::binary_search(strs.begin(), strs.end(), words[i])) continue;
                uintptr_t obj = a - 12, vt;
                if (SafeWord(obj, vt) && VtName(vt) == "10ConCommand") objs.push_back(obj);
            }
        }
    if (objs.size() != 1) return 0;
    uintptr_t cb = 0;
    uint8_t bits = 0;
    // bit 1: the CCommand callback; bit 2: a callback interface (never ours to call); with voidOk a no-argument one too
    // (called with the command as an extra argument - the caller cleans the stack, so it is ignored)
    if (!SafeWord(objs[0] + 24, cb) || !SafeRead((void *)(objs[0] + 32), &bits, 1) || !inCode(cb) || ((bits & 6) != 2 && !(voidOk && (bits & 6) == 0)))
        return 0;
    return cb;
}
static void CmdInit(BrainIfaceFn serverFactory)
{
    g_cmd = Cmds();
    auto off = [](const char *why) {
        g_cmd.state = -1;
        g_cmd.why = why;
        BLog("util levers: %s - the bots buy and throw grenades only the stock way", why);
    };
    if (!serverFactory) return off("no server factory");
    std::vector<MapRange> maps = ReadMaps();
    std::string path;
    for (auto &m : maps)
        if ((uintptr_t)serverFactory >= m.lo && (uintptr_t)serverFactory < m.hi) path = m.path;
    if (path.empty()) return off("server.so not found in /proc/self/maps");
    std::vector<MapRange> mine, data;
    bool chain = false;
    uintptr_t total = 0;
    for (auto &m : maps)
    {
        bool rw = m.perm.size() >= 2 && m.perm[0] == 'r' && m.perm[1] == 'w';
        if (m.path == path)
        {
            mine.push_back(m);
            chain = rw;
            if (rw && total < (64u << 20)) { data.push_back(m); total += m.hi - m.lo; }
        }
        else if (chain && m.path.empty() && rw && total < (64u << 20))
        {
            data.push_back(m);   // server.so's .bss: only the first anonymous region right after its data
            total += m.hi - m.lo;
            chain = false;
        }
        else
            chain = false;
    }
    std::function<bool(uintptr_t)> inCode = [&](uintptr_t a) {
        for (auto &m : mine)
            if (a >= m.lo && a < m.hi && m.perm.size() >= 3 && m.perm[2] == 'x') return true;
        return false;
    };
    void *sgc = serverFactory("ServerGameClients004", nullptr);
    if (!sgc) sgc = serverFactory("ServerGameClients003", nullptr);
    uintptr_t vt = 0;
    if (!sgc || !SafeWord((uintptr_t)sgc, vt)) return off("no ServerGameClients interface");
    int found = 0, slot = -1;
    unsigned g = 0;
    std::vector<uintptr_t> fns;
    for (int i = 0; i < 48; i++)
    {
        uintptr_t fn;
        if (!SafeWord(vt + 4 * i, fn) || !inCode(fn)) break;
        fns.push_back(fn);
        unsigned char code[24];
        unsigned a = 0;
        if (SafeRead((void *)fn, code, sizeof(code)) && fl::SetterPattern(code, sizeof(code), &a))
        {
            found++;
            g = a;
            slot = i;
        }
    }
    if (found != 1) return off(found ? "more than one one-line setter in ServerGameClients" : "SetCommandClient not found in ServerGameClients");
    bool gOk = false;
    for (auto &m : data) gOk = gOk || (g >= m.lo && g + 4 <= m.hi);
    int cur = -99;
    if (!gOk || !SafeRead((void *)(uintptr_t)g, &cur, 4) || cur < -1 || cur > 64) return off("the command-client global does not hold a player index");
    g_cmd.cmdClient = g;
    g_cmd.sgc = sgc;
    // ClientCommand sits right before SetCommandClient in the interface (GetPlayerLimits, ClientConnect, ClientActive,
    // ClientFullyConnect, ClientDisconnect, ClientPutInServer, ClientCommand, SetCommandClient, ...): trusted only when the
    // setter was found at exactly slot 7 - the two facts must agree
    if (slot == 7 && fns.size() > 7 && fns[6] != fns[7] && fns[6] != fns[5]) g_cmd.clientCmd = fns[6];
    g_cmd.useCb = FindConCmd("use", mine, data, inCode);
    g_cmd.buyCb = FindConCmd("buy", mine, data, inCode);   // a ConCommand "buy" would do too (the command client is set)
    g_cmd.backCb = FindConCmd("lastinv", mine, data, inCode, true);
    g_cmd.useHow = g_cmd.useCb ? "ConCommand" : g_cmd.clientCmd ? "ClientCommand" : "none";
    g_cmd.buyHow = g_cmd.buyCb ? "ConCommand" : g_cmd.clientCmd ? "ClientCommand" : "none";
    g_cmd.state = (g_cmd.useCb || g_cmd.buyCb || g_cmd.clientCmd) ? 1 : -1;
    BLog("util levers: use=%s buy=%s back=%s (SetCommandClient slot %d, ClientCommand %s, command client %p) grenade pin %s", g_cmd.useHow.c_str(),
         g_cmd.buyHow.c_str(), g_cmd.backCb ? "lastinv" : "none (the stock AI takes its gun back)", slot, g_cmd.clientCmd ? "slot 6" : "-",
         (void *)(uintptr_t)g, g_np.pin > 0 ? "found" : "missing");
}
// run a console / client command as if the bot typed it: a ConCommand of that name (the command client set to the bot),
// else IServerGameClients::ClientCommand(its edict, the command). 0 = no lever.
static void MakeCmd(CCmd &c, const std::vector<std::string> &args)
{
    memset(&c, 0, sizeof(c));
    std::string all;
    for (size_t i = 0; i < args.size(); i++) all += (i ? " " : "") + args[i];
    snprintf(c.argS, sizeof(c.argS), "%s", all.c_str());
    c.argv0Size = (int)args[0].size() + 1;   // ArgS() = what follows the command and its space
    size_t off = 0;
    for (size_t i = 0; i < args.size() && i < 8; i++)
    {
        if (off + args[i].size() + 1 > sizeof(c.argvBuf)) break;
        memcpy(c.argvBuf + off, args[i].c_str(), args[i].size() + 1);
        c.argv[i] = c.argvBuf + off;
        off += args[i].size() + 1;
        c.argc = (int)i + 1;
    }
}
static int RunAs(Player &b, const std::vector<std::string> &args, uintptr_t conCb)
{
    if (g_cmd.state != 1 || !b.bot || !b.e || !g_cmd.cmdClient || args.empty()) return 0;
    int idx = EdictIndexOf(b.e);
    if (idx < 1) return 0;
    static CCmd cmd;
    MakeCmd(cmd, args);
    int *cc = (int *)g_cmd.cmdClient;
    int old = *cc, how = 0;
    *cc = idx - 1;   // the engine's command client = the bot's client slot
    if (conCb)
    {
        ((void (*)(const CCmd &))conCb)(cmd);
        how = 1;
    }
    else if (g_cmd.clientCmd)
    {
        ((void (*)(void *, edict_t *, const CCmd &))g_cmd.clientCmd)(g_cmd.sgc, b.e, cmd);
        how = 2;
    }
    *cc = old;
    return how;
}
// -- the weapons: an EHANDLE -> its entity through the edict array (index checked against the edict's own), the kind by RTTI
static uintptr_t EntByHandle(uint32_t h, const Player &any)
{
    if (h == 0xFFFFFFFFu || h == 0 || !any.e) return 0;
    int me = EdictIndexOf(any.e);
    if (me < 1 || !g_chat.stride) return 0;
    uintptr_t base = (uintptr_t)any.e - (uintptr_t)me * g_chat.stride;
    int idx = (int)(h & 0x7FF);
    if (idx <= 0) return 0;
    uintptr_t e = base + (uintptr_t)idx * g_chat.stride, unk = 0;
    int16_t ei = 0;
    if (!SafeRead((void *)(e + 6), &ei, 2) || ei != idx || !SafeWord(e + 12, unk) || !unk) return 0;
    return unk;
}
static int WeaponKind(uintptr_t w, std::string *cls = nullptr)
{
    uintptr_t vt;
    if (!w || !SafeWord(w, vt)) return -2;
    static std::map<uintptr_t, std::pair<int, std::string>> cache;
    auto it = cache.find(vt);
    if (it == cache.end())
    {
        std::string n = VtName(vt);
        int k = n.empty() || n.find("Projectile") != std::string::npos ? -2 : fl::UtilKindOf(n.c_str());
        it = cache.emplace(vt, std::make_pair(k, n)).first;
    }
    if (cls) *cls = it->second.second;
    return it->second.first;
}
static int g_invCheck;   // m_hMyWeapons checked against m_hActiveWeapon on live bots (3 agreeing = ok)
// what it carries (grenades by kind, a flashbang pair counts once); false = unknown
static bool Carried(Player &b)
{
    for (int k = 0; k < fl::UK_N; k++) b.ut.have[k] = 0;
    if (!b.ent || g_np.myWeapons <= 0 || g_np.activeWeapon <= 0 || g_invCheck < 0) return false;
    uint32_t hs[64], act = 0;
    if (!SafeRead((void *)(b.ent + g_np.myWeapons), hs, sizeof(hs)) || !SafeRead((void *)(b.ent + g_np.activeWeapon), &act, 4)) return false;
    bool actIn = false;
    int n = 0;
    for (int i = 0; i < 64; i++)
    {
        if (hs[i] == 0xFFFFFFFFu || hs[i] == 0) continue;
        n++;
        actIn = actIn || hs[i] == act;
        std::string cls;
        int k = WeaponKind(EntByHandle(hs[i], b), &cls);
        if (k >= 0)
        {
            b.ut.have[k]++;
            if (k == fl::UK_FIRE) b.ut.item = cls.find("Incendiary") != std::string::npos ? "weapon_incgrenade" : "weapon_molotov";
        }
    }
    if (g_invCheck >= 0 && g_invCheck < 3 && n > 0 && act != 0xFFFFFFFFu)
    {
        if (actIn) g_invCheck++;
        else
        {
            g_invCheck = -1;
            BLog("util: the carried-weapons list does not hold the weapon in hand - inventory unknown (buys by money only)");
            return false;
        }
        if (g_invCheck == 3) BLog("util: carried weapons check out (m_hMyWeapons holds m_hActiveWeapon on 3 live bots)");
    }
    return g_invCheck >= 3;
}
static int ActiveKind(Player &b, uintptr_t *w)
{
    *w = 0;
    uint32_t h;
    if (!b.ent || g_np.activeWeapon <= 0 || !SafeRead((void *)(b.ent + g_np.activeWeapon), &h, 4)) return -2;
    *w = EntByHandle(h, b);
    return WeaponKind(*w);
}
static const char *UtilItem(const Player &b, int k)
{
    switch (k)
    {
    case fl::UK_SMOKE: return "weapon_smokegrenade";
    case fl::UK_FLASH: return "weapon_flashbang";
    case fl::UK_HE: return "weapon_hegrenade";
    case fl::UK_FIRE: return !b.ut.item.empty() ? b.ut.item.c_str() : b.team == 3 ? "weapon_incgrenade" : "weapon_molotov";
    }
    return "weapon_decoy";
}
static float UtilDial(const Player &p) { return DialOf(p, D_UTIL, false); }
// -- buying ------------------------------------------------------------------------------------------------------------
static float g_utFreezeAt = -100;
static int g_utLogged;
static void UtilFreezeTick(float now)
{
    if (!UtilOn() || !g_k.utilBuy || g_cmd.state != 1 || g_cmd.buyOff || !g_moneyOff || !g_matchLive) return;
    bool freeze = g_phase == PH_FREEZE && now - g_utFreezeAt >= 2.0f && now - g_utFreezeAt < 60.f;
    bool early = g_phase == PH_LIVE && now - g_liveAt < 2.0f;   // a short freeze: the buy time runs on into the round
    if (!freeze && !early) return;
    for (auto &kv : g_pl)
    {
        Player &b = kv.second;
        if (!b.bot || !b.alive || !b.ent || b.ut.bought || (b.team != 2 && b.team != 3) || !UtilTeamOk(b.team)) continue;
        b.ut.bought = true;
        bool inv = Carried(b);
        int money = *(int *)(b.ent + g_moneyOff);
        if (money < 0 || money > 16000) continue;
        std::vector<int> list = fl::UtilBuyList(money, b.team, UtilDial(b), b.ut.have);
        std::string got;
        for (int k : list)
        {
            const char *item = fl::UtilBuyName(k, b.team);
            if (!RunAs(b, {"buy", item}, g_cmd.buyCb)) break;
            g_cmd.buySent++;
            b.ut.buys++;
            g_pendBuy.push_back({b.name, item, now});
            got += std::string(got.empty() ? "" : ",") + fl::UtilName(k);
        }
        std::string had;
        for (int k = 0; k < fl::UK_N; k++)
            if (b.ut.have[k]) had += std::string(had.empty() ? "" : ",") + fl::UtilName(k);
        BLog("UTILBUY map=%s r=%d bot=%s team=%s elo=%.0f dial=%.2f money=%d inv=%d had=%s buys=%s", g_mapName.c_str(), g_roundNo + 1, b.name.c_str(),
             b.team == 2 ? "T" : "CT", b.elo, UtilDial(b), money, (int)inv, had.empty() ? "-" : had.c_str(), got.empty() ? "-" : got.c_str());
    }
}
// a "purchased" line of the server log: one of our buys arrived
static void UtilPurchased(const std::string &name, const char *item)
{
    for (auto it = g_pendBuy.begin(); it != g_pendBuy.end(); ++it)
        if (it->bot == name && strstr(item, it->item.c_str()))
        {
            if (!g_cmd.buyOk++) BLog("util: buy lever proven - %s bought %s (the server log's purchase line)", name.c_str(), item);
            g_pendBuy.erase(it);
            return;
        }
}
static void UtilBuyCheck(float now)
{
    for (auto it = g_pendBuy.begin(); it != g_pendBuy.end();)
    {
        if (now - it->at < 4.f && now >= it->at) { ++it; continue; }
        g_cmd.buyFail++;
        UtilFailRound(g_cmd.buyRound, g_cmd.buyRounds);
        it = g_pendBuy.erase(it);
        if (!g_cmd.buyOk && g_cmd.buyFail >= 4 && g_cmd.buyRounds >= 2 && !g_cmd.buyOff)
        {
            g_cmd.buyOff = true;
            BLog("util: %d buys sent, none showed up in the server log - buying off (the stock buys only)", g_cmd.buyFail);
        }
    }
}
// -- aiming: the arc from where it stands (flight model + world traces) ----------------------------------------------------
static float ArcHit(const Vec &eye, float pitch, float yaw, float fuse, Vec *hit)
{
    std::vector<Vec> a = fl::ThrowArc(eye, pitch, yaw, 0.06f, fuse);
    for (size_t i = 0; i + 1 < a.size(); i++)
    {
        float fr = TraceFracM(a[i], a[i + 1], 0x400b);
        if (fr >= 0.f && fr < 0.999f)
        {
            *hit = {a[i].x + (a[i + 1].x - a[i].x) * fr, a[i].y + (a[i + 1].y - a[i].y) * fr, a[i].z + (a[i + 1].z - a[i].z) * fr};
            return (i + fr) * 0.06f;   // when it hits (s)
        }
    }
    *hit = a.back();
    return -1.f;   // still in the air at the fuse
}
static bool UtilSolve(Player &b, int kind, const Vec &target, float *pitch, float *yaw, Vec *land, float *miss)
{
    if (g_traceOk < 0 || !g_trace) return false;
    Vec eye = EyeOf(b.ent, b.fpos);
    *yaw = YawTo(eye, target);
    bool air = kind == fl::UK_FLASH || kind == fl::UK_HE;
    std::vector<float> base;
    if (air)
    {
        float m;
        base.push_back(fl::AirPitch(eye, target, 1.5f, &m));
    }
    else
    {
        float pt[2];
        int n = fl::ThrowPitches(eye, target, pt);
        for (int i = 0; i < n; i++) base.push_back(pt[i]);
    }
    float bd = 1e9f;
    for (float p0 : base)
        for (float dp : {0.f, -1.5f, 1.5f, -3.f, 3.f})
        {
            float p = p0 + dp;
            if (p < -85.f || p > 85.f) continue;
            Vec h;
            float t = ArcHit(eye, p, *yaw, air ? 1.5f : 3.5f, &h);
            if (!air && t < 0) continue;   // never came down within 3.5 s
            float d = Dist(h, target) + (air ? 0.f : fabsf(h.z - target.z) > 120.f ? 400.f : 0.f);
            if (d < bd)
            {
                bd = d;
                *pitch = p;
                *land = h;
            }
        }
    *miss = bd;
    return bd < fl::UtilTol(kind);
}
// -- jobs ---------------------------------------------------------------------------------------------------------------
struct UtilThrowRec
{
    std::string who, job;
    int kind, team;
    Vec target;
    float at;
    bool matched = false;
};
static std::vector<UtilThrowRec> g_uthrows;
static struct UtilTeam
{
    float nextAt = 0;
    bool execDone = false, goDone = false, plantSmoke = false, defuseFire = false, testDone = false;
    float goAt = 0;
    int aways = 0;   // bots that turned away from our own flash
    std::vector<Vec> smokeT, fireT;
    Vec flashT{};
} g_ut[4];
static bool UtilBusy(const Player &p)
{
    float now = CurTime();
    return p.ut.phase != 0 && now >= p.ut.at && now < p.ut.deadline && p.ut.waitR <= 0.f;
}
// the throw levers all there (not switched off by their own checks)
static bool UtilThrowOk() { return UtilOn() && g_k.utilThrow && g_cmd.state == 1 && g_np.pin > 0 && !g_cmd.pinOff && !g_cmd.useOff; }
// may it stop for a throw: a state step 9 steers (or a fight), not defusing, not escaping the blast, not the bomber about
// to plant (a retaker on the stock "find the bomb" task may throw - that is the retake's util)
static bool UtilCanAct(const Player &b)
{
    if (!b.ent || !J5State(b)) return false;
    int task = BotTask(b.ent);
    return task != 3 && task != 9 && task != 15 && !(b.name == g_bomber && (g_plan.executed || task == 1));
}
static bool UtilStill(const Player &p) { return p.ut.phase >= 2; }
// the arc from where it stands, and the landing checked: not on a teammate or itself (part F9's rule, fire / HE); a flash
// may pop in front of the team - they turn away from it (UtilAway). nullptr = fine, else why not
static const char *UtilCheck(Player &b, int kind, const Vec &target, float *pitch, float *yaw, Vec *land, float *miss)
{
    if (!UtilSolve(b, kind, target, pitch, yaw, land, miss)) return "no arc";
    std::vector<Vec> mates;
    for (auto &kv : g_pl)
        if (kv.second.alive && kv.second.team == b.team && kv.second.name != b.name) mates.push_back(kv.second.pos);
    int f = fl::NadeDanger(*land, b.fpos, mates, {}, false, false);
    if ((kind == fl::UK_FIRE || kind == fl::UK_HE) && (f & (fl::ND_TEAM | fl::ND_SELF))) return "teammate";
    if (Dist2(*land, b.fpos) < 300.f && kind != fl::UK_FLASH) return "own feet";
    return nullptr;
}
static bool UtilAssign(Player &b, int kind, const Vec &target, const char *job, float delay, const Vec *waitAt = nullptr, float waitR = 0)
{
    if (!UtilThrowOk()) return false;
    if (b.ut.phase || !b.falive || !b.ent || kind < 0 || (b.ut.haveOk && b.ut.have[kind] <= 0) || !UtilTeamOk(b.team)) return false;
    float now = CurTime();
    Player::Ut &u = b.ut;
    if (now < u.cool) return false;   // it just gave one up
    // thrown from where it stands: the arc and the landing are checked before it takes the grenade out (a throw it cannot
    // make is never started - no switching back and forth); from somewhere else: checked there
    Vec eye = EyeOf(b.ent, b.fpos);
    if (waitR <= 0.f)
    {
        float pi, ya, mi;
        Vec la;
        if (UtilCheck(b, kind, target, &pi, &ya, &la, &mi))
        {
            u.cool = now + 3.f;
            return false;
        }
        u.pitch = pi;
        u.yaw = ya;
        u.land = la;
        u.miss = mi;
    }
    u.phase = 1;
    u.kind = kind;
    u.job = job;
    u.target = target;
    u.at = now + delay;
    u.deadline = u.at + (waitR > 0 ? 12.f : 5.f);
    u.tries = u.steady = 0;
    u.nextTry = 0;
    u.equipAt = u.pinAt = u.relAt = -1;
    u.forced = false;
    u.waitR = waitR;
    if (waitAt) u.waitAt = *waitAt;
    u.solvedFrom = waitR <= 0.f ? eye : Vec{0, 0, -1e9f};
    // a lower rank aims worse: up to 3 deg off at the bottom of its utility dial, 0.3 at the top
    float e = Lerp(3.f, 0.3f, UtilDial(b));
    u.errP = (Rnd() * 2.f - 1.f) * e;
    u.errY = (Rnd() * 2.f - 1.f) * e;
    g_ut[b.team].nextAt = std::max(g_ut[b.team].nextAt, u.at + fl::UtilGap(kind));
    return true;
}
static std::string OneWord(std::string s)   // a log value the analyzers can split on spaces
{
    std::replace(s.begin(), s.end(), ' ', '_');
    return s;
}
static void UtilLog(Player &b, const char *result, const char *why)
{
    Player::Ut &u = b.ut;
    if (g_utLogged++ >= g_k.utilLog) return;
    BLog("UTIL map=%s r=%d bot=%s team=%s elo=%.0f kind=%s job=%s tx=%.0f ty=%.0f tz=%.0f d=%.0f pitch=%.1f yaw=%.1f miss=%.0f result=%s why=%s", g_mapName.c_str(),
         g_roundNo, b.name.c_str(), b.team == 2 ? "T" : "CT", b.elo, fl::UtilName(u.kind), OneWord(u.job).c_str(), u.target.x, u.target.y, u.target.z,
         Dist(b.fpos, u.target), u.pitch, u.yaw, u.miss, result, OneWord(why).c_str());
}
static void UtilAbort(Player &b, const char *why)
{
    Player::Ut &u = b.ut;
    uintptr_t w = 0;
    int ak = b.falive && b.ent ? ActiveKind(b, &w) : -2;
    if (u.phase == 3 && u.relAt < 0 && g_np.pin > 0 && ak == u.kind && w) *(uint8_t *)(w + g_np.pin) = 0;   // never leave a pulled pin behind
    UtilLog(b, "aborted", why);
    // the grenade is out and not thrown: back to the gun it held ("lastinv", checked: it must not hold the grenade 0.6 s on)
    if (ak == u.kind && u.kind >= 0 && g_cmd.backCb && !g_cmd.backOff && RunAs(b, {"lastinv"}, g_cmd.backCb))
    {
        g_cmd.backSent++;
        u.backAt = CurTime();
    }
    u.phase = 0;
    u.aborted++;
    u.cool = CurTime() + 5.f;   // no new job for 5 s (a throw it gave up is not tried again at once)
}
// "lastinv" sent 0.6 s ago: is the grenade away? (proof, or off after 3 misses in 2+ rounds with no success)
static void UtilBackCheck(Player &b, float now)
{
    Player::Ut &u = b.ut;
    if (u.backAt < 0 || now - u.backAt < 0.6f) return;
    u.backAt = -1;
    uintptr_t w;
    int ak = b.falive && b.ent ? ActiveKind(b, &w) : -2;
    if (ak == -2) return;
    if (ak != u.kind)
    {
        if (!g_cmd.backOk++) BLog("util: switch-back lever proven - %s has its gun again after a throw it gave up (lastinv)", b.name.c_str());
        return;
    }
    g_cmd.backFail++;
    UtilFailRound(g_cmd.backRound, g_cmd.backRounds);
    if (!g_cmd.backOk && g_cmd.backFail >= 3 && g_cmd.backRounds >= 2 && !g_cmd.backOff)
    {
        g_cmd.backOff = true;
        BLog("util: lastinv sent %d times, the grenade stayed in hand - switch-back off (the stock AI takes its gun back)", g_cmd.backFail);
    }
}
// our own flashes about to pop (where, when): a bot of that team that looks at one turns away for the pop, as a player does
struct UtilPop
{
    int team;
    Vec pos;
    float at;
};
static std::vector<UtilPop> g_pops;
static void UtilAway(Player &b, float now)
{
    Player::Ut &u = b.ut;
    if (!b.falive || !b.ent || u.phase >= 2 || (now >= u.awayUntil && g_pops.empty())) return;
    Vec eye = EyeOf(b.ent, b.fpos);
    if (now >= u.awayUntil)
    {
        float yaw;
        for (auto &p : g_pops)
        {
            if (p.team != b.team || now < p.at - 0.6f || now > p.at + 0.05f || Dist(eye, p.pos) > 2500.f) continue;
            if (!EyeYaw(b, &yaw) || !fl::InCone(eye, yaw, p.pos, 60.f) || !Los(eye, p.pos)) continue;
            u.awayUntil = p.at + 0.25f;
            u.awayFrom = p.pos;
            g_ut[b.team].aways++;
            break;
        }
        if (now >= u.awayUntil) return;
    }
    if (BotAttacking(b.ent) || now - b.sh.lastSeen < 0.3f) return;   // a fight comes first
    float dx = eye.x - u.awayFrom.x, dy = eye.y - u.awayFrom.y, l = sqrtf(dx * dx + dy * dy);
    if (l < 1.f) return;
    LookWant(b, LK_UTIL, {eye.x + 1000.f * dx / l, eye.y + 1000.f * dy / l, eye.z}, 900.f, 0.3f, "flash away");
}
static void UtilFrame(Player &b, float now)
{
    Player::Ut &u = b.ut;
    UtilBackCheck(b, now);
    UtilAway(b, now);
    if (!u.phase) return;
    if (!b.falive || !b.ent) return UtilAbort(b, "dead");
    if (!UtilOn() || !g_k.utilThrow) return UtilAbort(b, "switched off");
    if (now > u.deadline) return UtilAbort(b, u.phase == 1 ? "no switch" : u.phase == 2 ? "aim" : "timeout");
    if (now < u.at) return;
    if (u.waitR > 0.f)
    {
        if (Dist(b.fpos, u.waitAt) > u.waitR && now < u.at + 6.f) return;   // on its way to where it throws from
        u.waitR = 0.f;
    }
    if (u.phase <= 2 && (BotAttacking(b.ent) || b.sh.panic || now - b.sh.lastSeen < 0.3f)) return UtilAbort(b, "fight");
    if (u.phase <= 2 && b.fr.phase != 0) return UtilAbort(b, "under fire");   // part G's turn to the shooter comes first
    if (!J5State(b) && !BotAttacking(b.ent)) return UtilAbort(b, "busy");   // planting, defusing, buying, a door...
    uintptr_t w = 0;
    int ak = ActiveKind(b, &w);
    Vec eye = EyeOf(b.ent, b.fpos);
    if (u.phase == 1)
    {
        if (ak == u.kind)
        {
            u.phase = 2;
            u.equipAt = now;
            u.steady = 0;
            if (!g_cmd.useOk++) BLog("util: switch lever proven - %s has its %s in hand", b.name.c_str(), fl::UtilName(u.kind));
            return;
        }
        if (now >= u.nextTry)
        {
            if (u.tries >= 3)
            {
                if (ak != -2 && !g_cmd.useOk)
                {
                    g_cmd.useFail++;
                    UtilFailRound(g_cmd.useRound, g_cmd.useRounds);
                    if (g_cmd.useFail >= 4 && g_cmd.useRounds >= 2 && !g_cmd.useOff)
                    {
                        g_cmd.useOff = true;   // "use" sent, the grenade never came out: the switch lever does not work here
                        BLog("util: %d jobs sent \"use\" (%s) and no grenade came out - throwing off (the stock throws only)", g_cmd.useFail,
                             g_cmd.useHow.c_str());
                    }
                }
                return UtilAbort(b, ak == -2 ? "no weapon read" : "cannot switch to it");
            }
            RunAs(b, {"use", UtilItem(b, u.kind)}, g_cmd.useCb);
            g_cmd.useSent++;
            u.tries++;
            u.nextTry = now + 0.5f;
        }
        return;
    }
    if (ak != u.kind && u.phase == 2)
    {
        u.phase = 1;   // it switched away (the stock AI): switch again
        u.nextTry = now;
        return;
    }
    if (Dist(eye, u.solvedFrom) > 40.f && u.phase == 2)
    {
        if (const char *why = UtilCheck(b, u.kind, u.target, &u.pitch, &u.yaw, &u.land, &u.miss)) return UtilAbort(b, why);
        u.solvedFrom = eye;
    }
    float pr = (u.pitch + u.errP) / 57.29578f, yr = (u.yaw + u.errY) / 57.29578f;
    Vec aim{eye.x + 1000.f * cosf(pr) * cosf(yr), eye.y + 1000.f * cosf(pr) * sinf(yr), eye.z - 1000.f * sinf(pr)};
    LookWant(b, LK_UTIL, aim, 720.f, 0.4f, "util");
    auto viewOff = [&]() {   // how far its view is from the solved throw (deg; 1e9 = unreadable)
        float vy = *(float *)(b.ent + kOffViewYaw), vp = *(float *)(b.ent + kOffViewYaw - 4);
        if (!std::isfinite(vy) || !std::isfinite(vp)) return 1e9f;
        return std::max(fabsf(Wrap180(vy - (u.yaw + u.errY))), fabsf(vp - (u.pitch + u.errP)));
    };
    if (u.phase == 2)
    {
        if (now - u.equipAt > 1.8f) return UtilAbort(b, "aim");   // it never lined up: not 5 s standing still
        u.steady = viewOff() < 1.0f ? u.steady + 1 : 0;
        if (u.steady < 3 || now - u.equipAt < 0.9f || !w) return;   // lined up, and the grenade is out (deploy time)
        if (g_np.redraw > 0 && *(uint8_t *)(w + g_np.redraw)) return;   // the last one just left: the next is not out yet
        uint8_t *pin = (uint8_t *)(w + g_np.pin);
        float *st = (float *)(w + g_np.strength), *tt = (float *)(w + g_np.throwTime);
        if (*pin > 1 || !(std::isfinite(*st) && *st >= 0.f && *st <= 1.f) || !(std::isfinite(*tt) && *tt >= 0.f)) return UtilAbort(b, "grenade fields");
        if (*pin == 1 || *tt > 0.f) return UtilAbort(b, "already throwing");
        *st = 1.f;   // a full throw (the model's speed)
        *pin = 1;    // the pin is pulled: the game throws it on its next frame without the attack button
        u.phase = 3;
        u.pinAt = now;
        g_cmd.pinTries++;
        if (!g_cmd.pinOk) UtilFailRound(g_cmd.pinRound, g_cmd.pinRounds);   // rounds with pins pulled and no throw proven yet
        return;
    }
    // phase 3: released yet? (the game clears the pin at the release, then throws 0.1 s later with the bot's view)
    if (u.relAt < 0 && w && ak == u.kind)
    {
        uint8_t pin = *(uint8_t *)(w + g_np.pin);
        if (!pin) u.relAt = now;
        else if (now - u.pinAt > 0.6f && !u.forced)
        {
            // still held (the attack button): forced only with no fight and its view still on the solved throw - else the
            // pin goes back in (never a molotov at its feet because the stock aim turned it)
            if (BotAttacking(b.ent)) return UtilAbort(b, "fight");
            if (viewOff() > 2.f) return UtilAbort(b, "lost aim");
            *(float *)(w + g_np.throwTime) = CurTime();   // the same code's second branch: throw now
            *(uint8_t *)(w + g_np.pin) = 0;
            u.forced = true;
            g_cmd.forced++;
        }
    }
    else if (u.relAt < 0 && ak != u.kind)
        u.relAt = now;   // the grenade left the hand (the last one: the game switched weapons)
    if (u.relAt >= 0 && now - u.relAt > 0.3f)
    {
        g_uthrows.push_back({b.name, u.job, u.kind, b.team, u.target, now});
        if (g_uthrows.size() > 64) g_uthrows.erase(g_uthrows.begin());
        if (u.kind == fl::UK_FLASH) g_pops.push_back({b.team, u.land, u.relAt + 1.6f});   // it pops 1.5 s after the throw
        while (!g_pops.empty() && now - g_pops.front().at > 2.f) g_pops.erase(g_pops.begin());
        UtilLog(b, "thrown", u.forced ? "forced" : "pin");
        u.phase = 0;
        u.threw++;
        if (u.kind >= 0 && u.kind < fl::UK_N && u.have[u.kind] > 0) u.have[u.kind]--;
        return;
    }
    if (now - u.pinAt > 1.6f) return UtilAbort(b, "not thrown");
}
// our throws seen by the game (grenade_thrown / weapon_fire) - the lever check; a defuse to burn
static void UtilEvent(const char *n, Player *p, void *ev, float now)
{
    if (!p) return;
    if (!strcmp(n, "grenade_thrown") || !strcmp(n, "weapon_fire"))
    {
        const char *w = VF<const char *(*)(void *, const char *, const char *)>(ev, 10)(ev, "weapon", "");
        int k = w ? fl::UtilKindOf(w) : -1;
        if (k >= 0 && p->ut.phase == 3 && p->ut.kind == k && !g_cmd.pinOk++)
            BLog("util: throw lever proven - %s's %s left its hand (%s event)", p->name.c_str(), fl::UtilName(k), n);
        return;
    }
    if (!strcmp(n, "bomb_begindefuse") && p->team == 3 && UtilOn() && g_plan.planted && !g_ut[2].defuseFire)
    {
        // post-plant: a molotov / HE on the defuser from the nearest T that has one
        Player *best = nullptr;
        float bd = 1600.f;
        for (auto *t : Team(2, true))
        {
            int k = t->ut.have[fl::UK_FIRE] > 0 ? fl::UK_FIRE : t->ut.have[fl::UK_HE] > 0 ? fl::UK_HE : -1;
            float d = Dist(t->pos, g_plan.bomb);
            if (k >= 0 && !t->ut.phase && d > 400.f && d < bd && !BotAttacking(t->ent))
            {
                bd = d;
                best = t;
            }
        }
        if (best && UtilAssign(*best, best->ut.have[fl::UK_FIRE] > 0 ? fl::UK_FIRE : fl::UK_HE, g_plan.bomb, "defuse", 0.f))
            g_ut[2].defuseFire = true;
    }
}
static void UtilDetonate(Player *p, const std::string &kind, const Vec &at, float now)
{
    if (!p) return;
    int k = fl::UtilKindOf(kind.c_str());
    for (auto it = g_uthrows.rbegin(); it != g_uthrows.rend(); ++it)
        if (!it->matched && it->who == p->name && it->kind == k && now - it->at < 8.f && now >= it->at - 0.5f)
        {
            it->matched = true;
            float miss = Dist(at, it->target);
            if (!g_cmd.pinOk++) BLog("util: throw lever proven - %s's %s went off (%s event)", p->name.c_str(), fl::UtilName(k), kind.c_str());
            BLog("UTILHIT map=%s r=%d bot=%s team=%s kind=%s job=%s miss=%.0f hit=%d flight=%.1f", g_mapName.c_str(), g_roundNo, p->name.c_str(),
                 it->team == 2 ? "T" : "CT", fl::UtilName(k), OneWord(it->job).c_str(), miss, (int)(miss < fl::UtilTol(k)), now - it->at);
            return;
        }
}
// -- where to throw -------------------------------------------------------------------------------------------------------
// T execute targets for a site: smokes on the CT side (the CT rotation's last waypoint into the site from each other
// site / mid, else the retake point; then the deepest hold), a molotov on the first corner (the hold nearest the way in,
// off the straight line in), the flash point 600 u into the site and 200 u up
static void ExecTargets(const Site &s, const Vec &entry, std::vector<Vec> &smoke, std::vector<Vec> &fire, Vec &flash)
{
    smoke.clear();
    fire.clear();
    for (auto &kv : g_mi.rot)
    {
        size_t gt = kv.first.find('>');
        if (gt == std::string::npos || kv.first.substr(gt + 1) != s.label || kv.second.empty()) continue;
        const Vec &w = kv.second.back();
        bool dup = false;
        for (auto &o : smoke) dup = dup || Dist(o, w) < 600.f;
        if (!dup && smoke.size() < 2) smoke.push_back(w);
    }
    if (smoke.empty()) smoke.push_back(s.retake);
    if (smoke.size() < 2 && !s.hold.empty())
    {
        Vec deep = s.hold[0];
        for (auto &h : s.hold)
            if (Dist(h, entry) > Dist(deep, entry)) deep = h;
        if (Dist(deep, smoke[0]) > 500.f) smoke.push_back(deep);
    }
    float bd = 1e9f;
    Vec best{};
    bool have = false;
    for (auto &h : s.hold)
    {
        float d = Dist(h, entry);
        if (d < 300.f || fl::SegDist2(h, entry, s.c) < 200.f) continue;
        if (d < bd)
        {
            bd = d;
            best = h;
            have = true;
        }
    }
    if (have) fire.push_back(best);
    float L = std::max(1.f, Dist2(entry, s.c)), f = std::min(0.9f, 600.f / L);
    flash = {entry.x + (s.c.x - entry.x) * f, entry.y + (s.c.y - entry.y) * f, entry.z + 200.f};   // deep and high: it pops over the site
}
// part D's execute: the group heads for the entrance - smokes and a molotov, thrown from the entrance
static void UtilExecute()
{
    if (!UtilOn() || !g_k.utilThrow || !T12() || g_ut[2].execDone) return;
    g_ut[2].execDone = true;
    const Site &s = g_mi.sites[g_tp.plan.site];
    const Approach &A = ApOf(s, g_tp.plan.ap1);
    ExecTargets(s, A.entry, g_ut[2].smokeT, g_ut[2].fireT, g_ut[2].flashT);
    std::vector<Player *> ts;
    std::vector<std::vector<int>> have;
    for (auto *p : Team(2, true))
        if ((p->role == "group" || p->role == "group2") && !p->ut.phase)
        {
            ts.push_back(p);
            have.push_back(std::vector<int>(p->ut.have, p->ut.have + fl::UK_N));
        }
    std::vector<fl::UtilJob> jobs = fl::ExecuteJobs(have, (int)g_ut[2].smokeT.size(), (int)g_ut[2].fireT.size(), MeanSmart(Team(2, true)));
    for (auto &j : jobs)
    {
        Player *p = ts[j.bot];
        Vec at = GroupChoke(p->t12.group);
        const Vec &tg = j.kind == fl::UK_SMOKE ? g_ut[2].smokeT[j.slot] : g_ut[2].fireT[j.slot];
        UtilAssign(*p, j.kind, tg, j.kind == fl::UK_SMOKE ? "execute smoke" : "execute fire", j.delay, &at, 350.f);
    }
}
// the go signal: first 1-2 flashes pop over the site, then the group steps in (true = wait: flashes on their way, up to
// 1.6 s after the last one left the hand - it pops at 1.5 s - and 5 s at most)
static bool UtilFlashFirst(float now)
{
    if (!UtilThrowOk() || !T12()) return false;
    if (g_ut[2].goDone)
    {
        if (now - g_ut[2].goAt > 5.f) return false;
        for (auto *p : Team(2, true))
            if (p->ut.phase && p->ut.job == "execute flash") return true;
        for (auto &t : g_uthrows)
            if (t.team == 2 && t.job == "execute flash" && now - t.at < 1.6f) return true;
        return false;
    }
    g_ut[2].goDone = true;
    g_ut[2].goAt = now;
    if (!g_ut[2].execDone)
    {
        const Site &s = g_mi.sites[g_tp.plan.site];
        ExecTargets(s, ApOf(s, g_tp.plan.ap1).entry, g_ut[2].smokeT, g_ut[2].fireT, g_ut[2].flashT);
    }
    std::vector<Player *> ts;
    std::vector<std::vector<int>> have;
    std::vector<char> threw;
    for (auto *p : Team(2, true))
        if ((p->role == "group" || p->role == "group2" || p->role == "escort") && !p->ut.phase)
        {
            ts.push_back(p);
            have.push_back(std::vector<int>(p->ut.have, p->ut.have + fl::UK_N));
            threw.push_back(p->ut.threw > 0 ? 1 : 0);
        }
    std::vector<int> f = fl::ExecuteFlashes(have, threw, MeanSmart(Team(2, true)));
    int n = 0;
    for (int i : f) n += UtilAssign(*ts[i], fl::UK_FLASH, g_ut[2].flashT, "execute flash", 0.15f * n) ? 1 : 0;
    return n > 0;
}
// the go-in waits for the smokes of the execute (thrown, or given up), up to 7 s after the group reached the entrance
static bool UtilHoldGo(float now)
{
    float from = g_plan.firstAtDoor > 0 ? g_plan.firstAtDoor : now;   // not there yet: the wait has not started
    if (!UtilOn() || !g_k.utilThrow || !g_ut[2].execDone || now - from > 7.f) return false;
    for (auto *p : Team(2, true))
        if (p->ut.phase && p->ut.job == "execute smoke") return true;
    for (auto &t : g_uthrows)
        if (t.team == 2 && t.job == "execute smoke" && now - t.at < 1.0f) return true;   // let it land and bloom
    return false;
}
// every 0.25 s: inventories, then at most one new job per team
static void UtilTick(float now)
{
    UtilBuyCheck(now);
    if (!g_cmd.pinOff && g_cmd.pinTries >= 6 && !g_cmd.pinOk && g_cmd.pinRounds >= 2)
    {
        g_cmd.pinOff = true;   // six pins pulled, not one throw seen by the game: the lever does not work on this build
        BLog("util: %d pins pulled, no throw seen (grenade_thrown / weapon_fire / a detonation) - throwing off (the stock throws only)", g_cmd.pinTries);
    }
    if (!UtilThrowOk() || g_phase != PH_LIVE) return;
    for (auto &kv : g_pl)
    {
        Player &b = kv.second;
        if (!b.bot || !b.alive || now < b.ut.nextInv) continue;
        b.ut.nextInv = now + 1.f;
        b.ut.haveOk = Carried(b);
        if (b.ut.startHave < 0 && now - g_liveAt > 1.f)
        {
            int n = 0;
            for (int k = 0; k < fl::UK_DECOY; k++) n += b.ut.have[k];
            b.ut.startHave = b.ut.haveOk ? n : -2;
        }
    }
    // the lever check (util_test 1): every bot with util throws one 700 u ahead, 3 s into the round
    if (g_k.utilTest && now - g_liveAt > 3.f)
        for (int team = 2; team <= 3; team++)
        {
            if (g_ut[team].testDone) continue;
            g_ut[team].testDone = true;
            float d = 0.f;
            for (auto *p : Team(team, true))
                for (int k : {fl::UK_SMOKE, fl::UK_FIRE, fl::UK_FLASH, fl::UK_HE})
                    if (p->ut.have[k] > 0)
                    {
                        float yaw;
                        if (!EyeYaw(*p, &yaw)) break;
                        Vec t{p->fpos.x + 700.f * cosf(yaw / 57.29578f), p->fpos.y + 700.f * sinf(yaw / 57.29578f), p->fpos.z};
                        UtilAssign(*p, k, t, "test", d);
                        d += 1.f;
                        break;
                    }
        }
    for (int team = 2; team <= 3; team++)
    {
        if (now < g_ut[team].nextAt) continue;
        int enemy = team == 2 ? 3 : 2;
        std::vector<Vec> known;
        for (auto &kv : g_intel[team])
        {
            Player *e = FindPl(kv.first);
            if (e && e->alive && e->team == enemy && now - kv.second.t < 4.f && now >= kv.second.t) known.push_back(kv.second.pos);
        }
        auto groupAt = [&](Vec *c) {   // 2+ known enemies within 350 u of each other: their centre
            for (size_t i = 0; i < known.size(); i++)
            {
                int n = 1;
                Vec m = known[i];
                for (size_t j = 0; j < known.size(); j++)
                    if (j != i && Dist(known[i], known[j]) < 350.f)
                    {
                        n++;
                        m = {m.x + known[j].x, m.y + known[j].y, m.z + known[j].z};
                    }
                if (n >= 2)
                {
                    *c = {m.x / n, m.y / n, m.z / n};
                    return true;
                }
            }
            return false;
        };
        bool done = false;
        for (auto *p : Team(team, true))
        {
            if (done) break;
            Player &b = *p;
            if (b.ut.phase || !b.ent || BotAttacking(b.ent) || b.sh.panic || J5Busy(b) || !UtilCanAct(b) || now < b.ut.cool) continue;
            // a rotation / retake on its way (part N: it does not stop): only the retake's own util, near the bomb
            bool onTheWay = RotUrgent(b) && Dist(b.fpos, b.task.goal) > b.task.arrive + 150.f;
            if (onTheWay && !(team == 3 && g_plan.planted)) continue;
            int total = 0;
            for (int k = 0; k < fl::UK_DECOY; k++) total += b.ut.have[k];
            if (!total) continue;
            float t = now - g_liveAt;
            Vec c;
            // 2+ enemies known together: HE / molotov on them
            if (!onTheWay && groupAt(&c) && Dist(b.fpos, c) > 500.f && Dist(b.fpos, c) < 1800.f)
            {
                int k = b.ut.have[fl::UK_HE] > 0 ? fl::UK_HE : b.ut.have[fl::UK_FIRE] > 0 ? fl::UK_FIRE : -1;
                if (k >= 0 && UtilAssign(b, k, c, "group", 0.f)) { done = true; continue; }
            }
            if (team == 3 && !g_plan.planted)
            {
                // a defender whose site is under threat
                const Site *s = FindSite(b.role);
                if (!s && b.role == "rotate") s = FindSite(g_plan.hitSite);
                if (!s || Dist(b.fpos, s->c) > s->radius + 900.f) s = nullptr;
                if (s)
                {
                    std::vector<Vec> th;
                    for (auto &k : known)
                        if (Dist(k, s->c) < s->radius + 1300.f) th.push_back(k);
                    bool called = g_plan.hitSite == s->label && g_plan.hitLevel >= 1;
                    if (!th.empty() || called)
                    {
                        bool outside = true;
                        Vec m{0, 0, 0};
                        for (auto &k : th)
                        {
                            outside = outside && Dist(k, s->c) > s->radius + 150.f;
                            m = {m.x + k.x, m.y + k.y, m.z + k.z};
                        }
                        if (!th.empty()) m = {m.x / th.size(), m.y / th.size(), m.z / th.size()};
                        else m = s->entry;
                        int k = fl::DefendKind(b.ut.have, outside);
                        Vec tg = m;
                        if (outside && (k == fl::UK_SMOKE || k == fl::UK_FIRE))
                        {
                            float bd = 1e9f;   // their way in: the approach choke nearest to them
                            for (auto &a : s->ap)
                                if (Dist(a.choke, m) < bd)
                                {
                                    bd = Dist(a.choke, m);
                                    tg = a.choke;
                                }
                        }
                        if (k == fl::UK_FLASH) tg.z += 90.f;
                        float d = Dist(b.fpos, tg);
                        if (k >= 0 && d > 350.f && d < 1800.f && UtilAssign(b, k, tg, "defend", 0.f)) { done = true; continue; }
                    }
                }
            }
            if (team == 3 && g_plan.planted && (b.role == "retake" || b.role == "retake-awp") && Dist(b.fpos, g_plan.bomb) < 1600.f)
            {
                const Site *s = FindSite(g_plan.plantSite);
                int k = fl::RetakeKind(b.ut.have);
                Vec tg = g_plan.bomb;
                if (k == fl::UK_FLASH)
                    tg = {g_plan.bomb.x, g_plan.bomb.y, g_plan.bomb.z + 120.f};
                else if ((k == fl::UK_SMOKE || k == fl::UK_FIRE) && s && !s->post.empty())
                {
                    // smoke: the post spot farthest from it (cuts that angle); fire: the nearest one off the bomb
                    Vec far = s->post[0], near = s->post[0];
                    float fd = -1, nd = 1e9f;
                    for (auto &q : s->post)
                    {
                        float d = Dist(q, b.fpos);
                        if (d > fd) { fd = d; far = q; }
                        if (d < nd && Dist(q, g_plan.bomb) > 300.f) { nd = d; near = q; }
                    }
                    tg = k == fl::UK_SMOKE ? far : near;
                    if (k == fl::UK_FIRE && Dist(tg, g_plan.bomb) < 300.f) k = -1;   // never burn the bomb it must defuse
                }
                else if (k == fl::UK_HE && !groupAt(&tg))
                    k = -1;
                float d = Dist(b.fpos, tg);
                if (k >= 0 && d > 350.f && d < 1800.f && UtilAssign(b, k, tg, "retake", 0.f)) { done = true; continue; }
            }
            if (team == 2 && g_plan.planted && !g_ut[2].plantSmoke && b.ut.have[fl::UK_SMOKE] > 0 && now - g_plan.plantAt > 2.f)
            {
                // post-plant: smoke the retake's way in (the CT rotation's last waypoint into the site)
                const Site *s = FindSite(g_plan.plantSite);
                if (s)
                {
                    std::vector<Vec> sm, fi;
                    Vec fp;
                    ExecTargets(*s, s->entry, sm, fi, fp);
                    if (!sm.empty() && Dist(b.fpos, sm[0]) > 400.f && Dist(b.fpos, sm[0]) < 1800.f && UtilAssign(b, fl::UK_SMOKE, sm[0], "post-plant", 0.f))
                    {
                        g_ut[2].plantSmoke = true;
                        done = true;
                        continue;
                    }
                }
            }
            // a holder that knows of an enemy it cannot see: sometimes a flash before it peeks
            if (!onTheWay && b.ut.have[fl::UK_FLASH] > 0 && now >= b.ut.nextRoll && StateName(b.ent) == "9HideState" && now - b.sh.lastSeen > 1.5f)
            {
                b.ut.nextRoll = now + 3.f;
                for (auto &k : known)
                {
                    float d = Dist(b.fpos, k);
                    if (d > 500.f && d < 1400.f && Rnd() < Lerp(0.2f, 0.6f, UtilDial(b)))
                    {
                        if (UtilAssign(b, fl::UK_FLASH, {k.x, k.y, k.z + 80.f}, "peek", 0.f)) done = true;
                        break;
                    }
                }
                if (done) continue;
            }
            // late: what is left goes at a known enemy (T: 70 s in and no plant; CT: once its site is called / the plant)
            bool late = team == 2 ? (t > 70.f && !g_plan.planted) : (g_plan.hitLevel >= 1 || g_plan.planted);
            if (late && !onTheWay && !known.empty())
            {
                Vec e = known[0];
                for (auto &k : known)
                    if (Dist(k, b.fpos) < Dist(e, b.fpos)) e = k;
                float d = Dist(b.fpos, e);
                int k = b.ut.have[fl::UK_FIRE] > 0 ? fl::UK_FIRE : b.ut.have[fl::UK_HE] > 0 ? fl::UK_HE : b.ut.have[fl::UK_FLASH] > 0 ? fl::UK_FLASH : -1;
                if (k == fl::UK_FLASH) e.z += 80.f;
                if (k >= 0 && d > 500.f && d < 1800.f && UtilAssign(b, k, e, "late", 0.f)) done = true;
            }
        }
    }
}
static void UtilRoundStart()
{
    g_utFreezeAt = CurTime();
    g_utLogged = 0;
    g_pendBuy.clear();
    g_uthrows.clear();
    g_pops.clear();
    for (int t = 0; t < 4; t++) g_ut[t] = UtilTeam();
    for (auto &kv : g_pl)
    {
        Player::Ut &u = kv.second.ut;
        int keep[fl::UK_N];
        memcpy(keep, u.have, sizeof(keep));
        u = Player::Ut();
        memcpy(u.have, keep, sizeof(keep));
    }
}
static void UtilRoundEnd()
{
    if (!g_mi.ok) return;
    for (int team = 2; team <= 3; team++)
    {
        int n = 0, withUtil = 0, threw = 0, aborted = 0, buys = 0, kinds[fl::UK_N] = {};
        for (auto &kv : g_pl)
        {
            const Player &p = kv.second;
            if (!p.bot || p.team != team) continue;
            n++;
            withUtil += p.ut.startHave > 0;
            threw += p.ut.threw > 0;
            aborted += p.ut.aborted;
            buys += p.ut.buys;
        }
        for (auto &t : g_uthrows)
            if (t.team == team && t.kind >= 0 && t.kind < fl::UK_N) kinds[t.kind]++;
        BLog("UTILR map=%s r=%d team=%s on=%d bots=%d withUtil=%d threw=%d smoke=%d flash=%d he=%d fire=%d aborted=%d buys=%d buyOk=%d pinOk=%d forced=%d"
             " away=%d backOk=%d",
             g_mapName.c_str(), g_roundNo, team == 2 ? "T" : "CT", (int)(UtilThrowOk() && UtilTeamOk(team)), n, withUtil, threw, kinds[fl::UK_SMOKE],
             kinds[fl::UK_FLASH], kinds[fl::UK_HE], kinds[fl::UK_FIRE], aborted, buys, g_cmd.buyOk, g_cmd.pinOk, g_cmd.forced, g_ut[team].aways,
             g_cmd.backOk);
    }
}
// ---- P: movement in fights ------------------------------------------------------------------------------------------------
// His words: "They stop moving too often... a normal person would move, stop, shoot, move, stop, shoot. They stop, shoot,
// and then stay stopped for as long as possible... They see me peek out long and stand still the whole time... They
// don't try to reposition after three people are looking at him. They have no reaction in terms of movement. They are
// very predictable, doing the same things every time, holding the same spots."
// Why: step 11 keeps every bot's Skill under Valve's 0.5 line so it sprays - and the stock fight only side-steps above
// that line; step 10's stop-to-shoot plants its feet for 0.35 s at the start and 0.22 s after EVERY shot, so a spray keeps
// it planted for the whole fight; part G moves it only while it is being shot at; nothing moved a holder after a fight or
// when several enemies had it in view; T post-plant and lurk spots were the same every round. Now:
//   cycle   a MOVER fight (a roll per sighting / fight by the footwork dial - step 10's plugin dial, 0 at elo 300: a
//           Silver keeps its planted spray; ~45% of Gold Nova fights, 95% at the top, fl::Mover) runs fl::CycStep: STOP
//           (the speed scale at 5%, the step-11 profile: it sprays standing - the burst 0.55 .. 0.30 s, longer inside
//           800 u) then MOVE (full speed, profile Skill lifted into the stock fight's side-step band 0.55 .. 0.70), then
//           STOP again at its next shot or once the move is over with its crosshair on him (0.7 s at most). The Skill
//           band is already on from the moment it sees him (so the stock fight starts with its side-step on, whenever
//           the stock decides it). Never in step 11's close-range panic (part A unchanged).
//   after   a holder that just finished a fight takes another spot 250-700 u away that the enemy's last position cannot
//           see (20% .. 80% by its angles dial, x0.7 if it did not win) - "reposition after a kill" (DIALS C2).
//   seen    seen by 2+ enemies at once (it is inside their view with a clear line) and not fighting: it falls back to a
//           spot none of them sees, 300-900 u away, not towards them (fl::PickFallback) - never on a rotation, a retake,
//           a trade, the hit or the bomb; once in 6 s.
//   vary    T post-plant spots and the T lurk spot: not last round's when another is free (as part M does for the CTs).
// Proof: "FIGHT" per fight (on and off alike: mover, seconds, moving / stopped share, cycles, shots, hits, a kill, stood
// still the whole fight), "MOVE" per reposition / fallback, "MOVR" per bot per round, "TPOST" per plant (spots, the same
// as last round).
// Switches: brain_move 0 (all of P), move_cycle 0, move_repo 0, move_fall 0, move_vary 0; A/B move_team 2|3; tests
// move_dial <v> (every bot's footwork for P, absolute), move_log <n> (FIGHT lines a round).
static bool MoveOn(const Player &p) { return g_k.move && (!g_k.moveTeam || g_k.moveTeam == p.team) && g_modeOk && g_layout == 1 && g_np.ok; }
static float MoveDial(const Player &p) { return g_k.moveDial >= 0 ? std::min(1.f, g_k.moveDial) : p.fw; }
static bool MoveCycling(const Player &p) { return p.mv.mover && p.mv.cyc.phase > 0 && MoveOn(p) && g_k.moveCyc && !p.sh.panic; }
static void J5bWant(Player &p, float *w)
{
    if (!MoveOn(p) || !g_k.moveCyc || !p.mv.mover) return;
    float now = CurTime();
    bool sighted = !p.mv.inFight && now - p.sh.lastSeen < 0.5f && now >= p.sh.lastSeen;
    if (p.mv.cyc.phase == 2 || sighted) w[PF_SKILL] = std::max(w[PF_SKILL], fl::MoveSkill(MoveDial(p)));
}
static int g_fightLogged, g_moveLogged;   // FIGHT lines (move_log a round), MOVE lines (60 a round)
// hiding spots around `c` (lo..hi u, the same floor), not exposed; with their cover flag
static void SpotsAround(const Vec &c, float lo, float hi, std::vector<Vec> &out, std::vector<char> &cover, size_t max)
{
    std::vector<std::pair<float, int>> near;
    for (size_t i = 0; i < g_mi.spots.size(); i++)
    {
        const Vec &v = g_mi.spots[i].first;
        float d = Dist2(v, c);
        if (d < lo || d > hi || fabsf(v.z - c.z) > 150.f || (g_mi.spots[i].second & 8)) continue;
        near.push_back({d, (int)i});
    }
    std::sort(near.begin(), near.end());
    if (near.size() > max) near.resize(max);
    for (auto &n : near)
    {
        out.push_back(g_mi.spots[n.second].first);
        cover.push_back((g_mi.spots[n.second].second & 1) ? 1 : 0);
    }
}
static bool HiddenFrom(const Vec &spot, const std::vector<Vec> &eyes)
{
    for (auto &e : eyes)
        if (Los(e, {spot.x, spot.y, spot.z + 64.f}) || Los(e, {spot.x, spot.y, spot.z + 40.f})) return false;
    return true;
}
// a plain hold P may move a bot off: a CT's site / mid / lurk spot before the plant, a T's post-plant or lurk spot (or its
// hostage hold) - never a push (L ends it), a rotation or the retake (N), a trade, the hit, the entrance or the bomber
static bool MoveHold(const Player &b)
{
    if (b.task.kind != T_HOLD || !b.ent || b.name == g_bomber || Urgent(b, BotTask(b.ent))) return false;
    const std::string &w = b.task.why;
    bool ours = w == "reposition" || w == "fall back (seen)";
    if (b.team == 3)
        return !g_plan.planted && (ours || w.compare(0, 7, "CT hold") == 0 || w == "CT anchor" || w == "CT mid" || w == "CT lurk" ||
                                   w == "CT back from push" || w == "CT back from trade");
    if (b.team == 2)
        return g_plan.planted ? ours || w.compare(0, 12, "T post-plant") == 0
                              : w == "T lurk" || w == "T guard hostages" || w == "T hold hostage approach" || (ours && (b.role == "lurk" || g_mi.hostage));
    return false;
}
// a holder at its spot takes another one the enemy (at `from`) cannot see
static void MoveRepo(Player &b, float now, bool killed, const Vec &from)
{
    Player::Mv &m = b.mv;
    if (!MoveHold(b) || Dist(b.pos, b.task.goal) > b.task.arrive + 150.f || now - m.repoAt < 8.f) return;
    if (!fl::RepoAfter(DialOf(b, D_ANGLES, false), killed, Rnd())) return;
    std::vector<Vec> c;
    std::vector<char> cov, hid;
    SpotsAround(b.fpos, 250.f, 700.f, c, cov, 12);
    const Site *own = b.team == 3 ? FindSite(b.role) : nullptr;
    Vec eye{from.x, from.y, from.z + 62.f};
    bool q = g_c.qFall && CtQHolder(b) && !own;   // JOB 5c Q: the mid / lurker stays within 600 u of its spot too
    for (auto &v : c) hid.push_back((own && Dist2(v, own->c) > own->radius + 150.f) || (q && Dist2(v, b.ct.setSpot) > 600.f) ? 0 : HiddenFrom(v, {eye}) ? 1 : 0);
    int k = fl::PickFallback(b.fpos, {from}, c, hid, cov, 250.f, 700.f);
    if (k < 0) return;
    m.repoAt = now;
    m.repos++;
    Assign(&b, T_HOLD, c[k], 1, 120, "reposition", std::max(20.f, b.task.holdFor));
    if (b.team == 3 && (CtSetOn() || PushOn())) b.ct.lastSpot = c[k];   // M / L's "back to its spot" (part C's own choice untouched)
    if (g_moveLogged++ < 60)
        BLog("MOVE map=%s r=%d bot=%s team=%s what=reposition killed=%d d=%.0f | %s takes another angle after the fight", g_mapName.c_str(), g_roundNo,
             b.name.c_str(), b.team == 2 ? "T" : "CT", (int)killed, Dist(b.fpos, c[k]), b.name.c_str());
}
// seen by 2+ enemies at once: fall back to a spot none of them sees
static void MoveFall(Player &b, float now, const std::vector<Vec> &eyes, const std::vector<Vec> &at)
{
    Player::Mv &m = b.mv;
    if (now - m.fallAt < 6.f || BotAttacking(b.ent) || b.sh.panic || UtilBusy(b) || J5Busy(b) || !J5MayMove(b)) return;
    const std::string &w = b.task.why;
    if (Urgent(b, BotTask(b.ent)) || w == "T hit" || w == "T to the entrance" || w == "CT push" || b.name == g_bomber) return;
    // JOB 5c Q: a CT holder falls back only once it is AT its spot, and only onto its own ground (5b: seen crossing mid on
    // its way out, it took a spot by mid doors for the rest of the round - "every single one of them was mid doors")
    bool own = g_c.qFall && CtQHolder(b);
    if (own && !fl::CtAtSpot(Dist(b.pos, b.task.goal), b.task.arrive)) return;
    const Site *os = own ? FindSite(b.role) : nullptr;
    std::vector<Vec> c;
    std::vector<char> cov, hid;
    SpotsAround(b.fpos, 300.f, 900.f, c, cov, 16);
    for (auto &v : c)
        hid.push_back((!own || fl::CtOwnGround(os != nullptr, os ? Dist2(v, os->c) : 0.f, os ? os->radius : 0.f, Dist2(v, b.ct.setSpot))) && HiddenFrom(v, eyes) ? 1 : 0);
    int k = fl::PickFallback(b.fpos, at, c, hid, cov, 300.f, 900.f);
    m.fallAt = now;
    if (k < 0) return;
    m.falls++;
    Assign(&b, T_HOLD, c[k], 1, 120, "fall back (seen)", 20);
    if (b.team == 3 && (CtSetOn() || PushOn())) b.ct.lastSpot = c[k];
    if (g_moveLogged++ < 60)
        BLog("MOVE map=%s r=%d bot=%s team=%s what=fallback seenBy=%d d=%.0f | %s had %d enemies on it - falls back out of their view", g_mapName.c_str(),
             g_roundNo, b.name.c_str(), b.team == 2 ? "T" : "CT", (int)at.size(), Dist(b.fpos, c[k]), b.name.c_str(), (int)at.size());
}
static void MoveFrame(Player &b, float now)
{
    Player::Mv &m = b.mv;
    float dt = now - m.lastT;
    m.lastT = now;
    if (dt < 0 || dt > 0.5f) dt = 0;
    if (!b.falive || !b.ent)
    {
        m.inFight = false;
        m.cyc = fl::Cyc();
        return;
    }
    bool attack = BotAttacking(b.ent), on = MoveOn(b);
    // a new sighting: the mover roll (so the stock fight can start with its side-step on)
    if (!m.inFight && b.sh.sightAt > m.rollAt)
    {
        m.rollAt = b.sh.sightAt;
        m.mover = on && g_k.moveCyc && fl::Mover(MoveDial(b), Rnd());
    }
    if (attack && !m.inFight)
    {
        m.inFight = true;
        m.fightAt = now;
        m.fMove = m.fStop = 0;
        m.shots0 = b.m.shots;
        m.hits0 = b.m.hits;
        m.kills0 = b.m.kills;
        m.fights++;
        if (now - m.rollAt > 2.f) m.mover = on && g_k.moveCyc && fl::Mover(MoveDial(b), Rnd());
        m.moverFights += m.mover;
        m.cyc = fl::Cyc();
        m.shotSeen = b.lastFire;
        // JOB 5b Q: this fight's bursts crouched? (a bot that held its angle crouched stays down for it)
        Player *e0 = FindPl(b.seenName);
        float d0 = e0 && e0->falive ? Dist(b.fpos, e0->fpos) : 1000.f;
        b.cr.spray = CrouchOn(b) && g_k.crouchSpray && (b.cr.wrote == 1 || fl::CrouchSpray(MoveDial(b), d0, Rnd()));
        b.cr.sprays += b.cr.spray;
        b.cr.fightDuck = 0;
    }
    if (attack) m.lastAtk = now;
    if (m.inFight)
    {
        m.fightT += dt;
        if (b.speed > 80.f)
        {
            m.fMove += dt;
            m.moveT += dt;
        }
        else if (b.speed < 30.f)
            m.fStop += dt;
        if (m.mover && on && g_k.moveCyc && attack && !b.sh.panic)
        {
            bool shot = b.lastFire > m.shotSeen + 1e-4f;
            m.shotSeen = b.lastFire;
            Player *e = FindPl(b.seenName);
            float d = e && e->falive ? Dist(b.fpos, e->fpos) : 1000.f, yaw;
            bool onT = false;
            if (e && e->falive && EyeYaw(b, &yaw))
            {
                Vec eye = EyeOf(b.ent, b.fpos);
                onT = fabsf(Wrap180(YawTo(eye, e->fpos) - yaw)) < std::max(2.f, atan2f(32.f, std::max(1.f, d)) * 57.29578f);
            }
            fl::CycStep(m.cyc, true, now, onT, shot, fl::CycFor(MoveDial(b), d));
        }
        else if (m.cyc.phase)
            m.cyc.phase = 0;
        if (!attack && now - m.lastAtk > 1.0f)
        {
            m.inFight = false;
            m.cycles += m.cyc.cycles;
            float dur = std::max(0.f, m.lastAtk - m.fightAt);
            bool killed = b.m.kills > m.kills0, still = dur > 1.5f && m.fMove < 0.1f * dur;
            m.still += still;
            if (g_fightLogged++ < g_k.moveLog)
                BLog("FIGHT map=%s r=%d bot=%s team=%s elo=%.0f dial=%.2f on=%d mover=%d dur=%.1f move=%.1f stop=%.1f cycles=%d shots=%d hits=%d kill=%d still=%d"
                     " crouch=%.1f cspray=%d crouchOn=%d",
                     g_mapName.c_str(), g_roundNo, b.name.c_str(), b.team == 2 ? "T" : "CT", b.elo, MoveDial(b), (int)on, (int)m.mover, dur, m.fMove,
                     m.fStop, m.cyc.cycles, b.m.shots - m.shots0, b.m.hits - m.hits0, (int)killed, (int)still, b.cr.fightDuck, (int)b.cr.spray,
                     (int)CrouchOn(b));
            m.cyc = fl::Cyc();
            if (on && g_k.moveRepo)
            {
                Vec from = b.sh.closePos;
                auto it = g_intel[b.team].find(b.seenName);
                if (it != g_intel[b.team].end() && now - it->second.t < 5.f) from = it->second.pos;
                if (from.x != 0.f || from.y != 0.f) MoveRepo(b, now, killed, from);
            }
        }
    }
    // seen by 2+ enemies at once (every 0.2 s, on and off alike for the metric)
    if (now < m.nextSeen) return;
    m.nextSeen = now + 0.2f;
    std::vector<Vec> eyes, at;
    Vec me{b.fpos.x, b.fpos.y, b.fpos.z + 56.f};
    for (auto &kv : g_pl)
    {
        Player &e = kv.second;
        float yaw;
        if (!e.falive || e.team == b.team || (e.team != 2 && e.team != 3) || Dist(e.fpos, b.fpos) > 2500.f || !EyeYaw(e, &yaw)) continue;
        Vec eye{e.fpos.x, e.fpos.y, e.fpos.z + 62.f};
        if (!fl::InCone(eye, yaw, b.fpos, 40.f) || !Los(eye, me)) continue;
        eyes.push_back(eye);
        at.push_back(e.fpos);
    }
    m.seenBy = (int)eyes.size();
    if (m.seenBy >= 2)
    {
        if (now - m.seenEp > 3.f) m.seen2++;   // one episode per 3 s
        m.seenEp = now;
        if (on && g_k.moveFall) MoveFall(b, now, eyes, at);
    }
}
// T post-plant / lurk spots: another spot than last round's when there is one
static std::vector<Vec> g_tPostLast, g_tPostNow;
static Vec g_tLurkLast{};
static bool MoveVaryT() { return g_k.move && g_k.moveVary && (!g_k.moveTeam || g_k.moveTeam == 2); }   // T spots only
static Vec VaryPost(const Site *s, const Vec &bomb, int k, const std::vector<Vec> &sorted)
{
    Vec def = sorted.empty() ? bomb : sorted[k % sorted.size()];
    if (!MoveVaryT() || !s)
    {
        g_tPostNow.push_back(def);
        return def;
    }
    std::vector<fl::SpotCand> c;
    for (auto &p : sorted) c.push_back({p, 1.f});
    std::vector<Vec> sp;
    std::vector<char> cov;
    SpotsAround(bomb, 250.f, 800.f, sp, cov, 12);
    for (size_t i = 0; i < sp.size(); i++) c.push_back({sp[i], cov[i] ? 0.7f : 0.4f});
    std::vector<float> rnd;
    for (size_t i = 0; i < c.size(); i++) rnd.push_back(Rnd());
    int j = fl::PickSpot(c, g_tPostNow, g_tPostLast, 0.6f, rnd);
    Vec v = j >= 0 ? c[j].p : def;
    g_tPostNow.push_back(v);
    return v;
}
static Vec VaryLurk(const Vec &lurk)
{
    if (!MoveVaryT()) return lurk;
    std::vector<Vec> sp;
    std::vector<char> cov;
    SpotsAround(lurk, 0.f, 450.f, sp, cov, 10);
    std::vector<fl::SpotCand> c = {{lurk, 0.8f}};
    for (size_t i = 0; i < sp.size(); i++) c.push_back({sp[i], cov[i] ? 0.7f : 0.4f});
    std::vector<float> rnd;
    for (size_t i = 0; i < c.size(); i++) rnd.push_back(Rnd());
    int j = fl::PickSpot(c, {}, {g_tLurkLast}, 0.6f, rnd);
    g_tLurkLast = j >= 0 ? c[j].p : lurk;
    return g_tLurkLast;
}
static void MoveRoundStart()
{
    g_fightLogged = g_moveLogged = 0;
    if (!g_tPostNow.empty()) g_tPostLast = g_tPostNow;
    g_tPostNow.clear();
    for (auto &kv : g_pl)
    {
        kv.second.mv = Player::Mv();
        int wrote = kv.second.cr.wrote;
        kv.second.cr = Player::Cr();
        kv.second.cr.wrote = wrote;   // what we last wrote stays known (released by CrouchFrame)
    }
}
static void MoveRoundEnd()
{
    if (!g_tPostNow.empty())
    {
        int again = 0;
        for (auto &v : g_tPostNow)
            for (auto &h : g_tPostLast) again += Dist(v, h) < 120.f ? 1 : 0;
        BLog("TPOST map=%s r=%d on=%d spots=%d again=%d", g_mapName.c_str(), g_roundNo, (int)(g_k.move && g_k.moveVary), (int)g_tPostNow.size(), again);
    }
    for (auto &kv : g_pl)
    {
        Player &p = kv.second;
        if (!p.bot || (p.team != 2 && p.team != 3)) continue;
        const Player::Mv &m = p.mv;
        const Player::Cr &c = p.cr;
        BLog("MOVR map=%s r=%d %s team=%s elo=%.0f dial=%.2f on=%d fights=%d movers=%d cycles=%d fightT=%.1f moveT=%.1f still=%d repos=%d falls=%d seen2=%d"
             " crouchOn=%d holdT=%.1f holdDuck=%.1f cholds=%d sholds=%d sprays=%d",
             g_mapName.c_str(), g_roundNo, p.name.c_str(), p.team == 2 ? "T" : "CT", p.elo, MoveDial(p), (int)MoveOn(p), m.fights, m.moverFights, m.cycles,
             m.fightT, m.moveT, m.still, m.repos, m.falls, m.seen2, (int)CrouchOn(p), c.holdT, c.holdDuckT, c.crouchHolds, c.standHolds, c.sprays);
    }
}
// ---- Q: crouching (his follow-up, 09-27: "we definitely want to make the bots crouch"; "I've seen them crouch... they
// were almost doing it too much") -------------------------------------------------------------------------------------
// The stock AI crouches on its own (hiding at a spot, "crouch and hold" at the start of some fights); job 5 put every CT
// on a hold, so they crouched a lot, and nothing chose WHEN. The lever is the bot's own crouch flag - the byte the bot turns
// into the duck button of its next command. Its offset is not taken from any source layout:
//   find   while the rounds are played, every 0.25 s, the bot's own part of the object (from its profile pointer to its
//          embedded states, 376 bytes) is read on every live bot next to the player's FL_DUCKING (send table m_fFlags):
//          the one byte that is only ever 0 / 1 and equals FL_DUCKING whenever both have been steady for 0.5 s - on 2+
//          bots that ducked, 97%+ of the ducked and of the standing samples, 5 points clear of the next (fl::CrouchPick);
//          two look-alikes = none;
//   prove  before any use: on an idle bot (the freeze first) the byte is set every frame - the bot must duck within 0.8 s
//          - then cleared - it must stand again within 0.8 s; else crouching stays off for good ("does not duck");
//   use    written every frame while we want it (the bot's own posture code may reset it): +1 crouch, -1 stand, and
//          released once when we stop (the stock bot decides again):
//            - crouch-spray: a roll per fight by the footwork dial (0 at elo 300 - the kid floor; ~30% of Gold Nova fights,
//              50% at the top, 350-1500 u, fl::CrouchSpray): its bursts are fired crouched (a mover's STOP phases, part P;
//              a non-mover its whole fight); a mover strafes standing (MOVE phases); never in step 11's panic;
//            - holds: at its hold spot a bot crouches by a roll per spot (10 .. 40% by its angles dial, 0 at elo 300,
//              fl::CrouchHold) and only where its crouched eye still sees what it holds (its site's way in, mid, the choke,
//              the bomb); every other hold is held STANDING (the stock hide crouch was the "too much").
// Proof: "crouch: ... found / proven / off" lines; FIGHT gains crouch= (seconds ducked in the fight) and cspray=; MOVR gains
// holdT / holdDuck (seconds holding, and of it ducked - on AND off, the "too much" number), crouched / standing holds.
// Switches: brain_crouch 0 (the stock crouching only), crouch_spray 0, crouch_hold 0.
enum { kCrN = kStateLo - kOffProfile };
static struct CrouchF
{
    int state = 0;   // 0 watching, 1 found (to prove), 2 proven, -1 off
    int off = 0;     // the flag's offset in the bot object
    std::vector<fl::CrouchCand> cand;
    int ducked = 0, standing = 0;
    std::set<std::string> botsDucked;
    struct Snap
    {
        bool d;
        unsigned char b[kCrN];
    };
    std::map<std::string, std::deque<Snap>> hist;
    float next = 0;
    std::string testBot;
    int testPhase = 0, tries = 0, noDuck = 0;
    float testAt = 0, retryAt = 0;
} g_cr;
static bool CrouchOn(const Player &p) { return g_k.crouch && g_cr.state == 2 && g_modeOk && g_layout == 1 && p.bot && p.ent; }
static bool Ducked(const Player &p) { return p.ent && g_np.flags > 0 && (*(int *)(p.ent + g_np.flags) & 2); }
static void CrouchWrite(Player &p, int v)
{
    if (g_cr.off <= 0 || !p.ent) return;
    *(uint8_t *)(p.ent + g_cr.off) = v ? 1 : 0;
}
static void CrouchEndTest(const char *why, bool fail)
{
    Player *b = FindPl(g_cr.testBot);
    if (b && b->falive && b->ent) CrouchWrite(*b, 0);
    g_cr.testPhase = 0;
    g_cr.testBot.clear();
    g_cr.tries++;
    if (fail) g_cr.noDuck++;
    g_cr.retryAt = CurTime() + 3.f;
    if (g_cr.noDuck >= 2 || g_cr.tries >= 12)
    {
        g_cr.state = -1;
        BLog("crouch: the flag at 0x%x %s - crouching stays the stock bots' own (brain_crouch has nothing to do)", g_cr.off, why);
    }
}
// the proof: set it on an idle bot, it must duck; clear it, it must stand
static void CrouchProve(float now)
{
    if (now < g_cr.retryAt && g_cr.testPhase == 0) return;
    Player *b = g_cr.testBot.empty() ? nullptr : FindPl(g_cr.testBot);
    if (g_cr.testPhase == 0)
    {
        for (auto &kv : g_pl)
        {
            Player &p = kv.second;
            if (!p.bot || !p.falive || !p.ent || BotAttacking(p.ent) || p.speed > 30.f || Ducked(p) || (g_phase == PH_LIVE && NearEnemy(p, 1500.f))) continue;
            b = &p;
            break;
        }
        if (!b) return;
        g_cr.testBot = b->name;
        g_cr.testPhase = 1;
        g_cr.testAt = now;
    }
    if (!b || !b->falive || !b->ent || BotAttacking(b->ent)) return CrouchEndTest("test interrupted", false);
    if (g_cr.testPhase == 1)
    {
        CrouchWrite(*b, 1);
        if (Ducked(*b))
        {
            BLog("crouch: %s ducked %.2f s after its crouch flag (0x%x) was set", b->name.c_str(), now - g_cr.testAt, g_cr.off);
            g_cr.testPhase = 2;
            g_cr.testAt = now;
        }
        else if (now - g_cr.testAt > 0.8f)
            CrouchEndTest("was set for 0.8 s and the bot did not duck", true);
        return;
    }
    CrouchWrite(*b, 0);
    if (!Ducked(*b))
    {
        g_cr.state = 2;
        g_cr.testPhase = 0;
        BLog("crouch: lever proven - %s stood up again %.2f s after the flag was cleared: the bots' crouch flag is at 0x%x", b->name.c_str(),
             now - g_cr.testAt, g_cr.off);
    }
    else if (now - g_cr.testAt > 0.8f)
        CrouchEndTest("was cleared and the bot stayed down", true);
}
// every frame: watch (4 a second, live rounds), then prove
static void CrouchTick(float now)
{
    if (!g_np.ok || g_np.flags <= 0 || g_layout != 1 || g_cr.state < 0 || g_cr.state == 2) return;
    if (g_cr.state == 1)
    {
        if (g_k.crouch) CrouchProve(now);
        return;
    }
    if (now < g_cr.next && g_cr.next - now < 1.f) return;
    g_cr.next = now + 0.25f;
    if (g_phase != PH_LIVE) return;   // the stock crouching happens in play
    if (g_cr.cand.empty()) g_cr.cand.resize(kCrN);
    for (auto &kv : g_pl)
    {
        Player &b = kv.second;
        if (!b.bot || !b.falive || !b.ent) continue;
        CrouchF::Snap sn;
        sn.d = Ducked(b);
        if (!SafeRead((void *)(b.ent + kOffProfile), sn.b, kCrN)) continue;
        auto &h = g_cr.hist[b.name];
        h.push_back(sn);
        if (h.size() > 3) h.pop_front();
        for (int i = 0; i < kCrN; i++)
            if (sn.b[i] > 1) g_cr.cand[i].bad = true;
        if (h.size() < 3 || h[0].d != h[1].d || h[1].d != h[2].d) continue;
        if (sn.d)
        {
            g_cr.ducked++;
            g_cr.botsDucked.insert(b.name);
        }
        else
            g_cr.standing++;
        for (int i = 0; i < kCrN; i++)
        {
            fl::CrouchCand &cc = g_cr.cand[i];
            if (cc.bad || h[0].b[i] != h[1].b[i] || h[1].b[i] != h[2].b[i]) continue;
            bool m = (h[2].b[i] == 1) == sn.d;
            (sn.d ? cc.totalD : cc.totalS)++;
            (sn.d ? cc.agreeD : cc.agreeS) += m;
        }
    }
    int k = fl::CrouchPick(g_cr.cand, g_cr.ducked, g_cr.standing, (int)g_cr.botsDucked.size());
    if (k < 0) return;
    g_cr.off = kOffProfile + k;
    g_cr.state = 1;
    BLog("crouch: the bots' crouch flag found at 0x%x (agrees with FL_DUCKING in %d of %d ducked and %d of %d standing steady samples, on %d"
         " bots) - proving it next",
         g_cr.off, g_cr.cand[k].agreeD, g_cr.cand[k].totalD, g_cr.cand[k].agreeS, g_cr.cand[k].totalS, (int)g_cr.botsDucked.size());
}
// what a hold watches: its site's way in, mid, the nearest choke, the bomb
static bool CrouchSees(const Player &b)
{
    Vec w{};
    bool have = false;
    if (g_plan.planted && b.team == 2)
    {
        w = g_plan.bomb;
        have = true;
    }
    else if (const Site *s = FindSite(b.role))
    {
        float bd = 1e9f;
        for (auto &a : s->ap)
            if (Dist(a.choke, b.fpos) < bd)
            {
                bd = Dist(a.choke, b.fpos);
                w = a.entry;
                have = true;
            }
    }
    else if (b.role == "mid" && !g_mi.midAreas.empty())
    {
        float bd = 1e9f;
        for (auto &m : g_mi.midAreas)
        {
            Vec c{0.5f * (m.x0 + m.x1), 0.5f * (m.y0 + m.y1), m.z};
            if (Dist(c, b.fpos) < bd && Dist(c, b.fpos) > 300.f)
            {
                bd = Dist(c, b.fpos);
                w = c;
                have = true;
            }
        }
    }
    else
    {
        float bd = 1e9f;
        for (auto &s : g_mi.sites)
            for (auto &a : s.ap)
                if (Dist(a.choke, b.fpos) < bd)
                {
                    bd = Dist(a.choke, b.fpos);
                    w = a.choke;
                    have = true;
                }
    }
    return have && Los({b.fpos.x, b.fpos.y, b.fpos.z + 46.f}, {w.x, w.y, w.z + 40.f});
}
static void CrouchFrame(Player &b, float now)
{
    Player::Cr &c = b.cr;
    static std::map<std::string, float> lastT;
    float &lt = lastT[b.name], dt = now - lt;
    lt = now;
    if (dt < 0 || dt > 0.5f) dt = 0;
    if (!b.falive || !b.ent)
    {
        c.wrote = -1;
        c.want = 0;
        return;
    }
    bool attack = BotAttacking(b.ent), d = Ducked(b);
    bool atHold = !attack && b.task.kind == T_HOLD && Dist(b.fpos, b.task.goal) < b.task.arrive + 60.f && b.hsp < 30.f && StateName(b.ent) == "9HideState";
    if (atHold)   // the metric, on and off alike: how much of its holding it spends crouched
    {
        c.holdT += dt;
        if (d) c.holdDuckT += dt;
    }
    if (attack && d) c.fightDuck += dt;
    int want = 0;
    if (CrouchOn(b))
    {
        if (attack)
        {
            if (!b.sh.panic && c.spray && g_k.crouchSpray && (!b.mv.mover || b.mv.cyc.phase == 1)) want = 1;   // its bursts, crouched
            else if (!b.sh.panic && b.mv.mover && b.mv.cyc.phase == 2) want = -1;                               // it strafes standing
        }
        else if (atHold && g_k.crouchHold)
        {
            if (Dist(c.holdGoal, b.task.goal) > 1.f)
            {
                c.holdGoal = b.task.goal;
                c.holdCrouch = fl::CrouchHold(DialOf(b, D_ANGLES, true), Rnd()) && CrouchSees(b);
                (c.holdCrouch ? c.crouchHolds : c.standHolds)++;
            }
            want = c.holdCrouch ? 1 : -1;
        }
    }
    c.want = want;
    if (want)
    {
        CrouchWrite(b, want > 0);
        c.wrote = want > 0 ? 1 : 0;
    }
    else
    {
        if (c.wrote == 1) CrouchWrite(b, 0);   // let go of our crouch once; the stock bot decides again
        c.wrote = -1;
    }
}
static void CrouchRestoreAll()
{
    for (auto &kv : g_pl)
        if (kv.second.cr.wrote == 1 && kv.second.ent && kv.second.falive) CrouchWrite(kv.second, 0);
}
static void J5bRoundStart()
{
    for (auto &kv : g_pl) kv.second.rt = Player::Rt();
    UtilRoundStart();   // O
    MoveRoundStart();   // P
}
static void J5bRoundEnd()
{
    float now = CurTime();
    for (auto &kv : g_pl) RotLog(kv.second, "round end", now);
    UtilRoundEnd();     // O
    MoveRoundEnd();     // P
}
static void J5bMatchStart()
{
    g_pushHist.clear();
    g_pushLast = -100;
}
static void J5bLevelInit()
{
    g_cmd.buyOff = g_cmd.pinOff = g_cmd.useOff = g_cmd.backOff = false;   // O: a lever's self-check starts again on each map
    g_cmd.buyFail = g_cmd.useFail = g_cmd.backFail = 0;
    if (!g_cmd.pinOk) g_cmd.pinTries = 0;
    g_cmd.buyRounds = g_cmd.pinRounds = g_cmd.useRounds = g_cmd.backRounds = 0;
    g_cmd.buyRound = g_cmd.pinRound = g_cmd.useRound = g_cmd.backRound = -1;
    g_cr.hist.clear();
    if (g_cr.state == 1) g_cr.testPhase = 0, g_cr.testBot.clear();
    g_cc = CtCands();
    g_spotLast.clear();
    g_extraLast = -1;
    g_pushSpots.clear();
    g_pushBuilt = false;
    g_pushHist.clear();
    g_pushLast = -100;
    g_push = CtPush();
}
// ==== end JOB 5b ====================================================================================================

// ==== JOB 5c (ADDENDUM 3 - his playtest of job 5b, 09-29, de_dust2): points Q-U =====================================
// The switches and their meaning are above JOB 5b (g_c, RefreshJ5c). Same bar as before: bots that play like people.
// ---- Q: the CT setup, 2 - 2 - 1 -------------------------------------------------------------------------------------
// His words: "we push B, there is nobody defending B... every single one of them was pretty much mid doors." What he
// wants: "two on A, two on B. One lingers. That lingerer usually watches mid. And then they play angles until somebody
// pushes into them... with every now and then a random aggressive push."
// Why 5b did this - measured, not guessed (bot-only matches, elo 300 like his pick, de_dust2, brain_diag 1: every CT's
// role, task goal and position every 3 s): 5b's CTSET / CTSPOT were right (B holders got B spots) - not a map-point
// mistake. The holders never got there, for three reasons in the movement:
//   (1) a 5b bug: part P's "fall back when 2+ enemies see it" fired as the CTs crossed mid on their way out (the Ts see
//       down mid from their side) and swapped the site spot for a hiding spot 300-900 u away - by mid doors - for the whole
//       round: rounds with 3-5 CTs (A, B and lurk holders) parked at CT mid;
//   (2) step 9's walk: every CT hold took the game's SAFEST route, and the game's planner (even its fastest route, once the
//       teams had swapped at the half) sent B holders from the CT spawn round through A, down long and across the T side;
//   (3) a step-9 steering hole: Steer counts a bot as "on its way" while it is in MoveToState with our goal recorded - it
//       never looked at the goal the MoveTo state really has, so a MoveTo the stock AI re-targeted (a noise, a hunt) was
//       left alone (the same hole sent Ts down mid - part T).
// Now (switch brain_ctq):
//   roles  fl::CtRoles5c: an anchor on each site, the lingerer (mid; the lurk spot 20% of rounds), the second men one per
//          site - 2 / 2 / 1 with 5 CTs (a smart team's rare stack stays part C's roll); ctq_roles 0 = 5b's A, B, mid, lurk + 1;
//   ground a holder falls back / repositions only once it is AT its spot, and only onto its own ground (its site, or 600 u
//          round its mid / lurk spot); ctq_fall 0 = 5b's;
//   route  the walk to a site spot takes the fastest route over step 9b's CT-side waypoints (CT spawn -> CT mid -> B doors ->
//          B), and a MoveTo the stock AI re-targets is set back (Steer's re-target guard, shared with part T - a re-issue
//          for that never counts towards step 9's "cannot reach it, give up after 40"); ctq_route 0 = 5b's walk.
// Proof (on and off alike): "CTQ" per CT at freeze end + 20 s (its role, where it stands, how far from its spot, its task)
// and "CTQ20" per round (how many CTs stand on each site / mid / elsewhere).
static struct CtQ
{
    bool done = false;
    float callAt = -1;   // the first site call / hit of the round (s after freeze end)
} g_ctq;
static const char *CtAreaName(int a)
{
    if (a >= 0 && a < (int)g_mi.sites.size()) return g_mi.sites[a].label.c_str();
    return a == fl::CA_MID ? "mid" : a == fl::CA_LURK ? "lurk" : a == fl::CA_SPAWN ? "CTspawn" : a == fl::CA_TSIDE ? "Tside" : "other";
}
static int CtAreaOf(const Player &p)
{
    std::vector<Vec> sc;
    std::vector<float> sr;
    for (auto &s : g_mi.sites)
    {
        sc.push_back(s.c);
        sr.push_back(s.radius);
    }
    float dl = p.ct.setRole == "lurk" ? Dist2(p.pos, p.ct.setSpot) : 1e9f;
    return fl::CtArea(p.pos, sc, sr, g_mi.midAreas.empty() ? (g_mi.hasMid ? Dist2(p.pos, g_mi.mid) : 1e9f) : MidDist(p.pos), dl, Dist2(p.pos, g_mi.ctspawn),
                      fl::TGround(OccAt(2, p.pos), OccAt(3, p.pos)));
}
static void CtQTick(float now)
{
    if (g_ctq.done || g_phase != PH_LIVE || !g_mi.ok || g_mi.hostage || !g_matchLive) return;
    if (g_ctq.callAt < 0 && (g_plan.hitLevel >= 1 || g_plan.planted)) g_ctq.callAt = now - g_liveAt;
    if (now - g_liveAt < 20.f) return;
    g_ctq.done = true;
    std::map<std::string, std::string> at;
    int n = 0, home = 0;
    for (auto *p : Team(3, true))
    {
        if (p->ct.setRole.empty()) continue;
        n++;
        int a = CtAreaOf(*p);
        std::string an = CtAreaName(a);
        bool onRole = an == p->ct.setRole;
        home += onRole;
        at[an] += (at[an].empty() ? "" : ",") + p->name;
        std::string st = p->ent ? StateName(p->ent) : "-";
        float stockOff = st == "11MoveToState" && p->task.kind != T_NONE ? Dist(*(Vec *)(p->ent + kOffMoveTo + 4), p->issuedGoal) : -1.f;
        BLog("CTQ map=%s r=%d on=%d bot=%s role=%s at=%s home=%d dSpot=%.0f task=\"%s\" state=%s moveOff=%.0f x=%.0f y=%.0f", g_mapName.c_str(), g_roundNo,
             (int)CtQOn(), p->name.c_str(), p->ct.setRole.c_str(), an.c_str(), (int)onRole, Dist2(p->pos, p->ct.setSpot),
             p->task.kind == T_NONE ? "-" : p->task.why.c_str(), st.c_str(), stockOff, p->pos.x, p->pos.y);
    }
    if (!n) return;
    std::string cnt, who;
    for (auto &s : g_mi.sites) cnt += s.label + "=" + std::to_string(at.count(s.label) ? std::count(at[s.label].begin(), at[s.label].end(), ',') + 1 : 0) + " ";
    for (const char *k : {"mid", "lurk"}) cnt += std::string(k) + "=" + std::to_string(at.count(k) ? std::count(at[k].begin(), at[k].end(), ',') + 1 : 0) + " ";
    int away = 0;
    for (auto &kv : at)
    {
        who += kv.first + ":" + kv.second + " ";
        if (!FindSite(kv.first) && kv.first != "mid" && kv.first != "lurk") away += (int)std::count(kv.second.begin(), kv.second.end(), ',') + 1;
    }
    BLog("CTQ20 map=%s r=%d on=%d n=%d %saway=%d home=%d/%d call=%.1f plan=%s | %s", g_mapName.c_str(), g_roundNo, (int)CtQOn(), n, cnt.c_str(), away, home, n, g_ctq.callAt,
         g_ctm.setup.empty() ? "-" : g_ctm.setup.c_str(), who.c_str());
}
// a bot tells its team (either side): its own team chat (step 12D's lever), else the console say when no human is on the
// other team to overhear it, else the log only
static std::string TeamSay5c(Player *b, const std::string &text)
{
    if (!b) return "log";
    if (BotSay(*b, text)) return "chat";
    if (g_j.planSay && g_serverSay && !HumanOn(b->team == 2 ? 3 : 2))
    {
        g_serverSay(((b->team == 2 ? "[T] " : "[CT] ") + b->name + ": " + text).c_str());
        g_chat.fallback++;
        return "console";
    }
    return "log";
}
// ---- R: a site holder that sees / hears a push fights it and calls it -----------------------------------------------
// His words: "we all push long. There's a bot that was on A who watched all five of us push and didn't do anything."
// Why: step 12K drops a hold that waits to be shot (the stock hold never fires first) only for a QUIET enemy - none that
// fired in the last 1.5 s. A team that pushes shooting (at the other CTs, at a smoke) was never quiet: in his match the
// UNHOLD lines came 5-11 s after the holder first saw them. And a call came only once 3 Ts stood 700 u from the site's
// entry (part C) - a push up long, 2500 u out, was seen and not called. Now (switch brain_react):
//   fight  the hold drops for any enemy in plain view past its reaction time, unless that enemy's shots are at THIS holder
//          (then the stock hold fights back itself) - fl::HoldDrop - at its spot (tried on a holder still walking to it:
//          the stock AI then chased him down mid - worse, taken out);
//   call   2+ enemies on one of a site's ways in (fl::OnCorridor: the part within 2800 u of the site; a *mid* way in only
//          within 1400 u), at least one SEEN by a CT holder (any holder: the mid watcher calls a B push through mid too),
//          the rest seen or heard (team intel, 3 s) = that site is hit (part C's level 2: the rotations go). The holder
//          says it in team chat - "3 long" (the way in's callout, fl::CallName) - once per way in a round. On a *mid*
//          way in (it runs to both sites) it only says it ("2 mid"), no rotation - part C's calls take over at the site.
// Proof: "REACT what=call" per call (site, way in, seen / heard, who, the chat), "REACT what=watched" when a CT holder at
// its spot had an enemy in plain view 2 s without firing (on AND off: the "did nothing" number), "REACTR" per round (calls, watched,
// holds dropped for a shooting enemy).
static bool ReactOn(const Player &b) { return g_c.r && CtOn() && b.team == 3 && CtHolder(&b); }
static struct ReactS
{
    struct Sight
    {
        Vec pos;
        float t;
        std::string by;
    };
    std::map<std::string, Sight> seen;   // enemy (T) -> last sight by a CT holder
    std::set<std::string> called;        // "site:approach" called this round
    std::map<std::string, float> watchT; // CT holder -> seconds it has had an enemy in view without firing
    std::set<std::string> watchedOnce;
    int calls = 0, watched = 0, drops = 0, logged = 0;
    float next = 0;
} g_re;
static void ReactDrop() { g_re.drops++; }
static void ReactSeen(Player &p, Player &e, const Vec &at, float now)
{
    if (p.team != 3 || e.team != 2 || g_plan.planted || g_mi.hostage || !CtHolder(&p)) return;
    g_re.seen[e.name] = {at, now, p.name};
}
static void ReactTick(float now)
{
    if (g_phase != PH_LIVE || !g_mi.ok || g_mi.hostage || g_plan.planted) return;
    if (now < g_re.next && g_re.next - now < 1.f) return;
    float dt = 0.25f;
    g_re.next = now + dt;
    // the metric (on and off alike): a CT holder with an enemy in plain view 2 s without firing
    for (auto *p : Team(3, true))
    {
        if (!p->ent || !CtHolder(p) || !fl::CtAtSpot(Dist(p->pos, p->task.goal), p->task.arrive)) continue;   // a holder AT its spot
        float &w = g_re.watchT[p->name];
        bool sees = now - p->sh.lastSeen < 0.15f && now >= p->sh.lastSeen;
        if (!sees || BotAttacking(p->ent) || now - p->lastFire < 0.5f)
        {
            w = 0;
            g_re.watchedOnce.erase(p->name);
            continue;
        }
        w += dt;
        if (w >= 2.f && !g_re.watchedOnce.count(p->name))
        {
            g_re.watchedOnce.insert(p->name);
            g_re.watched++;
            Player *e = FindPl(p->seenName);
            if (g_re.logged++ < 40)
                BLog("REACT map=%s r=%d what=watched on=%d bot=%s role=%s enemy=%s d=%.0f state=%s | %s has had %s in plain view 2 s without firing", g_mapName.c_str(),
                     g_roundNo, (int)ReactOn(*p), p->name.c_str(), p->role.c_str(), e ? e->name.c_str() : "?", e ? Dist(p->fpos, e->fpos) : -1.f,
                     StateName(p->ent).c_str(), p->name.c_str(), e ? e->name.c_str() : "an enemy");
        }
    }
    if (!g_c.r || !CtOn()) return;
    for (size_t si = 0; si < g_mi.sites.size(); si++)
    {
        const Site &s = g_mi.sites[si];
        for (auto &a : s.ap)
        {
            std::string key = s.label + ":" + a.name;
            if (g_re.called.count(key)) continue;
            std::vector<Vec> pts = a.wp;
            pts.push_back(a.stack);
            pts.push_back(a.choke);
            pts.push_back(a.entry);
            bool mid = a.flags & 1;
            float near = mid ? 1400.f : 2800.f;
            int seen = 0, heard = 0;
            std::string by;
            float byT = -1;
            std::set<std::string> counted;
            for (auto &kv : g_re.seen)
            {
                Player *t = FindPl(kv.first);
                if (!t || !t->falive || now - kv.second.t > 3.f || now < kv.second.t) continue;
                if (!fl::OnCorridor(kv.second.pos, pts, s.c, near, 450.f)) continue;
                seen++;
                counted.insert(kv.first);
                if (kv.second.t > byT)
                {
                    byT = kv.second.t;
                    by = kv.second.by;
                }
            }
            for (auto &kv : g_intel[3])
            {
                Player *t = FindPl(kv.first);
                if (counted.count(kv.first) || !t || !t->falive || now - kv.second.t > 3.f || now < kv.second.t) continue;
                if (fl::OnCorridor(kv.second.pos, pts, s.c, near, 450.f)) heard++;
            }
            if (!fl::PushSeen(seen, heard)) continue;
            g_re.called.insert(key);
            g_re.calls++;
            Player *caller = FindPl(by);
            std::string place = fl::CallName(a.name);
            if (place.empty()) place = s.label;
            std::string text = std::to_string(seen + heard) + " " + place;
            int before = g_plan.hitLevel;
            if (!g_plan.planted && !mid) OnSiteHit(s, 2);   // a *mid* way in runs to both sites: said, not a site call
            std::string how = caller && caller->alive ? TeamSay5c(caller, text) : "log";
            BLog("REACT map=%s r=%d what=call site=%s ap=%s seen=%d heard=%d by=%s hit=%d text=\"%s\" chat=%s | %s sees %d on %s's way in (%s) - calls \"%s\"",
                 g_mapName.c_str(), g_roundNo, s.label.c_str(), a.name.c_str(), seen, heard, by.c_str(), (int)(g_plan.hitLevel != before && g_plan.hitSite == s.label),
                 text.c_str(), how.c_str(), by.c_str(), seen + heard, s.label.c_str(), a.name.c_str(), text.c_str());
        }
    }
}
static void ReactRoundEnd()
{
    if (!g_mi.ok || g_mi.hostage) return;
    BLog("REACTR map=%s r=%d on=%d calls=%d watched=%d drops=%d", g_mapName.c_str(), g_roundNo, g_c.r && CtOn(), g_re.calls, g_re.watched, g_re.drops);
}
// ---- S: death info ----------------------------------------------------------------------------------------------------
// His words: "If a person dies or a bot dies on a team, it should actively give the other bots information... where I died
// and who killed me... or the minimap." Before, a death moved only the nearest teammate (the step-9 trade) and turned a
// holder within 1800 u (part C); nobody else learnt where the killer was. Now (switch brain_deathinfo), on every death
// (humans' and bots'; the server log's kill line has both positions):
//   the victim's team knows the killer's spot (team intel, "seen"), and how many enemies are known within 800 u of it;
//   each living teammate bot takes it in by rank (fl::DeathFor: a Silver 55% of deaths 1.4-1.8 s late, the top every death
//   0.3-0.7 s late) and turns its angle to that spot when it is within 2500 u and not on an urgent job (a holder holds it);
//   the nearest teammate's trade run stays step 9's (a CT holder never runs to trade - part C);
//   the defenders, before the plant: a death on a site's way in (or on it) with 2+ enemies known there = that site is hit
//   (the rotations go), with one = a helper (part C's level 1) - fl::DeathCallLevel, from the first teammate that took it in;
//   one living teammate bot says it in team chat: "Ian died long, 2 there" (death_chat 0 = log only).
// Proof: "DEATHINFO" per death (victim, killer, the place, enemies there, takers, turned, the call, the chat line) and
// "DEATHR" per round.
static bool DeathOn() { return g_c.s && g_steer && g_layout == 1 && g_mi.ok && !g_mi.hostage && g_modeOk; }
// the callout of a spot: a site, a way in (the part near its site), mid, the spawns; "" = none
static std::string WherePlace(const Vec &p, int *siteWay)
{
    *siteWay = -1;
    for (size_t i = 0; i < g_mi.sites.size(); i++)
        if (Dist2(p, g_mi.sites[i].c) <= g_mi.sites[i].radius + 250.f && fabsf(p.z - g_mi.sites[i].c.z) < 350.f)
        {
            *siteWay = (int)i;
            return g_mi.sites[i].label + " site";
        }
    std::string best;
    float bd = 1e9f;
    for (size_t i = 0; i < g_mi.sites.size(); i++)
    {
        const Site &s = g_mi.sites[i];
        for (auto &a : s.ap)
        {
            std::vector<Vec> pts = a.wp;
            pts.push_back(a.stack);
            pts.push_back(a.choke);
            pts.push_back(a.entry);
            bool mid = a.flags & 1;
            if (!fl::OnCorridor(p, pts, s.c, mid ? 1400.f : 2800.f, 450.f)) continue;
            float d = Dist2(p, s.c) + (mid ? 800.f : 0.f);   // a real way in first, the nearest site first
            if (d < bd)
            {
                bd = d;
                best = fl::CallName(a.name);
                if (best.empty()) best = "near " + s.label;
                *siteWay = mid ? -1 : (int)i;   // a *mid* way in runs to both sites: no site call from it
            }
        }
    }
    if (!best.empty()) return best;
    if (NearMid(p)) return "mid";
    if (Dist2(p, g_mi.ctspawn) < 700.f) return "CT";
    if (Dist2(p, g_mi.tspawn) < 700.f) return "T spawn";
    const Site *ns = NearestSite(p);
    return ns ? "near " + ns->label : "";
}
static struct DeathS
{
    struct Pend
    {
        std::string bot, killer;
        Vec kpos;
        float due;
    };
    std::vector<Pend> pend;
    struct Call
    {
        int site, level;
        float due;
        std::string victim;
    };
    std::vector<Call> calls;
    int deaths = 0, takers = 0, turned = 0, called = 0, said = 0;
    float lastSay[4] = {-100, -100, -100, -100};
} g_di;
static void DeathInfo(const char *msg, const std::string &vname, int vt, const Vec &vpos)
{
    if (g_phase != PH_LIVE || (vt != 2 && vt != 3)) return;
    std::string kname, kteam;
    if (!ParseActor(msg, kname, kteam)) return;
    const char *q = strstr(msg, "\" [");
    Vec kp{};
    if (!q || sscanf(q + 2, "[%f %f %f]", &kp.x, &kp.y, &kp.z) != 3) return;
    int kt = kteam == "CT" ? 3 : kteam == "TERRORIST" ? 2 : 0;
    if (kt == vt || !kt) return;   // a team kill / the world
    float now = CurTime();
    g_di.deaths++;
    if (!DeathOn())
    {
        BLog("DEATHINFO map=%s r=%d on=0 victim=%s team=%s killer=%s", g_mapName.c_str(), g_roundNo, vname.c_str(), vt == 2 ? "T" : "CT", kname.c_str());
        return;
    }
    IntelPut(vt, kname, kp, now, 2);
    int there = 0;
    for (auto &kv : g_intel[vt])
    {
        Player *e = FindPl(kv.first);
        if (e && e->alive && now - kv.second.t < 5.f && now >= kv.second.t && Dist(kv.second.pos, kp) < 800.f) there++;
    }
    there = std::max(there, 1);
    int way = -1;
    std::string place = WherePlace(kp, &way);
    // each teammate bot takes it in by rank; the first to take it in makes the defenders' call
    int takers = 0;
    float first = 1e9f;
    Player *sayer = nullptr;
    float sd = 1e9f;
    for (auto *p : Team(vt, true))
    {
        if (p->name == vname || !p->ent) continue;
        if (Dist(p->pos, vpos) < sd)
        {
            sd = Dist(p->pos, vpos);
            sayer = p;
        }
        fl::DeathReact r = fl::DeathFor(p->s, Rnd(), Rnd());
        if (!r.takes) continue;
        takers++;
        first = std::min(first, r.delay);
        g_di.pend.push_back({p->name, kname, kp, now + r.delay});
    }
    g_di.takers += takers;
    int level = vt == 3 && !g_plan.planted && takers ? fl::DeathCallLevel(there, way >= 0) : 0;
    if (level) g_di.calls.push_back({way, level, now + first, vname});
    std::string text = fl::DeathText(vname, place.empty() ? "somewhere" : place, there), how = "-";
    if (g_c.sChat && sayer && now - g_di.lastSay[vt] > 2.f)
    {
        g_di.lastSay[vt] = now;
        how = TeamSay5c(sayer, text);
        g_di.said++;
    }
    BLog("DEATHINFO map=%s r=%d on=1 victim=%s team=%s killer=%s place=%s there=%d takers=%d first=%.2f call=%s%s by=%s text=\"%s\" chat=%s", g_mapName.c_str(),
         g_roundNo, vname.c_str(), vt == 2 ? "T" : "CT", kname.c_str(), place.empty() ? "-" : place.c_str(), there, takers, takers ? first : -1.f,
         level ? g_mi.sites[way].label.c_str() : "-", level == 2 ? "(hit)" : level == 1 ? "(help)" : "", sayer ? sayer->name.c_str() : "-", text.c_str(), how.c_str());
}
static void DeathTick(float now)
{
    if (g_phase != PH_LIVE)
    {
        g_di.pend.clear();
        g_di.calls.clear();
        return;
    }
    for (auto it = g_di.pend.begin(); it != g_di.pend.end();)
    {
        if (now < it->due && it->due - now < 5.f) { ++it; continue; }
        Player *p = FindPl(it->bot);
        if (p && p->bot && p->alive && p->ent && DeathOn() && !BotAttacking(p->ent) && !Urgent(*p, BotTask(p->ent)) && Dist(p->fpos, it->kpos) < 2500.f &&
            LookWant(*p, LK_HEAR, {it->kpos.x, it->kpos.y, it->kpos.z + 60.f}, 600.f, 2.5f, "death info"))
            g_di.turned++;
        it = g_di.pend.erase(it);
    }
    for (auto it = g_di.calls.begin(); it != g_di.calls.end();)
    {
        if (now < it->due && it->due - now < 5.f) { ++it; continue; }
        if (DeathOn() && !g_plan.planted && it->site >= 0 && it->site < (int)g_mi.sites.size())
        {
            int before = g_plan.hitLevel;
            std::string was = g_plan.hitSite;
            OnSiteHit(g_mi.sites[it->site], it->level);
            if (g_plan.hitLevel != before || g_plan.hitSite != was)
            {
                g_di.called++;
                BLog("plan: CT call from %s's death: %s %s", it->victim.c_str(), g_mi.sites[it->site].label.c_str(), it->level == 2 ? "hit" : "- a helper goes");
            }
        }
        it = g_di.calls.erase(it);
    }
}
// ---- T: the call, the bomb carrier and every T's route agree -------------------------------------------------------
// His words: "the bots called B to A, but then they went mid to A. So that was the wrong call." And round 1: "the bot with
// the bomb called B. I followed him, the other three bots rushed mid, then ran back up B."
// Why: in his match "B push" (the tunnels way, mid share 0) had 3 and 4 of 5 Ts through mid (TPR midTs=3/5, 4/5), and
// "B to A" 4 of 5. Traced in a bot-only match (brain_diag): a T on "B push" walks its waypoints to upper tunnels, then -
// still heading for B - turns back through lower tunnels, mid and B doors: step 9 gives the Ts' gather / go-to-site moves the
// game's SAFEST route, which re-plans round the first contact at the tunnel exit - through mid ("the other three bots
// rushed mid, then ran back up B") - and the game's path cost adds a penalty for every nav area a TEAMMATE stands in, so
// a team bunched in the tunnel sends the ones behind round through mid even on the fastest route. Now every T on the called
// plan walks the FASTEST route along its group's way (short hops along it were tried in wave l and removed: the CT
// setup fell apart - 2A+2B 69% -> 27% - and the call/route mismatch did not move). And
// each T was given its approach's waypoints, but step 9's Steer counts a bot as "on its way" while it is
// in MoveToState and the last goal WE issued is still the same - it never looks at the goal the MoveTo state really has.
// The stock AI re-targets its own MoveTo (a noise it heard, a hunt, the bomber's favourite site) and then walks there -
// down mid - with our plan still "issued". The same hole let a CT holder walk off to the T side with its setup task still
// on (part Q's CTQ lines: moveOff = how far the state's own goal is from ours). Now (switch brain_callroute for the Ts;
// brain_ctq's ctq_route for the CT holders): a bot whose MoveTo state heads more than 300 u from our goal is re-issued
// (Steer, once a second at most; such a re-issue never counts towards step 9's "cannot reach it, give up after 40" - an
// early 5c build counted it and so set Ts free to the stock AI) - so every T walks its group's way from freeze end, and the
// bomb carrier (a bot: it plays the main group, step 12I) walks the called way too. A human carrier still overrules (part H: the team follows him).
// Proof: "TROUTE" per round (on AND off: the plan, its way in, Ts that went through mid when their group's way does not,
// where the bomb carrier went, re-issues), "TMISMATCH" per T bot the first time it is somewhere its plan does not go (its
// role, group, the plan's way, the mid place, its task, the state's own goal offset).
static bool TPlanRole(const Player &p)
{
    if (!T12() || !p.bot || p.team != 2 || g_plan.planted || p.task.kind == T_NONE) return false;
    return p.role == "group" || p.role == "group2" || p.role == "trickle" || p.role == "lurk" || p.role == "escort";
}
static struct TRouteS
{
    std::set<std::string> mid, mism;
    std::map<std::string, int> site;   // T -> the site whose way in it was first seen on (-1 none)
    std::string carrier;               // the bomb carrier at freeze end
    int carrierBot = -1;
    bool carrierSet = false;
    int logged = 0;
    float next = 0;
} g_tr;
static bool InMidArea(const Vec &p, std::string *place)
{
    for (auto &m : g_mi.midAreas)
        if (p.x >= m.x0 - 16.f && p.x <= m.x1 + 16.f && p.y >= m.y0 - 16.f && p.y <= m.y1 + 16.f && fabsf(p.z - m.z) < 150.f)
        {
            if (place) *place = m.place;
            return true;
        }
    return false;
}
// may this T be in mid under the plan? its group's way in (or the fake's) goes through mid, or it is off-plan anyway
static bool MidAllowed(const Player &p)
{
    const fl::TPlan &P = g_tp.plan;
    // a lurker's spot, the human carrier's way, a trade run (step 9: to a teammate's death) are not the call's route
    if (p.role == "lurk" || p.role == "escort" || p.role == "trade" || !p.bot) return true;
    if (P.site < 0 || P.site >= (int)g_mi.sites.size()) return true;
    const Site &gs = P.type == fl::PL_FAKE && P.from >= 0 && P.from < (int)g_mi.sites.size() ? g_mi.sites[P.from] : g_mi.sites[P.site];
    const Approach &a = ApOf(gs, p.t12.ap);
    if (a.flags & 1) return true;
    // on its own way in (dust2's long route runs through the "TopofMid" place by the T spawn)
    std::vector<Vec> pts = {g_mi.tspawn};
    pts.insert(pts.end(), a.wp.begin(), a.wp.end());
    pts.push_back(a.stack);
    pts.push_back(a.choke);
    pts.push_back(a.entry);
    if (fl::OnCorridor(p.pos, pts, gs.c, 1e9f, 500.f)) return true;
    // a fake: from its first site on to the target the way runs where it runs (dust2: B tunnels -> A goes through mid)
    if (P.type == fl::PL_FAKE) return (g_plan.gatherUntil > 0 && CurTime() >= g_plan.gatherUntil) || (!p.task.via.empty() && p.task.wp >= p.task.via.size());
    return false;
}
static void TRouteTick(float now)
{
    if (g_phase != PH_LIVE || !g_mi.ok || g_mi.hostage || !g_tp.active || g_plan.planted || now - g_liveAt > 45.f || !g_matchLive) return;
    if (now < g_tr.next && g_tr.next - now < 1.f) return;
    g_tr.next = now + 0.25f;
    if (!g_tr.carrierSet)
    {
        g_tr.carrierSet = true;
        g_tr.carrier = g_bomber;
        Player *b = FindPl(g_bomber);
        g_tr.carrierBot = b ? (int)b->bot : -1;
    }
    for (auto *p : Team(2, false))
    {
        if (!g_tr.site.count(p->name))
        {
            int way = -1;
            std::string pl = WherePlace(p->pos, &way);
            if (way >= 0 && pl != "mid") g_tr.site[p->name] = way;
        }
        std::string place;
        if (now - g_liveAt < 8.f || !InMidArea(p->pos, &place) || Dist2(p->pos, g_mi.tspawn) < 700.f) continue;   // still leaving the spawn
        g_tr.mid.insert(p->name);
        if (MidAllowed(*p) || g_tr.mism.count(p->name)) continue;
        g_tr.mism.insert(p->name);
        std::string st = p->ent ? StateName(p->ent) : "-";
        float off = p->ent && st == "11MoveToState" && p->task.kind != T_NONE ? Dist(*(Vec *)(p->ent + kOffMoveTo + 4), p->issuedGoal) : -1.f;
        if (g_tr.logged++ < 20)
            BLog("TMISMATCH map=%s r=%d on=%d bot=%s role=%s group=%d plan=\"%s\" ap=%s place=%s task=\"%s\" state=%s moveOff=%.0f t=%.1f | %s is in %s - the plan does not go there",
                 g_mapName.c_str(), g_roundNo, (int)g_c.t, p->name.c_str(), p->role.c_str(), p->t12.group, g_tp.text.c_str(),
                 ApName(g_tp.plan.type == fl::PL_FAKE ? g_tp.plan.from : g_tp.plan.site, p->t12.ap).c_str(), place.c_str(),
                 p->task.kind == T_NONE ? "-" : p->task.why.c_str(), st.c_str(), off, now - g_liveAt, p->name.c_str(), place.c_str());
    }
}
static void TRouteRoundEnd()
{
    if (!g_mi.ok || g_mi.hostage || !g_tp.active || !g_matchLive) return;
    std::string cs = "-";
    auto c = g_tr.site.find(g_tr.carrier);
    if (!g_tr.carrier.empty()) cs = c != g_tr.site.end() && c->second >= 0 ? g_mi.sites[c->second].label : "?";
    int bots = (int)Team(2, true, false).size();
    BLog("TROUTE map=%s r=%d on=%d plan=\"%s\" site=%s mid=%d/%d mismatch=%d carrier=%s carrierBot=%d carrierWent=%s retargetT=%d retargetCT=%d", g_mapName.c_str(),
         g_roundNo, (int)g_c.t, g_tp.text.c_str(), g_mi.sites[g_tp.plan.site].label.c_str(), (int)g_tr.mid.size(), bots, (int)g_tr.mism.size(),
         g_tr.carrier.empty() ? "-" : g_tr.carrier.c_str(), g_tr.carrierBot, cs.c_str(), g_retargetT, g_retargetCT);
}
// ---- U: defenders face the way in they hold; rotations run -----------------------------------------------------------
// His words: "looking weird directions, walking real slow."
// Why (measured): a holder at its spot is the stock HideState, which looks round the spot's "approach points" (any
// direction the nav says an enemy could come from, the CT side too): 5b CT holders faced the way in they hold ~31-36% of
// their holding time. And the slow rotations were the route, not the walk: part N's "sneaky" switch to the game's
// SAFEST route went the long way round - rotations / retakes took a median 16-17 s to arrive against 2-14 s the direct
// way; the stock walk is only ~16% of CT rotation time (5b and 5c alike). Now (switch brain_ctface):
//   face  a CT holder at its spot (hiding, not fighting, nothing in view) turns to the way in it holds and can see from
//         there: a site holder its site's entrances / chokes, the mid watcher a mid area, the lurker a choke - one the
//         Ts reach earliest (fl::FacePick; a random one of those within 3 s, so the angle varies) - through the look
//         controller at the lowest priority (hearing, fire, pre-aim, utility all win); ctface_face 0 = the stock look;
//   way   a rotation / retake takes the direct way (never part N's safest-route switch); ctface_way 0 = part N's.
// Tried and taken out: forcing a run with the bot's own run flag (CBot::m_isRunning - found at run time at 0x3bc0, the
// byte before the crouch flag, 100% agreement with m_bIsWalking): the stock AI sets it again every frame before it moves
// (only 7 of 55 self-checked writes stopped a walk), so it did nothing.
// Proof: "UFACE" per CT holder per round (seconds held at its spot, share of it facing its way in within 30 deg - on AND
// off, the "weird directions" number); "UMOVE" per round (CT seconds moving on rotations / retakes and to their spots, and
// how much of it walking - on AND off); part N's "ROT" lines (arrive = seconds to get there, sneaky = the safest route).
static bool FaceOn() { return g_c.u && CtOn(); }
static bool URunOn() { return FaceOn() && g_c.uRun; }
static bool FaceTarget(Player &b, Vec &out)
{
    const Vec &g = b.task.goal;
    Vec eye{g.x, g.y, g.z + 64.f};
    if (b.ct.hand && Dist(g, b.ct.setSpot) < 1.f)   // JOB 5d: his spot - look where he looked when he placed it
    {
        float yr = b.ct.handY * 0.0174533f, pr = b.ct.handP * 0.0174533f;
        out = {eye.x + 1000.f * cosf(pr) * cosf(yr), eye.y + 1000.f * cosf(pr) * sinf(yr), eye.z - 1000.f * sinf(pr)};
        return true;
    }
    std::vector<Vec> c;
    if (const Site *s = FindSite(b.role))
        for (auto &a : s->ap)
        {
            c.push_back(a.entry);
            c.push_back(a.choke);
        }
    else if (b.role == "mid")
    {
        std::vector<std::pair<float, Vec>> m;
        for (auto &a : g_mi.midAreas)
        {
            Vec v{0.5f * (a.x0 + a.x1), 0.5f * (a.y0 + a.y1), a.z};
            float d = Dist(v, g);
            if (d > 300.f && d < 2500.f) m.push_back({d, v});
        }
        std::sort(m.begin(), m.end(), [](const std::pair<float, Vec> &x, const std::pair<float, Vec> &y) { return x.first < y.first; });
        for (size_t i = 0; i < m.size() && i < 12; i++) c.push_back(m[i].second);
    }
    else if (b.role == "lurk")
        for (auto &s : g_mi.sites)
            for (auto &a : s.ap)
                if (Dist(a.choke, g) < 1800.f) c.push_back(a.choke);
    std::vector<char> vis;
    std::vector<int> occ;
    for (auto &v : c)
    {
        vis.push_back(Dist(v, g) > 150.f && Los(eye, {v.x, v.y, v.z + 48.f}) ? 1 : 0);
        occ.push_back(OccAt(2, v));
    }
    int k = fl::FacePick(vis, occ, Rnd());
    if (k < 0) return false;
    out = {c[k].x, c[k].y, c[k].z + 48.f};
    return true;
}
static int g_faceLogged;
static void UFrame(Player &b, float now, float dt)
{
    Player::Fc &f = b.fc;
    if (!b.bot || b.team != 3 || !b.falive || !b.ent || g_phase != PH_LIVE || g_mi.hostage || !g_mi.ok) return;
    bool attack = BotAttacking(b.ent);
    // the walking metric: a rotation / retake, or the walk to its setup spot
    bool toSpot = CtHolder(&b) && b.task.kind == T_HOLD && Dist(b.pos, b.task.goal) > b.task.arrive + 150.f;
    bool urgent = RotUrgent(b) || b.role == "retake" || b.task.why == "CT retake";
    bool moving = b.hsp > 30.f && !attack;
    bool walking = moving && IsWalking(b);
    if (moving && (urgent || toSpot))
    {
        (urgent ? f.moveT : f.spotMoveT) += dt;
        if (walking) (urgent ? f.walkT : f.spotWalkT) += dt;
    }
    // face: at its spot, hiding, nothing in view
    if (now < f.next && f.next - now < 1.f) return;
    f.next = now + 0.25f;
    bool atSpot = CtHolder(&b) && Dist(b.fpos, b.task.goal) < b.task.arrive + 80.f && b.hsp < 40.f && !attack && StateName(b.ent) == "9HideState";
    if (!atSpot) return;
    if (Dist(f.goal, b.task.goal) > 1.f)
    {
        f.goal = b.task.goal;
        f.have = FaceTarget(b, f.tgt);
    }
    if (!f.have) return;
    f.holdT += 0.25f;
    float yaw;
    if (EyeYaw(b, &yaw) && fabsf(Wrap180(YawTo(EyeOf(b.ent, b.fpos), f.tgt) - yaw)) < 30.f) f.okT += 0.25f;
    if (FaceOn() && g_c.uFace && now - b.sh.lastSeen > 1.f) LookWant(b, LK_PREAIM, f.tgt, 2000.f, 0.6f, "face");
}
static void URoundEnd()
{
    if (!g_mi.ok || g_mi.hostage) return;
    float mv = 0, wk = 0, smv = 0, swk = 0;
    for (auto &kv : g_pl)
    {
        Player &p = kv.second;
        if (!p.bot || p.team != 3) continue;
        mv += p.fc.moveT;
        wk += p.fc.walkT;
        smv += p.fc.spotMoveT;
        swk += p.fc.spotWalkT;
        if (p.fc.holdT > 0 && g_faceLogged++ < 60)
            BLog("UFACE map=%s r=%d on=%d bot=%s role=%s holdT=%.1f face=%.0f%%", g_mapName.c_str(), g_roundNo, (int)(FaceOn() && g_c.uFace), p.name.c_str(),
                 p.role.c_str(), p.fc.holdT, 100.f * p.fc.okT / p.fc.holdT);
    }
    BLog("UMOVE map=%s r=%d on=%d rotT=%.1f rotWalk=%.0f%% spotT=%.1f spotWalk=%.0f%%", g_mapName.c_str(), g_roundNo, (int)URunOn(), mv,
         mv > 0 ? 100.f * wk / mv : -1.f, smv, smv > 0 ? 100.f * swk / smv : -1.f);
}
static void J5cRoundStart()
{
    g_ctq = CtQ();
    g_re = ReactS();
    g_di = DeathS();
    g_tr = TRouteS();
    g_retargetCT = g_retargetT = 0;
    g_faceLogged = 0;
    for (auto &kv : g_pl) kv.second.fc = Player::Fc();
    for (auto &kv : g_pl) kv.second.ct.setRole.clear();
}
static void J5cRoundEnd()
{
    if (!g_matchLive) return;   // the warmup is not a round
    ReactRoundEnd();   // R
    TRouteRoundEnd();  // T
    URoundEnd();       // U
    if (g_mi.ok && !g_mi.hostage)
        BLog("DEATHR map=%s r=%d on=%d deaths=%d takers=%d turned=%d calls=%d said=%d", g_mapName.c_str(), g_roundNo, (int)DeathOn(), g_di.deaths, g_di.takers,
             g_di.turned, g_di.called, g_di.said);
}
static void J5cFrame(float now)
{
    static float last;
    float dt = now - last;
    last = now;
    if (dt < 0 || dt > 0.5f) dt = 0;
    CtQTick(now);    // Q
    ReactTick(now);  // R
    DeathTick(now);  // S
    TRouteTick(now); // T
    for (auto &kv : g_pl) UFrame(kv.second, now, dt);   // U
}
// ==== end JOB 5c ====================================================================================================

// ---- entry points (family_party.cpp) ----
void BrainFactories(BrainIfaceFn engineFactory, BrainIfaceFn serverFactory, double (*testValue)(const char *, double))
{
    g_tv = testValue;
    FindNetProps(serverFactory);
    HookEvents(engineFactory);
    ShootInit(engineFactory);   // STEP 11
    ChatInit(serverFactory);    // STEP 12D: can a bot speak in team chat (found and checked, never guessed)
    CmdInit(serverFactory);     // JOB 5b O: can a bot buy / switch to its grenades (the same way: found and checked)
}
void BrainUnload()
{
    CrouchRestoreAll();   // JOB 5b Q
    FootRestoreAll();
    ShootRestoreAll();   // STEP 11: every bot profile back to its botprofile.db values
    if (g_evMgr)
        VF<void (*)(void *, void *)>(g_evMgr, 6)(g_evMgr, &g_evL);
    g_evMgr = nullptr;
}
void BrainLoad(void *(*playerInfo)(edict_t *), void *playerInfoMgr, double (*eloOf)(const char *))
{
    g_pi = playerInfo;
    g_eloOf = eloOf;
    if (playerInfoMgr)
        g_globals = VF<void *(*)(void *)>(playerInfoMgr, 1)(playerInfoMgr);
    srand((unsigned)time(nullptr) ^ 0x5bd1e995u);
    BLog("brain loaded");
}
static void LoadHandSpots(const std::string &map);   // JOB 5d (below)
void BrainLevelInit(const char *map)
{
    // keep the log small: over 4 MB it becomes family_brain.log.old (one generation)
    if (FILE *f = fopen(kBrainLog, "r"))
    {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fclose(f);
        if (n > 4 * 1024 * 1024)
            rename(kBrainLog, (std::string(kBrainLog) + ".old").c_str());
    }
    g_mapName = map ? map : "";
    LoadHandSpots(g_mapName);   // JOB 5d
    g_layout = 0;
    g_moneyOff = 0;
    g_moneyTries = 0;
    g_masked.clear();
    g_money.clear();
    g_teamOf.clear();
    g_pl.clear();
    g_phase = PH_NONE;
    g_matchLive = false;
    g_plan = Plan();
    g_rm = RoundM();
    g_bomber.clear();
    LoadMapInfo(g_mapName);
    // STEP 10
    LoadDials();
    g_byUid.clear();
    g_vmTrace.clear();
    g_fwLast = 0;
    g_matchSeed = Fnv(g_mapName + ":" + std::to_string((long)time(nullptr)));
    ShootLevelInit();   // STEP 11
    J5LevelInit();      // STEP 12
}
void BrainSetDiag(int d) { g_diag = d; }
void BrainSetSteer(int on) { g_steer = on != 0; }
void BrainSetMode(int type, int mode)
{
    bool ok = type < 0 || (type == 0 && mode >= 0 && mode <= 2);   // unknown (still reading) counts as classic
    if (ok != g_modeOk)
        BLog("game mode %d/%d: team play %s", type, mode, ok ? "on" : "off (not a classic mode)");
    g_modeOk = ok;
}

void BrainFrame(const std::set<edict_t *> &clients)
{
    FootFrame();   // STEP 10: every frame (the speed scale is re-applied each tick)
    ShootFrame();  // STEP 11: every frame (profile values, recoil, panic turn, sight metrics)
    Job5Frame();   // STEP 12: every frame (hearing, the look controller, the job-5 parts)
    static float next;
    float now = CurTime();
    if (now < next - 60.f)
        next = 0;   // curtime restarted (new level)
    if (now < next)
        return;
    next = now + 0.25f;
    std::set<std::string> seen;
    for (edict_t *e : clients)
    {
        void *pi = g_pi ? g_pi(e) : nullptr;
        if (!pi)
            continue;
        const char *nm = PI_Name(pi);
        if (!nm || !*nm)
            continue;
        Player &p = g_pl[nm];
        p.name = nm;
        p.e = e;
        p.team = PI_Team(pi);
        p.alive = (p.team == 2 || p.team == 3) && !PI_Dead(pi) && !PI_Observer(pi);
        p.pos = PI_Origin(pi);
        void *bot = BotEntity(e);
        if (bot && !p.bot)
        {
            p.s = Smart(nm);
            p.bot = true;   // STEP 10: this round's dials for a bot that joined mid-round
            p.fw = DialOf(p, D_FOOT, true);
            p.aim = DialOf(p, D_AIM, false);
            p.elo = g_eloOf ? (float)g_eloOf(nm) : 0.f;
            p.walker = Rnd() < p.fw;
            p.safe = Rnd() < p.fw;    // STEP 10b
            p.sh.react = DialOf(p, D_REACT, false);   // STEP 11
            p.sh.spray = DialOf(p, D_SPRAY, false);
            p.sh.aimd = DialOf(p, D_AIM, false);
            p.hr.h = DialOf(p, D_AWARE, false);       // STEP 12B
            p.fr.g = 0.5f * (DialOf(p, D_FOOT, false) + DialOf(p, D_REACT, false));   // STEP 12G
        }
        p.bot = bot != nullptr;
        p.ent = (uintptr_t)bot;
        p.pent = bot ? (uintptr_t)bot : PlayerEnt(e);   // STEP 12: humans' entity too
        seen.insert(nm);
        if (p.bot && g_layout == 0)
            g_layout = VerifyLayout(p.ent) ? 1 : -1;
    }
    for (auto it = g_pl.begin(); it != g_pl.end();)
        it = seen.count(it->first) ? std::next(it) : g_pl.erase(it);
    FindMoneyOffset();
    SampleMetrics();
    if (g_diag && g_layout == 1)
    {
        static float nd;
        if (now > nd || now < nd - 60)
        {
            nd = now + 3.f;
            for (auto &kv : g_pl)
                if (kv.second.bot && kv.second.alive)
                    BLog("DIAG %s %s s=%.2f role %s task %d (%.0f %.0f %.0f) at (%.0f %.0f %.0f) state %s w3ea0 %08x w3ea4 %d w3ea8 %08x bomber %d", kv.first.c_str(),
                         kv.second.team == 2 ? "T" : "CT", kv.second.s, kv.second.role.c_str(), kv.second.task.kind, kv.second.task.goal.x,
                         kv.second.task.goal.y, kv.second.task.goal.z, kv.second.pos.x, kv.second.pos.y, kv.second.pos.z,
                         StateName(kv.second.ent).c_str(), *(uint32_t *)(kv.second.ent + 0x3ea0), *(int *)(kv.second.ent + 0x3ea4),
                         *(uint32_t *)(kv.second.ent + 0x3ea8), kv.first == g_bomber);
        }
    }
    if (g_layout != 1 || !g_steer || !g_modeOk)
        return;
    TeamBuy();
    UtilFreezeTick(now);   // JOB 5b O: util buys a moment into the freeze (after the bots' own)
    TickPlan();
    for (auto &kv : g_pl)
        if (kv.second.bot)
            Steer(kv.second);
}

// ==== JOB 5d (09-29, his ask): hand-placed spots, set from the in-game console ======================================
// His words: "set up a ton of different spots on every single site ... the command will be something like
// Bot_ASiteSpot1, Bot_BSiteSpot2". A player stands where a bot should hold, aims where it should look and types
//   bot_asitespot<N> | bot_bsitespot<N> | bot_midspot<N>      save (or move) spot N there, with the aim and the crouch
//   bot_deleteasitespot<N> | bot_deletebsitespot<N> | bot_deletemidspot<N>   remove spot N
//   bot_spots                                                   how many spots this map has
// (the number may also be a separate word: "bot_asitespot 3"). The CS:GO client forwards a command it does not know to
// the server, where family_party hands it here. File: csgo/addons/family_spots/<map>.txt, one line per spot:
//   <A|B|MID> <n> <x> <y> <z> <pitch> <yaw> <crouch>
// Every save rewrites the file and answers in chat ("say") so he sees it landed.
static const char *kSpotDir = "csgo/addons/family_spots";
struct HandSpot
{
    std::string site;
    int n = 0;
    Vec p{};
    float pitch = 0, yaw = 0;
    int crouch = 0;
};
static std::vector<HandSpot> g_hand;   // this map's
static std::string g_handMap;
static std::string HandFile(const std::string &map) { return std::string(kSpotDir) + "/" + map + ".txt"; }
static void LoadHandSpots(const std::string &map)
{
    g_hand.clear();
    g_handMap = map;
    FILE *f = fopen(HandFile(map).c_str(), "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        HandSpot h;
        char site[8] = "";
        if (sscanf(line, "%7s %d %f %f %f %f %f %d", site, &h.n, &h.p.x, &h.p.y, &h.p.z, &h.pitch, &h.yaw, &h.crouch) == 8)
        {
            h.site = site;
            g_hand.push_back(h);
        }
    }
    fclose(f);
    BLog("SPOTS map=%s loaded %d hand-placed spot(s)", map.c_str(), (int)g_hand.size());
}
static bool SaveHandSpots()
{
    mkdir(kSpotDir, 0755);
    std::sort(g_hand.begin(), g_hand.end(), [](const HandSpot &a, const HandSpot &b) { return a.site != b.site ? a.site < b.site : a.n < b.n; });
    std::string path = HandFile(g_handMap), tmp = path + ".new";
    FILE *f = fopen(tmp.c_str(), "w");
    if (!f)
        return false;
    for (auto &h : g_hand)
        fprintf(f, "%s %d %.1f %.1f %.1f %.1f %.1f %d\n", h.site.c_str(), h.n, h.p.x, h.p.y, h.p.z, h.pitch, h.yaw, h.crouch);
    fclose(f);
    return rename(tmp.c_str(), path.c_str()) == 0;
}
static void SpotSay(const char *fmt, ...)
{
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    BLog("SPOTS %s", buf);
    if (g_serverSay)
        g_serverSay(buf);
}
static int HandCount(const std::string &site)
{
    int c = 0;
    for (auto &h : g_hand) c += h.site == site;
    return c;
}
// true = a spot command (handled here, the game does not see it)
bool BrainClientCommand(edict_t *e, int argc, const char *const *argv)
{
    if (argc < 1 || !argv[0] || strncasecmp(argv[0], "bot_", 4) != 0)
        return false;
    std::string c;
    for (const char *s = argv[0] + 4; *s; s++) c += (char)tolower((unsigned char)*s);
    if (g_handMap != g_mapName)
        LoadHandSpots(g_mapName);
    if (c == "spots")
    {
        SpotSay("%s spots: A %d, B %d, mid %d", g_mapName.c_str(), HandCount("A"), HandCount("B"), HandCount("MID"));
        return true;
    }
    bool del = c.compare(0, 6, "delete") == 0;
    if (del) c = c.substr(6);
    std::string site;
    size_t k = 0;
    if (c.compare(0, 9, "asitespot") == 0) site = "A", k = 9;
    else if (c.compare(0, 9, "bsitespot") == 0) site = "B", k = 9;
    else if (c.compare(0, 7, "midspot") == 0) site = "MID", k = 7;
    else return false;
    std::string num = c.substr(k);
    if (num.empty() && argc >= 2 && argv[1]) num = argv[1];
    int n = atoi(num.c_str());
    if (n <= 0 || n > 999 || num.find_first_not_of("0123456789") != std::string::npos)
    {
        SpotSay("Spot needs a number, like bot_%ssitespot1", site == "MID" ? "mid" : site == "A" ? "a" : "b");
        return true;
    }
    const char *nice = site == "MID" ? "Mid" : site.c_str();
    auto it = std::find_if(g_hand.begin(), g_hand.end(), [&](const HandSpot &h) { return h.site == site && h.n == n; });
    if (del)
    {
        if (it == g_hand.end())
        {
            SpotSay("%s spot %d does not exist", nice, n);
            return true;
        }
        g_hand.erase(it);
        bool ok = SaveHandSpots();
        SpotSay(ok ? "%s spot %d deleted (%d left on %s)" : "%s spot %d deleted but NOT saved to disk (%d left on %s)", nice, n,
                HandCount(site), nice);
        return true;
    }
    void *pi = g_pi ? g_pi(e) : nullptr;
    uintptr_t ent = PlayerEnt(e);
    if (!pi || !ent || PI_Dead(pi) || PI_Observer(pi))
    {
        SpotSay("Spot not saved: be alive on the map to place one");
        return true;
    }
    HandSpot h;
    h.site = site;
    h.n = n;
    h.p = PI_Origin(pi);
    if (g_np.eyeP > 0 && g_np.eyeY > 0)
    {
        h.pitch = *(float *)(ent + g_np.eyeP);
        h.yaw = *(float *)(ent + g_np.eyeY);
    }
    if (g_np.flags > 0) h.crouch = (*(int *)(ent + g_np.flags) & 2) ? 1 : 0;   // FL_DUCKING
    bool moved = it != g_hand.end();
    if (moved) *it = h;
    else g_hand.push_back(h);
    bool ok = SaveHandSpots();
    SpotSay("%s spot %d %s%s%s (%d on %s)", nice, n, moved ? "moved here" : "saved", h.crouch ? ", crouching" : "",
            ok ? "" : " - NOT saved to disk", HandCount(site), nice);
    return true;
}
// ==== end JOB 5d spots ===============================================================================================

// ==== JOB 5d (09-29, his rule): the defenders' setup - 2 on A, 2 on B, 1 watching mid. Nothing else. ================
// His words: "Defenders should just have two on A, two on B, one watching mid. Period ... If I'm a defender, the bots are
// the two that are on A and B. If I'm playing with one other person, the bots are still on A and B. They'll never be the
// lurker. The lurker is always only a bot if there's a team of bots." So the bots fill A, B, A, B in that order; the fifth
// (only when no human is a defender) watches mid. Each bot takes one of HIS spots for its site (bot_asitespot<N> ...), a
// different one from its teammates, mostly not last round's; it looks where he looked. A site with too few of his spots
// is filled from the worked-out spots (5b's), so a map he has not done yet still sets up 2-2-1.
// Switch: brain_hand 0 = job 5c's setup again. Proof lines: CTSPOT (hand=1 for his spots) and CTSET, as before.
static bool PlanCT5d()
{
    if (!g_tv || !(int)g_tv("brain_hand", 1)) return false;
    if (g_handMap != g_mapName) LoadHandSpots(g_mapName);
    const Site *sa = FindSite("A"), *sb = FindSite("B");
    if (!sa || !sb) return false;   // not a two-site bomb map: the old setup
    auto bots = Team(3, true);
    int n = (int)bots.size(), humans = (int)Team(3, false).size() - n;
    if (!n) return true;
    if (!g_cc.built) BuildCtCands();
    int ia = (int)(sa - &g_mi.sites[0]), ib = (int)(sb - &g_mi.sites[0]);
    // roles in fill order: A, B, A, B, then mid (a team of bots only) - a 5th bot with a human on CT cannot happen (cap 5)
    std::vector<std::string> roles;
    for (int i = 0; i < n; i++)
        roles.push_back(i == 4 && humans == 0 ? "MID" : (i % 2 == 0 ? "A" : "B"));
    // who plays which: a bot mostly keeps last round's site ("his" site), the rest is random
    std::vector<Player *> left(bots.begin(), bots.end()), who(roles.size(), nullptr);
    for (size_t i = 0; i < roles.size(); i++)
    {
        std::string want = roles[i] == "MID" ? "mid" : roles[i];
        int best = -1;
        float bs = -1e9f;
        for (size_t j = 0; j < left.size(); j++)
        {
            float v = (left[j]->ct.lastSite == want ? 0.6f : 0.f) + Rnd();
            if (v > bs) { bs = v; best = (int)j; }
        }
        who[i] = left[best];
        left.erase(left.begin() + best);
    }
    struct Pick { Vec p; float pitch, yaw; bool hand; };
    std::vector<Vec> taken;
    auto choose = [&](const std::string &site, int r) -> Pick {
        std::vector<Pick> c;
        for (auto &h : g_hand)
            if (h.site == site) c.push_back({h.p, h.pitch, h.yaw, true});
        if (c.empty() || c.size() < 2)   // too few of his: add the worked-out ones
        {
            const std::vector<fl::SpotCand> &g = r >= 0 ? g_cc.site[r] : g_cc.mid;
            for (auto &s : g) c.push_back({s.p, 0, 0, false});
        }
        std::vector<std::pair<float, size_t>> order;
        for (size_t k = 0; k < c.size(); k++)
        {
            bool near = false, last = false;
            for (auto &t : taken) near = near || Dist(t, c[k].p) < 150.f;
            for (auto &l : g_spotLast) last = last || Dist(l, c[k].p) < 120.f;
            if (near) continue;
            order.push_back({Rnd() + (c[k].hand ? 1.f : 0.f) - (last ? 0.7f : 0.f), k});
        }
        if (order.empty()) return {r >= 0 ? g_mi.sites[r].c : g_mi.mid, 0, 0, false};
        std::sort(order.begin(), order.end(), [](const std::pair<float, size_t> &a, const std::pair<float, size_t> &b) { return a.first > b.first; });
        return c[order[0].second];
    };
    std::map<std::string, std::string> line;
    std::set<std::string> anchored;
    int repeats = 0;
    for (size_t i = 0; i < roles.size(); i++)
    {
        Player *p = who[i];
        const std::string &R = roles[i];
        int r = R == "A" ? ia : R == "B" ? ib : -1;
        Pick k = choose(R, r);
        bool again = false;
        for (auto &h : g_spotLast) again = again || Dist(h, k.p) < 120.f;
        repeats += again;
        taken.push_back(k.p);
        std::string role = r >= 0 ? g_mi.sites[r].label : "mid";
        bool anchor = r >= 0 && !anchored.count(role);
        if (anchor) anchored.insert(role);
        Assign(p, T_HOLD, k.p, 1, 120, anchor ? "CT anchor" : r >= 0 ? "CT hold" : "CT mid", 45);
        if (r >= 0) SetRoute(p, g_mi.sites[r]);   // over the CT side (5c Q)
        p->role = role;
        p->ct.anchor = anchor;
        p->ct.lastSite = role;
        p->ct.lastSpot = k.p;
        p->ct.setSpot = k.p;
        p->ct.setRole = role;
        p->ct.hand = k.hand;
        p->ct.handP = k.pitch;
        p->ct.handY = k.yaw;
        line[role] += p->name + (anchor ? "* " : " ");
        BLog("CTSPOT map=%s r=%d bot=%s role=%s anchor=%d hand=%d q=1.00 onSite=%d nearCT=0 mid=%d again=%d x=%.0f y=%.0f z=%.0f yaw=%.0f", g_mapName.c_str(),
             g_roundNo, p->name.c_str(), role.c_str(), (int)anchor, (int)k.hand, r >= 0 ? (int)(Dist2(k.p, g_mi.sites[r].c) <= g_mi.sites[r].radius) : -1,
             (int)(r < 0), (int)again, k.p.x, k.p.y, k.p.z, k.yaw);
    }
    g_spotLast = taken;
    std::string out;
    for (auto &kv : line) out += kv.first + "=" + kv.second;
    g_ctm.stack = "-";
    g_ctm.setup = out;
    BLog("CTSET map=%s r=%d n=%d stack=- reads=- | %s(* anchor) repeats=%d humansCT=%d hand A %d B %d mid %d (JOB 5d)", g_mapName.c_str(), g_roundNo, n,
         out.c_str(), repeats, humans, HandCount("A"), HandCount("B"), HandCount("MID"));
    return true;
}
// ==== end JOB 5d setup ===============================================================================================

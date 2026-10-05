// family_party: Valve server plugin (ISERVERPLUGINCALLBACKS002) for the family CS:GO server (legacy 1.38.8.1, Linux x86).
// STEP 3 (2026-09-23). No SDK needed: the few engine interfaces it touches are called through their vtables.
//
// 1. "Auto" on the team-select screen (the client sends "jointeam 0 1") follows the family party rule:
//    the team that already has humans (the party plays together against bots) while it has a slot; when that team is
//    full the OTHER team; only when both are full the player spectates. Team size = 2 in Wingman, 5 otherwise.
//    Picking T or CT by hand is never changed (mp_autoteambalance 0, mp_limitteams 0 in family.cfg).
//    Implemented by rewriting the "0" argument of that one command in place before the game handles it. A player who
//    stays on the team-select screen just stays there (mp_force_pick_time is long; the 45 s fallback that sent
//    "jointeam" to the client restarted the server and was removed in STEP 4).
// 2. Drop-in mode switch: when a human becomes active, the plugin asks their client for the Play-menu request the
//    family GO button left in the client cvar ui_playsettings_maps_workshop ("family:<game_type>:<game_mode>:<map,...>").
//    If nobody else is on the server, it switches the server to that mode/map over its own local RCON
//    ("game_type T; game_mode M; changelevel MAP"); if anyone else is on, the player just joins the running game.
//
// 3. STEP 9 (2026-09-24): bot team play - site holds, rotations, group hits, lurker, post-plant, trades, team buys,
//    scaled by each bot's elo - lives in family_brain.cpp (log: csgo/addons/family_brain.log).
//
// Build (32-bit, static C++ runtime): plugin/build.sh -> overlay/csgo/addons/family_party.so
// Log: /opt/csgo/server/csgo/addons/family_party.log (and stdout = docker logs csgo).

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <atomic>
#include <mutex>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

typedef void *(*CreateInterfaceFn)(const char *name, int *returnCode);
struct edict_t;

static const char *kLogPath = "csgo/addons/family_party.log";   // cwd of srcds is /opt/csgo/server
static const char *kRconPwFile = "/run/csgo/rcon_password";
static const char *kRequestCvar = "ui_playsettings_maps_workshop";
static int g_rconPort = 27025;

static std::mutex g_logMutex;
static void Log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    time_t t = time(nullptr);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&t));
    std::lock_guard<std::mutex> lock(g_logMutex);
    printf("[family_party] %s\n", buf);
    fflush(stdout);
    if (FILE *f = fopen(kLogPath, "a"))
    {
        fprintf(f, "%s %s\n", ts, buf);
        fclose(f);
    }
}

template <typename Fn> static Fn VFunc(void *obj, int index)
{
    return reinterpret_cast<Fn>((*reinterpret_cast<void ***>(obj))[index]);
}

// ---- engine / game interfaces (vtable indices from the Source SDK headers, CS:GO build) ----
static void *g_helpers;        // ISERVERPLUGINHELPERS001: 0 CreateMessage, 1 ClientCommand, 2 StartQueryCvarValue
static void *g_playerInfoMgr;  // PlayerInfoManager002:    0 GetPlayerInfo(edict_t *)
// STEP 9: the bot team-play layer lives in family_brain.cpp
void BrainLoad(void *(*playerInfo)(edict_t *), void *playerInfoMgr, double (*eloOf)(const char *));
void BrainLevelInit(const char *map);
void BrainFrame(const std::set<edict_t *> &clients);
void BrainLogLine(const char *msg);
void BrainSetDiag(int d);
void BrainSetSteer(int on);
void BrainSetMode(int type, int mode);
// STEP 10: dials + footwork (send-table offsets, game events, test hooks)
void BrainFactories(void *(*engineFactory)(const char *, int *), void *(*serverFactory)(const char *, int *),
                    double (*testValue)(const char *, double));
void BrainUnload();
// STEP 12D (job 5): the game's chat lines (a human's "a" / "b" overrule, proof that a bot's team chat line went out) and
// the console "say" the brain may use when no bot can speak
void BrainChatLine(const char *msg);
void BrainSetServerSay(void (*fn)(const char *));
bool BrainClientCommand(edict_t *e, int argc, const char *const *argv);   // JOB 5d: bot_asitespot<N> etc.

static void *PlayerInfo(edict_t *e)
{
    if (!g_playerInfoMgr || !e)
        return nullptr;
    return VFunc<void *(*)(void *, edict_t *)>(g_playerInfoMgr, 0)(g_playerInfoMgr, e);
}
// IPlayerInfo: 0 GetName, 1 GetUserID, 2 GetNetworkIDString, 3 GetTeamIndex
static const char *PI_Name(void *pi) { return VFunc<const char *(*)(void *)>(pi, 0)(pi); }
static const char *PI_NetworkID(void *pi) { return VFunc<const char *(*)(void *)>(pi, 2)(pi); }
static int PI_Team(void *pi) { return VFunc<int (*)(void *)>(pi, 3)(pi); }

static bool IsBot(void *pi)
{
    const char *id = PI_NetworkID(pi);
    return id && strcmp(id, "BOT") == 0;
}

// game_type / game_mode of the running level, read over the local RCON a few seconds after each level start
// (IGameTypes' vtable differs between the Windows and Linux builds, so it is not called here).
static std::atomic<int> g_curType{-1}, g_curMode{-1};
static int CurrentGameType() { return g_curType.load(); }
static int CurrentGameMode() { return g_curMode.load(); }

static std::set<edict_t *> g_clients;   // every client put in server (bots too); edicts are stable
static std::map<edict_t *, time_t> g_activeSince;   // humans still to pick a team, since when
static std::set<edict_t *> g_asked;   // humans already asked for their Play-menu request on this level
static int FamilyAutoPick(edict_t *e, const char *why);

// The family "Auto" rule on plain numbers (unit-tested by plugin/test_pick.cpp): humans already on T / CT (not
// counting the player who asks) and the team size. 0 = nobody on a team yet (the game picks), 2 = T, 3 = CT,
// 1 = spectator. The party team is the one with humans (more humans; a tie goes to T); when it is full the other
// team; when both are full the player watches.
extern "C" int FamilyPick(int humansT, int humansCT, int cap)
{
    if (humansT == 0 && humansCT == 0)
        return 0;
    int party = humansT >= humansCT ? 2 : 3;
    int other = 5 - party;
    int inParty = party == 2 ? humansT : humansCT;
    int inOther = party == 2 ? humansCT : humansT;
    if (inParty < cap)
        return party;
    if (inOther < cap)
        return other;
    return 1;
}
static std::string g_map;
static time_t g_lastSwitch;

// ---- CCommand (tier1 convar.h, CS:GO): argc, argv0size, argS buffer[512], argv buffer[512], argv pointers[64] ----
struct CCommandLayout
{
    int argc;
    int argv0Size;
    char argSBuffer[512];
    char argvBuffer[512];
    const char *argv[64];
};

static bool CommandLooksSane(const CCommandLayout *c)
{
    if (c->argc < 1 || c->argc > 64)
        return false;
    for (int i = 0; i < c->argc; i++)
    {
        if (c->argv[i] < c->argvBuffer || c->argv[i] >= c->argvBuffer + sizeof(c->argvBuffer))
            return false;
    }
    return true;
}

// ---- local RCON (runs on a worker thread only: srcds answers RCON on its main thread) ----
static bool RconExchange(int s, int id, int type, const std::string &body)
{
    std::string p;
    int32_t size = 4 + 4 + (int32_t)body.size() + 2;
    p.append(reinterpret_cast<char *>(&size), 4);
    p.append(reinterpret_cast<char *>(&id), 4);
    p.append(reinterpret_cast<char *>(&type), 4);
    p.append(body);
    p.append("\0\0", 2);
    return send(s, p.data(), p.size(), 0) == (ssize_t)p.size();
}

static bool RconRead(int s, int &id, int &type, std::string &body)
{
    auto exact = [s](char *buf, int n) {
        int got = 0;
        while (got < n)
        {
            ssize_t r = recv(s, buf + got, n - got, 0);
            if (r <= 0)
                return false;
            got += (int)r;
        }
        return true;
    };
    int32_t size;
    if (!exact(reinterpret_cast<char *>(&size), 4) || size < 10 || size > 65536)
        return false;
    std::vector<char> d(size);
    if (!exact(d.data(), size))
        return false;
    memcpy(&id, d.data(), 4);
    memcpy(&type, d.data() + 4, 4);
    body.assign(d.data() + 8, size - 10);
    return true;
}

// Runs commands in order and returns their replies ("" on failure).
static std::vector<std::string> Rcon(const std::vector<std::string> &cmds)
{
    std::vector<std::string> out(cmds.size());
    FILE *f = fopen(kRconPwFile, "r");
    if (!f)
    {
        Log("rcon: no password file %s", kRconPwFile);
        return out;
    }
    char pw[256] = {};
    if (!fgets(pw, sizeof(pw), f))
        pw[0] = 0;
    fclose(f);
    pw[strcspn(pw, "\r\n")] = 0;

    int s = socket(AF_INET, SOCK_STREAM, 0);
    timeval tv{5, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(g_rconPort);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0)
    {
        Log("rcon: connect failed");
        close(s);
        return out;
    }
    int id, type;
    std::string body;
    RconExchange(s, 1, 3, pw);
    bool authed = false;
    while (RconRead(s, id, type, body))
    {
        if (type == 2)
        {
            authed = (id != -1);
            break;
        }
    }
    if (!authed)
    {
        Log("rcon: auth failed");
        close(s);
        return out;
    }
    for (size_t i = 0; i < cmds.size(); i++)
    {
        RconExchange(s, 100 + (int)i, 2, cmds[i]);
        RconExchange(s, 900 + (int)i, 0, "");   // its echo marks the end of a multi-packet reply
        while (RconRead(s, id, type, body))
        {
            if (id == 900 + (int)i)
                break;
            out[i] += body;
        }
    }
    close(s);
    return out;
}

static int CvarValue(const std::string &reply)   // "\"game_type\" = \"0\" ..." -> 0
{
    size_t eq = reply.find("= \"");
    return eq == std::string::npos ? -1 : atoi(reply.c_str() + eq + 3);
}

static std::mutex g_switchMutex;

static void SwitchWorker(std::string requester, int wantType, int wantMode, std::vector<std::string> maps)
{
    std::lock_guard<std::mutex> lock(g_switchMutex);
    if (g_lastSwitch && time(nullptr) - g_lastSwitch < 45)
    {
        Log("request from %s ignored: a switch happened %ds ago", requester.c_str(), (int)(time(nullptr) - g_lastSwitch));
        return;
    }
    std::vector<std::string> r = Rcon({"status", "game_type", "game_mode", "host_map"});
    const std::string &st = r[0];
    size_t p = st.find("players : ");
    if (p == std::string::npos)
    {
        Log("request from %s: could not read status", requester.c_str());
        return;
    }
    int humans = atoi(st.c_str() + p + 10);
    int curType = CvarValue(r[1]), curMode = CvarValue(r[2]);
    std::string curMap = g_map;
    size_t m = r[3].find("= \"");
    if (m != std::string::npos)
        curMap = r[3].substr(m + 3, r[3].find('"', m + 3) - (m + 3));
    if (curMap.size() > 4 && curMap.compare(curMap.size() - 4, 4, ".bsp") == 0)   // host_map says "de_dust2.bsp"
        curMap.resize(curMap.size() - 4);
    bool mapOk = false;
    for (auto &x : maps)
        mapOk |= (x == curMap);
    Log("request from %s: want %d/%d maps[%d]; server %d/%d on %s with %d human(s)", requester.c_str(), wantType, wantMode,
        (int)maps.size(), curType, curMode, curMap.c_str(), humans);
    if (humans > 1)
    {
        Log("others are playing: %s joins the running game (%d/%d %s)", requester.c_str(), curType, curMode, curMap.c_str());
        return;
    }
    if (curType == wantType && curMode == wantMode && (mapOk || maps.empty()))
    {
        Log("server already runs the requested mode/map");
        return;
    }
    std::string target = mapOk ? curMap : (maps.empty() ? curMap : maps[rand() % maps.size()]);
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "game_type %d; game_mode %d; changelevel %s", wantType, wantMode, target.c_str());
    g_lastSwitch = time(nullptr);
    Log("SWITCH (server empty apart from %s): %s", requester.c_str(), cmd);
    Rcon({cmd});
}

static bool ValidMapName(const std::string &s)
{
    if (s.empty() || s.size() > 64)
        return false;
    for (char c : s)
        if (!(isalnum((unsigned char)c) || c == '_' || c == '-'))
            return false;
    return true;
}

static void ModeReader(std::string map)
{
    for (int attempt = 0; attempt < 20; attempt++)
    {
        sleep(3);
        std::vector<std::string> r = Rcon({"game_type", "game_mode"});
        int t = CvarValue(r[0]), m = CvarValue(r[1]);
        if (t >= 0 && m >= 0)
        {
            g_curType = t;
            g_curMode = m;
            Log("level %s runs game_type %d game_mode %d", map.c_str(), t, m);
            return;
        }
    }
    Log("level %s: could not read game_type/game_mode", map.c_str());
}

// ==== STEP 8 (2026-09-24): bot director + per-bot / per-person elo =================================================
// 3. The roster (csgo/addons/family_roster.txt, generated with botprofile.db by servers/csgo/bots/make_botprofile.py)
//    names ~50 bots with a starting elo. Live elo for every bot and every person (keyed by steamid) is kept in
//    csgo/addons/family_elo.json. Every person starts at 300 (Silver I).
// 4. The director picks the bots (gamemode cfgs set bot_quota 0): target = the average elo of the humans on a team
//    (solo = that player's elo; nobody on = 1000); each team gets bots so that its average (humans + bots) is the
//    target, so both teams are even. Swaps wait for a round end once a round has been played (additions, warmup
//    and the first round go in at once);
//    a team is left alone while its bot average is within 150 of what it needs.
// 5. After every competitive / Wingman match ("Game Over" in the server log) every human and every bot on both
//    teams gets a standard team-elo move (K 32 for the first 10 games, then 16). Bot-only matches are not rated
//    (the server plays them all day) unless the test hook csgo/addons/family_test.txt says "rate_bots 1".
// The server log (family.cfg: log on) is tailed on the main thread: Match_Start, Round_End, scores, plants, defuses.
static const char *kRosterPath = "csgo/addons/family_roster.txt";
static const char *kEloPath = "csgo/addons/family_elo.json";
static const char *kTestPath = "csgo/addons/family_test.txt";   // test hook, absent in normal use
static const char *kLogDir = "csgo/logs";
static const double kStartEloHuman = 300.0;
static const double kDefaultTarget = 1000.0;
static const double kKeepTolerance = 150.0;

// ---- pure logic (unit-tested by plugin/test_pick.cpp) ----
struct FamilyBot
{
    std::string name;
    double elo;
    int team;   // 0 = not in the game, 2 = T, 3 = CT (anything else: in the game, never picked)
};
struct FamilyPlan
{
    double target = 0, needT = 0, needCT = 0;
    std::vector<std::string> keepT, keepCT, addT, addCT, kick;
};

static double Mean(const std::vector<double> &v)
{
    double s = 0;
    for (double x : v)
        s += x;
    return v.empty() ? 0 : s / v.size();
}

// humT / humCT: elo of the humans on each team. bots: the roster with each bot's live elo and where it is now.
FamilyPlan FamilyDirect(const std::vector<double> &humT, const std::vector<double> &humCT, int cap,
                        const std::vector<FamilyBot> &bots, double defaultTarget, unsigned seed, bool force,
                        double botElo)
{
    FamilyPlan p;
    std::vector<double> all(humT);
    all.insert(all.end(), humCT.begin(), humCT.end());
    // botElo > 0 (family_test.txt "bot_elo <elo>", the bot rank picker): the bots themselves average that elo, humans
    // on or not; each bot still gets its own jitter. Humans' elo is not touched.
    p.target = botElo > 0 ? botElo : all.empty() ? defaultTarget : Mean(all);
    if (bots.empty())
        return p;
    double lo = 1e9, hi = -1e9;
    for (auto &b : bots)
    {
        lo = std::min(lo, b.elo);
        hi = std::max(hi, b.elo);
    }
    std::vector<char> used(bots.size(), 0);
    unsigned rng = seed ? seed : 1;
    auto jitter = [&rng]() {   // +-100 elo, so two matches at the same rank meet different bots
        rng = rng * 1103515245u + 12345u;
        return (double)((rng >> 8) % 2001) / 10.0 - 100.0;
    };
    // phase 1: each team either keeps its bots (right count, average within the tolerance) or is re-picked
    int n[4] = {0, 0, 0, 0};
    double need[4] = {0, 0, 0, 0};
    bool repick[4] = {false, false, false, false};
    std::vector<int> cur[4], chosen[4];
    for (int team : {2, 3})
    {
        const std::vector<double> &hum = team == 2 ? humT : humCT;
        n[team] = std::max(0, cap - (int)hum.size());
        double sumH = 0;
        for (double x : hum)
            sumH += x;
        need[team] = n[team] ? (botElo > 0 ? botElo : (cap * p.target - sumH) / n[team]) : 0;
        need[team] = std::min(hi, std::max(lo, need[team]));
        (team == 2 ? p.needT : p.needCT) = need[team];
        double curSum = 0;
        for (size_t i = 0; i < bots.size(); i++)
            if (bots[i].team == team)
            {
                cur[team].push_back((int)i);
                curSum += bots[i].elo;
            }
        repick[team] = force || (int)cur[team].size() != n[team] ||
                       (n[team] > 0 && fabs(curSum / n[team] - need[team]) > kKeepTolerance);
        if (!repick[team])
            for (int i : cur[team])
            {
                chosen[team].push_back(i);
                used[i] = 1;
            }
    }
    // phase 2: the re-picked teams take turns (the one with more seats first), so at the ends of the roster
    // neither team gets all the nearest bots
    double sumC[4] = {0, 0, 0, 0};
    int order[2] = {2, 3};
    if (n[3] > n[2])
        std::swap(order[0], order[1]);
    for (bool any = true; any;)
    {
        any = false;
        for (int team : order)
        {
            int k = (int)chosen[team].size();
            if (!repick[team] || k >= n[team])
                continue;
            double want = (n[team] * need[team] - sumC[team]) / (n[team] - k) + jitter();
            int best = -1;
            double bestD = 1e18;
            for (size_t i = 0; i < bots.size(); i++)
            {
                if (used[i] || (bots[i].team != 0 && bots[i].team != team))
                    continue;
                double d = fabs(bots[i].elo - want) - (bots[i].team == team ? 25.0 : 0.0);   // mild churn guard
                if (d < bestD)
                {
                    bestD = d;
                    best = (int)i;
                }
            }
            if (best < 0)
            {
                repick[team] = false;   // roster exhausted
                continue;
            }
            used[best] = 1;
            chosen[team].push_back(best);
            sumC[team] += bots[best].elo;
            any = true;
        }
    }
    for (int team : {2, 3})
    {
        std::vector<std::string> &keep = team == 2 ? p.keepT : p.keepCT;
        std::vector<std::string> &add = team == 2 ? p.addT : p.addCT;
        for (int i : chosen[team])
            (bots[i].team == team ? keep : add).push_back(bots[i].name);
        for (int i : cur[team])
            if (std::find(chosen[team].begin(), chosen[team].end(), i) == chosen[team].end())
                p.kick.push_back(bots[i].name);
    }
    return p;
}

struct FamilyRated
{
    std::string key;
    double elo;
    int games;
    int team;   // 2 T, 3 CT
};

// Standard team elo: E = 1 / (1 + 10^((opp avg - own avg) / 400)); S = 1 win, 0.5 tie, 0 loss; move = K (S - E),
// K 32 for a player's first 10 games, then 16. False (nothing changed) unless both teams have someone on them.
bool FamilyEloUpdate(std::vector<FamilyRated> &pl, int scoreT, int scoreCT)
{
    double sT = 0, sC = 0;
    int nT = 0, nC = 0;
    for (auto &p : pl)
    {
        if (p.team == 2) { sT += p.elo; nT++; }
        if (p.team == 3) { sC += p.elo; nC++; }
    }
    if (!nT || !nC)
        return false;
    double eT = 1.0 / (1.0 + pow(10.0, (sC / nC - sT / nT) / 400.0));
    double resT = scoreT > scoreCT ? 1.0 : scoreT == scoreCT ? 0.5 : 0.0;
    for (auto &p : pl)
    {
        if (p.team != 2 && p.team != 3)
            continue;
        double k = p.games < 10 ? 32.0 : 16.0;
        double d = p.team == 2 ? resT - eT : (1.0 - resT) - (1.0 - eT);
        p.elo = std::max(100.0, p.elo + k * d);
        p.games++;
    }
    return true;
}

// Elo -> the 18 CS:GO competitive ranks (1 = Silver I at 300 and below ... 18 = Global Elite from 2400).
int FamilyRank(double elo)
{
    int r = 1 + (int)floor((elo - 300.0) / (2100.0 / 17.0));
    return std::max(1, std::min(18, r));
}

// ---- tiny JSON (only what family_elo.json needs) ----
struct JVal
{
    enum Type { NUL, NUM, STR, OBJ, ARR, BOOL } t = NUL;
    double n = 0;
    std::string s;
    std::vector<std::pair<std::string, JVal>> o;
    std::vector<JVal> a;
    const JVal *get(const char *k) const
    {
        for (auto &kv : o)
            if (kv.first == k)
                return &kv.second;
        return nullptr;
    }
    double num(const char *k, double def) const
    {
        const JVal *v = get(k);
        return v && v->t == NUM ? v->n : def;
    }
};

static void JSkip(const char *&p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
}
static bool JStr(const char *&p, std::string &out)
{
    if (*p != '"')
        return false;
    p++;
    out.clear();
    while (*p && *p != '"')
    {
        if (*p == '\\' && p[1])
        {
            p++;
            char c = *p;
            if (c == 'n') out += '\n';
            else if (c == 't') out += '\t';
            else if (c == 'u' && p[1] && p[2] && p[3] && p[4]) { out += '?'; p += 4; }
            else out += c;
            p++;
        }
        else
            out += *p++;
    }
    if (*p != '"')
        return false;
    p++;
    return true;
}
static bool JParse(const char *&p, JVal &v, int depth)
{
    if (depth > 32)
        return false;
    JSkip(p);
    if (*p == '{')
    {
        v.t = JVal::OBJ;
        p++;
        JSkip(p);
        if (*p == '}') { p++; return true; }
        for (;;)
        {
            std::string k;
            JSkip(p);
            if (!JStr(p, k)) return false;
            JSkip(p);
            if (*p++ != ':') return false;
            JVal child;
            if (!JParse(p, child, depth + 1)) return false;
            v.o.emplace_back(k, std::move(child));
            JSkip(p);
            if (*p == ',') { p++; continue; }
            if (*p == '}') { p++; return true; }
            return false;
        }
    }
    if (*p == '[')
    {
        v.t = JVal::ARR;
        p++;
        JSkip(p);
        if (*p == ']') { p++; return true; }
        for (;;)
        {
            JVal child;
            if (!JParse(p, child, depth + 1)) return false;
            v.a.push_back(std::move(child));
            JSkip(p);
            if (*p == ',') { p++; continue; }
            if (*p == ']') { p++; return true; }
            return false;
        }
    }
    if (*p == '"') { v.t = JVal::STR; return JStr(p, v.s); }
    if (!strncmp(p, "true", 4)) { v.t = JVal::BOOL; v.n = 1; p += 4; return true; }
    if (!strncmp(p, "false", 5)) { v.t = JVal::BOOL; v.n = 0; p += 5; return true; }
    if (!strncmp(p, "null", 4)) { v.t = JVal::NUL; p += 4; return true; }
    char *end = nullptr;
    v.n = strtod(p, &end);
    if (end == p) return false;
    v.t = JVal::NUM;
    p = end;
    return true;
}
static std::string JEsc(const std::string &s)
{
    std::string o = "\"";
    for (unsigned char c : s)
    {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}
static std::string JDump(const JVal &v)
{
    char b[64];
    switch (v.t)
    {
    case JVal::NUM: snprintf(b, sizeof(b), "%.10g", v.n); return b;
    case JVal::STR: return JEsc(v.s);
    case JVal::BOOL: return v.n ? "true" : "false";
    case JVal::OBJ:
    {
        std::string o = "{";
        for (size_t i = 0; i < v.o.size(); i++)
            o += (i ? ", " : "") + JEsc(v.o[i].first) + ": " + JDump(v.o[i].second);
        return o + "}";
    }
    case JVal::ARR:
    {
        std::string o = "[";
        for (size_t i = 0; i < v.a.size(); i++)
            o += (i ? ", " : "") + JDump(v.a[i]);
        return o + "]";
    }
    default: return "null";
    }
}

// ---- the store (main thread only) ----
struct EloRec
{
    std::string name;
    double elo;
    int games;
};
static std::vector<std::pair<std::string, double>> g_roster;   // name, starting elo (roster order)
static std::map<std::string, EloRec> g_botElo;                 // by bot name
static std::map<std::string, EloRec> g_humanElo;               // by steamid
static std::vector<JVal> g_matches;                            // recent rated matches (newest last, max 30)

static void LoadRoster()
{
    g_roster.clear();
    FILE *f = fopen(kRosterPath, "r");
    if (!f)
    {
        Log("roster: %s missing - the director is off", kRosterPath);
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        char name[64];
        double elo;
        if (line[0] == '/' || line[0] == '#' || sscanf(line, "%63s %lf", name, &elo) != 2)
            continue;
        g_roster.emplace_back(name, elo);
    }
    fclose(f);
}

static void LoadElo()
{
    g_botElo.clear();
    g_humanElo.clear();
    g_matches.clear();
    if (FILE *f = fopen(kEloPath, "rb"))
    {
        std::string text;
        char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
            text.append(buf, n);
        fclose(f);
        JVal root;
        const char *p = text.c_str();
        if (!JParse(p, root, 0) || root.t != JVal::OBJ)
        {
            Log("elo: %s unreadable - starting fresh (the old file is kept as .bad)", kEloPath);
            rename(kEloPath, "csgo/addons/family_elo.json.bad");
        }
        else
        {
            if (const JVal *b = root.get("bots"))
                for (auto &kv : b->o)
                    g_botElo[kv.first] = {kv.first, kv.second.num("elo", 1000), (int)kv.second.num("games", 0)};
            if (const JVal *h = root.get("humans"))
                for (auto &kv : h->o)
                {
                    const JVal *nm = kv.second.get("name");
                    g_humanElo[kv.first] = {nm && nm->t == JVal::STR ? nm->s : "?", kv.second.num("elo", kStartEloHuman),
                                            (int)kv.second.num("games", 0)};
                }
            if (const JVal *m = root.get("matches"))
                g_matches = m->a;
        }
    }
    for (auto &r : g_roster)   // a roster bot not in the file yet starts at its roster elo
        if (!g_botElo.count(r.first))
            g_botElo[r.first] = {r.first, r.second, 0};
    Log("elo: %d roster bots; %d bots and %d people in %s", (int)g_roster.size(), (int)g_botElo.size(),
        (int)g_humanElo.size(), kEloPath);
}

static void SaveElo()
{
    std::string o = "{\n \"about\": \"family CS:GO elo (family_party.so, step 8). humans by steamid, start 300 = Silver I; "
                    "rank 1-18 = CS:GO competitive rank\",\n \"humans\": {";
    bool first = true;
    char b[128];
    for (auto &kv : g_humanElo)
    {
        snprintf(b, sizeof(b), "\"elo\": %.1f, \"games\": %d, \"rank\": %d}", kv.second.elo, kv.second.games,
                 FamilyRank(kv.second.elo));
        o += std::string(first ? "\n  " : ",\n  ") + JEsc(kv.first) + ": {\"name\": " + JEsc(kv.second.name) + ", " + b;
        first = false;
    }
    o += "\n },\n \"bots\": {";
    first = true;
    for (auto &kv : g_botElo)
    {
        snprintf(b, sizeof(b), "{\"elo\": %.1f, \"games\": %d}", kv.second.elo, kv.second.games);
        o += std::string(first ? "\n  " : ",\n  ") + JEsc(kv.first) + ": " + b;
        first = false;
    }
    o += "\n },\n \"matches\": [";
    for (size_t i = 0; i < g_matches.size(); i++)
        o += std::string(i ? ",\n  " : "\n  ") + JDump(g_matches[i]);
    o += "\n ]\n}\n";
    std::string tmp = std::string(kEloPath) + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f)
    {
        Log("elo: cannot write %s", tmp.c_str());
        return;
    }
    fwrite(o.data(), 1, o.size(), f);
    fclose(f);
    rename(kEloPath, (std::string(kEloPath) + ".bak").c_str());   // the previous version stays as .bak
    rename(tmp.c_str(), kEloPath);
}

static double HumanElo(const std::string &id)
{
    auto it = g_humanElo.find(id);
    return it == g_humanElo.end() ? kStartEloHuman : it->second.elo;
}
static double BotElo(const std::string &name)
{
    auto it = g_botElo.find(name);
    return it == g_botElo.end() ? 1000.0 : it->second.elo;
}

// test hook values (csgo/addons/family_test.txt lines: "target <elo>", "rate_bots 1", "bot_elo <elo>"); re-read on each use
static double TestValue(const char *key, double def)
{
    FILE *f = fopen(kTestPath, "r");
    if (!f)
        return def;
    char k[64];
    double v, out = def;
    while (fscanf(f, "%63s %lf", k, &v) == 2)
        if (!strcmp(k, key))
            out = v;
    fclose(f);
    return out;
}

// ---- match state from the server log ----
struct MatchState
{
    bool live = false;
    int rounds = 0, plants = 0, defuses = 0, exploded = 0, scoreT = 0, scoreCT = 0;
};
static MatchState g_match;
static bool g_directPending;                 // a round ended: the director may swap bots now
static std::atomic<bool> g_directBusy{false};
static time_t g_lastDirect, g_lastDirectCheck;
static int g_directActions;                  // per level, capped so a lineup that cannot be reached never loops
static time_t g_loadTime;

struct Seat
{
    std::string name, id;
    bool bot;
    int team;
};
static std::vector<Seat> Seats()
{
    std::vector<Seat> out;
    for (edict_t *c : g_clients)
    {
        void *pi = PlayerInfo(c);
        if (!pi)
            continue;
        const char *nm = PI_Name(pi), *id = PI_NetworkID(pi);
        out.push_back({nm ? nm : "", id ? id : "", IsBot(pi), PI_Team(pi)});
    }
    return out;
}

static int TeamCap() { return (CurrentGameType() == 0 && CurrentGameMode() == 2) ? 2 : 5; }
static bool RankedMode() { return CurrentGameType() == 0 && (CurrentGameMode() == 1 || CurrentGameMode() == 2); }

static void OnGameOver(const char *msg)
{
    bool ranked = RankedMode();
    std::vector<Seat> seats = Seats();
    int humans = 0;
    for (auto &s : seats)
        humans += !s.bot && (s.team == 2 || s.team == 3);
    bool rateBots = TestValue("rate_bots", 0) != 0;
    Log("GAME OVER on %s (%s): T %d - CT %d, rounds %d, plants %d, defuses %d, bombs exploded %d, humans %d | %s",
        g_map.c_str(), ranked ? "ranked" : "not ranked", g_match.scoreT, g_match.scoreCT, g_match.rounds, g_match.plants,
        g_match.defuses, g_match.exploded, humans, msg);
    if (!ranked)
        return;
    if (humans == 0 && !rateBots)
    {
        Log("elo: bot-only match, not rated");
        return;
    }
    std::vector<FamilyRated> pl;
    for (auto &s : seats)
    {
        if (s.team != 2 && s.team != 3)
            continue;
        if (s.bot && !g_botElo.count(s.name))
            continue;   // not a roster bot
        if (!s.bot && !g_humanElo.count(s.id))
            g_humanElo[s.id] = {s.name, kStartEloHuman, 0};   // every person starts at 300 (Silver I)
        EloRec &rec = s.bot ? g_botElo[s.name] : g_humanElo[s.id];
        if (!s.bot)
            rec.name = s.name;
        pl.push_back({std::string(s.bot ? "b:" : "h:") + (s.bot ? s.name : s.id), rec.elo, rec.games, s.team});
    }
    std::vector<FamilyRated> before = pl;
    if (!FamilyEloUpdate(pl, g_match.scoreT, g_match.scoreCT))
    {
        Log("elo: a team is empty, not rated");
        return;
    }
    auto numv = [](double x) { JVal v; v.t = JVal::NUM; v.n = x; return v; };
    auto strv = [](const std::string &x) { JVal v; v.t = JVal::STR; v.s = x; return v; };
    JVal m, moves;
    m.t = JVal::OBJ;
    moves.t = JVal::OBJ;
    char when[32];
    time_t now = time(nullptr);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", localtime(&now));
    m.o.emplace_back("when", strv(when));
    m.o.emplace_back("map", strv(g_map));
    m.o.emplace_back("mode", strv(CurrentGameMode() == 2 ? "wingman" : "competitive"));
    m.o.emplace_back("score_t", numv(g_match.scoreT));
    m.o.emplace_back("score_ct", numv(g_match.scoreCT));
    m.o.emplace_back("plants", numv(g_match.plants));
    m.o.emplace_back("defuses", numv(g_match.defuses));
    for (size_t i = 0; i < pl.size(); i++)
    {
        std::string key = pl[i].key.substr(2);
        bool bot = pl[i].key[0] == 'b';
        EloRec &rec = bot ? g_botElo[key] : g_humanElo[key];
        rec.elo = pl[i].elo;
        rec.games = pl[i].games;
        char d[96];
        snprintf(d, sizeof(d), "%s %.1f -> %.1f", pl[i].team == 2 ? "T" : "CT", before[i].elo, pl[i].elo);
        moves.o.emplace_back(bot ? key : rec.name + " " + key, strv(d));
        Log("elo: %-3s %-24s %6.1f -> %6.1f (%+.1f)", pl[i].team == 2 ? "T" : "CT", (bot ? key : rec.name).c_str(),
            before[i].elo, pl[i].elo, pl[i].elo - before[i].elo);
    }
    m.o.emplace_back("moves", moves);
    g_matches.push_back(m);
    if (g_matches.size() > 30)
        g_matches.erase(g_matches.begin());
    SaveElo();
    Log("elo: saved %s", kEloPath);
}

static void OnLogLine(const char *line)
{
    // "L 09/24/2026 - 01:29:50: <message>"
    if (strncmp(line, "L ", 2) != 0 || strlen(line) < 26)
        return;
    const char *msg = line + 25;
    bool fromPlayer = msg[0] == '"';
    if (fromPlayer && (strstr(msg, "\" say \"") || strstr(msg, "\" say_team \"")))
    {
        BrainChatLine(msg);   // STEP 12D: a human's plan call / a bot's own line coming back
        return;               // chat text is never an event
    }
    BrainLogLine(msg);
    if (strstr(msg, "World triggered \"Match_Start\"") == msg)
    {
        g_match = MatchState();
        g_match.live = true;
        Log("match start on %s", g_map.c_str());
    }
    else if (strstr(msg, "World triggered \"Round_End\"") == msg)
    {
        g_match.rounds++;
        g_directPending = true;
    }
    else if (!strncmp(msg, "Team \"CT\" scored \"", 18))
        g_match.scoreCT = atoi(msg + 18);
    else if (!strncmp(msg, "Team \"TERRORIST\" scored \"", 25))
        g_match.scoreT = atoi(msg + 25);
    else if (fromPlayer && strstr(msg, "triggered \"Planted_The_Bomb\""))
        g_match.plants++;
    else if (fromPlayer && strstr(msg, "triggered \"Defused_The_Bomb\""))
        g_match.defuses++;
    else if (strstr(msg, "triggered \"SFUI_Notice_Target_Bombed\""))
        g_match.exploded++;
    else if (!strncmp(msg, "Game Over:", 10))
    {
        OnGameOver(msg);
        g_match.live = false;
        g_directPending = true;
    }
}

static std::string g_logFile;
static long g_logPos;
static std::string g_logBuf;
static time_t g_lastLogScan;

static std::string NewestLog(time_t *mtime)
{
    std::string best;
    time_t bestT = 0;
    if (DIR *d = opendir(kLogDir))
    {
        while (dirent *e = readdir(d))
        {
            size_t n = strlen(e->d_name);
            if (n < 5 || strcmp(e->d_name + n - 4, ".log") != 0)
                continue;
            std::string p = std::string(kLogDir) + "/" + e->d_name;
            struct stat st;
            if (stat(p.c_str(), &st) == 0 && (st.st_mtime > bestT || (st.st_mtime == bestT && p > best)))
            {
                bestT = st.st_mtime;
                best = p;
            }
        }
        closedir(d);
    }
    *mtime = bestT;
    return best;
}

static void PollLog()
{
    time_t now = time(nullptr);
    if (now - g_lastLogScan >= 3)
    {
        g_lastLogScan = now;
        time_t mt;
        std::string nf = NewestLog(&mt);
        if (!nf.empty() && nf != g_logFile)
        {
            // the newest file at load time, if older than this plugin, belongs to an earlier run: skip what it holds
            struct stat st;
            bool old = g_logFile.empty() && mt < g_loadTime - 5;
            g_logFile = nf;
            g_logPos = (old && stat(nf.c_str(), &st) == 0) ? (long)st.st_size : 0;
            g_logBuf.clear();
            Log("reading server log %s from byte %ld", nf.c_str(), g_logPos);
        }
    }
    if (g_logFile.empty())
        return;
    FILE *f = fopen(g_logFile.c_str(), "rb");
    if (!f)
        return;
    fseek(f, g_logPos, SEEK_SET);
    static char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (!n)
        return;
    g_logPos += (long)n;
    g_logBuf.append(buf, n);
    size_t start = 0, nl;
    while ((nl = g_logBuf.find('\n', start)) != std::string::npos)
    {
        std::string line = g_logBuf.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        OnLogLine(line.c_str());
        start = nl + 1;
    }
    g_logBuf.erase(0, start);
}

// Runs on a worker thread (srcds answers RCON on its main thread). Kicks first (their kickid runs later in the same
// command buffer, so the adds go in a second exchange), then the adds; bot_quota is set explicitly after each.
static void DirectWorker(std::vector<std::string> kicks, std::string adds, int kickQuota)
{
    if (!kicks.empty())
    {
        std::string k;
        for (auto &n : kicks)
            k += "bot_kick " + n + "; ";
        char q[48];
        snprintf(q, sizeof(q), "bot_quota %d", kickQuota);
        Rcon({k + q});
        usleep(700000);
    }
    if (!adds.empty())
        Rcon({adds});
    g_directBusy = false;
}

static void DirectorTick()
{
    time_t now = time(nullptr);
    if (now == g_lastDirectCheck)
        return;
    g_lastDirectCheck = now;
    if (g_roster.empty() || CurrentGameType() < 0 || g_directBusy || now - g_lastDirect < 5 || g_directActions >= 40)
        return;
    if (TestValue("director", 1) == 0)
        return;   // STEP 10 test hook: the test adds its own bots (elo and dial A/B matches)
    std::vector<Seat> seats = Seats();
    std::vector<double> humT, humCT;
    std::map<std::string, int> botTeam;
    int botsInGame = 0;
    for (auto &s : seats)
    {
        if (s.bot)
        {
            botTeam[s.name] = s.team;
            botsInGame++;
        }
        else if (s.team == 2)
            humT.push_back(HumanElo(s.id));
        else if (s.team == 3)
            humCT.push_back(HumanElo(s.id));
    }
    std::vector<FamilyBot> bots;
    for (auto &r : g_roster)
    {
        auto it = botTeam.find(r.first);
        int team = it == botTeam.end() ? 0 : (it->second == 2 || it->second == 3) ? it->second : 1;
        bots.push_back({r.first, BotElo(r.first), team});
    }
    int cap = TeamCap();
    double botElo = TestValue("bot_elo", 0);   // the bot rank picker; 0 / absent = follow the humans
    FamilyPlan p = FamilyDirect(humT, humCT, cap, bots, TestValue("target", kDefaultTarget), (unsigned)now, false,
                                botElo);
    if (p.addT.empty() && p.addCT.empty() && p.kick.empty())
    {
        g_directPending = false;
        return;
    }
    // swaps wait for a round end once the match has played a round; before that (warmup - the log cannot tell warmup
    // from the first round - an empty server idles in warmup) and for a lineup with only additions they go in at once
    if (!p.kick.empty() && g_match.live && g_match.rounds > 0 && !g_directPending)
        return;
    int humansOn = (int)(humT.size() + humCT.size());
    int quota = humansOn + (int)(p.keepT.size() + p.keepCT.size() + p.addT.size() + p.addCT.size());   // fill mode
    int kickQuota = humansOn + botsInGame - (int)p.kick.size();
    std::string adds;
    for (auto &n : p.addT)
        adds += "bot_add_t " + n + "; ";
    for (auto &n : p.addCT)
        adds += "bot_add_ct " + n + "; ";
    char q[48];
    snprintf(q, sizeof(q), "bot_quota %d", quota);
    adds += q;
    auto join = [](const std::vector<std::string> &v) {
        std::string s;
        for (auto &x : v)
            s += (s.empty() ? "" : ",") + x;
        return s.empty() ? std::string("-") : s;
    };
    Log("DIRECTOR (%s, cap %d): humans T %d CT %d, target %.0f%s, bots need T %.0f CT %.0f | keep T %s CT %s | "
        "add T %s CT %s | kick %s | quota %d",
        !g_match.live ? "no match live" : g_directPending ? "round end" : g_match.rounds ? "live" : "warmup/first round", cap, (int)humT.size(),
        (int)humCT.size(), p.target, botElo > 0 ? " (bot_elo)" : "", p.needT, p.needCT, join(p.keepT).c_str(), join(p.keepCT).c_str(),
        join(p.addT).c_str(), join(p.addCT).c_str(), join(p.kick).c_str(), quota);
    g_directBusy = true;
    g_lastDirect = now;
    g_directActions++;
    g_directPending = false;
    std::thread(DirectWorker, p.kick, adds, kickQuota).detach();
}
// ==== end STEP 8 ====================================================================================================

// ==== STEP 12 (Job 5, 2026-09-26) ====================================================================================
// D: the console "say" for a plan call when no bot can speak (the brain uses it only when no human is on CT: everyone
//    sees it). RCON runs on a worker thread (srcds answers it on its main thread).
static void SayWorker(std::string text) { Rcon({"say " + text}); }
static void ServerSay(const char *text)
{
    std::string t;
    for (const char *c = text; *c && t.size() < 160; c++)
        if (*c != ';' && *c != '"' && (unsigned char)*c >= 32) t += *c;
    std::thread(SayWorker, t).detach();
}
// I (his words 09-26: "I shouldn't always spawn with it. They could have it sometimes."): Valve's C4 handout skips the
//    bots while a human is on T when bot_defer_to_human_items is 1 (the game's own cvar, present in 1.38's server.so -
//    step 10 DIALS). brain_c4fair 1 (default) sets it to 0: the handout picks among ALL Ts, bots included. brain_c4fair 0
//    puts back the value the level had before (read once, the first time). Applied a few seconds into every level (after
//    the mode cfgs ran) and whenever the switch changes. Proof: "C4:" here, and the brain's "C4 ... carrier=<name> bot=0|1"
//    line every round.
static std::atomic<int> g_c4Stock{-2};   // the cvar's value before this plugin touched it (-2 not read yet)
static std::atomic<int> g_c4Applied{-1};
static std::atomic<bool> g_c4Busy{false};
static std::atomic<int> g_c4Level{0};   // +1 every level: a worker from the last level must not mark this one done
static void C4Worker(int fair, int level)
{
    std::vector<std::string> r = Rcon({"bot_defer_to_human_items"});
    int cur = CvarValue(r[0]);
    if (cur < 0)
    {
        Log("C4: could not read bot_defer_to_human_items - left as it is");
        if (level == g_c4Level) g_c4Applied = fair;
        g_c4Busy = false;
        return;
    }
    int unread = -2;
    g_c4Stock.compare_exchange_strong(unread, cur);
    int target = fair ? 0 : g_c4Stock.load();
    if (cur != target)
    {
        char c[64];
        snprintf(c, sizeof(c), "bot_defer_to_human_items %d", target);
        Rcon({c});
    }
    Log("C4: bot_defer_to_human_items %d -> %d (%s)", cur, target,
        fair ? "fair: Valve's handout picks among all Ts, bots too" : "brain_c4fair 0: the level's own value");
    if (level == g_c4Level) g_c4Applied = fair;   // else the level changed under it: the new level applies it again
    g_c4Busy = false;
}
static void C4Tick()
{
    if (CurrentGameType() < 0 || g_c4Busy) return;   // the level's mode (and cfgs) not read yet
    int fair = TestValue("brain_c4fair", 1) != 0 ? 1 : 0;
    if (fair == g_c4Applied) return;
    g_c4Busy = true;
    std::thread(C4Worker, fair, g_c4Level.load()).detach();
}
// ==== end STEP 12 ===================================================================================================

// ---- the plugin object: a hand-made vtable (CS:GO callback order, see s_vtable) ----
struct Plugin
{
    void **vtable;
};

static int s_slotCalls[40];
static void Probe(int slot)
{
    if (s_slotCalls[slot]++ < 2)
        Log("callback slot %d", slot);
}

static bool P_Load(Plugin *, CreateInterfaceFn engineFactory, CreateInterfaceFn serverFactory)
{
    Probe(0);
    g_helpers = engineFactory("ISERVERPLUGINHELPERS001", nullptr);
    g_playerInfoMgr = serverFactory("PlayerInfoManager002", nullptr);
    if (const char *p = getenv("PORT"))
        g_rconPort = atoi(p);
    srand((unsigned)time(nullptr));
    g_loadTime = time(nullptr);
    Log("loaded: helpers=%p playerinfo=%p rcon port %d", g_helpers, g_playerInfoMgr, g_rconPort);
    BrainLoad(PlayerInfo, g_playerInfoMgr, [](const char *n) { return BotElo(n); });
    BrainFactories(engineFactory, serverFactory, TestValue);
    BrainSetServerSay(ServerSay);   // STEP 12D
    LoadRoster();
    LoadElo();
    return g_helpers && g_playerInfoMgr;
}
static void P_Unload(Plugin *) { Probe(1); BrainUnload(); Log("unloaded"); }
static void P_Pause(Plugin *) { Probe(2); }
static void P_UnPause(Plugin *) { Probe(3); }
static const char *P_Description(Plugin *) { Probe(4); return "family_party (auto team + drop-in mode switch + bot director + elo)"; }
static void P_LevelInit(Plugin *, const char *map)
{
    Probe(5);
    g_map = map ? map : "";
    g_clients.clear();
    g_activeSince.clear();
    g_asked.clear();
    g_curType = -1;
    g_curMode = -1;
    g_match = MatchState();
    g_directPending = false;
    g_directActions = 0;
    g_lastLogScan = 0;
    g_c4Level++;        // STEP 12I: apply the C4 switch again on this level (after its cfgs)
    g_c4Applied = -1;
    Log("level %s", g_map.c_str());
    BrainLevelInit(map);
    std::thread(ModeReader, g_map).detach();
}
static void P_ServerActivate(Plugin *, edict_t *, int, int) { Probe(6); }
static void P_GameFrame(Plugin *, bool)
{
    Probe(7);
    // STEP 4 (2026-09-23): the 45 s team-select fallback is gone. Sending "jointeam" to an idle client from here
    // (helpers->ClientCommand) crashed the server and that client (kid-PC sim: container restart 21:52:58 UTC,
    // csgo_gc.exe tier0 0xc0000409). An idle player now simply stays on team select (mp_force_pick_time 3600).
    // STEP 8: once a second, read the new server-log lines (match events, elo) and let the bot director look.
    static time_t last;
    time_t now = time(nullptr);
    if (now != last)
    {
        last = now;
        PollLog();
        DirectorTick();
        BrainSetDiag((int)TestValue("brain_diag", 0));
        BrainSetSteer((int)TestValue("brain_steer", 1));
        BrainSetMode(CurrentGameType(), CurrentGameMode());
        C4Tick();   // STEP 12I
    }
    BrainFrame(g_clients);
}
static void P_LevelShutdown(Plugin *) { Probe(8); }

// Ask a human's client for the Play-menu request (once per level) and start their team-select idle timer.
static void AskRequest(edict_t *e, const char *via)
{
    g_clients.insert(e);
    void *pi = PlayerInfo(e);
    if (!pi || IsBot(pi) || g_asked.count(e))
        return;
    g_asked.insert(e);
    if (!g_activeSince.count(e))
        g_activeSince[e] = time(nullptr);
    int cookie = g_helpers ? VFunc<int (*)(void *, edict_t *, const char *)>(g_helpers, 2)(g_helpers, e, kRequestCvar) : -1;
    Log("human in (%s): %s (%s), asked for %s (cookie %d)", via, PI_Name(pi), PI_NetworkID(pi), kRequestCvar, cookie);
}

static void P_ClientFullyConnect(Plugin *, edict_t *e) { Probe(9); AskRequest(e, "fully connected"); }
static void P_ClientActive(Plugin *, edict_t *e)
{
    Probe(10);
    void *pi = PlayerInfo(e);
    if (pi && !IsBot(pi))
        g_activeSince[e] = time(nullptr);   // the team-select screen opens now
    AskRequest(e, "active");
}
static void P_ClientDisconnect(Plugin *, edict_t *e) { Probe(11); g_clients.erase(e); g_activeSince.erase(e); g_asked.erase(e); }
static void P_ClientPutInServer(Plugin *, edict_t *e, const char *) { Probe(12); g_clients.insert(e); }
static void P_SetCommandClient(Plugin *, int) { Probe(13); }
static void P_ClientSettingsChanged(Plugin *, edict_t *) { Probe(14); }
static int P_ClientConnect(Plugin *, bool *, edict_t *, const char *, const char *, char *, int) { Probe(15); return 0; }

// The family "Auto" rule. Returns 2 (T), 3 (CT), 1 (spectator) or 0 (nobody on a team yet: the game picks).
static int FamilyAutoPick(edict_t *e, const char *why)
{
    void *self = PlayerInfo(e);
    if (!self)
        return 0;
    int humans[4] = {0, 0, 0, 0};
    for (edict_t *c : g_clients)
    {
        if (c == e)
            continue;
        void *pi = PlayerInfo(c);
        if (!pi || IsBot(pi))
            continue;
        int t = PI_Team(pi);
        if (t >= 0 && t < 4)
            humans[t]++;
    }
    int type = CurrentGameType(), mode = CurrentGameMode();
    int cap = (type == 0 && mode == 2) ? 2 : 5;   // Wingman 2v2, every other family mode 5v5
    // test hook: csgo/addons/family_party_testcap.txt (absent in normal use) overrides the team size, so the
    // "both teams full -> spectate" branch can be shown with 3 test clients
    if (FILE *tc = fopen("csgo/addons/family_party_testcap.txt", "r"))
    {
        int v = 0;
        if (fscanf(tc, "%d", &v) == 1 && v >= 0 && v < 6)
        {
            cap = v;
            Log("TEST team size override: %d", cap);
        }
        fclose(tc);
    }
    if (FamilyPick(humans[2], humans[3], cap) == 0)
    {
        Log("Auto (%s) for %s: no humans on a team yet, the game picks (T %d / CT %d, cap %d)", why, PI_Name(self),
            humans[2], humans[3], cap);
        return 0;
    }
    int party = humans[2] >= humans[3] ? 2 : 3;   // the team with (more) humans; a tie goes to T
    int pick = FamilyPick(humans[2], humans[3], cap);
    Log("Auto (%s) for %s: humans T %d / CT %d, cap %d, party team %s -> %s", why, PI_Name(self), humans[2], humans[3],
        cap, party == 2 ? "T" : "CT", pick == 2 ? "T" : pick == 3 ? "CT" : "spectator (both teams full)");
    return pick;
}

static int P_ClientCommand(Plugin *, edict_t *e, const CCommandLayout *args)
{
    Probe(16);
    if (!args || !CommandLooksSane(args))
        return 0;
    AskRequest(e, args->argv[0]);
    if (BrainClientCommand(e, args->argc, args->argv))
        return 2;   // PLUGIN_STOP: a spot command (JOB 5d), the game does not see it
    if (args->argc < 2 || strcasecmp(args->argv[0], "jointeam") != 0 || strcmp(args->argv[1], "0") != 0)
        return 0;
    void *self = PlayerInfo(e);
    if (!self || IsBot(self))
        return 0;
    g_activeSince.erase(e);
    int pick = FamilyAutoPick(e, "button");
    if (pick)
    {
        // rewrite "0" in place: argv[1] points into the command's own argv buffer
        char *arg = const_cast<char *>(args->argv[1]);
        arg[0] = (char)('0' + pick);
    }
    return 0;   // PLUGIN_CONTINUE: the game now handles "jointeam <pick> ..."
}

static int P_NetworkIDValidated(Plugin *, const char *, const char *) { Probe(17); return 0; }

static void P_OnQueryCvarValueFinished(Plugin *, int cookie, edict_t *e, int status, const char *name, const char *value)
{
    Probe(18);
    void *pi = PlayerInfo(e);
    std::string who = pi ? PI_Name(pi) : "?";
    Log("cvar reply cookie %d from %s: status %d %s = \"%s\"", cookie, who.c_str(), status, name ? name : "", value ? value : "");
    if (status != 0 || !name || strcmp(name, kRequestCvar) != 0 || !value || strncmp(value, "family:", 7) != 0)
        return;
    // family:<type>:<mode>:<map,map,...>
    int t = -1, m = -1;
    char maps[512] = {};
    if (sscanf(value + 7, "%d:%d:%511s", &t, &m, maps) < 2 || t < 0 || t > 6 || m < 0 || m > 3)
    {
        Log("bad request \"%s\"", value);
        return;
    }
    std::vector<std::string> list;
    for (char *tok = strtok(maps, ","); tok; tok = strtok(nullptr, ","))
        if (ValidMapName(tok))
            list.push_back(tok);
    std::thread(SwitchWorker, who, t, m, list).detach();
}

static void P_OnEdictAllocated(Plugin *, edict_t *) { Probe(19); }
static void P_OnEdictFreed(Plugin *, const edict_t *) { Probe(20); }
static int P_Spare(Plugin *) { Probe(21); return 0; }

// CS:GO's IServerPluginCallbacks order (hl2sdk-csgo): ClientFullyConnect sits after LevelShutdown, and the engine uses
// this order for every interface version (observed: bots trigger slots 13, 12, 9, 14 = SetCommandClient,
// ClientPutInServer, ClientFullyConnect, ClientSettingsChanged).
static void *s_vtable[] = {
    (void *)P_Load, (void *)P_Unload, (void *)P_Pause, (void *)P_UnPause, (void *)P_Description,
    (void *)P_LevelInit, (void *)P_ServerActivate, (void *)P_GameFrame, (void *)P_LevelShutdown,
    (void *)P_ClientFullyConnect,
    (void *)P_ClientActive, (void *)P_ClientDisconnect, (void *)P_ClientPutInServer, (void *)P_SetCommandClient,
    (void *)P_ClientSettingsChanged, (void *)P_ClientConnect, (void *)P_ClientCommand, (void *)P_NetworkIDValidated,
    (void *)P_OnQueryCvarValueFinished, (void *)P_OnEdictAllocated, (void *)P_OnEdictFreed,
    (void *)P_Spare, (void *)P_Spare, (void *)P_Spare, (void *)P_Spare,
};
static Plugin s_plugin = {s_vtable};

extern "C" __attribute__((visibility("default"))) void *CreateInterface(const char *name, int *returnCode)
{
    if (name && strcmp(name, "ISERVERPLUGINCALLBACKS002") == 0)
    {
        if (returnCode)
            *returnCode = 0;
        return &s_plugin;
    }
    if (returnCode)
        *returnCode = 1;
    return nullptr;
}

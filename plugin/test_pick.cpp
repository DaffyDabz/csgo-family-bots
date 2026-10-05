// Unit test of the family "Auto" rule (FamilyPick in family_party.cpp): players press Auto one after another.
// STEP 8: also the bot director (FamilyDirect: solo human at 300, family average, split teams, Wingman, no churn),
// the team elo move (FamilyEloUpdate) and the elo -> rank map (FamilyRank).
// Built and run by plugin/build.sh (32-bit, like the plugin): g++ -m32 test_pick.cpp family_party.cpp.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "family_logic.h"   // STEP 12: the job-5 decision rules

extern "C" int FamilyPick(int humansT, int humansCT, int cap);

// same layout as in family_party.cpp
struct FamilyBot
{
    std::string name;
    double elo;
    int team;
};
struct FamilyPlan
{
    double target = 0, needT = 0, needCT = 0;
    std::vector<std::string> keepT, keepCT, addT, addCT, kick;
};
struct FamilyRated
{
    std::string key;
    double elo;
    int games;
    int team;
};
FamilyPlan FamilyDirect(const std::vector<double> &humT, const std::vector<double> &humCT, int cap,
                        const std::vector<FamilyBot> &bots, double defaultTarget, unsigned seed, bool force,
                        double botElo = 0);
bool FamilyEloUpdate(std::vector<FamilyRated> &pl, int scoreT, int scoreCT);
int FamilyRank(double elo);

static int g_fail;
static const char *Name(int t) { return t == 2 ? "T" : t == 3 ? "CT" : t == 1 ? "spectator" : "game picks"; }

// n players press Auto in turn; the first one (nobody on a team yet) gets `first` from the game.
static void Scenario(const char *label, int cap, int n, int first, const char *expect)
{
    int t = 0, ct = 0;
    char got[256] = {};
    for (int i = 0; i < n; i++)
    {
        int pick = FamilyPick(t, ct, cap);
        if (pick == 0)
            pick = first;
        if (pick == 2) t++;
        if (pick == 3) ct++;
        strcat(got, i ? " " : "");
        strcat(got, Name(pick));
    }
    bool ok = strcmp(got, expect) == 0;
    g_fail += !ok;
    printf("%s %-44s -> %-40s (T %d, CT %d)\n", ok ? "PASS" : "FAIL", label, got, t, ct);
}

// the shipped roster: 50 bots, elo 300 + 2100 * (i/49)^1.5 (servers/csgo/bots/roster.txt)
static std::vector<FamilyBot> Roster()
{
    const char *names[50] = {"Calvin", "Jenssen", "Adam", "Andy", "Dennis", "Duffy", "Ian", "Mike", "Adrian", "Bank",
                             "Brad", "Connor", "Dave", "Derek", "Don", "Eric", "Erik", "Finn", "Kevin", "Reed", "Rick",
                             "Troy", "Wade", "Wayne", "Xander", "Brian", "Chad", "Chet", "Gabe", "Hank", "Ivan", "Jim",
                             "Joe", "John", "Tony", "Tyler", "Victor", "Zane", "Cory", "Quinn", "Seth", "Vinny",
                             "Arnold", "Brett", "Kurt", "Kyle", "Zach", "Cliffe", "Wolf", "Gunner"};
    std::vector<FamilyBot> r;
    for (int i = 0; i < 50; i++)
        r.push_back({names[i], (double)(long)std::lround(300 + 2100 * pow(i / 49.0, 1.5)), 0});
    return r;
}
static double EloOf(const std::vector<FamilyBot> &r, const std::string &n)
{
    for (auto &b : r)
        if (b.name == n)
            return b.elo;
    return -1;
}
static void Check(bool ok, const char *what)
{
    g_fail += !ok;
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
}

// Prints a lineup and gives back the team averages (humans + bots).
static void Lineup(const char *label, const std::vector<double> &hT, const std::vector<double> &hCT, int cap,
                   double *avgT, double *avgCT, FamilyPlan *out, unsigned seed = 7)
{
    std::vector<FamilyBot> r = Roster();
    FamilyPlan p = FamilyDirect(hT, hCT, cap, r, 1000, seed, false);
    printf("---- %s (cap %d): target %.0f, bots need T %.0f CT %.0f\n", label, cap, p.target, p.needT, p.needCT);
    for (int team : {2, 3})
    {
        const std::vector<double> &h = team == 2 ? hT : hCT;
        const std::vector<std::string> &add = team == 2 ? p.addT : p.addCT;
        double sum = 0;
        printf("     %-2s humans:", team == 2 ? "T" : "CT");
        for (double x : h)
        {
            printf(" %.0f", x);
            sum += x;
        }
        printf(" | bots:");
        for (auto &n : add)
        {
            printf(" %s(%.0f)", n.c_str(), EloOf(r, n));
            sum += EloOf(r, n);
        }
        int n = (int)(h.size() + add.size());
        double avg = n ? sum / n : 0;
        printf(" | team avg %.0f\n", avg);
        (team == 2 ? *avgT : *avgCT) = avg;
    }
    *out = p;
}

static void DirectorTests()
{
    double aT, aCT;
    FamilyPlan p;
    // solo: one new player (300, Silver I) on T in competitive
    Lineup("SOLO human at 300, competitive", {300}, {}, 5, &aT, &aCT, &p);
    Check(p.addT.size() == 4 && p.addCT.size() == 5 && p.kick.empty(), "solo: 4 teammate bots + 5 enemy bots");
    Check(fabs(aCT - 300) <= 150, "solo: enemy bots average within 150 of the player's 300");
    Check(fabs(aT - aCT) <= 100, "solo: both teams' averages within 100");
    // family: Dad 900 + kid 300 on T -> family rank = average 600
    Lineup("FAMILY Dad 900 + kid 300 on T, competitive", {900, 300}, {}, 5, &aT, &aCT, &p);
    Check(fabs(p.target - 600) < 0.01, "family: target = average of the humans (600)");
    Check(fabs(aCT - 600) <= 150 && fabs(aT - aCT) <= 100, "family: enemy avg ~600 and both teams even");
    // family split: Dad 1500 on T, kid 300 on CT
    Lineup("FAMILY split: Dad 1500 on T, kid 300 on CT", {1500}, {300}, 5, &aT, &aCT, &p);
    Check(fabs(aT - aCT) <= 100, "split: both teams' averages within 100");
    Check(p.needT < p.needCT, "split: Dad's team gets the weaker bots");
    // Wingman solo at 300
    Lineup("SOLO human at 300, Wingman", {300}, {}, 2, &aT, &aCT, &p);
    Check(p.addT.size() == 1 && p.addCT.size() == 2, "wingman: 1 teammate bot + 2 enemy bots");
    Check(fabs(aT - aCT) <= 100, "wingman: both teams even");
    // a strong player
    Lineup("SOLO human at 2000, competitive", {2000}, {}, 5, &aT, &aCT, &p);
    Check(fabs(aCT - 2000) <= 150, "strong solo: enemy avg within 150 of 2000");
    // no churn: apply a lineup, run the director again -> nothing to do
    {
        std::vector<FamilyBot> r = Roster();
        FamilyPlan a = FamilyDirect({300}, {}, 5, r, 1000, 11, false);
        for (auto &b : r)
        {
            for (auto &n : a.addT)
                if (b.name == n)
                    b.team = 2;
            for (auto &n : a.addCT)
                if (b.name == n)
                    b.team = 3;
        }
        FamilyPlan again = FamilyDirect({300}, {}, 5, r, 1000, 99, false);
        Check(again.addT.empty() && again.addCT.empty() && again.kick.empty(), "stable: a second pass changes nothing");
        // a second human (900) joins T and takes a bot's slot (the engine kicks one T bot)
        for (auto &b : r)
            if (b.team == 2 && b.name == a.addT[0])
            {
                b.team = 0;
                break;
            }
        FamilyPlan joined = FamilyDirect({300, 900}, {}, 5, r, 1000, 5, false);
        printf("     kid 300 alone, then Dad 900 joins T: kick %d, add T %d, add CT %d (target %.0f)\n",
               (int)joined.kick.size(), (int)joined.addT.size(), (int)joined.addCT.size(), joined.target);
        Check(joined.target == 600 && !(joined.addT.empty() && joined.addCT.empty() && joined.kick.empty()),
              "drop-in: the new family average 600 re-picks the out-of-band bots");
    }
    // elo: 5 v 5 all at 300, T wins -> each T +16, each CT -16 (first games, K 32)
    std::vector<FamilyRated> pl;
    for (int i = 0; i < 10; i++)
        pl.push_back({"p" + std::to_string(i), 300, 0, i < 5 ? 2 : 3});
    FamilyEloUpdate(pl, 16, 10);
    Check(fabs(pl[0].elo - 316) < 0.01 && fabs(pl[9].elo - 284) < 0.01 && pl[0].games == 1,
          "elo: equal teams, T wins 16-10 -> T +16, CT -16");
    // underdog wins: T avg 300 beats CT avg 700 (K 16) -> bigger move
    pl.clear();
    for (int i = 0; i < 4; i++)
        pl.push_back({"u" + std::to_string(i), i < 2 ? 300.0 : 700.0, 12, i < 2 ? 2 : 3});
    FamilyEloUpdate(pl, 9, 7);
    printf("     underdog 300 beats 700 (K 16): T %.1f, CT %.1f\n", pl[0].elo, pl[2].elo);
    Check(pl[0].elo > 314 && pl[2].elo < 686, "elo: an upset moves more than an even game");
    pl.clear();
    pl.push_back({"a", 300, 0, 2});
    pl.push_back({"b", 300, 0, 3});
    FamilyEloUpdate(pl, 8, 8);
    Check(pl[0].elo == 300 && pl[1].elo == 300, "elo: a tie between equals moves nothing");
    // bot_elo override: kid at 300 on T, bots pinned to 1100 (Gold Nova I) on both teams; 0 = back to the humans
    {
        std::vector<FamilyBot> r = Roster();
        FamilyPlan b = FamilyDirect({300}, {}, 5, r, 1000, 7, false, 1100);
        double sT = 0, sCT = 0;
        for (auto &n : b.addT)
            sT += EloOf(r, n);
        for (auto &n : b.addCT)
            sCT += EloOf(r, n);
        double mT = b.addT.empty() ? 0 : sT / b.addT.size(), mCT = b.addCT.empty() ? 0 : sCT / b.addCT.size();
        printf("     bot_elo 1100, kid 300 on T: target %.0f, T bots avg %.0f (%d), CT bots avg %.0f (%d)\n", b.target, mT,
               (int)b.addT.size(), mCT, (int)b.addCT.size());
        Check(b.target == 1100 && b.addT.size() == 4 && b.addCT.size() == 5 && fabs(mT - 1100) <= 100 &&
                  fabs(mCT - 1100) <= 100,
              "bot_elo: with a human at 300 the bots average ~1100 on both teams");
        FamilyPlan off = FamilyDirect({300}, {}, 5, r, 1000, 7, false, 0);
        Check(off.target == 300, "bot_elo 0: target falls back to the human mean (300)");
    }
    Check(FamilyRank(300) == 1 && FamilyRank(2400) == 18 && FamilyRank(100) == 1 && FamilyRank(1350) == 9,
          "rank: 300 -> 1 (Silver I), 1350 -> 9, 2400 -> 18 (Global Elite)");
}

// ==== STEP 12 (job 5): the pure decision rules in family_logic.h ====
static void Near(float got, float want, float tol, const char *what)
{
    bool ok = fabsf(got - want) <= tol;
    g_fail += !ok;
    printf("%s %s (got %.3f, want %.3f +- %.3f)\n", ok ? "PASS" : "FAIL", what, got, want, tol);
}
static void HearingTests()
{
    using namespace fl;
    HearRanges hr = DefaultRanges();
    Check(Audible(hr, NZ_STEP, 800, 250, false, false), "hear: a running step at 800u is heard");
    Check(!Audible(hr, NZ_STEP, 1200, 250, false, false), "hear: a running step at 1200u is not (range 1100)");
    Check(!Audible(hr, NZ_STEP, 300, 130, true, false), "hear: a shift-walking step is silent even at 300u");
    Check(!Audible(hr, NZ_STEP, 300, 250, false, true), "hear: a crouching step is silent");
    Check(!Audible(hr, NZ_STEP, 300, 120, false, false), "hear: a step under 140 u/s (walk speed) is silent");
    Check(Audible(hr, NZ_LAND, 900, 0, true, false), "hear: a landing is heard even from a walker (900u)");
    Check(Audible(hr, NZ_SHOT, 1700, 0, false, false) && !Audible(hr, NZ_SHOT_SIL, 700, 0, false, false),
          "hear: a shot at 1700u is heard, a silenced one at 700u is not");
    Check(Audible(hr, NZ_DEFUSE, 1000, 0, false, true), "hear: a defuse at 1000u is heard (crouched defuser too)");
    Check(!Audible(hr, NZ_RELOAD, 800, 0, false, false) && Audible(hr, NZ_RELOAD, 600, 0, false, false), "hear: a reload at 600u yes, 800u no");
    Check(NoisePrio(NZ_DEFUSE) > NoisePrio(NZ_SHOT) && NoisePrio(NZ_SHOT) > NoisePrio(NZ_STEP), "hear: priority defuse > shot > step");
    HearReact lo = HearFor(0.f, 0.f), mid = HearFor(0.35f, 0.f), hi = HearFor(1.f, 0.f), loSlow = HearFor(0.f, 1.f);
    printf("     hearing by rank: Silver I delay %.2f-%.2f s err %.0f turn %.0f deg/s | h=0.35 delay %.2f err %.1f | Global delay %.2f err %.0f turn %.0f\n",
           lo.delay, loSlow.delay, lo.errDeg, lo.turnSpeed, mid.delay, mid.errDeg, hi.delay, hi.errDeg, hi.turnSpeed);
    Check(lo.delay > mid.delay && mid.delay > hi.delay, "hear: higher rank reacts sooner");
    Check(loSlow.delay <= 0.8f, "hear: the lowest rank still always reacts (<= 0.8 s)");
    Check(lo.errDeg > hi.errDeg && lo.turnSpeed < hi.turnSpeed && lo.holdFor < hi.holdFor, "hear: higher rank is sharper, turns faster, holds longer");
    Check(!lo.corner && hi.corner && lo.stopChance == 0.f && hi.stopChance > 0.5f, "hear: only higher ranks pre-aim corners / stop to listen");
    Check(HearBetter(3, 900, 1, 300, 0.1f), "hear: a defuse beats a close step");
    Check(!HearBetter(1, 700, 1, 800, 0.2f) && HearBetter(1, 400, 1, 800, 0.2f), "hear: a slightly closer step does not re-trigger, a much closer one does");
    Check(HearBetter(1, 900, 1, 300, 1.0f), "hear: a stale reaction (> 0.8 s) is replaced");
    // corner pick: the bot at the origin, a noise at (1000, 0): spots behind the bot or far off to the side are skipped
    V3 bot{0, 0, 0}, noise{1000, 0, 0};
    std::vector<V3> spots = {{-300, 0, 0}, {800, 100, 0}, {900, -40, 0}, {600, 700, 0}, {950, 10, 0}};
    std::vector<char> vis = {1, 1, 1, 1, 0};
    Check(PickCorner(bot, noise, spots, vis) == 2, "hear: the visible corner nearest the noise, ahead of the bot");
    vis = {1, 0, 0, 1, 0};
    Check(PickCorner(bot, noise, spots, vis) == -1, "hear: no usable corner -> aim at the noise itself");
    Near(TurnStep(170.f, -170.f, 5.f), 175.f, 0.01f, "turn: 170 -> -170 goes the short way (+5 per step)");
    Near(TurnStep(10.f, 12.f, 5.f), 12.f, 0.01f, "turn: stops on the target");
    Near(PitchTo({0, 0, 64}, {100, 0, 0}), 32.6f, 0.2f, "pitch: a target below the eye is a positive (down) pitch");
}

static void FireTests()
{
    using namespace fl;
    // a Gold bot (0.35) fighting a visible shooter, not hurt much: dodge with a high draw, nothing with a low one
    FireSit s{0.35f, true, true, 80, 20, 1, 900, true, false};
    FireChoice c = FireDecide(s, 0.9f, 0.1f);
    Check(c.act == FA_DODGE && c.skill > 0.5f, "fire: a Gold bot shot while it fights side-steps (Skill over the 0.5 line)");
    Check(FireDecide(s, 0.9f, 0.99f).act == FA_NONE, "fire: ... but not every time");
    s.seen = false;
    Check(FireDecide(s, 0.9f, 0.99f).act == FA_TURN, "fire: shot from outside its view -> it always turns (every rank)");
    s.dial = 0.f;
    Check(FireDecide(s, 0.9f, 0.99f).act == FA_TURN, "fire: ... Silver I too");
    // losing: two hits -> cover when a hidden spot exists and it is not point blank
    FireSit l{0.8f, true, true, 45, 60, 2, 900, true, false};
    Check(FireLosing(l) && FireDecide(l, 0.1f, 0.5f).act == FA_COVER, "fire: losing the trade -> breaks the line to cover");
    l.dist = 250;
    Check(FireDecide(l, 0.1f, 0.5f).act != FA_COVER, "fire: never runs for cover inside 300u (the panic fight)");
    l.dist = 900; l.cover = false;
    Check(FireDecide(l, 0.1f, 0.5f).act != FA_COVER, "fire: no hidden spot -> no cover run");
    l.cover = true; l.objective = true;
    Check(FireDecide(l, 0.1f, 0.1f).act == FA_NONE, "fire: a defusing / planting bot stays on the job");
    // a Silver I bot far away does not dodge (would strafe-tap), up close it may (the stock sprays there anyway)
    FireSit lo{0.f, true, true, 90, 10, 1, 900, false, false};
    Check(FireDecide(lo, 0.5f, 0.0f).act == FA_NONE, "fire: Silver I at 900u keeps spraying (no side-step band far away)");
    lo.dist = 300;
    Check(FireDecide(lo, 0.5f, 0.0f).act == FA_DODGE, "fire: Silver I at 300u may dodge while it sprays");
    FireChoice fast = FireDecide(l, 1, 1), slow = FireDecide(lo, 1, 1);
    Check(fast.delay < slow.delay && fast.turnSpeed > slow.turnSpeed && fast.hold < slow.hold, "fire: higher rank reacts sooner, turns faster, re-peeks sooner");
    // cover spot: the bot at the origin, the shooter at (1000, 0)
    std::vector<V3> sp = {{-200, 0, 0}, {50, 0, 0}, {0, 300, 0}, {300, 20, 0}, {0, -200, 0}, {0, -250, 200}};
    std::vector<char> hid = {1, 1, 1, 1, 0, 1}, cov = {0, 0, 1, 0, 0, 0};
    int k = PickCover({0, 0, 0}, {1000, 0, 0}, sp, hid, cov);
    Check(k == 0 || k == 2, "fire: cover is away from the shooter, hidden, not too near, same floor");
    Check(k == 0, "fire: the nearer hidden spot wins unless an IN_COVER one is within 60u of it");
    std::vector<float> still(15, 10.f), moving(15, 180.f);
    Check(StoodStill(still) && !StoodStill(moving), "fire: stood-still classifier (1.5 s of speed samples)");
}

static std::string Roles(const std::vector<int> &r)
{
    std::string o;
    for (int x : r) o += x < 0 ? 'm' : (char)('A' + x);
    return o;
}
static void DefenderTests()
{
    using namespace fl;
    // slots: the anchors of every site first, then the seconds, then the rotator (m)
    Check(Roles(CtRoles(5, 2, true, -1)) == "ABABm", "ct: 5 CTs, two sites + mid -> 2 / 2 / 1 (anchors A, B first)");
    Check(Roles(CtRoles(4, 2, true, -1)) == "ABAB", "ct: 4 -> 2 / 2");
    Check(Roles(CtRoles(3, 2, true, -1)) == "ABm", "ct: 3 -> 1 / 1 / rotator");
    Check(Roles(CtRoles(2, 2, true, -1)) == "AB" && Roles(CtRoles(1, 2, true, -1)) == "A", "ct: 2 -> one per site; 1 -> one");
    Check(Roles(CtRoles(5, 2, true, 0)) == "ABAAm", "ct: stack A with 5 -> 3 / 1 / 1 (B keeps its anchor)");
    Check(Roles(CtRoles(4, 2, true, 1)) == "ABBB", "ct: stack B with 4 -> 1 / 3");
    Check(Roles(CtRoles(5, 2, false, -1)) == "ABABA", "ct: no mid point -> 3 / 2");
    Check(Roles(CtRoles(5, 3, true, -1)) == "ABCAB", "ct: three sites -> an anchor on each");
    bool anchorsAlways = true;
    for (int n = 2; n <= 6; n++)
        for (int st = -1; st < 2; st++)
        {
            std::vector<int> r = CtRoles(n, 2, true, st);
            int a = 0, b = 0;
            for (int x : r) { a += x == 0; b += x == 1; }
            anchorsAlways = anchorsAlways && a >= 1 && b >= 1 && (int)r.size() == n;
        }
    Check(anchorsAlways, "ct: every size 2-6, stacked or not, keeps >= 1 CT on each site");
    // stacks: never for a sloppy team; 12% base; towards the site hit in 2 of the last 3 rounds (32%)
    Check(CtStackPick({0, 0, 0}, 2, false, 0.f, 0.f) == -1, "ct: a sloppy team never stacks");
    Check(CtStackPick({}, 2, true, 0.11f, 0.7f) == 1 && CtStackPick({}, 2, true, 0.13f, 0.7f) == -1, "ct: 12% base stack, random site");
    Check(CtStackPick({1, 0, 0}, 2, true, 0.30f, 0.9f) == 0 && CtStackPick({1, 0, 0}, 2, true, 0.33f, 0.9f) == -1,
          "ct: Ts hit A twice in the last 3 -> 32% stack towards A");
    Check(CtStackPick({0, 0, 1, 1, -1}, 2, true, 0.2f, 0.f) == 1, "ct: B twice in the last 3 -> B (older A rounds do not count)");
    Check(CtStackPick({0, 0, -1, 1, -1}, 2, true, 0.2f, 0.f) == -1, "ct: only the last 3 rounds count (A twice before them: 12% only)");
    Check(TGround(100, 140) && !TGround(100, 125), "ct: T ground = Ts there 3 s before the CTs can be");
}

// simulate a map's plans for many rounds with the history fed back: counts per text, mid-heavy share, longest repeat
static void PlanSim(const char *map, const std::vector<fl::PlanSite> &sites, int rounds, float smart, int *midHeavy, int *maxRun,
                    int *midIn3, std::map<std::string, int> &count)
{
    using namespace fl;
    std::vector<TPlan> hist;
    unsigned rng = 12345;
    auto rnd = [&]() { rng = rng * 1103515245u + 12345u; return (float)((rng >> 8) % 100000) / 100000.f; };
    *midHeavy = *maxRun = *midIn3 = 0;
    int run = 0;
    for (int r = 0; r < rounds; r++)
    {
        TPlan p = PickPlan(sites, 5, smart, hist, true, rnd());
        count[PlanText(p, sites)]++;
        *midHeavy += MidHeavy(p);
        run = !hist.empty() && SamePlan(p, hist.back()) ? run + 1 : 1;
        *maxRun = std::max(*maxRun, run);
        hist.push_back(p);
        if (hist.size() >= 3)
        {
            int m = 0;
            for (size_t i = hist.size() - 3; i < hist.size(); i++) m += MidHeavy(hist[i]);
            if (m >= 2) (*midIn3)++;
        }
        if (hist.size() > 8) hist.erase(hist.begin());
    }
    printf("     %s plans over %d rounds:", map, rounds);
    for (auto &kv : count) printf(" %s %d,", kv.first.c_str(), kv.second);
    printf(" mid-heavy %d (%.0f%%), windows of 3 with 2+ mid-heavy %d, longest repeat %d\n", *midHeavy, 100.0 * *midHeavy / rounds, *midIn3, *maxRun);
}
static void PlanTests()
{
    using namespace fl;
    // de_dust2-like: A = long (0) + short through mid (1, mid); B = tunnels (0) + mid doors (1, mid)
    std::vector<PlanSite> d2 = {{"A", {0, 1}}, {"B", {0, 1}}};
    // de_mirage-like: A = ramp/palace (0) + through mid/connector (1, mid); B = apartments (0) + mid/market (1, mid)
    std::vector<PlanSite> mi = {{"A", {0, 1}}, {"B", {0, 1}}};
    // an old map file (one approach each, the only one through mid on B)
    std::vector<PlanSite> old = {{"A", {0}}, {"B", {1}}};
    int mh, run, in3;
    std::map<std::string, int> c1, c2, c3;
    PlanSim("de_dust2-like", d2, 300, 0.4f, &mh, &run, &in3, c1);
    Check(mh <= 300 / 3 && in3 == 0, "plan: dust2 - mid-heavy under a third of rounds and never 2 in any 3");
    Check(run <= 2, "plan: dust2 - the same call never 3 rounds running");
    Check(c1["A push"] > 20 && c1["B push"] > 20 && c1["A split"] > 20 && c1["B split"] > 20 && c1["A to B"] + c1["B to A"] > 10,
          "plan: dust2 - pushes, splits and fakes on both sites all get called");
    PlanSim("de_mirage-like (smart 0.9)", mi, 300, 0.9f, &mh, &run, &in3, c2);
    Check(mh <= 100 && in3 == 0 && run <= 2, "plan: mirage - same limits for a smart team");
    PlanSim("old map file", old, 300, 0.4f, &mh, &run, &in3, c3);
    Check(in3 == 0 && run <= 2 && c3["B push"] > 0 && c3["A push"] > c3["B push"], "plan: one approach through mid on B -> B less often, never 2 in 3");
    // text
    TPlan p;
    p.site = 1;
    p.ap1 = 1;
    Check(PlanText(p, d2) == "mid to B", "plan text: a push through the mid approach = 'mid to B'");
    p.ap1 = 0;
    Check(PlanText(p, d2) == "B push", "plan text: 'B push'");
    p.type = PL_FAKE;
    p.from = 0;
    Check(PlanText(p, d2) == "A to B", "plan text: fake A then B = 'A to B'");
    // a forced site (a human typed b)
    std::vector<TPlan> none;
    bool onlyB = true;
    for (int i = 0; i < 50; i++)
    {
        TPlan q = PickPlan(d2, 5, 0.5f, none, true, i / 50.f, 1);
        onlyB = onlyB && q.site == 1 && q.type != PL_FAKE;
    }
    Check(onlyB, "plan: a human's 'b' -> only B push / B split");
    Check(PickPlan(old, 5, 0.5f, none, true, 0.5f, 0, PL_SPLIT).type == PL_PUSH, "plan: 'a split' where A has one approach -> A push");
    // chat parsing
    std::vector<std::string> L = {"A", "B"};
    int ty;
    Check(ParseCall("b", L, &ty) == 1 && ty == -1, "chat: 'b' -> B");
    Check(ParseCall("A", L, &ty) == 0, "chat: 'A' -> A");
    Check(ParseCall("go b!", L, &ty) == 1 && ParseCall("rush B", L, &ty) == 1 && ty == PL_PUSH, "chat: 'go b!' / 'rush B'");
    Check(ParseCall("b split", L, &ty) == 1 && ty == PL_SPLIT, "chat: 'b split' -> B, split");
    Check(ParseCall("lets go a site", L, &ty) == 0, "chat: 'lets go a site' -> A");
    Check(ParseCall("a bot is bad", L, &ty) == -1 && ParseCall("a or b", L, &ty) == -1 && ParseCall("nice", L, &ty) == -1,
          "chat: ordinary chat is not a call");
    Check(ParseCall("", L, &ty) == -1 && ParseCall("b b b b b", L, &ty) == -1, "chat: empty / too long");
    // the bomb carrier's heading: T spawn (0,0); A route north then east, B route south then east
    std::vector<Route> R = {{0, {{0, 0, 0}, {0, 1500, 0}, {2500, 1500, 0}}}, {1, {{0, 0, 0}, {0, -1500, 0}, {2500, -1500, 0}}}};
    std::vector<V3> sc = {{2800, 1500, 0}, {2800, -1500, 0}};
    std::vector<float> sr = {450, 450};
    Check(CarrierHeading(R, sc, sr, {0, 200, 0}) == -1, "carrier: still near the spawn -> not decided");
    Check(CarrierHeading(R, sc, sr, {0, 1200, 0}) == 0, "carrier: 1200u up the A route -> A");
    Check(CarrierHeading(R, sc, sr, {1000, -1450, 0}) == 1, "carrier: along the B route -> B");
    Check(CarrierHeading(R, sc, sr, {1500, 0, 0}) == -1, "carrier: off every route -> not decided");
    Check(CarrierHeading(R, sc, sr, {2700, -1400, 0}) == 1, "carrier: on site B -> B");
    // shared start: both routes run east first, then split north / south
    std::vector<Route> S = {{0, {{0, 0, 0}, {1500, 0, 0}, {1500, 1500, 0}}}, {1, {{0, 0, 0}, {1500, 0, 0}, {1500, -1500, 0}}}};
    std::vector<V3> sc2 = {{1500, 1800, 0}, {1500, -1800, 0}};
    Check(CarrierHeading(S, sc2, sr, {1300, 0, 0}) == -1, "carrier: on the shared part of two routes -> not decided");
    Check(CarrierHeading(S, sc2, sr, {1500, -700, 0}) == 1, "carrier: past the fork towards B -> B");
}

static void ChatTests()
{
    using namespace fl;
    unsigned a = 0;
    const unsigned char f1[] = {0x55, 0x89, 0xe5, 0x8b, 0x45, 0x0c, 0xa3, 0x44, 0x33, 0x22, 0x11, 0x5d, 0xc3};   // frame, eax
    Check(SetterPattern(f1, sizeof(f1), &a) && a == 0x11223344u, "chat: setter with a frame (eax) -> its global");
    const unsigned char f2[] = {0x8b, 0x44, 0x24, 0x08, 0xa3, 0x10, 0x20, 0x30, 0x01, 0xc3};                     // no frame
    Check(SetterPattern(f2, sizeof(f2), &a) && a == 0x01302010u, "chat: setter without a frame");
    const unsigned char f3[] = {0x55, 0x89, 0xe5, 0x8b, 0x55, 0x0c, 0x5d, 0x89, 0x15, 0x00, 0x00, 0x60, 0x08, 0xc3};   // edx, pop first
    Check(SetterPattern(f3, sizeof(f3), &a) && a == 0x08600000u, "chat: setter (edx, pop before the store)");
    const unsigned char g1[] = {0x55, 0xa1, 0x44, 0x33, 0x22, 0x11, 0x89, 0xe5, 0x5d, 0xc3};                     // a getter (step 10)
    Check(!SetterPattern(g1, sizeof(g1), &a), "chat: a getter is not a setter");
    const unsigned char g2[] = {0x55, 0x89, 0xe5, 0x8b, 0x45, 0x0c, 0x83, 0xc0, 0x01, 0xa3, 0x44, 0x33, 0x22, 0x11, 0x5d, 0xc3};   // eax+1
    Check(!SetterPattern(g2, sizeof(g2), &a), "chat: anything between load and store -> not the setter");
    const unsigned char g3[] = {0x55, 0x89, 0xe5, 0x8b, 0x45, 0x08, 0xa3, 0x44, 0x33, 0x22, 0x11, 0x5d, 0xc3};   // arg0 (this)
    Check(!SetterPattern(g3, sizeof(g3), &a), "chat: storing 'this' is not it");
    Check(!SetterPattern(f1, 8, &a), "chat: a short read is not it");
    Check(EdictStride({{1000, 1}, {1020, 2}, {1100, 6}}) == 20, "chat: edict indices consistent with a 20-byte edict array");
    Check(EdictStride({{1000, 1}, {1020, 2}, {1100, 7}}) == 0, "chat: an inconsistent index -> not trusted");
    Check(EdictStride({{1000, 1}}) == 0 && EdictStride({{1000, 1}, {1020, 1}}) == 0 && EdictStride({{1000, 0}, {1020, 1}}) == 0,
          "chat: one player / duplicate / zero index -> not trusted");
}

static void PeekTests()
{
    using namespace fl;
    PeekParams lo = PeekFor(0.f), gold = PeekFor(0.33f), hi = PeekFor(1.f);
    printf("     habits by rank: Silver I pre-aim %.0f%% jiggle %.0f%% | Gold pre-aim %.0f%% jiggle %.0f%% out %.2f s | Global pre-aim %.0f%% jiggle %.0f%% x%d out %.2f s\n",
           100 * lo.preaim, 100 * lo.jiggle, 100 * gold.preaim, 100 * gold.jiggle, gold.out, 100 * hi.preaim, 100 * hi.jiggle, hi.peeks, hi.out);
    Check(lo.jiggle == 0.f && hi.jiggle > 0.7f && lo.preaim < hi.preaim, "peek: the bottom never jiggles (walks into sight), the top mostly does");
    Check(hi.out < lo.out && hi.back < lo.back && hi.peeks >= lo.peeks, "peek: a higher rank peeks shorter, more often");
    std::vector<float> off = {64, -64, 96, -96, 128, -128};
    Check(PickPeek(off, {0, 0, 1, 0, 1, 1}) == 2, "peek: the smallest sideways step that sees the corner");
    Check(PickPeek(off, {0, 0, 0, 0, 0, 0}) == -1, "peek: no step sees it -> no jiggle");
    // pre-aim: the bot at the origin moving east (yaw 0)
    std::vector<V3> sp = {{600, 0, 0}, {400, 300, 0}, {-500, 0, 0}, {200, 0, 0}, {1500, 0, 0}, {500, -100, 0}};
    std::vector<char> one(6, 1);
    Check(PickPreaim({0, 0, 0}, 0.f, sp, one, one, one) == 1, "preaim: the nearest angle ahead (250..1300u, within 75 deg)");
    std::vector<char> vis = {1, 0, 1, 1, 1, 1};
    Check(PickPreaim({0, 0, 0}, 0.f, sp, vis, one, one) == 5, "preaim: not an angle it cannot see");
    std::vector<char> reach = {1, 1, 1, 1, 1, 0}, fresh = {0, 1, 1, 1, 1, 1};
    Check(PickPreaim({0, 0, 0}, 0.f, sp, vis, reach, fresh) == -1, "preaim: not where the enemy cannot be yet, not one just checked");
    Check(PickPreaim({0, 0, 0}, 180.f, sp, one, one, one) == 2, "preaim: moving west -> the angle behind the start is ahead now");
}

static void GoldTests()
{
    using namespace fl;
    Check(GoldBump(0.f) == 0.f && GoldBump(0.095f) == 0.f, "gold: no change at the bottom (elo 300 .. ~700: the kid floor's both sides)");
    Check(GoldBump(0.33f) == 1.f && GoldBump(0.46f) == 1.f, "gold: the full lift at Gold Nova (aim dial 0.33) .. ~1300");
    Check(GoldBump(0.74f) == 0.f && GoldBump(0.9f) == 0.f && GoldBump(1.f) == 0.f, "gold: no change from ~1700 up (step 11's top calibration)");
    bool mono = true;
    for (float a = 0.10f; a < 0.26f; a += 0.01f) mono = mono && GoldBump(a + 0.01f) >= GoldBump(a);
    for (float a = 0.46f; a < 0.74f; a += 0.01f) mono = mono && GoldBump(a + 0.01f) <= GoldBump(a);
    Check(mono && GoldBump(0.18f) > 0.3f && GoldBump(0.18f) < 0.7f, "gold: smooth rise and fall");
    // step 11's Want() values for a Gold Nova I bot (aim = react = 0.33), then with the full lift
    float r = sqrtf(0.33f), a = powf(0.33f, 0.75f);
    auto LogLerp = [](float x, float y, float t) { return expf(logf(x) + (logf(y) - logf(x)) * t); };
    float rt = Lerp(0.40f, 0.06f, r), fi = LogLerp(10.f, 0.5f, a), fo = LogLerp(0.45f, 0.05f, a), laa = LogLerp(3000.f, 15000.f, a);
    GoldMul m = GoldFor(1.f);
    printf("     Gold Nova I (dial 0.33): ReactionTime %.3f -> %.3f s, AimFocusInitial %.2f -> %.2f deg, OffsetScale %.3f -> %.3f, look accel %.0f -> %.0f\n",
           rt, rt * m.rt, fi, fi * m.fi, fo, fo * m.fo, laa, laa * m.laa);
    Check(fabsf(rt * m.rt - 0.144f) < 0.005f && fabsf(fi * m.fi - 1.63f) < 0.05f, "gold: Gold Nova I reacts in ~0.14 s and aims its first shot within ~1.6 deg");
    GoldMul none = GoldFor(0.f);
    Check(none.rt == 1.f && none.fi == 1.f && none.fo == 1.f && none.laa == 1.f, "gold: no bump = step 11's values exactly");
}

static void RestTests()
{
    using namespace fl;
    // F5 clearing: 8 corners over 3 Ts -> 3 / 3 / 2, every corner someone's
    std::vector<int> a = ClearShare(8, 3, 0), b = ClearShare(8, 3, 1), c = ClearShare(8, 3, 2);
    Check(a.size() == 3 && b.size() == 3 && c.size() == 2 && a[0] == 0 && b[0] == 1 && c[1] == 5, "clear: 8 corners over 3 Ts, shared out");
    Check(ClearShare(3, 5, 4).empty() && ClearShare(0, 2, 0).empty(), "clear: more Ts than corners -> some have none");
    Check(fabsf(ClearCheck(1.f) - 1.f) < 1e-5f && fabsf(ClearCheck(0.f) - 0.4f) < 1e-5f, "clear: the top checks 100% of its corners, the bottom 40%");
    Check(ClearSlow(1.f, 0.5f) && !ClearSlow(1.f, 0.6f) && !ClearSlow(0.f, 0.06f), "clear: a smart team takes it slow ~55% of rounds, a sloppy one ~5%");
    Check(ClearHold(true, 1.f) > ClearHold(false, 1.f), "clear: a slow take holds each corner longer");
    // F8 retakes
    Check(RetakeWait(800, 1500, 25, 10) && !RetakeWait(800, 1300, 25, 10), "retake: the first waits for one 600u+ behind, not for a close one");
    Check(!RetakeWait(800, 1500, 14, 10) && !RetakeWait(1000, 1800, 25, 10), "retake: never on a tight clock, not before it is near the bomb");
    Check(fabsf(RetakeDeadline(100, 5, 10) - 115.f) < 0.01f && fabsf(RetakeDeadline(100, 20, 10) - 107.f) < 0.01f, "retake: gather <= 15 s, less for a long walk");
    // F9 grenade flight: level throw goes up first, lands ~ where the lineage says
    std::vector<V3> arc = ThrowArc({0, 0, 64}, 0.f, 0.f, 0.1f, 2.0f);
    Check(arc[1].z > 64.f && arc[1].x > 60.f, "nade: a level throw goes forward and 10 deg up");
    float peak = 0;
    for (auto &q : arc) peak = std::max(peak, q.z);
    Check(peak > 70.f && peak < 120.f, "nade: its peak is a short lob (~100u over the eye for a level throw)");
    std::vector<V3> down = ThrowArc({0, 0, 64}, 60.f, 90.f, 0.1f, 0.5f);
    Check(down[3].z < 64.f && fabsf(down[3].x) < 5.f && down[3].y > 50.f, "nade: looking down / north throws down / north");
    std::vector<V3> mates = {{300, 0, 0}}, foes = {{1000, 0, 0}};
    Check(NadeDanger({320, 40, 0}, {0, 0, 0}, mates, foes, true, false) == ND_TEAM, "nade: a fire on a teammate is dumb (team)");
    Check(NadeDanger({100, 0, 0}, {0, 0, 0}, {}, foes, false, false) == ND_SELF, "nade: at its own feet is dumb (self)");
    Check(NadeDanger({2000, 0, 0}, {0, 0, 0}, {}, {}, true, false) == ND_NOTARGET && NadeDanger({2000, 0, 0}, {0, 0, 0}, {}, {}, true, true) == 0,
          "nade: a fire where no enemy is known is dumb, unless it is on the bomb");
    Check(NadeDanger({1400, 0, 0}, {0, 0, 0}, mates, foes, true, false) == 0, "nade: a fire near a known enemy, away from the team, is fine");
}

// ==== JOB 5b (addendum 2): L-P ====
static float TRnd()   // a fixed pseudo-random stream (the tests must not depend on the libc rand)
{
    static unsigned x = 2463534242u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return (x % 100000) / 100000.f;
}
static void PushTests()
{
    using namespace fl;
    Check(PushChance(0.4f, 1) == 0.f && PushChance(0.4f, 2) > 0.f && PushChance(0.4f, 2) < PushChance(0.4f, 5),
          "push: never two rounds in a row, half as likely right after one");
    Check(PlanPush(1.f, 9, 5, {}, 2, 0.f, 0.5f, 0.5f, 0.5f).target < 0 && PlanPush(1.f, 9, 0, {}, 5, 0.f, 0.5f, 0.5f, 0.5f).target < 0,
          "push: never with fewer than 3 CTs or no push spots");
    // a Gold team (smarts 0.38), 5 push spots, 1000 rounds: how often, how varied
    std::vector<int> hist, count(5, 0);
    int last = -100, pushes = 0, inRow = 0, repeat = 0, two = 0, badT = 0;
    for (int r = 0; r < 1000; r++)
    {
        PushPlan pp = PlanPush(0.38f, r - last, 5, hist, 5, TRnd(), TRnd(), TRnd(), TRnd());
        if (pp.target < 0) continue;
        if (r - last == 1) inRow++;
        int from = std::max(0, (int)hist.size() - 2);
        for (int k = from; k < (int)hist.size(); k++) repeat += hist[k] == pp.target;
        hist.push_back(pp.target);
        count[pp.target]++;
        pushes++;
        two += pp.n == 2;
        last = r;
        badT += !(pp.at >= 2.f && pp.at <= 10.f && pp.hold >= 2.5f && pp.hold <= 5.f);
    }
    Check(badT == 0, "push: 2-10 s into the round, held 2.5-5 s");
    int mn = *std::min_element(count.begin(), count.end());
    printf("     Gold team, 1000 rounds: %d pushes (%.1f%%), 2-man %d, per spot %d %d %d %d %d\n", pushes, pushes / 10.f, two, count[0], count[1],
           count[2], count[3], count[4]);
    Check(pushes >= 80 && pushes <= 200, "push: a Gold team pushes in 8-20% of rounds (not every round)");
    Check(inRow == 0 && repeat == 0, "push: never two rounds in a row, never one of the last two spots again");
    Check(mn >= pushes / 10, "push: every spot gets pushed (no favourite)");
    Check(PushSpotOk(300, 320) && !PushSpotOk(300, 400) && !PushSpotOk(1200, 100) && PushSpotScore(300, 250) == 50,
          "push: a spot the CTs reach within 3 s of the Ts is contested, deeper is T ground");
}

static void SetupTests()
{
    using namespace fl;
    auto str = [](const std::vector<int> &r) {
        std::string o;
        for (int x : r) o += x == CR_MID ? "M" : x == CR_LURK ? "L" : std::string(1, (char)('A' + x));
        return o;
    };
    Check(str(CtRoles5b(5, 2, true, true, -1, 1, 0)) == "ABMLB", "setup: 5 CTs = A, B, mid, lurker, + B (the extra)");
    Check(str(CtRoles5b(5, 2, true, true, 0, 1, 0)) == "ABMAA", "setup: stacked A = A, B, mid, A, A (no lurker)");
    Check(str(CtRoles5b(4, 2, true, true, -1, 0, 1)) == "BAML" && str(CtRoles5b(3, 2, true, true, -1, 0, 0)) == "ABM",
          "setup: 4 CTs = both anchors, mid, lurker; 3 = both anchors + mid");
    Check(str(CtRoles5b(1, 2, true, true, -1, 0, 1)) == "B" && str(CtRoles5b(2, 2, true, true, -1, 0, 0)) == "AB", "setup: 1-2 CTs = anchors only");
    Check(str(CtRoles5b(5, 2, false, false, -1, 0, 0)) == "ABABA", "setup: no mid / lurker spot on the map = the extras to the sites");
    bool oneMid = true;
    for (int n = 1; n <= 8; n++)
        for (int st = -1; st < 2; st++)
        {
            std::vector<int> r = CtRoles5b(n, 2, true, true, st, 1, n);
            int m = 0, l = 0, a = 0, b = 0;
            for (int x : r) { m += x == CR_MID; l += x == CR_LURK; a += x == 0; b += x == 1; }
            oneMid = oneMid && m <= 1 && l <= 1 && (int)r.size() == n && (n < 2 || (a >= 1 && b >= 1));
        }
    Check(oneMid, "setup: never 2 on mid or 2 lurkers, both sites anchored from 2 CTs up (1-8 CTs, stacked or not)");
    // spots: 6 candidates on a site, 3 CTs a round for 20 rounds: never a spot twice in one round, rarely one of last round's
    std::vector<SpotCand> c;
    for (int i = 0; i < 6; i++) c.push_back({{(float)i * 300.f, 0, 0}, i < 2 ? 1.f : 0.5f});
    std::vector<V3> last;
    int reuse = 0, dup = 0, good = 0, total = 0;
    for (int r = 0; r < 20; r++)
    {
        std::vector<V3> taken;
        for (int k = 0; k < 3; k++)
        {
            std::vector<float> rnd;
            for (size_t i = 0; i < c.size(); i++) rnd.push_back(TRnd());
            int j = PickSpot(c, taken, last, 0.8f, rnd);
            if (j < 0) { dup++; continue; }
            for (auto &t : taken) dup += Dist(t, c[j].p) < 150.f;
            for (auto &h : last) reuse += Dist(h, c[j].p) < 120.f;
            good += c[j].q > 0.9f;
            total++;
            taken.push_back(c[j].p);
        }
        last = taken;
    }
    printf("     spots: 20 rounds x 3 CTs on 6 spots: %d reused from the round before, %d on the 2 best spots\n", reuse, good);
    Check(dup == 0 && reuse == 0, "spots: never two CTs on one spot, never last round's spot again (6 spots, 3 CTs)");
    Check(good >= total / 4 && good < total, "spots: a smart bot takes the good spots often, not always");
    std::vector<V3> none;
    std::vector<float> rr(6, 0.5f);
    Check(PickSpot(c, none, none, 1.f, rr) <= 1 && PickSpot(c, {c[0].p, c[1].p}, none, 1.f, rr) >= 2, "spots: best first, never a taken one");
    Check(RectDist({5, 5, 0}, {0, 0, 10, 10, 0}) == 0.f && fabsf(RectDist({13, 14, 0}, {0, 0, 10, 10, 0}) - 5.f) < 1e-4f &&
              RectDist({5, 5, 400}, {0, 0, 10, 10, 0}) > 1e8f,
          "rect: inside 0, outside the corner distance, another floor far");
    V3 site{0, 0, 0}, entry{500, 0, 0}, ct{-900, 0, 0};
    Check(SiteSpotOk({100, 100, 0}, site, 600, entry, ct, false) && !SiteSpotOk({700, 0, 0}, site, 600, entry, ct, false) &&
              !SiteSpotOk({450, 0, 0}, site, 600, entry, ct, false) && !SiteSpotOk({-450, 0, 0}, site, 600, entry, ct, false) &&
              !SiteSpotOk({100, 100, 0}, site, 600, entry, ct, true),
          "setup: a site spot is on the site, not in the doorway, not by the CT spawn, not in mid");
    Check(MidSpotOk(200, 100, 800, 1000, 1500, 600) && !MidSpotOk(110, 100, 800, 1000, 1500, 600) && !MidSpotOk(200, 100, 800, 500, 1500, 600),
          "setup: a mid spot is CT ground (2 s ahead of the Ts), not in the CT spawn");
    Check(LurkSpotOk(300, 100, 1200, 600, false, 1200, 900) && !LurkSpotOk(300, 100, 1200, 600, true, 1200, 900) &&
              !LurkSpotOk(300, 100, 700, 600, false, 1200, 900) && !LurkSpotOk(300, 100, 1200, 600, false, 1200, 2000),
          "setup: a lurker spot is CT ground off the sites, out of mid, watching an approach");
}

static void RotateTests()
{
    using namespace fl;
    Check(RotateDelay(0.f, 1.f) <= 1.4f + 1e-4f && RotateDelay(1.f, 0.f) >= 0.2f - 1e-4f && RotateDelay(0.38f, 0.5f) < 1.1f,
          "rotate: leaves within 1.4 s of the call at any rank (Gold ~0.9 s; step 9b up to 4 s)");
    std::vector<V3> way = {{0, 0, 0}, {1000, 0, 0}, {1000, 1000, 0}};
    Check(NeedSneaky(way, {{500, 600, 0}}) && !NeedSneaky(way, {{-500, 1500, 0}}) && !NeedSneaky(way, {}),
          "rotate: sneaky when a known enemy is near the way ahead (segment distance), not when he is far");
    Check(!NeedSneaky(way, {{500, 300, 900}}), "rotate: an enemy on another floor does not count");
    Check(fabsf(SegDist2({5, 5, 0}, {0, 0, 0}, {10, 0, 0}) - 5.f) < 1e-4f && fabsf(SegDist2({-3, 4, 0}, {0, 0, 0}, {10, 0, 0}) - 5.f) < 1e-4f,
          "rotate: segment distance inside and past the end");
}

// where a ThrowArc crosses height z on the way down (x only: throws along +x), -1 if never
static float LandX(const fl::V3 &eye, float pitch, float z)
{
    std::vector<fl::V3> a = fl::ThrowArc(eye, pitch, 0.f, 0.002f, 8.f);
    for (size_t i = 1; i < a.size(); i++)
        if (a[i].z < z && a[i - 1].z >= z && a[i].z < a[i - 1].z) return a[i].x;
    return -1;
}
static void UtilTests()
{
    using namespace fl;
    int none[UK_N] = {0, 0, 0, 0, 0};
    auto str = [](const std::vector<int> &v) {
        std::string o;
        for (int k : v) o += std::string(o.empty() ? "" : ",") + UtilName(k);
        return o;
    };
    Check(str(UtilBuyList(1000, 2, 1.f, none)) == "smoke,flash,fire", "util buy: T with $1000 left, top rank: smoke, flash, molotov");
    Check(str(UtilBuyList(5000, 3, 1.f, none)) == "smoke,flash,fire,flash", "util buy: rich CT: smoke, flash, incendiary, flash (4 in all)");
    Check(str(UtilBuyList(5000, 2, 0.f, none)) == "smoke" && str(UtilBuyList(5000, 2, 0.35f, none)) == "smoke,flash",
          "util buy: a Silver buys one, a Gold Nova two");
    int sf[UK_N] = {1, 1, 0, 0, 0};
    Check(str(UtilBuyList(5000, 2, 0.5f, sf)) == "fire", "util buy: already has smoke + flash -> the molotov next (not a 2nd flash)");
    Check(str(UtilBuyList(250, 2, 1.f, none)) == "flash" && UtilBuyList(150, 3, 1.f, none).empty(), "util buy: only what the money allows");
    int full[UK_N] = {1, 2, 1, 0, 0};
    Check(UtilBuyList(9000, 2, 1.f, full).empty(), "util buy: four carried - nothing more");
    int f1[UK_N] = {0, 1, 0, 0, 0}, sfh[UK_N] = {1, 1, 1, 0, 0};
    Check(str(UtilBuyList(5000, 2, 1.f, f1)) == "smoke,fire" && UtilBuyList(5000, 2, 1.f, sfh).empty(),
          "util buy: a carried flash may be a pair - no 2nd flash on top, and it counts as two for the 4-grenade limit");
    Check(UtilPrice(UK_FIRE, 2) == 400 && UtilPrice(UK_FIRE, 3) == 600 && !strcmp(UtilBuyName(UK_FIRE, 3), "incgrenade") &&
              !strcmp(UtilBuyName(UK_FIRE, 2), "molotov"),
          "util buy: T molotov $400, CT incendiary $600");
    Check(UtilKindOf("weapon_smokegrenade") == UK_SMOKE && UtilKindOf("incgrenade") == UK_FIRE && UtilKindOf("13CSmokeGrenade") == UK_SMOKE &&
              UtilKindOf("18CIncendiaryGrenade") == UK_FIRE && UtilKindOf("10CFlashbang") == UK_FLASH && UtilKindOf("weapon_ak47") < 0,
          "util: kinds from item names and class names");
    // the throw solver against the flight model: flat ground 64 u under the eye, 800 u away
    V3 eye{0, 0, 64};
    float pt[2];
    int n = ThrowPitches(eye, {800, 0, 0}, pt);
    float x0 = n > 0 ? LandX(eye, pt[0], 0.f) : -1, x1 = n > 1 ? LandX(eye, pt[1], 0.f) : -1;
    printf("     throw 800 u: %d answers, pitch %.1f lands %.0f, pitch %.1f lands %.0f\n", n, n > 0 ? pt[0] : 0.f, x0, n > 1 ? pt[1] : 0.f, x1);
    Check(n == 2 && fabsf(x0 - 800.f) < 25.f && fabsf(x1 - 800.f) < 25.f && pt[0] > pt[1], "util aim: a flat throw and a lob, both land 800 u out");
    n = ThrowPitches(eye, {300, 0, 150}, pt);
    Check(n >= 1 && fabsf(LandX(eye, pt[0], 150.f) - 300.f) < 25.f, "util aim: onto a ledge 150 u up, 300 u out");
    Check(ThrowPitches(eye, {3000, 0, 0}, pt) == 0, "util aim: out of reach (3000 u) - no answer");
    float miss = 0, ap = AirPitch(eye, {700, 0, 120}, 1.5f, &miss);
    std::vector<V3> fa = ThrowArc(eye, ap, 0.f, 1.5f, 1.5f);
    printf("     flash pop 700 u: pitch %.0f, %.0f u off at 1.5 s\n", ap, miss);
    Check(miss < UtilTol(UK_FLASH) && fabsf(Dist(fa.back(), {700, 0, 120}) - miss) < 1.f, "util aim: a flash pops within its tolerance (320 u) at 1.5 s");
    // T execute: smokes first (1 sloppy / 2 smart), then a fire, different bots, spaced out
    std::vector<std::vector<int>> have = {{1, 0, 0, 0, 0}, {1, 0, 0, 1, 0}, {0, 0, 0, 1, 0}, {0, 1, 0, 0, 0}};
    std::vector<UtilJob> j = ExecuteJobs(have, 2, 1, 0.6f);
    Check(j.size() == 3 && j[0].bot == 0 && j[0].kind == UK_SMOKE && j[1].bot == 1 && j[1].slot == 1 && j[2].bot == 2 && j[2].kind == UK_FIRE &&
              j[1].delay > j[0].delay && j[2].delay > j[1].delay,
          "util execute: 2 smokes by 2 bots, then a molotov by a third, spaced");
    j = ExecuteJobs(have, 2, 1, 0.1f);
    Check(j.size() == 2 && j[0].kind == UK_SMOKE && j[1].bot == 1 && j[1].kind == UK_FIRE, "util execute: a sloppy team, one smoke + a molotov");
    std::vector<std::vector<int>> fl2 = {{0, 1, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 2, 0, 0, 0}};
    std::vector<int> f = ExecuteFlashes(fl2, {1, 0, 0, 0}, 0.5f);
    Check(f.size() == 2 && f[0] == 3 && f[1] == 0, "util execute: 2 flashes at the go-in, one who threw nothing yet first");
    int so[UK_N] = {1, 0, 1, 1, 0}, on[UK_N] = {1, 0, 0, 0, 0};
    Check(DefendKind(so, true) == UK_SMOKE && DefendKind(so, false) == UK_HE && DefendKind(on, false) < 0,
          "util defend: outside -> smoke the way in; inside -> no smoke on its own site");
    Check(RetakeKind(so) == UK_SMOKE && RetakeKind(none) < 0, "util retake: flash, smoke, fire, HE");
    Check(UtilGap(UK_FLASH) < UtilGap(UK_SMOKE), "util: throws spaced out (flashes closer)");
}

static void MoveTests()
{
    using namespace fl;
    int m0 = 0, m35 = 0, m1 = 0;
    for (int i = 0; i < 1000; i++)
    {
        float r = TRnd();
        m0 += Mover(0.f, r);
        m35 += Mover(0.35f, r);
        m1 += Mover(1.f, r);
    }
    printf("     movers: footwork 0 %d/1000, 0.35 (Gold Nova) %d/1000, 1 %d/1000\n", m0, m35, m1);
    Check(m0 == 0 && m35 > 350 && m35 < 550 && m1 > 900, "move: a Silver never cycles (kid floor), ~45% of Gold fights, ~95% at the top");
    // a 3 s fight: the bot fires every 0.1 s when it can (STOP), moves in between
    Cyc c;
    CycParams p = CycFor(0.35f, 1200.f);
    int stopN = 0, moveN = 0, flips = 0, last = 0;
    for (int f = 0; f < 300; f++)
    {
        float t = f * 0.01f;
        bool shot = c.phase == 1 && f % 10 == 0;
        CycStep(c, true, t, true, shot, p);
        stopN += c.phase == 1;
        moveN += c.phase == 2;
        flips += last && c.phase != last;
        last = c.phase;
    }
    printf("     a 3 s fight at 1200 u (dial 0.35): stopped %d%%, moving %d%%, %d phase changes, %d cycles\n", stopN / 3, moveN / 3, flips, c.cycles);
    Check(c.cycles >= 3 && stopN > 90 && moveN > 90, "move: move, stop, shoot, move, stop, shoot - both phases, 3+ cycles in 3 s");
    CycStep(c, false, 3.1f, true, false, p);
    Check(c.phase == 0, "move: the cycle ends with the fight");
    Cyc d;
    CycStep(d, true, 0.f, false, false, p);
    Check(d.phase == 1, "move: a fight starts with a stop (counter-strafe into the first burst)");
    CycStep(d, true, p.burst + 0.01f, false, false, p);
    CycStep(d, true, p.burst + 0.1f, false, true, p);
    Check(d.phase == 2, "move: a shot right after it starts moving does not stop it (0.15 s)");
    CycStep(d, true, p.burst + 0.01f + p.maxMove + 0.01f, false, false, p);
    Check(d.phase == 1, "move: never moves longer than 0.7 s without stopping to shoot");
    Check(CycFor(1.f, 1500.f).burst < CycFor(0.f, 1500.f).burst && CycFor(0.5f, 500.f).burst > CycFor(0.5f, 1500.f).burst,
          "move: a higher rank shoots shorter bursts; longer up close");
    Check(MoveSkill(0.f) > 0.5f && MoveSkill(1.f) <= 0.7f, "move: the side-step band is above the stock 0.5 line");
    int r0 = 0, r1 = 0;
    for (int i = 0; i < 1000; i++)
    {
        float r = TRnd();
        r0 += RepoAfter(0.f, true, r);
        r1 += RepoAfter(1.f, true, r);
    }
    Check(r0 > 150 && r0 < 250 && r1 > 750 && r1 < 850, "move: after a fight 20% .. 80% take another spot");
    Check(InCone({0, 0, 0}, 0.f, {100, 20, 0}, 30.f) && !InCone({0, 0, 0}, 90.f, {100, 0, 0}, 30.f), "move: in / out of an enemy's view");
    std::vector<V3> threats = {{1000, 0, 0}, {1000, 200, 0}}, cands = {{300, 0, 0}, {-400, 0, 0}, {-200, 500, 0}, {-600, -100, 0}};
    std::vector<char> hid = {1, 0, 1, 1}, cov = {0, 0, 0, 1};
    int k = PickFallback({0, 0, 0}, threats, cands, hid, cov, 250.f, 900.f);
    Check(k == 3, "move: falls back away from the two who see it, to a hidden covered spot (not towards them, not a seen one)");
    Check(PickFallback({0, 0, 0}, {}, cands, hid, cov, 250.f, 900.f) < 0, "move: no threats - no fallback");
}

static void CrouchTests()
{
    using namespace fl;
    // 376 candidate bytes (15 ducked, 45 standing samples): byte 117 is the flag, byte 40 a look-alike, byte 200 went to 7 once
    std::vector<CrouchCand> c(376);
    for (auto &x : c) x = {0, 15, 45, 45, false};   // a byte that is always 0: matches the standing samples only
    c[117] = {15, 15, 44, 45, false};
    c[40] = {13, 15, 45, 45, false};
    c[200] = {15, 15, 45, 45, true};
    Check(CrouchPick(c, 15, 45, 3) == 117, "crouch: the one byte that follows FL_DUCKING 97%+, clear of the next, is the flag");
    Check(CrouchPick(c, 8, 45, 3) < 0 && CrouchPick(c, 15, 45, 1) < 0, "crouch: not before 12 ducked samples on 2+ bots");
    c[40] = {15, 15, 45, 45, false};
    Check(CrouchPick(c, 15, 45, 3) < 0, "crouch: two look-alikes (97% and 100%) - none (never guess)");
    c[40] = {13, 15, 45, 45, false};
    c[117].totalD = 5;
    Check(CrouchPick(c, 15, 45, 3) < 0, "crouch: a byte seen steady in too few samples is not taken");
    std::vector<CrouchCand> r(376);
    for (auto &x : r) x = {0, 12, 1000, 1000, false};
    r[117] = {12, 12, 995, 1000, false};
    Check(CrouchPick(r, 12, 1000, 2) == 117, "crouch: rare ducking (12 of 1012) - an always-0 byte does not crowd out the flag");
    int s0 = 0, s35 = 0, s1 = 0, h0 = 0, h1 = 0;
    for (int i = 0; i < 1000; i++)
    {
        float r = TRnd();
        s0 += CrouchSpray(0.f, 800.f, r);
        s35 += CrouchSpray(0.35f, 800.f, r);
        s1 += CrouchSpray(1.f, 800.f, r);
        h0 += CrouchHold(0.f, r);
        h1 += CrouchHold(1.f, r);
    }
    printf("     crouch-spray per 1000 fights: dial 0 %d, 0.35 %d, 1 %d; crouched holds: 0 %d, 1 %d\n", s0, s35, s1, h0, h1);
    Check(s0 == 0 && s35 > 230 && s35 < 360 && s1 > 450 && s1 < 550, "crouch: crouch-spray 0 at elo 300 (kid floor), ~30% Gold, ~50% top");
    Check(!CrouchSpray(1.f, 200.f, 0.f) && !CrouchSpray(1.f, 2000.f, 0.f), "crouch: not point blank (step 11's panic) nor at 2000 u");
    Check(h0 == 0 && h1 > 350 && h1 < 450, "crouch: crouched holds 0 at the bottom, 40% at the top");
}

// ---- JOB 5c Q: the CT setup, 2 - 2 - 1 ----
static void CtSetup5cTests()
{
    using namespace fl;
    auto str = [](const std::vector<int> &r) {
        std::string o;
        for (int x : r) o += x == CR_MID ? "M" : x == CR_LURK ? "L" : std::string(1, (char)('A' + x));
        return o;
    };
    Check(str(CtRoles5c(5, 2, true, true, -1, false, 0)) == "ABMAB" && str(CtRoles5c(5, 2, true, true, -1, false, 1)) == "BAMBA",
          "5c setup: 5 CTs = two on A, two on B, one on mid");
    Check(str(CtRoles5c(5, 2, true, true, -1, true, 0)) == "ABLAB", "5c setup: the lingerer takes the lurk spot when rolled");
    Check(str(CtRoles5c(5, 2, false, true, -1, false, 0)) == "ABLAB" && str(CtRoles5c(5, 2, false, false, -1, false, 0)) == "ABABA",
          "5c setup: no mid spot = the lurker lingers; neither = the fifth to a site");
    Check(str(CtRoles5c(4, 2, true, true, -1, false, 1)) == "BAMB" && str(CtRoles5c(3, 2, true, true, -1, false, 0)) == "ABM" &&
              str(CtRoles5c(2, 2, true, true, -1, false, 0)) == "AB" && str(CtRoles5c(1, 2, true, true, -1, false, 1)) == "B",
          "5c setup: 4 CTs = both anchors, mid, + one on the extra site; 3 = anchors + mid; 1-2 = anchors");
    Check(str(CtRoles5c(5, 2, true, true, 0, false, 1)) == "BAMAA", "5c setup: a stack (smart teams, sometimes) = 3 on it, the other site keeps its anchor");
    bool ok = true;
    for (int n = 1; n <= 8; n++)
        for (int st = -1; st < 2; st++)
            for (int ex = 0; ex < 2; ex++)
            {
                std::vector<int> r = CtRoles5c(n, 2, true, true, st, (n + ex) % 2 == 0, ex);
                int m = 0, l = 0, a = 0, b = 0;
                for (int x : r) { m += x == CR_MID; l += x == CR_LURK; a += x == 0; b += x == 1; }
                ok = ok && m + l <= 1 && (int)r.size() == n && (n < 2 || (a >= 1 && b >= 1)) && (st >= 0 || n < 5 || (a >= 2 && b >= 2));
            }
    Check(ok, "5c setup: one lingerer at most, both sites anchored from 2 CTs, 2 + 2 from 5 CTs when not stacked (1-8 CTs)");
    Check(!CtAtSpot(900.f, 120.f) && CtAtSpot(200.f, 120.f), "5c fall back: not before it is at its spot (on its way it keeps going)");
    Check(CtOwnGround(true, 500.f, 450.f, 2000.f) && !CtOwnGround(true, 700.f, 450.f, 50.f) && CtOwnGround(false, 3000.f, 450.f, 550.f) &&
              !CtOwnGround(false, 3000.f, 450.f, 700.f),
          "5c fall back: a site holder stays on its site, the mid / lurker within 600 u of its spot");
    std::vector<V3> sc = {{1188, 2450, 96}, {-1550, 2688, 6}};
    std::vector<float> sr = {450, 450};
    Check(CtArea({1300, 2400, 40}, sc, sr, 2000, 3000, 1000, false) == 0 && CtArea({-1400, 2700, 60}, sc, sr, 2000, 3000, 2000, false) == 1 &&
              CtArea({-100, 2150, -128}, sc, sr, 100, 3000, 380, false) == CA_MID && CtArea({400, -200, 0}, sc, sr, 1500, 3000, 2400, true) == CA_TSIDE &&
              CtArea({275, 2175, -125}, sc, sr, 900, 3000, 0, false) == CA_SPAWN && CtArea({-1000, 2100, 0}, sc, sr, 900, 100, 1300, false) == CA_LURK,
          "5c proof: where a CT stands (on A, on B, mid, T side, CT spawn, its lurk spot)");
}
// ---- JOB 5c R: a site holder that sees a push fights it and calls it ----
static void React5cTests()
{
    using namespace fl;
    // de_dust2 A LongDoors: waypoints from the T spawn, stack, choke, entry (the map file's apwp / apstk / apchk / apent)
    std::vector<V3> lng = {{-238, -700, 55}, {262, -438, 0}, {488, -12, 1}, {700, 450, 1}, {650, 925, 1}, {1162, 1088, -1}, {1275, 1525, 0}, {1338, 2050, -6}};
    V3 a{1188, 2450, 96};
    Check(OnCorridor({500, 20, 0}, lng, a, 2800.f, 450.f) && OnCorridor({1200, 1300, 0}, lng, a, 2800.f, 450.f),
          "5c react: at long doors and up long = on A's long way in");
    Check(!OnCorridor({-600, -800, 60}, lng, a, 2800.f, 450.f) && !OnCorridor({-400, 1600, -120}, lng, a, 2800.f, 450.f),
          "5c react: the T spawn (every approach's start) and mid doors are not on A long");
    Check(PushSeen(1, 1) && PushSeen(3, 0) && !PushSeen(0, 4) && !PushSeen(1, 0), "5c react: a push = 2+ on a way in, one of them seen");
    Check(HoldDrop(false, false, false) && !HoldDrop(true, false, false) && HoldDrop(true, false, true) && !HoldDrop(true, true, true),
          "5c react: the hold drops for a pushing team that shoots at others (5b: only for a quiet one); never while shot at itself");
    Check(CallName("LongDoors") == "long" && CallName("Catwalk") == "cat" && CallName("UpperTunnel") == "tunnels" && CallName("MidDoors") == "mid" &&
              CallName("TMain") == "main" && CallName("PalaceAlley") == "palace alley" && CallName("route1") == "" && CallName("Banana") == "banana",
          "5c react: callouts from the map's way-in names");
}
// ---- JOB 5c S: death info ----
static void Death5cTests()
{
    using namespace fl;
    int silver = 0, top = 0;
    float dS = 0, dT = 0;
    for (int i = 0; i < 1000; i++)
    {
        float r1 = TRnd(), r2 = TRnd();
        DeathReact a = DeathFor(0.f, r1, r2), b = DeathFor(1.f, r1, r2);
        silver += a.takes;
        top += b.takes;
        dS += a.delay;
        dT += b.delay;
    }
    Check(silver > 480 && silver < 620 && top == 1000, "5c death info: a Silver takes in ~55% of deaths, a Global Elite every one");
    Check(dS / 1000 > 1.4f && dS / 1000 < 1.8f && dT / 1000 > 0.3f && dT / 1000 < 0.7f, "5c death info: Silver 1.4-1.8 s late, the top 0.3-0.7 s");
    Check(DeathCallLevel(2, true) == 2 && DeathCallLevel(1, true) == 1 && DeathCallLevel(3, false) == 0, "5c death info: 2+ there = the site is hit, 1 = a helper, off a way in = angles only");
    Check(DeathText("Ian", "long", 2) == "Ian died long, 2 there" && DeathText("Dad", "B site", 1) == "Dad died B site", "5c death info: the chat line");
}
// ---- JOB 5c U: face the way in ----
static void Face5cTests()
{
    using namespace fl;
    Check(FacePick({0, 1, 1}, {100, 400, 250}, 0.f) == 2 && FacePick({0, 1, 1}, {100, 400, 250}, 0.99f) == 2, "5c face: the visible way in the Ts reach first");
    Check(FacePick({1, 1, 0}, {200, 220, 50}, 0.f) == 0 && FacePick({1, 1, 0}, {200, 220, 50}, 0.9f) == 1, "5c face: two within 3 s of each other = either (the angle varies)");
    Check(FacePick({0, 0}, {100, 200}, 0.5f) == -1, "5c face: sees no way in = the stock look");
}

int main()
{
    Scenario("Wingman (2v2), 5 players, first -> T", 2, 5, 2, "T T CT CT spectator");
    Scenario("Wingman (2v2), 5 players, first -> CT", 2, 5, 3, "CT CT T T spectator");
    Scenario("Wingman (2v2), 3 players, first -> CT", 2, 3, 3, "CT CT T");
    Scenario("Casual (5v5), 3 players", 5, 3, 2, "T T T");
    Scenario("Casual (5v5), 11 players", 5, 11, 3, "CT CT CT CT CT T T T T T spectator");
    // someone picked the other team by hand: A on T, B on CT -> tie goes to T
    int p = FamilyPick(1, 1, 2);
    printf("%s %-44s -> %s\n", p == 2 ? "PASS" : "FAIL", "A on T, B on CT (by hand), C presses Auto", Name(p));
    g_fail += p != 2;
    p = FamilyPick(0, 2, 5);
    printf("%s %-44s -> %s\n", p == 3 ? "PASS" : "FAIL", "two humans on CT, Auto in casual", Name(p));
    g_fail += p != 3;
    DirectorTests();
    HearingTests();   // STEP 12B
    FireTests();      // STEP 12G
    DefenderTests();  // STEP 12C
    PlanTests();      // STEP 12D/H/J
    ChatTests();      // STEP 12D
    PeekTests();      // STEP 12E
    GoldTests();      // STEP 12K
    RestTests();      // STEP 12F
    PushTests();      // JOB 5b L
    SetupTests();     // JOB 5b M
    RotateTests();    // JOB 5b N
    UtilTests();      // JOB 5b O
    MoveTests();      // JOB 5b P
    CrouchTests();    // JOB 5b Q (his follow-up)
    CtSetup5cTests(); // JOB 5c Q
    React5cTests();   // JOB 5c R
    Death5cTests();   // JOB 5c S
    Face5cTests();    // JOB 5c U
    printf("%s\n", g_fail ? "SOME FAILED" : "ALL PASS");
    return g_fail != 0;
}

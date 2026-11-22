# Lords2 HD battle rules

Nov 22, 2026 · @Skaldic

Every battle rule the HD patch adds on the v1.6.0 line (pushed up to 8586885), with its numbers, so we can agree what each unit should do before changing more. Each rule names its setting and whose units it moves.

## How to read this

- **Setting:** the ini key (F10, Battles). Five settings change battles: JitterFix, GroupMoveFix, EngageFix, MopUpFix (all on by default) and BattleAI (0 Normal by default, 1 Hard, 2 Legendary).
- **Both sides:** the rule moves your units and the computer's alike. **Computer only:** BattleAI rules; Normal is the original game, unchanged.
- **Cell:** one square of the 80 × 80 battle map. **Step:** one logic tick, about 82 a second. A group **thinks** every 200 steps (about 2.4 s).
- **Walking distance:** steps a unit needs round obstacles. **Straight distance:** cells in a straight line, ignoring them. Many problems below come from the gap between the two.
- Each rule cell ends with **OK?** where a decision from you is still open.

## Terrain

Grass is the only ground every rule treats as open. Anything a unit can't step onto is an obstacle. Standing beside one makes a whole formation hold its line, and walking round one is what makes the walking distance long.

| Terrain | Can units walk on it? | Next to it, a formation holds its line? | Notes |
| --- | --- | --- | --- |
| Grass, open ground | Yes | No | Only the map edge doesn't count as an obstacle. |
| Water (river, lake) | No | Yes | The walk round can be 100+ cells when the straight line is 10 (last battle). |
| Stakes, stones, hedge, forest | No | Yes | Units behind them are no threat to archers: archers keep shooting. |
| Hill step of 2+ levels (cliff) | No | Yes | One level up or down is walkable. |
| Castle wall (3 up), gate (4 up) | Only by the stairs or gate | Yes | Units on a wall never engage or step back; archers there stay put. |
| Unfilled moat | Only to fill it | Yes | Added today: your pikemen by the drawbridge stepped onto it. |
| Burning oil, spreading fire | Yes, and it burns | No | Computer soldiers step out (Hard and Legendary, sieges only). |

**Where each kind of distance is used:**

| Rule | Distance used | Effect across water or stakes |
| --- | --- | --- |
| Idle units engage (EngageFix) | Both: straight 3 cells, and a walk of no more than 3 | Nobody steps out to an enemy across water. |
| Archers step back (EngageFix, Hard) | A walk of no more than straight + 2 | Archers don't step back from soldiers behind water. |
| Chase target pick (MopUpFix) | Walking, for 4 units per step; the rest get the game's own pick by straight distance | At a charge most units get the game's pick (last battle: 24 units on a 110-cell detour). |
| Computer advance and charge (Hard, Legendary) | Straight | It walks up to water and charges "9 cells away" though the walk is 110. |
| Computer rescue of its archers (Hard) | Straight to choose the group; attackers must reach within straight + 2 | — |

## What the original game does

Every rule below changes one of these original behaviours; with all settings off and BattleAI on Normal, this is the whole game.

| Behaviour | Original game |
| --- | --- |
| Idle soldier | Fights only an enemy that walks into it. |
| Chase target | Nearest enemy by straight distance. Archers and crossbowmen count at half distance, and each unit already on a target makes it count 2 cells further. |
| After a fight | A unit, archers included, chases any enemy within 5 cells. |
| Charge (Mop-Up) | Every unit of the group chases a target of its own until the battle ends. |
| Groups | Every 500 steps (about 6 s) any group of 4+ units is formed up again round its point, order or not. A box selection forms up after 20 steps. |
| Computer attack or defence | Weighed every 100 steps with a random −10 to +21, so it flipped up to 28 times in 2 minutes. |
| Computer melee | Each group walks to a point and charges on its own timer, one at a time. In defence the whole side charges at the first hits. |
| Archers under attack | They fight back in melee; nothing makes them step away. |

## Movement (JitterFix, GroupMoveFix)

These stop units stepping back and forth and keep groups together; both sides follow them. None of them picks who to fight.

| Rule | Setting | Whose | What it does |
| --- | --- | --- | --- |
| Wait for a friend | JitterFix | Both | A unit whose next cell holds a walking friend waits for it instead of turning back. |
| No swapping back | JitterFix | Both | A unit may swap places once per 64 steps, and the same two units not again within 5 s. Within 3 cells of its place, behind a groupmate, it just stops. |
| Stop beside a taken place | JitterFix | Both | A unit whose place is taken stops next to it instead of circling it. |
| Fight the live enemy | JitterFix | Both | A unit next to a dying enemy fights the live one beside it instead of sidestepping. |
| Group routes | GroupMoveFix | Both | A group sent round a forest, river or other obstacle: the first unit plans the route, the others follow side by side, closing up only where the gap is narrow. |
| Keep walking | GroupMoveFix | Both | When the group is formed up again and a unit's new place is within 4 cells of the old, it keeps its route. |
| Form up only when you ask | GroupMoveFix | Your units | Your groups form up after your order or your box selection, never every 6 s. Today's fix b3d1fff brought back the form-up on selection. The computer's groups form up as in the original. |

## Idle units fight (EngageFix)

An idle soldier now steps out to a close enemy, but never far and never into bad odds; a formation by an obstacle holds instead. Both sides follow all of it.

| Rule | What it does | Numbers |
| --- | --- | --- |
| Reach | Checked every 8 steps: an enemy within the reach in a straight line and by walking. | 3 cells; knights 4; enemy archers and crossbowmen 5 |
| Leash | It remembers where it stood and walks back after the fight. | Never more than 4 cells from that spot, nor after an enemy more than 5 from it |
| Let go | An enemy that keeps getting away is dropped, then ignored. | Gets away for 1 s, ignored for 3 s |
| No baiting | At most 2 units go for the same enemy, unless it is already fighting or right beside them. | 2 |
| Odds | Only when its soldiers near that enemy have at least as many men as the enemy has there and coming up. An enemy right beside it is always fought. | Ours within 3 cells of the enemy; theirs within 5, their archers counted half |
| Line holding | When any unit of the formation (groupmates no more than 2 cells apart) stands next to an obstacle, the whole formation fights only enemies right beside it, after a fight too. | Obstacles: see Terrain |
| Archers back up soldiers | With no target you gave, archers and crossbowmen shoot the nearest enemy fighting one of their side. | Within their range |
| Archers don't chase | After a fight, archers stand and shoot instead of running at the next enemy. | — |
| Archers step back | An idle archer steps away from a soldier marching or charging at it, unless that soldier is behind water, stakes or a hedge. | Threat within 2 cells; steps 3 cells, stays within 5 of its spot, once per 3 s; fights when cornered |
| Archers leave a fight | They step out once one of their soldiers takes over the enemy. | — |
| Who never engages | Siege engines, and units standing on a wall. | — |

**OK?** Line holding with water: in the last battle your pikemen by the river stood idle while friends fought an enemy 2 cells away. That is this rule working as written. Keep it, or let a holding unit step 1 cell to help a groupmate in a fight?

## Chasing (MopUpFix)

A charging unit picks the nearest enemy it can walk to and keeps chasing until the battle ends. Only knights and macemen go a long way round for a target; slower units take the enemy at hand. Both sides follow these rules, the computer's charges included.

| Rule | What it does | Numbers |
| --- | --- | --- |
| Next target | A unit whose target falls goes after the nearest enemy instead of standing idle. | — |
| Pick by walking | Nearest by walking distance; not one on a wall, across water or a level up. Archers still count at half distance, and each unit already on a target makes it count 2 cells further (both from the original). | Up to 4 walking searches per step; past that, the game's own pick by straight distance |
| Give up | A target it can't reach, or a close one it isn't getting to, is dropped and avoided. | Close = a walk of 6 or less, no closer for 2 s (2 or less: 1 s); avoided for 4 s |
| Far targets | Knights and macemen may go round for a target. Slower units drop a target that is too far round, checked while they chase, and take a closer one within their limit; with none, they keep the nearest (built 65761eb). | Knights and macemen: a walk up to 60. Others: up to twice the straight line plus 10 |
| Back to chasing | An idle unit of a charging group starts chasing again. | Every 16 steps |
| Your orders | A box selection keeps the charge; your move or attack order ends it, so the group obeys. | — |

**Decided and built (65761eb):** in the river battle all 24 of the computer's melee units went for your archers and crossbowmen behind the water, on walks of 105 to 124 cells, past your peasants. Now only knights and macemen may go round like that (up to a 60-cell walk); peasants, archers, crossbowmen, swordsmen and pikemen take an enemy within twice the straight line plus 10. Both sides, your Mop-Up units too.

## Computer tactics: Hard (BattleAI=1)

On Hard the computer decides once, keeps its archers behind its soldiers, and moves up and charges as one army. These rules move the computer's units only, and in field battles only, except the burning-oil rule. Each group thinks every 200 steps; a group with a unit in melee doesn't think.

| Rule | What it does | Numbers |
| --- | --- | --- |
| Steady mode | The first weighing decides attack or defence. It switches only when the armies really change, or attacks after a long wait when about even. | Switch at 20 past the threshold; wait 60 weighings (about 70 s) when no more than 10 below |
| Archers 1: step back | From your soldiers close by who can walk up to them, unless its own soldiers cover them. | Threat within 5 cells; step 4 cells |
| Archers 2: shoot | At the nearest of your groups in range, when at least half the archers reach it. | Each archer's own range |
| Archers 3: walk up | Out of range of a fight its soldiers are in: walk up until 2 cells inside range. | Crossbows 8 cells, bows 15 |
| Archers 4: stay behind | Otherwise stand behind its own soldiers. | 3 cells behind |
| Rescue | Your soldiers reach its archers: the nearest free melee group goes to help; two groups when more than 3 of yours are there. | Attackers within 3 cells; helpers within 20; they charge from 4 cells |
| Advance | Moves its melee up towards your nearest unit, never ahead of its rearmost group. A walking group keeps its order; one that has arrived moves only for a real step. | 6 cells a think; stops 6 cells from you; next step at least 4 |
| Charge in attack | When hit, when units are fighting, when every group is in place and its archers have had time to shoot, or after a long attack with the line close. | In place = within 8; archers 400 steps (about 5 s); long attack = 5000 steps (about 60 s) with the rearmost group within 20 |
| Charge in defence | When your units come close, its units are fighting, or it has lost men. | 3 of yours within 6 cells; 3 of its units fighting; 15% of its men lost |
| Together | Once its side charges, every melee group charges at its next step. A group already charging isn't charged again (ad28c87), so its chasers keep their paths. | — |
| Crossing: park | While attacking or charging, with its melee near a crossing you hold (your units on it or close to it, none of yours further onto its bank), its melee stops chasing and stands in a line on its bank, not in front of the mouth. Units fighting there fight on. Defending, nothing changes. | Looked at every 50 steps; its melee's centre within 20 cells; 6 cells back; groups 4 apart; your units within 4 cells of it count, either bank |
| Crossing: soften | Its archers walk up beside the lane on its bank to where the far end is in range, as far back as that allows, and shoot. | At least 3 cells off the lane; crossbows 8, bows 15 |
| Crossing: storm | Once every melee group has stopped at its place, every melee group charges together when your men there are down to half of its melee men (after its archers have shot for a while; at once without archers), or after a long wait when it's the stronger army. When the crossing comes free it storms at once. When yours come over well onto its bank, or it turns to defence, the assault ends and the usual rules take over. | Clocks start in place: 800 steps (about 10 s) of shooting; long wait 5000 steps (about 60 s) |
| Crossing: wait at a jam | A unit whose new chase route is long only because units are in the way (the walk on an empty field is within its limit) waits where it is and plans again shortly, instead of the long way round. | About 32 steps |
| Burning oil (sieges) | Its soldiers in burning oil step out to the nearest safe cell, and don't walk back in while it burns. Units marching through keep going. | Looks 5 cells; passing through = in the fire under 40 steps |

**Problems found in the last battle (all computer only):**

- Advance and charge measure the straight line, so it lined up on the near bank of the water and charged "9 cells away" into a 110-cell walk.
- Each think re-issued the game's charge to groups that already charged, wiping every chaser's path every 200 steps. Fixed (ad28c87).
- At your bridge it sent everyone at once; the bridge filled with its own men and the units behind walked 103 to 116 cells round the other bridge. Built: the crossing assault above (park, soften, storm, wait at a jam).
- The rescue order doesn't reach a charging group: its units keep chasing their own targets. Its pikemen and peasants were sent "to the archers" 3 times and none turned round.
- The archers' group doesn't think while one of its units is in melee. The other crossbowmen stood 1 to 2 cells from that fight, kept shooting, and were caught one by one.

## Computer tactics: Legendary (BattleAI=2)

Legendary is every Hard rule plus six more. Only the unit counters touch your units, and only in melee.

| Rule | What it does | Numbers |
| --- | --- | --- |
| Unit counters (both sides) | Damage between some unit pairs in melee is scaled. | Pikes on knights ×2; knights on pikes ×0.75; swords on pikes ×1.5; maces on knights ×1.5; knights and swords on peasants, archers and maces ×1.5 |
| Target picks | When it charges at a moment it chose, each melee group goes for one of your groups: archers first, then ones it counters, then weakened ones. Not after a charge set off by hits or contact. | At most 2 groups per target; only targets within 10 cells more than the nearest; charges within 4 cells |
| Closing in under fire | Shot at from afar, the line keeps advancing instead of charging at the first hit. | Charges within 12 cells, or after losing 10% of its men |
| Regrouping | A badly beaten group falls back behind its archers while its side is losing; it rejoins when you're worn down. | Down to 35% of its men; 4 cells behind; rejoins when you have under 40% of its side's men |
| Archers shoot archers | Its archers pick your archers first. | Only while none of your soldiers is within 8 cells |
| Late attack | A weaker computer attacks after the long wait instead of standing off. | Up to 30 below the threshold, after about 70 s |

**OK?** Target picks measure the straight line too. In the battle before last, its groups walked 49 to 57 cells to their picks, and the fast peasants arrived first. Should target picks use walking distance and skip targets behind water?

**Added at your request (e37906e): knights stomp the lightly armoured.** Knights deal ×1.5 to peasants, archers and macemen. Macemen keep their ×1.5 on knights, so they hit hard and fall fast. Pikemen stay the anti-knight unit. Crossbowmen are armoured, so they get no counter.

**Added at your request: swordsmen cut down the lightly armoured too** ("knights without the steed"). Swordsmen deal ×1.5 to peasants, archers and macemen, on top of their ×1.5 on pikemen. Crossbowmen and knights get no counter from them.

## Unit types

Only knights and the two ranged types differ in the rules above; the four foot soldier types differ only by the Legendary counters.

| Unit | Steps out to (idle) | Shooting range | Legendary counters | Notes |
| --- | --- | --- | --- | --- |
| Peasants | 3 cells (5 for your archers) | — | Take ×1.5 from knights and swordsmen | Grouped with pikemen by the computer's handler; faster than swordsmen and pikemen, so they arrived first when a charge started far away. |
| Pikemen | 3 cells (5 for your archers) | — | Deal ×2 to knights; take ×0.75 from knights | The anti-knight unit; toughest foot unit in the original game. |
| Swordsmen | 3 cells (5 for your archers) | — | Deal ×1.5 to pikemen, peasants, archers and maces | Knights without the horse: hit soft targets hard. |
| Macemen | 3 cells (5 for your archers) | — | Deal ×1.5 to knights; take ×1.5 from knights and swordsmen | Fast, good against armour, poorly armoured: hit hard and fall fast. |
| Knights | 4 cells (5 for your archers) | — | Take ×2 from pikes, ×1.5 from maces; deal ×0.75 to pikes, ×1.5 to peasants, archers and maces | — |
| Crossbowmen | never | 8 cells | none (armoured) | Strong shot (power 200). Count at half distance as a chase target. Step back, never chase after a fight. |
| Archers | never | 15 cells | Take ×1.5 from knights and swordsmen | Weak shot (power 50). Same half distance, step back and no chase as crossbowmen. |

## Problems seen in your battles

Six reports are fixed and pushed; the last battle left four open problems and three decisions for you.

| Reported | Problem | Cause | Status |
| --- | --- | --- | --- |
| Last battle (by the river) | Computer pikemen stood by the water doing nothing next to a fight | All 24 of its melee units chased your archers on 105 to 124-cell walks; the game's pathfinder kept failing and stepping back | Open: a fix needs a decision below |
| Last battle | Its army walked past your peasants attacking its archers | Same chase; the rescue order doesn't reach a charging group | Open |
| Last battle | Crossbowmen fighting in melee | Its crossbowmen stood beside a fight while one of them was in melee, so the group never thought to step back | Open |
| Last battle | Your pikemen idle next to friends fighting | Line holding by the river | Decision below |
| Selection | A box selection no longer pulled units together | Today's form-up change also blocked the selection's form-up | Fixed, b3d1fff |
| Legendary | Peasants charged first, then the rest | Advance stalled with groups counted as walking; the 60 s charge came from 50 cells | Fixed, 752a4fb |
| Legendary, bridge | Its archers idle during the fight | Fire order checked the range; nothing walked them up | Fixed, 23ec645 |
| Siege, gate | A pikeman left your line onto the drawbridge | The moat didn't count as an obstacle | Fixed, 9d74dcc |
| Siege | Soldiers stood in burning oil | No rule; then marching units were skipped | Fixed, 59c1b18 and 4eac2ae |
| Earlier | Archers by stakes walked round through the gap | Groups formed up every 6 s unasked | Fixed, afb63ce |

**Decisions for you:**

- [ ] Line holding by water: keep it, or let a holding unit step 1 cell to help a groupmate in a fight?
- [ ] Far chase targets, your idea: only fast units keep a far target. Knights (move delay 0) and macemen (1) may go round, up to a 60-cell walk. Peasants, archers and crossbowmen (2), swordsmen (3) and pikemen (4) drop a target whose walk is more than twice the straight line plus 10, and take the closest one. Decided: both sides, your Mop-Up units too. Built in 65761eb.
- [ ] Reading the battlefield, step 1 of 3 (built in 8586885, logged only): at the start the patch finds every narrow crossing (bridge, ford, gap in stakes or hedges, at most 3 cells across) that is the only way or whose closing makes the way round at least 15 steps longer. The log lists each one with its size, the way round and each side's walking distance, and every 200 steps says who holds it. On your river map it finds both bridges, each about 106-109 steps round. Step 2, built at your request: the crossing assault on Hard and Legendary (park, soften, storm; no flank round the other bridge). Step 3: its own route planner.
- [ ] Legendary target picks: measure by walking distance and skip groups behind water?
- [ ] Knights on light units: decided and built (e37906e). Knights ×1.5 on peasants, archers and macemen; maces keep ×1.5 on knights; crossbowmen none.

**Computer-only fixes ready to make once you agree (Hard and Legendary, Normal unchanged):**

1. Don't re-issue the charge to groups already charging, so chasers keep their paths. Done (ad28c87).
2. A rescue turns a charging group's units onto the units attacking its archers.
3. Archers step back even while one of their group is in melee.
4. Advance and charge stop treating units across water as close.

Still on the backlog for the pathfinding overhaul: unit jitter when blocked, Mop-Up idling, and long routes where the game's pathfinder keeps failing.

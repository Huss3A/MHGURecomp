# Small monster sync in multiplayer (research, 2026-10-20)

Goal: small monsters ("zako") synced between players like large monsters are - positions, actions, targets - not
only HP / deaths. Unchanged game behaviour so far; nothing below is applied yet. Addresses are runtime addresses
(image offset + 0x200000, what `tools/disasm.py`, `MHGU_FNTRACE` and thread dumps use) unless marked "image".
`tools/retool.py` (strings / xref / callers / dti / vtslot) finds all of this again.

## What the game does

Network layer: Capcom's MT Framework RPC (`cRemoteCall` messages, `cRemoteProcedure` handlers) in
`nFestaNetwork`, over Nintendo Pia. 120 message types; the monster ones:

| Large monsters | Small monster herds ("Mure" = herd) |
|---|---|
| `netEmAction` (0xcc bytes: full state at each new action) | `netMureEmStatus` (herd state number, sequence) |
| `netEmMasterSelect / Inquire / Change` | `netMureEmMasterSelect / Inquire / Change` |
| `netEmMasterOnlyStatus`, `netEmSpecificStatus`, `netEmUniqueData`, `netEmRevivalSet` | `netMureEmTarget`, `netMureEmCallStatus` |
| `netSystemEmSendStatus` (`sysSendData.em_set_id ... zako_set_idx, set_num`) | |

`netEmAction` fields (property names in rodata, image 0x1559491..): header `net_em.{prio, master_id, uid, type}`,
`head.{em_set_id, rev_count, em_id}`, damage packs, `pos.xyz`, `rot`, `hp_vital`, `status_bit`, `act_st`,
`act_no`, `net_act_rand`, `area_no`, `next_area`, `target_area`, waypoint / `way_goto_pos` / `area_move_*` fields,
`homing_pos[]`, `target_sel*[]`, ailment points (sleep, para, poison, KO, ...), `mMasterId`, `cmd_flag[]`.
Herd messages carry `mure_head.{mure_type, mure_id, mure_area_no, sequence_no}`, `mure_status`, `mure_st_no`,
`target_type/id`, `call_status`, `pos_data_idx` (an index into the zone's preset spawn points), `local_ck_flag`.
Every enemy message starts with the same `net_em` header, so "which player is master" exists for herds too.

So the design is the one the user guessed: a large monster has a *master* player whose game runs its AI and sends
each new action with the full state; the others replay it. Large monsters live on between zones (area / waypoint
fields: their off-screen travel is simulated). Small monsters are spawned per zone from `quest\zako\emSetList\`
when a zone loads; for them only the herd is synced (who is herd master, the herd's state, target and spawn-point
index), and every game runs each small monster's AI itself.

Classes (MT DTI tree): small and large monsters are the same family -
`uEmsNNN_00` / `uEmNNN_00` (0xcad0..) -> `uFestaEnemy` (0xcac0) -> `uEnemy` (0xca80) -> `uEnemyBase` (0xb740) ->
`uEnemyCharacter`. No virtual function differs systematically between all small and all large monsters
(compared all 89 vtables): the difference is data, set when a monster is spawned.

## Why small monsters are not synced (found)

- Send dispatcher, `uFestaEnemy` net send `0x2cafe8(this, kind, data)`: sends only if a session is up
  (`0x249624`) **and** the monster's network id `*(u16 *)(*(this + 0x1428) + 0x1b6) != 0xffff`; then calls the
  vtable slot for the kind (kind 1 = action: slot `+0x42c` = `0x2cb330`, which fills the 0xcc-byte netEmAction and
  sends it through `0x3eb810` -> `0xbfc3d8`).
- Receive: `netEmActionProcedure` (`0x3f4554`) -> enemy manager `0x265b14` -> lookup `0x25cf90(mgr, id)`, which
  compares the id with **exactly 10 monster pointers at manager +0x1c..+0x40** (the large-monster slots) -> found:
  per-monster receive `0x2cb120` (ignores its own messages, then dispatches by kind through slots +0x430...).
- The network id is reset to 0xffff by the enemy status initializer (around `0x2ace44`) and given a real value
  when a monster is spawned from the quest's large-monster set (e.g. `0x1280114` for multi-part monsters).
  Expected (not yet confirmed at runtime): small monsters keep 0xffff, so they never send, and they are not in the
  10 slots, so nothing addressed to them could be delivered.

Not a data flag in the romfs: the gate is code (the id check and the 10-slot lookup). A plain mod can't do it;
the port can (tools/gen.py PATCHES = instruction-level replacements at generation time, or host functions).

## What a fix needs (plan)

1. A network id for every small monster that is **the same on every player's game**: derived from the herd
   (`mure_id`, `mure_area_no`) and the member index / `pos_data_idx`, which the herd sync already makes agree.
   Kept in a range the large monsters never use.
2. The receive lookup (`0x25cf90`) extended to find small monsters by that id (a table the port keeps, filled at
   spawn, cleared at despawn) - then `netEmAction` & co. reach them through the existing per-monster code.
3. Only the herd master sends, the others replay: the large-monster path decides "master or follower" per
   monster (`net_em.master_id`, `mMasterId`); for small monsters the herd master (`netMureEmMaster*`) has to be
   used as that monster's master. **Open question: where the follower side stops running its own AI** for large
   monsters (the check that makes a non-master wait for `netEmAction`), and whether it already reads a per-monster
   master that we can set from the herd master.
4. Bandwidth: one netEmAction per new action per monster, only between players in the same zone. A zone has a
   handful of small monsters; well within what the PC link carries.

Fallback if (3) turns out to be tangled: a port-side sideband (our own message in the local play link) where the
herd master sends each small monster's position / rotation / action id a few times a second and the others snap
to it. Simpler to reason about, but it fights the followers' own AI unless that is switched off too.

## Testing (needs a 2-player quest with small monsters in a zone)

- `MHGU_FNTRACE=2cafe8-2cafec` logs every call of the send dispatcher with the monster (r0) and caller: shows the
  small monsters trying to send and being refused; `MHGU_FNTRACE=25cf90-25cf94` the receive lookups.
- Control file `peek <hex guest addr> [words]` logs guest memory: `peek <monster+0x1428>` then the pointed-to
  block's `+0x1b6` gives a monster's network id.
- Two instances on one PC (run/ hosts, run2/ joins; see docs/STATUS.md "Local Play over IP").

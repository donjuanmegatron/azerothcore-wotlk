-- Add UNIT_NPC_FLAG_MAILBOX (0x04000000 = 67108864) to the Sanctum Warden so CanOpenMailBox() passes.
-- Without this flag the server rejects the CMSG_GET_MAIL_LIST request and the mail UI never populates.
-- Note: 4096 = UNIT_NPC_FLAG_REPAIR (wrong flag, shows anvil cursor — do not use).
-- Previous npcflag was 1 (GOSSIP). New value: 1 | 67108864 = 67108865.
UPDATE creature_template SET npcflag = 67108865 WHERE entry = 700200;

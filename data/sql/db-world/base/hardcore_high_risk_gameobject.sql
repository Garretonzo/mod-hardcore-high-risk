-- mod-hardcore-high-risk: chests that hold a fallen hardcore character's belongings.
-- Loot is filled in by the module (Data1 lootId = 0). Data0 lockId 57 is the plain "Opening" lock used by world chests,
-- Data3 consumable = 1 despawns the chest once emptied, Data10 losOK = 1, Data15 groupLootRules = 0 (no rolls).
DELETE FROM `gameobject_template` WHERE `entry` IN (601000, 601001);
INSERT INTO `gameobject_template` (`entry`, `type`, `displayId`, `name`, `IconName`, `castBarCaption`, `unk1`, `size`, `Data0`, `Data1`, `Data2`, `Data3`, `Data4`, `Data5`, `Data6`, `Data7`, `Data8`, `Data9`, `Data10`, `Data11`, `Data12`, `Data13`, `Data14`, `Data15`, `Data16`, `Data17`, `Data18`, `Data19`, `Data20`, `Data21`, `Data22`, `Data23`, `AIName`, `ScriptName`, `VerifiedBuild`) VALUES
(601000, 3, 1, 'Gear of the Fallen', '', '', '', 1, 57, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '', '', 0),
(601001, 3, 259, 'Pack of the Fallen', '', '', '', 1, 57, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '', '', 0);

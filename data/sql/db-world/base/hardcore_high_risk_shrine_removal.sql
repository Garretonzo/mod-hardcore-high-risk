-- mod-hardcore-high-risk: hardcore is now realm-wide, so the Shrine of the Hardcore (and the former Shrine of
-- Challenge it replaced) is removed.
DELETE FROM `gameobject` WHERE `id` = 254605 AND `guid` BETWEEN 5530536 AND 5530544;
DELETE FROM `gameobject_template` WHERE `entry` = 254605;
DELETE FROM `npc_text` WHERE `ID` = 601000;

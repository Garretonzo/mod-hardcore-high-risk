-- mod-hardcore-high-risk: hardcore is decided by config now, so the per-character opt-in flag is gone.
ALTER TABLE `mod_hardcore_high_risk_character` DROP COLUMN `hardcore`;
DELETE FROM `mod_hardcore_high_risk_character` WHERE `dead` = 0 AND `pending_bot_death` = 0;

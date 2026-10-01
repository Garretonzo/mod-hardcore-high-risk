-- mod-hardcore-high-risk: per-character hardcore state. Characters without a row are not hardcore.
CREATE TABLE IF NOT EXISTS `mod_hardcore_high_risk_character` (
  `guid` INT UNSIGNED NOT NULL COMMENT 'characters.guid',
  `hardcore` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'Chose hardcore at the shrine',
  `dead` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'Fallen for good: can never be resurrected',
  `pending_bot_death` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'Random bot still owes its reset or retire',
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-hardcore-high-risk character state';

-- State formerly kept in character_settings (mod-challenge-modes and earlier versions of this module).
DELETE FROM `character_settings` WHERE `source` IN ('mod-challenge-modes', 'mod-hardcore-high-risk');

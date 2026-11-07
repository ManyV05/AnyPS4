# Tested compatibility

AnyPS4 was developed and tested only for the title below. Other titles are not supported.

| Game | ID | Windows |
|------|----|---------|
| The Witch and the Hundred Knight: Revival Edition | CUSA02799 | In game: title screen, prologue, house, level and town played in test sessions of a few minutes |

## Known limits

- The distance attenuation of effects (for example the totem hum) has not been confirmed by ear; the effect volume matrix is applied by default and `ORBIS_NGS2_SPATIAL=0` turns it off.
- Tests were sessions of a few minutes; long sessions have not been measured.
- Save data lives in `savedata/` beside the executable and the game files are never copied: `app0` is a junction to the folder you provide.

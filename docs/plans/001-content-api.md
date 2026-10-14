# Local content API

The game's gameplay backend is NESiCA AHAC, not a generic "content" hostname.
`sub_140228CF0` selects a profile and stores:

- game: `<profile>.ahac.nesica.net/ahac-api/tg4gs/gs`
- photo: `<profile>.photo.ahac.nesica.net/ahac-api/tg4gs/gs`

DDGACreg does not talk to those hosts. The DLL rewrites both stored hosts to
the local server. The server listens on `127.0.0.1:8750` only.

## Base path

`http://127.0.0.1:8750/ahac-api/tg4gs/gs`

Photo uploads use the same server under `/ahac-api/tg4gs/gs/photo`.

## Commands confirmed in IDA

ASCII command stubs (widened by `sub_140198080`):

- `login`
- `addcredit`
- `gamestart`
- `getmissionranking`
- `getstoremileageranking`
- `ranking` (response field, also a command string at `0x1423997F0` and `0x14239BD58`)
- `ranking_reward`

Ranking objects contain `start_at`, `end_at`, `update_at` (unix milliseconds)
and a `ranking` array. See `docs/process/2026-10-11-research.md`.

UE reflection names that still need a native request shape (the server should
accept them once IDA shows the body):

- card: select / register / reissue / unlock
- player: config, badge, wappen, information
- round: gameover, result, total result
- credit: insert credit (distinct from the `addcredit` stub already found)

## SQLite

File: `server/data/content.sqlite`

Tables: `cards`, `players`, `player_data`, `shop_lineup`, `owned_items`,
`equipped_badges`, `equipped_wappen`, `player_tims`, `leaderboards`, `credits`.
The DLL process does not open this file. It only sends HTTP to the server.

## DLL redirect

Hook `sub_140228CF0` (`0x140228CF0`) or the host wide-strings it writes, and
replace the game and photo hosts with `127.0.0.1:8750/ahac-api/tg4gs/gs`.
Do not patch the official hosts into a remote attacker. Localhost only.

## Routes confirmed 2026-10-11

`sub_14023F520` builds `http://` + the wide host at object `+312` + `/` + a
command. After the DLL rewrite that host is
`127.0.0.1:<port>/ahac-api/tg4gs/gs`, so the login URL is

`http://127.0.0.1:8750/ahac-api/tg4gs/gs/login`

The photo host at object `+328` is the same local path. Bodies are JSON
objects (numbers, bools, and strings). `ping` is the exception: it posts one
zero byte with `Content-type: application/x-www-form-urlencoded`.

| Command | Request fields | Response the server returns |
| --- | --- | --- |
| `ping` | empty / 1 byte | `{"result":1}` |
| `init` | (name stub only) | `game_version` string, `web_point_limit` number, `rank_border` object with `D/C/B/A/S/SS` |
| `login` | `player_id` number, `relogin` bool, `consume_ticket` number, `change_customize` bool | player object (`exist`, `player_id`, `player_name`, `title`, `tims`, credits, points, equipped items). `change_customize` is stored. |
| `addcredit` | `player_id`, `credit_num`, `service_credit_num`, `is_continue` | same player object with updated `credit_num` / `service_credit_num` |
| `gamestart` | `player_id`, `game_mode` (internal+1), `mission_mode`, `mission_type` (internal+1), `mission_id`, `difficulty` (internal+1), `timezone` (internal+1), `isContinue` bool, `badge` array of `{id, position}`, `mission_ticket_id` | player object with a new `playlog_id` |
| `getmissionranking` | `mission_id`, `difficulty`, `offset`, `count` | `start_at` / `end_at` / `update_at` strings (unix ms) and `ranking[]` |
| `getstoremileageranking` | `count` (the client sends 20) | same ranking object |
| `ranking` | ranking field name; also a command string | same ranking object |
| `ranking_reward` | response keys not fully recovered | ranking object plus `ranking_reward: []` |
| (empty path) | `{}` | player object: `exist`, `player_id`, `player_name`, `web_point`, `total_web_point`, `meister_point`, `latest_login_at`, `information`, `sound_config`, `tims`, `title`, `badge_list`, `badge`, `wappen_list`, `wappen`, `mission_ticket`, plus the existing credit fields |
| `shopinitialize` | `{}` | `badge` / `wappen` / `mission_ticket` objects keyed by lineup id. Nested fields: `id`, `price`, `status`, `buyable` |
| `iteminitialize` | `item_category` | `mission_ticket: []`, `wappen` catalog (`id`, `cell_type`, `max_num`, `name`, `description`), `wappen_set: []` |
| `purchasepoint` | `player_id`, `web_point` (delta) | `web_point`, `total_web_point` |
| `purchaseshopitem` | `player_id`, `lineup_id`, `num` | `meister_point`, `badge_list`, `wappen_list`, `mission_ticket` (each `{id, num}`) |
| `playerbadge` | `player_id`, `badge` `[{id, position}]`, `wappen_reset` | `badge` and `wappen` (`id`, `size_type`, `position_x`, `position_y`) |
| `playerwappen` | `player_id`, `wappen` `[{id, position_x, position_y, size_type}]` | `wappen` array |
| `config` | `player_id`, `sound_config` (`bgm_volume`, `driver_volume`, `conductor_volume`, `inside_volume`, `companion_volume`) | `sound_config` |

Ranking entry numbers: `rank` is stored as json+1, and `mode` / `title_type` /
`title_rarity` / `title_train_line` are stored as json-1. `name`, `title_name`,
`title_ruby`, and `store_name` are strings. `player_id`, `title_category`,
`title_year`, and `value` are numbers stored as-is.

Card select / register and player config have no ASCII command literal. The
server still accepts `selectcard`, `registercard`, `reissuecard`, `cardunlock`,
`playerconfig`, and `playerdata`. `playerbadge`, `playerwappen`, `config`,
`shopinitialize`, `iteminitialize`, `purchasepoint`, and `purchaseshopitem`
are recovered command names and are not those aliases. The card id is read from `card`, `card_code`,
`cardid`, `card_id`, `raw`, `access_code`, or `nesica_id` (the native call is
`NESYS_CARD_SELECT(raw, 4, ansi)`, so the JSON key itself was not recovered).
A known card returns `exist: true` plus `player_id`, `player_name`, `tims`,
and `title`. An unknown code returns `{"exist":false}`. See
`docs/process/2026-10-11-content.md`.

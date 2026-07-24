# Tibia Playerbots Project

Custom Tibia 8.60 development base combining:

- `server/`: Giorox Angelion real-map server on the TFS 1.5 downgrade.
- `client/`: current OTClient Redemption configured for standard protocol 8.60.

The repositories are imported as squashed Git subtrees so server and client
changes can be committed atomically while retaining an upstream update path.

## Prerequisites

- Docker Desktop with Docker Compose
- PowerShell 7+
- GitHub CLI authenticated with `gh auth login`

## Client bootstrap

The executable and Tibia DAT/SPR files are intentionally excluded from Git.
Download and verify the pinned files after cloning:

```powershell
pwsh -File scripts/bootstrap-client.ps1
```

Launch the client with `client/launch-angelion-redemption.cmd`.

## Server

```powershell
docker compose -f server/compose.yaml up --build --detach
docker compose -f server/compose.yaml logs --follow server
```

Local accounts:

| Account | Password | Character |
| ------- | -------- | --------- |
| `admin` | `admin` | `GOD Admin`, `Rook Tester` |

The services bind only to `127.0.0.1`. Reset the disposable database with:

```powershell
docker compose -f server/compose.yaml down --volumes
docker compose -f server/compose.yaml up --detach
```

## Upstream updates

Configure the upstream remotes once after cloning:

```powershell
git remote add angelion-upstream https://github.com/Giorox/Angelion-TFS-1.5-Downgrade-8.6.git
git remote add redemption-upstream https://github.com/opentibiabr/otclient.git
```

```powershell
git subtree pull --prefix=server angelion-upstream 8.60 --squash
git subtree pull --prefix=client redemption-upstream main --squash
```

Review and test each upstream update before merging it into `master`.

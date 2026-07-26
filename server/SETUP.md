# Local setup

> **Integrated repository note:** This file is inherited server reference
> documentation and is not the complete Angelion project workflow. From the
> repository root, use [`../README.md`](../README.md) for setup and
> [`../docs/testing.md`](../docs/testing.md) for supported Compose and playerbot
> validation commands.

Start the server and database:

```sh
docker compose up --build -d
docker compose logs -f server
```

The bundled local GOD account is `admin`, the password is `admin`, and the
character is `GOD Admin`.

Stop the services with `docker compose down`. Add `--volumes` only when you
want to delete and recreate the database.

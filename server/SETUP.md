# Local setup

Start the server and database:

```sh
docker compose up --build -d
docker compose logs -f server
```

The bundled local GOD account is `admin`, the password is `admin`, and the
character is `GOD Admin`.

Stop the services with `docker compose down`. Add `--volumes` only when you
want to delete and recreate the database.

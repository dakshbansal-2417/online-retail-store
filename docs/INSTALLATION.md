# Installation Guide

These steps work on **Ubuntu 22.04 / 24.04**, either installed directly or inside
**WSL on Windows**. Commands are typed in a Linux terminal.

## 1. Install the required packages

```bash
sudo apt update
sudo apt install -y build-essential git mysql-server libmysqlclient-dev apache2 apache2-utils
```

| Package | Why |
|---|---|
| build-essential | gcc and make |
| mysql-server | the database |
| libmysqlclient-dev | MySQL C API headers and library (`mysql.h`, `mysql_config`) |
| apache2, apache2-utils | web server and the `htpasswd` tool |

Start MySQL. On WSL, services do not start by themselves:

```bash
sudo service mysql start
```

## 2. Get the source code and build it

```bash
git clone https://github.com/dakshbansal-2417/online-retail-store.git
cd online-retail-store
make
```

This produces:

```
bin/ors_server     bin/ors_client     bin/ors_loadtest
bin/catalog.cgi    bin/reports.cgi
```

To build only the client on a machine without MySQL, run `make client`.

## 3. Create the database

```bash
./scripts/setup_db.sh --demo
```

The script runs these files as the MySQL root user:

1. `sql/schema.sql` creates the `retail_store` database, tables and views
2. `sql/db_users.sql` creates the MySQL accounts `ors_app` (server) and `ors_web` (web pages, read-only)
3. `sql/seed.sql` adds 17 products
4. `sql/sample_data.sql` adds 3 demo customers and two weeks of orders. It only runs with `--demo`; leave it out for an empty store.

It also copies `config/server.conf.example` to `config/server.conf` with mode 600.

> **Passwords:** the default MySQL passwords are in `sql/db_users.sql`. If you
> change them, put the same values in `config/server.conf` (`db_pass`) and
> `/etc/ors/web.conf`.

## 4. Start the server

```bash
./bin/ors_server            # foreground, logs also shown on screen
```

The first time it starts, it creates the admin account from `config/server.conf`
(`admin` / `Admin@123`). Log in and change that password from the admin menu.

Other ways to run it:

```bash
./bin/ors_server -p 9000                 # different port
./bin/ors_server -d                      # background daemon (log: logs/server.log)
kill -USR1 $(cat logs/server.pid)        # write statistics to the log
kill -HUP  $(cat logs/server.pid)        # reopen the log file
kill       $(cat logs/server.pid)        # graceful shutdown (SIGTERM)
```

## 5. Start the client

In a second terminal:

```bash
./bin/ors_client                     # connects to 127.0.0.1:8080
./bin/ors_client 192.168.1.20 8080   # server on another machine
```

## 6. Install the web pages (Apache)

```bash
./scripts/install_web.sh
```

The script:
- builds the CGI programs
- enables `mod_cgid`
- copies them to `/usr/lib/cgi-bin/ors/`
- copies the landing page to `/var/www/ors/`
- installs `/etc/ors/web.conf`
- asks for a password for the reports page (user `admin`)
- enables `apache/ors.conf` and restarts Apache

On WSL, start Apache with `sudo service apache2 start` if it is not running.

Open in a browser:
- http://localhost/ors/ (landing page)
- http://localhost/ors-cgi/catalog.cgi (product catalogue)
- http://localhost/ors-cgi/reports.cgi (admin reports, login required)

## 7. Run the concurrency test

```bash
# 20 customers race for 5 units of product 3: exactly 5 orders must succeed
./bin/ors_loadtest -i 3 -n 20 -s 5 -a admin -w 'Admin@123'
```

Raise `max_clients` in `config/server.conf` above 20 for larger runs.

## 8. Troubleshooting

| Problem | Fix |
|---|---|
| `mysql_config not found` while building | `sudo apt install libmysqlclient-dev` |
| `Can't connect to local MySQL server through socket` | `sudo service mysql start` |
| `Access denied for user 'ors_app'` | Passwords in `config/server.conf` and `sql/db_users.sql` do not match. Fix one and rerun `sudo mysql < sql/db_users.sql` |
| `Database schema missing` | Run `./scripts/setup_db.sh` |
| `Another ORS server is already running` | A server is already running: `kill $(cat logs/server.pid)` |
| `Address already in use` | Use another port: `./bin/ors_server -p 9090` and `./bin/ors_client 127.0.0.1 9090` |
| Web page shows "database is not reachable" | Check `/etc/ors/web.conf` and `sudo tail /var/log/apache2/error.log` |
| Client says "Server busy" | `max_clients` has been reached. Increase it in the config file |

## 9. Uninstall

```bash
sudo mysql -e "DROP DATABASE retail_store; DROP USER 'ors_app'@'localhost'; DROP USER 'ors_web'@'localhost';"
sudo a2disconf ors && sudo service apache2 reload
sudo rm -rf /usr/lib/cgi-bin/ors /var/www/ors /etc/ors /etc/apache2/conf-available/ors.conf
```

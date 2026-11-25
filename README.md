# Online Retail Store Management System (Linux Systems Programming)

OS Lab mini project. Roll number: **BC2025025**

An online retail store back end written in **C on Linux**. A multi-threaded TCP
server manages inventory, customers, carts, orders, payments and reports, and
stores everything in **MySQL**. Customers and administrators use a terminal
client, and **Apache** serves a public product catalogue and an admin sales
dashboard through C CGI programs.

```
 ors_client (customer/admin) ──TCP 8080──►  ors_server ──libmysqlclient──► MySQL
 ors_client ...                ──TCP──────►   │ 1 thread per client             ▲
 ors_loadtest (N processes)    ──TCP──────►   │ fork() → payment gateway        │
                                              │ process (pipe)                  │
 Browser ──HTTP──► Apache ──CGI──► catalog.cgi / reports.cgi ──(read-only)──────┘
```

## Features

| Requirement | Where it is implemented |
|---|---|
| User-friendly interface for inventory, sales and customers | Menu-driven terminal client (`src/client/client.c`) and web pages (`src/cgi/`) |
| Real-time inventory management | Stock is reduced inside the checkout transaction with row locks (`SELECT … FOR UPDATE`). Every change is written to `inventory_log`, and low-stock alerts appear in the server log |
| Secure login for admin and customers | SHA-512 salted password hashes (`crypt_r`), role checks on every command, account lock after 5 failed logins, hidden password input |
| Add, modify and delete products | `ADD_PRODUCT`, `UPDATE_PRODUCT`, `RESTOCK`, `ADJUST_STOCK`, `DELETE_PRODUCT` |
| Online payment processing | Card (Luhn check, expiry, CVV), UPI or cash on delivery. A simulated bank gateway runs as a **child process** and replies through a **pipe**. Card numbers and CVVs are never stored |
| Order processing and tracking | PLACED → CONFIRMED → SHIPPED → OUT_FOR_DELIVERY → DELIVERED, or CANCELLED (stock goes back), with a full status timeline |
| Reporting and analysis | Business summary, daily sales, top products, sales by category, inventory status and the stock movement log. Reports can be exported to CSV, and the Apache dashboard shows charts |

## Linux systems programming concepts

| Concept | Used for |
|---|---|
| Sockets (`socket`, `bind`, `listen`, `accept4`, `connect`, `getaddrinfo`) | Client/server communication |
| POSIX threads (detached) | One worker thread per connected client |
| Mutex and condition variable | Shared statistics, client registry, waiting for clients during shutdown |
| POSIX counting semaphore | Allows at most `max_clients` clients at once (others get "server busy") |
| `fork`, `pipe`, `poll`, `waitpid`, `kill` | Payment gateway process with timeout; load tester with N processes |
| Signals (`sigwait` thread, `sigaction`) | SIGINT/SIGTERM graceful stop, SIGHUP log reopen, SIGUSR1 statistics, SIGPIPE ignored, client Ctrl+C handler |
| `fcntl` record locks | Single-instance PID file lock, process-safe log file writes |
| Low-level file I/O (`open`, `read`, `write`, `O_APPEND`) | Log file, `/dev/urandom` salt, CSV export |
| `termios` | Hiding password and CVV input |
| Daemons (`fork`, `setsid`, `umask`, `dup2`) | `ors_server -d` runs in the background |
| Socket timeouts (`SO_RCVTIMEO`) | Disconnecting idle clients |
| CGI processes under Apache | Web catalogue and reports |

## Quick start (Ubuntu / WSL)

```bash
sudo apt update
sudo apt install -y build-essential mysql-server libmysqlclient-dev apache2 apache2-utils
sudo service mysql start

git clone https://github.com/dakshbansal-2417/online-retail-store.git
cd online-retail-store
make                              # builds bin/ors_server, ors_client, ors_loadtest, *.cgi
./scripts/setup_db.sh --demo      # schema + MySQL users + products + demo orders
./bin/ors_server                  # terminal 1
./bin/ors_client                  # terminal 2  (admin / Admin@123  or  aarav / Demo@1234)
./scripts/install_web.sh          # optional: Apache pages at http://localhost/ors/
```

See [docs/INSTALLATION.md](docs/INSTALLATION.md) for the full guide.

## Test data

| What | Value |
|---|---|
| Admin login | `admin` / `Admin@123` (created on first server start; change it) |
| Demo customers (`--demo`) | `aarav`, `priya`, `rohan` / `Demo@1234` |
| Card that is approved | `4111 1111 1111 1111`, any future expiry, any 3-digit CVV |
| Card that is declined | `4000 0000 0002 0000` |
| UPI | `name@okbank` is approved; an ID starting with `fail` is declined |

## Repository layout

```
include/protocol.h      wire protocol constants
src/common/             socket helpers, config file parser
src/server/             server.c (threads, signals), handlers.c (business logic),
                        db.c, auth.c, payment.c, logger.c, resp.c
src/client/client.c     interactive terminal client
src/tools/loadtest.c    concurrency test (fork N customers)
src/cgi/                Apache CGI programs: catalog.c, reports.c
sql/                    schema, MySQL accounts, seed products, demo data
config/                 example configuration files
apache/ors.conf         Apache site configuration
web/index.html          landing page
scripts/                setup_db.sh, install_web.sh, package.sh
docs/                   design document, user manual, installation guide, demo script
```

## Documentation

- [Design document](docs/DESIGN.md)
- [User manual](docs/USER_MANUAL.md)
- [Installation guide](docs/INSTALLATION.md)
- [Demo video script](docs/DEMO_SCRIPT.md)

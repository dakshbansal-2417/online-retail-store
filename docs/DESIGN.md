# Design Document: Online Retail Store Management System

**Course:** Operating Systems Lab, mini project
**Roll number:** BC2025025
**Technology:** Linux, C (C11 / POSIX), MySQL 8, Apache 2

---

## 1. Objective

Build an online retail store management system with Linux systems programming.
The system manages the store's full inventory, processes customer orders quickly
and correctly even when many customers order at the same time, and gives the
store owner reports on sales, revenue and stock.

## 2. Architecture

```
                         +-----------------------------------------------+
  ors_client  --TCP-->   |  ors_server (one process)                     |
  ors_client  --TCP-->   |                                               |
  ors_client  --TCP-->   |  main thread: poll() + accept4()              |
                         |     | counting semaphore (max_clients)        |
                         |     v                                         |
                         |  worker thread per client --- own MySQL conn -+--> MySQL 8
                         |     | checkout                                |    (InnoDB)
                         |     v                                         |      ^
                         |  fork() -> payment gateway child --pipe-->    |      |
                         |                                               |      |
                         |  signal thread: sigwait(INT,TERM,HUP,USR1)    |      |
                         |  logger: O_APPEND file + mutex + fcntl lock   |      |
                         +-----------------------------------------------+      |
                                                                                |
  Browser --HTTP--> Apache --CGI (new process per request)--> catalog.cgi ------+
                                                              reports.cgi  (read-only DB user)
```

The system has four programs:

| Program | Role |
|---|---|
| `ors_server` | Multi-threaded TCP server that contains all the business logic |
| `ors_client` | Menu-driven terminal client for customers and administrators |
| `catalog.cgi`, `reports.cgi` | C programs that Apache runs to show the web catalogue and the admin dashboard |
| `ors_loadtest` | Testing tool that forks N customer processes, all buying the same product at once |

### 2.1 Why a client/server design

The server is the only component that changes the database. Clients send short
text commands and never connect to MySQL directly. This means:
- Business rules such as stock checks, role checks and price calculation are enforced in one place.
- Database passwords stay on the server.
- Many clients on different machines can use the store at the same time.

## 3. Module design

| File | Responsibility |
|---|---|
| `src/server/server.c` | Startup, configuration, daemon mode, PID file lock, listening socket, accept loop, worker threads, signal thread, graceful shutdown, statistics |
| `src/server/handlers.c` | Parses requests, checks roles, and implements all 36 commands |
| `src/server/db.c` | MySQL connection, printf-style queries, escaping, transactions |
| `src/server/auth.c` | SHA-512 crypt password hashing with a random salt, password policy, input validation |
| `src/server/payment.c` | Card/UPI/COD validation (Luhn, expiry, CVV) and the gateway child process |
| `src/server/logger.c` | Thread-safe and process-safe log file |
| `src/server/resp.c` | Builds protocol responses and sanitises values |
| `src/common/net.c` | Buffered line reader and `send` loop that handles partial writes |
| `src/common/config.c` | `key = value` configuration file parser |
| `src/client/client.c` | Menus, table rendering, hidden input, CSV export |
| `src/cgi/*.c` | HTML generation for the catalogue and reports |
| `src/tools/loadtest.c` | Concurrency test |

## 4. Operating-system concepts in detail

### 4.1 Processes
- **Payment gateway (`payment.c`).** For card and UPI payments the server
  `fork()`s a child process that plays the bank. It waits a simulated network
  delay, decides approve or decline, and writes a fixed-size reply struct into a
  `pipe()`. The reply is smaller than `PIPE_BUF`, so the write is atomic. The
  parent waits with `poll()` and a 5-second timeout. If the child hangs, the
  parent kills it with `SIGKILL`. The parent always reaps the child with
  `waitpid()`, so no zombie processes are left. Because the server is
  multi-threaded, the child only does async-signal-safe work and ends with `_exit()`.
- **Daemon (`ors_server -d`).** Uses the classic double `fork()`, `setsid()`, `umask()` and redirects stdio to `/dev/null`.
- **Load tester.** Forks N processes. A `pipe()` works as a start barrier: every
  child blocks in `read()` until the parent closes the write end, so all
  checkouts start at the same instant. The parent collects results through the
  children's exit codes with `waitpid()`.
- **CGI.** Apache creates a new process for each web request and passes it the
  `QUERY_STRING` and `REMOTE_USER` environment variables.

### 4.2 Threads and synchronisation
- **One detached `pthread` per client.** Each thread opens its own MySQL connection, because a connection must not be shared between threads.
- **`pthread_mutex_t g_lock`** protects the statistics counters and the client registry, which is an array of connected sockets.
- **`pthread_cond_t g_all_gone`.** During shutdown the main thread waits with `pthread_cond_timedwait()` until the last client thread has finished.
- **Counting semaphore `sem_t g_slots`** starts at `max_clients`. Each new
  connection calls `sem_trywait()`. When no slot is free, the client gets
  "Server busy" and is disconnected. A finished thread calls `sem_post()`.
- **Logger.** A mutex serialises threads in the process, and an `fcntl(F_SETLKW)` write lock serialises separate processes writing to the same file.

### 4.3 Signals
All handled signals are blocked with `pthread_sigmask()` before any thread
starts. One dedicated thread receives them synchronously with `sigwait()`.
This avoids async-signal-safety problems in the rest of the server.

| Signal | Action |
|---|---|
| SIGINT, SIGTERM | Set `g_running = 0`, wake the accept loop through a self-pipe, `shutdown()` every client socket, wait for threads, remove the PID file |
| SIGHUP | Reopen the log file (supports log rotation) |
| SIGUSR1 | Write statistics to the log |
| SIGPIPE | Ignored, and `send(..., MSG_NOSIGNAL)` is used, so a disconnected client cannot kill the server |

The client installs a `sigaction()` handler for SIGINT. It restores the
terminal's echo setting if Ctrl+C is pressed during password entry.

### 4.4 Files and I/O
- The PID file is locked with `fcntl(F_SETLK)`, so a second server instance refuses to start.
- The log file is opened with `O_APPEND | O_CLOEXEC`.
- Password salts are read from `/dev/urandom`.
- The client's CSV export uses `open()`, `write()` and `close()`.
- `termios` turns off echo while a password or CVV is typed.
- `SO_RCVTIMEO` disconnects clients that stay idle longer than `idle_timeout`.

## 5. Concurrency and real-time inventory

Checkout runs as **one InnoDB transaction**:

```
START TRANSACTION
SELECT cart rows JOIN product rows ... ORDER BY product_id FOR UPDATE   -- row locks
  check every item: still on sale? stock >= quantity?
INSERT orders, order_items
UPDATE products SET stock = stock - q WHERE product_id = ? AND stock >= q
INSERT inventory_log (SALE, -q, stock_after)
fork() payment gateway  -> declined? ROLLBACK (stock restored automatically)
INSERT payments, order_status_history; DELETE cart
COMMIT
```

- `FOR UPDATE` locks the product rows. A second customer buying the same
  product waits until the first transaction commits, then sees the new stock.
  Overselling is impossible.
- Rows are locked in `product_id` order, so two carts cannot lock the same
  products in opposite orders and deadlock. If MySQL still reports a deadlock or
  lock timeout, the customer is asked to retry.
- If payment fails, `ROLLBACK` undoes the order and the stock change together.
- `ors_loadtest -i 3 -n 20 -s 5 ...` checks this: 20 processes race for 5 units, and exactly 5 orders succeed.

Cancelling an order (by the customer, or by the admin with status CANCELLED)
locks the order row and returns every item to stock in one transaction. It logs
`CANCEL` movements and sets the payment to REFUNDED, or to VOID for cash on
delivery.

## 6. Database design

```
users 1---* orders 1---* order_items *---1 products
  |            |  1---1 payments                |
  |            |  1---* order_status_history     |
  1---* cart_items *----------------------------1
                         inventory_log *---------1
```

| Table | Key columns |
|---|---|
| `users` | username (unique), password_hash, role ADMIN/CUSTOMER, contact details, is_active, failed_logins, locked_until |
| `products` | name (unique), category, price, stock (CHECK ≥ 0), reorder_level, is_active |
| `cart_items` | (user_id, product_id) primary key, quantity |
| `orders` | status enum, total_amount, payment_status, shipping_address |
| `order_items` | quantity, unit_price (price at time of sale), subtotal |
| `payments` | method, masked_detail (for example "Card **** 1111"), status, reference |
| `order_status_history` | tracking timeline: status, time, who changed it, note |
| `inventory_log` | every stock movement: INITIAL / SALE / RESTOCK / CANCEL / ADJUST, stock_after |

Views `v_daily_sales` and `v_product_sales` feed the reports.
Money is stored as `DECIMAL` in MySQL and handled as integer paise in C, so there are no floating-point rounding errors.
Products that appear in past orders are **soft-deleted** (`is_active = 0`), so old orders stay valid.

## 7. Communication protocol

Plain-text and line-based, so it can be tested with `nc localhost 8080`:

```
Request :  COMMAND|field1|field2|...\n
Response:  OK <message>  |  ERR <message>
           #Section title        (optional)
           col1|col2|col3        (first row of a section = header)
           val1|val2|val3
           .                     (end of response)
```

Values containing `|` or newlines are sanitised. A row that starts with `.` or
`#` gets an extra `.` prefix (dot-stuffing, as in SMTP).

| Role | Commands |
|---|---|
| Anyone | HELP, PING, QUIT, REGISTER, LOGIN, LIST_PRODUCTS, VIEW_PRODUCT |
| Logged in | LOGOUT, PROFILE, UPDATE_PROFILE, CHANGE_PASSWORD, ORDER_DETAILS |
| Customer | CART_ADD, CART_UPDATE, CART_REMOVE, CART_CLEAR, CART_VIEW, CHECKOUT, MY_ORDERS, CANCEL_ORDER |
| Admin | ADD_PRODUCT, UPDATE_PRODUCT, RESTOCK, ADJUST_STOCK, DELETE_PRODUCT, LIST_ORDERS, UPDATE_ORDER_STATUS, LIST_CUSTOMERS, SET_USER_ACTIVE, REPORT_SUMMARY, REPORT_SALES, REPORT_TOP, REPORT_CATEGORY, REPORT_INVENTORY, REPORT_STOCK_LOG, SERVER_STATS |

## 8. Security

| Threat | Protection |
|---|---|
| Stolen database | Passwords stored as salted SHA-512 crypt hashes. Card numbers and CVVs are never stored, only a masked hint |
| Password guessing | Account locked for 15 minutes after 5 failures. Failed logins are logged. Logins for unknown users take the same time as for real users, so attackers cannot tell which usernames exist |
| SQL injection | Every string is escaped with `mysql_real_escape_string`, every number is parsed and range-checked, and field names come from whitelists |
| Unauthorised actions | Every command has a required role, checked on the server |
| Secrets in logs | Command arguments are never logged. Password and card buffers are wiped with `explicit_bzero` |
| Too many connections | Semaphore limit, idle timeout, maximum line length |
| Database privileges | The server uses `ors_app` (data access only, no schema changes). The web pages use `ors_web` (SELECT only) |
| Web reports | Protected by Apache HTTP Basic authentication |
| Config file | The server warns if it is world-readable |

## 9. Order life cycle

```
PLACED --> CONFIRMED --> SHIPPED --> OUT_FOR_DELIVERY --> DELIVERED
   |            |           \______________________________/^
   +----+-------+
        v
    CANCELLED  (stock returned, payment REFUNDED / VOID)
```

Cash-on-delivery orders change from payment PENDING to PAID when they are marked DELIVERED.

## 10. Testing

These tests were run against MySQL 8.0 (server in Ubuntu 24.04 WSL2, MySQL on Windows). They all passed.

| Test | How | Result |
|---|---|---|
| Build | `make` with `-Wall -Wextra -Wshadow -Wformat=2` | 0 warnings |
| Customer flow | Register (weak password rejected, duplicate username rejected), login (wrong password rejected), cart, address required before checkout, checkout, order list, change password | Correct |
| Payments | Declined card `…0000`, bad Luhn number, expired card, UPI `fail…`: rejected and the cart is unchanged. Valid card, UPI and COD: order placed | Correct |
| Admin inventory | Add, duplicate name, invalid price, update, restock, adjust, hard delete (never sold), soft delete (sold) | Correct; every change appears in the stock log |
| Order life cycle | Invalid jump PLACED→SHIPPED refused; PLACED→…→DELIVERED; COD marked PAID on delivery; cancelling restocks items and sets payment REFUNDED / VOID; cancelling a shipped order is refused | Correct |
| Access control | Customer calling an admin command, admin calling a customer command, disabled account logging in | Refused |
| SQL injection | Product name `O'Brien "quote"; DROP TABLE users;--`, search for `%` | Stored and matched literally |
| Reports | All 6 reports, cancelled orders excluded from revenue | Correct |
| Overselling | `ors_loadtest -i 2 -n 20 -s 5`: 20 processes race for 5 units | 5 sold, 15 rejected, stock 0 |
| Overselling with card payments | `ors_loadtest -i 3 -n 12 -s 4 -m card` (one gateway fork per order) | 4 sold, 0 zombie processes |
| Admission control | `max_clients = 2`, 3 connections | 3rd gets "Server busy" |
| Signals | `kill -USR1`, `kill -HUP`, Ctrl+C (SIGINT), `kill` (SIGTERM) | Stats logged, log reopened, graceful shutdown, PID file removed |
| Single instance | Start a second server | "Another ORS server is already running" |
| Daemon | `ors_server -d` | Detached (no tty), stops on SIGTERM |
| CGI | `catalog.cgi?q=tea`, `reports.cgi` with the real database | Valid HTML, 0 errors |

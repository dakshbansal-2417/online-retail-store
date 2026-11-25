# Demo Video Script (about 8–10 minutes)

Use this script to record the demonstration video. Arrange three terminals on
screen: **server** (left), **customer client** (right) and **admin client**
(bottom), plus a browser for the web pages.

Before recording:
```bash
./scripts/setup_db.sh --demo      # fresh data
make
```

---

### 1. Introduction (30 s)
- Show the README: what the project is, the tech stack and the architecture diagram.

### 2. Start the server (45 s)
```bash
./bin/ors_server
```
- Point out the log line: port, PID, max clients, database.
- In another terminal run `./bin/ors_server` again. It prints **"Another ORS server is already running"** (fcntl lock on the PID file).

### 3. Customer: register, browse, cart (2 min)
```bash
./bin/ors_client
```
1. Register a new customer with a weak password first, to show the password policy, then a valid one.
2. Log in. The password is hidden (termios).
3. Browse products. Point out "Only 3 left" and "Out of stock".
4. Search for `book`.
5. Add 2 × product 1 and 1 × product 6 to the cart. Try 500 units to show the stock check.
6. View the cart.

### 4. Checkout and payment (1.5 min)
1. Checkout with card `4000 0000 0002 0000`: **declined**. The cart is unchanged and stock is untouched.
2. Checkout with card `4111 1111 1111 1111`, expiry `12/30`, CVV `123`: **approved**, with an order number and payment reference.
3. In the server terminal, show the log lines for the order and any LOW STOCK warning.
4. Explain that the payment gateway ran as a **forked child process** that answered through a **pipe**.
5. My orders, then track the order.

### 5. Admin: inventory and orders (2 min)
In a third terminal, log in as `admin` / `Admin@123`.
1. View inventory: stock for product 1 went down (real-time update).
2. Add a product, update its price, restock it, delete it.
3. View orders, then update the new order: CONFIRMED → SHIPPED → DELIVERED with notes.
4. In the customer window, track the order again to see the timeline.
5. Customer cancels a different PLACED order. Show that its stock comes back (Stock movement log).

### 6. Reports (1 min)
1. Reports: Business summary, Daily sales (14 days), Top products, Inventory status.
2. Save a report as CSV and `cat` the file.
3. Server statistics.

### 7. Concurrency test (1 min)
```bash
./bin/ors_loadtest -i 2 -n 20 -s 5 -a admin -w 'Admin@123'
```
- 20 processes race for 5 units. The result shows **exactly 5 orders: PASS (no overselling)**.
- Explain `SELECT … FOR UPDATE` row locking and the transaction rollback.

### 8. Web pages through Apache (1 min)
- `http://localhost/ors/`, then the catalogue (search and filter), then reports (browser login prompt, then the dashboard with the revenue chart).

### 9. Signals and shutdown (30 s)
```bash
kill -USR1 $(cat logs/server.pid)    # statistics written to the log
```
- Press Ctrl+C in the server window. It logs "Received SIGINT - shutting down gracefully", disconnects clients and prints "Server stopped".

### 10. Wrap-up (15 s)
- Recap the OS concepts: sockets, threads, mutex, condition variable, semaphore, fork/pipe, signals, file locks, daemon, termios.

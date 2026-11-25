# User Manual

The store is used through the **ORS terminal client** (`./bin/ors_client`) and
two **web pages**. Every menu is numbered: type the number and press Enter.
Passwords and CVVs are hidden while you type.

---

## 1. Main menu (not logged in)

```
   1. Login
   2. Register as a new customer
   3. Browse products (guest)
   4. Search products (guest)
   0. Exit
```

### Registering
Choose **2** and enter:
- **Username:** 3–30 letters, digits, `_` or `.`
- **Password:** 8–64 characters with at least one letter and one digit
- **Full name, e-mail, phone** (phone is optional)
- **Shipping address**

After 5 wrong passwords in a row the account is locked for 15 minutes.

---

## 2. Customer menu

```
  Shopping                      Orders
   1. Browse all products         8. My orders
   2. Search products             9. Track an order
   3. Product details            10. Cancel an order
   4. Add to cart               Account
   5. View cart                  11. My profile
   6. Change quantity / remove   12. Update profile / address
   7. Checkout & pay             13. Change password
                                  0. Logout
```

### Shopping
1. **Browse** (1) or **search** (2) to see product IDs, prices and availability.
2. **Add to cart** (4): enter the product ID and quantity (at most 100 per product).
   The client tells you if not enough stock is available.
3. **View cart** (5) shows each line, its subtotal and the total.
4. **Change quantity** (6): enter `0` to remove the item.

### Checkout and payment (7)
The cart is shown first. Then choose a payment method:

| Method | You enter | Notes |
|---|---|---|
| Card | number, expiry `MM/YY`, CVV | The number is checked (Luhn), the expiry must be in the future, CVV is hidden |
| UPI | UPI ID such as `name@okbank` | |
| Cash on delivery | nothing | Paid when the order is delivered |

Confirm with `y`. If the payment is approved, you get an order number, the total
and a payment reference, and stock is reduced at once. If the payment is
declined, nothing is charged and your cart stays as it was.

Test values: card `4111 1111 1111 1111` is approved and `4000 0000 0002 0000` is
declined. A UPI ID starting with `fail` is declined.

### Orders
- **My orders** (8) lists every order with its status and payment state.
- **Track an order** (9) shows the order, its items, the payment and the tracking
  history (when each status was reached).
- **Cancel an order** (10) works while the order is PLACED or CONFIRMED. The
  items go back into stock, and card/UPI payments are marked REFUNDED.

### Account
- **Profile** (11) shows your details.
- **Update** (12) changes your name, e-mail, phone or **shipping address**. An
  address is required before checkout.
- **Change password** (13).

---

## 3. Admin menu

Log in as `admin` (first password `Admin@123`, change it with option 15).

```
  Inventory                       Orders
   1. View inventory                8. View orders
   2. Search inventory              9. Order details
   3. Add product                  10. Update order status
   4. Update product              Customers
   5. Restock product              11. List / search customers
   6. Adjust stock count           12. Enable / disable customer
   7. Delete product              System
                                   13. Reports & analysis
                                   14. Server statistics
                                   15. Change password
                                    0. Logout
```

### Inventory
| Option | What it does |
|---|---|
| 1 View inventory | All products with stock, reorder level and status: OK, LOW, OUT or DELETED |
| 3 Add product | Name, category, price, opening stock, reorder level, description |
| 4 Update product | Change the name, category, description, price, reorder level, or active (0 hides it from customers) |
| 5 Restock | Adds the units received. Logged as RESTOCK |
| 6 Adjust stock | Sets the actual count, for example after damage or a stock-take. Logged as ADJUST |
| 7 Delete | A product that was never sold is removed completely. A sold product is hidden but kept, so old orders stay correct |

**Low-stock alerts:** when a sale takes stock down to the reorder level or
below, the server log shows a `LOW STOCK` warning. The product also appears in
the inventory report and on the web dashboard.

### Orders
- **View orders** (8) shows all orders, or only those with one status.
- **Update order status** (10) moves an order forward. The allowed changes are:
  `PLACED → CONFIRMED → SHIPPED → (OUT_FOR_DELIVERY →) DELIVERED`.
  Choosing **CANCELLED**, while the order is PLACED or CONFIRMED, puts the items
  back in stock. Marking a cash-on-delivery order DELIVERED records the payment
  as PAID. You can add a note, which the customer sees in the tracking history.

### Customers
- **List / search** (11) shows each customer's contact details, number of orders and total spent.
- **Enable / disable** (12): a disabled customer cannot log in. Enabling a customer also unlocks a locked account.

### Reports and analysis (13)

| Report | Contents |
|---|---|
| Business summary | Total revenue, today, last 30 days, average order, orders by state, units sold, customers, low-stock count, inventory value |
| Daily sales | Orders, units and revenue for each day over the last N days |
| Top-selling products | Best sellers by units and revenue |
| Sales by category | Units and revenue for each category |
| Inventory status | Every product with stock value and whether it needs reordering |
| Stock movement log | The latest 50 stock changes, for all products or one product |

After a report is shown, answer `y` to **save it as a CSV file**, which opens in Excel or LibreOffice.

### Server statistics (14)
Shows the server's process ID, uptime, connected clients compared with the
maximum, total connections, rejected connections, requests served, orders
placed and failed logins.

---

## 4. Web pages

| Page | Address | Who |
|---|---|---|
| Home | `http://<server>/ors/` | everyone |
| Catalogue | `http://<server>/ors-cgi/catalog.cgi` | everyone: search by text and category, shows stock status |
| Reports | `http://<server>/ors-cgi/reports.cgi` | admin, with a browser login prompt (password set during installation) |

The reports page shows summary cards, a 14-day revenue bar chart, top products,
sales by category, products to restock, orders by status and the latest orders.

---

## 5. Advanced: talking to the server directly

The protocol is plain text, so you can test it with netcat:

```
$ nc localhost 8080
OK Welcome to ORS - Online Retail Store v1.0. Type HELP for commands.
.
LOGIN|admin|Admin@123
OK Welcome, Store Administrator!
user_id|username|role|full_name
1|admin|ADMIN|Store Administrator
.
REPORT_SUMMARY
...
```

Type `HELP` to see every command available to your role.

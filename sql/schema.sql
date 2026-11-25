-- ======================================================================
--  Online Retail Store Management System - database schema (MySQL 8)
--  Run as the MySQL root user:   sudo mysql < sql/schema.sql
-- ======================================================================

CREATE DATABASE IF NOT EXISTS retail_store
  CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
USE retail_store;

-- ---------------------------------------------------------------------
-- Users: administrators and customers
-- ---------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS users (
  user_id        INT AUTO_INCREMENT PRIMARY KEY,
  username       VARCHAR(30)  NOT NULL UNIQUE,
  password_hash  VARCHAR(255) NOT NULL,                 -- SHA-512 crypt, never plain text
  role           ENUM('ADMIN','CUSTOMER') NOT NULL DEFAULT 'CUSTOMER',
  full_name      VARCHAR(100) NOT NULL,
  email          VARCHAR(100) NOT NULL,
  phone          VARCHAR(20),
  address        VARCHAR(255),
  is_active      TINYINT(1)   NOT NULL DEFAULT 1,
  failed_logins  INT          NOT NULL DEFAULT 0,
  locked_until   DATETIME     NULL,                     -- brute-force protection
  created_at     DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  last_login     DATETIME     NULL
) ENGINE = InnoDB;

-- ---------------------------------------------------------------------
-- Products / inventory
-- ---------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS products (
  product_id     INT AUTO_INCREMENT PRIMARY KEY,
  name           VARCHAR(100)  NOT NULL UNIQUE,
  category       VARCHAR(50)   NOT NULL,
  description    VARCHAR(255),
  price          DECIMAL(10,2) NOT NULL,
  stock          INT           NOT NULL DEFAULT 0,
  reorder_level  INT           NOT NULL DEFAULT 5,      -- low-stock alert threshold
  is_active      TINYINT(1)    NOT NULL DEFAULT 1,      -- 0 = removed from sale
  created_at     DATETIME      NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at     DATETIME      NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  CONSTRAINT chk_price CHECK (price >= 0),
  CONSTRAINT chk_stock CHECK (stock >= 0),
  INDEX idx_products_category (category)
) ENGINE = InnoDB;

-- ---------------------------------------------------------------------
-- Shopping cart (one row per customer + product)
-- ---------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS cart_items (
  user_id     INT NOT NULL,
  product_id  INT NOT NULL,
  quantity    INT NOT NULL,
  added_at    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (user_id, product_id),
  CONSTRAINT fk_cart_user    FOREIGN KEY (user_id)    REFERENCES users(user_id)       ON DELETE CASCADE,
  CONSTRAINT fk_cart_product FOREIGN KEY (product_id) REFERENCES products(product_id) ON DELETE CASCADE,
  CONSTRAINT chk_cart_qty CHECK (quantity > 0)
) ENGINE = InnoDB;

-- ---------------------------------------------------------------------
-- Orders
-- ---------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS orders (
  order_id          INT AUTO_INCREMENT PRIMARY KEY,
  user_id           INT NOT NULL,
  order_date        DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  status            ENUM('PLACED','CONFIRMED','SHIPPED','OUT_FOR_DELIVERY','DELIVERED','CANCELLED')
                    NOT NULL DEFAULT 'PLACED',
  total_amount      DECIMAL(12,2) NOT NULL,
  payment_status    ENUM('PENDING','PAID','REFUNDED','VOID') NOT NULL DEFAULT 'PENDING',
  shipping_address  VARCHAR(255) NOT NULL,
  updated_at        DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  CONSTRAINT fk_orders_user FOREIGN KEY (user_id) REFERENCES users(user_id),
  INDEX idx_orders_date (order_date),
  INDEX idx_orders_status (status)
) ENGINE = InnoDB;

CREATE TABLE IF NOT EXISTS order_items (
  order_item_id  INT AUTO_INCREMENT PRIMARY KEY,
  order_id       INT NOT NULL,
  product_id     INT NOT NULL,
  quantity       INT NOT NULL,
  unit_price     DECIMAL(10,2) NOT NULL,                -- price at the time of sale
  subtotal       DECIMAL(12,2) NOT NULL,
  CONSTRAINT fk_items_order   FOREIGN KEY (order_id)   REFERENCES orders(order_id) ON DELETE CASCADE,
  CONSTRAINT fk_items_product FOREIGN KEY (product_id) REFERENCES products(product_id),
  CONSTRAINT chk_item_qty CHECK (quantity > 0)
) ENGINE = InnoDB;

-- order tracking timeline
CREATE TABLE IF NOT EXISTS order_status_history (
  history_id  INT AUTO_INCREMENT PRIMARY KEY,
  order_id    INT NOT NULL,
  status      VARCHAR(20) NOT NULL,
  changed_by  INT NULL,
  changed_at  DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  note        VARCHAR(255),
  CONSTRAINT fk_hist_order FOREIGN KEY (order_id)   REFERENCES orders(order_id) ON DELETE CASCADE,
  CONSTRAINT fk_hist_user  FOREIGN KEY (changed_by) REFERENCES users(user_id)  ON DELETE SET NULL
) ENGINE = InnoDB;

-- ---------------------------------------------------------------------
-- Payments (card numbers and CVVs are NEVER stored - only a masked hint)
-- ---------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS payments (
  payment_id     INT AUTO_INCREMENT PRIMARY KEY,
  order_id       INT NOT NULL UNIQUE,
  method         ENUM('CARD','UPI','COD') NOT NULL,
  masked_detail  VARCHAR(80) NOT NULL,
  amount         DECIMAL(12,2) NOT NULL,
  status         ENUM('SUCCESS','PENDING','REFUNDED','VOID') NOT NULL,
  reference      VARCHAR(40) NOT NULL,
  paid_at        DATETIME NULL,
  created_at     DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  CONSTRAINT fk_pay_order FOREIGN KEY (order_id) REFERENCES orders(order_id) ON DELETE CASCADE
) ENGINE = InnoDB;

-- ---------------------------------------------------------------------
-- Inventory audit trail: every stock change is recorded
-- ---------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS inventory_log (
  log_id        INT AUTO_INCREMENT PRIMARY KEY,
  product_id    INT NOT NULL,
  change_qty    INT NOT NULL,                            -- +restock, -sale
  reason        ENUM('INITIAL','SALE','RESTOCK','CANCEL','ADJUST') NOT NULL,
  ref_order_id  INT NULL,
  changed_by    INT NULL,
  stock_after   INT NOT NULL,
  note          VARCHAR(255),
  created_at    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  CONSTRAINT fk_log_product FOREIGN KEY (product_id) REFERENCES products(product_id) ON DELETE CASCADE,
  CONSTRAINT fk_log_user    FOREIGN KEY (changed_by) REFERENCES users(user_id) ON DELETE SET NULL,
  INDEX idx_log_product (product_id)
) ENGINE = InnoDB;

-- ---------------------------------------------------------------------
-- Reporting views
-- ---------------------------------------------------------------------
CREATE OR REPLACE VIEW v_daily_sales AS
SELECT DATE(o.order_date)   AS sale_date,
       COUNT(*)             AS orders,
       SUM(t.units)         AS units,
       SUM(o.total_amount)  AS revenue
FROM orders o
JOIN (SELECT order_id, SUM(quantity) AS units FROM order_items GROUP BY order_id) t
  ON t.order_id = o.order_id
WHERE o.status <> 'CANCELLED'
GROUP BY DATE(o.order_date);

CREATE OR REPLACE VIEW v_product_sales AS
SELECT p.product_id,
       p.name,
       p.category,
       COALESCE(s.units, 0)   AS units_sold,
       COALESCE(s.revenue, 0) AS revenue
FROM products p
LEFT JOIN (SELECT oi.product_id, SUM(oi.quantity) AS units, SUM(oi.subtotal) AS revenue
           FROM order_items oi JOIN orders o ON o.order_id = oi.order_id
           WHERE o.status <> 'CANCELLED'
           GROUP BY oi.product_id) s
  ON s.product_id = p.product_id;

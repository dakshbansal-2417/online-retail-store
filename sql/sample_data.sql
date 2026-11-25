-- ======================================================================
--  OPTIONAL demo data: 3 customers and two weeks of order history so the
--  reports have something to show. Run ONCE after seed.sql:
--      sudo mysql < sql/sample_data.sql
--
--  Demo customer logins (password for all three: Demo@1234)
--      aarav    priya    rohan
-- ======================================================================
USE retail_store;

INSERT IGNORE INTO users (username, password_hash, role, full_name, email, phone, address) VALUES
 ('aarav', '$6$OrsDemoSalt2025$M5dkZlpCyVSyNHzHMQdgmgF/4KCiLJwgw8COHysfFlCGKRT2w72YRR9JpjW./RJEpUUNW3SqBI6qhuR0fYnz./',
  'CUSTOMER', 'Aarav Sharma', 'aarav@example.com', '9876500001', '12 MG Road, Bengaluru 560001'),
 ('priya', '$6$OrsDemoSalt2025$M5dkZlpCyVSyNHzHMQdgmgF/4KCiLJwgw8COHysfFlCGKRT2w72YRR9JpjW./RJEpUUNW3SqBI6qhuR0fYnz./',
  'CUSTOMER', 'Priya Patel', 'priya@example.com', '9876500002', '45 Park Street, Kolkata 700016'),
 ('rohan', '$6$OrsDemoSalt2025$M5dkZlpCyVSyNHzHMQdgmgF/4KCiLJwgw8COHysfFlCGKRT2w72YRR9JpjW./RJEpUUNW3SqBI6qhuR0fYnz./',
  'CUSTOMER', 'Rohan Mehta', 'rohan@example.com', '9876500003', '8 Marine Drive, Mumbai 400020');

DROP PROCEDURE IF EXISTS demo_order;
DELIMITER //
CREATE PROCEDURE demo_order(IN p_user VARCHAR(30), IN p_days INT, IN p_status VARCHAR(20),
                            IN p_method VARCHAR(10),
                            IN p_prod1 VARCHAR(100), IN p_qty1 INT,
                            IN p_prod2 VARCHAR(100), IN p_qty2 INT)
BEGIN
  DECLARE v_uid INT;
  DECLARE v_addr VARCHAR(255);
  DECLARE v_oid INT;
  DECLARE v_total DECIMAL(12,2);
  DECLARE v_date DATETIME;
  DECLARE v_pay VARCHAR(10);

  SELECT user_id, address INTO v_uid, v_addr FROM users WHERE username = p_user;
  SET v_date = NOW() - INTERVAL p_days DAY - INTERVAL FLOOR(RAND() * 300) MINUTE;

  SET v_pay = CASE
      WHEN p_status = 'CANCELLED' THEN IF(p_method = 'COD', 'VOID', 'REFUNDED')
      WHEN p_method = 'COD' AND p_status <> 'DELIVERED' THEN 'PENDING'
      ELSE 'PAID' END;

  INSERT INTO orders (user_id, order_date, status, total_amount, payment_status, shipping_address)
  VALUES (v_uid, v_date, p_status, 0, v_pay, v_addr);
  SET v_oid = LAST_INSERT_ID();

  INSERT INTO order_items (order_id, product_id, quantity, unit_price, subtotal)
  SELECT v_oid, product_id, p_qty1, price, price * p_qty1 FROM products WHERE name = p_prod1;
  IF p_prod2 IS NOT NULL THEN
    INSERT INTO order_items (order_id, product_id, quantity, unit_price, subtotal)
    SELECT v_oid, product_id, p_qty2, price, price * p_qty2 FROM products WHERE name = p_prod2;
  END IF;

  SELECT SUM(subtotal) INTO v_total FROM order_items WHERE order_id = v_oid;
  UPDATE orders SET total_amount = v_total WHERE order_id = v_oid;

  INSERT INTO payments (order_id, method, masked_detail, amount, status, reference, paid_at)
  VALUES (v_oid, p_method,
          CASE p_method WHEN 'CARD' THEN 'Card **** 1111' WHEN 'UPI' THEN 'UPI de****@okbank' ELSE 'Cash on delivery' END,
          v_total,
          CASE v_pay WHEN 'PAID' THEN 'SUCCESS' WHEN 'PENDING' THEN 'PENDING' ELSE v_pay END,
          CONCAT(IF(p_method = 'COD', 'COD', 'TXN'), LPAD(v_oid, 10, '0')),
          IF(v_pay = 'PAID', v_date, NULL));

  INSERT INTO order_status_history (order_id, status, changed_by, changed_at, note)
  VALUES (v_oid, 'PLACED', v_uid, v_date, 'Order placed by customer');
  IF p_status <> 'PLACED' THEN
    INSERT INTO order_status_history (order_id, status, changed_at, note)
    VALUES (v_oid, p_status, LEAST(NOW(), v_date + INTERVAL 1 DAY), 'Demo history');
  END IF;
END //
DELIMITER ;

CALL demo_order('aarav', 13, 'DELIVERED', 'CARD', 'Wireless Mouse', 2, 'Green Tea 100 Bags', 1);
CALL demo_order('priya', 12, 'DELIVERED', 'UPI',  'Operating System Concepts', 1, 'The C Programming Language', 1);
CALL demo_order('rohan', 11, 'DELIVERED', 'COD',  'Basmati Rice 5kg', 2, NULL, 0);
CALL demo_order('aarav', 10, 'DELIVERED', 'CARD', 'Mechanical Keyboard', 1, NULL, 0);
CALL demo_order('priya',  9, 'CANCELLED', 'CARD', 'Running Shoes', 1, NULL, 0);
CALL demo_order('rohan',  8, 'DELIVERED', 'UPI',  'Cotton T-Shirt', 3, 'Denim Jeans', 1);
CALL demo_order('aarav',  7, 'DELIVERED', 'COD',  'Ceramic Coffee Mug', 4, NULL, 0);
CALL demo_order('priya',  6, 'DELIVERED', 'CARD', 'Bluetooth Earbuds', 1, 'Steel Water Bottle 1L', 2);
CALL demo_order('rohan',  5, 'DELIVERED', 'CARD', '1TB Portable SSD', 1, NULL, 0);
CALL demo_order('aarav',  4, 'SHIPPED',   'UPI',  'LED Desk Lamp', 1, 'Wireless Mouse', 1);
CALL demo_order('priya',  3, 'SHIPPED',   'COD',  'Green Tea 100 Bags', 3, NULL, 0);
CALL demo_order('rohan',  2, 'CONFIRMED', 'CARD', 'USB-C Charger 65W', 1, 'The Linux Command Line', 1);
CALL demo_order('aarav',  1, 'PLACED',    'COD',  'Basmati Rice 5kg', 1, 'Almonds 500g', 1);
CALL demo_order('priya',  0, 'PLACED',    'UPI',  'Cotton T-Shirt', 2, NULL, 0);

DROP PROCEDURE demo_order;

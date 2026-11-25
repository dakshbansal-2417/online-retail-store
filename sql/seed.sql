-- ======================================================================
--  Initial product catalogue (safe to run more than once).
--  Run as root:   sudo mysql < sql/seed.sql
--  The admin account is created automatically by ors_server on first start
--  (admin_user / admin_password in config/server.conf).
-- ======================================================================
USE retail_store;

INSERT IGNORE INTO products (name, category, description, price, stock, reorder_level) VALUES
 ('Wireless Mouse',              'Electronics', '2.4 GHz optical mouse with USB nano receiver',      599.00,  40, 8),
 ('Mechanical Keyboard',         'Electronics', 'Tenkeyless keyboard, blue switches, backlit',       3499.00, 15, 5),
 ('Bluetooth Earbuds',           'Electronics', 'True wireless earbuds, 24 h battery with case',     1999.00, 25, 6),
 ('USB-C Charger 65W',           'Electronics', 'GaN fast charger for laptops and phones',           1499.00,  4, 5),
 ('1TB Portable SSD',            'Electronics', 'USB 3.2 external solid state drive',                6999.00, 10, 3),
 ('Operating System Concepts',   'Books',       'Classic OS textbook - processes, memory, files',     899.00, 20, 5),
 ('The C Programming Language',  'Books',       'The standard introduction to C',                     549.00, 18, 5),
 ('The Linux Command Line',      'Books',       'Complete introduction to the shell',                 699.00,  3, 4),
 ('Basmati Rice 5kg',            'Grocery',     'Aged long-grain basmati rice',                       625.00, 60, 10),
 ('Green Tea 100 Bags',          'Grocery',     'Natural green tea bags',                             349.00, 45, 10),
 ('Almonds 500g',                'Grocery',     'California almonds, vacuum packed',                  499.00,  0, 8),
 ('Cotton T-Shirt',              'Clothing',    '100% cotton round-neck t-shirt',                     399.00, 80, 15),
 ('Denim Jeans',                 'Clothing',    'Slim-fit stretch denim',                            1299.00, 30, 8),
 ('Running Shoes',               'Clothing',    'Lightweight mesh running shoes',                    2499.00, 12, 4),
 ('Steel Water Bottle 1L',       'Home',        'Insulated, keeps drinks cold for 24 h',              449.00, 35, 8),
 ('LED Desk Lamp',               'Home',        'Adjustable brightness, USB powered',                 899.00,  9, 4),
 ('Ceramic Coffee Mug',          'Home',        '350 ml microwave-safe mug',                          249.00, 50, 10);

-- opening stock entries in the inventory audit trail
INSERT INTO inventory_log (product_id, change_qty, reason, stock_after, note)
SELECT p.product_id, p.stock, 'INITIAL', p.stock, 'Opening stock (seed data)'
FROM products p
WHERE NOT EXISTS (SELECT 1 FROM inventory_log l WHERE l.product_id = p.product_id);

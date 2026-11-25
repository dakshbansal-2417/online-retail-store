-- ======================================================================
--  MySQL accounts used by the application (least privilege).
--  Run as root:   sudo mysql < sql/db_users.sql
--  CHANGE THESE PASSWORDS and put the same values in
--  config/server.conf (ors_app) and /etc/ors/web.conf (ors_web).
-- ======================================================================

-- used by the ORS server: read/write data, cannot change the schema
CREATE USER IF NOT EXISTS 'ors_app'@'localhost' IDENTIFIED BY 'OrsApp@2025';
GRANT SELECT, INSERT, UPDATE, DELETE ON retail_store.* TO 'ors_app'@'localhost';

-- used by the Apache CGI pages: read-only
CREATE USER IF NOT EXISTS 'ors_web'@'localhost' IDENTIFIED BY 'OrsWeb@2025';
GRANT SELECT ON retail_store.* TO 'ors_web'@'localhost';

FLUSH PRIVILEGES;

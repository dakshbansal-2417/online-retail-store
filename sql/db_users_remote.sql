-- ======================================================================
--  Use INSTEAD of db_users.sql when MySQL runs on a different machine than
--  ors_server - for example MySQL on Windows and ors_server inside WSL.
--  Connections then arrive over TCP from another IP, not from 'localhost'.
--  Run as root:   mysql -u root -p -e "source sql/db_users_remote.sql"
-- ======================================================================

CREATE USER IF NOT EXISTS 'ors_app'@'%' IDENTIFIED BY 'OrsApp@2025';
GRANT SELECT, INSERT, UPDATE, DELETE ON retail_store.* TO 'ors_app'@'%';

CREATE USER IF NOT EXISTS 'ors_web'@'%' IDENTIFIED BY 'OrsWeb@2025';
GRANT SELECT ON retail_store.* TO 'ors_web'@'%';

FLUSH PRIVILEGES;

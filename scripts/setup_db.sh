#!/usr/bin/env bash
# Create the database, tables, application accounts and product catalogue.
# Usage:  ./scripts/setup_db.sh [--demo]     (--demo also loads sample orders)
set -euo pipefail
cd "$(dirname "$0")/.."

MYSQL="sudo mysql"

echo ">> Creating schema (sql/schema.sql)"
$MYSQL < sql/schema.sql
echo ">> Creating MySQL accounts ors_app / ors_web (sql/db_users.sql)"
$MYSQL < sql/db_users.sql
echo ">> Loading product catalogue (sql/seed.sql)"
$MYSQL < sql/seed.sql

if [[ "${1:-}" == "--demo" ]]; then
    echo ">> Loading demo customers and order history (sql/sample_data.sql)"
    $MYSQL < sql/sample_data.sql
fi

if [[ ! -f config/server.conf ]]; then
    cp config/server.conf.example config/server.conf
    chmod 600 config/server.conf
    echo ">> Created config/server.conf"
fi
echo "Database ready."

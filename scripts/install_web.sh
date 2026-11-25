#!/usr/bin/env bash
# Install the CGI programs and landing page into Apache.
# Usage:  ./scripts/install_web.sh        (asks for the sudo password)
set -euo pipefail
cd "$(dirname "$0")/.."

make cgi

echo ">> Enabling Apache CGI module"
sudo a2enmod cgid >/dev/null 2>&1 || sudo a2enmod cgi

echo ">> Installing CGI programs to /usr/lib/cgi-bin/ors"
sudo install -d -m 755 /usr/lib/cgi-bin/ors
sudo install -m 755 bin/catalog.cgi bin/reports.cgi /usr/lib/cgi-bin/ors/

echo ">> Installing landing page to /var/www/ors"
sudo install -d -m 755 /var/www/ors
sudo install -m 644 web/index.html /var/www/ors/index.html

echo ">> Installing database settings to /etc/ors/web.conf"
sudo install -d -m 755 /etc/ors
if [[ ! -f /etc/ors/web.conf ]]; then
    sudo install -m 640 -g www-data config/web.conf.example /etc/ors/web.conf
fi

if [[ ! -f /etc/ors/htpasswd ]]; then
    echo ">> Create the login for the admin reports page (user: admin)"
    sudo htpasswd -c /etc/ors/htpasswd admin
    sudo chgrp www-data /etc/ors/htpasswd
    sudo chmod 640 /etc/ors/htpasswd
fi

echo ">> Enabling site configuration"
sudo install -m 644 apache/ors.conf /etc/apache2/conf-available/ors.conf
sudo a2enconf ors >/dev/null
sudo apache2ctl configtest
sudo service apache2 restart || sudo systemctl restart apache2

echo
echo "Done. Open:  http://localhost/ors/"

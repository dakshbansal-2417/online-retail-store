# ======================================================================
#  Online Retail Store Management System - build file
#
#  make            build everything into bin/
#  make client     build only the client (no MySQL headers needed)
#  make clean      remove build output
#  make run        start the server in the foreground
#  make package    create the submission zip
# ======================================================================

CC           ?= gcc
CFLAGS       ?= -O2 -g
override CFLAGS += -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -Iinclude -Isrc/common
MYSQL_CONFIG ?= mysql_config
MYSQL_CFLAGS := $(shell $(MYSQL_CONFIG) --cflags 2>/dev/null)
MYSQL_LIBS   := $(shell $(MYSQL_CONFIG) --libs 2>/dev/null)

BIN     := bin
COMMON  := src/common/net.c
HEADERS := $(wildcard include/*.h src/common/*.h src/server/*.h src/cgi/*.h)

SERVER_SRC := src/server/server.c src/server/handlers.c src/server/resp.c src/server/db.c \
              src/server/auth.c src/server/payment.c src/server/logger.c src/common/config.c $(COMMON)
CLIENT_SRC := src/client/client.c $(COMMON)
LOAD_SRC   := src/tools/loadtest.c $(COMMON)
CGI_COMMON := src/cgi/cgi_util.c src/common/config.c

TARGETS := $(BIN)/ors_server $(BIN)/ors_client $(BIN)/ors_loadtest $(BIN)/catalog.cgi $(BIN)/reports.cgi

.PHONY: all server client tools cgi clean run check-mysql package

all: $(TARGETS)
server: $(BIN)/ors_server
client: $(BIN)/ors_client
tools:  $(BIN)/ors_loadtest
cgi:    $(BIN)/catalog.cgi $(BIN)/reports.cgi

$(BIN):
	mkdir -p $(BIN)

check-mysql:
	@command -v $(MYSQL_CONFIG) >/dev/null 2>&1 || { \
	  echo "ERROR: $(MYSQL_CONFIG) not found. Install the MySQL client library:"; \
	  echo "       sudo apt install libmysqlclient-dev"; exit 1; }

$(BIN)/ors_server: $(SERVER_SRC) $(HEADERS) | $(BIN) check-mysql
	$(CC) $(CFLAGS) $(MYSQL_CFLAGS) -o $@ $(SERVER_SRC) $(MYSQL_LIBS) -lcrypt -pthread

$(BIN)/ors_client: $(CLIENT_SRC) $(HEADERS) | $(BIN)
	$(CC) $(CFLAGS) -o $@ $(CLIENT_SRC)

$(BIN)/ors_loadtest: $(LOAD_SRC) $(HEADERS) | $(BIN)
	$(CC) $(CFLAGS) -o $@ $(LOAD_SRC)

$(BIN)/catalog.cgi: src/cgi/catalog.c $(CGI_COMMON) $(HEADERS) | $(BIN) check-mysql
	$(CC) $(CFLAGS) $(MYSQL_CFLAGS) -o $@ src/cgi/catalog.c $(CGI_COMMON) $(MYSQL_LIBS)

$(BIN)/reports.cgi: src/cgi/reports.c $(CGI_COMMON) $(HEADERS) | $(BIN) check-mysql
	$(CC) $(CFLAGS) $(MYSQL_CFLAGS) -o $@ src/cgi/reports.c $(CGI_COMMON) $(MYSQL_LIBS)

config/server.conf:
	cp config/server.conf.example config/server.conf
	chmod 600 config/server.conf
	@echo "Created config/server.conf - edit the database password if you changed it."

run: $(BIN)/ors_server config/server.conf
	./$(BIN)/ors_server -c config/server.conf

package:
	./scripts/package.sh

clean:
	rm -rf $(BIN)
